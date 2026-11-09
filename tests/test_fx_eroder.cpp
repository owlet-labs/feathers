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
#include "fx/eroder/eroder_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <string_view>
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

Settings defaults() { return stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings; }

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

TEST_CASE("Enabled off, and Mix at zero, leave the buffer alone and the state unmoved") {
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
		CHECK(l == in);
		CHECK(r == in);
		CHECK(e.holdsForTest() == 0); // `isEnabled()` is `mix > 0`: the reference does not run at all
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
		const auto gain = static_cast<float>(e.derivedForTest().wetGain);
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
	const Settings d = defaults();
	CHECK(d.enabled);
	CHECK(d.cutoffOctaves == 0.0f);
	CHECK(d.gamma == 0.0f);
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
	REQUIRE(std::size(kFaceOrder) == static_cast<size_t>(Param::kCount) - 1);
	CHECK(kFaceOrder[0] == static_cast<uint8_t>(Param::Tone));
	CHECK(kFaceOrder[1] == static_cast<uint8_t>(Param::Character));
	CHECK(kFaceOrder[2] == static_cast<uint8_t>(Param::Cutoff));
	CHECK(kFaceOrder[3] == static_cast<uint8_t>(Param::Mix));
	CHECK(kFaceOrder[4] == static_cast<uint8_t>(Param::Gamma));
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
