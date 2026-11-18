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

#include "character/fixedpoint.h"
#include "fx/eroder/eroder.h"
#include "device/format_value.h"
#include "fx/eroder/eroder_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

// **A PORT IS A CLAIM ABOUT ANOTHER PROGRAM, SO THIS SUITE RESTATES THAT PROGRAM.** "It sounds like
// the eroder" is true of a port with one bank entry transposed. So the reference's numbers are
// retyped HERE, from our firmware fork @ `2567b04c`, `dsp/eroder.cpp`, in the
// reference's own units — samples at 44.1 kHz, raw SVF coefficients, `/ 44100.0f` — and `derive()`
// is held to them. Nothing below reads the port's constants to build an expectation.

namespace {

using namespace feathers;
using namespace feathers::fx::eroder;

constexpr double kRate = 48000.0;

/// THE COMB'S CASES START FROM THE COMB: the device's own default engine is Dust, and its default Mix is Dust's.
Settings defaults() {
	Settings s = stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings;
	s.mode = Mode::Comb;
	s.mix = 0.5f;
	return s;
}
/// THE DEVICE'S DEFAULTS, as declared.
Settings declared() { return stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings; }

//--------------------------------------------------------------------------------------------------//
// THE REFERENCE, RESTATED.                                                                           //
//--------------------------------------------------------------------------------------------------//

struct RefTri {
	double phiFreq;
	double duty;
	double offset;
	bool bipolar;
};

// **THE REFERENCE'S `kPhiNNN` LITERALS, RETYPED — AND THREE OF THEM ARE NOT WHAT THEIR NAMES SAY.**
// The first draft of this table held the EXPONENT and computed `pow(phi, k)`, and the bank case went
// red on exactly the entries using 0.75, 1.25 and 1.75:
//
//     kPhi075 = 1.4352958   against phi^0.75 = 1.43463
//     kPhi125 = 1.8257419   against phi^1.25 = 1.82488   (it is sqrt(10/3) to seven places)
//     kPhi175 = 2.3197171   against phi^1.75 = 2.32128
//
// while 1.00, 1.50 and 2.00 are exact. A few parts in ten thousand — inaudible at one position and
// a different coordinate a few hundred turns of Gamma out. **THE PORT TAKES THE FIRMWARE'S NUMBERS,
// not the numbers the firmware meant**: they are in `src/character/phi_triangle.hpp`, which is the
// firmware's header, and the shaper and automod stand on the same ones. Recorded in backlog HW.
constexpr double kRefPhi075 = 1.4352958;
constexpr double kRefPhi100 = 1.6180340;
constexpr double kRefPhi125 = 1.8257419;
constexpr double kRefPhi150 = 2.0581710;
constexpr double kRefPhi175 = 2.3197171;
constexpr double kRefPhi200 = 2.6180340;

// `kEroderToneBank` and `kEroderCharBank`, retyped.
constexpr RefTri kRefTone[6] = {
	{ kRefPhi150, 0.85, 0.0, false },  { kRefPhi125, 0.8, 0.4, false },   { kRefPhi175, 0.7, 0.6, false },
	{ kRefPhi100, 0.65, 0.15, false }, { kRefPhi200, 0.75, 0.35, false }, { kRefPhi125, 0.5, 0.45, false },
};
constexpr RefTri kRefChar[5] = {
	{ kRefPhi150, 1.0, 0.25, false }, { kRefPhi175, 0.8, 0.3, false }, { kRefPhi100, 0.75, 0.7, false },
	{ kRefPhi075, 0.8, 0.2, false },  { kRefPhi075, 0.6, 0.1, true },
};

double refTriangle(double phase, const RefTri& t) {
	double x = (phase + t.offset) * t.phiFreq;
	x -= std::floor(x);
	if (!(x < t.duty)) {
		return 0.0;
	}
	const double u = x / t.duty;
	if (!t.bipolar) {
		return 1.0 - std::abs(2.0 * u - 1.0); // 0 -> 1 -> 0 across the duty
	}
	if (u < 0.25) { // 0 -> +1 -> 0 -> -1 -> 0 across the duty
		return 4.0 * u;
	}
	if (u < 0.75) {
		return 2.0 - 4.0 * u;
	}
	return 4.0 * u - 4.0;
}

/// `processEroder`'s per-buffer block, in ITS units, with the bank values handed in.
struct RefNumbers {
	float baseDelaySamples;
	float feedback;
	float svfCoefficient;
	float svfDamping;
	float hpfAlpha;
	float triStep; ///< per sample, as a fraction of full scale
	float wetGain;
	float depth;
};
RefNumbers reference(const ToneBank& freqResults, const CharacterBank& charResults, float knobOctaves,
                     int noteCode, int mix) {
	RefNumbers r {};
	float pitchRatio = 1.0f;
	if (noteCode >= 0 && noteCode < 128) {
		pitchRatio = std::exp2((static_cast<float>(noteCode) - 60.0f) / 12.0f);
	}
	constexpr float kMaxDelay = 256.0f;
	constexpr float kMinDelay = kMaxDelay * 0.25f;
	float baseDelay = kMinDelay + std::max(0.05f, freqResults[0]) * (kMaxDelay - 1.0f - kMinDelay);
	baseDelay /= pitchRatio;
	r.baseDelaySamples = std::clamp(baseDelay, kMinDelay, kMaxDelay - 1.0f);
	r.feedback = std::max(0.05f, freqResults[1]) * 0.95f;

	const float knobRatio = std::clamp(std::exp2(knobOctaves), 0.125f, 8.0f);
	r.svfCoefficient = std::clamp(0.15f * pitchRatio * knobRatio, 0.01f, 0.85f);
	r.depth = std::max(0.15f, charResults[0]);
	r.svfDamping = 1.0f - charResults[1] * 0.99f;

	const float hpfCutoff = 100.0f * std::exp2(3.0f * freqResults[4]) * pitchRatio * knobRatio;
	r.hpfAlpha = std::min(6.2831853f * hpfCutoff / 44100.0f, 0.5f);

	const float depth = freqResults[5];
	const float triFreq = 261.626f * pitchRatio * knobRatio * std::exp2(charResults[4] * 4.0f);
	r.triStep = std::min(std::min(4.0f * depth * triFreq / 44100.0f, 0.9f), depth);

	const float clipDrive = 1.0f + freqResults[2] * 4.0f;
	const float foldDrive = 1.0f + freqResults[3] * 3.0f;
	float comp = 1.0f - r.feedback;
	if (freqResults[2] > 0.0f) {
		comp /= clipDrive;
	}
	if (freqResults[3] > 0.0f) {
		comp /= foldDrive;
	}
	r.wetGain = static_cast<float>(mix) / 127.0f * comp;
	return r;
}

std::vector<float> sine(double hz, size_t frames, float amp = 0.5f) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		v[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
	}
	return v;
}

void run(Eroder& e, std::vector<float>& l, std::vector<float>& r) {
	e.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
}

/// THE FIRST PHASE ON A FINE GRID WHERE A PREDICATE HOLDS — the banks have dead zones and live ones,
/// and a case that needs "width is zero" or "the triangle is up" finds its coordinate rather than
/// trusting a number typed from a plot.
template <typename Pred>
double findPhase(Pred&& pred) {
	for (int i = 0; i < 4000; ++i) {
		const double phase = static_cast<double>(i) * 0.0005;
		if (pred(phase)) {
			return phase;
		}
	}
	return -1.0;
}

} // namespace

TEST_CASE("the two banks are the reference's twenty-two numbers") {
	for (int i = -400; i <= 1200; ++i) {
		const double phase = static_cast<double>(i) * 0.0037; // incommensurate with every duty
		CAPTURE(phase);
		const ToneBank tone = evalToneBank(phase);
		const CharacterBank ch = evalCharacterBank(phase);
		for (size_t k = 0; k < 6; ++k) {
			CAPTURE(k);
			CHECK(static_cast<double>(tone[k]) == doctest::Approx(refTriangle(phase, kRefTone[k])).epsilon(2e-3));
		}
		for (size_t k = 0; k < 5; ++k) {
			CAPTURE(k);
			CHECK(static_cast<double>(ch[k]) == doctest::Approx(refTriangle(phase, kRefChar[k])).epsilon(2e-3));
		}
	}
	// THE BIPOLAR ONE GOES NEGATIVE, and only it does — or "bipolar" is a flag nothing reads.
	float lowest = 1.0f;
	float lowestOther = 1.0f;
	for (int i = 0; i < 2000; ++i) {
		const CharacterBank ch = evalCharacterBank(static_cast<double>(i) * 0.001);
		lowest = std::min(lowest, ch[4]);
		lowestOther = std::min({ lowestOther, ch[0], ch[1], ch[2], ch[3] });
	}
	CHECK(lowest < -0.9f);
	CHECK(lowestOther >= 0.0f);
}

TEST_CASE("at 44.1 kHz derive() is the reference's arithmetic, in the reference's units") {
	int compared = 0;
	for (int ti = 0; ti < 40; ++ti) {
		for (int ci = 0; ci < 12; ++ci) {
			for (const int note : { -1, 36, 60, 67, 96 }) {
				for (const float knob : { -2.0f, -0.5f, 0.0f, 1.3f, 2.0f }) {
					const double tonePhase = static_cast<double>(ti) * 0.0257;
					const double charPhase = static_cast<double>(ci) * 0.0831;
					const int mix = 1 + (ti * 7 + ci * 13) % 127;
					CAPTURE(tonePhase);
					CAPTURE(charPhase);
					CAPTURE(note);
					CAPTURE(knob);

					const RefNumbers ref =
					    reference(evalToneBank(tonePhase), evalCharacterBank(charPhase), knob, note, mix);
					// `noteCode = -1` is "no pitch info", ratio one — which is note 60 here.
					const double semis = note < 0 ? 60.0 : static_cast<double>(note);
					const Derived d = derive({ tonePhase, charPhase }, static_cast<double>(knob),
					                         static_cast<double>(mix) / 127.0, semis, 44100.0);

					CHECK(d.baseDelaySeconds * 44100.0 == doctest::Approx(ref.baseDelaySamples).epsilon(1e-5));
					CHECK(d.feedback == doctest::Approx(ref.feedback).epsilon(1e-5));
					CHECK(d.svfCoefficient == doctest::Approx(ref.svfCoefficient).epsilon(1e-5));
					CHECK(1.0 - d.resonance == doctest::Approx(ref.svfDamping).epsilon(1e-5));
					CHECK(d.hpfAlpha == doctest::Approx(ref.hpfAlpha).epsilon(1e-4));
					CHECK(d.depth == doctest::Approx(ref.depth).epsilon(1e-6));
					CHECK(d.wetGain == doctest::Approx(ref.wetGain).epsilon(1e-4));
					const double step = std::min(std::min(4.0 * d.triangleDepth * d.triangleHz / 44100.0, 0.9),
					                             d.triangleDepth);
					CHECK(step == doctest::Approx(ref.triStep).epsilon(1e-4));
					++compared;
				}
			}
		}
	}
	CHECK(compared == 40 * 12 * 5 * 5);
}

TEST_CASE("at another rate the SOUND is the reference's: seconds and hertz hold, the SVF's coefficient is the firmware's, the highpass keeps its pole") {
	const Phases p { 0.37, 0.61 };
	const Derived a = derive(p, 0.5, 0.8, 64.0, 44100.0);
	for (const double rate : { 48000.0, 96000.0, 192000.0, 22050.0 }) {
		CAPTURE(rate);
		const Derived b = derive(p, 0.5, 0.8, 64.0, rate);
		CHECK(b.baseDelaySeconds == a.baseDelaySeconds);
		CHECK(b.svfHz == a.svfHz);
		CHECK(b.hpfHz == a.hpfHz);
		CHECK(b.triangleHz == a.triangleHz);
		CHECK(b.feedback == a.feedback);
		CHECK(b.wetGain == a.wetGain);
		// THE SVF RUNS ON THE 44.1 kHz MODULATION CLOCK (NX-D1), so its coefficient does not move at all.
		CHECK(b.svfCoefficient == a.svfCoefficient);
		// THE HIGHPASS RUNS AT THE HOST RATE: the same pole PER SECOND, `(1 - alpha)^rate`.
		CHECK(rate * std::log(1.0 - b.hpfAlpha) == doctest::Approx(44100.0 * std::log(1.0 - a.hpfAlpha)).epsilon(1e-9));
	}
	// AND WHERE THE FIRMWARE'S 0.5 CEILING HOLDS THE 44.1 kHz CORNER DOWN (an octave and a half up, Cutoff at +2),
	// the running rate keeps THAT corner rather than the unclamped one `2 pi f / rate` would give it.
	const double hiTone = findPhase([](double ph) { return evalToneBank(ph)[4] > 0.5f; });
	REQUIRE(hiTone >= 0.0);
	const Derived hiA = derive({ hiTone, 0.0 }, 2.0, 1.0, 84.0, 44100.0);
	REQUIRE(hiA.hpfAlpha == kHpfAlphaMax);
	const Derived hiB = derive({ hiTone, 0.0 }, 2.0, 1.0, 84.0, 96000.0);
	CHECK(96000.0 * std::log(1.0 - hiB.hpfAlpha) == doctest::Approx(44100.0 * std::log(0.5)).epsilon(1e-9));

	// AND THE REFERENCE CORNER IS 0.15 AS A COEFFICIENT, WHICH IS 1053.9 Hz.
	const Derived ref = derive({ 0.0, 0.0 }, 0.0, 1.0, 60.0, 44100.0);
	CHECK(ref.svfCoefficient == doctest::Approx(0.15).epsilon(1e-9));
	CHECK(ref.svfHz == doctest::Approx(1053.9).epsilon(1e-4));
}

namespace {
/// THE SETTINGS NX-D1 WAS MEASURED AT: the default, the SVF-wrap Character, the triangle wrapping deep, and the
/// default an octave up with Cutoff open (the SVF at a high corner).
struct RateCase {
	const char* name;
	float tone;
	float character;
	float cutoff;
	float note;
};
constexpr RateCase kRateCases[] = {
	{ "S0 default", 0.0f, 0.0f, 0.0f, 60.0f },
	{ "S1 Character 0.47", 0.0f, 0.47f, 0.0f, 60.0f },
	{ "S2 Tone 0.2145", 0.2145f, 0.0f, 0.0f, 60.0f },
	{ "S4 note 72, Cutoff +1.5", 0.0f, 0.0f, 1.5f, 72.0f },
};

/// ONE ERODER AT `rate` ON A CASE, rendered a frame at a time so the heads can be read after each.
struct HeadTrace {
	std::vector<double> headL;
	std::vector<double> headR;
	std::vector<uint64_t> ticks;
	uint64_t holds = 0;
	double baseDelaySeconds = 0.0;
};
HeadTrace traceHeads(const RateCase& c, double rate, double seconds, bool withInput) {
	Eroder e;
	e.prepare(rate);
	Settings s = defaults();
	s.tone = c.tone;
	s.character = c.character;
	s.cutoffOctaves = c.cutoff;
	s.mix = 1.0f;
	e.setTarget(s);
	device::NoteContext note {};
	note.pitchSemitones = c.note;
	e.setNoteContext(note);
	const auto frames = static_cast<size_t>(seconds * rate);
	HeadTrace t;
	t.headL.resize(frames);
	t.headR.resize(frames);
	t.ticks.resize(frames);
	for (size_t i = 0; i < frames; ++i) {
		// THE SAME CONTINUOUS SIGNAL AT EVERY RATE: a 110 Hz saw of six partials, near -6 dBFS.
		float x = 0.0f;
		if (withInput) {
			const double time = static_cast<double>(i) / rate;
			for (int k = 1; k <= 6; ++k) {
				x += static_cast<float>(0.5 / k * std::sin(2.0 * std::numbers::pi * 110.0 * k * time));
			}
		}
		float l = x;
		float r = x;
		e.processStereo(&l, &r, 1);
		t.headL[i] = e.lastDelaySecondsForTest(0);
		t.headR[i] = e.lastDelaySecondsForTest(1);
		t.ticks[i] = e.modTicksForTest();
	}
	t.holds = e.holdsForTest();
	t.baseDelaySeconds = e.derivedForTest().baseDelaySeconds;
	return t;
}
} // namespace

TEST_CASE("NX-D1: the modulation runs on a 44.1 kHz clock at every host rate, and the heads walk the 44.1 kHz path") {
	// WITH THE LEFT INPUT SILENT the hold listens to the filter alone, so the modulation is a pure function of the
	// noise stream and the tick count — and at any host rate the head, in SECONDS, must be the 44.1 kHz head at the
	// same tick. This is the strong form: not a statistic, the path itself.
	for (const RateCase& c : kRateCases) {
		const std::string_view caseName = c.name;
		CAPTURE(caseName);
		const HeadTrace ref = traceHeads(c, 44100.0, 0.25, false);
		// AT 44.1 kHz ONE TICK A FRAME — the firmware's loop, the reason the output is bit-identical there.
		REQUIRE(ref.ticks.back() == ref.ticks.size());
		for (const double rate : { 48000.0, 96000.0, 32000.0 }) {
			CAPTURE(rate);
			const HeadTrace t = traceHeads(c, rate, 0.25, false);
			// THE CLOCK: 44,100 ticks a second, to within one.
			const double expected = static_cast<double>(t.ticks.size()) * 44100.0 / rate;
			CHECK(std::abs(static_cast<double>(t.ticks.back()) - expected) <= 1.0);
			size_t compared = 0;
			size_t apart = 0;
			double worst = 0.0;
			for (size_t i = 0; i < t.ticks.size(); ++i) {
				const uint64_t tick = t.ticks[i];
				if (tick == 0 || tick > ref.ticks.size()) {
					continue;
				}
				const double dl = std::abs(t.headL[i] - ref.headL[tick - 1]);
				const double dr = std::abs(t.headR[i] - ref.headR[tick - 1]);
				worst = std::max(worst, std::max(dl, dr));
				apart += (dl > 1e-9 || dr > 1e-9) ? 1u : 0u;
				++compared;
			}
			CAPTURE(worst);
			CHECK(compared > t.ticks.size() * 9 / 10);
			// A NANOSECOND: the head is a float in FRAMES, so the same seconds round differently at another rate.
			CHECK(apart == 0);
			// AND THE HOLDS PER TICK ARE THE 44.1 kHz RUN'S.
			CHECK(static_cast<double>(t.holds) / static_cast<double>(t.ticks.back())
			      == doctest::Approx(static_cast<double>(ref.holds) / static_cast<double>(ref.ticks.back())).epsilon(0.02));
		}
	}
}

TEST_CASE("NX-D1: on a real input the shake, the wraps and the holds per second do not move with the host rate") {
	// THE MEASURE THE AUDIT USED: read-head JUMPS (a move of more than a quarter of the base delay in one frame —
	// the audible face of a wrap) per second, the head's RMS excursion in seconds, and holds per second. At 48 kHz
	// the first release halved S2's jumps (13,123 -> 6,448/s); at 96 kHz they went to zero.
	const double seconds = 3.0;
	for (const RateCase& c : kRateCases) {
		const std::string_view caseName = c.name;
		CAPTURE(caseName);
		struct Stats {
			double jumpsPerSecond;
			double rmsSeconds;
			double holdsPerSecond;
		};
		const auto stats = [&](double rate) {
			const HeadTrace t = traceHeads(c, rate, seconds, true);
			const size_t from = t.headL.size() / 4;
			double jumps = 0.0;
			double sq = 0.0;
			for (size_t i = from; i < t.headL.size(); ++i) {
				jumps += std::abs(t.headL[i] - t.headL[i - 1]) > 0.25 * t.baseDelaySeconds ? 1.0 : 0.0;
				const double off = t.headL[i] - t.baseDelaySeconds;
				sq += off * off;
			}
			const double span = static_cast<double>(t.headL.size() - from) / rate;
			return Stats { jumps / span, std::sqrt(sq / static_cast<double>(t.headL.size() - from)),
				           static_cast<double>(t.holds) / seconds };
		};
		const Stats ref = stats(44100.0);
		MESSAGE(c.name << " at 44.1 kHz: " << ref.jumpsPerSecond << " jumps/s, head RMS " << ref.rmsSeconds * 1e3
		               << " ms, " << ref.holdsPerSecond << " holds/s");
		for (const double rate : { 48000.0, 96000.0 }) {
			CAPTURE(rate);
			const Stats s = stats(rate);
			CAPTURE(s.jumpsPerSecond);
			CAPTURE(ref.jumpsPerSecond);
			CAPTURE(s.rmsSeconds);
			CAPTURE(ref.rmsSeconds);
			CAPTURE(s.holdsPerSecond);
			CAPTURE(ref.holdsPerSecond);
			// THE INPUT'S CROSSINGS FIRE HOLDS, which draw from the noise stream, so after the first one the two runs
			// are the same PROCESS on different draws and these are estimates. MEASURED: 3% covers jumps and the
			// excursion; the holds are noisier — 44.1 kHz against 44.1 kHz x 1.0001 gives 1,673 and 1,778 holds in
			// one second, 1,743 and 1,760/s over four — so 5%. The first release missed these by 2x (jumps) and 30%
			// (excursion at 96 kHz).
			CHECK(s.jumpsPerSecond == doctest::Approx(ref.jumpsPerSecond).epsilon(0.03));
			CHECK(s.rmsSeconds == doctest::Approx(ref.rmsSeconds).epsilon(0.03));
			CHECK(s.holdsPerSecond == doctest::Approx(ref.holdsPerSecond).epsilon(0.05));
		}
	}
}

TEST_CASE("pitch tracking: an octave up halves the comb and doubles the filter, the highpass and the wobble") {
	// A TONE PHASE WHERE THE DELAY BANK IS HIGH, so halving it stays inside the line's clamp.
	const double tonePhase = findPhase([](double ph) { return evalToneBank(ph)[0] > 0.95f; });
	REQUIRE(tonePhase >= 0.0);
	const Phases p { tonePhase, 0.4 };
	const Derived at60 = derive(p, 0.0, 1.0, 60.0, kRate);
	const Derived at72 = derive(p, 0.0, 1.0, 72.0, kRate);
	CHECK(at72.baseDelaySeconds == doctest::Approx(at60.baseDelaySeconds * 0.5));
	CHECK(at72.svfHz > at60.svfHz * 1.9); // not exactly 2: the asin is the coefficient's, not the corner's
	CHECK(at72.hpfHz == doctest::Approx(at60.hpfHz * 2.0));
	CHECK(at72.triangleHz == doctest::Approx(at60.triangleHz * 2.0));
	// WHAT THE NOTE DOES NOT TOUCH.
	CHECK(at72.feedback == at60.feedback);
	CHECK(at72.depth == at60.depth);

	// AND THE COMB STOPS TRACKING AT THE LINE'S ENDS, as the reference's clamp has it.
	CHECK(derive(p, 0.0, 1.0, 108.0, kRate).baseDelaySeconds == kMinBaseDelaySeconds);
	CHECK(derive(p, 0.0, 1.0, 12.0, kRate).baseDelaySeconds == kMaxBaseDelaySeconds);
}

TEST_CASE("Cutoff is octaves on the filter, the highpass and the wobble — and not on the comb") {
	const Phases p { 0.37, 0.61 };
	const Derived mid = derive(p, 0.0, 1.0, 60.0, kRate);
	const Derived up = derive(p, 2.0, 1.0, 60.0, kRate);
	const Derived down = derive(p, -2.0, 1.0, 60.0, kRate);
	CHECK(up.hpfHz == doctest::Approx(mid.hpfHz * 4.0));
	CHECK(down.hpfHz == doctest::Approx(mid.hpfHz * 0.25));
	CHECK(up.triangleHz == doctest::Approx(mid.triangleHz * 4.0));
	CHECK(up.svfHz > mid.svfHz * 3.5);
	CHECK(down.svfHz < mid.svfHz * 0.3);
	CHECK(up.baseDelaySeconds == mid.baseDelaySeconds);
	CHECK(down.baseDelaySeconds == mid.baseDelaySeconds);
}

TEST_CASE("Gamma is both phases at once, 1024 times coarser") {
	const Phases p = phasesOf(0.3, 0.7, 0.25);
	CHECK(p.tone == 0.3 + 256.0);
	CHECK(p.character == 0.7 + 256.0);
	CHECK(phasesOf(0.3, 0.7, 0.0).tone == 0.3);
	// AND IT IS HELD IN DOUBLE: a float would have lost this fraction.
	const Phases far = phasesOf(0.123456, 0.0, 5000.0);
	CHECK(far.tone - 5120000.0 == doctest::Approx(0.123456).epsilon(1e-6));
}

TEST_CASE("the soft clip: linear to two thirds, a quarter slope above, a ceiling at one, odd") {
	CHECK(softClip(0.5f, 1.0f) == 0.5f);
	CHECK(softClip(0.25f, 2.0f) == 0.5f);
	CHECK(softClip(1.0f, 1.0f) == doctest::Approx(2.0 / 3.0 + (1.0 / 3.0) * 0.25));
	CHECK(softClip(1.0f, 5.0f) == 1.0f); // 2/3 + (5 - 2/3)/4 = 1.75, and the ceiling takes it
	CHECK(softClip(0.4f, 5.0f) == doctest::Approx(2.0 / 3.0 + (2.0 - 2.0 / 3.0) * 0.25));
	for (const float x : { 0.1f, 0.5f, 0.9f, 1.0f }) {
		CHECK(softClip(-x, 3.0f) == -softClip(x, 3.0f));
	}
}

TEST_CASE("the fold: a period-4 triangle in the driven signal, by floor modulo") {
	CHECK(triFold(0.5f, 1.0f) == 0.5f);
	CHECK(triFold(0.5f, 3.0f) == doctest::Approx(0.5)); // 1.5 folds back to 0.5
	CHECK(triFold(1.0f, 2.0f) == doctest::Approx(0.0)); // 2 is the way back down through zero
	CHECK(triFold(1.0f, 3.0f) == doctest::Approx(-1.0));
	CHECK(triFold(-0.5f, 3.0f) == doctest::Approx(-0.5)); // the negative side, where a truncating
	                                                       // modulo would have given +0.5
	for (const float x : { -0.9f, -0.3f, 0.2f, 0.7f }) {
		CHECK(triFold(x + 4.0f, 1.0f) == doctest::Approx(triFold(x, 1.0f)).epsilon(1e-5));
	}
}

TEST_CASE("the wet is soft-clipped THEN folded, and each stage only when its intensity is up") {
	Derived d;
	d.clipIntensity = 0.5;
	d.clipDrive = 3.0;
	d.foldIntensity = 0.5;
	d.foldDrive = 2.5;
	// 0.5 * 3 = 1.5, clipped to 2/3 + (1.5 - 2/3)/4 = 0.875; folded at 2.5: 2.1875 -> phase 3.1875 ->
	// -0.1875. THE OTHER ORDER is fold first: 1.25 -> 0.75, then clipped at 3: 2.25 -> 1.0.
	const float x = 0.5f;
	CHECK(shapeWet(x, d) == doctest::Approx(-0.1875).epsilon(1e-5));
	CHECK(shapeWet(x, d) == doctest::Approx(triFold(softClip(x, 3.0f), 2.5f)));
	CHECK(std::abs(shapeWet(x, d) - softClip(triFold(x, 2.5f), 3.0f)) > 0.5f); // not vacuous

	// A STAGE AT ZERO INTENSITY IS SKIPPED, not run at a drive of one — a soft clip at unity drive
	// still bends everything above two thirds.
	d.foldIntensity = 0.0;
	CHECK(shapeWet(x, d) == doctest::Approx(0.875));
	d.clipIntensity = 0.0;
	CHECK(shapeWet(0.9f, d) == 0.9f);
	d.foldIntensity = 0.5;
	CHECK(shapeWet(x, d) == doctest::Approx(0.75));
}

TEST_CASE("the pitched triangle REACHES the read head: its depth is the head's excursion") {
	// WHERE THE TRIANGLE IS UP AND THE BASE DELAY LEAVES IT ROOM — a base near the top of the line
	// would clip the excursion against the line's end and the measurement would be of the clamp.
	const double tonePhase = findPhase([](double ph) {
		const ToneBank t = evalToneBank(ph);
		return t[5] > 0.6f && t[0] < 0.45f && t[0] > 0.1f;
	});
	// AND WHERE THE NOISE IS AT ITS FLOOR, so the triangle is most of what moves the head.
	const double charPhase = findPhase([](double ph) { return evalCharacterBank(ph)[0] < 0.15f; });
	REQUIRE(tonePhase >= 0.0);
	REQUIRE(charPhase >= 0.0);

	Eroder e;
	e.prepare(kRate);
	Settings s = defaults();
	s.tone = static_cast<float>(tonePhase);
	s.character = static_cast<float>(charPhase);
	e.setTarget(s);
	std::vector<float> one = { 0.0f };
	std::vector<float> oneR = { 0.0f };
	double lowest = 1e9;
	double highest = 0.0;
	for (int i = 0; i < 48000; ++i) {
		run(e, one, oneR);
		lowest = std::min(lowest, e.lastDelaySecondsForTest(0));
		highest = std::max(highest, e.lastDelaySecondsForTest(0));
	}
	const Derived& d = e.derivedForTest();
	// THE TRIANGLE ALONE SWINGS THE HEAD BY depth * half the base, each way.
	const double fromTriangle = 2.0 * d.triangleDepth * kModExcursion * d.baseDelaySeconds;
	CAPTURE(d.triangleDepth);
	CAPTURE(d.baseDelaySeconds);
	CHECK(highest - lowest >= fromTriangle * 0.95);
}

TEST_CASE("Enabled off, and Mix at zero, leave the buffer alone; switched off the state does not move, at no Mix it does") {
	for (const bool byMix : { false, true }) {
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.enabled = byMix;
		s.mix = byMix ? 0.0f : 0.5f;
		e.setTarget(s);
		const std::vector<float> in = sine(220.0, 4096);
		std::vector<float> l = in;
		std::vector<float> r = in;
		run(e, l, r);
		const bool untouched = l == in && r == in;
		CHECK(untouched);
		if (byMix) {
			// AT NO MIX THE COMB STILL RUNS, so bringing Mix up reads the present and not a line left standing.
			CHECK(e.holdsForTest() > 0);
		}
		else {
			CHECK(e.holdsForTest() == 0);
		}
	}
}

TEST_CASE("the dry passes whole, to the bit, until the comb's first return") {
	// ADDITIVE, the dry at unity: before anything has come back out of the line the output IS the
	// input. The earliest the read head can sit is half the base delay behind the write.
	Eroder e;
	e.prepare(kRate);
	Settings s = defaults();
	s.mix = 1.0f;
	e.setTarget(s);
	const std::vector<float> in = sine(440.0, 2048, 0.9f);
	std::vector<float> l = in;
	std::vector<float> r = in;
	run(e, l, r);
	// THE FIRST SAMPLE IS ZERO (a sine's), so the line holds nothing audible until sample 1; the
	// head is never nearer than one frame. Everything the dry is, is still there underneath:
	CHECK(l[0] == in[0]);
	// AND WITH THE WET REMOVED the dry is exactly recovered — asserted through silence instead:
	Eroder quiet;
	quiet.prepare(kRate);
	quiet.setTarget(s);
	std::vector<float> zl(48000, 0.0f);
	std::vector<float> zr(48000, 0.0f);
	run(quiet, zl, zr);
	// **THE NOISE NEVER REACHES THE OUTPUT.** It shakes the read head and that is all it does; a
	// silent input is a silent output however hard the head is shaken.
	CHECK(std::all_of(zl.begin(), zl.end(), [](float v) { return v == 0.0f; }));
	CHECK(std::all_of(zr.begin(), zr.end(), [](float v) { return v == 0.0f; }));
	CHECK(quiet.holdsForTest() > 0); // and it WAS running
}

TEST_CASE("the read heads stay inside half a base delay either side, and inside the line") {
	Eroder e;
	e.prepare(kRate);
	Settings s = defaults();
	s.mix = 1.0f;
	s.tone = 0.41f;
	s.character = 0.23f;
	e.setTarget(s);
	std::vector<float> one = { 0.3f };
	std::vector<float> oneR = { 0.3f };
	double lowest = 1.0;
	double highest = 0.0;
	bool apart = false;
	for (int i = 0; i < 20000; ++i) {
		run(e, one, oneR);
		const double base = e.derivedForTest().baseDelaySeconds;
		for (int c = 0; c < 2; ++c) {
			const double d = e.lastDelaySecondsForTest(c);
			lowest = std::min(lowest, d / base);
			highest = std::max(highest, d / base);
			CHECK(d >= kMinDelaySeconds - 1e-9);
			CHECK(d <= kMaxBaseDelaySeconds + 1e-9);
		}
		apart = apart || e.lastDelaySecondsForTest(0) != e.lastDelaySecondsForTest(1);
	}
	CHECK(lowest >= 0.5 - 1e-6);
	CHECK(highest <= 1.5 + 1e-6);
	CHECK(highest - lowest > 0.02); // and the head MOVED
	CAPTURE(e.derivedForTest().width);
	CHECK(apart == (e.derivedForTest().width > 0.0));
}

TEST_CASE("width at zero puts both heads on one noise; width up parts them") {
	const double mono = findPhase([](double ph) { return evalCharacterBank(ph)[3] == 0.0f; });
	const double wide = findPhase([](double ph) { return evalCharacterBank(ph)[3] > 0.9f; });
	REQUIRE(mono >= 0.0);
	REQUIRE(wide >= 0.0);
	for (const bool isWide : { false, true }) {
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.character = static_cast<float>(isWide ? wide : mono);
		e.setTarget(s);
		std::vector<float> one = { 0.2f };
		std::vector<float> oneR = { 0.2f };
		int differing = 0;
		for (int i = 0; i < 4000; ++i) {
			run(e, one, oneR);
			differing += e.lastDelaySecondsForTest(0) != e.lastDelaySecondsForTest(1) ? 1 : 0;
		}
		CAPTURE(isWide);
		if (isWide) {
			CHECK(differing > 3000);
		}
		else {
			CHECK(differing == 0);
		}
	}
}

TEST_CASE("the hold re-draws on the LEFT input's zero crossings, and the right channel cannot fire it") {
	const size_t frames = 48000;
	const double hz = 300.0;

	Eroder silent;
	silent.prepare(kRate);
	silent.setTarget(defaults());
	std::vector<float> zl(frames, 0.0f);
	std::vector<float> zr(frames, 0.0f);
	run(silent, zl, zr);
	const uint64_t fromFilterAlone = silent.holdsForTest();

	// THE SAME RUN WITH A SINE ON THE RIGHT ONLY: the noise stream is the same and the left input is
	// still silent, so every hold is still the filter's.
	Eroder rightOnly;
	rightOnly.prepare(kRate);
	rightOnly.setTarget(defaults());
	std::vector<float> l2(frames, 0.0f);
	std::vector<float> r2 = sine(hz, frames);
	run(rightOnly, l2, r2);
	CHECK(rightOnly.holdsForTest() == fromFilterAlone);

	// ON THE LEFT, IT FIRES: two crossings a cycle, on top of the filter's.
	Eroder leftOnly;
	leftOnly.prepare(kRate);
	leftOnly.setTarget(defaults());
	std::vector<float> l3 = sine(hz, frames);
	std::vector<float> r3(frames, 0.0f);
	run(leftOnly, l3, r3);
	CHECK(leftOnly.holdsForTest() >= static_cast<uint64_t>(2.0 * hz) - 2);
	CHECK(leftOnly.holdsForTest() > fromFilterAlone);
}

TEST_CASE("the pitched triangle runs at its derived frequency and inside its depth") {
	// A TONE PHASE WHERE ITS DEPTH IS WELL UP, and a character phase where its offset is near zero so
	// the step is not at its ceiling.
	const double tonePhase = findPhase([](double ph) { return evalToneBank(ph)[5] > 0.8f; });
	const double charPhase =
	    findPhase([](double ph) { return std::abs(evalCharacterBank(ph)[4]) < 0.02f; });
	REQUIRE(tonePhase >= 0.0);
	REQUIRE(charPhase >= 0.0);

	Eroder e;
	e.prepare(kRate);
	Settings s = defaults();
	s.tone = static_cast<float>(tonePhase);
	s.character = static_cast<float>(charPhase);
	e.setTarget(s);
	std::vector<float> one = { 0.0f };
	std::vector<float> oneR = { 0.0f };
	int turns = 0;
	float prev = 0.0f;
	float prevDelta = 0.0f;
	float peak = 0.0f;
	for (int i = 0; i < 48000; ++i) {
		run(e, one, oneR);
		const float v = e.triangleForTest();
		const float delta = v - prev;
		// A FRAME WITH NO TICK HOLDS THE VALUE (the 44.1 kHz modulation clock under a 48 kHz host), and a
		// zero step is no turn: only a change of a MOVING direction counts.
		if (delta != 0.0f) {
			if (prevDelta != 0.0f && (delta > 0.0f) != (prevDelta > 0.0f)) {
				++turns;
			}
			prevDelta = delta;
		}
		prev = v;
		peak = std::max(peak, std::fabs(v));
	}
	const Derived& d = e.derivedForTest();
	CAPTURE(d.triangleHz);
	CAPTURE(d.triangleDepth);
	CHECK(static_cast<double>(peak) <= d.triangleDepth + 1e-6);
	CHECK(static_cast<double>(peak) > d.triangleDepth * 0.95);
	// TWO TURNS A CYCLE, one second.
	CHECK(static_cast<double>(turns) == doctest::Approx(2.0 * d.triangleHz).epsilon(0.02));

	// AND WHERE ITS DEPTH IS ZERO IT DOES NOT RUN AT ALL.
	const double off = findPhase([](double ph) { return evalToneBank(ph)[5] == 0.0f; });
	REQUIRE(off >= 0.0);
	Eroder still;
	still.prepare(kRate);
	s.tone = static_cast<float>(off);
	still.setTarget(s);
	std::vector<float> l(4096, 0.0f);
	std::vector<float> r(4096, 0.0f);
	run(still, l, r);
	CHECK(still.triangleForTest() == 0.0f);
}

TEST_CASE("the bus note reaches the comb, and a lifted key does not un-tune it") {
	const double tonePhase = findPhase([](double ph) { return evalToneBank(ph)[0] > 0.95f; });
	REQUIRE(tonePhase >= 0.0);
	Eroder e;
	e.prepare(kRate);
	Settings s = defaults();
	s.tone = static_cast<float>(tonePhase);
	e.setTarget(s);
	std::vector<float> l(64, 0.1f);
	std::vector<float> r(64, 0.1f);

	run(e, l, r);
	const double untuned = e.derivedForTest().baseDelaySeconds;

	e.setNoteContext({ 72.0f, true });
	run(e, l, r);
	CHECK(e.derivedForTest().baseDelaySeconds == doctest::Approx(untuned * 0.5));

	// GATE DOWN, PITCH KEPT — `getLastNoteCode()` does not forget a note because its key came up.
	e.setNoteContext({ 72.0f, false });
	run(e, l, r);
	CHECK(e.derivedForTest().baseDelaySeconds == doctest::Approx(untuned * 0.5));
	CHECK(e.pitchSemitonesForTest() == 72.0);

	// A FRACTIONAL PITCH IS TAKEN AS ONE — glide and bend are already folded into it.
	e.setNoteContext({ 66.0f, true });
	run(e, l, r);
	CHECK(e.derivedForTest().baseDelaySeconds == doctest::Approx(untuned / std::sqrt(2.0)));
}

TEST_CASE("the first block snaps to the controls; after it a move glides by a TIME, not by a block count") {
	Settings s = defaults();
	s.tone = 0.8f;
	s.character = 0.3f;

	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	run(e, l, r);
	CHECK(e.smoothedToneForTest() == static_cast<double>(0.8f));
	CHECK(e.smoothedCharacterForTest() == static_cast<double>(0.3f));

	s.tone = 1.8f;
	e.setTarget(s);
	run(e, l, r);
	const double expected = 0.8f + (1.8f - 0.8f) * (1.0 - std::exp(-256.0 / (kSmoothingSeconds * kRate)));
	CHECK(e.smoothedToneForTest() == doctest::Approx(expected).epsilon(1e-9));

	// THE SAME 256 FRAMES AS FOUR CALLS OF 64 LAND IN THE SAME PLACE — which 5% per call would not.
	Eroder parts;
	parts.prepare(kRate);
	s.tone = 0.8f;
	parts.setTarget(s);
	run(parts, l, r);
	s.tone = 1.8f;
	parts.setTarget(s);
	std::vector<float> ql(64, 0.0f);
	std::vector<float> qr(64, 0.0f);
	for (int i = 0; i < 4; ++i) {
		run(parts, ql, qr);
	}
	CHECK(parts.smoothedToneForTest() == doctest::Approx(e.smoothedToneForTest()).epsilon(1e-12));
}

TEST_CASE("a partition does not change the samples WHILE A CONTROL IS TRAVELLING") {
	// **THE CASE THIS SUITE DID NOT HAVE, AND `test_fx_devices.cpp` DID.** The partition case below
	// parks the controls, and a device parked on its target agrees with itself under any partition
	// however it counts. The first draft of the device ran its control path once per CALL — the
	// firmware's arrangement — and only the registry's sweep, which moves a control first, saw it.
	const std::vector<float> in = sine(180.0, 4096, 0.8f);
	const auto render = [&](const std::vector<int>& sizes) {
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.tone = 0.2f;
		s.character = 0.9f;
		s.mix = 1.0f;
		e.setTarget(s);
		std::vector<float> wl(64, 0.3f);
		std::vector<float> wr(64, 0.3f);
		run(e, wl, wr);
		s.tone = 1.7f; // AND NOW IT IS TRAVELLING
		s.character = 0.1f;
		e.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		size_t at = 0;
		size_t k = 0;
		while (at < in.size()) {
			const auto n = std::min(static_cast<size_t>(sizes[k++ % sizes.size()]), in.size() - at);
			e.processStereo(l.data() + at, r.data() + at, static_cast<int>(n));
			at += n;
		}
		return std::make_pair(l, e.smoothedToneForTest());
	};
	const auto whole = render({ 4096 });
	for (const std::vector<int>& sizes : { std::vector<int> { 32 }, std::vector<int> { 8 },
	                                       std::vector<int> { 1 }, std::vector<int> { 40, 24, 56, 8, 128 } }) {
		const auto got = render(sizes);
		CHECK(got.first == whole.first);
		CHECK(got.second == whole.second);
	}
	// NOT VACUOUS: the glide was under way for the whole window and had not arrived.
	CHECK(whole.second > 0.2);
	CHECK(whole.second < 1.7);
}

TEST_CASE("reset is as-if-just-switched-on, and a partition does not change the samples") {
	Settings s = defaults();
	s.tone = 0.41f;
	s.character = 0.77f;
	s.mix = 0.9f;
	const std::vector<float> in = sine(180.0, 24000, 0.8f);

	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	e.setNoteContext({ 55.0f, true });
	std::vector<float> l1 = in;
	std::vector<float> r1 = in;
	run(e, l1, r1);
	CHECK(l1 != in);
	CHECK(l1 != r1);

	e.reset();
	CHECK(e.holdsForTest() == 0);
	std::vector<float> l2 = in;
	std::vector<float> r2 = in;
	run(e, l2, r2);
	CHECK(l1 == l2);
	CHECK(r1 == r2);

	Eroder parts;
	parts.prepare(kRate);
	parts.setTarget(s);
	parts.setNoteContext({ 55.0f, true });
	std::vector<float> l3 = in;
	std::vector<float> r3 = in;
	const int sizes[] = { 1, 5, 64, 300, 1024 };
	size_t at = 0;
	size_t k = 0;
	while (at < in.size()) {
		const auto n = std::min(static_cast<size_t>(sizes[k++ % std::size(sizes)]), in.size() - at);
		parts.processStereo(l3.data() + at, r3.data() + at, static_cast<int>(n));
		at += n;
	}
	CHECK(l1 == l3);
	CHECK(r1 == r3);
}

TEST_CASE("the mono road is the LEFT channel") {
	Settings s = defaults();
	s.character = static_cast<float>(findPhase([](double ph) { return evalCharacterBank(ph)[3] > 0.9f; }));
	const std::vector<float> in = sine(200.0, 12000);

	Eroder stereo;
	stereo.prepare(kRate);
	stereo.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	run(stereo, l, r);

	Eroder mono;
	mono.prepare(kRate);
	mono.setTarget(s);
	std::vector<float> m = in;
	mono.processMono(m.data(), static_cast<int>(m.size()));
	CHECK(m == l);
	CHECK(l != r);
}

TEST_CASE("a hot signal everywhere in the bank stays finite, and the wet stays inside its ceiling") {
	float worstExcess = 0.0f;
	for (int ti = 0; ti < 24; ++ti) {
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.mix = 1.0f;
		s.tone = static_cast<float>(ti) * 0.0817f;
		s.character = static_cast<float>(ti) * 0.1313f;
		s.cutoffOctaves = (ti % 3 == 0) ? 2.0f : -1.0f;
		e.setTarget(s);
		e.setNoteContext({ static_cast<float>(30 + ti * 3), true });
		const std::vector<float> in = sine(97.0 + ti * 40.0, 24000, 2.5f);
		std::vector<float> l = in;
		std::vector<float> r = in;
		run(e, l, r);
		const auto gain = static_cast<float>(e.derivedForTest().wetGain * e.derivedForTest().levelMakeUp);
		for (size_t i = 0; i < l.size(); ++i) {
			REQUIRE(std::isfinite(l[i]));
			REQUIRE(std::isfinite(r[i]));
			// THE WET PATH RUNS IN THE FIRMWARE'S UNITS, where the highpass's last subtraction WRAPS
			// into [-1, 1) as q31 does — so the wet is within one of those units, which is
			// `kHeadroom` of full scale, times the gain. (Stated as "two" before the path moved into
			// those units and learnt to wrap.)
			worstExcess = std::max(worstExcess, std::fabs(l[i] - in[i]) - gain * static_cast<float>(kHeadroom));
		}
	}
	CHECK(worstExcess <= 1e-5f);
}

TEST_CASE("the tail ends in exact silence, at the bank's highest feedback") {
	const double hottest = findPhase([](double ph) { return evalToneBank(ph)[1] > 0.995f; });
	REQUIRE(hottest >= 0.0);
	Eroder e;
	e.prepare(kRate);
	Settings s = defaults();
	s.mix = 1.0f;
	s.tone = static_cast<float>(hottest);
	e.setTarget(s);
	std::vector<float> l = sine(220.0, 24000, 0.9f);
	std::vector<float> r = l;
	run(e, l, r);
	CHECK(e.derivedForTest().feedback > 0.94);

	std::vector<float> zl(48000 * 4, 0.0f);
	std::vector<float> zr = zl;
	run(e, zl, zr);
	size_t lastLive = 0;
	for (size_t i = 0; i < zl.size(); ++i) {
		if (zl[i] != 0.0f || zr[i] != 0.0f) {
			lastLive = i;
		}
	}
	MESSAGE("eroder tail at the bank's highest feedback: " << static_cast<double>(lastLive) / kRate << " s");
	CHECK(lastLive < zl.size() / 2);
}

//--------------------------------------------------------------------------------------------------//
// THE FIRMWARE'S SAMPLE LOOP, IN Q31, AT THE FIRMWARE'S OWN SIGNAL LEVEL.                            //
//--------------------------------------------------------------------------------------------------//
//
// **WHY THIS EXISTS: on the first audition the effect was barely audible at most settings, far less
// intense than on the hardware.** Every case above held `derive()` to the firmware and the
// device's loop to its own statements, and all of them passed, because none of them asked what LEVEL
// the firmware's loop runs at. `fixedpoint.h` answers it: `EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128`. A
// hardware signal at full scale is 2^24 — 42 dB under the q31 ceiling — so the soft clip and the fold,
// whose knees sit near the ceiling, are almost always LINEAR there: a gain of `drive`, which the wet
// compensation's `/ drive` then cancels. The port ran the signal at the ceiling, where both stages
// crush it, and then divided by the drives anyway.
//
// **SO THE REFERENCE IS THE LOOP ITSELF, TRANSCRIBED, not a restatement of its intent.** It runs on
// the same xorshift stream the device draws from, so the noise matches to 2^-23 and the two outputs
// can be compared sample for sample. The input is scaled to the firmware's 0 dBFS on the way in and
// back on the way out, exactly as a firmware Sound hands it over.

namespace {

using feathers::character::q31_t;
using feathers::character::add_saturate;
using feathers::character::multiply_32x32_rshift32;

constexpr double kFirmwareZeroDbfs = 2147483647.0 / 128.0; // EFFECTIVE_0DBFS_Q31

/// ARM's VCVT saturates a float-to-int conversion; x86's does not (it yields INT_MIN). The firmware
/// casts `1.0f * ONE_Q31` in several places — width and depth reach exactly one — so this matters.
q31_t armCast(double v) {
	if (v >= 2147483647.0) {
		return INT32_MAX;
	}
	if (v <= -2147483648.0) {
		return INT32_MIN;
	}
	return static_cast<q31_t>(v);
}

/// HOW OFTEN THE FIRMWARE'S PLAIN ADDS WRAPPED — counted, because a wrap is a full-scale jump and
/// whether the port must reproduce it depends on whether it happens at all.
uint64_t gWraps = 0;

/// Wrapping add, as the firmware's plain `+=` on q31 behaves on the hardware.
q31_t wrapAdd(q31_t a, q31_t b) {
	const int64_t exact = static_cast<int64_t>(a) + b;
	if (exact > INT32_MAX || exact < INT32_MIN) {
		++gWraps;
	}
	return static_cast<q31_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

q31_t mul(q31_t a, q31_t b) { return multiply_32x32_rshift32(a, b) << 1; }

struct RefEroder {
	q31_t bufL[256] {};
	q31_t bufR[256] {};
	int32_t wp = 0;
	q31_t svfLowL = 0, svfLowR = 0, svfBandL = 0, svfBandR = 0;
	q31_t heldL = 0, prevInputL = 0, prevFilteredL = 0;
	q31_t feedbackL = 0, feedbackR = 0, wetHpfL = 0, wetHpfR = 0;
	q31_t triValue = 0, triStep = 0;
	uint32_t rng = 0xc2b2ae35u; // `Eroder::kRngSeed` — the same stream
	uint64_t holds = 0;
	/// THE HOLD COUNT AFTER EACH SAMPLE — the instant the two streams can first part.
	std::vector<uint64_t> holdsAt;
	/// THE TRIANGLE AFTER EACH SAMPLE.
	std::vector<q31_t> triAt;

	q31_t noise() {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		// THE DEVICE TAKES THE TOP 24 BITS; the firmware's getNoise() is a full int32. Same stream,
		// quantised the way the device quantises it, so what differs below is arithmetic, not luck.
		return static_cast<q31_t>(static_cast<uint32_t>(static_cast<int32_t>(rng) >> 8) << 8);
	}

	static q31_t readQ16(const q31_t* buf, int32_t wp, int32_t delayQ16) {
		const int32_t delayInt = delayQ16 >> 16;
		const int32_t idx0 = (wp - 1 - delayInt) & 255;
		const int32_t idx1 = (idx0 - 1) & 255;
		const q31_t fracQ = (delayQ16 & 0xFFFF) << 15;
		const q31_t oneMinusFrac = 2147483647 - fracQ;
		return add_saturate(mul(buf[idx0], oneMinusFrac), mul(buf[idx1], fracQ));
	}

	static q31_t softClip(q31_t x, int32_t driveQ28) {
		int32_t driven = multiply_32x32_rshift32(x, driveQ28);
		constexpr int32_t kKnee = (INT32_C(1) << 27) * 2 / 3;
		constexpr int32_t kUnit = INT32_C(1) << 27;
		if (driven > kKnee) {
			driven = kKnee + ((driven - kKnee) >> 2);
		}
		else if (driven < -kKnee) {
			driven = -kKnee + ((driven + kKnee) >> 2);
		}
		driven = std::clamp(driven, -kUnit, kUnit - 1);
		return static_cast<q31_t>(static_cast<uint32_t>(driven) << 4);
	}

	static q31_t triFold(q31_t x, int32_t driveQ28) {
		const int32_t driven = multiply_32x32_rshift32(x, driveQ28);
		constexpr int32_t kUnit = INT32_C(1) << 27;
		const int32_t phase = (driven + kUnit) & (INT32_C(4) * kUnit - 1);
		const int32_t result = phase < 2 * kUnit ? phase - kUnit : 3 * kUnit - phase;
		return static_cast<q31_t>(static_cast<uint32_t>(std::min(result, kUnit - 1)) << 4);
	}

	/// `processEroder`, one buffer, at 44.1 kHz, note -1, Cutoff 0, the coordinates PARKED (so the
	/// per-buffer smoothing is at its target and plays no part). `mix` is the firmware's 0..127.
	void process(std::vector<q31_t>& l, std::vector<q31_t>& r, double tonePhase, double charPhase, int mix) {
		const ToneBank f = evalToneBank(tonePhase);
		const CharacterBank c = evalCharacterBank(charPhase);
		constexpr float kMaxDelay = 256.0f;
		constexpr float kMinDelay = kMaxDelay * 0.25f;
		const float baseDelay =
		    std::clamp(kMinDelay + std::max(0.05f, f[0]) * (kMaxDelay - 1.0f - kMinDelay), kMinDelay, kMaxDelay - 1.0f);
		const q31_t feedbackQ = armCast(std::max(0.05f, f[1]) * 0.95f * 2147483648.0);
		const float clipIntensity = f[2];
		const float foldIntensity = f[3];
		const q31_t svfF = armCast(std::clamp(0.15f, 0.01f, 0.85f) * 2147483648.0);
		const q31_t depthQ = armCast(std::max(0.15f, c[0]) * 2147483648.0);
		const q31_t svfQ = armCast((1.0f - c[1] * 0.99f) * 2147483648.0);
		const q31_t shBlendQ = armCast(c[2] * 2147483648.0);
		const q31_t whiteBlendQ = 2147483647 - shBlendQ;
		const q31_t whiteDepthQ = mul(whiteBlendQ, depthQ);
		const q31_t shDepthQ = mul(shBlendQ, depthQ);
		const q31_t widthQ = armCast(c[3] * 2147483648.0);
		const q31_t monoQ = 2147483647 - widthQ;
		const q31_t monoWhiteDepthQ = mul(monoQ, whiteDepthQ);
		const q31_t widthWhiteDepthQ = mul(widthQ, whiteDepthQ);

		const auto baseDelayQ16 = static_cast<int32_t>(baseDelay * 65536.0f);
		const int32_t minDelayQ16 = 1 << 16;
		const int32_t maxDelayQ16 = 255 << 16;
		const auto modScaleQ = static_cast<int32_t>(baseDelay * 32768.0f);

		const float pitchedModDepth = f[5];
		const bool usePitchedMod = pitchedModDepth > 0.0f;
		q31_t triPeak = 0;
		if (usePitchedMod) {
			triPeak = armCast(pitchedModDepth * 2147483648.0);
			const float triFreq = 261.626f * std::exp2(c[4] * 4.0f);
			const float stepF = std::min(4.0f * pitchedModDepth * triFreq / 44100.0f, 0.9f);
			q31_t stepMag = armCast(stepF * 2147483648.0);
			stepMag = std::min(stepMag, triPeak);
			triStep = triStep >= 0 ? stepMag : -stepMag;
			triValue = std::clamp(triValue, -triPeak, triPeak);
		}

		const float clipDrive = 1.0f + clipIntensity * 4.0f;
		const float foldDrive = 1.0f + foldIntensity * 3.0f;
		const auto clipDriveQ28 = static_cast<int32_t>(clipDrive * static_cast<float>(1 << 28));
		const auto foldDriveQ28 = static_cast<int32_t>(foldDrive * static_cast<float>(1 << 28));
		const float hpfCutoff = 100.0f * std::exp2(3.0f * f[4]);
		const q31_t hpfAlpha = armCast(std::min(6.2831853f * hpfCutoff / 44100.0f, 0.5f) * 2147483648.0);

		float wetCompensation = 1.0f - std::max(0.05f, f[1]) * 0.95f;
		if (clipIntensity > 0.0f) {
			wetCompensation /= clipDrive;
		}
		if (foldIntensity > 0.0f) {
			wetCompensation /= foldDrive;
		}
		const q31_t wetQ = armCast(static_cast<float>(mix) / 127.0f * wetCompensation * 2147483648.0);

		for (size_t i = 0; i < l.size(); ++i) {
			const q31_t inputL = add_saturate(l[i], mul(feedbackL, feedbackQ));
			const q31_t inputR = add_saturate(r[i], mul(feedbackR, feedbackQ));
			bufL[wp] = inputL;
			bufR[wp] = inputR;
			wp = (wp + 1) & 255;

			const q31_t whiteL = noise();
			const q31_t whiteRInd = noise();
			const bool inputCross = (l[i] ^ prevInputL) < 0;
			const bool filterCross = (svfLowL ^ prevFilteredL) < 0;
			if (inputCross || filterCross) {
				heldL = noise() >> 1;
				++holds;
			}
			prevInputL = l[i];
			prevFilteredL = svfLowL;

			const q31_t shTerm = mul(heldL, shDepthQ);
			const q31_t noiseL = add_saturate(mul(whiteL, whiteDepthQ), shTerm);
			const q31_t noiseR =
			    add_saturate(add_saturate(mul(whiteL, monoWhiteDepthQ), mul(whiteRInd, widthWhiteDepthQ)), shTerm);

			const q31_t highL = add_saturate(wrapAdd(noiseL, -svfLowL), -mul(svfBandL, svfQ));
			svfBandL = wrapAdd(svfBandL, mul(highL, svfF));
			svfLowL = wrapAdd(svfLowL, mul(svfBandL, svfF));
			const q31_t highR = add_saturate(wrapAdd(noiseR, -svfLowR), -mul(svfBandR, svfQ));
			svfBandR = wrapAdd(svfBandR, mul(highR, svfF));
			svfLowR = wrapAdd(svfLowR, mul(svfBandR, svfF));

			q31_t combinedL = svfBandL;
			q31_t combinedR = svfBandR;
			if (usePitchedMod) {
				triValue = wrapAdd(triValue, triStep);
				if (triValue > triPeak) {
					triValue = triPeak - (triValue - triPeak);
					triStep = -triStep;
				}
				else if (triValue < -triPeak) {
					triValue = -triPeak - (triValue + triPeak);
					triStep = -triStep;
				}
				combinedL = add_saturate(combinedL, triValue);
				combinedR = add_saturate(combinedR, triValue);
			}
			const int32_t delayLQ16 = std::clamp(baseDelayQ16 + mul(combinedL, modScaleQ), minDelayQ16, maxDelayQ16);
			const int32_t delayRQ16 = std::clamp(baseDelayQ16 + mul(combinedR, modScaleQ), minDelayQ16, maxDelayQ16);
			q31_t wetL = readQ16(bufL, wp, delayLQ16);
			q31_t wetR = readQ16(bufR, wp, delayRQ16);
			feedbackL = wetL;
			feedbackR = wetR;
			if (clipIntensity > 0.0f) {
				wetL = softClip(wetL, clipDriveQ28);
				wetR = softClip(wetR, clipDriveQ28);
			}
			if (foldIntensity > 0.0f) {
				wetL = triFold(wetL, foldDriveQ28);
				wetR = triFold(wetR, foldDriveQ28);
			}
			wetHpfL = wrapAdd(wetHpfL, mul(wetL - wetHpfL, hpfAlpha));
			wetL -= wetHpfL;
			wetHpfR = wrapAdd(wetHpfR, mul(wetR - wetHpfR, hpfAlpha));
			wetR -= wetHpfR;
			l[i] = add_saturate(l[i], mul(wetL, wetQ));
			holdsAt.push_back(holds);
			triAt.push_back(triValue);
			r[i] = add_saturate(r[i], mul(wetR, wetQ));
		}
	}
};

/// THE WET — output minus input — of one coordinate, from the port and from the transcription, at a
/// level `amp` of full scale.
///
/// **TWO NOISE-DRIVEN PROCESSES AGREE ONLY WHILE THEIR RANDOM STREAMS AGREE.** The sample-and-hold
/// draws an extra noise sample on each zero crossing, and a crossing AT zero is decided by a sign test
/// that float and q31 can answer differently (a float -1e-9 against a q31 0). From that sample on the
/// two streams are out of step and the two outputs are different random signals of the same kind. So
/// what is asserted is exactly that: sample-for-sample agreement UNTIL the hold counts first differ,
/// and the same LEVEL throughout.
struct WetCompare {
	double port;
	double firmware;
	size_t firstApart;  ///< first sample where the outputs differ by more than 1e-4 of full scale
	size_t firstDesync; ///< first sample after which the two hold counts differ
	uint64_t wraps;     ///< how many of the firmware's plain adds wrapped
};
WetCompare compareWet(double tonePhase, double charPhase, int mix, float amp) {
	const size_t frames = 44100 / 2;
	std::vector<float> in(frames);
	for (size_t i = 0; i < frames; ++i) {
		// TWO PARTIALS, so the comb has something to comb and the input is not a pure sine.
		in[i] = amp * static_cast<float>(0.7 * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / 44100.0)
		                                 + 0.3 * std::sin(2.0 * std::numbers::pi * 1330.0 * static_cast<double>(i) / 44100.0));
	}

	Eroder e;
	e.prepare(44100.0);
	Settings s;
	s.mode = Mode::Comb;
	s.tone = static_cast<float>(tonePhase);
	s.character = static_cast<float>(charPhase);
	s.mix = static_cast<float>(mix) / 127.0f;
	e.setTarget(s);
	std::vector<float> pl = in;
	std::vector<float> pr = in;
	// ONE FRAME AT A TIME — the partition cases prove this is the same output as one call — so the hold
	// count can be read after every sample.
	std::vector<uint64_t> portHolds(frames);
	for (size_t i = 0; i < frames; ++i) {
		e.processStereo(pl.data() + i, pr.data() + i, 1);
		portHolds[i] = e.holdsForTest();
	}
	// **THE PORT'S LEVEL LAW IS NOT THE REFERENCE'S, ON PURPOSE**: its wet is the reference's times
	// sqrt((1 + fb) / (1 - fb)), so that a higher feedback is no quieter (`Derived::levelMakeUp`). Restated here from
	// the feedback and taken back out, so what is compared is the loop.
	const double fb = e.derivedForTest().feedback;
	const double makeUp = std::sqrt((1.0 + fb) / (1.0 - fb));
	for (size_t i = 0; i < frames; ++i) {
		pl[i] = static_cast<float>(static_cast<double>(in[i]) + (static_cast<double>(pl[i]) - static_cast<double>(in[i])) / makeUp);
	}

	RefEroder ref;
	std::vector<q31_t> ql(frames);
	for (size_t i = 0; i < frames; ++i) {
		ql[i] = armCast(static_cast<double>(in[i]) * kFirmwareZeroDbfs);
	}
	std::vector<q31_t> qr = ql;
	gWraps = 0;
	ref.process(ql, qr, static_cast<double>(s.tone), static_cast<double>(s.character), mix);


	WetCompare w { 0.0, 0.0, frames, frames, gWraps };
	for (size_t i = 0; i < frames; ++i) {
		if (w.firstDesync == frames && portHolds[i] != ref.holdsAt[i]) {
			w.firstDesync = i;
		}
		const double fw = static_cast<double>(ql[i]) / kFirmwareZeroDbfs;
		if (w.firstApart == frames && std::abs(static_cast<double>(pl[i]) - fw) > 1e-4) {
			w.firstApart = i;
		}
	}

	double portSq = 0.0;
	double fwSq = 0.0;
	const size_t from = frames / 4; // past the comb's first laps
	for (size_t i = from; i < frames; ++i) {
		const double dry = static_cast<double>(in[i]);
		const double portWet = static_cast<double>(pl[i]) - dry;
		const double fwWet = static_cast<double>(ql[i]) / kFirmwareZeroDbfs - dry;
		portSq += portWet * portWet;
		fwSq += fwWet * fwWet;
	}
	const auto n = static_cast<double>(frames - from);
	w.port = std::sqrt(portSq / n);
	w.firmware = std::sqrt(fwSq / n);
	return w;
}

} // namespace

TEST_CASE("the port's wet matches the firmware's loop, run at the firmware's own signal level") {
	int compared = 0;
	int heldToSamples = 0;
	int wrapping = 0;
	double worstLevelDb = 0.0;
	for (int ti = 0; ti < 16; ++ti) {
		for (int ci = 0; ci < 6; ++ci) {
			const double tone = static_cast<double>(ti) * 0.0613;
			const double ch = static_cast<double>(ci) * 0.1571;
			CAPTURE(tone);
			CAPTURE(ch);
			const WetCompare w = compareWet(tone, ch, 127, 0.5f);
			CAPTURE(w.port);
			CAPTURE(w.firmware);
			CAPTURE(w.firstApart);
			CAPTURE(w.firstDesync);
			CAPTURE(w.wraps);
			REQUIRE(w.firmware > 1e-3); // the firmware's wet is audible here, or the case asks nothing

			// THE LEVEL — the complaint was intensity. The first release failed this by 15 dB at the
			// default coordinate. 1.5 dB rather than a hair, because where the filter wraps the wet is
			// chaotic and a 0.37 s RMS of it is itself an estimate.
			const double levelDb = 20.0 * std::log10(w.port / w.firmware);
			worstLevelDb = std::max(worstLevelDb, std::abs(levelDb));
			CHECK(std::abs(levelDb) < 1.5);

			// THE SIGNAL, SAMPLE FOR SAMPLE, until the random streams part — wherever the pitched
			// triangle is off. Where it is on, its step is a float in the port and a q31 in the
			// firmware, ~1e-7 apart, and its phase drifts by that per sample until the outputs cross
			// the 1e-4 threshold; its frequency and depth are asserted by their own case instead.
			if (evalToneBank(tone)[5] == 0.0f) {
				CHECK(w.firstApart >= w.firstDesync);
				++heldToSamples;
			}
			wrapping += w.wraps > 0 ? 1 : 0;
			++compared;
		}
	}
	MESSAGE("over " << compared << " coordinates: worst wet level " << worstLevelDb << " dB off the firmware; "
	                << heldToSamples << " held sample-for-sample; the firmware wrapped at " << wrapping);
	// NOT VACUOUS IN EITHER DIRECTION: the grid reaches the wrapping country the port now reproduces, and
	// enough of it has the triangle off for the sample assertion to mean something.
	CHECK(wrapping >= 8);
	CHECK(heldToSamples >= 30);
}

TEST_CASE("the codec and the declarations") {
	const Settings d = declared();
	CHECK(d.enabled);
	CHECK(d.cutoffOctaves == 0.0f);
	CHECK(d.gamma == 0.0f);
	CHECK(d.mode == Mode::Dust);
	CHECK(Settings {} == d);

	State s;
	s.settings.tone = 3.25f;
	s.settings.character = -1.5f;
	s.settings.gamma = 12.0f;
	s.settings.cutoffOctaves = -1.25f;
	s.settings.mix = 0.3f;
	CHECK(stateOf(rowOf(s), static_cast<size_t>(Param::kCount)) == s);

	const State shortRead = stateOf(rowOf(s), static_cast<size_t>(Param::Tone) + 1);
	CHECK(shortRead.settings.tone == 3.25f);
	CHECK(shortRead.settings.mix == d.mix);

	State c;
	setValueOf(&c, Param::Cutoff, 9.0f);
	CHECK(c.settings.cutoffOctaves == 2.0f);
	setValueOf(&c, Param::Tone, 1234.5f); // NOT a wall: the coordinate keeps turning
	CHECK(c.settings.tone == 1234.5f);

	// THE TWO ZONED COORDINATES: eight zones as the drag and route UNIT, a route moves them one — and NO NAMES since
	// backlog IW: they are continuous, so Free-only, with a sigil where the dropdown was.
	for (const Param p : { Param::Tone, Param::Character }) {
		const device::ParamDesc& desc = kParamDescs[static_cast<size_t>(p)];
		CHECK(desc.zones == 8);
		CHECK(desc.modSpan == 0.125);
		CHECK(desc.zoneNames.empty());
		CHECK(desc.modulatable);
		CHECK_FALSE(device::isDragWindow(desc)); // zoned: its own seating, not a value box
	}
	// GAMMA IS A DRAG WINDOW AND SO A VALUE BOX — the codebase norm, by declaration not by name.
	CHECK(device::isDragWindow(kParamDescs[static_cast<size_t>(Param::Gamma)]));
	CHECK(kTraits.perVoiceControl == -1);

	// THE FACE: three across (photographed at four — "CHARACTER" clipped and Gamma's label ran into
	// its number), the firmware's four in menu order, Gamma last, every control but Enabled once.
	CHECK(kTraits.faceColumns == 3);
	// THE SHARED SECTION'S EIGHT (`kSharedParamCount`) ARE NOT ON THE FACE YET: a later change draws them.
	REQUIRE(std::size(kFaceOrder) == static_cast<size_t>(Param::kCount) - 1 - kSharedParamCount - kEtchParamCount - kEmphOffsetParamCount - kInputParamCount - kHpWetSemisParamCount);
	// THE COMB'S SEVEN, in that order, among whatever else the face holds: the other engine's are not drawn with them.
	std::vector<uint8_t> comb;
	for (const uint8_t c : kFaceOrder) {
		const auto& desc = kParamDescs[c];
		if (c == static_cast<uint8_t>(Param::Mix) || (desc.gateControl >= 0 && desc.gateMask == kCombOnly)) {
			comb.push_back(c);
		}
	}
	REQUIRE(comb.size() == 7);
	CHECK(comb[5] == static_cast<uint8_t>(Param::Heat));
	CHECK(comb[6] == static_cast<uint8_t>(Param::Blend));
	CHECK(comb[0] == static_cast<uint8_t>(Param::Tone));
	CHECK(comb[1] == static_cast<uint8_t>(Param::Character));
	CHECK(comb[2] == static_cast<uint8_t>(Param::Cutoff));
	CHECK(comb[3] == static_cast<uint8_t>(Param::Mix));
	CHECK(comb[4] == static_cast<uint8_t>(Param::Gamma));
}

TEST_CASE("SURVEY: what the default coordinate and the eight zone centres derive to") {
	// NOT AN ASSERTION ABOUT TASTE — a record, printed, of what a hand finds where. Automod shipped a
	// default at which it was a static filter; this is how that is noticed before anyone hears it.
	const Settings d = defaults();
	for (int z = -1; z < 8; ++z) {
		const double tone = z < 0 ? static_cast<double>(d.tone) : (static_cast<double>(z) + 0.5) / 8.0;
		const double ch = z < 0 ? static_cast<double>(d.character) : (static_cast<double>(z) + 0.5) / 8.0;
		const Derived v = derive({ tone, ch }, 0.0, 1.0, 60.0, kRate);
		MESSAGE((z < 0 ? "default" : "zone   ") << " " << z << ": delay " << v.baseDelaySeconds * 1000.0 << " ms, fb "
		        << v.feedback << ", clip " << v.clipIntensity << ", fold " << v.foldIntensity << ", hpf " << v.hpfHz
		        << " Hz, tri " << v.triangleDepth << " @ " << v.triangleHz << " Hz | depth " << v.depth << ", res "
		        << v.resonance << ", s&h " << v.shBlend << ", width " << v.width << ", gain " << v.wetGain);
	}
}

TEST_CASE("the pitched triangle WRAPS where the firmware's does, instead of reflecting") {
	// **THE FIRMWARE'S `triValue += triStep` IS A PLAIN INT ADD.** Its step is clamped to 0.9 and to the
	// peak, but nothing clamps peak PLUS step — so a deep triangle at a high pitch runs past 2^31 and
	// comes back in at the bottom, and the reflection tests that follow never see it: the "triangle"
	// becomes a ramp that climbs from the floor. MEASURED on the reference: 3,670 wraps in half a second
	// at Tone 0.2452. The comparison case above cannot guard this — its sample assertion leaves out the
	// triangle's coordinates because of the step's float/q31 rounding — so it is held here, against the
	// reference's own triangle, over a stretch short enough that the rounding cannot matter.
	const double tonePhase = findPhase([](double ph) { return evalToneBank(ph)[5] > 0.85f; });
	const double charPhase = findPhase([](double ph) { return evalCharacterBank(ph)[4] > 0.6f; });
	REQUIRE(tonePhase >= 0.0);
	REQUIRE(charPhase >= 0.0);
	const Derived d = derive({ tonePhase, charPhase }, 0.0, 1.0, 60.0, 44100.0);
	const double step = std::min(std::min(4.0 * d.triangleDepth * d.triangleHz / 44100.0, 0.9), d.triangleDepth);
	CAPTURE(d.triangleDepth);
	CAPTURE(step);
	REQUIRE(d.triangleDepth + step > 1.0); // or nothing here can wrap, and the case asks nothing

	const size_t frames = 400;
	std::vector<float> in(frames, 0.0f);

	Eroder e;
	e.prepare(44100.0);
	Settings s;
	s.mode = Mode::Comb;
	s.tone = static_cast<float>(tonePhase);
	s.character = static_cast<float>(charPhase);
	s.mix = 1.0f;
	e.setTarget(s);
	std::vector<float> port(frames);
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (size_t i = 0; i < frames; ++i) {
		e.processStereo(l.data() + i, r.data() + i, 1);
		port[i] = e.triangleForTest();
	}

	RefEroder ref;
	std::vector<q31_t> ql(frames, 0);
	std::vector<q31_t> qr(frames, 0);
	ref.process(ql, qr, static_cast<double>(s.tone), static_cast<double>(s.character), 127);

	int wrapsSeen = 0;
	for (size_t i = 0; i < frames; ++i) {
		CAPTURE(i);
		const double fw = static_cast<double>(ref.triAt[i]) / 2147483648.0;
		CHECK(static_cast<double>(port[i]) == doctest::Approx(fw).epsilon(1e-3).scale(1.0));
		// A WRAP IS A FALL OF MORE THAN THE PEAK IN ONE SAMPLE — a reflection can never do that.
		if (i > 0 && static_cast<double>(ref.triAt[i - 1]) - static_cast<double>(ref.triAt[i]) > 2147483648.0 * d.triangleDepth) {
			++wrapsSeen;
		}
	}
	CHECK(wrapsSeen > 0);
}

//--------------------------------------------------------------------------------------------------//
// DUST — the second engine. Every expectation below is restated here from the design, in seconds and //
// hertz, and none reads `deriveDust` to build itself.                                                //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A DETERMINISTIC WHITE INPUT of its own, so nothing here shares a stream with the device.
std::vector<float> whiteInput(size_t frames, float amp, uint32_t seed = 0x1234567u) {
	std::vector<float> out(frames);
	uint32_t x = seed;
	for (float& v : out) {
		x = x * 1664525u + 1013904223u;
		v = amp * (static_cast<float>(static_cast<int32_t>(x) >> 8) * (1.0f / 8388608.0f));
	}
	return out;
}

float secondsToLog2(double seconds) { return static_cast<float>(std::log2(seconds)); }
float hertzToLog2(double hertz) { return static_cast<float>(std::log2(hertz)); }

Settings dustStill() {
	// DUST WITH THE HEAD AT REST: Amount at the bottom of its range is no travel at all.
	Settings s = declared();
	s.amount = static_cast<float>(kDustAmountLog2Min);
	return s;
}

double rmsOf(const std::vector<float>& v, size_t from = 0) {
	double sum = 0.0;
	for (size_t i = from; i < v.size(); ++i) {
		sum += static_cast<double>(v[i]) * static_cast<double>(v[i]);
	}
	return std::sqrt(sum / static_cast<double>(v.size() - from));
}

} // namespace

TEST_CASE("Dust is the declared engine, and its controls are gated to it as the Comb's are to the Comb") {
	CHECK(declared().mode == Mode::Dust);
	const auto gatedTo = [](Param p, uint32_t mask) {
		const auto& d = kParamDescs[static_cast<size_t>(p)];
		return d.gateControl == static_cast<int16_t>(Param::Mode) && d.gateMask == mask;
	};
	const uint32_t dust = 1u << static_cast<uint32_t>(Mode::Dust);
	const uint32_t rust = 1u << static_cast<uint32_t>(Mode::Rust);
	const uint32_t etch = 1u << static_cast<uint32_t>(Mode::Etch);
	// THE HEAD'S OWN: only Dust has a line to move.
	CHECK(gatedTo(Param::Amount, dust));
	// RESONANCE IS THE INNER LOOP, and Rust and Etch have one too (a delay of one period of Freq, `loops.h`); Freq, Width
	// and Track are the shared section's centre, width and pitch, so Etch has them as well.
	for (const Param p : { Param::Resonance, Param::Freq, Param::Width, Param::Track }) {
		CHECK(gatedTo(p, dust | rust | etch));
	}
	// THE SHAKER'S: Dust moves a head with it, Rust multiplies by it; Etch has no shake.
	for (const Param p : { Param::Source, Param::Grit, Param::Stereo, Param::Step, Param::Detune, Param::ModFold }) {
		CHECK(gatedTo(p, dust | rust));
	}
	for (const Param p : { Param::RustBias, Param::RustRectify }) {
		CHECK(gatedTo(p, rust));
	}
	for (const Param p : { Param::Heat, Param::Blend }) {
		CHECK(gatedTo(p, 1u << static_cast<uint32_t>(Mode::Comb)));
	}
	for (const Param p : { Param::Tone, Param::Character, Param::Gamma, Param::Cutoff }) {
		const auto& d = kParamDescs[static_cast<size_t>(p)];
		CHECK(d.gateControl == static_cast<int16_t>(Param::Mode));
		CHECK(d.gateMask == (1u << static_cast<uint32_t>(Mode::Comb)));
	}
	// BOTH ENGINES': the Mix, the engine itself, the Drive stage and Follow.
	for (const Param p : { Param::Mix, Param::Mode, Param::Drive, Param::DriveMode, Param::DriveShape, Param::Follow,
	                       Param::HpWet, Param::SubThru }) {
		CHECK(kParamDescs[static_cast<size_t>(p)].gateControl == -1);
	}

	// THE CODEC CARRIES EVERY ONE.
	State s;
	s.settings.mode = Mode::Comb;
	s.settings.amount = -11.5f;
	s.settings.freq = 9.25f;
	s.settings.width = 0.2f;
	s.settings.source = 0.4f;
	s.settings.grit = 0.6f;
	s.settings.stereo = 0.3f;
	s.settings.resonance = -0.7f;
	s.settings.drive = 0.8f;
	s.settings.track = true;
	s.settings.driveMode = DriveMode::Crush;
	s.settings.driveShape = 0.35f;
	s.settings.hpWet = 8.5f;
	s.settings.subThru = true;
	s.settings.follow = 0.45f;
	s.settings.rustBias = 0.2f;
	s.settings.rustRectify = 0.9f;
	s.settings.step = -9.0f;
	s.settings.detune = 0.3f;
	s.settings.modFold = 0.7f;
	s.settings.heat = 0.55f;
	s.settings.blend = true;
	CHECK(stateOf(rowOf(s), static_cast<size_t>(Param::kCount)) == s);
}

TEST_CASE("Dust: one quantity per control") {
	Settings s = declared();
	const double rate = 48000.0;

	// AMOUNT IS A TIME; the bottom of its range is none.
	s.amount = secondsToLog2(250.0e-6);
	CHECK(std::abs(deriveDust(s, 60.0, rate).amountSeconds - 250.0e-6) < 1.0e-9);
	s.amount = static_cast<float>(kDustAmountLog2Min);
	CHECK(deriveDust(s, 60.0, rate).amountSeconds == 0.0);

	// FREQ IS HERTZ, and with Track on an octave up doubles it; with Track off the note does nothing.
	s.freq = hertzToLog2(1500.0);
	CHECK(std::abs(deriveDust(s, 72.0, rate).freqHz - 1500.0) < 0.01);
	s.track = true;
	CHECK(std::abs(deriveDust(s, 72.0, rate).freqHz - 3000.0) < 0.02);
	CHECK(std::abs(deriveDust(s, 60.0, rate).freqHz - 1500.0) < 0.01);
	// HELD UNDER THE RATE'S CEILING however far a note pushes it.
	CHECK(deriveDust(s, 127.0, rate).freqHz <= 0.45 * rate);
	// A ROUTE MAY CARRY FREQ PAST THE KNOB'S TOP, which is what lets a tracked low note reach the top of the band:
	// two octaves under middle C the knob's own top is a quarter of it, and two octaves of offset give it back.
	REQUIRE(kDescriptor.params[static_cast<size_t>(Param::Freq)].offsetsMayLeaveRange);
	s.freq = static_cast<float>(kDustFreqLog2Max);
	const double top = std::exp2(kDustFreqLog2Max);
	CHECK(std::abs(deriveDust(s, 36.0, rate).freqHz / (top / 4.0) - 1.0) < 1.0e-4);
	s.freq = static_cast<float>(kDustFreqLog2Max + 2.0);
	CHECK(std::abs(deriveDust(s, 36.0, rate).freqHz / top - 1.0) < 1.0e-4);
	// AND HELD IN HERTZ AT BOTH ENDS however far a route goes.
	s.freq = static_cast<float>(kDustFreqLog2Max + 9.0);
	CHECK(deriveDust(s, 36.0, rate).freqHz == std::min(20000.0, 0.45 * rate));
	s.freq = static_cast<float>(kDustFreqLog2Min - 9.0);
	CHECK(deriveDust(s, 36.0, rate).freqHz == 5.0);
	s.freq = hertzToLog2(1500.0);
	s.track = false;

	// WIDTH: Q 16 at the bottom, a half at the top, even in octaves between.
	s.width = 0.0f;
	CHECK(std::abs(deriveDust(s, 60.0, rate).q - 16.0) < 1.0e-9);
	s.width = 1.0f;
	CHECK(std::abs(deriveDust(s, 60.0, rate).q - 0.5) < 1.0e-9);
	s.width = 0.5f;
	CHECK(std::abs(deriveDust(s, 60.0, rate).q - std::sqrt(16.0 * 0.5)) < 1.0e-6);

	// RESONANCE IS SIGNED FEEDBACK, AND THE WET IS BROUGHT BACK BY THE COMB'S BROADBAND GAIN.
	s.resonance = -0.5f;
	s.mix = 0.8f;
	const DustDerived d = deriveDust(s, 60.0, rate);
	CHECK(std::abs(d.feedback - (-0.45)) < 1.0e-9);
	CHECK(std::abs(d.wetGain - 0.8f * std::sqrt(1.0 - 0.45 * 0.45)) < 1.0e-6);
	CHECK(std::abs(d.dryGain - (1.0 - static_cast<double>(0.8f))) < 1.0e-9);

	// DRIVE: not run at the bottom; from an eighth to sixteen times into the shaper above it.
	s.drive = 0.0f;
	CHECK_FALSE(deriveDust(s, 60.0, rate).driven);
	s.drive = 1.0f;
	CHECK(deriveDust(s, 60.0, rate).driven);
	CHECK(std::abs(deriveDust(s, 60.0, rate).driveGain - 16.0) < 1.0e-9);
	s.drive = 3.0f / 7.0f;
	CHECK(std::abs(deriveDust(s, 60.0, rate).driveGain - 1.0) < 1.0e-6);
}

TEST_CASE("Dust: with the head at rest the wet is the input two frames late, to the bit; at Mix 0 the dry, to the bit") {
	const size_t frames = 2000;
	const std::vector<float> in = whiteInput(frames, 0.5f);

	Eroder e;
	e.prepare(kRate);
	Settings s = dustStill();
	s.mix = 1.0f;
	e.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	e.processStereo(l.data(), r.data(), static_cast<int>(frames));
	CHECK(l[0] == 0.0f);
	CHECK(l[1] == 0.0f);
	size_t same = 0;
	for (size_t i = 2; i < frames; ++i) {
		same += (l[i] == in[i - 2] && r[i] == in[i - 2]) ? 1u : 0u;
	}
	CHECK(same == frames - 2);

	Eroder dry;
	dry.prepare(kRate);
	s = declared();
	s.mix = 0.0f;
	dry.setTarget(s);
	l = in;
	r = in;
	dry.processStereo(l.data(), r.data(), static_cast<int>(frames));
	CHECK(l == in);
	CHECK(r == in);
	// AND THE LINE KEPT FILLING: Mix 0 does not freeze the engine.
	CHECK(dry.holdsTail());
}

TEST_CASE("Dust: the head travels from the margin to the margin plus twice the Amount, and no further") {
	for (const double rate : { 44100.0, 96000.0 }) {
		for (const double amount : { 20.0e-6, 300.0e-6, 1.9e-3 }) {
			for (const float source : { 0.0f, 1.0f }) {
				Eroder e;
				e.prepare(rate);
				Settings s = declared();
				s.amount = secondsToLog2(amount);
				s.source = source;
				s.freq = hertzToLog2(3000.0);
				e.setTarget(s);
				double lo = 1.0e9;
				double hi = -1.0e9;
				float l = 0.1f;
				float r = 0.1f;
				for (int i = 0; i < static_cast<int>(rate); ++i) {
					l = r = 0.1f;
					e.processStereo(&l, &r, 1);
					for (int ch = 0; ch < 2; ++ch) {
						lo = std::min(lo, e.dustForTest().lastDelayFramesForTest(ch));
						hi = std::max(hi, e.dustForTest().lastDelayFramesForTest(ch));
					}
				}
				const double travel = amount * rate;
				CAPTURE(rate);
				CAPTURE(amount);
				CAPTURE(source);
				CHECK(lo >= 2.0);
				CHECK(hi <= 2.0 + 2.0 * travel * 1.0001);
				// NOT VACUOUS: it used most of the room it was given.
				CHECK(hi - lo > 1.5 * travel);
			}
		}
	}
}

TEST_CASE("Dust: the band noise is as deep at any Freq, Width and rate") {
	struct Point {
		double hz;
		float width;
		double seconds;
	};
	// A NARROW BAND LOW DOWN decorrelates slowly, so it gets longer to average over.
	const Point points[] = {
		{ 200.0, 1.0f, 4.0 },   { 200.0, 0.5f, 12.0 },  { 1000.0, 0.0f, 12.0 }, { 1000.0, 1.0f, 3.0 },
		{ 4000.0, 0.5f, 3.0 },  { 4000.0, 0.0f, 6.0 },  { 12000.0, 0.5f, 3.0 }, { 12000.0, 1.0f, 3.0 },
		{ 17000.0, 0.0f, 4.0 },
	};
	for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
		for (const Point& p : points) {
			Eroder e;
			e.prepare(rate);
			Settings s = declared();
			s.freq = hertzToLog2(p.hz);
			s.width = p.width;
			e.setTarget(s);
			const auto frames = static_cast<size_t>(p.seconds * rate);
			const size_t skip = frames / 8;
			double sum = 0.0;
			float l = 0.0f;
			float r = 0.0f;
			for (size_t i = 0; i < frames; ++i) {
				l = r = 0.0f;
				e.processStereo(&l, &r, 1);
				if (i >= skip) {
					const auto n = static_cast<double>(e.dustForTest().lastNoiseForTest(0));
					sum += n * n;
				}
			}
			const double rms = std::sqrt(sum / static_cast<double>(frames - skip));
			CAPTURE(rate);
			CAPTURE(p.hz);
			CAPTURE(p.width);
			// WITHIN A DECIBEL OF 0.45, the figure the design names.
			CHECK(rms > 0.45 / 1.122);
			CHECK(rms < 0.45 * 1.122);
		}
	}
}

TEST_CASE("Dust: the shaking sits at Freq — the sine exactly, a narrow band about it — at any rate") {
	for (const double rate : { 44100.0, 96000.0 }) {
		for (const double hz : { 300.0, 2500.0, 9000.0 }) {
			for (const float source : { 0.0f, 1.0f }) {
				Eroder e;
				e.prepare(rate);
				Settings s = declared();
				s.freq = hertzToLog2(hz);
				s.width = 0.0f;
				s.source = source;
				e.setTarget(s);
				const auto frames = static_cast<size_t>(4.0 * rate);
				size_t crossings = 0;
				float prev = 0.0f;
				float l = 0.0f;
				float r = 0.0f;
				for (size_t i = 0; i < frames; ++i) {
					l = r = 0.0f;
					e.processStereo(&l, &r, 1);
					const float m = e.dustForTest().lastModForTest(0);
					crossings += ((m < 0.0f) != (prev < 0.0f)) ? 1u : 0u;
					prev = m;
				}
				const double measured = static_cast<double>(crossings) / (2.0 * 4.0);
				CAPTURE(rate);
				CAPTURE(hz);
				CAPTURE(source);
				// A SINE CROSSES TWICE A CYCLE; a Q-16 band's crossings sit within a few percent of its centre.
				const double tolerance = source == 0.0f ? 0.002 : 0.06;
				CHECK(std::abs(measured - hz) < tolerance * hz);
			}
		}
	}
}

TEST_CASE("Dust: Stereo at zero puts both heads on one path; up parts them") {
	for (const float source : { 0.0f, 1.0f }) {
		for (const float stereo : { 0.0f, 1.0f }) {
			Eroder e;
			e.prepare(kRate);
			Settings s = declared();
			s.source = source;
			s.stereo = stereo;
			e.setTarget(s);
			size_t apart = 0;
			float l = 0.0f;
			float r = 0.0f;
			for (int i = 0; i < 4000; ++i) {
				l = r = 0.2f;
				e.processStereo(&l, &r, 1);
				apart += e.dustForTest().lastDelayFramesForTest(0) != e.dustForTest().lastDelayFramesForTest(1) ? 1u : 0u;
			}
			CAPTURE(source);
			CAPTURE(stereo);
			if (stereo == 0.0f) {
				CHECK(apart == 0);
			}
			else {
				CHECK(apart > 3900);
			}
		}
	}
}

TEST_CASE("Dust: Grit holds the head between the input's zero crossings, and re-draws at each") {
	const double hz = 173.0;
	Eroder e;
	e.prepare(kRate);
	Settings s = declared();
	s.grit = 1.0f;
	e.setTarget(s);
	size_t expectedHolds = 0;
	size_t movedOffCrossing = 0;
	float prevIn = 0.0f;
	double prevDelay = -1.0;
	float l = 0.0f;
	float r = 0.0f;
	for (int i = 0; i < 48000; ++i) {
		const auto in = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * hz * (static_cast<double>(i) + 0.5) / kRate));
		const bool crossed = (in < 0.0f) != (prevIn < 0.0f);
		expectedHolds += crossed ? 1u : 0u;
		prevIn = in;
		l = r = in;
		e.processStereo(&l, &r, 1);
		const double delay = e.dustForTest().lastDelayFramesForTest(0);
		if (!crossed && prevDelay >= 0.0 && delay != prevDelay) {
			++movedOffCrossing;
		}
		prevDelay = delay;
	}
	CHECK(expectedHolds > 300);
	CHECK(e.dustForTest().holdsForTest() == expectedHolds);
	CHECK(movedOffCrossing == 0);
}

TEST_CASE("Dust: Resonance is feedback round the line — an impulse comes back at r, r squared, … — and no quieter") {
	// THE HEAD AT REST, so the loop is three frames long — the head's two and the one the tap waits before it is
	// written back — and the answer can be written down.
	const double r = 0.6 * 0.9;
	const size_t frames = 64;
	std::vector<float> l(frames, 0.0f);
	l[0] = 1.0f;
	std::vector<float> rr = l;
	Eroder e;
	e.prepare(kRate);
	Settings s = dustStill();
	s.resonance = 0.6f;
	s.mix = 1.0f;
	e.setTarget(s);
	e.processStereo(l.data(), rr.data(), static_cast<int>(frames));
	const double makeup = std::sqrt(1.0 - r * r);
	for (size_t k = 0; k < 20; ++k) {
		const double expected = makeup * std::pow(r, static_cast<double>(k));
		CHECK(std::abs(static_cast<double>(l[2 + 3 * k]) - expected) < 2.0e-6);
		CHECK(l[3 * k] == 0.0f);
		CHECK(l[3 * k + 1] == 0.0f);
	}

	// AND ON A BROADBAND INPUT THE LEVEL HOLDS, either sign.
	const std::vector<float> in = whiteInput(96000, 0.25f);
	double level[3] = {};
	const float settings[3] = { 0.0f, 0.9f, -0.9f };
	for (int c = 0; c < 3; ++c) {
		Eroder d;
		d.prepare(kRate);
		Settings t = dustStill();
		t.resonance = settings[c];
		d.setTarget(t);
		std::vector<float> a = in;
		std::vector<float> b = in;
		d.processStereo(a.data(), b.data(), static_cast<int>(in.size()));
		level[c] = rmsOf(a, 4800);
	}
	CHECK(level[1] / level[0] > 1.0 / 1.122);
	CHECK(level[1] / level[0] < 1.122);
	CHECK(level[2] / level[0] > 1.0 / 1.122);
	CHECK(level[2] / level[0] < 1.122);
}

namespace {
/// A SINE THROUGH DUST WITH THE HEAD AT REST, both sides alike: the left, after `settle` frames.
std::vector<float> stillSine(const Settings& s, double hz, double peak, size_t frames, size_t settle, int block = 256) {
	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	std::vector<float> l(frames + settle);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = static_cast<float>(peak * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
	}
	std::vector<float> r = l;
	for (size_t done = 0; done < l.size(); done += static_cast<size_t>(block)) {
		e.processStereo(l.data() + done, r.data() + done, static_cast<int>(std::min<size_t>(block, l.size() - done)));
	}
	return { l.begin() + static_cast<std::ptrdiff_t>(settle), l.end() };
}

/// THE AMPLITUDE AT ONE FREQUENCY, by correlation over the whole vector (which holds whole cycles of it).
double toneAt(const std::vector<float>& v, double hz) {
	double re = 0.0;
	double im = 0.0;
	for (size_t i = 0; i < v.size(); ++i) {
		const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate;
		re += static_cast<double>(v[i]) * std::cos(w);
		im += static_cast<double>(v[i]) * std::sin(w);
	}
	return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(v.size());
}

double dbOf(double ratio) {
	return 20.0 * std::log10(std::max(ratio, 1.0e-12));
}

/// THE CURVES, RESTATED IN DOUBLE from their descriptions. `gain` is the Drive's, which Soft's dead zone is a share of.
double curveOf(DriveMode mode, double u, double shape, double gain = 1.0) {
	const double halfPi = std::numbers::pi * 0.5;
	switch (mode) {
	case DriveMode::Soft: {
		// STARVED: nothing under 0.6 of the reference peak (0.5) at full Shape, then the tanh of what is over it.
		const double zone = 0.6 * shape * gain * 0.5;
		const double over = std::max(0.0, std::abs(u) - zone);
		return std::tanh(u < 0.0 ? -over : over);
	}
	case DriveMode::Tube: {
		// TWO HALVES: a tanh above zero; below it a tanh of r u, over r, with r from 1.5 to 8 geometrically.
		const double r = 1.5 * std::pow(8.0 / 1.5, shape);
		return u >= 0.0 ? std::tanh(u) : std::tanh(r * u) / r;
	}
	case DriveMode::Fold: {
		// A SINE FOLDER READ FROM A BIAS of 0.75 shape quarter-periods, back through zero.
		const double b = 0.75 * shape;
		return std::sin(halfPi * (u + b)) - std::sin(halfPi * b);
	}
	case DriveMode::Wreck:
		return std::sin(halfPi * std::tanh(u) * (1.0 + 4.0 * shape));
	case DriveMode::Jag: {
		// A TRIANGLE OF PERIOD FOUR through the origin at a slope of one, after the tanh.
		const double x = std::tanh(u) * (1.0 + 4.0 * shape);
		const double phase = x - 4.0 * std::floor((x + 1.0) / 4.0);
		return phase <= 1.0 ? phase : 2.0 - phase;
	}
	case DriveMode::Chonk:
		return std::tanh(u);
	case DriveMode::Lean: {
		// A TANH READ FROM A BIAS, its bias from nothing, back through zero at a slope of one.
		const double b = 1.2 * shape;
		return (std::tanh(u + b) - std::tanh(b)) / (1.0 - std::tanh(b) * std::tanh(b));
	}
	default:
		return u;
	}
}

constexpr DriveMode kAllDrives[] = { DriveMode::Soft,  DriveMode::Tube,  DriveMode::Fold,     DriveMode::Wreck,
	                                 DriveMode::Jag,   DriveMode::Lean,  DriveMode::Chonk,   DriveMode::PaunchS,
	                                 DriveMode::PaunchH,  DriveMode::PaunchOS, DriveMode::Crush,   DriveMode::Decimate, DriveMode::Slew,
	                                 DriveMode::Lopside };
constexpr DriveMode kCurves[] = { DriveMode::Soft, DriveMode::Tube, DriveMode::Fold, DriveMode::Wreck,
	                                  DriveMode::Jag, DriveMode::Lean, DriveMode::Chonk };
} // namespace

TEST_CASE("Drive: each Type's curve is the one it is described as, through zero at zero") {
	for (const DriveMode mode : kCurves) {
		CAPTURE(static_cast<int>(mode));
		for (const float shape : { 0.0f, 0.3f, 1.0f }) {
			for (const float gain : { 0.25f, 1.0f, 12.0f }) {
				CHECK(shapeDrive(mode, 0.0f, shape, gain) == 0.0f);
				for (const float u : { -7.3f, -1.6f, -0.4f, 0.05f, 0.7f, 1.3f, 2.9f, 11.0f }) {
					CAPTURE(u);
					CAPTURE(shape);
					CAPTURE(gain);
					CHECK(std::abs(static_cast<double>(shapeDrive(mode, u, shape, gain))
					               - curveOf(mode, static_cast<double>(u), static_cast<double>(shape),
					                         static_cast<double>(gain)))
					      < 2.0e-5);
				}
			}
		}
	}
	// A SHAPE PAST ITS END IS ITS END, and the Type is whatever it was: nothing rolls over.
	Settings past = declared();
	past.drive = 0.5f;
	past.driveMode = DriveMode::Fold;
	past.driveShape = 1.7f;
	CHECK(deriveDust(past, 60.0, kRate).driveShape == 1.0f);
	CHECK(deriveDust(past, 60.0, kRate).driveMode == DriveMode::Fold);
	past.driveShape = -0.3f;
	CHECK(deriveDust(past, 60.0, kRate).driveShape == 0.0f);
	CHECK(deriveDust(past, 60.0, kRate).driveMode == DriveMode::Fold);
	// THE TYPES THAT ARE NOT CURVES ARE THE IDENTITY HERE.
	for (const DriveMode mode : { DriveMode::Crush, DriveMode::Decimate, DriveMode::Slew, DriveMode::Lopside }) {
		CHECK(shapeDrive(mode, 0.37f, 0.5f, 4.0f) == 0.37f);
	}
	// TUBE'S TWO HALVES DIFFER, more with Shape, and both go through zero at a slope of one: the negative half
	// (the steeper) is flat long before the positive.
	for (const float shape : { 0.0f, 0.5f, 1.0f }) {
		CHECK(std::abs(shapeDrive(DriveMode::Tube, 1.0e-4f, shape, 1.0f) / 1.0e-4f - 1.0f) < 1.0e-3f);
		CHECK(std::abs(shapeDrive(DriveMode::Tube, -1.0e-4f, shape, 1.0f) / -1.0e-4f - 1.0f) < 1.0e-3f);
	}
	CHECK(shapeDrive(DriveMode::Tube, -2.0f, 0.0f, 1.0f) > -0.9f * shapeDrive(DriveMode::Tube, 2.0f, 0.0f, 1.0f));
	CHECK(shapeDrive(DriveMode::Tube, -2.0f, 1.0f, 1.0f) > -0.2f * shapeDrive(DriveMode::Tube, 2.0f, 1.0f, 1.0f));
	// SOFT STARVES: at full Shape everything under 0.6 of the reference peak (times the Drive's gain) is exactly
	// nothing, and above it comes up from the edge of the zone; at none it is a plain tanh, to the bit.
	for (const float gain : { 0.5f, 3.0f }) {
		const float zone = 0.6f * 0.5f * gain;
		CHECK(shapeDrive(DriveMode::Soft, 0.99f * zone, 1.0f, gain) == 0.0f);
		CHECK(shapeDrive(DriveMode::Soft, -0.99f * zone, 1.0f, gain) == 0.0f);
		CHECK(shapeDrive(DriveMode::Soft, zone + 0.5f, 1.0f, gain) > 0.4f);
		CHECK(shapeDrive(DriveMode::Soft, -zone - 0.5f, 1.0f, gain) < -0.4f);
		for (const float u : { -2.5f, -0.3f, 0.7f, 5.0f }) {
			CHECK(shapeDrive(DriveMode::Soft, u, 0.0f, gain) == std::tanh(u));
		}
	}
	// A DEAD ZONE HAS NO SLOPE AT ZERO; the others' are as described.
	CHECK(driveSlope(DriveMode::Soft, 0.0f) == 1.0);
	CHECK(driveSlope(DriveMode::Soft, 0.2f) == 0.0);
	CHECK(driveSmallSignalGain(DriveMode::Soft, 4.0, 1.0f) == 0.0);
	CHECK(std::abs(driveSlope(DriveMode::Fold, 1.0f) - std::numbers::pi * 0.5 * std::cos(std::numbers::pi * 0.5 * 0.75)) < 1.0e-12);
	CHECK(driveSlope(DriveMode::Tube, 1.0f) == 1.0);
}

TEST_CASE("Drive: the make-up puts the reference sine out at the level it went in, and a vanishing signal's gain is the curve's slope") {
	for (const DriveMode mode : kCurves) {
		for (const double gain : { 0.125, 1.0, 16.0 }) {
			for (const float shape : { 0.0f, 0.6f, 1.0f }) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(gain);
				CAPTURE(shape);
				// A FINE QUADRATURE of the restated curve, its mean taken out.
				double sum = 0.0;
				double squares = 0.0;
				const int n = 4096;
				for (int i = 0; i < n; ++i) {
					const double y = curveOf(mode, gain * 0.5 * std::sin(2.0 * std::numbers::pi * (i + 0.5) / n),
					                         static_cast<double>(shape), gain);
					sum += y;
					squares += y * y;
				}
				const double rms = std::sqrt(squares / n - (sum / n) * (sum / n));
				const double makeUp = driveMakeUp(mode, gain, shape);
				CHECK(std::abs(dbOf(makeUp * rms / (0.5 / std::sqrt(2.0)))) < 0.25);
				// THE SLOPE AT ZERO, by a small step on the restated curve.
				const double slope = curveOf(mode, 1.0e-4, static_cast<double>(shape), gain) / 1.0e-4;
				if (mode == DriveMode::Soft && shape > 0.0f) {
					// A DEAD ZONE: no slope at zero, so nothing for a feedback through it to be divided by.
					CHECK(slope == 0.0);
					CHECK(driveSmallSignalGain(mode, gain, shape) == 0.0);
				}
				else {
					CHECK(std::abs(driveSmallSignalGain(mode, gain, shape) / (slope * gain * makeUp) - 1.0) < 1.0e-3);
				}
			}
		}
	}
	for (const DriveMode mode : { DriveMode::Crush, DriveMode::Decimate, DriveMode::Slew, DriveMode::Lopside }) {
		CHECK(driveMakeUp(mode, 16.0, 1.0f) == 1.0);
		CHECK(driveSmallSignalGain(mode, 16.0, 1.0f) == 1.0);
	}
}

TEST_CASE("Drive: at zero it is not run; just above, the sound is all but the same; the level holds all the way up") {
	Settings s = dustStill();
	const std::vector<float> clean = stillSine(s, 1000.0, 0.5, 4800, 4800);
	const double level = toneAt(clean, 1000.0);
	REQUIRE(std::abs(level - 0.5) < 1.0e-4);
	for (const DriveMode mode : kCurves) {
		CAPTURE(static_cast<int>(mode));
		s.driveMode = mode;
		s.driveShape = 0.0f;
		// JUST ABOVE THE BOTTOM: the fundamental within a twentieth of a dB, everything it adds 50 dB under it (Tube's
		// lean is a second harmonic from the first, 40 under).
		s.drive = 0.01f;
		const std::vector<float> barely = stillSine(s, 1000.0, 0.5, 4800, 4800);
		CHECK(std::abs(dbOf(toneAt(barely, 1000.0) / level)) < 0.05);
		for (const double harmonic : { 2000.0, 3000.0, 5000.0 }) {
			CAPTURE(harmonic);
			CHECK(dbOf(toneAt(barely, harmonic) / level) < (mode == DriveMode::Tube ? -40.0 : -50.0));
		}
		// AND THE WHOLE LEVEL IS HELD WITHIN A DECIBEL at any Drive (the reference sine is this one).
		for (const float drive : { 0.25f, 0.5f, 0.75f, 1.0f }) {
			CAPTURE(drive);
			s.drive = drive;
			const std::vector<float> out = stillSine(s, 1000.0, 0.5, 4800, 4800);
			CHECK(std::abs(dbOf(rmsOf(out) / rmsOf(clean))) < 1.0);
		}
	}
	// AT FULL SHAPE the reference sine (this one) is still put out at its level, whatever the Type; a quieter signal is
	// not, and Soft's dead zone cannot hold level for one that is under it (it is silent: its own case below).
	s.driveShape = 1.0f;
	for (const DriveMode mode : kCurves) {
		CAPTURE(static_cast<int>(mode));
		s.driveMode = mode;
		for (const float drive : { 0.25f, 0.5f, 0.75f, 1.0f }) {
			CAPTURE(drive);
			s.drive = drive;
			const std::vector<float> out = stillSine(s, 1000.0, 0.5, 4800, 4800);
			CHECK(std::abs(dbOf(rmsOf(out) / rmsOf(clean))) < 1.5);
		}
	}
	s.driveShape = 0.0f;
	// AT THE TOP IT IS A DISTORTION: Soft's third harmonic is within 20 dB of its fundamental.
	s.driveMode = DriveMode::Soft;
	s.drive = 1.0f;
	const std::vector<float> hard = stillSine(s, 1000.0, 0.5, 4800, 4800);
	CHECK(dbOf(toneAt(hard, 3000.0) / toneAt(hard, 1000.0)) > -20.0);
}

namespace {
double meanOf(const std::vector<float>& v) {
	double mean = 0.0;
	for (const float x : v) {
		mean += static_cast<double>(x);
	}
	return mean / static_cast<double>(v.size());
}
} // namespace

TEST_CASE("Drive: Tube's halves differ and Fold reads from a bias — even harmonics and no standing offset; Soft has none, nor has Fold at the bottom of Shape") {
	Settings s = dustStill();
	s.drive = 0.8f;
	s.driveMode = DriveMode::Tube;
	s.driveShape = 1.0f;
	const std::vector<float> lean = stillSine(s, 1000.0, 0.5, 4800, 48000);
	MESSAGE("Tube Shape 1: 2nd " << dbOf(toneAt(lean, 2000.0) / toneAt(lean, 1000.0)) << " dB, mean " << meanOf(lean));
	CHECK(dbOf(toneAt(lean, 2000.0) / toneAt(lean, 1000.0)) > -20.0);
	CHECK(std::abs(meanOf(lean)) < 1.0e-3);
	// FOLD READ FROM A BIAS: the even harmonics, and the blocker takes the offset out.
	s.driveMode = DriveMode::Fold;
	const std::vector<float> biased = stillSine(s, 1000.0, 0.5, 4800, 48000);
	MESSAGE("Fold Shape 1: 2nd " << dbOf(toneAt(biased, 2000.0) / toneAt(biased, 1000.0)) << " dB, mean " << meanOf(biased));
	CHECK(dbOf(toneAt(biased, 2000.0) / toneAt(biased, 1000.0)) > -20.0);
	CHECK(std::abs(meanOf(biased)) < 1.0e-3);
	s.driveShape = 0.0f;
	const std::vector<float> even = stillSine(s, 1000.0, 0.5, 4800, 48000);
	CHECK(dbOf(toneAt(even, 2000.0) / toneAt(even, 1000.0)) < -70.0);
	CHECK(dbOf(toneAt(even, 4000.0) / toneAt(even, 1000.0)) < -70.0);
	// SOFT HAS NONE AT ALL, starved or not.
	s.driveMode = DriveMode::Soft;
	for (const float shape : { 0.0f, 1.0f }) {
		s.driveShape = shape;
		const std::vector<float> level = stillSine(s, 1000.0, 0.5, 4800, 48000);
		CHECK(dbOf(toneAt(level, 2000.0) / toneAt(level, 1000.0)) < -70.0);
	}
}

TEST_CASE("Drive: Soft at full Shape is exactly silent under its dead zone and sounds over it") {
	// DRIVE 0.8 IS A GAIN OF 2^2.6, so the zone is 0.6 of the reference peak, 0.3, whatever the gain.
	const auto run = [](float peak, std::vector<float>& out) {
		DriveStage stage;
		stage.prepare(kRate);
		stage.set(deriveDrive(0.8f, DriveMode::Soft, 1.0f, kRate, nullptr));
		for (size_t i = 0; i < 96000; ++i) {
			const auto x = static_cast<float>(peak * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
			float l = x;
			float r = x;
			stage.process(l, r);
			if (i >= 48000) {
				out.push_back(l);
			}
		}
	};
	std::vector<float> under;
	run(0.25f, under);
	bool silent = true;
	for (const float v : under) {
		silent = silent && v == 0.0f;
	}
	CHECK(silent);
	std::vector<float> over;
	run(0.4f, over);
	MESSAGE("over the zone: 1 kHz " << toneAt(over, 1000.0));
	CHECK(toneAt(over, 1000.0) > 0.05);
}

TEST_CASE("Drive: Lean leans the two sides opposite ways — the even harmonics are the side, the mid has none, and at the bottom of Shape it is Soft's tanh") {
	// THE RIGHT'S CURVE IS THE LEFT'S TURNED OVER, in Lean alone.
	for (const float u : { -2.1f, -0.3f, 0.6f, 1.7f }) {
		CHECK(shapeDriveRight(DriveMode::Lean, u, 0.7f, 1.0f) == -shapeDrive(DriveMode::Lean, -u, 0.7f, 1.0f));
		CHECK(shapeDriveRight(DriveMode::Lean, u, 0.7f, 1.0f) != shapeDrive(DriveMode::Lean, u, 0.7f, 1.0f));
		CHECK(shapeDriveRight(DriveMode::Tube, u, 0.7f, 1.0f) == shapeDrive(DriveMode::Tube, u, 0.7f, 1.0f));
		CHECK(shapeDrive(DriveMode::Lean, u, 0.0f, 1.0f) == shapeDrive(DriveMode::Soft, u, 0.0f, 1.0f));
	}
	// THE STAGE, the same sine on both sides.
	const auto run = [](float shape, std::vector<float>& mid, std::vector<float>& side, std::vector<float>& left) {
		DriveStage stage;
		stage.prepare(kRate);
		stage.set(deriveDrive(0.8f, DriveMode::Lean, shape, kRate, nullptr));
		const size_t settle = 48000;
		const size_t frames = 4800;
		for (size_t i = 0; i < settle + frames; ++i) {
			const auto x = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
			float l = x;
			float r = x;
			stage.process(l, r);
			if (i >= settle) {
				mid.push_back(0.5f * (l + r));
				side.push_back(0.5f * (l - r));
				left.push_back(l);
			}
		}
	};
	std::vector<float> mid;
	std::vector<float> side;
	std::vector<float> left;
	run(1.0f, mid, side, left);
	const double fundamental = toneAt(mid, 1000.0);
	// EACH SIDE LEANS: a second harmonic within 30 dB of the fundamental, and no standing offset.
	CHECK(dbOf(toneAt(left, 2000.0) / toneAt(left, 1000.0)) > -30.0);
	double mean = 0.0;
	for (const float v : left) {
		mean += static_cast<double>(v);
	}
	CHECK(std::abs(mean / static_cast<double>(left.size())) < 1.0e-3);
	// THE SIDE IS THE EVEN HARMONICS and none of the odd; the mid is the odd and none of the even.
	CHECK(dbOf(toneAt(side, 2000.0) / toneAt(left, 2000.0)) > -0.1);
	CHECK(dbOf(toneAt(side, 1000.0) / fundamental) < -80.0);
	CHECK(dbOf(toneAt(side, 3000.0) / fundamental) < -80.0);
	CHECK(dbOf(toneAt(mid, 2000.0) / fundamental) < -80.0);
	CHECK(dbOf(toneAt(mid, 4000.0) / fundamental) < -80.0);
	CHECK(dbOf(toneAt(mid, 3000.0) / fundamental) > -40.0);
	// AT THE BOTTOM OF SHAPE THERE IS NO SIDE AT ALL.
	std::vector<float> flatMid;
	std::vector<float> flatSide;
	std::vector<float> flatLeft;
	run(0.0f, flatMid, flatSide, flatLeft);
	for (const float v : flatSide) {
		REQUIRE(v == 0.0f);
	}
	CHECK(toneAt(flatMid, 1000.0) > 0.1);
}

TEST_CASE("Drive: Chonk puts the odd harmonics in opposite polarity — Shape 0 is Soft's tanh, 0.5 leaves the right clean, 1 makes the mono sum the clean signal") {
	const auto run = [](DriveMode type, float shape, std::vector<float>& left, std::vector<float>& right) {
		DriveStage stage;
		stage.prepare(kRate);
		stage.set(deriveDrive(0.8f, type, shape, kRate, nullptr));
		const size_t settle = 48000;
		const size_t frames = 4800;
		for (size_t i = 0; i < settle + frames; ++i) {
			const auto x = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
			float l = x;
			float r = x;
			stage.process(l, r);
			if (i >= settle) {
				left.push_back(l);
				right.push_back(r);
			}
		}
	};
	std::vector<float> l0;
	std::vector<float> r0;
	run(DriveMode::Chonk, 0.0f, l0, r0);
	std::vector<float> softL;
	std::vector<float> softR;
	run(DriveMode::Soft, 0.0f, softL, softR);
	float worst = 0.0f;
	for (size_t i = 0; i < l0.size(); ++i) {
		REQUIRE(l0[i] == r0[i]); // the side is exactly nothing
		worst = std::max(worst, std::abs(l0[i] - softL[i]));
	}
	MESSAGE("Chonk Shape 0 against Soft Shape 0, worst difference: " << worst);
	CHECK(worst < 1.0e-6f);
	CHECK(toneAt(l0, 1000.0) > 0.1);

	std::vector<float> l1;
	std::vector<float> r1;
	run(DriveMode::Chonk, 1.0f, l1, r1);
	std::vector<float> mid(l1.size());
	std::vector<float> side(l1.size());
	for (size_t i = 0; i < l1.size(); ++i) {
		mid[i] = 0.5f * (l1[i] + r1[i]);
		side[i] = 0.5f * (l1[i] - r1[i]);
	}
	const double fundamental = toneAt(mid, 1000.0);
	MESSAGE("mid 1 kHz " << fundamental << ", mid 3 kHz " << dbOf(toneAt(mid, 3000.0) / fundamental) << " dB, 5 kHz "
	                     << dbOf(toneAt(mid, 5000.0) / fundamental) << " dB, side 3 kHz vs left "
	                     << dbOf(toneAt(side, 3000.0) / toneAt(l1, 3000.0)) << " dB, side 2 kHz "
	                     << dbOf(toneAt(side, 2000.0) / fundamental) << " dB");
	CHECK(std::abs(dbOf(fundamental / 0.5)) < 0.05);
	CHECK(dbOf(toneAt(mid, 3000.0) / fundamental) < -80.0);
	CHECK(dbOf(toneAt(mid, 5000.0) / fundamental) < -80.0);
	CHECK(std::abs(dbOf(toneAt(side, 3000.0) / toneAt(l1, 3000.0))) < 0.1);
	CHECK(dbOf(toneAt(side, 2000.0) / fundamental) < -80.0);

	std::vector<float> lh;
	std::vector<float> rh;
	run(DriveMode::Chonk, 0.5f, lh, rh);
	CHECK(dbOf(toneAt(rh, 3000.0) / toneAt(rh, 1000.0)) < -80.0);
}

namespace {

/// A CHONK STAGE at an exact Drive gain and Shape (the knob's mapping is not what is under test).
[[nodiscard]] DriveSettings chonkAt(double gain, float shape) {
	DriveSettings d = deriveDrive(0.8f, DriveMode::Chonk, shape, kRate, nullptr);
	d.gain = static_cast<float>(gain);
	d.norm = static_cast<float>(driveMakeUp(DriveMode::Chonk, gain, shape));
	return d;
}

/// BOTH CHANNELS of a 1 kHz sine of `amp` through the stage, the same signal in each, after the filters have settled.
/// 4800 frames is exactly 100 cycles, so a single-bin sum over them is exact.
void paunchRun(const DriveSettings& d, double amp, std::vector<float>& left, std::vector<float>& right,
               bool noise = false) {
	DriveStage stage;
	stage.prepare(kRate);
	stage.set(d);
	constexpr size_t kSettle = 48000;
	constexpr size_t kFrames = 4800;
	uint32_t seed = 12345u;
	for (size_t i = 0; i < kSettle + kFrames; ++i) {
		double v = amp * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate);
		if (noise) {
			seed = seed * 1664525u + 1013904223u;
			v = amp * (static_cast<double>(seed >> 8) / 8388608.0 - 1.0);
		}
		float l = static_cast<float>(v);
		float r = l;
		stage.process(l, r);
		if (i >= kSettle) {
			left.push_back(l);
			right.push_back(r);
		}
	}
}

/// THE COMPLEX AMPLITUDE of `hz` over a whole number of cycles, a direct sum.
[[nodiscard]] std::complex<double> binOf(const std::vector<float>& x, double hz) {
	std::complex<double> sum = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		const double w = -2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate;
		sum += static_cast<double>(x[i]) * std::complex<double>(std::cos(w), std::sin(w));
	}
	return sum * (2.0 / static_cast<double>(x.size()));
}

[[nodiscard]] double degreesApart(std::complex<double> a, std::complex<double> b) {
	return std::abs(std::arg(a * std::conj(b))) * 180.0 / std::numbers::pi;
}

} // namespace

TEST_CASE("Drive: Chonk's known behaviour, stated: at Shape 1 the mono sum is the raw input at every Drive gain, and the right thins and then flips") {
	// RESTATED: left tanh(g x) k, right yr + 2 (x - yr) = 2 x - yr, so (l + r) / 2 = x at any gain; the right's small-signal gain is
	// 2 - g k (k the make-up), +0.94, +0.24, -1.09, -3.90 at Drive gain 1, 4, 8, 16.
	const double wanted[] = { 0.94, 0.24, -1.09, -3.90 };
	size_t at = 0;
	for (const double gain : { 1.0, 4.0, 8.0, 16.0 }) {
		CAPTURE(gain);
		std::vector<float> l;
		std::vector<float> r;
		paunchRun(chonkAt(gain, 1.0f), 0.001, l, r); // -60 dBFS
		std::vector<float> mid(l.size());
		for (size_t i = 0; i < l.size(); ++i) {
			mid[i] = 0.5f * (l[i] + r[i]);
		}
		const double k = gain * driveMakeUp(DriveMode::Chonk, gain, 1.0f);
		const auto fr = binOf(r, 1000.0);
		const auto fl = binOf(l, 1000.0); // (the left has the saturator's own positive gain: the sign is read against it)
		const double rightGain = std::abs(fr) / 0.001 * ((fr * std::conj(fl)).real() < 0.0 ? -1.0 : 1.0);
		MESSAGE("gain " << gain << ": mid " << std::abs(binOf(mid, 1000.0)) / 0.001 << " of the input, right " << rightGain
		                << " (restated " << 2.0 - k << ")");
		CHECK(std::abs(std::abs(binOf(mid, 1000.0)) - 0.001) < 0.01 * 0.001);
		CHECK(std::abs(rightGain - (2.0 - k)) < 0.02 * std::max(1.0, std::abs(2.0 - k)));
		CHECK(std::abs(rightGain - wanted[at]) < 0.1);
		++at;
	}
}

TEST_CASE("Drive: Lean's two channels have the same small-signal gain and polarity, and the same bound") {
	// RESTATED from the curve: left f(u), right -f(-u); both have slope 1 at 0 and range [-1/(1-t), 1/(1+t)] mirrored.
	for (const float shape : { 0.5f, 1.0f }) {
		for (const double gain : { 1.0, 4.0, 16.0 }) {
			CAPTURE(shape);
			CAPTURE(gain);
			DriveSettings d = deriveDrive(0.8f, DriveMode::Lean, shape, kRate, nullptr);
			d.gain = static_cast<float>(gain);
			d.norm = static_cast<float>(driveMakeUp(DriveMode::Lean, gain, shape));
			std::vector<float> l;
			std::vector<float> r;
			paunchRun(d, 0.001, l, r);
			const auto fl = binOf(l, 1000.0);
			const auto fr = binOf(r, 1000.0);
			float peakL = 0.0f;
			float peakR = 0.0f;
			for (const double amp : { 1.0, 4.0 }) {
				std::vector<float> pl;
				std::vector<float> pr;
				paunchRun(d, amp, pl, pr);
				for (size_t i = 0; i < pl.size(); ++i) {
					peakL = std::max(peakL, std::abs(pl[i]));
					peakR = std::max(peakR, std::abs(pr[i]));
				}
			}
			MESSAGE("Lean shape " << shape << " gain " << gain << ": small-signal L " << std::abs(fl) / 0.001 << " R "
			                      << std::abs(fr) / 0.001 << ", peak L " << peakL << " R " << peakR);
			CHECK(std::abs(dbOf(std::abs(fl) / std::abs(fr))) < 0.1);
			CHECK(degreesApart(fl, fr) < 5.0);
			CHECK(std::abs(peakL - peakR) < 0.1f * std::max(peakL, peakR));
		}
	}
}

TEST_CASE("Drive: what the curve adds above the band is filtered off, not folded back into it") {
	// A 5 kHz SINE, HARD: its 7th and 9th harmonics (35 and 45 kHz) would fold to 13 and 3 kHz at 48 kHz.
	Settings s = dustStill();
	s.drive = 1.0f;
	s.driveMode = DriveMode::Soft;
	const std::vector<float> out = stillSine(s, 5000.0, 0.5, 4800, 4800);
	const double fundamental = toneAt(out, 5000.0);
	REQUIRE(dbOf(toneAt(out, 15000.0) / fundamental) > -20.0); // it IS distorting
	CHECK(dbOf(toneAt(out, 13000.0) / fundamental) < -60.0);
	CHECK(dbOf(toneAt(out, 3000.0) / fundamental) < -60.0);
}

TEST_CASE("Drive: Crush holds each sample for a time the knob sets and rounds it to a step Shape sets") {
	Settings s = dustStill();
	s.driveMode = DriveMode::Crush;
	s.drive = 1.0f;
	s.driveShape = 1.0f;
	const DustDerived d = deriveDust(s, 60.0, kRate);
	// THE HOLD: one frame at the bottom, 1 / 500 s more than nothing at the top.
	CHECK(std::abs(d.crushHoldFrames - (1.0 + kRate * (std::exp2(6.3) - 1.0) / 40000.0)) < 1.0e-9);
	CHECK(std::abs(d.crushStep - std::exp2(1.0 - 1.5)) < 1.0e-7); // a bit and a half across -1..1: 2^(1 - bits)
	s.driveShape = 0.0f;
	CHECK(std::abs(deriveDust(s, 60.0, kRate).crushStep - std::exp2(1.0 - 10.0)) < 1.0e-15);
	s.driveShape = 1.0f;

	const std::vector<float> out = stillSine(s, 220.0, 0.9, 48000, 480);
	size_t changes = 0;
	for (size_t i = 1; i < out.size(); ++i) {
		changes += out[i] != out[i - 1] ? 1u : 0u;
		const double steps = static_cast<double>(out[i]) / std::exp2(1.0 - 1.5);
		REQUIRE(std::abs(steps - std::round(steps)) < 1.0e-5); // every sample is on the grid, the restated step's
	}
	// AT MOST ONE NEW VALUE A HOLD, and a 220 Hz sine across three-level steps changes at most of them.
	const double holds = 48000.0 / d.crushHoldFrames;
	MESSAGE("Crush changes " << changes << " of " << holds << " holds");
	CHECK(static_cast<double>(changes) <= holds + 1.0);
	CHECK(static_cast<double>(changes) > 0.3 * holds);
	// AT THE BOTTOM OF SHAPE THE STEP IS 2^-9, whatever the hold.
	s.driveShape = 0.0f;
	const std::vector<float> fine = stillSine(s, 220.0, 0.9, 48000, 480);
	for (const float v : fine) {
		const double steps = static_cast<double>(v) / std::exp2(-9.0);
		REQUIRE(std::abs(steps - std::round(steps)) < 1.0e-3);
	}
}

namespace {
/// A STAGE RUN FRAME BY FRAME on the same signal both sides; the left comes back.
std::vector<float> stageOf(const DriveSettings& settings, const std::vector<float>& in) {
	DriveStage stage;
	stage.prepare(kRate);
	stage.set(settings);
	std::vector<float> out(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		float l = in[i];
		float r = in[i];
		stage.process(l, r);
		out[i] = l;
	}
	return out;
}

/// A SIGNAL THAT MOVES FROM FRAME TO FRAME, so a held value is told from its neighbours.
std::vector<float> wanderOf(size_t frames) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		v[i] = static_cast<float>(0.6 * std::sin(0.37 * static_cast<double>(i)) + 0.3 * std::sin(1.9 * static_cast<double>(i) + 1.0));
	}
	return v;
}

DriveSettings plainSettings(DriveMode mode, float shape, double hold) {
	DriveSettings d;
	d.driven = true;
	d.mode = mode;
	d.shape = shape;
	d.crushHoldFrames = hold;
	return d;
}
} // namespace

TEST_CASE("Drive: Decimate holds each sample as Crush does, as steps at the bottom of Shape and as a lagging line at the top") {
	// THE KNOB SETS THE HOLD BY CRUSH'S LAW, and rounds nothing.
	for (const float knob : { 0.1f, 0.5f, 1.0f }) {
		const double want = 1.0 + kRate * (std::exp2(6.3 * static_cast<double>(knob)) - 1.0) / 40000.0;
		CHECK(std::abs(deriveDrive(knob, DriveMode::Decimate, 0.5f, kRate).crushHoldFrames - want) < 1.0e-9);
		CHECK(deriveDrive(knob, DriveMode::Decimate, 0.5f, kRate).norm == 1.0f);
	}
	const size_t hold = 8;
	const std::vector<float> in = wanderOf(200);
	// THE FRAME A HOLD STARTS ON, restated: the first of them is the eighth frame, and so on every eighth.
	const auto startOf = [&](size_t i) { return static_cast<std::ptrdiff_t>((i + 1) / hold * hold) - 1; };
	const std::vector<float> steps = stageOf(plainSettings(DriveMode::Decimate, 0.0f, static_cast<double>(hold)), in);
	for (size_t i = 0; i < in.size(); ++i) {
		const std::ptrdiff_t c = startOf(i);
		CAPTURE(i);
		CHECK(steps[i] == (c < 0 ? 0.0f : in[static_cast<size_t>(c)]));
	}
	const std::vector<float> lines = stageOf(plainSettings(DriveMode::Decimate, 1.0f, static_cast<double>(hold)), in);
	int constantRuns = 0;
	for (size_t i = 0; i < in.size(); ++i) {
		const std::ptrdiff_t c = startOf(i);
		if (c < 0) {
			CHECK(lines[i] == 0.0f);
			continue;
		}
		const double held = in[static_cast<size_t>(c)];
		const double previous = c >= static_cast<std::ptrdiff_t>(hold) ? in[static_cast<size_t>(c) - hold] : 0.0;
		CAPTURE(i);
		// THE LINE: from the previous held value to the current one across the hold, so one hold behind.
		CHECK(std::abs(static_cast<double>(lines[i]) - (previous + (held - previous) * static_cast<double>(i - static_cast<size_t>(c)) / static_cast<double>(hold))) < 1.0e-6);
		if (i + 1 < in.size() && startOf(i + 1) == c) {
			CHECK(std::abs(static_cast<double>(lines[i + 1] - lines[i]) - (held - previous) / static_cast<double>(hold)) < 1.0e-6);
			++constantRuns;
		}
	}
	CHECK(constantRuns > 100);
	// BETWEEN IS THE LINEAR BLEND OF THE TWO.
	const std::vector<float> half = stageOf(plainSettings(DriveMode::Decimate, 0.5f, static_cast<double>(hold)), in);
	for (size_t i = 0; i < in.size(); ++i) {
		CHECK(std::abs(static_cast<double>(half[i]) - 0.5 * (static_cast<double>(steps[i]) + static_cast<double>(lines[i]))) < 1.0e-6);
	}
	// A HOLD OF ONE FRAME AT SHAPE 0 IS THE INPUT, to the bit; at Shape 1 it is the input a frame late.
	CHECK(stageOf(plainSettings(DriveMode::Decimate, 0.0f, 1.0), in) == in);
	const std::vector<float> late = stageOf(plainSettings(DriveMode::Decimate, 1.0f, 1.0), in);
	CHECK(late[0] == 0.0f);
	for (size_t i = 1; i < in.size(); ++i) {
		REQUIRE(late[i] == in[i - 1]);
	}
}

TEST_CASE("Drive: Slew and Lopside run the stage's Slew at the rate the knob sets, and drag by the balance Shape sets") {
	const auto hzOf = [](double knob) { return 20000.0 * std::pow(50.0 / 20000.0, knob); };
	for (const DriveMode mode : { DriveMode::Slew, DriveMode::Lopside }) {
		for (const float knob : { 0.25f, 0.5f, 1.0f }) {
			DriveStage stage;
			stage.prepare(kRate);
			stage.set(deriveDrive(knob, mode, 0.3f, kRate));
			CAPTURE(static_cast<int>(mode));
			CAPTURE(knob);
			CHECK(stage.slewForTest().on());
			// THE HARD LIMIT IS 4 * 0.5 * hz / rate A FRAME, hz the restated one.
			CHECK(std::abs(static_cast<double>(stage.slewForTest().limit()) - 2.0 * hzOf(static_cast<double>(knob)) / kRate) < 1.0e-7);
		}
	}
	// THE KNOB'S BOTTOM IS HELD UNDER 0.45 OF THE RATE.
	CHECK(deriveDrive(1.0e-6f, DriveMode::Slew, 0.0f, 24000.0).slewHz <= 0.45 * 24000.0);
	CHECK(deriveDrive(1.0f, DriveMode::Slew, 0.0f, kRate).slewHz == 50.0f);
	CHECK(deriveDrive(0.5f, DriveMode::Slew, 0.7f, kRate).slewCurve == 0.7f);
	CHECK(deriveDrive(0.5f, DriveMode::Slew, 0.7f, kRate).slewBalance == 0.0f);
	CHECK(deriveDrive(0.5f, DriveMode::Lopside, 0.7f, kRate).slewCurve == 0.0f);
	CHECK(deriveDrive(0.5f, DriveMode::Lopside, 0.0f, kRate).slewBalance == -1.0f);
	CHECK(deriveDrive(0.5f, DriveMode::Lopside, 1.0f, kRate).slewBalance == 1.0f);
	CHECK(deriveDrive(0.5f, DriveMode::Lopside, 0.5f, kRate).slewBalance == 0.0f);

	// A FAST 0.5-PEAK SINE (1 kHz at the bottom of the rate: its slope is thirty times the limit's) COMES OUT A
	// TRIANGLE, every step the limit but where it turns.
	const size_t frames = 48000;
	std::vector<float> sine(frames);
	for (size_t i = 0; i < frames; ++i) {
		sine[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
	}
	const double limit = 2.0 * hzOf(1.0) / kRate;
	const std::vector<float> tri = stageOf(deriveDrive(1.0f, DriveMode::Slew, 0.0f, kRate), sine);
	int atLimit = 0;
	int total = 0;
	for (size_t i = 24000; i < frames; ++i) {
		const double step = std::abs(static_cast<double>(tri[i] - tri[i - 1]));
		CHECK(step <= limit * (1.0 + 1.0e-4));
		atLimit += std::abs(step - limit) < limit * 1.0e-3 ? 1 : 0;
		++total;
	}
	MESSAGE("Slew: " << atLimit << " of " << total << " steps at the limit " << limit);
	CHECK(atLimit > total * 9 / 10);
	// LOPSIDE: RISING STEPS 2^balance THE LIMIT, FALLING 2^-balance; the two ends are reciprocals, the middle alike.
	double rise[3] = {};
	double fall[3] = {};
	int c = 0;
	for (const float shape : { 0.0f, 1.0f, 0.5f }) {
		const std::vector<float> out = stageOf(deriveDrive(1.0f, DriveMode::Lopside, shape, kRate), sine);
		for (size_t i = 24000; i < frames; ++i) {
			const double step = static_cast<double>(out[i] - out[i - 1]);
			rise[c] = std::max(rise[c], step);
			fall[c] = std::max(fall[c], -step);
		}
		const double balance = 2.0 * static_cast<double>(shape) - 1.0;
		CAPTURE(shape);
		CHECK(std::abs(rise[c] - limit * std::exp2(balance)) < limit * 1.0e-3);
		CHECK(std::abs(fall[c] - limit * std::exp2(-balance)) < limit * 1.0e-3);
		++c;
	}
	CHECK(std::abs(rise[0] / fall[0] * (rise[1] / fall[1]) - 1.0) < 1.0e-3);
	CHECK(std::abs(rise[0] / fall[0] - 0.25) < 1.0e-3);
	CHECK(std::abs(rise[2] / fall[2] - 1.0) < 1.0e-3);
}

TEST_CASE("Drive: the stage's own Mix — 0 is the bits that went in, 1 is the stage as it was, between is the restated blend") {
	const std::vector<float> in = wanderOf(4000);
	for (const DriveMode mode : kAllDrives) {
		CAPTURE(static_cast<int>(mode));
		const DriveSettings off = deriveDrive(0.8f, mode, 0.7f, kRate, nullptr, 0.0f);
		CHECK(off.mix == 0.0f);
		const std::vector<float> out = stageOf(off, in);
		CHECK(out == in);
		// DEFAULT MIX IS 1.
		CHECK(deriveDrive(0.8f, mode, 0.7f, kRate).mix == 1.0f);
	}
	// AT 1 THE STAGE IS WHAT IT WAS: Soft's frame, restated through the oversampler by hand, to the bit.
	{
		const DriveSettings d = deriveDrive(0.8f, DriveMode::Soft, 0.7f, kRate, nullptr, 1.0f);
		const std::vector<float> got = stageOf(d, in);
		fx::dyn::Oversampler4x over;
		over.reset();
		std::vector<float> want(in.size());
		for (size_t i = 0; i < in.size(); ++i) {
			float l = in[i];
			float r = in[i];
			const float gain = d.gain;
			const float shape = d.shape;
			over.process(&l, &r, 1, [gain, shape](float& a, float& b) {
				a = shapeDrive(DriveMode::Soft, a * gain, shape, gain);
				b = shapeDriveRight(DriveMode::Soft, b * gain, shape, gain);
			});
			want[i] = l * d.norm;
		}
		const bool same = got == want;
		CHECK(same);
	}
	// BETWEEN IS dry + mix (processed - dry), one oversampled Type and one not.
	for (const DriveMode mode : { DriveMode::Fold, DriveMode::Decimate }) {
		CAPTURE(static_cast<int>(mode));
		const std::vector<float> processed = stageOf(deriveDrive(0.8f, mode, 0.7f, kRate), in);
		const std::vector<float> half = stageOf(deriveDrive(0.8f, mode, 0.7f, kRate, nullptr, 0.5f), in);
		double worst = 0.0;
		double moved = 0.0;
		for (size_t i = 0; i < in.size(); ++i) {
			const double want = static_cast<double>(in[i]) + 0.5 * (static_cast<double>(processed[i]) - static_cast<double>(in[i]));
			worst = std::max(worst, std::abs(static_cast<double>(half[i]) - want));
			moved = std::max(moved, std::abs(static_cast<double>(processed[i]) - static_cast<double>(in[i])));
		}
		MESSAGE("worst difference " << worst);
		CHECK(worst < 1.0e-6);
		CHECK(moved > 0.1); // the blend is not between two equal things
	}
}

TEST_CASE("Tap: Clean feeds back what the head read, Driven what the shaper made — for every Type — and neither divides the loop") {
	for (const DriveMode type : kAllDrives) {
		CAPTURE(static_cast<int>(type));
		Settings s = declared();
		s.resonance = 1.0f;
		s.driveMode = type;
		s.driveShape = 1.0f;
		s.drive = 1.0f;
		// THE TAP DECIDES WHAT IS FED BACK, NOT HOW MUCH: Clean and Driven both carry the line's own feedback as set, and at no
		// Drive too. (Nothing is divided out.)
		CHECK(deriveDust(s, 60.0, kRate).feedback == 0.9);
		s.tapDriven = true;
		CHECK(deriveDust(s, 60.0, kRate).feedback == 0.9);
		s.drive = 0.0f;
		CHECK(deriveDust(s, 60.0, kRate).feedback == 0.9);
	}

	// AND THE TAP IS ON THE FAR SIDE OF THE SHAPER WHEN DRIVEN, the near side when Clean: frame by frame, what is fed back
	// is what the shaper made or what the head read, flushed.
	for (const DriveMode mode : { DriveMode::Wreck, DriveMode::Jag, DriveMode::Fold, DriveMode::Soft, DriveMode::Crush,
	                              DriveMode::Slew }) {
		for (const bool driven : { false, true }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(driven);
			Eroder e;
			e.prepare(kRate);
			Settings t = dustStill();
			t.resonance = 0.5f;
			t.drive = 0.6f;
			t.driveMode = mode;
			t.tapDriven = driven;
			e.setTarget(t);
			int tapped = 0;
			int differ = 0;
			int fedBack = 0;
			int counted = 0;
			float previous = 0.0f;
			for (int i = 0; i < 2000; ++i) {
				float l = 0.4f * static_cast<float>(std::sin(0.13 * i));
				float r = l;
				e.processStereo(&l, &r, 1);
				const Dust& d = e.dustForTest();
				tapped += d.lastTapForTest() == (driven ? d.lastShapedForTest() : d.lastReadForTest()) ? 1 : 0;
				differ += d.lastShapedForTest() != d.lastReadForTest() ? 1 : 0;
				// THE NEXT FRAME'S SUM SAW THIS FRAME'S TAP (flushed), where the inner limiter was not acting (its gain is exactly 1).
				if (i > 0 && std::abs(previous) <= 0.99f && d.sharedForTest().innerLimiterForTest().gain() == 1.0f) {
					++counted;
					fedBack += d.lastFedBackForTest() == (std::abs(previous) < 1.0e-9f ? 0.0f : previous) ? 1 : 0;
				}
				previous = d.lastTapForTest();
			}
			REQUIRE(differ > 1000); // the shaper is doing something, so the two sides are told apart
			CHECK(tapped == 2000);
			REQUIRE(counted > 500);
			CHECK(fedBack == counted);
		}
	}
}

TEST_CASE("Drive, HP Wet and Follow at work: bounded, the same at any partition, and the tail ends in exact silence") {
	for (const DriveMode mode : kAllDrives) {
		CAPTURE(static_cast<int>(mode));
		Settings s = declared();
		s.resonance = -1.0f;
		s.drive = 1.0f;
		s.driveMode = mode;
		s.driveShape = 1.0f;
		s.hpWet = 9.0f;
		s.subThru = true;
		s.follow = 0.7f;
		const std::vector<float> in = whiteInput(24000, 0.9f);
		const auto render = [&](int block) {
			Eroder e;
			e.prepare(kRate);
			e.setTarget(s);
			std::vector<float> l = in;
			std::vector<float> r = in;
			l.resize(in.size() + 96000, 0.0f);
			r.resize(in.size() + 96000, 0.0f);
			for (size_t done = 0; done < l.size();) {
				const size_t take = std::min<size_t>(l.size() - done, static_cast<size_t>(block));
				e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
				done += take;
			}
			return l;
		};
		const std::vector<float> whole = render(4096);
		const bool sameByOnes = render(1) == whole;
		const bool sameByOdd = render(37) == whole;
		CHECK(sameByOnes);
		CHECK(sameByOdd);
		float peak = 0.0f;
		for (const float v : whole) {
			REQUIRE(std::isfinite(v));
			peak = std::max(peak, std::abs(v));
		}
		CHECK(peak < 8.0f);
		CHECK(peak > 0.05f);
		// THE LAST HALF SECOND IS NOTHING AT ALL.
		bool silent = true;
		for (size_t i = whole.size() - 24000; i < whole.size(); ++i) {
			silent = silent && whole[i] == 0.0f;
		}
		CHECK(silent);
	}
}

TEST_CASE("HP Wet: no filter at the bottom; above, the shaken signal loses its low end by the filter's own curve; Sub Thru gives it back from the dry") {
	Settings s = dustStill();
	const std::vector<float> clean = stillSine(s, 100.0, 0.5, 4800, 9600);
	// AT THE BOTTOM OF THE RANGE THE FILTER IS NOT RUN: Sub Thru or not, the bits are the unfiltered ones.
	s.subThru = true;
	const bool untouched = stillSine(s, 100.0, 0.5, 4800, 9600) == clean;
	CHECK(untouched);
	CHECK_FALSE(deriveDust(s, 60.0, kRate).hpOn);

	s.subThru = false;
	s.hpWet = static_cast<float>(std::log2(1000.0));
	REQUIRE(deriveDust(s, 60.0, kRate).hpOn);
	CHECK(std::abs(deriveDust(s, 60.0, kRate).hpHz - 1000.0) < 1.0e-2);
	// THE CURVE: a two-pole highpass, Q 0.707, bilinear — restated at the warped frequency.
	const auto curve = [](double hz, double corner) {
		const double w = std::tan(std::numbers::pi * hz / kRate) / std::tan(std::numbers::pi * corner / kRate);
		return w * w / std::sqrt((1.0 - w * w) * (1.0 - w * w) + 2.0 * w * w);
	};
	for (const double hz : { 100.0, 500.0, 1000.0, 5000.0 }) {
		CAPTURE(hz);
		const std::vector<float> out = stillSine(s, hz, 0.5, 4800, 9600);
		CHECK(std::abs(dbOf(toneAt(out, hz) / 0.5) - dbOf(curve(hz, 1000.0))) < 0.1);
	}
	// SUB THRU: the highpassed copy plus what the same filter took from the dry is the signal again (at rest the two
	// are two frames apart, which at 100 Hz is nothing).
	s.subThru = true;
	const std::vector<float> whole = stillSine(s, 100.0, 0.5, 4800, 9600);
	CHECK(std::abs(dbOf(toneAt(whole, 100.0) / 0.5)) < 0.1);
}

TEST_CASE("Follow: the input's peak, fast up and slow down, scales the shake and leaves the centre") {
	// THE LAW.
	CHECK(followGain(0.0f, 0.0f) == 1.0f);
	CHECK(followGain(0.0f, 0.9f) == 1.0f);
	CHECK(followGain(1.0f, 0.0f) == 0.0f);
	CHECK(followGain(1.0f, 0.25f) == 0.5f);
	CHECK(followGain(1.0f, 0.5f) == 1.0f);
	CHECK(followGain(1.0f, 2.0f) == 1.0f);
	CHECK(followGain(0.5f, 0.25f) == 0.75f);

	// THE FOLLOWER: a steady sine's peak (it sags a little between crests), and nothing after silence.
	Eroder e;
	e.prepare(kRate);
	Settings s = declared();
	s.source = 0.0f; // a sine shakes the head, so its travel is exact
	s.freq = static_cast<float>(std::log2(997.0)); // and one whose crests the block ends do not keep missing
	s.follow = 1.0f;
	e.setTarget(s);
	const auto run = [&](double peak, size_t frames) {
		std::vector<float> l(frames);
		for (size_t i = 0; i < frames; ++i) {
			l[i] = static_cast<float>(peak * std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / kRate));
		}
		std::vector<float> r = l;
		float nearest = 1.0e9f;
		float farthest = 0.0f;
		for (size_t done = 0; done < frames; done += 32) {
			e.processStereo(l.data() + done, r.data() + done, 32);
			if (done > frames / 2) {
				const auto at = static_cast<float>(e.dustForTest().lastDelayFramesForTest(0));
				nearest = std::min(nearest, at);
				farthest = std::max(farthest, at);
			}
		}
		return std::pair { nearest, farthest };
	};
	const auto [nearQuiet, farQuiet] = run(0.1, 48000);
	CHECK(e.envelopeForTest() > 0.09f);
	CHECK(e.envelopeForTest() <= 0.1f);
	CHECK(std::abs(e.dustForTest().followGainForTest() - e.envelopeForTest() / 0.5f) < 0.01f);
	const auto [nearLoud, farLoud] = run(0.6, 48000);
	CHECK(std::abs(e.dustForTest().followGainForTest() - 1.0f) < 1.0e-4f);
	// THE TRAVEL: at a fifth of the level it takes, a fifth of the way; the middle of it does not move.
	const float amount = e.dustForTest().smoothedAmountFramesForTest();
	CHECK(std::abs((farLoud - nearLoud) - 2.0f * amount) < 0.05f * amount);
	CHECK(std::abs((farQuiet - nearQuiet) / (farLoud - nearLoud) - 0.2f) < 0.03f);
	CHECK(std::abs(0.5f * (farQuiet + nearQuiet) - 0.5f * (farLoud + nearLoud)) < 0.02f * amount);
	// SILENCE ENDS IT, exactly.
	std::vector<float> zl(240000, 0.0f);
	std::vector<float> zr(240000, 0.0f);
	e.processStereo(zl.data(), zr.data(), 240000);
	CHECK(e.envelopeForTest() == 0.0f);
	CHECK(e.dustForTest().followGainForTest() == 0.0f);
}

TEST_CASE("Follow: the Comb's depth rides it too, and at none of it the Comb is bit for bit what it was") {
	const std::vector<float> in = whiteInput(12000, 0.05f);
	const auto render = [&](float follow, float scale) {
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.follow = follow;
		e.setTarget(s);
		std::vector<float> l = in;
		for (float& v : l) {
			v *= scale;
		}
		std::vector<float> r = l;
		e.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		return std::pair { l, e.combFollowForTest() };
	};
	const auto [plain, plainGain] = render(0.0f, 1.0f);
	CHECK(plainGain == 1.0f);
	// A QUIET INPUT AT FULL FOLLOW: the depth is the follower's reach of half scale.
	const auto [followed, gain] = render(1.0f, 1.0f);
	CHECK(gain > 0.0f);
	CHECK(gain < 0.12f);
	const bool moved = followed != plain;
	CHECK(moved);
	// A LOUD ONE: whole.
	CHECK(render(1.0f, 15.0f).second == 1.0f);
}

TEST_CASE("Dust: a move of Amount arrives by a TIME, whatever the block length") {
	for (const int block : { 1, 64, 480 }) {
		Eroder e;
		e.prepare(kRate);
		Settings s = declared();
		s.amount = secondsToLog2(100.0e-6);
		e.setTarget(s);
		std::vector<float> l(4800, 0.0f);
		std::vector<float> r(4800, 0.0f);
		e.processStereo(l.data(), r.data(), 480);
		// THE FIRST BLOCK SNAPPED.
		CHECK(std::abs(static_cast<double>(e.dustForTest().smoothedAmountFramesForTest()) - 100.0e-6 * kRate) < 1.0e-3);
		s.amount = secondsToLog2(1000.0e-6);
		e.setTarget(s);
		// ONE TIME CONSTANT — 20 ms, 960 frames — LATER it is 1 - 1/e of the way, less the control block it waited for.
		for (int done = 0; done < 960; done += block) {
			e.processStereo(l.data(), r.data(), std::min(block, 960 - done));
		}
		const double from = 100.0e-6 * kRate;
		const double to = 1000.0e-6 * kRate;
		const double at = static_cast<double>(e.dustForTest().smoothedAmountFramesForTest());
		const double fraction = (at - from) / (to - from);
		CAPTURE(block);
		CHECK(fraction > 0.60);
		CHECK(fraction < 0.64);
	}
}

TEST_CASE("Dust: a partition does not change the samples while every control is travelling; reset is as-if-just-switched-on") {
	const size_t frames = 6000;
	const std::vector<float> inL = whiteInput(frames, 0.4f, 11u);
	const std::vector<float> inR = whiteInput(frames, 0.4f, 29u);
	const auto render = [&](const std::vector<int>& cuts, Eroder& e, std::vector<float>& l, std::vector<float>& r) {
		l = inL;
		r = inR;
		Settings s = declared();
		s.resonance = 0.5f;
		s.grit = 0.3f;
		s.source = 0.6f;
		s.drive = 0.4f;
		s.mix = 0.7f;
		size_t done = 0;
		size_t cut = 0;
		while (done < frames) {
			// EVERY CONTROL MOVES, as a function of where the passage is and not of how it was cut.
			const double t = static_cast<double>(done / 512) / 12.0;
			s.amount = secondsToLog2(50.0e-6 * std::exp2(4.0 * t));
			s.freq = hertzToLog2(500.0 * std::exp2(4.0 * t));
			s.width = static_cast<float>(t);
			s.stereo = static_cast<float>(1.0 - t);
			e.setTarget(s);
			const size_t toBoundary = 512 - (done % 512);
			const size_t take = std::min({ static_cast<size_t>(cuts[cut % cuts.size()]), toBoundary, frames - done });
			e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
			done += take;
			++cut;
		}
	};
	Eroder a;
	a.prepare(kRate);
	std::vector<float> al;
	std::vector<float> ar;
	render({ 512 }, a, al, ar);
	Eroder b;
	b.prepare(kRate);
	std::vector<float> bl;
	std::vector<float> br;
	render({ 1, 7, 33, 64, 3, 129 }, b, bl, br);
	CHECK(al == bl);
	CHECK(ar == br);
	// NOT VACUOUS: it is doing something to the signal.
	CHECK(al != inL);

	a.reset();
	std::vector<float> cl;
	std::vector<float> cr;
	render({ 512 }, a, cl, cr);
	CHECK(cl == al);
	CHECK(cr == ar);
}

TEST_CASE("Dust: a change of engine starts the new one as if just switched on") {
	const size_t frames = 3000;
	const std::vector<float> in = whiteInput(frames, 0.4f);
	Eroder fresh;
	fresh.prepare(kRate);
	fresh.setTarget(declared());
	std::vector<float> fl = in;
	std::vector<float> fr = in;
	fresh.processStereo(fl.data(), fr.data(), static_cast<int>(frames));

	// DUST, THEN THE COMB, THEN DUST AGAIN: what Dust's line and noise stream were left holding must not come back.
	Eroder e;
	e.prepare(kRate);
	e.setTarget(declared());
	std::vector<float> l = in;
	std::vector<float> r = in;
	e.processStereo(l.data(), r.data(), 777);
	e.setTarget(defaults()); // the Comb
	l = in;
	r = in;
	e.processStereo(l.data(), r.data(), 64);
	e.setTarget(declared());
	l = in;
	r = in;
	e.processStereo(l.data(), r.data(), static_cast<int>(frames));
	CHECK(l == fl);
	CHECK(r == fr);
}

TEST_CASE("Dust: the control path runs on its own 32-frame grid, however the host cuts the blocks") {
	Eroder e;
	e.prepare(kRate);
	Settings s = declared();
	s.amount = secondsToLog2(100.0e-6);
	e.setTarget(s);
	float l = 0.0f;
	float r = 0.0f;
	for (int i = 0; i < 10; ++i) {
		e.processStereo(&l, &r, 1);
	}
	// A MOVE THAT ARRIVES TEN FRAMES INTO A CONTROL BLOCK is taken up at the next one, frame 32, and not before:
	// the mapping is solved once a block whatever the host's block length.
	s.amount = secondsToLog2(400.0e-6);
	e.setTarget(s);
	int tookEffectAt = -1;
	for (int i = 10; i < 80 && tookEffectAt < 0; ++i) {
		e.processStereo(&l, &r, 1);
		if (std::abs(e.dustForTest().derivedForTest().amountSeconds - 400.0e-6) < 1.0e-9) {
			tookEffectAt = i;
		}
	}
	CHECK(tookEffectAt == 32);
}

TEST_CASE("Dust: a hot signal stays finite and bounded everywhere, and the tail ends in exact silence") {
	const std::vector<float> in = whiteInput(24000, 4.0f);
	for (const float resonance : { -1.0f, 0.0f, 1.0f }) {
		for (const double amount : { 4.0e-6, 200.0e-6, 1.9e-3 }) {
			for (const double hz : { 20.0, 4000.0, 18000.0 }) {
				for (const float source : { 0.0f, 1.0f }) {
					Eroder e;
					e.prepare(kRate);
					Settings s = declared();
					s.resonance = resonance;
					s.amount = secondsToLog2(amount);
					s.freq = hertzToLog2(hz);
					s.source = source;
					s.grit = 0.5f;
					s.drive = 0.0f;
					e.setTarget(s);
					std::vector<float> l = in;
					std::vector<float> r = in;
					e.processStereo(l.data(), r.data(), static_cast<int>(in.size()));
					float peak = 0.0f;
					bool finite = true;
					for (size_t i = 0; i < l.size(); ++i) {
						finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
						peak = std::max({ peak, std::abs(l[i]), std::abs(r[i]) });
					}
					CAPTURE(resonance);
					CAPTURE(amount);
					CAPTURE(hz);
					CAPTURE(source);
					CHECK(finite);
					// THE LINE'S OWN CEILING, with the interpolation's overshoot allowed for.
					CHECK(peak <= kDustCeiling * 1.3f);

					// THE INPUT STOPS. Within two seconds the output is exactly zero and the line holds nothing.
					std::vector<float> zl(96000, 0.0f);
					std::vector<float> zr(96000, 0.0f);
					e.processStereo(zl.data(), zr.data(), static_cast<int>(zl.size()));
					bool silent = true;
					for (size_t i = zl.size() - 4800; i < zl.size(); ++i) {
						silent = silent && zl[i] == 0.0f && zr[i] == 0.0f;
					}
					CHECK(silent);
					CHECK_FALSE(e.holdsTail());
				}
			}
		}
	}
}

TEST_CASE("Dust: the mono road is the LEFT channel") {
	const size_t frames = 2000;
	const std::vector<float> in = whiteInput(frames, 0.4f);
	Eroder stereo;
	stereo.prepare(kRate);
	stereo.setTarget(declared());
	std::vector<float> l = in;
	std::vector<float> r = in;
	stereo.processStereo(l.data(), r.data(), static_cast<int>(frames));
	Eroder mono;
	mono.prepare(kRate);
	mono.setTarget(declared());
	std::vector<float> m = in;
	mono.processMono(m.data(), static_cast<int>(frames));
	CHECK(m == l);
	CHECK(l != r);
}

//--------------------------------------------------------------------------------------------------//
// WHAT THE DEVICE PUBLISHES FOR ITS SURFACES.                                                        //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A sine at `hz`, amplitude `level`, both sides, through a prepared device in blocks of `block`; returns the readings
/// the device handed its sink on the last call.
struct Heard {
	std::array<float, kReadingCount> values {};
	int calls = 0;
	size_t count = 0;
};

Heard listen(Eroder& e, double rate, double hz, float level, int frames, int block = 256) {
	Heard heard;
	e.setReadingsSink({ &heard, [](void* context, const float* values, size_t count) {
		                   auto& h = *static_cast<Heard*>(context);
		                   h.count = count;
		                   ++h.calls;
		                   std::copy(values, values + std::min(count, h.values.size()), h.values.begin());
	                   } });
	std::vector<float> l(static_cast<size_t>(block));
	std::vector<float> r(static_cast<size_t>(block));
	for (int done = 0; done < frames; done += block) {
		for (int i = 0; i < block; ++i) {
			const double t = static_cast<double>(done + i) / rate;
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = level * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * t));
		}
		e.processStereo(l.data(), r.data(), block);
	}
	return heard;
}

size_t binOf(double hz) {
	size_t best = 0;
	for (size_t k = 1; k < feathers::dsp::kAnalyserBins; ++k) {
		if (std::abs(std::log(feathers::dsp::analyserBinHz(k) / hz)) < std::abs(std::log(feathers::dsp::analyserBinHz(best) / hz))) {
			best = k;
		}
	}
	return best;
}

float reading(const Heard& h, Reading r, size_t offset = 0) { return h.values[static_cast<size_t>(r) + offset]; }

} // namespace

TEST_CASE("Readings: a whole spectrum frame of what came in and what went out, the counter at both ends") {
	Eroder e;
	e.prepare(48000.0);
	Settings s = declared();
	s.mix = 0.0f; // Dust at Mix 0 is the dry, to the bit: what went out IS what came in.
	e.setTarget(s);
	const Heard h = listen(e, 48000.0, 1000.0, 0.5f, 48000);
	CHECK(h.count == kReadingCount);
	CHECK(h.calls == 48000 / 256 + 1);
	// A FRAME HAS BEEN FINISHED, and it is whole.
	CHECK(reading(h, Reading::FrameA) >= 1.0f);
	CHECK(reading(h, Reading::FrameA) == reading(h, Reading::FrameB));
	// THE SINE IS IN ITS OWN BIN, and far above the bins an octave either side.
	const size_t at = binOf(1000.0);
	size_t loudest = 0;
	for (size_t k = 1; k < feathers::dsp::kAnalyserBins; ++k) {
		if (reading(h, Reading::PreFirst, k) > reading(h, Reading::PreFirst, loudest)) {
			loudest = k;
		}
	}
	CHECK(loudest == at);
	CHECK(reading(h, Reading::PreFirst, at) > -30.0f);
	CHECK(reading(h, Reading::PreFirst, at) - reading(h, Reading::PreFirst, binOf(500.0)) > 40.0f);
	CHECK(reading(h, Reading::PreFirst, at) - reading(h, Reading::PreFirst, binOf(2000.0)) > 40.0f);
	// OUT IS IN, bin for bin, because the samples are.
	for (size_t k = 0; k < feathers::dsp::kAnalyserBins; ++k) {
		CAPTURE(k);
		CHECK(reading(h, Reading::PostFirst, k) == reading(h, Reading::PreFirst, k));
	}
	// AND WHAT THE SINK HEARD IS WHAT THE DEVICE HOLDS.
	CHECK(h.values == e.readingsForTest());
}

TEST_CASE("Readings: the shaking shows in what went out and not in what came in") {
	Eroder e;
	e.prepare(48000.0);
	Settings s = declared();
	s.amount = static_cast<float>(kDustAmountLog2Max);
	e.setTarget(s);
	const Heard h = listen(e, 48000.0, 1000.0, 0.5f, 48000);
	REQUIRE(reading(h, Reading::FrameA) == reading(h, Reading::FrameB));
	// A PURE SINE IN: nothing two octaves up. A SHAKEN ONE OUT: sidebands there, tens of dB over the input's floor.
	const size_t up = binOf(4000.0);
	CHECK(reading(h, Reading::PostFirst, up) - reading(h, Reading::PreFirst, up) > 30.0f);
}

TEST_CASE("Readings: where the read head has been, in seconds, inside the travel the Amount allows") {
	const double rate = 48000.0;
	const auto margin = static_cast<float>(kDustMarginFrames / rate);
	SUBCASE("no Amount: the head sits on its margin, both sides, the whole call") {
		Eroder e;
		e.prepare(rate);
		Settings s = declared();
		s.amount = static_cast<float>(kDustAmountLog2Min);
		e.setTarget(s);
		const Heard h = listen(e, rate, 300.0, 0.5f, 9600);
		for (const Reading r : { Reading::HeadNearL, Reading::HeadFarL, Reading::HeadNearR, Reading::HeadFarR }) {
			CHECK(reading(h, r) == doctest::Approx(margin).epsilon(1e-6));
		}
	}
	SUBCASE("with an Amount: near and far apart, and both inside margin .. margin + 2 x Amount") {
		Eroder e;
		e.prepare(rate);
		Settings s = declared();
		s.amount = -11.0f; // 488 microseconds
		e.setTarget(s);
		const Heard h = listen(e, rate, 300.0, 0.5f, 9600, 2048);
		const auto reach = static_cast<float>(2.0 * std::exp2(-11.0));
		for (const auto& [nearAt, farAt] : { std::pair { Reading::HeadNearL, Reading::HeadFarL },
		                                    std::pair { Reading::HeadNearR, Reading::HeadFarR } }) {
			CHECK(reading(h, nearAt) >= margin * 0.999f);
			CHECK(reading(h, farAt) <= (margin + reach) * 1.001f);
			// A 2048-frame call holds 64 control blocks of a 4 kHz band: the head has been over most of its travel.
			CHECK(reading(h, farAt) - reading(h, nearAt) > 0.4f * reach);
		}
	}
	SUBCASE("the Comb publishes its head too: between its shortest line and its longest") {
		Eroder e;
		e.prepare(rate);
		Settings s = defaults();
		e.setTarget(s);
		const Heard h = listen(e, rate, 300.0, 0.5f, 9600);
		CHECK(reading(h, Reading::HeadFarL) > 0.0f);
		CHECK(reading(h, Reading::HeadFarL) <= static_cast<float>(1.5 * kMaxBaseDelaySeconds + 4.0 / rate));
		CHECK(reading(h, Reading::HeadNearL) <= reading(h, Reading::HeadFarL));
	}
}

TEST_CASE("Readings: the note the bus last played is published as it was given") {
	Eroder e;
	e.prepare(48000.0);
	e.setTarget(declared());
	device::NoteContext note {};
	note.pitchSemitones = 67.5f;
	e.setNoteContext(note);
	const Heard h = listen(e, 48000.0, 300.0, 0.25f, 512);
	CHECK(reading(h, Reading::PitchSemitones) == 67.5f);
}

// THAT THE TAP ITSELF LEAVES THE SOUND ALONE is held by the cases that pin the sound to the bit (the Comb against its
// reference loop, Dust's head at rest): both run through it. This one holds the sink to the same.
TEST_CASE("Readings: a sink changes no sample, in either engine, and the tap does run") {
	const auto render = [](bool withSink, Mode mode) {
		Eroder e;
		e.prepare(48000.0);
		Settings s = mode == Mode::Dust ? declared() : defaults();
		e.setTarget(s);
		Heard heard;
		if (withSink) {
			e.setReadingsSink({ &heard, [](void*, const float*, size_t) {} });
		}
		std::vector<float> l(6000);
		std::vector<float> r(6000);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.4f * static_cast<float>(std::sin(0.031 * static_cast<double>(i)));
			r[i] = 0.4f * static_cast<float>(std::sin(0.047 * static_cast<double>(i)));
		}
		// AN AWKWARD PARTITION, so the tap's own grid falls across the host's cuts.
		for (size_t done = 0; done < l.size();) {
			const size_t take = std::min<size_t>(l.size() - done, 37 + done % 211);
			e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
			done += take;
		}
		l.insert(l.end(), r.begin(), r.end());
		return l;
	};
	for (const Mode mode : { Mode::Dust, Mode::Comb }) {
		CAPTURE(static_cast<int>(mode));
		CHECK(render(true, mode) == render(false, mode));
	}
	// AND THE TAP DID RUN: a second of audio is more than twenty hops, four transforms each.
	Eroder e;
	e.prepare(48000.0);
	e.setTarget(declared());
	(void)listen(e, 48000.0, 1000.0, 0.5f, 48000);
	CHECK(e.spectrumTransformsForTest() >= 80);
	CHECK(e.spectrumTransformsForTest() <= 100);
}

namespace {
/// A HOST'S LEASE: samples of audio the spectrum is still wanted for, spent by each block.
struct Lease {
	int64_t left = 0;
	int asks = 0;
	static bool wanted(void* context, int frames) {
		auto& self = *static_cast<Lease*>(context);
		++self.asks;
		if (self.left <= 0) {
			return false;
		}
		self.left -= frames;
		return true;
	}
	static void publish(void*, const float*, size_t) {}
	device::ReadingsSink sink() { return { this, &Lease::publish, &Lease::wanted }; }
};

std::vector<float> leased(Eroder& e, int frames) {
	std::vector<float> l(static_cast<size_t>(frames));
	std::vector<float> r(static_cast<size_t>(frames));
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = r[i] = 0.4f * static_cast<float>(std::sin(0.031 * static_cast<double>(i)));
	}
	for (int done = 0; done < frames; done += 256) {
		e.processStereo(l.data() + done, r.data() + done, std::min(256, frames - done));
	}
	return l;
}
} // namespace

TEST_CASE("Readings: no transform runs while no view has asked or nobody listens, and the sound is the same") {
	// NOBODY TO PUBLISH TO: no spectrum at all.
	Eroder bare;
	bare.prepare(48000.0);
	bare.setTarget(declared());
	const std::vector<float> heardBare = leased(bare, 48000);
	CHECK(bare.spectrumTransformsForTest() == 0);
	// A HOST THAT NEVER LEARNED THE QUESTION: always.
	Eroder always;
	always.prepare(48000.0);
	always.setTarget(declared());
	always.setReadingsSink({ nullptr, &Lease::publish });
	CHECK(leased(always, 48000) == heardBare);
	const int expected = always.spectrumTransformsForTest();
	REQUIRE(expected >= 80);
	// NO READER: asked once a block, not one transform, the same sound.
	Lease none;
	Eroder idle;
	idle.prepare(48000.0);
	idle.setTarget(declared());
	idle.setReadingsSink(none.sink());
	CHECK(leased(idle, 48000) == heardBare);
	CHECK(idle.spectrumTransformsForTest() == 0);
	CHECK(none.asks == 48000 / 256 + 1);
	// HALF A SECOND OF LEASE: about half the transforms, then none; and asking again starts it again.
	Lease half;
	half.left = 24000;
	Eroder part;
	part.prepare(48000.0);
	part.setTarget(declared());
	part.setReadingsSink(half.sink());
	CHECK(leased(part, 48000) == heardBare);
	const int ran = part.spectrumTransformsForTest();
	CHECK(ran > expected * 40 / 100);
	CHECK(ran < expected * 60 / 100);
	half.left = 24000;
	(void)leased(part, 8192);
	CHECK(part.spectrumTransformsForTest() > 0); // a picture that was idle starts again from nothing, its count with it
}

TEST_CASE("Comb: the Drive stage is the last thing on its wet — the same settings as Dust's, the dry untouched") {
	// THE SETTINGS ARE THE ONES DUST DERIVES from the same three controls.
	Settings s = defaults();
	s.drive = 0.7f;
	s.driveMode = DriveMode::Fold;
	s.driveShape = 0.4f;
	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	const std::vector<float> in = whiteInput(4800, 0.3f);
	std::vector<float> l = in;
	std::vector<float> r = in;
	e.processStereo(l.data(), r.data(), static_cast<int>(in.size()));
	const DriveSettings want = deriveDrive(0.7f, DriveMode::Fold, 0.4f, kRate);
	const DriveSettings& got = e.combDriveForTest();
	CHECK(got.driven);
	CHECK(got.mode == DriveMode::Fold);
	CHECK(got.shape == want.shape);
	CHECK(got.gain == want.gain);
	CHECK(got.norm == want.norm);

	// CRUSH, BARELY ON, IS A ROUNDING OF EVERY FRAME: so what the stage did to the wet, and that the dry went round
	// it, can be read frame by frame.
	s.driveMode = DriveMode::Crush;
	s.drive = 1.0e-6f;
	s.driveShape = 1.0f; // a step of 2^-0.5
	Eroder c;
	c.prepare(kRate);
	c.setTarget(s);
	int rounded = 0;
	int moved = 0;
	const auto step = static_cast<float>(std::exp2(-0.5));
	for (size_t i = 0; i < 4000; ++i) {
		float a = in[i];
		float b = in[i];
		c.processStereo(&a, &b, 1);
		const float wet = c.lastCombWetForTest();
		const float shaped = c.lastCombShapedForTest();
		rounded += shaped == std::round(wet / step) * step ? 1 : 0;
		moved += shaped != wet ? 1 : 0;
		// THE OUTPUT IS THE DRY PLUS THE SHAPED WET, exactly.
		REQUIRE(a == in[i] + shaped);
	}
	CHECK(rounded >= 3990); // a hold of one frame and a millionth skips one in a great many
	CHECK(moved > 3000);    // and the wet was not on the grid already
}

TEST_CASE("Comb: with Drive at work it is bounded, the same at any partition, and its tail ends in exact silence") {
	for (const DriveMode mode : kAllDrives) {
		CAPTURE(static_cast<int>(mode));
		Settings s = defaults();
		s.mix = 1.0f;
		s.drive = 1.0f;
		s.driveMode = mode;
		s.driveShape = 1.0f;
		s.follow = 0.5f;
		s.heat = 0.6f;
		s.hpWet = 9.0f;
		s.blend = true;
		s.subThru = true;
		const std::vector<float> in = whiteInput(24000, 0.9f);
		const auto render = [&](int block) {
			Eroder e;
			e.prepare(kRate);
			e.setTarget(s);
			std::vector<float> l = in;
			std::vector<float> r = in;
			l.resize(in.size() + 144000, 0.0f);
			r.resize(in.size() + 144000, 0.0f);
			for (size_t done = 0; done < l.size();) {
				const size_t take = std::min<size_t>(l.size() - done, static_cast<size_t>(block));
				e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
				done += take;
			}
			return l;
		};
		const std::vector<float> whole = render(4096);
		const bool sameByOnes = render(1) == whole;
		const bool sameByOdd = render(37) == whole;
		CHECK(sameByOnes);
		CHECK(sameByOdd);
		float peak = 0.0f;
		for (const float v : whole) {
			REQUIRE(std::isfinite(v));
			peak = std::max(peak, std::abs(v));
		}
		CHECK(peak < 8.0f);
		bool silent = true;
		for (size_t i = whole.size() - 24000; i < whole.size(); ++i) {
			silent = silent && whole[i] == 0.0f;
		}
		CHECK(silent);
	}
}

namespace {
/// THE WET HIGHPASS, RESTATED: a two-pole trapezoidal highpass at Q 0.707, one channel.
struct RefHighpass {
	double g = 0.0;
	double ic1 = 0.0;
	double ic2 = 0.0;
	explicit RefHighpass(double hz) : g(std::tan(std::numbers::pi * hz / kRate)) {}
	double run(double x) {
		const double k = std::sqrt(2.0);
		const double a1 = 1.0 / (1.0 + g * (g + k));
		const double v3 = x - ic2;
		const double v1 = a1 * ic1 + g * a1 * v3;
		const double v2 = ic2 + g * a1 * ic1 + g * g * a1 * v3;
		ic1 = 2.0 * v1 - ic1;
		ic2 = 2.0 * v2 - ic2;
		return x - k * v1 - v2;
	}
};

int signChanges(const std::vector<float>& v) {
	int n = 0;
	for (size_t i = 1; i < v.size(); ++i) {
		n += (v[i] < 0.0f) != (v[i - 1] < 0.0f) ? 1 : 0;
	}
	return n;
}

/// THE LEFT (or right) SHAKE, frame by frame, of a device fed a steady tone.
std::vector<float> shakeOf(const Settings& s, int channel, size_t frames) {
	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	std::vector<float> out(frames);
	for (size_t i = 0; i < frames; ++i) {
		float l = 0.3f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
		float r = l;
		e.processStereo(&l, &r, 1);
		out[i] = e.dustForTest().lastModForTest(channel);
	}
	return out;
}
} // namespace

TEST_CASE("Step holds the shake for a time; at the bottom of its range it moves every frame") {
	Settings s = declared();
	CHECK(deriveDust(s, 60.0, kRate).stepHoldFrames == 1.0);
	s.step = static_cast<float>(std::log2(0.001));
	CHECK(std::abs(deriveDust(s, 60.0, kRate).stepHoldFrames - 48.0) < 1.0e-3);
	s.step = 0.0f; // past the top: held at the top, 1/32 s
	CHECK(std::abs(deriveDust(s, 60.0, kRate).stepHoldFrames - kRate / 32.0) < 1.0e-6);

	const auto changes = [](const std::vector<float>& v) {
		int n = 0;
		for (size_t i = 1; i < v.size(); ++i) {
			n += v[i] != v[i - 1] ? 1 : 0;
		}
		return n;
	};
	// FREE: a band of noise moves on every frame.
	Settings freeRun = declared();
	CHECK(changes(shakeOf(freeRun, 0, 9600)) > 9000); // all but the frames it sits against its clamp
	// A MILLISECOND: one new value a hold, 200 of them in 9600 frames.
	Settings held = declared();
	held.step = static_cast<float>(std::log2(0.001));
	const int stairs = changes(shakeOf(held, 0, 9600));
	CHECK(stairs <= 200);
	CHECK(stairs >= 195);
}

TEST_CASE("Detune runs the right side's sine above the left's; Mod Fold turns the sine back at its peaks; neither is run at nothing") {
	Settings s = declared();
	s.source = 0.0f;
	s.stereo = 0.0f; // no lead, so the two sides differ only by what is under test
	s.freq = static_cast<float>(std::log2(100.0));
	const DustDerived plain = deriveDust(s, 60.0, kRate);
	CHECK_FALSE(plain.detuned);
	CHECK(plain.modFold == 0.0f);
	const std::vector<float> left = shakeOf(s, 0, 48000);
	const bool same = shakeOf(s, 1, 48000) == left;
	CHECK(same);
	CHECK(std::abs(signChanges(left) - 200) <= 2);

	// AN OCTAVE: twice the crossings on the right, the left unmoved.
	Settings twin = s;
	twin.detune = 1.0f;
	const DustDerived d = deriveDust(twin, 60.0, kRate);
	CHECK(d.detuned);
	CHECK(std::abs(d.sineStepRight / d.sineStep - 2.0) < 1.0e-12);
	twin.detune = 0.5f;
	CHECK(std::abs(deriveDust(twin, 60.0, kRate).sineStepRight / d.sineStep - std::sqrt(2.0)) < 1.0e-12);
	twin.detune = 1.0f;
	const bool leftUnmoved = shakeOf(twin, 0, 48000) == left;
	CHECK(leftUnmoved);
	CHECK(std::abs(signChanges(shakeOf(twin, 1, 48000)) - 400) <= 2);

	// THE FOLD: the sine times five, turned back by a triangle of period four — restated from the plain sine.
	Settings folded = s;
	folded.modFold = 1.0f;
	const std::vector<float> turned = shakeOf(folded, 0, 4800);
	double worst = 0.0;
	for (size_t i = 0; i < turned.size(); ++i) {
		const double u = 5.0 * static_cast<double>(left[i]);
		const double phase = u + 1.0 - 4.0 * std::floor((u + 1.0) / 4.0);
		const double want = phase < 2.0 ? phase - 1.0 : 3.0 - phase;
		worst = std::max(worst, std::abs(static_cast<double>(turned[i]) - want));
	}
	CHECK(worst < 2.0e-5);
	CHECK(signChanges(turned) > 3 * signChanges({ left.begin(), left.begin() + 4800 }));
}

TEST_CASE("Rust multiplies the signal by the shake: Rectify folds the carrier to one side, Bias lets the signal past, and there is no line") {
	Settings s = declared();
	s.mode = Mode::Rust;
	s.resonance = 0.0f; // at none of it Rust has no loop at all (its inner loop is `loops.h`'s)
	const DustDerived d = deriveDust(s, 60.0, kRate);
	CHECK(d.ring);
	CHECK(d.feedback == 0.0);
	CHECK(d.wetGain == 1.0);
	CHECK_FALSE(deriveDust(declared(), 60.0, kRate).ring);

	for (const auto& [bias, rectify] : { std::pair { 0.0f, 0.0f }, std::pair { 0.0f, 1.0f }, std::pair { 0.6f, 0.3f } }) {
		CAPTURE(bias);
		CAPTURE(rectify);
		s.rustBias = bias;
		s.rustRectify = rectify;
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		double worst = 0.0;
		float least = 9.0f;
		for (int i = 0; i < 4800; ++i) {
			const float in = 0.4f * static_cast<float>(std::sin(0.07 * i));
			float l = in;
			float r = in;
			e.processStereo(&l, &r, 1);
			const Dust& dust = e.dustForTest();
			// THE OUTPUT IS THE INPUT TIMES THE MULTIPLIER, exactly, with no delay.
			REQUIRE(l == in * dust.lastRingForTest(0));
			// AND THE MULTIPLIER IS THE SHAKE, folded and let past — restated.
			const double c = static_cast<double>(dust.lastModForTest(0));
			const double carrier = c + static_cast<double>(rectify) * (std::abs(c) - c);
			const double want = static_cast<double>(bias) + (1.0 - static_cast<double>(bias)) * carrier;
			worst = std::max(worst, std::abs(static_cast<double>(dust.lastRingForTest(0)) - want));
			least = std::min(least, dust.lastRingForTest(0));
			REQUIRE(dust.lastDelayFramesForTest(0) == 0.0);
		}
		CHECK(worst < 1.0e-6);
		// RECTIFIED WHOLE, the carrier never goes negative.
		if (rectify == 1.0f) {
			CHECK(least >= 0.0f);
		}
		else if (bias == 0.0f) {
			CHECK(least < -0.2f);
		}
	}

	// FOLLOW: at all of it, with next to nothing coming in, the multiplier stays at one.
	s.follow = 1.0f;
	Eroder quiet;
	quiet.prepare(kRate);
	quiet.setTarget(s);
	float far = 0.0f;
	for (int i = 0; i < 4800; ++i) {
		float l = 1.0e-4f * static_cast<float>(std::sin(0.07 * i));
		float r = l;
		quiet.processStereo(&l, &r, 1);
		far = std::max(far, std::abs(quiet.dustForTest().lastRingForTest(0) - 1.0f));
	}
	CHECK(far < 1.0e-3f);
}

TEST_CASE("Rust with everything at work: bounded, the same at any partition, and silent the moment the input is") {
	Settings s = declared();
	s.mode = Mode::Rust;
	s.rustRectify = 0.5f;
	s.step = -10.0f;
	s.detune = 0.4f;
	s.modFold = 0.5f;
	s.source = 0.5f;
	s.drive = 0.8f;
	s.driveMode = DriveMode::Fold;
	s.hpWet = 8.0f;
	s.subThru = true;
	s.follow = 0.5f;
	const std::vector<float> in = whiteInput(24000, 0.9f);
	const auto render = [&](int block) {
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		l.resize(in.size() + 48000, 0.0f);
		r.resize(in.size() + 48000, 0.0f);
		for (size_t done = 0; done < l.size();) {
			const size_t take = std::min<size_t>(l.size() - done, static_cast<size_t>(block));
			e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
			done += take;
		}
		return l;
	};
	const std::vector<float> whole = render(4096);
	const bool sameByOnes = render(1) == whole;
	const bool sameByOdd = render(37) == whole;
	CHECK(sameByOnes);
	CHECK(sameByOdd);
	float peak = 0.0f;
	for (const float v : whole) {
		REQUIRE(std::isfinite(v));
		peak = std::max(peak, std::abs(v));
	}
	CHECK(peak < 8.0f);
	CHECK(peak > 0.05f);
	bool silent = true;
	for (size_t i = whole.size() - 24000; i < whole.size(); ++i) {
		silent = silent && whole[i] == 0.0f;
	}
	CHECK(silent);
}

TEST_CASE("Comb: Heat is the same comb on a hotter signal — the device at no Heat, fed that much more and turned down again") {
	Settings s = defaults();
	s.mix = 1.0f;
	Eroder cold;
	cold.prepare(kRate);
	cold.setTarget(s);
	Settings hotSettings = s;
	hotSettings.heat = 2.0f / 7.0f; // two octaves: four times (eight would carry the cold side's wet over the wet ceiling)
	Eroder hot;
	hot.prepare(kRate);
	hot.setTarget(hotSettings);
	const std::vector<float> in = whiteInput(9600, 0.5f);
	double worst = 0.0;
	double moved = 0.0;
	for (size_t i = 0; i < in.size(); ++i) {
		float a = in[i];
		float b = in[i];
		hot.processStereo(&a, &b, 1);
		float c = in[i] * 4.0f;
		float d = c;
		cold.processStereo(&c, &d, 1);
		// THE WET ALONE, each side: the hot device's, and the cold one's turned down by the same four.
		const double wetHot = static_cast<double>(a) - static_cast<double>(in[i]);
		const double wetCold = (static_cast<double>(c) - static_cast<double>(in[i]) * 4.0) / 4.0;
		worst = std::max(worst, std::abs(wetHot - wetCold));
		moved = std::max(moved, std::abs(wetHot));
	}
	CHECK(hot.combHeatForTest() == doctest::Approx(4.0f).epsilon(1.0e-6));
	CHECK(cold.combHeatForTest() == 1.0f);
	CHECK(moved > 0.01);
	CHECK(worst < 1.0e-5);
}

TEST_CASE("Comb: the level law's make-up is sqrt((1 + fb) / (1 - fb)), and the wet goes out at the reference's gain times it") {
	for (const double tone : { 0.05, 0.31, 0.47, 0.83 }) {
		CAPTURE(tone);
		const Derived d = derive({ tone, 0.2 }, 0.0, 0.7, 60.0, kRate);
		CHECK(std::abs(d.levelMakeUp - std::sqrt((1.0 + d.feedback) / (1.0 - d.feedback))) < 1.0e-12);
		CHECK(d.levelMakeUp >= 1.0);
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.tone = static_cast<float>(tone);
		s.character = 0.2f;
		s.mix = 0.7f;
		e.setTarget(s);
		std::vector<float> l(64, 0.1f);
		std::vector<float> r(64, 0.1f);
		e.processStereo(l.data(), r.data(), 64);
		const Derived& ran = e.derivedForTest();
		CHECK(e.combOutGainForTest()
		      == doctest::Approx(ran.wetGain * std::sqrt((1.0 + ran.feedback) / (1.0 - ran.feedback)) * 128.0).epsilon(1e-6));
		CHECK(e.combDryGainForTest() == 1.0f);
	}
}

TEST_CASE("Comb: Mix, Cutoff and Gamma arrive by a TIME, whatever the block length") {
	for (const int block : { 1, 64, 480 }) {
		CAPTURE(block);
		Eroder e;
		e.prepare(kRate);
		Settings s = defaults();
		s.mix = 0.2f;
		s.cutoffOctaves = 0.0f;
		s.gamma = 0.0f;
		e.setTarget(s);
		std::vector<float> l(4800, 0.1f);
		std::vector<float> r(4800, 0.1f);
		const auto run = [&](size_t frames) {
			for (size_t done = 0; done < frames; done += static_cast<size_t>(block)) {
				const size_t take = std::min<size_t>(static_cast<size_t>(block), frames - done);
				e.processStereo(l.data(), r.data(), static_cast<int>(take));
			}
		};
		run(960);
		const float startGain = e.combOutGainForTest();
		s.mix = 0.8f;
		e.setTarget(s);
		run(2400); // a twentieth of a second: one time constant
		// THE MIX, a sample at a time: 1 - 1/e of the way, to the frame.
		{
			const Derived& d = e.derivedForTest();
			const double target = d.wetGain * d.levelMakeUp * 128.0;
			REQUIRE(target > 2.0 * static_cast<double>(startGain));
			const double want = target + (static_cast<double>(startGain) - target) * std::exp(-2400.0 / (0.05 * kRate));
			CHECK(e.combOutGainForTest() == doctest::Approx(want).epsilon(2e-3));
		}
		s.cutoffOctaves = 2.0f;
		s.gamma = 0.01f;
		e.setTarget(s);
		run(2400);
		// CUTOFF AND GAMMA, a control block at a time: 75 blocks of a one-pole.
		const double left = std::pow(std::exp(-32.0 / (0.05 * kRate)), 75.0);
		CHECK(e.smoothedCutoffForTest() == doctest::Approx(2.0 * (1.0 - left)).epsilon(1e-6));
		CHECK(e.smoothedGammaForTest() == doctest::Approx(0.01 * (1.0 - left)).epsilon(1e-6));
		// AND AT REST THEY LAND ON WHAT THEY ARE SET TO.
		run(48000);
		run(48000);
		CHECK(e.smoothedCutoffForTest() == 2.0);
		CHECK(e.combOutGainForTest() == static_cast<float>(e.derivedForTest().wetGain * e.derivedForTest().levelMakeUp) * 128.0f);
	}
}

TEST_CASE("Comb: Blend crossfades to the comb alone; HP Wet filters the wet after the shaper; Sub Thru gives the crossfade's low end back") {
	Settings s = defaults();
	s.blend = true;
	s.mix = 1.0f;
	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	const std::vector<float> in = whiteInput(4800, 0.4f);
	for (size_t i = 0; i < in.size(); ++i) {
		float l = in[i];
		float r = in[i];
		e.processStereo(&l, &r, 1);
		// AT ALL OF MIX THERE IS NO DRY IN IT: the output is the comb's wet, exactly.
		REQUIRE(e.combDryGainForTest() == 0.0f);
		REQUIRE(l == e.lastCombShapedForTest());
	}
	// PART WAY: the dry at one minus Mix.
	s.mix = 0.3f;
	Eroder part;
	part.prepare(kRate);
	part.setTarget(s);
	float a = 0.2f;
	float b = 0.2f;
	part.processStereo(&a, &b, 1);
	CHECK(part.combDryGainForTest() == 1.0f - 0.3f);

	// HP WET at 1 kHz, Sub Thru on, Mix 60%: out = dry * 0.4 + HP(wet) + 0.6 * (dry - HP(dry)), restated.
	s.mix = 0.6f;
	s.hpWet = static_cast<float>(std::log2(1000.0));
	s.subThru = true;
	Eroder f;
	f.prepare(kRate);
	f.setTarget(s);
	RefHighpass wetFilter(1000.0);
	RefHighpass dryFilter(1000.0);
	double worst = 0.0;
	double taken = 0.0;
	for (size_t i = 0; i < in.size(); ++i) {
		float l = in[i];
		float r = in[i];
		f.processStereo(&l, &r, 1);
		const auto dry = static_cast<double>(in[i]);
		const double filtered = wetFilter.run(static_cast<double>(f.lastCombShapedForTest()));
		const double low = dry - dryFilter.run(dry);
		const double want = dry * static_cast<double>(1.0f - 0.6f) + filtered + static_cast<double>(0.6f) * low;
		worst = std::max(worst, std::abs(static_cast<double>(l) - want));
		taken = std::max(taken, std::abs(low));
	}
	CHECK(worst < 2.0e-5);
	CHECK(taken > 0.01); // the filter is taking something, so the sum above is not the unfiltered one

	// WITH THE DRY WHOLE (Blend off) SUB THRU HAS NOTHING TO GIVE BACK: the same bits either way.
	s.blend = false;
	const auto render = [&](bool sub) {
		Settings t = s;
		t.subThru = sub;
		Eroder g;
		g.prepare(kRate);
		g.setTarget(t);
		std::vector<float> l = in;
		std::vector<float> r = in;
		g.processStereo(l.data(), r.data(), static_cast<int>(in.size()));
		return l;
	};
	const bool inert = render(true) == render(false);
	CHECK(inert);
}

//--------------------------------------------------------------------------------------------------//
// THE SHARED SECTION: EMPHASIS, SMEAR, TAP AND TWO LOOPS (`loops.h`), in every engine.                //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A NUMBER THE SUITE MEASURED, printed when FEATHERS_MEASURE is set (the bound beside it is set from it).
void measured(const char* what, double value) {
	if (std::getenv("FEATHERS_MEASURE") != nullptr) {
		std::fprintf(stderr, "MEASURED %s = %.9g\n", what, value);
	}
}

/// A RATE AT WHICH THE FIVE HUNDRED AND TWELVE HERTZ OF 2^9 IS A WHOLE NUMBER OF FRAMES: a period of 2^k hertz is exact.
constexpr double kLoopRate = 32768.0;

std::vector<float> renderAt(double rate, const Settings& s, const std::vector<float>& in, int block = 256,
                            const device::NoteContext* note = nullptr) {
	Eroder e;
	e.prepare(rate);
	if (note != nullptr) {
		e.setNoteContext(*note);
	}
	e.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (size_t done = 0; done < l.size();) {
		const size_t take = std::min<size_t>(l.size() - done, static_cast<size_t>(block));
		e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
		done += take;
	}
	return l;
}

std::vector<float> impulseOf(size_t frames, float amplitude = 0.5f) {
	std::vector<float> v(frames, 0.0f);
	v[0] = amplitude;
	return v;
}

/// ROOT-MEAN-SQUARE OF THE DIFFERENCE, over the root-mean-square of the reference.
double relativeError(const std::vector<float>& a, const std::vector<float>& reference) {
	double err = 0.0;
	double ref = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		const double d = static_cast<double>(a[i]) - static_cast<double>(reference[i]);
		err += d * d;
		ref += static_cast<double>(reference[i]) * static_cast<double>(reference[i]);
	}
	return std::sqrt(err / std::max(ref, 1.0e-30));
}

float peakOf(const std::vector<float>& v) {
	float peak = 0.0f;
	for (const float x : v) {
		peak = std::max(peak, std::abs(x));
	}
	return peak;
}

/// RUST WITH THE MULTIPLIER OPEN (Bias at the top is a gain of exactly one), so it is the loops and nothing else.
Settings rustPure() {
	Settings s = declared();
	s.mode = Mode::Rust;
	s.rustBias = 1.0f;
	s.mix = 1.0f;
	s.freq = 10.0f; // 1024 Hz: 32 frames at kLoopRate
	return s;
}

/// A TONE'S AMPLITUDE THROUGH A SETTING, after `settle` frames, at any rate.
double toneThrough(const Settings& s, double hz, double rate, size_t frames, size_t settle) {
	std::vector<float> in(frames + settle);
	for (size_t i = 0; i < in.size(); ++i) {
		in[i] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
	}
	const std::vector<float> out = renderAt(rate, s, in);
	const std::vector<float> tail(out.begin() + static_cast<std::ptrdiff_t>(settle), out.end());
	double re = 0.0;
	double im = 0.0;
	for (size_t i = 0; i < tail.size(); ++i) {
		const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i + settle) / rate;
		re += static_cast<double>(tail[i]) * std::cos(w);
		im += static_cast<double>(tail[i]) * std::sin(w);
	}
	return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(tail.size());
}

/// EVERYTHING ON, for one engine and one Type (the cases that must be bounded, partition-proof and end in silence).
Settings everythingOn(Mode mode, DriveMode type) {
	Settings s = declared();
	s.mode = mode;
	s.mix = 0.7f;
	s.freq = hertzToLog2(800.0);
	s.emphAmount = 9.0f;
	s.emphInLoop = true;
	s.smear = 1.0f;
	s.tapDriven = true;
	s.resonance = 0.8f;
	s.loop = 1.0f;
	s.loopInterval = 7.0f;
	s.drive = 1.0f;
	s.driveMode = type;
	s.driveShape = 0.7f;
	return s;
}

} // namespace

TEST_CASE("Shared section: eight declared controls, stored by their keys, and at the defaults no stage runs in any engine") {
	struct Row {
		Param param;
		const char* key;
		double min;
		double max;
		double def;
	};
	const Row rows[] = {
		{ Param::EmphAmount, "eroder_emph_amount", -18.0, 18.0, 0.0 }, { Param::EmphShape, "eroder_emph_shape", 0.0, 1.0, 0.0 },
		{ Param::EmphRestore, "eroder_emph_restore", 0.0, 1.0, 1.0 },  { Param::EmphInLoop, "eroder_emph_in_loop", 0.0, 1.0, 0.0 },
		{ Param::Smear, "eroder_smear", 0.0, 1.0, 0.0 },               { Param::FbTap, "eroder_fb_tap", 0.0, 1.0, 0.0 },
		{ Param::Loop, "eroder_loop", 0.0, 1.0, 0.0 },                 { Param::LoopInterval, "eroder_loop_interval", -24.0, 24.0, 0.0 },
	};
	REQUIRE(std::size(rows) == kSharedParamCount);
	for (const Row& r : rows) {
		const auto& d = kParamDescs[static_cast<size_t>(r.param)];
		CAPTURE(r.key);
		CHECK(std::string_view(d.key) == r.key);
		CHECK(d.min == r.min);
		CHECK(d.max == r.max);
		CHECK(d.def == r.def);
		CHECK(d.gateControl == -1); // every engine's
	}
	// THE AMOUNT, THE SMEAR, THE LOOP AND THE INTERVAL ARE MODULATABLE; the amount and the interval are centred.
	for (const Param p : { Param::EmphAmount, Param::Smear, Param::Loop, Param::LoopInterval }) {
		CHECK(kParamDescs[static_cast<size_t>(p)].modulatable);
	}
	CHECK(kParamDescs[static_cast<size_t>(Param::EmphAmount)].centred);
	CHECK(kParamDescs[static_cast<size_t>(Param::LoopInterval)].centred);
	CHECK(kParamDescs[static_cast<size_t>(Param::EmphShape)].names.size() == 2);
	CHECK(kParamDescs[static_cast<size_t>(Param::FbTap)].names.size() == 2);

	// THE CODEC CARRIES ALL EIGHT, and a short row from before them reads their defaults.
	State st;
	st.settings.emphAmount = -7.5f;
	st.settings.emphBell = true;
	st.settings.emphRestore = false;
	st.settings.emphInLoop = true;
	st.settings.smear = 0.4f;
	st.settings.tapDriven = true;
	st.settings.loop = 0.65f;
	st.settings.loopInterval = -9.0f;
	CHECK(stateOf(rowOf(st), static_cast<size_t>(Param::kCount)) == st);
	const State old = stateOf(rowOf(st), static_cast<size_t>(Param::EmphAmount));
	CHECK(old.settings.emphAmount == 0.0f);
	CHECK(old.settings.emphRestore);
	CHECK(old.settings.loop == 0.0f);
	CHECK(Settings {} == declared());

	// AT THE DEFAULTS NO STAGE OF THE SHARED SECTION IS RUN, in Dust, Rust or the Comb: the stages report not-run.
	const std::vector<float> in = whiteInput(4800, 0.4f);
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		Settings s = declared();
		s.mode = mode;
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		e.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		const SharedSection& sh = mode == Mode::Comb ? e.combSharedForTest() : e.dustForTest().sharedForTest();
		const LoopDerived& loops = mode == Mode::Comb ? e.combLoopsForTest() : e.dustForTest().derivedForTest().loops;
		CHECK_FALSE(sh.emphOn());
		CHECK_FALSE(sh.emphInLoop());
		CHECK_FALSE(sh.smearOn());
		CHECK_FALSE(sh.outerWanted());
		CHECK_FALSE(sh.limitInner());
		CHECK(sh.outerGainForTest() == 0.0f);
		CHECK(loops.loopGain == 0.0);
		CHECK_FALSE(sh.holdsSound());
	}
	// AND THE COMB'S INNER GAIN IS THE BANK'S, to the bit.
	Settings c = defaults();
	Eroder comb;
	comb.prepare(kRate);
	comb.setTarget(c);
	std::vector<float> l = in;
	std::vector<float> r = in;
	comb.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	CHECK(comb.combFeedbackGainForTest() == static_cast<float>(comb.derivedForTest().feedback));
	CHECK(comb.combLoopsForTest().innerFeedback == comb.derivedForTest().feedback);
}

TEST_CASE("Emphasis Offset: the emphasis is tuned to Freq times two to the offset over twelve, clamped; zero is Freq itself; Smear stays at Freq") {
	const auto& d = kParamDescs[static_cast<size_t>(Param::EmphOffset)];
	CHECK(std::string_view(d.key) == "eroder_emph_offset");
	CHECK(std::string_view(d.name) == "Eroder Emphasis Offset");
	CHECK(d.min == -48.0);
	CHECK(d.max == 48.0);
	CHECK(d.def == 0.0);
	CHECK_FALSE(d.offsetsMayLeaveRange);
	CHECK(d.modulatable);
	CHECK(d.gateControl == -1);
	// THE CODEC CARRIES IT.
	State st;
	st.settings.emphOffset = -17.5f;
	CHECK(stateOf(rowOf(st), static_cast<size_t>(Param::kCount)) == st);
	CHECK(Settings {}.emphOffset == 0.0f);

	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		for (const bool bell : { false, true }) {
			for (const float offset : { 0.0f, 7.0f, -12.0f, 24.5f, 48.0f, -48.0f }) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(bell);
				CAPTURE(offset);
				Settings s = declared();
				s.mode = mode;
				s.freq = std::log2(440.0f);
				s.emphAmount = 9.0f;
				s.emphBell = bell;
				s.emphOffset = offset;
				s.smear = 0.5f;
				Eroder e;
				e.prepare(kRate);
				e.setTarget(s);
				std::vector<float> l = whiteInput(512, 0.3f);
				std::vector<float> r = l;
				e.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
				const SharedSection& sh = mode == Mode::Comb ? e.combSharedForTest() : e.dustForTest().sharedForTest();
				const double centre = mode == Mode::Comb ? 1.0 / e.derivedForTest().baseDelaySeconds
				                                         : e.dustForTest().derivedForTest().freqHz;
				// AN INDEPENDENT RESTATEMENT of the tuning.
				double expected = centre;
				if (offset != 0.0f) {
					expected = std::min(std::max(centre * std::pow(2.0, static_cast<double>(offset) / 12.0), 20.0),
					                    std::min(20000.0, 0.45 * kRate));
				}
				CHECK(sh.emphasisForTest().hzForTest() == static_cast<float>(expected));
				if (offset == 0.0f) {
					CHECK(sh.emphasisForTest().hzForTest() == static_cast<float>(centre)); // Freq, to the bit
				}
			}
		}
	}
	// THE CLAMPS, from the static: the floor, the ceiling, and the rate's 0.45.
	CHECK(SharedSection::emphasisHz(30.0, -48.0, 48000.0) == 20.0);
	CHECK(SharedSection::emphasisHz(10000.0, 48.0, 48000.0) == 20000.0);
	CHECK(SharedSection::emphasisHz(10000.0, 48.0, 22050.0) == doctest::Approx(0.45 * 22050.0));
	CHECK(SharedSection::emphasisHz(1000.0, 12.0, 48000.0) == doctest::Approx(2000.0));
	CHECK(SharedSection::emphasisHz(1000.0, 0.0, 48000.0) == 1000.0);
	CHECK(SharedSection::emphasisHz(3.0, 0.0, 48000.0) == 3.0); // zero is untouched even under the floor

	// THE OFFSET MOVES THE SOUND (so the check above is not of a dead control), in every engine, and the Smear's centre is Freq's.
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		const std::vector<float> in = whiteInput(4800, 0.4f);
		std::vector<float> out[2];
		for (int k = 0; k < 2; ++k) {
			Settings s = declared();
			s.mode = mode;
			s.emphAmount = 12.0f;
			s.emphBell = true;
			s.drive = 0.6f;
			s.emphOffset = k == 0 ? 0.0f : 19.0f;
			Eroder e;
			e.prepare(kRate);
			e.setTarget(s);
			out[k] = in;
			std::vector<float> r = in;
			e.processStereo(out[k].data(), r.data(), static_cast<int>(in.size()));
		}
		CHECK(out[0] != out[1]);
	}
}

TEST_CASE("Emphasis: with no Drive and Restore on the pair cancels — Dust still to 1e-5; the Comb to what its moving head allows; Rust is bounded and differs") {
	const std::vector<float> in = whiteInput(24000, 0.5f);
	for (const bool bell : { false, true }) {
		for (const float db : { 12.0f, -12.0f }) {
			CAPTURE(bell);
			CAPTURE(db);
			Settings base = dustStill();
			base.freq = hertzToLog2(1000.0);
			base.emphBell = bell;
			Settings with = base;
			with.emphAmount = db;
			const std::vector<float> a = renderAt(kRate, base, in);
			const std::vector<float> b = renderAt(kRate, with, in);
			const double err = relativeError(b, a);
			measured("dust still emphasis cancel relative error", err);
			CHECK(err < 1.0e-5);
			// NON-VACUITY: with Restore off the emphasis is in the sound.
			with.emphRestore = false;
			CHECK(relativeError(renderAt(kRate, with, in), a) > 0.1);
		}
	}

	// THE COMB: the head is shaken by noise, and a moving delay does not commute with a filter, so the pair cancels only to the
	// shake's own size.
	// WITH FOLLOW AT THE TOP AND A QUIET INPUT THE SHAKE IS NEARLY NONE (it rides the input's level), which is where the pair can be read.
	std::vector<float> quiet = in;
	for (float& v : quiet) {
		v *= 0.002f;
	}
	Settings combBase = defaults();
	combBase.follow = 1.0f;
	Settings combWith = combBase;
	combWith.emphAmount = 12.0f;
	const std::vector<float> ca = renderAt(kRate, combBase, quiet);
	const std::vector<float> cb = renderAt(kRate, combWith, quiet);
	const double combErr = relativeError(cb, ca);
	measured("comb emphasis cancel relative error", combErr);
	CHECK(combErr < 0.01); // measured 0.0019
	combWith.emphRestore = false;
	const double combOff = relativeError(renderAt(kRate, combWith, quiet), ca);
	measured("comb emphasis restore-off relative difference", combOff);
	CHECK(combOff > 100.0 * combErr); // measured 1.17 against 0.0019

	// RUST MOVES FREQUENCIES (a ring modulator), so a filter before it and its inverse after it are not a pair.
	Settings rb = declared();
	rb.mode = Mode::Rust;
	Settings rw = rb;
	rw.emphAmount = 12.0f;
	const std::vector<float> ra = renderAt(kRate, rb, in);
	const std::vector<float> rr = renderAt(kRate, rw, in);
	CHECK(peakOf(rr) < 4.0f);
	CHECK(relativeError(rr, ra) > 0.05);
}

TEST_CASE("Emphasis: with Drive up a tilt moves a tone's distortion the way it predicts — a low tone driven less under +12, more under -12") {
	// A LOW TONE AND A HIGH ONE through Soft at Drive 0.8, the pivot between them. +12 dB cuts the low tone going in (by
	// the tilt's response there) and Restore puts it back, so its third harmonic is what a quieter tone made: LOWER than
	// at no emphasis; -12 lifts the low tone going in, and the harmonic comes up.
	const double lo = 187.5; // whole cycles in a second at 48 kHz
	const double hi = 3000.0;
	const size_t frames = 48000;
	std::vector<float> in(frames + 9600);
	for (size_t i = 0; i < in.size(); ++i) {
		const double t = static_cast<double>(i) / kRate;
		in[i] = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * lo * t) + 0.25 * std::sin(2.0 * std::numbers::pi * hi * t));
	}
	const auto third = [&](float db) {
		Settings s = dustStill();
		s.freq = hertzToLog2(1000.0);
		s.drive = 0.8f;
		s.driveMode = DriveMode::Soft;
		s.emphAmount = db;
		const std::vector<float> out = renderAt(kRate, s, in);
		const std::vector<float> tail(out.begin() + 9600, out.end());
		return dbOf(toneAt(tail, 3.0 * lo) / toneAt(tail, lo));
	};
	const double flat = third(0.0f);
	const double up = third(12.0f);
	const double down = third(-12.0f);
	measured("third harmonic of the low tone, no emphasis, dB re fundamental", flat);
	measured("third harmonic of the low tone, +12 dB tilt", up);
	measured("third harmonic of the low tone, -12 dB tilt", down);
	// THE SIGN, AND A SIZE: half of what was measured (the numbers are in the report).
	// MEASURED: -39.0 flat, -64.8 at +12 (25.8 dB down), -5.3 at -12 (33.7 dB up). The bounds are half of each.
	CHECK(up < flat - 12.9);
	CHECK(down > flat + 16.8);
}

TEST_CASE("Emphasis: with Restore off the response is the emphasis' own, restated — the tilt's shelf and the bell's peak") {
	const double rate = kRate;
	Settings s = dustStill();
	s.freq = hertzToLog2(1000.0);
	s.emphRestore = false;
	const auto gainDb = [&](const Settings& t, double hz) {
		return dbOf(toneThrough(t, hz, rate, 48000, 9600) / 0.3);
	};
	// THE TILT: H(s) = (g s + 1) / (s + g), g = 10^(dB / 20), s = j tan(pi f / rate) / tan(pi pivot / rate): unity at the pivot.
	const auto tiltDb = [&](double db, double hz) {
		const double g = std::pow(10.0, db / 20.0);
		const std::complex<double> sn(0.0, std::tan(std::numbers::pi * hz / rate) / std::tan(std::numbers::pi * 1000.0 / rate));
		return 20.0 * std::log10(std::abs((g * sn + 1.0) / (sn + g)));
	};
	for (const float db : { 12.0f, -9.0f }) {
		s.emphAmount = db;
		s.emphBell = false;
		for (const double hz : { 125.0, 500.0, 1000.0, 2000.0, 6000.0 }) {
			CAPTURE(db);
			CAPTURE(hz);
			CHECK(std::abs(gainDb(s, hz) - tiltDb(static_cast<double>(db), hz)) < 0.05);
		}
		CHECK(std::abs(gainDb(s, 1000.0)) < 0.05); // unity at the pivot
	}
	// THE BELL: its gain at the centre is exactly the amount, and a band far away is left alone; its width is Width's, in octaves.
	s.emphBell = true;
	s.emphAmount = 12.0f;
	for (const float width : { 0.0f, 0.5f, 1.0f }) {
		s.width = width;
		CAPTURE(width);
		CHECK(std::abs(gainDb(s, 1000.0) - 12.0) < 0.1);
		CHECK(std::abs(gainDb(s, 60.0)) < 0.5);
		// BETWEEN THE HALF-GAIN POINTS: the octaves the control maps to, at one end of the response.
		const double octaves = SharedSection::emphasisWidthOctaves(width);
		CHECK(octaves >= kEmphasisWidthOctavesMin - 1.0e-12);
		CHECK(octaves <= kEmphasisWidthOctavesMax + 1.0e-12);
		const double edge = 1000.0 * std::pow(2.0, octaves * 0.5);
		if (edge < 20000.0) {
			CHECK(std::abs(gainDb(s, edge) - 6.0) < 1.0); // half the gain in dB, near the half-gain point
		}
	}
	CHECK(SharedSection::emphasisWidthOctaves(0.0f) == doctest::Approx(0.25));
	CHECK(SharedSection::emphasisWidthOctaves(1.0f) == doctest::Approx(3.0));
}

TEST_CASE("Smear: not run at none of it; at all of it the level is unchanged to 0.05 dB and a square's corners come apart; its centre follows Freq") {
	Settings s = dustStill();
	s.freq = hertzToLog2(1000.0);
	s.smear = 0.0f;
	{
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l = whiteInput(512, 0.3f);
		std::vector<float> r = l;
		e.processStereo(l.data(), r.data(), 512);
		CHECK_FALSE(e.dustForTest().sharedForTest().smearOn());
	}
	Settings smeared = s;
	smeared.smear = 1.0f;
	for (const double hz : { 200.0, 1000.0, 3000.0, 8000.0 }) {
		CAPTURE(hz);
		const double flat = toneThrough(s, hz, kRate, 48000, 48000);
		const double with = toneThrough(smeared, hz, kRate, 48000, 48000);
		CHECK(std::abs(dbOf(with / flat)) < 0.05);
	}

	// A SQUARE WAVE'S CREST FACTOR RISES.
	std::vector<float> square(96000);
	for (size_t i = 0; i < square.size(); ++i) {
		square[i] = (static_cast<double>(i % 480) < 240.0) ? 0.4f : -0.4f; // 100 Hz
	}
	const auto crest = [&](const Settings& t) {
		const std::vector<float> out = renderAt(kRate, t, square);
		const std::vector<float> tail(out.begin() + 48000, out.end());
		return static_cast<double>(peakOf(tail)) / rmsOf(tail);
	};
	const double crestFlat = crest(s);
	const double crestSmeared = crest(smeared);
	measured("square crest factor, no smear", crestFlat);
	measured("square crest factor, smear 1", crestSmeared);
	CHECK(crestSmeared > crestFlat * 1.05);

	// THE CENTRE FOLLOWS FREQ: the chain's group delay at its centre is 1 / Freq, so an octave up halves it — by the knob, and by Track.
	const auto delayAt = [&](float freqLog2, bool track, float note) {
		Settings t = smeared;
		t.freq = freqLog2;
		t.track = track;
		Eroder e;
		e.prepare(kRate);
		e.setNoteContext({ note, true });
		e.setTarget(t);
		std::vector<float> l(64, 0.0f);
		std::vector<float> r = l;
		e.processStereo(l.data(), r.data(), 64);
		return e.dustForTest().sharedForTest().smearForTest().delayAtCentre();
	};
	const double d500 = delayAt(hertzToLog2(500.0), false, 60.0f);
	const double d1000 = delayAt(hertzToLog2(1000.0), false, 60.0f);
	const double d2000 = delayAt(hertzToLog2(2000.0), false, 60.0f);
	measured("smear delay at centre, 500 Hz, frames", d500);
	CHECK(d500 / d1000 == doctest::Approx(2.0).epsilon(0.02));
	CHECK(d1000 / d2000 == doctest::Approx(2.0).epsilon(0.02));
	const double tracked = delayAt(hertzToLog2(1000.0), true, 72.0f); // an octave up the keyboard
	CHECK(tracked / d1000 == doctest::Approx(0.5).epsilon(0.02));

	// THE TUNED DELAYS ARE AS SET, WITH THE SMEAR ON: it does not take its own delay out of them (that clamped every one to
	// its floor, since the chain's delay at its centre is over a period at the gentlest smear, and Interval did nothing).
	Settings rust = rustPure();
	rust.resonance = 0.5f;
	rust.loop = 0.5f;
	rust.smear = 0.3f;
	Eroder e;
	e.prepare(kLoopRate);
	e.setTarget(rust);
	std::vector<float> l(64, 0.0f);
	std::vector<float> r = l;
	e.processStereo(l.data(), r.data(), 64);
	const SharedSection& sh = e.dustForTest().sharedForTest();
	CHECK(sh.smearForTest().delayAtCentre() > 32.0);
	CHECK(sh.innerFramesForTest() == doctest::Approx(32.0));
	CHECK(sh.outerFramesForTest() == doctest::Approx(32.0));
	rust.loopInterval = 12.0f;
	e.setTarget(rust);
	e.processStereo(l.data(), r.data(), 64);
	CHECK(e.dustForTest().sharedForTest().outerFramesForTest() == doctest::Approx(16.0));
	CHECK(e.dustForTest().sharedForTest().innerFramesForTest() == doctest::Approx(32.0));

	// THE COMB'S LINE IS AS SET TOO: the head sits where it does with the Smear off (the shake is the same stream either way).
	for (const float smear : { 0.3f, 1.0f }) {
		CAPTURE(smear);
		Settings c = defaults();
		Settings cs = c;
		cs.smear = smear;
		Eroder plain;
		plain.prepare(kRate);
		plain.setTarget(c);
		Eroder smeared;
		smeared.prepare(kRate);
		smeared.setTarget(cs);
		std::vector<float> a = whiteInput(512, 0.3f);
		std::vector<float> b = a;
		std::vector<float> ar = a;
		std::vector<float> br = a;
		plain.processStereo(a.data(), ar.data(), 512);
		smeared.processStereo(b.data(), br.data(), 512);
		CHECK(smeared.lastDelaySecondsForTest(0) == plain.lastDelaySecondsForTest(0));
		CHECK(plain.lastDelaySecondsForTest(0) > 0.0);
	}
}

namespace {

/// THE OUTER LOOP'S FIRST RETURN WITH THE SMEAR ON, restated: the difference between the render with the loop and without
/// is the loop's returns, and until the second one arrives (a tuned delay later) it is the impulse delayed by the tuned
/// delay and put through the Smear twice — once as the sum goes on to the loop's write, once more before the engine's
/// output — at the loop's gain. The Smear is the shell's own, run alone.
void checkSmearedFirstReturn(Mode mode, double intervalSemitones) {
	Settings s = mode == Mode::Etch ? declared() : rustPure();
	s.mode = mode;
	s.mix = 1.0f;
	s.freq = 10.0f; // 1024 Hz: 32 frames at kLoopRate
	s.rustBias = 1.0f;
	s.smear = 0.5f;
	s.loop = 0.8f;
	s.loopInterval = static_cast<float>(intervalSemitones);
	Settings flat = s;
	flat.loop = 0.0f;
	const std::vector<float> in = impulseOf(1024);
	const std::vector<float> with = renderAt(kLoopRate, s, in);
	const std::vector<float> without = renderAt(kLoopRate, flat, in);

	const double delay = 32.0 * std::pow(2.0, -intervalSemitones / 12.0);
	const auto whole = static_cast<size_t>(std::floor(delay));
	REQUIRE(delay == static_cast<double>(whole));
	const double gain = 0.8 * kLoopMaxGain;
	// THE RETURNS BEFORE THE SECOND ARRIVES: n < 2 * delay.
	std::vector<double> want(2 * whole, 0.0);
	// Restate the chain frame by frame: the smeared impulse S, then its return delayed and smeared again.
	Smear a;
	a.prepare(kLoopRate);
	a.set({ 0.5f, 1024.0f });
	Smear b = a;
	b.reset();
	CHECK(a.delayAtCentre() > 32.0);
	std::vector<float> smeared(want.size());
	for (size_t n = 0; n < want.size(); ++n) {
		smeared[n] = a.process(0, n == 0 ? 0.5f : 0.0f);
	}
	for (size_t n = 0; n < want.size(); ++n) {
		const float returned = n >= whole ? static_cast<float>(gain) * smeared[n - whole] : 0.0f;
		want[n] = static_cast<double>(b.process(0, returned));
	}
	double worst = 0.0;
	double peak = 0.0;
	size_t firstNonzero = want.size();
	for (size_t n = 0; n < want.size(); ++n) {
		const double got = static_cast<double>(with[n]) - static_cast<double>(without[n]);
		worst = std::max(worst, std::abs(got - want[n]));
		peak = std::max(peak, std::abs(got));
		if (got != 0.0 && firstNonzero == want.size()) {
			firstNonzero = n;
		}
	}
	measured("smeared first return, worst deviation from the restatement", worst);
	CHECK(worst < 1.0e-5);
	CHECK(peak > 1.0e-3); // NON-VACUITY: a return is there
	CHECK(firstNonzero >= whole); // never earlier than the tuned delay
	// SMEARED: the return is spread, not an impulse of the loop's gain.
	CHECK(peak < 0.5 * gain * 0.9);
}

} // namespace

TEST_CASE("Smear in the loop: the first return is the tuned delay later and smeared, and Interval moves it by the restated difference") {
	for (const Mode mode : { Mode::Rust, Mode::Etch }) {
		for (const double interval : { 0.0, 12.0, -12.0 }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(interval);
			checkSmearedFirstReturn(mode, interval);
		}
	}
}

TEST_CASE("Rust's inner loop: a comb at Freq — an impulse returns after the period at the gain, tracked or not") {
	for (const bool track : { false, true }) {
		CAPTURE(track);
		Settings s = rustPure();
		s.resonance = 0.6f;
		s.track = track;
		const double fb = 0.6 * kDustFeedbackCeiling;
		const double wetGain = std::sqrt(1.0 - fb * fb);
		const size_t period = track ? 16u : 32u; // an octave up the keyboard halves the period
		const device::NoteContext note { track ? 72.0f : 60.0f, true };
		const std::vector<float> out = renderAt(kLoopRate, s, impulseOf(1024), 256, &note);
		for (size_t n = 0; n < 12 * period; ++n) {
			const bool on = n % period == 0;
			const double want = on ? 0.5 * wetGain * std::pow(fb, static_cast<double>(n / period)) : 0.0;
			CAPTURE(n);
			CHECK(std::abs(static_cast<double>(out[n]) - want) < 1.0e-6);
		}
	}
	// A NEGATIVE RESONANCE INVERTS EACH RETURN.
	Settings s = rustPure();
	s.resonance = -0.5f;
	const std::vector<float> out = renderAt(kLoopRate, s, impulseOf(256));
	const double fb = 0.5 * kDustFeedbackCeiling;
	CHECK(std::abs(static_cast<double>(out[32]) + 0.5 * std::sqrt(1.0 - fb * fb) * fb) < 1.0e-6);
	// AND AT NO RESONANCE THERE IS NO LOOP: the delay is not written, and the output is the input.
	s.resonance = 0.0f;
	const std::vector<float> plain = renderAt(kLoopRate, s, impulseOf(256));
	for (size_t n = 1; n < 256; ++n) {
		CHECK(plain[n] == 0.0f);
	}
	CHECK(plain[0] == 0.5f);
}

TEST_CASE("Outer loop: an impulse returns after the period times 2^(-interval / 12) at the restated gain, at every Interval, tracked or not") {
	for (const bool track : { false, true }) {
		for (const float interval : { 0.0f, 7.0f, -12.0f }) {
			CAPTURE(track);
			CAPTURE(interval);
			Settings s = rustPure();
			s.loop = 0.8f;
			s.loopInterval = interval;
			s.track = track;
			const device::NoteContext note { track ? 72.0f : 60.0f, true };
			const double period = track ? 16.0 : 32.0;
			const double delay = period * std::pow(2.0, -static_cast<double>(interval) / 12.0);
			const double gain = 0.8 * kLoopMaxGain; // no inner loop, no Drive: nothing to divide by
			const std::vector<float> out = renderAt(kLoopRate, s, impulseOf(2048), 256, &note);
			// THE RETURN LANDS BETWEEN TWO FRAMES BY THE FRACTION; each time round it comes back again.
			std::vector<double> want(out.size(), 0.0);
			want[0] = 0.5;
			for (size_t n = 0; n + 1 < want.size(); ++n) {
				if (want[n] == 0.0) {
					continue;
				}
				// A RETURN IS (1 - f) AT floor(delay) AND f AT THE FRAME AFTER
				const double whole = std::floor(delay);
				const double frac = delay - whole;
				if (n + static_cast<size_t>(whole) + 1 < want.size()) {
					want[n + static_cast<size_t>(whole)] += want[n] * gain * (1.0 - frac);
					want[n + static_cast<size_t>(whole) + 1] += want[n] * gain * frac;
				}
			}
			// ONLY THE FIRST RETURN IS A CLEAN STATEMENT (the second is the first's own spread): check up to it.
			const auto first = static_cast<size_t>(std::floor(delay));
			CHECK(std::abs(static_cast<double>(out[first]) - 0.5 * gain * (1.0 - (delay - std::floor(delay)))) < 1.0e-6);
			CHECK(std::abs(static_cast<double>(out[first + 1]) - 0.5 * gain * (delay - std::floor(delay))) < 1.0e-6);
			for (size_t n = 1; n < first; ++n) {
				CHECK(out[n] == 0.0f);
			}
			// AND THE WHOLE RUN FOLLOWS THE RESTATED RECURRENCE.
			double worst = 0.0;
			for (size_t n = 0; n < 600; ++n) {
				worst = std::max(worst, std::abs(static_cast<double>(out[n]) - want[n]));
			}
			CHECK(worst < 1.0e-5);
		}
	}
	// A DUST WITH THE HEAD AT REST ADDS ITS OWN TWO FRAMES: the impulse is read at the head's margin, taken after the shaper,
	// returned a period later, and read at the margin again — returns at 2, 2 + (32 + 2), …, each at the gain.
	Settings d = dustStill();
	d.freq = 10.0f;
	d.mix = 1.0f;
	d.loop = 0.7f;
	const std::vector<float> dout = renderAt(kLoopRate, d, impulseOf(1024));
	for (size_t k = 0; k < 6; ++k) {
		CAPTURE(k);
		CHECK(std::abs(static_cast<double>(dout[2 + 34 * k]) - 0.5 * std::pow(0.7 * kLoopMaxGain, static_cast<double>(k))) < 1.0e-6);
	}
}

TEST_CASE("Outer loop: the limiter holds what is fed back at or under full scale with Drive hard and Loop at the top") {
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		Settings s = declared();
		s.mode = mode;
		s.mix = 1.0f;
		s.freq = hertzToLog2(800.0);
		s.drive = 0.3f;
		s.driveMode = DriveMode::Decimate; // a gain of one: nothing divides the loop, so the limiter is what holds it
		s.loop = 1.0f;
		const std::vector<float> in = whiteInput(24000, 0.9f);
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		float worst = 0.0f;
		uint64_t limited = 0;
		for (size_t done = 0; done < l.size(); done += 64) {
			e.processStereo(l.data() + done, r.data() + done, 64);
			const SharedSection& sh = mode == Mode::Comb ? e.combSharedForTest() : e.dustForTest().sharedForTest();
			worst = std::max(worst, sh.lastOuterPeakForTest());
			limited = sh.limitedFramesForTest();
		}
		const float threshold = mode == Mode::Comb ? 1.0f / static_cast<float>(kHeadroom) : 1.0f;
		measured("outer limiter frames acted on", static_cast<double>(limited));
		CHECK(worst <= threshold * 1.000001f);
		CHECK(limited > 100); // NON-VACUITY: the limiter acted
		CHECK(peakOf(l) < 16.0f);
	}
}

TEST_CASE("In Loop: it differs from off only with an emphasis and a loop; on, a +12 dB tilt and Loop at the top stay bounded and the loop gain is as set, nothing divided out") {
	const std::vector<float> in = whiteInput(24000, 0.6f);
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		Settings s = declared();
		s.mode = mode;
		s.mix = 0.7f;
		s.freq = hertzToLog2(800.0);
		s.resonance = 0.5f;
		s.drive = 0.5f;
		const auto same = [&](Settings a) {
			Settings b = a;
			b.emphInLoop = true;
			return renderAt(kRate, a, in) == renderAt(kRate, b, in);
		};
		// NO EMPHASIS, OR NO LOOP: In Loop has nothing to move.
		Settings noEmph = s;
		noEmph.loop = 1.0f;
		CHECK(same(noEmph));
		Settings noLoop = s;
		noLoop.emphAmount = 6.0f;
		noLoop.resonance = 0.0f;
		if (mode != Mode::Comb) {
			CHECK(same(noLoop)); // the Comb's own loop is always there, so for it an emphasis alone is a loop
		}
		// BOTH: it differs (for the Comb its own loop is the inner one, so an emphasis alone is enough).
		Settings both = s;
		both.emphAmount = 6.0f;
		both.loop = 0.6f;
		CHECK_FALSE(same(both));

		// +12 dB, LOOP AT THE TOP, 2 s of noise: bounded.
		Settings hot = s;
		hot.emphAmount = 12.0f;
		hot.emphInLoop = true;
		hot.loop = 1.0f;
		hot.tapDriven = true;
		const std::vector<float> noise = whiteInput(96000, 0.8f);
		const std::vector<float> out = renderAt(kRate, hot, noise);
		measured("in loop +12 dB, loop 1, peak over 2 s", peakOf(out));
		CHECK(peakOf(out) < 6.0f);
		CHECK(peakOf(out) > 0.05f);

		// NOTHING IS DIVIDED OUT: the inner gain is as set, the outer is Loop times 0.98, with a Driven tap and +12 dB in the loop.
		Eroder e;
		e.prepare(kRate);
		e.setTarget(hot);
		std::vector<float> l = in;
		std::vector<float> r = in;
		e.processStereo(l.data(), r.data(), 4800);
		const LoopDerived& loops = mode == Mode::Comb ? e.combLoopsForTest() : e.dustForTest().derivedForTest().loops;
		const SharedSection& sh = mode == Mode::Comb ? e.combSharedForTest() : e.dustForTest().sharedForTest();
		const double rawInner = mode == Mode::Comb ? e.derivedForTest().feedback : 0.5 * kDustFeedbackCeiling;
		CHECK(std::abs(loops.innerFeedback - rawInner) < 1.0e-9);
		CHECK(std::abs(loops.loopGain - kLoopMaxGain) < 1.0e-9);
		CHECK(std::abs(static_cast<double>(sh.outerGainForTest()) - kLoopMaxGain) < 1.0e-6);
	}
}

TEST_CASE("Everything on, in every engine and four Types: bounded, the same at any partition while a control travels, and the tail ends in exact silence") {
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		for (const DriveMode type : { DriveMode::Soft, DriveMode::Fold, DriveMode::Wreck, DriveMode::Slew }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(static_cast<int>(type));
			const Settings on = everythingOn(mode, type);
			Settings quieter = on;
			quieter.loop = 0.4f;
			quieter.smear = 0.3f;
			quieter.emphAmount = -8.0f;
			const std::vector<float> noise = whiteInput(24000, 0.9f);
			const size_t total = 24000 + 3 * 48000;
			const auto render = [&](int block) {
				Eroder e;
				e.prepare(kRate);
				e.setTarget(on);
				std::vector<float> l = noise;
				l.resize(total, 0.0f);
				std::vector<float> r = l;
				// THE CONTROLS MOVE AT FIXED FRAMES, so the render is the same whatever the blocks.
				const size_t switchAt = 8000;
				size_t done = 0;
				while (done < total) {
					size_t take = std::min<size_t>(total - done, static_cast<size_t>(block));
					if (done < switchAt) {
						take = std::min(take, switchAt - done);
					}
					if (done == switchAt) {
						e.setTarget(quieter);
					}
					e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
					done += take;
				}
				return l;
			};
			const std::vector<float> whole = render(4096);
			CHECK(render(1) == whole);
			CHECK(render(37) == whole);
			float peak = 0.0f;
			for (const float v : whole) {
				REQUIRE(std::isfinite(v));
				peak = std::max(peak, std::abs(v));
			}
			CHECK(peak < 8.0f);
			CHECK(peak > 0.05f);
			bool silent = true;
			for (size_t i = whole.size() - 24000; i < whole.size(); ++i) {
				silent = silent && whole[i] == 0.0f;
			}
			CHECK(silent);
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// ETCH: TWO SLOTS, EACH A DRIVE STAGE, IN THE SHARED SECTION.                                         //
//--------------------------------------------------------------------------------------------------//

namespace {

struct StereoBuffer {
	std::vector<float> l;
	std::vector<float> r;
	friend bool operator==(const StereoBuffer&, const StereoBuffer&) = default;
};

Settings etchBase() {
	Settings s = declared();
	s.mode = Mode::Etch;
	s.mix = 1.0f;
	s.freq = 10.0f; // 1024 Hz: 32 frames at kLoopRate
	return s;
}

StereoBuffer renderStereo(double rate, const Settings& s, const StereoBuffer& in, int block = 256,
                          const device::NoteContext* note = nullptr, Eroder* keep = nullptr) {
	Eroder local;
	Eroder& e = keep != nullptr ? *keep : local;
	e.prepare(rate);
	if (note != nullptr) {
		e.setNoteContext(*note);
	}
	e.setTarget(s);
	StereoBuffer out = in;
	for (size_t done = 0; done < out.l.size();) {
		const size_t take = std::min<size_t>(out.l.size() - done, static_cast<size_t>(block));
		e.processStereo(out.l.data() + done, out.r.data() + done, static_cast<int>(take));
		done += take;
	}
	return out;
}

StereoBuffer noiseStereo(size_t frames, float amp) {
	return { whiteInput(frames, amp, 0x1234567u), whiteInput(frames, amp, 0x7654321u) };
}

/// A STAGE AS A SLOT RESTATES IT: the Drive's own derivation at the slot's four controls.
DriveStage stageOf(const SlotSettings& x, double rate, float mix) {
	DriveStage st;
	st.prepare(rate);
	st.set(deriveDrive(x.amount, x.type, x.shape, rate, nullptr, mix));
	return st;
}

/// ONE STAGE, RUN ALONE over a stereo buffer: what a slot at Mix 1 must be, sample for sample.
StereoBuffer throughStage(DriveStage& st, const StereoBuffer& in) {
	StereoBuffer out = in;
	for (size_t i = 0; i < out.l.size(); ++i) {
		st.process(out.l[i], out.r[i]);
	}
	return out;
}

} // namespace

TEST_CASE("Etch: nine declared controls, gated to it, with Freq, Width, Resonance and Track live in it; stored by their keys") {
	struct Row {
		Param param;
		const char* key;
		double def;
		bool modulatable;
	};
	const Row rows[] = {
		{ Param::SlotAAmount, "eroder_slot_a_amount", 0.0, true }, { Param::SlotAType, "eroder_slot_a_type", static_cast<double>(DriveMode::Slew), false },
		{ Param::SlotAShape, "eroder_slot_a_shape", 0.0, true },   { Param::SlotAMix, "eroder_slot_a_mix", 1.0, true },
		{ Param::SlotBAmount, "eroder_slot_b_amount", 0.0, true }, { Param::SlotBType, "eroder_slot_b_type", static_cast<double>(DriveMode::Decimate), false },
		{ Param::SlotBShape, "eroder_slot_b_shape", 0.0, true },   { Param::SlotBMix, "eroder_slot_b_mix", 1.0, true },
		{ Param::SlotRoute, "eroder_slot_route", 0.0, false },
	};
	for (const Row& r : rows) {
		const auto& d = kParamDescs[static_cast<size_t>(r.param)];
		CAPTURE(r.key);
		CHECK(std::string_view(d.key) == r.key);
		CHECK(d.def == r.def);
		CHECK(d.modulatable == r.modulatable);
		CHECK(d.gateControl == static_cast<int16_t>(Param::Mode));
		CHECK(d.gateMask == (1u << static_cast<uint32_t>(Mode::Etch)));
	}
	CHECK(std::string_view(kParamDescs[static_cast<size_t>(Param::SlotAType)].names[static_cast<size_t>(DriveMode::Slew)]) == "Slew");
	CHECK(std::string_view(kParamDescs[static_cast<size_t>(Param::SlotBType)].names[static_cast<size_t>(DriveMode::Decimate)]) == "Decimate");
	CHECK(kParamDescs[static_cast<size_t>(Param::SlotAType)].names.size() == static_cast<size_t>(DriveMode::kCount));
	CHECK(kParamDescs[static_cast<size_t>(Param::SlotRoute)].names.size() == 2);
	CHECK(std::string_view(kParamDescs[static_cast<size_t>(Param::SlotRoute)].names[1]) == "Parallel");
	CHECK(std::string_view(kModeNames[static_cast<size_t>(Mode::Etch)]) == "Etch");
	CHECK(static_cast<size_t>(Mode::kCount) == 4);
	// AND THE SHAKE'S OWN CONTROLS ARE NOT ETCH'S.
	for (const Param p : { Param::Amount, Param::Source, Param::Grit, Param::Stereo, Param::Step, Param::Detune,
	                       Param::ModFold, Param::RustBias, Param::RustRectify }) {
		CHECK((kParamDescs[static_cast<size_t>(p)].gateMask & (1u << static_cast<uint32_t>(Mode::Etch))) == 0u);
	}
	// THE CODEC CARRIES THEM; a row from before them reads the defaults.
	State st;
	st.settings.mode = Mode::Etch;
	st.settings.slotA = { 0.7f, DriveMode::Fold, 0.3f, 0.6f };
	st.settings.slotB = { 0.2f, DriveMode::Lopside, 0.9f, 0.4f };
	st.settings.slotParallel = true;
	CHECK(stateOf(rowOf(st), static_cast<size_t>(Param::kCount)) == st);
	const State old = stateOf(rowOf(st), static_cast<size_t>(Param::SlotAAmount));
	CHECK(old.settings.slotA == SlotSettings {});
	CHECK(old.settings.slotB == SlotSettings { 0.0f, DriveMode::Decimate, 0.0f, 1.0f });
	CHECK_FALSE(old.settings.slotParallel);
	CHECK(old.settings.mode == Mode::Etch);
}

TEST_CASE("Etch at rest is a true bypass: bit for bit the input at Mix 0, 0.37 and 1, stereo, at any partition") {
	const StereoBuffer in = noiseStereo(9600, 0.7f);
	for (const float mix : { 0.0f, 0.37f, 1.0f }) {
		for (const int variant : { 0, 1, 2 }) {
			CAPTURE(mix);
			CAPTURE(variant);
			Settings s = etchBase();
			s.mix = mix;
			if (variant == 1) {
				// A SLOT WITH NO AMOUNT, THE OTHER WITH NO MIX: both skipped.
				s.slotA = { 0.0f, DriveMode::Fold, 0.5f, 1.0f };
				s.slotB = { 0.9f, DriveMode::Crush, 0.5f, 0.0f };
			}
			if (variant == 2) {
				s.slotParallel = true;
				s.follow = 1.0f;
				s.track = true;
				s.width = 0.2f;
			}
			for (const int block : { 1, 37, 4096 }) {
				CAPTURE(block);
				Eroder e;
				const StereoBuffer out = renderStereo(kRate, s, in, block, nullptr, &e);
				const bool same = out == in;
				CHECK(same);
				CHECK_FALSE(e.dustForTest().slotForTest(0).active());
				CHECK_FALSE(e.dustForTest().slotForTest(1).active());
			}
		}
	}
	// NON-VACUITY: one slot up, and it is in the sound at Mix 0.37 (the bypass is not what every setting does).
	Settings s = etchBase();
	s.mix = 0.37f;
	s.slotA = { 0.8f, DriveMode::Fold, 0.5f, 1.0f };
	CHECK_FALSE(renderStereo(kRate, s, in) == in);
}

TEST_CASE("Etch: a single slot is a DriveStage run alone, sample for sample — a curve, Slew, Decimate and Crush, in either slot") {
	const StereoBuffer in = noiseStereo(9600, 0.6f);
	for (const DriveMode type : { DriveMode::Fold, DriveMode::Slew, DriveMode::Decimate, DriveMode::Crush }) {
		for (const bool second : { false, true }) {
			CAPTURE(static_cast<int>(type));
			CAPTURE(second);
			Settings s = etchBase();
			const SlotSettings slot { 0.6f, type, 0.4f, 1.0f };
			(second ? s.slotB : s.slotA) = slot;
			DriveStage alone = stageOf(slot, kRate, 1.0f);
			const StereoBuffer want = throughStage(alone, in);
			Eroder e;
			const StereoBuffer got = renderStereo(kRate, s, in, 256, nullptr, &e);
			const bool same = got == want;
			CHECK(same);
			// NON-VACUITY: the stage did something, and the seam says the slot ran and the other did not.
			CHECK_FALSE(want == in);
			CHECK(e.dustForTest().slotForTest(second ? 1 : 0).active());
			CHECK_FALSE(e.dustForTest().slotForTest(second ? 0 : 1).active());
			const bool sameByOnes = renderStereo(kRate, s, in, 1) == want;
			CHECK(sameByOnes);
		}
	}
	// AMOUNT 0 AND MIX 0 EACH SKIP THE SLOT: the seam says it did not run and the bits are the input's.
	for (const bool noAmount : { true, false }) {
		Settings s = etchBase();
		s.slotA = { noAmount ? 0.0f : 0.8f, DriveMode::Fold, 0.4f, noAmount ? 1.0f : 0.0f };
		Eroder e;
		const bool same = renderStereo(kRate, s, in, 256, nullptr, &e) == in;
		CHECK(same);
		CHECK_FALSE(e.dustForTest().slotForTest(0).active());
	}
	// AND A SKIPPED SLOT LEAVES THE OTHER'S OUTPUT EXACTLY, in either route.
	for (const bool parallel : { false, true }) {
		Settings s = etchBase();
		s.slotParallel = parallel;
		s.slotA = { 0.0f, DriveMode::Fold, 0.4f, 1.0f };
		s.slotB = { 0.7f, DriveMode::Slew, 0.4f, 1.0f };
		DriveStage alone = stageOf(s.slotB, kRate, 1.0f);
		const bool same = renderStereo(kRate, s, in) == throughStage(alone, in);
		CHECK(same);
		s.slotA = { 0.7f, DriveMode::Slew, 0.4f, 1.0f };
		s.slotB = { 0.7f, DriveMode::Fold, 0.4f, 0.0f };
		DriveStage aloneA = stageOf(s.slotA, kRate, 1.0f);
		const bool sameA = renderStereo(kRate, s, in) == throughStage(aloneA, in);
		CHECK(sameA);
	}
}

TEST_CASE("Etch: Series is B of A of the input, Parallel is the input plus what each slot changed; Slot Mix is the restated blend") {
	const StereoBuffer in = noiseStereo(9600, 0.6f);
	Settings s = etchBase();
	s.slotA = { 0.7f, DriveMode::Fold, 0.3f, 1.0f };
	s.slotB = { 0.5f, DriveMode::Slew, 0.4f, 1.0f };

	// SERIES: A, then B on what A made — two stages chained by hand.
	{
		DriveStage a = stageOf(s.slotA, kRate, 1.0f);
		DriveStage b = stageOf(s.slotB, kRate, 1.0f);
		StereoBuffer want = throughStage(a, in);
		want = throughStage(b, want);
		const bool same = renderStereo(kRate, s, in) == want;
		CHECK(same);
		// NON-VACUITY: B alone on the input is not that.
		DriveStage alone = stageOf(s.slotB, kRate, 1.0f);
		CHECK_FALSE(throughStage(alone, in) == want);
	}
	// PARALLEL: both read the input; out = x + (A(x) - x) + (B(x) - x).
	{
		s.slotParallel = true;
		DriveStage a = stageOf(s.slotA, kRate, 1.0f);
		DriveStage b = stageOf(s.slotB, kRate, 1.0f);
		const StereoBuffer ay = throughStage(a, in);
		const StereoBuffer by = throughStage(b, in);
		const StereoBuffer got = renderStereo(kRate, s, in);
		double worst = 0.0;
		for (size_t i = 0; i < in.l.size(); ++i) {
			const double wl = static_cast<double>(in.l[i]) + (static_cast<double>(ay.l[i]) - in.l[i]) + (static_cast<double>(by.l[i]) - in.l[i]);
			const double wr = static_cast<double>(in.r[i]) + (static_cast<double>(ay.r[i]) - in.r[i]) + (static_cast<double>(by.r[i]) - in.r[i]);
			worst = std::max({ worst, std::abs(wl - got.l[i]), std::abs(wr - got.r[i]) });
		}
		measured("etch parallel against the restated sum, worst", worst);
		CHECK(worst < 1.0e-6);
		// NON-VACUITY, AND NOT AN AVERAGE: the sum of the two changes is further from the input than either's.
		double sumDev = 0.0;
		double avgDev = 0.0;
		for (size_t i = 0; i < in.l.size(); ++i) {
			sumDev = std::max(sumDev, std::abs(static_cast<double>(got.l[i]) - in.l[i]));
			avgDev = std::max(avgDev, std::abs(0.5 * (static_cast<double>(ay.l[i]) + by.l[i]) - in.l[i]));
		}
		CHECK(sumDev > 1.2 * avgDev);
	}
	// SLOT MIX 0.5: the dry and the stage's output half and half.
	{
		Settings m = etchBase();
		m.slotA = { 0.7f, DriveMode::Fold, 0.3f, 0.5f };
		DriveStage full = stageOf(m.slotA, kRate, 1.0f);
		const StereoBuffer processed = throughStage(full, in);
		const StereoBuffer got = renderStereo(kRate, m, in);
		double worst = 0.0;
		double distance = 0.0;
		for (size_t i = 0; i < in.l.size(); ++i) {
			const double want = 0.5 * static_cast<double>(in.l[i]) + 0.5 * static_cast<double>(processed.l[i]);
			worst = std::max(worst, std::abs(want - got.l[i]));
			distance = std::max(distance, std::abs(static_cast<double>(processed.l[i]) - got.l[i]));
		}
		CHECK(worst < 1.0e-6);
		CHECK(distance > 0.01);
	}
}

TEST_CASE("Etch: Follow scales both slots' mix by the follower's gain; at none of it the mix is the control's") {
	// A QUIET, STEADY INPUT: the follower settles far under the full mark.
	StereoBuffer quiet;
	quiet.l.assign(48000, 1.0e-3f);
	quiet.r = quiet.l;
	Settings s = etchBase();
	s.slotA = { 0.7f, DriveMode::Fold, 0.3f, 0.8f };
	s.slotB = { 0.5f, DriveMode::Slew, 0.4f, 0.6f };
	s.slotParallel = true;
	for (const float follow : { 0.0f, 1.0f, 0.5f }) {
		CAPTURE(follow);
		s.follow = follow;
		Eroder e;
		renderStereo(kRate, s, quiet, 256, nullptr, &e);
		const Dust& dust = e.dustForTest();
		const float gain = dust.followGainForTest();
		// THE FOLLOWER'S GAIN, restated: (1 - f) + f * (envelope / full mark), the envelope the follower settled to.
		const float restated = followGain(follow, e.envelopeForTest());
		CHECK(std::abs(gain - restated) < 2.0e-5f);
		CHECK(dust.slotForTest(0).settings().mix == doctest::Approx(0.8f * gain).epsilon(1.0e-6));
		CHECK(dust.slotForTest(1).settings().mix == doctest::Approx(0.6f * gain).epsilon(1.0e-6));
		if (follow == 0.0f) {
			CHECK(gain == 1.0f);
			CHECK(dust.slotForTest(0).settings().mix == 0.8f);
			CHECK(dust.slotForTest(1).settings().mix == 0.6f);
		}
		else if (follow == 1.0f) {
			// FAR UNDER THE FULL MARK: the slots are nearly out of the sound.
			CHECK(gain < 0.2f);
		}
	}
	// A LOUD INPUT REACHES THE FULL MARK: the mix is the control's again.
	StereoBuffer loud = noiseStereo(48000, 0.95f);
	s.follow = 1.0f;
	Eroder e;
	renderStereo(kRate, s, loud, 256, nullptr, &e);
	CHECK(e.dustForTest().followGainForTest() > 0.5f);
	CHECK(std::abs(e.dustForTest().followGainForTest() - followGain(1.0f, e.envelopeForTest())) < 0.5f);
	// AND THE FOLLOWER'S GAIN IS LATCHED ON THE GRID: the same sound at any partition, with the input's level moving.
	StereoBuffer moving = noiseStereo(24000, 0.9f);
	for (size_t i = 0; i < moving.l.size(); ++i) {
		const float level = 0.02f + 0.9f * static_cast<float>(0.5 + 0.5 * std::sin(0.0007 * static_cast<double>(i)));
		moving.l[i] *= level;
		moving.r[i] *= level;
	}
	const StereoBuffer whole = renderStereo(kRate, s, moving, 4096);
	const bool byOnes = renderStereo(kRate, s, moving, 1) == whole;
	const bool byOdd = renderStereo(kRate, s, moving, 37) == whole;
	const bool byPrime = renderStereo(kRate, s, moving, 101) == whole;
	CHECK(byOnes);
	CHECK(byOdd);
	CHECK(byPrime);
	// NON-VACUITY: Follow is in the sound.
	Settings none = s;
	none.follow = 0.0f;
	CHECK_FALSE(renderStereo(kRate, none, moving, 4096) == whole);
}

namespace {

/// THE INNER LOOP OF ETCH, RESTATED over a mono impulse: the sum takes the tap from a period ago at the gain, the slot and
/// then the Drive run on it, the tap is after the slot (Clean) or the Drive (Driven), and the output is the wet at the level
/// the loop's gain leaves. No limiter: nothing here goes over full scale.
std::vector<float> restatedInner(const Settings& s, double feedback, size_t period, bool driven, const std::vector<float>& in) {
	DriveStage slot = stageOf(s.slotA, kLoopRate, 1.0f);
	DriveStage drive;
	drive.prepare(kLoopRate);
	drive.set(deriveDrive(s.drive, s.driveMode, s.driveShape, kLoopRate));
	const double wetGain = std::sqrt(1.0 - feedback * feedback);
	std::vector<float> taps(in.size(), 0.0f);
	std::vector<float> out(in.size(), 0.0f);
	for (size_t n = 0; n < in.size(); ++n) {
		const float back = n >= period ? taps[n - period] : 0.0f;
		float v = in[n] + back * static_cast<float>(feedback);
		float v2 = v;
		slot.process(v, v2);
		float tap = v;
		float w2 = v;
		drive.process(v, w2);
		if (driven) {
			tap = v;
		}
		taps[n] = std::abs(tap) < 1.0e-9f ? 0.0f : tap;
		out[n] = static_cast<float>(static_cast<double>(v) * wetGain);
	}
	return out;
}

} // namespace

TEST_CASE("Etch's loops: the inner loop (Clean after the slots, Driven after the Drive) and the outer one return at the restated period and gain; Track moves both") {
	// NO SLOT, NO DRIVE: Etch is a comb at Freq, returning after the period at the gain, tracked or not.
	for (const bool track : { false, true }) {
		CAPTURE(track);
		Settings s = etchBase();
		s.resonance = 0.6f;
		s.track = track;
		const double fb = 0.6 * kDustFeedbackCeiling;
		const double wetGain = std::sqrt(1.0 - fb * fb);
		const size_t period = track ? 16u : 32u;
		const device::NoteContext note { track ? 72.0f : 60.0f, true };
		const std::vector<float> out = renderAt(kLoopRate, s, impulseOf(1024), 256, &note);
		for (size_t n = 0; n < 12 * period; ++n) {
			const bool on = n % period == 0;
			const double want = on ? 0.5 * wetGain * std::pow(fb, static_cast<double>(n / period)) : 0.0;
			CAPTURE(n);
			CHECK(std::abs(static_cast<double>(out[n]) - want) < 1.0e-6);
		}
	}
	// WITH A SLOT AND A DRIVE IN THE LOOP, Clean and Driven, against the restated recurrence.
	for (const bool driven : { false, true }) {
		CAPTURE(driven);
		Settings s = etchBase();
		s.resonance = 0.6f;
		s.tapDriven = driven;
		s.slotA = { 0.3f, DriveMode::Fold, 0.2f, 1.0f };
		s.drive = 0.3f;
		s.driveMode = DriveMode::Decimate; // a gain of one: nothing to divide, so the restatement is the plain recurrence
		// THE SLOTS' SMALL-SIGNAL GAIN IS DIVIDED OUT OF THE LOOP: the seam's divisor, checked on its own in the case below.
		const double fb = 0.6 * kDustFeedbackCeiling;
		const std::vector<float> in = impulseOf(2048);
		const std::vector<float> out = renderAt(kLoopRate, s, in);
		const std::vector<float> want = restatedInner(s, fb, 32, driven, in);
		double worst = 0.0;
		for (size_t n = 0; n < out.size(); ++n) {
			worst = std::max(worst, std::abs(static_cast<double>(out[n]) - want[n]));
		}
		measured("etch inner loop against the restated recurrence, worst", worst);
		CHECK(worst < 1.0e-5);
		// NON-VACUITY: the loop returned, and the two taps differ (the slot is not what the Drive makes of it).
		CHECK(std::abs(out[32]) > 1.0e-4f);
	}
	{
		Settings a = etchBase();
		a.resonance = 0.6f;
		a.slotA = { 0.3f, DriveMode::Fold, 0.2f, 1.0f };
		a.drive = 0.3f;
		a.driveMode = DriveMode::Decimate;
		a.driveShape = 0.0f;
		Settings b = a;
		b.drive = 0.9f;
		b.tapDriven = true;
		CHECK_FALSE(renderAt(kLoopRate, a, impulseOf(2048)) == renderAt(kLoopRate, b, impulseOf(2048)));
	}
	// THE OUTER LOOP: the period times 2^(-interval / 12), at the restated gain, tracked or not.
	for (const bool track : { false, true }) {
		for (const float interval : { 0.0f, 7.0f, -12.0f }) {
			CAPTURE(track);
			CAPTURE(interval);
			Settings s = etchBase();
			s.loop = 0.8f;
			s.loopInterval = interval;
			s.track = track;
			const device::NoteContext note { track ? 72.0f : 60.0f, true };
			const double period = track ? 16.0 : 32.0;
			const double delay = period * std::pow(2.0, -static_cast<double>(interval) / 12.0);
			const double gain = 0.8 * kLoopMaxGain;
			const std::vector<float> out = renderAt(kLoopRate, s, impulseOf(2048), 256, &note);
			const auto first = static_cast<size_t>(std::floor(delay));
			const double frac = delay - std::floor(delay);
			CHECK(std::abs(static_cast<double>(out[first]) - 0.5 * gain * (1.0 - frac)) < 1.0e-6);
			CHECK(std::abs(static_cast<double>(out[first + 1]) - 0.5 * gain * frac) < 1.0e-6);
			CHECK(out[0] == 0.5f);
			for (size_t n = 1; n < first; ++n) {
				CHECK(out[n] == 0.0f);
			}
		}
	}
}

TEST_CASE("Etch with everything on — both slots hard, emphasis either way, Smear, Tap Driven, Resonance, Loop, In Loop, Drive hard: bounded, the same at any partition, silent after") {
	struct Pair {
		DriveMode a;
		DriveMode b;
	};
	float highest = 0.0f;
	for (const Pair pair : { Pair { DriveMode::Fold, DriveMode::Slew }, Pair { DriveMode::Wreck, DriveMode::Crush } }) {
		for (const float emph : { 12.0f, -12.0f }) {
			for (const bool parallel : { false, true }) {
				CAPTURE(static_cast<int>(pair.a));
				CAPTURE(emph);
				CAPTURE(parallel);
				Settings on = everythingOn(Mode::Etch, DriveMode::Wreck);
				on.emphAmount = emph;
				on.slotA = { 0.9f, pair.a, 0.6f, 1.0f };
				on.slotB = { 0.9f, pair.b, 0.6f, 1.0f };
				on.slotParallel = parallel;
				on.resonance = 0.9f;
				
				on.follow = 0.3f;
				Settings later = on;
				later.slotA.amount = 0.4f;
				later.loop = 0.5f;
				later.smear = 0.4f;
				const StereoBuffer noise = noiseStereo(24000, 0.9f);
				const size_t total = 24000 + 3 * 48000;
				const auto render = [&](int block) {
					Eroder e;
					e.prepare(kRate);
					e.setTarget(on);
					StereoBuffer b = noise;
					b.l.resize(total, 0.0f);
					b.r.resize(total, 0.0f);
					const size_t switchAt = 8000;
					size_t done = 0;
					while (done < total) {
						size_t take = std::min<size_t>(total - done, static_cast<size_t>(block));
						if (done < switchAt) {
							take = std::min(take, switchAt - done);
						}
						if (done == switchAt) {
							e.setTarget(later);
						}
						e.processStereo(b.l.data() + done, b.r.data() + done, static_cast<int>(take));
						done += take;
					}
					return b;
				};
				const StereoBuffer whole = render(4096);
				const bool byOnes = render(1) == whole;
				const bool byOdd = render(37) == whole;
				CHECK(byOnes);
				CHECK(byOdd);
				float peak = 0.0f;
				for (size_t i = 0; i < whole.l.size(); ++i) {
					REQUIRE(std::isfinite(whole.l[i]));
					REQUIRE(std::isfinite(whole.r[i]));
					peak = std::max({ peak, std::abs(whole.l[i]), std::abs(whole.r[i]) });
				}
				highest = std::max(highest, peak);
				CHECK(peak < 5.0f);
				CHECK(peak > 0.05f);
				bool silent = true;
				for (size_t i = whole.l.size() - 24000; i < whole.l.size(); ++i) {
					silent = silent && whole.l[i] == 0.0f && whole.r[i] == 0.0f;
				}
				CHECK(silent);
				{
					float tailPeak = 0.0f;
					size_t lastNonzero = 0;
					for (size_t i = 0; i < whole.l.size(); ++i) {
						if (whole.l[i] != 0.0f || whole.r[i] != 0.0f) {
							lastNonzero = i;
						}
						if (i >= whole.l.size() - 24000) {
							tailPeak = std::max({ tailPeak, std::abs(whole.l[i]), std::abs(whole.r[i]) });
						}
					}
					measured("etch tail peak", tailPeak);
					measured("etch last nonzero frame", static_cast<double>(lastNonzero));
				}
			}
		}
	}
	measured("etch everything on, highest peak over the eight renders", highest);
}

TEST_CASE("Etch: the inner loop is limited whenever a slot or the Drive runs, and what it feeds back never passes full scale") {
	const StereoBuffer noise = noiseStereo(24000, 0.9f);
	struct Case {
		const char* what;
		SlotSettings slot;
		float drive;
		bool limited;
		float resonance = 0.9f;
	};
	// THE INNER LIMITER IS ON WHENEVER THE INNER LOOP HAS A GAIN (Resonance not zero), and, at Resonance zero, when a stage runs.
	const Case cases[] = {
		{ "nothing runs, at Resonance 0", { 0.0f, DriveMode::Wreck, 0.6f, 1.0f }, 0.0f, false, 0.0f },
		{ "nothing runs but Resonance does", { 0.0f, DriveMode::Wreck, 0.6f, 1.0f }, 0.0f, true },
		{ "a slot runs", { 0.9f, DriveMode::Wreck, 0.6f, 1.0f }, 0.0f, true },
		{ "a slot at no Mix, at Resonance 0", { 0.9f, DriveMode::Wreck, 0.6f, 0.0f }, 0.0f, false, 0.0f },
		{ "the Drive runs", { 0.0f, DriveMode::Wreck, 0.6f, 1.0f }, 0.8f, true },
	};
	for (const Case& c : cases) {
		CAPTURE(c.what);
		Settings s = etchBase();
		s.resonance = c.resonance;
		s.tapDriven = false; // Clean: no Drive division
		s.slotA = c.slot;
		s.drive = c.drive;
		s.driveMode = DriveMode::Fold;
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		StereoBuffer b = noise;
		float fedBack = 0.0f;
		for (size_t done = 0; done < b.l.size(); done += 16) {
			e.processStereo(b.l.data() + done, b.r.data() + done, 16);
			fedBack = std::max(fedBack, std::abs(e.dustForTest().lastFedBackForTest()));
		}
		CHECK(e.dustForTest().derivedForTest().loops.limitInner == c.limited);
		CHECK(e.dustForTest().sharedForTest().limitInner() == c.limited);
		measured("etch inner loop, largest fed-back sample", fedBack);
		if (c.limited) {
			CHECK(fedBack <= 1.000001f);
			CHECK(fedBack > 0.5f); // NON-VACUITY: the loop carried a full-scale signal, and the limiter held it
		}
	}
}

TEST_CASE("Etch: the slots' gain is not divided out of either loop — Resonance as set, Loop times 0.98 — in Series and in Parallel") {
	for (const bool parallel : { false, true }) {
		CAPTURE(parallel);
		Settings s = everythingOn(Mode::Etch, DriveMode::Wreck);
		s.resonance = 0.9f;
		s.tapDriven = false;
		s.drive = 0.0f;
		s.emphAmount = 0.0f;
		s.slotA = { 0.9f, DriveMode::Fold, 0.6f, 1.0f };
		s.slotB = { 0.9f, DriveMode::Wreck, 0.6f, 0.7f };
		s.slotParallel = parallel;
		const double raw = static_cast<double>(0.9f) * kDustFeedbackCeiling;
		const DustDerived d = deriveDust(s, 60.0, kRate);
		CHECK(d.loops.innerFeedback == doctest::Approx(raw).epsilon(1.0e-9));
		CHECK(d.feedback == d.loops.innerFeedback);
		CHECK(d.loops.loopGain == doctest::Approx(kLoopMaxGain).epsilon(1.0e-9));
		CHECK(d.loops.limitInner);
		// NON-VACUITY: the slots really do have gain at rest, so the old law would have divided by something.
		const DriveSettings a = deriveDrive(s.slotA.amount, s.slotA.type, s.slotA.shape, kRate, nullptr, s.slotA.mix);
		CHECK(driveSmallSignalGain(a.mode, a.gain, a.shape) > 2.0);
	}
}


//--------------------------------------------------------------------------------------------------//
// HOTTER LOOPS: THE REST GAIN DIVIDED OUT, THE LIMITERS HOLD THE REST, A SILENT INPUT ENDS THE TAIL.    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// RESO 100%, LOOP 100%, TAP DRIVEN, DRIVE HIGH, for one engine and Type: the loops' small-signal gains total over one.
Settings hotLoops(Mode mode, DriveMode type, float drive = 1.0f) {
	Settings s = everythingOn(mode, type);
	s.resonance = 1.0f;
	s.loop = 1.0f;
	s.tapDriven = true;
	s.drive = drive;
	s.smear = 0.0f;
	if (mode == Mode::Etch) {
		s.slotA = { 0.9f, DriveMode::Fold, 0.6f, 1.0f };
		s.slotB = { 0.9f, DriveMode::Wreck, 0.6f, 1.0f };
	}
	return s;
}

} // namespace

TEST_CASE("Hotter loops: the outer gain is Loop times 0.98 and the inner is Resonance as set, whatever the Drive, the emphasis in the loop and the slots") {
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		Settings s = declared();
		s.mode = mode;
		s.resonance = 0.4f;
		s.loop = 1.0f;
		s.tapDriven = true;
		s.drive = 0.8f;
		s.driveMode = DriveMode::Soft;
		s.emphAmount = 6.0f;
		s.emphInLoop = true;
		if (mode == Mode::Etch) {
			s.slotA = { 0.9f, DriveMode::Fold, 0.6f, 1.0f };
		}
		const DustDerived d = deriveDust(s, 60.0, kRate);
		// RESTATED: 0.98 at Loop 1 and Resonance 0.4 of the ceiling, with a Drive that has gain, +6 dB in the loop and a slot.
		const DriveSettings drive = deriveDrive(0.8f, DriveMode::Soft, 0.0f, kRate, nullptr);
		REQUIRE(driveSmallSignalGain(drive.mode, drive.gain, drive.shape) > 1.5); // NON-VACUOUS: there was something to divide by
		CHECK(d.loops.loopGain == 0.98);
		CHECK(d.loops.innerFeedback == static_cast<double>(0.4f) * kDustFeedbackCeiling);
		CHECK(d.feedback == static_cast<double>(0.4f) * kDustFeedbackCeiling);
		s.loop = 0.5f;
		CHECK(deriveDust(s, 60.0, kRate).loops.loopGain == 0.5 * kLoopMaxGain);
		s.loop = 0.0f;
		CHECK(deriveDust(s, 60.0, kRate).loops.loopGain == 0.0);
	}
}

TEST_CASE("Hotter loops: both limiters are on whenever their loop has a gain, and at high settings they act and the output stays bounded") {
	// THE FLAGS: Resonance alone puts the inner limiter on (Clean tap, no emphasis); the outer one runs with the loop.
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Etch }) {
		Settings s = declared();
		s.mode = mode;
		CHECK_FALSE(deriveDust(s, 60.0, kRate).loops.limitInner);
		s.resonance = 0.2f;
		CHECK(deriveDust(s, 60.0, kRate).loops.limitInner);
	}
	const std::vector<float> in = sine(220.0, 48000, 0.5f);
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		// DRIVE OFF: nothing is divided out, so the loops total 0.9 + 0.98 and sit on the limiter (Etch's slots divide theirs).
		Settings s = hotLoops(mode, DriveMode::Soft, 0.0f);
		s.rustBias = 1.0f; // Rust's multiplier lets the signal through whole, so its loops are as hot as Dust's
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		float peak = 0.0f;
		const SharedSection& sh = mode == Mode::Comb ? e.combSharedForTest() : e.dustForTest().sharedForTest();
		for (size_t done = 0; done < l.size(); done += 32) {
			e.processStereo(l.data() + done, r.data() + done, 32);
		}
		for (const float v : l) {
			REQUIRE(std::isfinite(v));
			peak = std::max(peak, std::abs(v));
		}
		measured("hot loops, steady output peak on a 0.5 sine", peak);
		measured("hot loops, frames the outer limiter acted on", static_cast<double>(sh.limitedFramesForTest()));
		if (mode == Mode::Dust || mode == Mode::Rust) { // (the Comb's gain is the bank's, Etch's slots divide theirs)
			CHECK(sh.limitedFramesForTest() > 1000); // the limiter engaged: the loop sits on it
		}
		CHECK(peak < 8.0f);
		CHECK(peak > 0.05f);
	}
}

namespace {

/// THE WORST SETTINGS the loops can be given: Reso 100%, Loop 100%, Drive at maximum, an emphasis of +18 dB in the loop, either
/// Tap, a Type as the Drive and (in Etch) as both slots.
Settings worstLoops(Mode mode, DriveMode type, bool tapDriven, bool bell) {
	Settings s = hotLoops(mode, type, 1.0f);
	s.tapDriven = tapDriven;
	s.emphAmount = 18.0f;
	s.emphBell = bell;
	s.emphInLoop = true;
	s.mix = 1.0f;
	s.blend = true; // the Comb's dry is whole unless Blend: at Mix 1 with Blend the output is the wet alone
	s.rustBias = 1.0f;
	if (mode == Mode::Etch) {
		s.slotA = { 1.0f, type, 0.6f, 1.0f };
		s.slotB = { 1.0f, type, 0.6f, 1.0f };
	}
	return s;
}

constexpr DriveMode kEveryType[] = { DriveMode::Soft,  DriveMode::Tube,  DriveMode::Fold,     DriveMode::Wreck,
	                                 DriveMode::Jag,   DriveMode::Lean,  DriveMode::Chonk,   DriveMode::PaunchS,
	                                 DriveMode::PaunchH,  DriveMode::PaunchOS, DriveMode::Crush,   DriveMode::Decimate, DriveMode::Slew,
	                                 DriveMode::Lopside };

} // namespace

TEST_CASE("Hotter loops: at the worst settings — every engine, every Type as the Drive and as both slots, both Taps, a +18 dB Tilt and Bell in the loop — the tail ends in exact zero within the bound") {
	constexpr double kBoundSeconds = 8.0;
	double worst = 0.0;
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		for (const DriveMode type : kEveryType) {
			for (const bool driven : { false, true }) {
				for (const bool bell : { false, true }) {
					CAPTURE(static_cast<int>(mode));
					CAPTURE(static_cast<int>(type));
					CAPTURE(driven);
					CAPTURE(bell);
					const Settings s = worstLoops(mode, type, driven, bell);
					const size_t inFrames = 24000;
					const size_t total = inFrames + static_cast<size_t>((kBoundSeconds + 1.0) * kRate);
					std::vector<float> l = whiteInput(inFrames, 0.9f);
					l.resize(total, 0.0f);
					std::vector<float> r = l;
					Eroder e;
					e.prepare(kRate);
					e.setTarget(s);
					for (size_t done = 0; done < total; done += 480) {
						e.processStereo(l.data() + done, r.data() + done, 480);
					}
					size_t last = 0;
					for (size_t i = 0; i < total; ++i) {
						REQUIRE(std::isfinite(l[i]));
						REQUIRE(std::isfinite(r[i]));
						if (l[i] != 0.0f || r[i] != 0.0f) {
							last = i;
						}
					}
					const double tail = static_cast<double>(last > inFrames ? last - inFrames : 0) / kRate;
					worst = std::max(worst, tail);
					CHECK(tail < kBoundSeconds);
					CHECK(last < total - static_cast<size_t>(kRate));
				}
			}
		}
	}
	measured("worst-settings tail, the longest over every combination, seconds", worst);
}

TEST_CASE("Hotter loops: a howling loop comes out at about the same level whatever the emphasis, and 0 dB leaves the limiters' thresholds as they were") {
	// THE FOUR COMBINATIONS OF In Loop AND Restore, by the sign and shape of the amount (restated from the topology).
	const double a18 = std::pow(10.0, 18.0 / 20.0);
	const auto gain = [&](float db, bool bell, bool inLoop, bool restore) {
		Settings s = declared();
		s.emphAmount = db;
		s.emphBell = bell;
		s.emphRestore = restore;
		return SharedSection::pathGainAfterReturn(s, inLoop && db != 0.0f);
	};
	CHECK(gain(-18.0f, false, false, true) == doctest::Approx(a18));  // out of the loop, Restore on: the inverse's boost
	CHECK(gain(18.0f, false, false, true) == doctest::Approx(a18));   // a tilt boosts either way
	CHECK(gain(18.0f, true, false, true) == 1.0);                     // a bell boost is cut by its inverse
	CHECK(gain(-18.0f, true, false, true) == doctest::Approx(a18));   // a bell cut is boosted by its inverse
	CHECK(gain(-18.0f, false, false, false) == 1.0);                  // no Restore, out of the loop: nothing after the return
	CHECK(gain(18.0f, false, true, false) == doctest::Approx(a18));   // in the loop, no Restore: the emphasis's boost stays
	CHECK(gain(-18.0f, true, true, false) == 1.0);                    // a bell cut in the loop does not boost
	CHECK(gain(18.0f, false, true, true) == doctest::Approx(a18));    // in the loop with Restore: the tap is before the inverse
	CHECK(gain(0.0f, false, false, true) == 1.0);
	CHECK(gain(0.0f, true, true, false) == 1.0);

	// AT 0 dB the thresholds are as they were, whatever In Loop, Restore and the shape say.
	for (const bool inLoop : { false, true }) {
		for (const bool restore : { false, true }) {
			Settings s = declared();
			s.resonance = 0.9f;
			s.loop = 1.0f;
			s.emphAmount = 0.0f;
			s.emphInLoop = inLoop;
			s.emphRestore = restore;
			Eroder e;
			e.prepare(kRate);
			e.setTarget(s);
			std::vector<float> l = whiteInput(960, 0.5f);
			std::vector<float> r = l;
			e.processStereo(l.data(), r.data(), 960);
			CHECK(e.dustForTest().sharedForTest().outerLimiterForTest().thresholdForTest() == 1.0f);
			CHECK(e.dustForTest().sharedForTest().innerLimiterForTest().thresholdForTest() == 1.0f);
		}
	}

	// THE HOWLING LOOP: Dust, Reso 0.9, Loop 1, Drive 0 (the limiter was at -7 dB there). Steady peak on a 0.5 sine.
	const std::vector<float> in = sine(220.0, 96000, 0.5f);
	struct Case {
		const char* what;
		float db;
		bool inLoop;
		bool restore;
	};
	const Case cases[] = {
		{ "0 dB", 0.0f, false, true },
		{ "-18 dB Tilt + Restore", -18.0f, false, true },
		{ "+18 dB Tilt + Restore", 18.0f, false, true },
		{ "+18 dB Tilt In Loop", 18.0f, true, true },
		{ "+18 dB Tilt In Loop, no Restore", 18.0f, true, false },
	};
	double lowest = 1.0e9;
	double highest = 0.0;
	for (const Case& c : cases) {
		Settings s = declared();
		s.mix = 1.0f;
		s.resonance = 0.9f;
		s.loop = 1.0f;
		s.emphAmount = c.db;
		s.emphInLoop = c.inLoop;
		s.emphRestore = c.restore;
		const std::vector<float> out = renderAt(kRate, s, in);
		const std::vector<float> steady(out.begin() + 48000, out.end());
		const double peak = static_cast<double>(peakOf(steady));
		measured(c.what, peak);
		CAPTURE(c.what);
		CHECK(peak > 0.1);
		// (NO RESTORE IS NOT IN THE SPREAD: a +18 dB tilt left in the sound is a tone change, its lows 18 dB down, not a level.)
		if (c.restore) {
			lowest = std::min(lowest, peak);
			highest = std::max(highest, peak);
		}
	}
	const double spreadDb = 20.0 * std::log10(highest / lowest);
	measured("howling loop, spread between the emphasis settings, dB", spreadDb);
	CHECK(spreadDb <= 3.0); // asked for within 6 dB; measured 1.9 dB with the scaling and 5.3 dB without, so 3 is what tells them apart
}

TEST_CASE("Hotter loops: the output stays under its bound while a 0.5 sine or noise plays at the worst settings, in every engine") {
	// THE BOUND, FROM THE PARTS. Each loop's return is held by its limiter to the threshold (1.0 in Dust, Rust and Etch), so what
	// reaches the engine is at most the input (0.5) plus 0.9 of one return and 0.98 of the other: R = 2.38. The slots and the
	// Drive then take that through their curves: a curve's output is the largest `|curve(x * gain)| * make-up` over |x| <= R (a
	// scan), and a stage that is not a curve gives back at most R (plus a step's rounding). The emphasis's inverse then gives
	// the lows up to its +18 dB back (x 7.94), and Mix 1 puts the wet out whole. The emphasis is in the loop, so the sum meets it
	// (x 7.94 at most) before the slots and the Drive: a stage that is not a curve does not hold that down.
	const double inverse = std::pow(10.0, 18.0 / 20.0);
	const double R = (0.5 + 0.9 + kLoopMaxGain) * inverse;
	const auto stage = [](const DriveSettings& d, double reach) {
		if (!d.driven || driveIsNotCurve(d.mode)) {
			return reach * 1.05;
		}
		double best = 0.0;
		for (double x = -reach; x <= reach; x += reach / 4000.0) {
			best = std::max(best, std::abs(static_cast<double>(shapeDrive(d.mode, static_cast<float>(x * d.gain), d.shape, d.gain)) * d.norm));
			best = std::max(best, std::abs(static_cast<double>(shapeDriveRight(d.mode, static_cast<float>(x * d.gain), d.shape, d.gain)) * d.norm));
		}
		return best;
	};
	double highest = 0.0;
	double tightest = 1.0e9;
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		double modeHighest = 0.0;
		for (const DriveMode type : kEveryType) {
			for (const bool driven : { false, true }) {
				for (const bool bell : { false, true }) {
					CAPTURE(static_cast<int>(mode));
					CAPTURE(static_cast<int>(type));
					CAPTURE(driven);
					CAPTURE(bell);
					const Settings s = worstLoops(mode, type, driven, bell);
					double reach = R;
					if (mode == Mode::Etch) {
						const DriveSettings a = deriveDrive(s.slotA.amount, s.slotA.type, s.slotA.shape, kRate, nullptr, s.slotA.mix);
						const DriveSettings b = deriveDrive(s.slotB.amount, s.slotB.type, s.slotB.shape, kRate, nullptr, s.slotB.mix);
						reach = stage(b, stage(a, reach));
					}
					const double bound = inverse * stage(deriveDrive(s.drive, s.driveMode, s.driveShape, kRate, nullptr), reach);
					for (const bool noise : { false, true }) {
						const std::vector<float> in = noise ? whiteInput(48000, 0.5f) : sine(220.0, 48000, 0.5f);
						Eroder e;
						e.prepare(kRate);
						e.setTarget(s);
						std::vector<float> out = in;
						std::vector<float> outR = in;
						float fedBack = 0.0f;
						for (size_t done = 0; done < out.size(); done += 32) {
							e.processStereo(out.data() + done, outR.data() + done, 32);
							if (mode != Mode::Comb) {
								fedBack = std::max(fedBack, std::abs(e.dustForTest().lastFedBackForTest()));
							}
						}
						float peak = 0.0f;
						for (const float v : out) {
							REQUIRE(std::isfinite(v));
							peak = std::max(peak, std::abs(v));
						}
						// THE INNER LIMITER HOLDS WHAT THE INNER LOOP RETURNS to its threshold, at any gain the loop is given.
						CHECK(fedBack <= 1.000001f);
						highest = std::max(highest, static_cast<double>(peak));
						modeHighest = std::max(modeHighest, static_cast<double>(peak));
						tightest = std::min(tightest, bound - static_cast<double>(peak));
						CHECK(static_cast<double>(peak) <= bound);
						// AND THE CEILING: at Mix 1 the output is the wet alone, which the safety holds to +-4 whatever the loops do.
						CHECK(static_cast<double>(peak) <= 4.0);
					}
				}
			}
		}
		char what[80];
		std::snprintf(what, sizeof what, "worst-settings output peak, engine %d", static_cast<int>(mode));
		measured(what, modeHighest);
	}
	measured("worst-settings output peak, the highest over every combination", highest);
	measured("worst-settings output peak, the least headroom under the derived bound", tightest);
}

TEST_CASE("Hotter loops: measured — steady output peak and limiter gain reduction on a 0.5 sine, over Reso, Loop, Drive and Tap, in Dust and Etch") {
	// A TABLE FOR READING (FEATHERS_MEASURE=1): how much of the knob range sits on the limiters. The assertion is that every row is finite and bounded.
	const std::vector<float> in = sine(220.0, 96000, 0.5f);
	const auto driveFor12Db = static_cast<float>((2.0 - kDriveLog2Min) / (kDriveLog2Max - kDriveLog2Min));
	for (const Mode mode : { Mode::Dust, Mode::Etch }) {
		for (const float reso : { 0.5f, 0.9f }) {
			for (const float loop : { 0.0f, 1.0f }) {
				for (const float drive : { 0.0f, driveFor12Db }) {
					for (const bool driven : { false, true }) {
						Settings s = declared();
						s.mode = mode;
						s.mix = 1.0f;
						s.resonance = reso;
						s.loop = loop;
						s.tapDriven = driven;
						s.drive = drive;
						s.driveMode = DriveMode::Soft;
						if (mode == Mode::Etch) {
							s.slotA = { 0.5f, DriveMode::Wreck, 0.3f, 1.0f };
						}
						Eroder e;
						e.prepare(kRate);
						e.setTarget(s);
						std::vector<float> l = in;
						std::vector<float> r = in;
						float peak = 0.0f;
						float lowestGain = 1.0f;
						const SharedSection& sh = e.dustForTest().sharedForTest();
						for (size_t i = 0; i < l.size(); ++i) {
							e.processStereo(l.data() + i, r.data() + i, 1);
							if (i >= 48000) {
								peak = std::max(peak, std::abs(l[i]));
								lowestGain = std::min({ lowestGain, sh.outerLimiterGainForTest(), sh.innerLimiterForTest().gain() });
							}
						}
						char what[160];
						std::snprintf(what, sizeof what, "%s reso %.1f loop %.0f drive %s tap %s: peak / limiter dB", mode == Mode::Dust ? "Dust" : "Etch",
						              static_cast<double>(reso), static_cast<double>(loop), drive == 0.0f ? "0" : "+12", driven ? "Driven" : "Clean");
						measured(what, static_cast<double>(peak));
						std::snprintf(what, sizeof what, "%s reso %.1f loop %.0f drive %s tap %s: limiter gain reduction dB", mode == Mode::Dust ? "Dust" : "Etch",
						              static_cast<double>(reso), static_cast<double>(loop), drive == 0.0f ? "0" : "+12", driven ? "Driven" : "Clean");
						measured(what, 20.0 * std::log10(static_cast<double>(lowestGain)));
						CHECK(std::isfinite(peak));
						CHECK(peak < 8.0f);
					}
				}
			}
		}
	}
}

TEST_CASE("Hotter loops: with the input silent the limiter-held loop dies — Reso 100%, Loop 100%, Tap Driven, Drive high, every engine — to exact zero within a bound") {
	// THE BOUND: the fade starts after kLoopFadeStartSeconds, falls by e every kLoopFadeSeconds, and the delay lines then play out.
	constexpr double kBoundSeconds = 8.0;
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb, Mode::Etch }) {
		for (const DriveMode type : { DriveMode::Soft, DriveMode::Fold, DriveMode::Wreck, DriveMode::Slew }) {
		for (const float drive : { 0.0f, 1.0f }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(static_cast<int>(type));
			CAPTURE(drive);
			const Settings s = hotLoops(mode, type, drive);
			const size_t inFrames = 24000;
			const size_t total = inFrames + static_cast<size_t>((kBoundSeconds + 2.0) * kRate);
			std::vector<float> l = whiteInput(inFrames, 0.9f);
			l.resize(total, 0.0f);
			std::vector<float> r = l;
			Eroder e;
			e.prepare(kRate);
			e.setTarget(s);
			size_t last = 0;
			float heldPeak = 0.0f;
			for (size_t done = 0; done < total; done += 480) {
				e.processStereo(l.data() + done, r.data() + done, 480);
			}
			for (size_t i = 0; i < total; ++i) {
				if (l[i] != 0.0f || r[i] != 0.0f) {
					last = i;
				}
				if (i > inFrames && i < inFrames + static_cast<size_t>(0.5 * kRate)) {
					heldPeak = std::max(heldPeak, std::abs(l[i]));
				}
			}
			const double tailSeconds = static_cast<double>(last > inFrames ? last - inFrames : 0) / kRate;
			measured("hot loops tail, seconds after the input stopped", tailSeconds);
			CHECK(heldPeak > 0.002f); // NON-VACUITY: the loop was ringing when the input stopped
			CHECK(tailSeconds < kBoundSeconds);
			CHECK(last < total - static_cast<size_t>(kRate)); // and the last second is exactly zero
		}
		}
	}
}

TEST_CASE("Hotter loops: the silent-input rules are partition-proof, the fade comes back with the input, and the gate closes only on a quiet loop") {
	Settings s = hotLoops(Mode::Dust, DriveMode::Fold);
	const size_t total = static_cast<size_t>(6.0 * kRate);
	std::vector<float> in = whiteInput(12000, 0.8f);
	in.resize(total, 0.0f);
	// A SECOND BURST after the fade has run down, so the scale must come back for the loop to ring again.
	const std::vector<float> burst = whiteInput(6000, 0.8f);
	std::copy(burst.begin(), burst.end(), in.begin() + static_cast<std::ptrdiff_t>(4.5 * kRate));
	const auto run = [&](int block, float* scaleAtEnd) {
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		for (size_t done = 0; done < total;) {
			const size_t take = std::min<size_t>(static_cast<size_t>(block), total - done);
			e.processStereo(l.data() + done, r.data() + done, static_cast<int>(take));
			done += take;
		}
		if (scaleAtEnd != nullptr) {
			*scaleAtEnd = e.dustForTest().sharedForTest().loopScaleForTest();
		}
		return l;
	};
	float scale = 0.0f;
	const std::vector<float> whole = run(480, &scale);
	CHECK(run(1, nullptr) == whole);
	CHECK(run(37, nullptr) == whole);
	// THE SECOND BURST RANG: the loop was open again.
	float second = 0.0f;
	for (size_t i = static_cast<size_t>(4.5 * kRate) + 6000; i < static_cast<size_t>(4.5 * kRate) + 24000; ++i) {
		second = std::max(second, std::abs(whole[i]));
	}
	measured("after the second burst, ring peak", second);
	CHECK(second > 0.01f);
	// AT THE END (1.5 s of silence after the burst, more than the fade's start) the scale is below one.
	CHECK(scale < 1.0f);

	// THE GATE DOES NOT CLOSE ON A LOUD LOOP: a section whose limiter envelope is high stays open with the input silent.
	SharedSection sh;
	sh.prepare(kRate, true);
	sh.setThreshold(1.0f);
	const Settings on = hotLoops(Mode::Dust, DriveMode::Soft);
	sh.set(on, deriveLoops(on, 800.0, 0.5), 800.0);
	// A LOUD LOOP: the return is written loud every frame and read back, the input silent throughout.
	for (int i = 0; i < 2000; ++i) {
		(void)sh.outerBegin();
		float a = 0.0f;
		float b = 0.0f;
		sh.outerReturn(a, b);
		sh.outerWrite(0.9f, 0.9f);
		sh.inputFrame(true);
	}
	CHECK_FALSE(sh.gatedForTest());
	CHECK(sh.loopScale() == 1.0f);
	// AND IT CLOSES ONCE THE ENVELOPE HAS RELEASED: 50 ms per e-fold, 14 e-folds to -120 dB.
	for (int i = 0; i < static_cast<int>(0.9 * kRate); ++i) {
		float a = 0.0f;
		float b = 0.0f;
		sh.outerReturn(a, b);
		sh.outerWrite(0.0f, 0.0f);
		sh.inputFrame(true);
	}
	CHECK(sh.gatedForTest());
	CHECK(sh.loopScale() == 0.0f);
	// AND OPENS WITH THE INPUT.
	sh.inputFrame(false);
	CHECK_FALSE(sh.gatedForTest());
}

//--------------------------------------------------------------------------------------------------//
// THE INPUT DRIVE: a plain gain on the signal entering the wet chain.                                //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE RATIO OF THE `harmonic`-TH PARTIAL TO THE FUNDAMENTAL, by correlation over whole cycles (a restatement, not the device's).
double harmonicLevel(const std::vector<float>& v, size_t from, size_t period, size_t harmonic) {
	double re = 0.0;
	double im = 0.0;
	for (size_t i = from; i < v.size(); ++i) {
		const double phase = 2.0 * 3.14159265358979323846 * static_cast<double>(harmonic) * static_cast<double>(i - from) /
		                     static_cast<double>(period);
		re += static_cast<double>(v[i]) * std::cos(phase);
		im += static_cast<double>(v[i]) * std::sin(phase);
	}
	return std::sqrt(re * re + im * im);
}

double thdOf(const std::vector<float>& v, size_t from, size_t period) {
	const double fundamental = harmonicLevel(v, from, period, 1);
	double rest = 0.0;
	for (size_t k = 2; k <= 9; ++k) {
		const double h = harmonicLevel(v, from, period, k);
		rest += h * h;
	}
	return std::sqrt(rest) / std::max(fundamental, 1.0e-30);
}

std::vector<float> sineInput(size_t frames, size_t period, float amp) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		v[i] = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(period)));
	}
	return v;
}

} // namespace

TEST_CASE("Input drive: declared, carried by the codec, and at 0 dB a gain of exactly one") {
	const auto& d = kParamDescs[static_cast<size_t>(Param::Input)];
	CHECK(std::string_view(d.key) == "eroder_input");
	CHECK(std::string_view(d.name) == "Eroder Input");
	CHECK(d.min == -24.0);
	CHECK(d.max == 24.0);
	CHECK(d.def == 0.0);
	CHECK(d.unit == fx::ParamUnit::Decibel);
	CHECK(d.modulatable);
	CHECK(d.gateControl == -1);
	State st;
	st.settings.inputDb = -7.5f;
	CHECK(stateOf(rowOf(st), static_cast<size_t>(Param::kCount)) == st);
	CHECK(Settings {}.inputDb == 0.0f);
	CHECK(inputGainOf(0.0f) == 1.0f); // to the bit: the chain is untouched at rest
	CHECK(inputGainOf(12.0f) == doctest::Approx(3.98107f).epsilon(1e-5));
	CHECK(inputGainOf(-12.0f) == doctest::Approx(0.251189f).epsilon(1e-5));
	// AN OLD PATCH WITHOUT THE FIELD READS ZERO.
	CHECK(stateOf(rowOf(State {}), static_cast<size_t>(Param::Input)).settings.inputDb == 0.0f);
}

TEST_CASE("Input drive: at a fully linear setting the wet scales by exactly the gain, in the engines that have a linear setting") {
	const std::vector<float> in = whiteInput(9600, 0.1f);
	for (const Mode mode : { Mode::Dust, Mode::Etch }) {
		CAPTURE(static_cast<int>(mode));
		Settings s = dustStill();
		s.mode = mode;
		s.mix = 1.0f; // the dry is turned right down, so what comes out is the wet alone
		const std::vector<float> base = renderAt(kRate, s, in);
		REQUIRE(peakOf(base) > 0.01f); // non-vacuous: there is a wet to scale
		for (const float db : { 12.0f, -12.0f }) {
			CAPTURE(db);
			Settings g = s;
			g.inputDb = db;
			const std::vector<float> out = renderAt(kRate, g, in);
			std::vector<float> scaled = base;
			const float gain = inputGainOf(db);
			for (float& v : scaled) {
				v *= gain;
			}
			const double err = relativeError(out, scaled);
			measured("input drive linear wet relative error", err);
			CHECK(err < 1.0e-4);
			CHECK(relativeError(out, base) > 0.5); // and the gain is not nothing
		}
	}
}

TEST_CASE("Input drive: into a saturating Drive type, +12 dB makes more harmonic distortion than 0 dB") {
	constexpr size_t period = 48;
	const std::vector<float> in = sineInput(9600, period, 0.1f);
	Settings s = declared();
	s.mode = Mode::Etch;
	s.mix = 1.0f;
	s.slotA.amount = 0.8f;
	s.slotA.type = DriveMode::Soft;
	s.slotA.shape = 0.5f;
	s.slotA.mix = 1.0f;
	double thd[2] = {};
	for (int k = 0; k < 2; ++k) {
		Settings g = s;
		g.inputDb = k == 0 ? 0.0f : 12.0f;
		thd[k] = thdOf(renderAt(kRate, g, in), 4800, period);
	}
	measured("input drive thd at 0 dB", thd[0]);
	measured("input drive thd at +12 dB", thd[1]);
	CHECK(thd[1] > 1.5 * thd[0]);
	CHECK(thd[1] > 0.01); // the saturation is audible at all
}

TEST_CASE("Input drive: Follow's envelope takes the signal before the gain, and the dry is not driven") {
	const std::vector<float> in = whiteInput(4800, 0.3f);
	float envelope[2] = {};
	std::vector<float> dryOnly[2];
	for (int k = 0; k < 2; ++k) {
		Settings s = declared();
		s.follow = 1.0f;
		s.mix = 0.0f; // no wet: what comes out is the dry
		s.inputDb = k == 0 ? 0.0f : 18.0f;
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		dryOnly[k] = in;
		std::vector<float> r = in;
		e.processStereo(dryOnly[k].data(), r.data(), static_cast<int>(in.size()));
		envelope[k] = e.envelopeForTest();
	}
	REQUIRE(envelope[0] > 0.01f);
	CHECK(envelope[1] == envelope[0]);
	CHECK(dryOnly[1] == dryOnly[0]);
}

TEST_CASE("Input drive: the Comb takes the gain too, and its tail still reaches exact zero") {
	const std::vector<float> in = whiteInput(4800, 0.1f);
	Settings s = defaults();
	s.mix = 1.0f;
	const std::vector<float> a = renderAt(kRate, s, in);
	s.inputDb = 12.0f;
	const std::vector<float> b = renderAt(kRate, s, in);
	CHECK(relativeError(b, a) > 0.5);
	CHECK(peakOf(b) > peakOf(a));
	// A TAIL: input, then silence, at +24 dB; it must come to exactly zero.
	Eroder e;
	e.prepare(kRate);
	Settings t = defaults();
	t.inputDb = 24.0f;
	e.setTarget(t);
	std::vector<float> l = whiteInput(2400, 0.3f);
	std::vector<float> r = l;
	e.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	bool silent = false;
	for (int block = 0; block < 4000 && !silent; ++block) {
		std::vector<float> z(480, 0.0f);
		std::vector<float> zr = z;
		e.processStereo(z.data(), zr.data(), 480);
		silent = peakOf(z) == 0.0f && peakOf(zr) == 0.0f;
	}
	CHECK(silent);
}

//--------------------------------------------------------------------------------------------------//
// THE LOOPS' LIMITER READINGS, for the window's feedback meters.                                      //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("Limiter readings: a howling loop publishes a gain below one and a level at its threshold, a quiet one publishes one and nothing") {
	const auto at = [](const auto& readings, Reading r) { return readings[static_cast<size_t>(r)]; };
	const std::vector<float> sineIn = sine(220.0, 48000, 0.5f);
	for (const Mode mode : { Mode::Dust, Mode::Rust, Mode::Comb }) {
		CAPTURE(static_cast<int>(mode));
		// DRIVE OFF, BOTH LOOPS WIDE OPEN: nothing is divided out, so the loops sit on their limiters (`hotLoops`).
		Settings howl = hotLoops(mode, DriveMode::Soft, 0.0f);
		howl.rustBias = 1.0f;
		Eroder loud;
		loud.prepare(kRate);
		loud.setTarget(howl);
		std::vector<float> l = sineIn;
		std::vector<float> r = sineIn;
		float lowestInner = 1.0f;
		float lowestOuter = 1.0f;
		float highestLevel = 0.0f;
		for (size_t done = 0; done < l.size(); done += 480) {
			loud.processStereo(l.data() + done, r.data() + done, 480);
			const auto& readings = loud.readingsForTest();
			lowestInner = std::min(lowestInner, at(readings, Reading::LimiterInnerGain));
			lowestOuter = std::min(lowestOuter, at(readings, Reading::LimiterOuterGain));
			highestLevel = std::max({ highestLevel, at(readings, Reading::LimiterInnerLevel), at(readings, Reading::LimiterOuterLevel) });
		}
		measured("limiter inner gain, howling", static_cast<double>(lowestInner));
		measured("limiter outer gain, howling", static_cast<double>(lowestOuter));
		if (mode != Mode::Comb) { // (the Comb's inner gain is the bank's)
			CHECK(lowestInner < 1.0f);
			CHECK(lowestOuter < 1.0f);
			CHECK(lowestInner > 0.0f);
			CHECK(highestLevel > 0.9f); // the return runs at the threshold
		}
		CHECK(highestLevel <= 4.0f);
		// A QUIET SETTING: no loop at all, so the limiters let everything by and nothing returns.
		Settings quiet = declared();
		quiet.mode = mode == Mode::Comb ? Mode::Dust : mode;
		quiet.resonance = 0.0f;
		quiet.loop = 0.0f;
		std::vector<float> ql = whiteInput(4800, 0.001f);
		std::vector<float> qr = ql;
		Eroder e;
		e.prepare(kRate);
		e.setTarget(quiet);
		e.processStereo(ql.data(), qr.data(), 4800);
		const auto& calm = e.readingsForTest();
		CHECK(at(calm, Reading::LimiterOuterGain) == 1.0f);
		CHECK(at(calm, Reading::LimiterOuterLevel) == 0.0f);
		CHECK(at(calm, Reading::LimiterInnerGain) == 1.0f);
		CHECK(at(calm, Reading::LimiterInnerLevel) == 0.0f);
	}
	// EACH LOOP ON ITS OWN, so the two readings cannot be read for one another: Resonance alone limits the inner and not the outer, and
	// the Loop alone does not touch the inner.
	for (const bool innerOnly : { true, false }) {
		CAPTURE(innerOnly);
		Settings one = hotLoops(Mode::Dust, DriveMode::Soft, 0.0f);
		one.tapDriven = false; // a Driven tap would put the inner limiter on whatever Resonance says
		one.emphAmount = 0.0f; // and so would an emphasis in the loop
		one.emphInLoop = false;
		one.resonance = innerOnly ? 1.0f : 0.0f;
		one.loop = innerOnly ? 0.0f : 1.0f;
		Eroder e;
		e.prepare(kRate);
		e.setTarget(one);
		std::vector<float> l = sineIn;
		std::vector<float> r = sineIn;
		float lowest[2] = { 1.0f, 1.0f };
		for (size_t done = 0; done < l.size(); done += 480) {
			e.processStereo(l.data() + done, r.data() + done, 480);
			lowest[0] = std::min(lowest[0], at(e.readingsForTest(), Reading::LimiterInnerGain));
			lowest[1] = std::min(lowest[1], at(e.readingsForTest(), Reading::LimiterOuterGain));
		}
		// (The Loop alone totals under one, so its limiter rests; the inner one is what must not move for it.)
		if (innerOnly) {
			CHECK(lowest[0] < 1.0f);
			CHECK(lowest[1] == 1.0f);
		}
		else {
			CHECK(lowest[0] == 1.0f);
		}
	}
	// THE READINGS ARE AFTER THE SPECTRUM'S AND THE HEAD'S: the older layout is where it was.
	CHECK(static_cast<size_t>(Reading::LimiterInnerGain) == static_cast<size_t>(Reading::HeadFarR) + 1);
	CHECK(kReadingCount == static_cast<size_t>(Reading::LimiterOuterLevel) + 1);
}

//------------------------------------------------------------------------------------------------------------------------------//
// HP WET IN SEMITONES FROM THE PLAYED NOTE (Track on).                                                                         //
//------------------------------------------------------------------------------------------------------------------------------//

namespace {
/// A NOTE'S FREQUENCY, restated: A440 is MIDI note 69, an octave is twelve.
double noteHzOf(double note) {
	return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}
std::vector<float> trackedRun(const Settings& s, double note, int block) {
	Eroder e;
	e.prepare(kRate);
	e.setTarget(s);
	e.setNoteContext({ static_cast<float>(note), true });
	std::vector<float> l(9600);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 150.0 * static_cast<double>(i) / kRate));
	}
	std::vector<float> r = l;
	for (size_t done = 0; done < l.size(); done += static_cast<size_t>(block)) {
		const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), l.size() - done));
		e.processStereo(l.data() + done, r.data() + done, n);
	}
	return l;
}
} // namespace

TEST_CASE("HP Wet semitones: declared as its own key, continuous, -24 to +128 with the bottom off, default off") {
	const auto& d = kParamDescs[static_cast<size_t>(Param::HpWetSemis)];
	CHECK(std::string_view(d.key) == "eroder_hp_wet_semis");
	CHECK_FALSE(d.stepped);
	CHECK(d.min == -24.0);
	CHECK(d.max == 128.0);
	CHECK(d.def == d.min);
	CHECK(device::formatValue(d, d.min) == "off");
	CHECK(device::formatValue(d, 12.0).find("12") == 0);
	CHECK(device::formatValue(d, 12.0).find("st") != std::string::npos);
	// HP Wet's own key keeps its meaning.
	CHECK(std::string_view(kParamDescs[static_cast<size_t>(Param::HpWet)].key) == "eroder_hp_wet");
}

TEST_CASE("HP Wet semitones: with Track on the corner is the note's frequency times 2^(st/12), and follows the note; Track off uses the Hz control") {
	Settings s = declared();
	s.hpWet = static_cast<float>(std::log2(300.0));
	for (const double st : { -12.0, 0.0, 19.0 }) {
		for (const double note : { 57.0, 69.0, 72.0 }) {
			CAPTURE(st);
			CAPTURE(note);
			s.track = true;
			s.hpWetSemis = static_cast<float>(st);
			const DustDerived d = deriveDust(s, note, kRate);
			REQUIRE(d.hpOn);
			CHECK(std::abs(d.hpHz - noteHzOf(note) * std::pow(2.0, st / 12.0)) < 1.0e-6);
			// THE HERTZ CONTROL IS IGNORED while tracking.
			Settings other = s;
			other.hpWet = static_cast<float>(std::log2(5000.0));
			CHECK(deriveDust(other, note, kRate).hpHz == d.hpHz);
			// AND THE SEMITONES ARE IGNORED with Track off: the hertz control is the corner, as before.
			s.track = false;
			const DustDerived off = deriveDust(s, note, kRate);
			REQUIRE(off.hpOn);
			CHECK(std::abs(off.hpHz - 300.0) < 1.0e-2);
		}
	}
	// OFF at the bottom while tracking, whatever the Hz control says; the Hz control is not consulted.
	s.track = true;
	s.hpWetSemis = static_cast<float>(kHpWetSemisMin);
	CHECK_FALSE(deriveDust(s, 69.0, kRate).hpOn);
	CHECK_FALSE(wetHighpassOn(s, true));
	CHECK(wetHighpassOn(s, false));
	// THE LEGAL RANGE: a high note plus a high offset stops at 8 kHz, a low one at 20 Hz — and still filters.
	s.hpWetSemis = 60.0f;
	CHECK(std::abs(deriveDust(s, 100.0, kRate).hpHz - 8000.0) < 1.0e-6);
	// A 30 HZ NOTE (restated: 69 + 12 log2(30 / 440)): +60 st is five octaves up, 960 Hz; +128 st is far past the top and clamps.
	const double bass = 69.0 + 12.0 * std::log2(30.0 / 440.0);
	CHECK(std::abs(deriveDust(s, bass, kRate).hpHz - 960.0) < 1.0e-6);
	s.hpWetSemis = 128.0f;
	const DustDerived top = deriveDust(s, bass, kRate);
	CHECK(top.hpOn);
	CHECK(std::abs(top.hpHz - std::exp2(kHpWetLog2Max)) < 1.0e-6);
	s.hpWetSemis = -23.0f;
	const DustDerived low = deriveDust(s, 24.0, kRate);
	CHECK(low.hpOn);
	CHECK(std::abs(low.hpHz - 20.0) < 1.0e-6);
	// THE COMB HAS NO TRACK: its highpass is HP Wet's hertz whatever Track holds.
	s.hpWet = static_cast<float>(std::log2(300.0));
	s.hpWetSemis = 0.0f;
	CHECK(deriveWetHighpass(s, false, 69.0, kRate).hz == deriveHighpass(s.hpWet, kRate).hz);
}

TEST_CASE("HP Wet semitones: off while tracking is today's no-filter bit for bit, and any partition gives the same samples") {
	Settings s = dustStill();
	s.track = true;
	s.hpWet = static_cast<float>(kHpWetLog2Min);
	const std::vector<float> today = trackedRun(s, 69.0, 256);
	// THE HERTZ CONTROL IS NOT CONSULTED: raised, the same bits.
	s.hpWet = 9.0f;
	CHECK(trackedRun(s, 69.0, 256) == today);
	// ON, it filters (the second assertion that the first is not vacuous), and the same at any partition.
	s.hpWetSemis = 0.0f;
	s.subThru = true;
	const std::vector<float> on = trackedRun(s, 72.0, 256);
	CHECK(on != today);
	CHECK(trackedRun(s, 72.0, 64) == on);
	CHECK(trackedRun(s, 72.0, 37) == on);
}

//--------------------------------------------------------------------------------------------------//
// PAUNCH-S AND PAUNCH-H: the rectified or squared mid added to the left and taken from the right.      //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE STAGE ON A STEREO PAIR, after 1 s of settling, the last 4800 frames (100 cycles of 1 kHz) kept.
struct Pair {
	std::vector<float> l;
	std::vector<float> r;
};

template <class Input>
Pair lawRun(const DriveSettings& d, Input&& input, size_t settle = 48000, size_t frames = 4800) {
	DriveStage stage;
	stage.prepare(kRate);
	stage.set(d);
	Pair out;
	for (size_t i = 0; i < settle + frames; ++i) {
		float l = 0.0f;
		float r = 0.0f;
		input(i, l, r);
		stage.process(l, r);
		if (i >= settle) {
			out.l.push_back(l);
			out.r.push_back(r);
		}
	}
	return out;
}

double sineAt(size_t i, double hz, double peak) {
	return peak * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate);
}

Pair lawSine(DriveMode type, float knob, float shape, double peak) {
	return lawRun(deriveDrive(knob, type, shape, kRate, nullptr), [peak](size_t i, float& l, float& r) {
		l = r = static_cast<float>(sineAt(i, 1000.0, peak));
	});
}

std::vector<float> sideOf(const Pair& p) {
	std::vector<float> v(p.l.size());
	for (size_t i = 0; i < v.size(); ++i) {
		v[i] = 0.5f * (p.l[i] - p.r[i]);
	}
	return v;
}
std::vector<float> midOf(const Pair& p) {
	std::vector<float> v(p.l.size());
	for (size_t i = 0; i < v.size(); ++i) {
		v[i] = 0.5f * (p.l[i] + p.r[i]);
	}
	return v;
}
double partial(const std::vector<float>& v, int k) {
	return std::abs(binOf(v, 1000.0 * k));
}
} // namespace

TEST_CASE("Paunch laws: the Type names, and the Amount is linear 0..4 with no make-up") {
	CHECK(std::string(kDriveModeNames[static_cast<size_t>(DriveMode::PaunchS)]) == "Paunch-S");
	CHECK(std::string(kDriveModeNames[static_cast<size_t>(DriveMode::PaunchH)]) == "Paunch-H");
	CHECK(static_cast<int>(DriveMode::PaunchS) == static_cast<int>(DriveMode::Chonk) + 1);
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		for (const float knob : { 0.1f, 0.25f, 0.5f, 1.0f }) {
			const DriveSettings d = deriveDrive(knob, type, 0.0f, kRate, nullptr);
			CHECK(d.gain == doctest::Approx(4.0f * knob));
			CHECK(d.norm == 1.0f);
			CHECK(driveMakeUp(type, d.gain, 0.0f) == 1.0);
		}
		CHECK_FALSE(deriveDrive(0.0f, type, 0.0f, kRate, nullptr).driven);
		CHECK(driveSlope(type, 0.0f) == 1.0);
	}
	CHECK(driveSmallSignalGain(DriveMode::PaunchH, 4.0, 0.0f) == 1.0);
	CHECK(driveSmallSignalGain(DriveMode::PaunchS, 4.0, 0.0f) == doctest::Approx(5.0));
	CHECK(driveSmallSignalGain(DriveMode::PaunchS, 4.0, 0.5f) == doctest::Approx(1.0));
	// KAPPA, restated: equal 2nd harmonics at a = 4 and the reference peak.
	const double kappa = 4.0 / (3.0 * std::numbers::pi);
	CHECK(kPaunchSquareK == doctest::Approx(kappa).epsilon(1.0e-12));
	CHECK(kappa * (4.0 * kDriveReferencePeak) * (4.0 * kDriveReferencePeak) / 2.0 ==
	      doctest::Approx(4.0 * kDriveReferencePeak * 4.0 / (3.0 * std::numbers::pi)).epsilon(1.0e-12));
}

TEST_CASE("Paunch-S: the side is a |x|, the 2nd harmonic is a P 4 / (3 pi), the mid carries none of it, L and R fundamentals match") {
	const double a = 1.0;
	const double P = 0.5;
	const Pair p = lawSine(DriveMode::PaunchS, 0.25f, 0.0f, P); // Amount at a quarter: a = 1
	const std::vector<float> side = sideOf(p);
	const std::vector<float> mid = midOf(p);
	const double h2 = partial(side, 2);
	const double expected2 = a * P * 4.0 / (3.0 * std::numbers::pi);
	measured("Paunch-S 2nd harmonic of the side at a=1, P=0.5", h2);
	measured("Paunch-S 2nd harmonic expected", expected2);
	CHECK(std::abs(h2 / expected2 - 1.0) < 0.02);
	// THE RECTIFIED SINE'S SERIES: the 2k-th term is 4 / (pi (4k^2 - 1)), so against the 2nd it is 3 / (4k^2 - 1).
	for (const int k : { 2, 3, 4 }) {
		const double want = 3.0 / (4.0 * k * k - 1.0);
		const double got = partial(side, 2 * k) / h2;
		CAPTURE(k);
		CHECK(std::abs(dbOf(got / want)) < 0.5);
	}
	const double fundamental = partial(mid, 1);
	CHECK(fundamental == doctest::Approx(P).epsilon(0.01));
	for (int k = 2; k <= 10; ++k) {
		CAPTURE(k);
		CHECK(dbOf(partial(mid, k) / fundamental) < -80.0);
	}
	// THE FUNDAMENTAL: the same level and phase on both sides (the side has none).
	const std::complex<double> fl = binOf(p.l, 1000.0);
	const std::complex<double> fr = binOf(p.r, 1000.0);
	CHECK(std::abs(dbOf(std::abs(fl) / std::abs(fr))) < 0.05);
	CHECK(degreesApart(fl, fr) < 0.1);
	// NO OFFSET LEAVES THE STAGE (the law's s is never negative: its mean is a P 2 / pi = 0.32 before the blocker).
	CHECK(dbOf(std::abs(meanOf(p.l)) / peakOf(p.l)) < -80.0);
	CHECK(dbOf(std::abs(meanOf(p.r)) / peakOf(p.r)) < -80.0);
	// AND THE SIDE REALLY IS THERE (a difference, as the second assertion): the left is not the right.
	CHECK(peakOf(side) > 0.2);
}

TEST_CASE("Paunch laws: the right at +x is the left's mirror at -x, sample for sample, at Shape 0") {
	const std::vector<float> in = wanderOf(24000);
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		for (const float knob : { 0.3f, 1.0f }) {
			CAPTURE(static_cast<int>(type));
			CAPTURE(knob);
			const DriveSettings d = deriveDrive(knob, type, 0.0f, kRate, nullptr);
			const Pair pos = lawRun(d, [&](size_t i, float& l, float& r) { l = r = in[i]; }, 0, in.size());
			const Pair neg = lawRun(d, [&](size_t i, float& l, float& r) { l = r = -in[i]; }, 0, in.size());
			double worst = 0.0;
			double energy = 0.0;
			for (size_t i = 0; i < in.size(); ++i) {
				worst = std::max(worst, static_cast<double>(std::abs(pos.r[i] + neg.l[i])));
				energy = std::max(energy, static_cast<double>(std::abs(pos.l[i] - pos.r[i])));
			}
			measured("mirror, worst |r(x) + l(-x)|", worst);
			CHECK(worst < 1.0e-5);
			CHECK(energy > 0.05); // not vacuous: the two sides differ
		}
	}
}

TEST_CASE("Paunch-H: a pure 2nd, quadratic in the level and in the Amount, equal to kappa (a P)^2 / 2") {
	const auto second = [](float knob, double peak, std::vector<float>* side = nullptr) {
		const Pair p = lawSine(DriveMode::PaunchH, knob, 0.0f, peak);
		const std::vector<float> s = sideOf(p);
		if (side != nullptr) {
			*side = s;
		}
		return partial(s, 2);
	};
	std::vector<float> side;
	const double a = 2.0; // knob 0.5
	const double P = 0.5;
	const double h2 = second(0.5f, P, &side);
	const double expected = kPaunchSquareK * (a * P) * (a * P) / 2.0;
	measured("Paunch-H 2nd at a=2, P=0.5", h2);
	CHECK(std::abs(h2 / expected - 1.0) < 0.02);
	for (int k = 4; k <= 10; k += 2) {
		CAPTURE(k);
		CHECK(dbOf(partial(side, k) / h2) < -60.0);
	}
	CHECK(partial(side, 3) / h2 < 1.0e-3);
	CHECK(std::abs(dbOf(second(0.5f, 0.5) / second(0.5f, 0.25)) - 12.04) < 0.2);
	CHECK(std::abs(dbOf(second(0.5f, 0.5) / second(0.25f, 0.5)) - 12.04) < 0.2);
}

TEST_CASE("Paunch laws: at full Amount and the reference peak the two Types' 2nd harmonics are equal") {
	const double s2 = partial(sideOf(lawSine(DriveMode::PaunchS, 1.0f, 0.0f, kDriveReferencePeak)), 2);
	const double h2 = partial(sideOf(lawSine(DriveMode::PaunchH, 1.0f, 0.0f, kDriveReferencePeak)), 2);
	measured("calibration, Paunch-S 2nd", s2);
	measured("calibration, Paunch-H 2nd", h2);
	measured("calibration, difference dB", dbOf(s2 / h2));
	CHECK(std::abs(dbOf(s2 / h2)) < 0.2);
}

TEST_CASE("Paunch laws: Shape 0.5 is the dry, Shape 1 puts the products in the mid and leaves the side empty") {
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		CAPTURE(static_cast<int>(type));
		const Pair half = lawSine(type, 0.6f, 0.5f, 0.5);
		const std::vector<float> halfMid = midOf(half);
		for (int k = 2; k <= 10; ++k) {
			CHECK(dbOf(partial(halfMid, k) / partial(halfMid, 1)) < -80.0);
			CHECK(dbOf(partial(sideOf(half), k) / partial(halfMid, 1) + 1.0e-12) < -80.0);
		}
		CHECK(partial(halfMid, 1) == doctest::Approx(0.5).epsilon(0.001));
		const Pair top = lawSine(type, 0.6f, 1.0f, 0.5);
		const double fund = partial(midOf(top), 1);
		CHECK(dbOf(partial(sideOf(top), 2) / fund + 1.0e-12) < -80.0);
		CHECK(dbOf(partial(midOf(top), 2) / fund) > -40.0); // the products are there, in the mid (not vacuous)
		for (size_t i = 0; i < top.l.size(); ++i) {
			REQUIRE(std::abs(top.l[i] - top.r[i]) < 1.0e-5f);
		}
	}
}

TEST_CASE("Paunch laws: for a stereo input the mono sum is the input's at Shape 0, the products cancel") {
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		CAPTURE(static_cast<int>(type));
		const DriveSettings d = deriveDrive(0.9f, type, 0.0f, kRate, nullptr);
		const Pair p = lawRun(d, [](size_t i, float& l, float& r) {
			l = static_cast<float>(sineAt(i, 1000.0, 0.4));
			r = static_cast<float>(sineAt(i, 1500.0, 0.3));
		});
		const std::vector<float> mid = midOf(p);
		double worst = 0.0;
		for (int k = 2; k <= 12; ++k) {
			if (k != 3) { // 1500 Hz is the input's own partial
				worst = std::max(worst, partial(mid, k) / 0.35);
			}
		}
		measured("stereo input, largest mid partial other than the input's", dbOf(worst));
		CHECK(dbOf(worst) < -80.0);
		CHECK(partial(mid, 1) == doctest::Approx(0.2).epsilon(0.001));
		// NOT VACUOUS: the side holds the products.
		CHECK(partial(sideOf(p), 2) > 0.05);
	}
}

TEST_CASE("Paunch laws: Amount 0 is not run, and an Etch slot runs them") {
	const std::vector<float> in = wanderOf(4000);
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		CHECK(stageOf(deriveDrive(0.0f, type, 0.0f, kRate, nullptr), in) == in);
		Settings s = dustStill();
		s.mode = Mode::Etch;
		s.mix = 1.0f;
		s.slotA = { 0.8f, type, 0.0f, 1.0f };
		s.slotB = { 0.0f, DriveMode::Soft, 0.0f, 1.0f };
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l(24000);
		std::vector<float> r(24000);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = r[i] = static_cast<float>(sineAt(i, 1000.0, 0.4));
		}
		const std::vector<float> dry = l;
		for (size_t done = 0; done < l.size(); done += 480) {
			e.processStereo(l.data() + done, r.data() + done, 480);
		}
		const std::vector<float> tailL(l.end() - 4800, l.end());
		const std::vector<float> tailR(r.end() - 4800, r.end());
		double sideEnergy = 0.0;
		for (size_t i = 0; i < tailL.size(); ++i) {
			sideEnergy += std::abs(tailL[i] - tailR[i]);
		}
		CHECK(sideEnergy > 10.0); // the slot made a side
	}
}

TEST_CASE("Paunch laws: a burst's tail reaches exact zero, and the stage's mix blends as it does for every Type") {
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		CAPTURE(static_cast<int>(type));
		const DriveSettings d = deriveDrive(1.0f, type, 0.0f, kRate, nullptr);
		DriveStage stage;
		stage.prepare(kRate);
		stage.set(d);
		size_t last = 0;
		const size_t total = 48000 * 4;
		for (size_t i = 0; i < total; ++i) {
			float l = i < 4800 ? static_cast<float>(sineAt(i, 700.0, 0.8)) : 0.0f;
			float r = i < 4800 ? static_cast<float>(sineAt(i, 900.0, 0.8)) : 0.0f;
			stage.process(l, r);
			if (l != 0.0f || r != 0.0f) {
				last = i;
			}
		}
		measured("Paunch law tail, last non-zero frame", static_cast<double>(last));
		CHECK(last < total - 48000);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE BIAS OF PAUNCH-S AND PAUNCH-H: kept through the chain, taken out once after the last shaper.     //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A 1 kHz sine of `peak` in both channels through the engine, one second to settle, then 4800 frames (100 cycles) kept. The tap
/// (what went back into the inner loop, left) is kept too, a frame at a time.
Pair engineRun(const Settings& s, bool blockInStages, double peak, std::vector<float>* tap = nullptr, size_t block = 480) {
	Eroder e;
	e.prepare(kRate);
	e.blockBiasInStagesForTest(blockInStages);
	e.setTarget(s);
	Pair out;
	const size_t settle = 48000;
	const size_t frames = 4800;
	std::vector<float> l(block);
	std::vector<float> r(block);
	size_t at = 0;
	while (at < settle + frames) {
		const size_t n = tap != nullptr ? 1 : std::min(block, settle + frames - at);
		for (size_t i = 0; i < n; ++i) {
			l[i] = r[i] = static_cast<float>(sineAt(at + i, 1000.0, peak));
		}
		e.processStereo(l.data(), r.data(), static_cast<int>(n));
		for (size_t i = 0; i < n; ++i) {
			if (at + i >= settle) {
				out.l.push_back(l[i]);
				out.r.push_back(r[i]);
				if (tap != nullptr) {
					tap->push_back(e.dustForTest().lastTapForTest());
				}
			}
		}
		at += n;
	}
	return out;
}

double meanDb(const std::vector<float>& v) {
	double mean = 0.0;
	double peak = 1.0e-12;
	for (const float x : v) {
		mean += static_cast<double>(x);
		peak = std::max(peak, static_cast<double>(std::abs(x)));
	}
	return dbOf(std::abs(mean / static_cast<double>(v.size())) / peak + 1.0e-30);
}

} // namespace

TEST_CASE("Paunch bias: kept between the slots, the bias pushes slot B off centre — its side's even harmonic is 6 dB or more away from the one with a blocker between") {
	Settings s = etchBase();
	s.slotA = { 0.5f, DriveMode::PaunchS, 0.0f, 1.0f };
	s.slotB = { 0.9f, DriveMode::Soft, 0.0f, 1.0f };
	const Pair kept = engineRun(s, false, 0.3);
	const Pair blocked = engineRun(s, true, 0.3);
	const double keptSide = partial(sideOf(kept), 2);
	const double blockedSide = partial(sideOf(blocked), 2);
	measured("bias kept: side 2nd harmonic after slot B", keptSide);
	measured("bias blocked: side 2nd harmonic after slot B", blockedSide);
	measured("kept over blocked, dB", dbOf(keptSide / blockedSide));
	CHECK(std::abs(dbOf(keptSide / blockedSide)) >= 6.0); // (the bias drives slot B into its shoulder, so the kept side is the smaller)
	// NOT VACUOUS: the blocked run has a side at all, and both leave the stage mean-free.
	CHECK(blockedSide > 1.0e-3);
	CHECK(meanDb(kept.l) < -80.0);
	CHECK(meanDb(blocked.l) < -80.0);
}

TEST_CASE("Paunch bias: the engine's output has no offset with a Paunch law in a slot, in the Drive, and in both, in every engine") {
	for (const DriveMode type : { DriveMode::PaunchS, DriveMode::PaunchH }) {
		for (const int where : { 0, 1, 2 }) { // a slot, the Drive, both
			CAPTURE(static_cast<int>(type));
			CAPTURE(where);
			Settings s = etchBase();
			if (where != 1) {
				s.slotA = { 0.6f, type, 0.0f, 1.0f };
			}
			if (where != 0) {
				s.drive = 0.7f;
				s.driveMode = type;
			}
			const Pair p = engineRun(s, false, 0.4);
			measured("output mean relative to peak, dB", meanDb(p.l));
			CHECK(meanDb(p.l) < -80.0);
			CHECK(meanDb(p.r) < -80.0);
			CHECK(peakOf(p.l) > 0.1f); // not vacuous
			// WITHOUT THE FINAL BLOCKER THE OFFSET WOULD BE THERE: the stages' own would have taken it, the seam shows what stays.
			Eroder e;
			e.prepare(kRate);
			e.setTarget(s);
			std::vector<float> l(480, 0.1f);
			std::vector<float> r = l;
			e.processStereo(l.data(), r.data(), 480);
			CHECK(e.dustForTest().biasFinalForTest());
		}
	}
	// THE COMB: its Drive.
	Settings c = declared();
	c.mode = Mode::Comb;
	c.drive = 0.7f;
	c.driveMode = DriveMode::PaunchS;
	const Pair p = engineRun(c, false, 0.4);
	// (the Comb's own clip and fold leave an offset of their own: the law adds none to it)
	c.driveMode = DriveMode::Soft;
	const Pair base = engineRun(c, false, 0.4);
	measured("Comb, mean with Paunch-S / with Soft, dB", meanDb(p.l) - meanDb(base.l));
	CHECK(meanDb(p.l) < std::max(meanDb(base.l), -80.0) + 12.0); // (its wet moves, so a 100 ms mean is not exactly zero for either)
	// A SETTING WITH NO PAUNCH LAW has no blocker in at all.
	Settings plain = etchBase();
	plain.slotA = { 0.6f, DriveMode::Tube, 0.5f, 1.0f };
	plain.drive = 0.7f;
	Eroder e;
	e.prepare(kRate);
	e.setTarget(plain);
	std::vector<float> l(480, 0.1f);
	std::vector<float> r = l;
	e.processStereo(l.data(), r.data(), 480);
	CHECK_FALSE(e.dustForTest().biasFinalForTest());
	CHECK_FALSE(e.dustForTest().biasCleanForTest());
}

TEST_CASE("Paunch bias: the Clean tap is taken after a blocker of its own, so the loop does not recirculate the bias, and the output stays bounded") {
	Settings s = etchBase();
	s.slotA = { 0.5f, DriveMode::PaunchS, 0.0f, 1.0f };
	s.resonance = 0.9f;
	s.tapDriven = false;
	std::vector<float> tap;
	const Pair p = engineRun(s, false, 0.3, &tap);
	measured("fed-back signal mean relative to its peak, dB", meanDb(tap));
	CHECK(meanDb(tap) < -80.0);
	CHECK(peakOf(tap) > 0.05f); // not vacuous: something was fed back
	CHECK(peakOf(p.l) < 8.0f);
}

TEST_CASE("Paunch bias: a slot switched to Paunch-S in mid-render gives the same output at any block size") {
	const auto run = [](size_t block) {
		Settings s = etchBase();
		s.slotB = { 0.5f, DriveMode::Soft, 0.0f, 1.0f };
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> out;
		for (int segment = 0; segment < 3; ++segment) {
			if (segment == 1) {
				s.slotA = { 0.6f, DriveMode::PaunchS, 0.0f, 1.0f }; // the final blocker switches on
			}
			if (segment == 2) {
				s.slotA = { 0.6f, DriveMode::Soft, 0.0f, 1.0f }; // and off again
			}
			e.setTarget(s);
			std::vector<float> l(9600);
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = static_cast<float>(sineAt(i + 9600 * static_cast<size_t>(segment), 300.0, 0.4));
			}
			std::vector<float> r = l;
			for (size_t done = 0; done < l.size(); done += block) {
				const int n = static_cast<int>(std::min(block, l.size() - done));
				e.processStereo(l.data() + done, r.data() + done, n);
			}
			out.insert(out.end(), l.begin(), l.end());
			out.insert(out.end(), r.begin(), r.end());
		}
		return out;
	};
	const std::vector<float> reference = run(256);
	CHECK(run(32) == reference);
	CHECK(run(37) == reference);
	// NOT VACUOUS: the middle segment differs from what Soft alone makes of it.
	CHECK(peakOf(reference) > 0.1f);
}

//--------------------------------------------------------------------------------------------------//
// PAUNCH-OS: a level-tracked third harmonic of the mid, added to one side and taken from the other.    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A SINE OF `hz` IN BOTH CHANNELS through the stage, `settle` frames to settle, then `frames` kept (4800 frames is 22 cycles of 220 Hz).
Pair oddRun(DriveMode type, float knob, float shape, double peak, double hz = 220.0, size_t settle = 24000, size_t frames = 4800) {
	return lawRun(deriveDrive(knob, type, shape, kRate, nullptr),
	              [peak, hz](size_t i, float& l, float& r) { l = r = static_cast<float>(sineAt(i, hz, peak)); }, settle, frames);
}
double partialAt(const std::vector<float>& v, double hz, int k) {
	return std::abs(binOf(v, hz * k));
}

} // namespace

TEST_CASE("Paunch-OS: the side is a pure 3rd of amplitude a A / 4, no fundamental, the mid is the dry, the sides stay level-balanced") {
	for (const float knob : { 0.25f, 1.0f }) { // a = 1 and a = 4
		const double a = 4.0 * static_cast<double>(knob);
		const double A = 0.5;
		CAPTURE(a);
		const Pair p = oddRun(DriveMode::PaunchOS, knob, 0.0f, A);
		const std::vector<float> side = sideOf(p);
		const std::vector<float> mid = midOf(p);
		const double h3 = partialAt(side, 220.0, 3);
		const double fundamental = partialAt(mid, 220.0, 1);
		measured("Paunch-OS side 3rd / (a A / 4)", h3 / (a * A / 4.0));
		measured("Paunch-OS side fundamental re the mid's, dB", dbOf(partialAt(side, 220.0, 1) / fundamental));
		CHECK(std::abs(h3 / (a * A / 4.0) - 1.0) < 0.03);
		CHECK(dbOf(partialAt(side, 220.0, 1) / fundamental) < -40.0);
		for (const int k : { 5, 7, 9 }) {
			CAPTURE(k);
			measured("Paunch-OS side partial re the 3rd, dB", dbOf(partialAt(side, 220.0, k) / h3));
			CHECK(dbOf(partialAt(side, 220.0, k) / h3) < -40.0);
		}
		for (int k = 2; k <= 10; ++k) {
			CAPTURE(k);
			CHECK(dbOf(partialAt(mid, 220.0, k) / fundamental) < -80.0);
		}
		const std::complex<double> fl = binOf(p.l, 220.0);
		const std::complex<double> fr = binOf(p.r, 220.0);
		CHECK(std::abs(dbOf(std::abs(fl) / std::abs(fr))) < 0.1);
		CHECK(degreesApart(fl, fr) < 1.0);
		CHECK(std::abs(degreesApart(binOf(p.l, 660.0), binOf(p.r, 660.0)) - 180.0) < 5.0);
		CHECK(dbOf(std::abs(meanOf(p.l)) / peakOf(p.l) + 1.0e-30) < -80.0); // no offset: odd
	}
}

TEST_CASE("Paunch-OS: level independent — the 3rd over the fundamental is the same at -30, -12 and 0 dBFS") {
	double lo = 1.0e9;
	double hi = -1.0e9;
	for (const double dbfs : { -30.0, -12.0, 0.0 }) {
		const double peak = std::pow(10.0, dbfs / 20.0);
		const Pair p = oddRun(DriveMode::PaunchOS, 0.5f, 0.0f, peak);
		const double ratio = dbOf(partialAt(sideOf(p), 220.0, 3) / partialAt(midOf(p), 220.0, 1));
		measured("Paunch-OS 3rd over fundamental, dB", ratio);
		lo = std::min(lo, ratio);
		hi = std::max(hi, ratio);
		CHECK(std::abs(ratio - dbOf(2.0 / 4.0)) < 0.3); // a / 4 with a = 2
	}
	CHECK(hi - lo < 0.3);
}

TEST_CASE("Paunch-OS: for a stereo input the mono sum is the input's at Shape 0, the products cancel") {
	const DriveSettings d = deriveDrive(0.9f, DriveMode::PaunchOS, 0.0f, kRate, nullptr);
	const Pair p = lawRun(d, [](size_t i, float& l, float& r) {
		l = static_cast<float>(sineAt(i, 220.0, 0.4));
		r = static_cast<float>(sineAt(i, 330.0, 0.3));
	}, 24000, 4800);
	const std::vector<float> mid = midOf(p);
	// THE MID HAS THE TWO INPUT SINES (220 and 330 Hz, halved) AND NOTHING ELSE: every other partial of 110 Hz up to 2 kHz is far under.
	double worst = 0.0;
	for (int k = 1; k <= 18; ++k) {
		if (k != 2 && k != 3) {
			worst = std::max(worst, std::abs(binOf(mid, 110.0 * k)));
		}
	}
	measured("stereo input, largest other mid partial re 0.2, dB", dbOf(worst / 0.2));
	CHECK(dbOf(worst / 0.2) < -80.0);
	CHECK(std::abs(binOf(mid, 220.0)) == doctest::Approx(0.2).epsilon(0.001));
	CHECK(partialAt(sideOf(p), 220.0, 3) > 0.05); // not vacuous: the side holds the 3rd
}

TEST_CASE("Paunch-OS: a sine switched on from silence — the follower's onset is bounded by its cap, and the imbalance and the settling are measured") {
	// THE BOUND, from the law: |s| <= 1.25 a |m| (q is capped at 2, so |q - 0.75| <= 1.25), on any signal, from any state.
	for (const float a : { 1.0f, 4.0f }) {
		float power = 0.0f;
		const float coefficient = paunchFollowCoefficient(4.0 * kRate);
		uint32_t seed = 7u;
		for (size_t i = 0; i < 200000; ++i) {
			seed = seed * 1664525u + 1013904223u;
			const float x = (i % 20000 < 10000 ? 0.0f : 0.6f) * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) +
			                static_cast<float>(sineAt(i, 220.0, i % 30000 < 15000 ? 0.5 : 0.01));
			float l = x;
			float r = x;
			paunchOdd(DriveMode::PaunchOS, l, r, a, 0.0f, power, coefficient);
			REQUIRE(std::abs(l - x) <= 1.25f * a * std::abs(x) * (1.0f + 1.0e-5f) + 1.0e-9f);
			REQUIRE(std::isfinite(l));
		}
	}
	// THE MEASUREMENT, through the stage: 220 Hz at 0.5 from silence, a = 4. A one-cycle sliding window on L, R and the side.
	DriveStage stage;
	stage.prepare(kRate);
	stage.set(deriveDrive(1.0f, DriveMode::PaunchOS, 0.0f, kRate, nullptr));
	const size_t period = 218;
	std::vector<float> l;
	std::vector<float> r;
	for (size_t i = 0; i < 24000; ++i) {
		float a = static_cast<float>(sineAt(i, 220.0, 0.5));
		float b = a;
		stage.process(a, b);
		l.push_back(a);
		r.push_back(b);
	}
	double worstImbalance = 0.0;
	double settledAt = -1.0;
	for (size_t end = period; end < l.size(); ++end) {
		double el = 0.0;
		double er = 0.0;
		std::complex<double> side = 0.0;
		std::complex<double> mid = 0.0;
		for (size_t i = end - period; i < end; ++i) {
			el += static_cast<double>(l[i]) * l[i];
			er += static_cast<double>(r[i]) * r[i];
			const double w = -2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / kRate;
			const std::complex<double> z(std::cos(w), std::sin(w));
			side += 0.5 * static_cast<double>(l[i] - r[i]) * z;
			mid += 0.5 * static_cast<double>(l[i] + r[i]) * z;
		}
		if (end <= 4800) {
			worstImbalance = std::max(worstImbalance, std::abs(dbOf(std::sqrt(el / er))));
		}
		if (settledAt < 0.0 && end > 2 * period && dbOf(std::abs(side) / std::abs(mid)) < -30.0) {
			settledAt = static_cast<double>(end) / kRate;
		}
	}
	measured("Paunch-OS onset: worst L/R level imbalance in the first 100 ms, dB", worstImbalance);
	measured("Paunch-OS onset: seconds until the side's fundamental is 30 dB under the mid's", settledAt);
	CHECK(settledAt > 0.0);
}

TEST_CASE("Paunch-OS: any block size gives the same output, silence gives exact zero, Amount 0 is not run") {
	const auto run = [](size_t block) {
		Settings s = etchBase();
		s.slotA = { 0.7f, DriveMode::PaunchOS, 0.2f, 1.0f };
		Eroder e;
		e.prepare(kRate);
		e.setTarget(s);
		std::vector<float> l(24000);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = i < 12000 ? static_cast<float>(sineAt(i, 220.0, 0.4) + sineAt(i, 1700.0, 0.1)) : 0.0f;
		}
		std::vector<float> r = l;
		for (size_t done = 0; done < l.size(); done += block) {
			const int n = static_cast<int>(std::min(block, l.size() - done));
			e.processStereo(l.data() + done, r.data() + done, n);
		}
		l.insert(l.end(), r.begin(), r.end());
		return l;
	};
	const std::vector<float> reference = run(256);
	CHECK(run(32) == reference);
	CHECK(run(37) == reference);
	CHECK(peakOf(reference) > 0.1f);
	// THE TAIL: input stops at 12000; the stage's output is exactly zero well before the end.
	size_t last = 0;
	for (size_t i = 0; i < 24000; ++i) {
		if (reference[i] != 0.0f || reference[24000 + i] != 0.0f) {
			last = i;
		}
	}
	measured("Paunch-OS tail, last non-zero frame after the input stopped", static_cast<double>(last) - 12000.0);
	CHECK(last < 23000);
	const std::vector<float> in = wanderOf(4000);
	CHECK(stageOf(deriveDrive(0.0f, DriveMode::PaunchOS, 0.0f, kRate, nullptr), in) == in);
	CHECK(driveSmallSignalGain(DriveMode::PaunchOS, 4.0, 0.0f) == doctest::Approx(6.0));
	CHECK(driveMakeUp(DriveMode::PaunchOS, 4.0, 0.0f) == 1.0);
}
