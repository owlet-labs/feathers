/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/owltt/crossover.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace feathers::fx::owltt;

// A Linkwitz-Riley crossover makes THREE promises, and every one of them is a number here:
//
//   1. SEPARATION. The lowpass and highpass sides are each -6 dB at the corner and fall at the
//      order's slope — 12 dB/oct for LR2, 24 for LR4. A tone two octaves below the low corner
//      reaches MID at (Ω²/(1+Ω²)) for LR2 and (Ω²/√(1+Ω⁴))² for LR4, Ω = f/fc — the highpass
//      magnitude, restated from the prototype rather than from what the code emits.
//   2. A FLAT SUM. low + mid + high has the input's magnitude at every frequency — but NOT its
//      phase: the sum is the input through one first-order (LR2) or second-order (LR4) allpass per
//      crossover. That is not a defect, it is the definition; the defect would be pretending
//      otherwise, which is what a wet/dry blend against the raw input does.
//   3. THE DRY REFERENCE. `alignDry` IS that allpass chain, so the sum and the aligned dry agree
//      SAMPLE FOR SAMPLE for the phase-compensated variants. This is the number a blend leans on.
//
// Backlog ER (2026-09-16) is why the file reads like this. The port's "LR" formed `rest = x - LP(x)`
// and called the rest a highpass; a one-pole cascade's lag makes that difference HALF a tone two
// octaves below the corner, and the old case here asserted only that the sum was flat — which a
// subtraction is by construction, whatever it separates. A sum that is right says nothing about
// whether the parts are, so the parts are asserted first.

namespace {

struct Response {
	float low = 0.0f;
	float mid = 0.0f;
	float high = 0.0f;
	float sum = 0.0f;
	/// The largest |sum - alignDry| over the measured stretch, in input units.
	float dryMismatch = 0.0f;
};

/// The crossover's magnitude response at one frequency: each band's RMS, the sum's RMS, and the
/// worst disagreement between the sum and the aligned dry — all relative to a unit-amplitude input,
/// so 1.0 is "all of it" and 0.5 is -6 dB.
template <class Xover>
Response responseAt(Xover& xover, float freqHz, float sampleRate) {
	// A second of settling is generous for a 200 Hz corner (the slowest pole here rings out in tens of
	// milliseconds) and cheap; the measured stretch is a whole number of periods where it can be.
	const size_t settle = static_cast<size_t>(sampleRate);
	const size_t measure = static_cast<size_t>(sampleRate);
	const float omega = 2.0f * static_cast<float>(std::numbers::pi) * freqHz / sampleRate;

	xover.reset();
	double low = 0.0, mid = 0.0, high = 0.0, sum = 0.0;
	float mismatch = 0.0f;
	for (size_t i = 0; i < settle + measure; ++i) {
		const float input = std::sin(omega * static_cast<float>(i));
		CrossoverBands bands {};
		xover.process(input, bands);
		const float dry = xover.alignDry(input);
		if (i >= settle) {
			const float s = bands.low + bands.mid + bands.high;
			low += static_cast<double>(bands.low) * bands.low;
			mid += static_cast<double>(bands.mid) * bands.mid;
			high += static_cast<double>(bands.high) * bands.high;
			sum += static_cast<double>(s) * s;
			mismatch = std::max(mismatch, std::fabs(s - dry));
		}
	}
	const double n = static_cast<double>(measure);
	const auto rms = [n](double sq) { return static_cast<float>(std::sqrt(sq / n) * std::numbers::sqrt2); };
	return { rms(low), rms(mid), rms(high), rms(sum), mismatch };
}

/// The prototype highpass magnitude at Ω = f / fc: what a tone below the low corner leaves in the
/// rest, restated from the Butterworth prototypes rather than from the code under test.
float lr2Highpass(float ratio) {
	return (ratio * ratio) / (1.0f + ratio * ratio);
}
float lr4Highpass(float ratio) {
	const float r2 = ratio * ratio;
	const float one = r2 / std::sqrt(1.0f + r2 * r2);
	return one * one;
}

} // namespace

TEST_CASE("backlog ER: LR2 separates at 12 dB/oct, is -6 dB at each corner, sums flat, and its aligned dry IS the sum") {
	constexpr float sampleRate = 48000.0f;
	LR2CrossoverFull xover;
	xover.setLowCrossover(200.0f, sampleRate);
	xover.setHighCrossover(2000.0f, sampleRate);

	SUBCASE("two octaves below the low corner: LOW has the tone, MID has the prototype's leak, HIGH nothing") {
		const Response r = responseAt(xover, 50.0f, sampleRate);
		// LP2 at Ω = 0.25: (1/√(1+Ω²))² = 0.941.
		CHECK(r.low == doctest::Approx(0.941f).epsilon(0.01));
		// The rest is the highpass, and MID takes nearly all of it this far below the high corner.
		CHECK(r.mid == doctest::Approx(lr2Highpass(0.25f)).epsilon(0.03)); // 0.0588 — was 0.5 before ER
		CHECK(r.high < 0.002f);
	}
	SUBCASE("at the low corner both sides are -6 dB") {
		const Response r = responseAt(xover, 200.0f, sampleRate);
		CHECK(r.low == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(r.mid == doctest::Approx(0.5f).epsilon(0.02)); // LP2 at 2 kHz takes 1 % off it
	}
	SUBCASE("at the high corner both sides are -6 dB") {
		const Response r = responseAt(xover, 2000.0f, sampleRate);
		CHECK(r.mid == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(r.high == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(r.low < 0.02f);
	}
	SUBCASE("two octaves above the high corner: HIGH has the tone") {
		const Response r = responseAt(xover, 8000.0f, sampleRate);
		CHECK(r.high == doctest::Approx(0.941f).epsilon(0.01));
		CHECK(r.mid == doctest::Approx(lr2Highpass(0.25f)).epsilon(0.05));
	}
	SUBCASE("the sum is flat to 0.2 % and equals the aligned dry to float precision") {
		for (float freq : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f }) {
			CAPTURE(freq);
			const Response r = responseAt(xover, freq, sampleRate);
			CHECK(r.sum == doctest::Approx(1.0f).epsilon(0.002));
			CHECK(r.dryMismatch < 2e-4f);
		}
	}
}

TEST_CASE("backlog ER: LR4 separates at 24 dB/oct, is -6 dB at each corner, sums flat, and its aligned dry IS the sum") {
	constexpr float sampleRate = 48000.0f;
	LR4CrossoverFull xover;
	xover.setLowCrossover(200.0f, sampleRate);
	xover.setHighCrossover(2000.0f, sampleRate);

	SUBCASE("two octaves below the low corner: the leak into MID is the prototype's, 24 dB down on LR2's") {
		const Response r = responseAt(xover, 50.0f, sampleRate);
		// LP4 at Ω = 0.25: (1/√(1+Ω⁴))² = 0.996.
		CHECK(r.low == doctest::Approx(0.996f).epsilon(0.005));
		CHECK(r.mid == doctest::Approx(lr4Highpass(0.25f)).epsilon(0.05)); // 0.0039
		CHECK(r.high < 1e-4f);
	}
	SUBCASE("at each corner both sides are -6 dB") {
		const Response lo = responseAt(xover, 200.0f, sampleRate);
		CHECK(lo.low == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(lo.mid == doctest::Approx(0.5f).epsilon(0.01));
		const Response hi = responseAt(xover, 2000.0f, sampleRate);
		CHECK(hi.mid == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(hi.high == doctest::Approx(0.5f).epsilon(0.01));
		CHECK(hi.low < 0.001f);
	}
	SUBCASE("steeper than LR2 wherever a band is out of place") {
		LR2CrossoverFull lr2;
		lr2.setLowCrossover(200.0f, sampleRate);
		lr2.setHighCrossover(2000.0f, sampleRate);
		const Response two = responseAt(lr2, 8000.0f, sampleRate);
		const Response four = responseAt(xover, 8000.0f, sampleRate);
		CHECK(four.low < two.low);
		CHECK(four.mid < two.mid);
		CHECK(four.high > two.high);
	}
	SUBCASE("the sum is flat to 0.2 % and equals the aligned dry to float precision") {
		for (float freq : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f }) {
			CAPTURE(freq);
			const Response r = responseAt(xover, freq, sampleRate);
			CHECK(r.sum == doctest::Approx(1.0f).epsilon(0.002));
			CHECK(r.dryMismatch < 2e-4f);
		}
	}
}

TEST_CASE("backlog ER: the Fast variants skip the low band's phase match and pay a ripple in the sum, not a hole") {
	constexpr float sampleRate = 48000.0f;
	// The compensation the Fast variants drop is the HIGH corner's allpass on the LOW band. Where the
	// two overlap — around the low corner — the low band then leads the rest by that allpass's phase
	// there, a few degrees for corners a decade apart, so the sum ripples by well under a dB. This
	// pins the ripple as small AND as present: a Fast variant whose sum were exact would be a Full
	// one wearing the wrong name.
	LR2CrossoverFast lr2;
	lr2.setLowCrossover(200.0f, sampleRate);
	lr2.setHighCrossover(2000.0f, sampleRate);
	LR4CrossoverFast lr4;
	lr4.setLowCrossover(200.0f, sampleRate);
	lr4.setHighCrossover(2000.0f, sampleRate);
	float worst2 = 0.0f, worst4 = 0.0f;
	for (float freq : { 50.0f, 100.0f, 200.0f, 400.0f, 1000.0f, 2000.0f, 8000.0f }) {
		CAPTURE(freq);
		const Response r2 = responseAt(lr2, freq, sampleRate);
		const Response r4 = responseAt(lr4, freq, sampleRate);
		CHECK(r2.sum == doctest::Approx(1.0f).epsilon(0.03));
		CHECK(r4.sum == doctest::Approx(1.0f).epsilon(0.03));
		worst2 = std::max(worst2, std::fabs(r2.sum - 1.0f));
		worst4 = std::max(worst4, std::fabs(r4.sum - 1.0f));
	}
	CHECK(worst2 > 0.002f);
	CHECK(worst4 > 0.002f);
}

namespace {

/// **OWLTT'S THREE-BAND LR CROSSOVER AS IT STOOD AT 72840f5f**, frozen here verbatim (only renamed) — the reference
/// `MultiSplit<Split, 3>` is proved against, and the class OWLTT now runs on is proved against, so the proof does not
/// compare the moved class with itself.
template <class Split, bool PHASE_COMPENSATED>
class FrozenLinkwitzRiley {
public:
	using Coeffs = typename Split::Coeffs;
	void setLowCrossover(float freqHz, float sampleRate) { low_ = Split::coeffsFor(freqHz, sampleRate); }
	void setHighCrossover(float freqHz, float sampleRate) { high_ = Split::coeffsFor(freqHz, sampleRate); }
	void process(float input, CrossoverBands& out) {
		float low = 0.0f;
		float rest = 0.0f;
		lowSplit_.split(input, low_, low, rest);
		float mid = 0.0f;
		float high = 0.0f;
		highSplit_.split(rest, high_, mid, high);
		if constexpr (PHASE_COMPENSATED) {
			low = lowComp_.process(low, high_);
		}
		out.low = low;
		out.mid = mid;
		out.high = high;
	}
	[[nodiscard]] float alignDry(float input) { return dryHigh_.process(dryLow_.process(input, low_), high_); }

private:
	Split lowSplit_ {};
	Split highSplit_ {};
	typename Split::Allpass lowComp_ {};
	typename Split::Allpass dryLow_ {};
	typename Split::Allpass dryHigh_ {};
	Coeffs low_ = Split::coeffsFor(200.0f, 48000.0f);
	Coeffs high_ = Split::coeffsFor(2000.0f, 48000.0f);
};

/// A fixed pseudo-random signal, and the corners moving under it every 4096 samples.
template <class Split, bool PC> void provedBitForBit() {
	FrozenLinkwitzRiley<Split, PC> frozen;
	LinkwitzRileyCrossover<Split, PC> moved;
	MultiSplit<Split, 3, PC> multi;
	AllpassChain<Split, 3> dry;
	typename Split::Coeffs corners[2] = { Split::coeffsFor(200.0f, 48000.0f), Split::coeffsFor(2000.0f, 48000.0f) };
	uint32_t seed = 12345u;
	const std::pair<float, float> settings[] = { { 200.0f, 2000.0f }, { 90.0f, 7000.0f }, { 1500.0f, 1600.0f }, { 30.0f, 19000.0f } };
	size_t mismatches = 0;
	for (size_t i = 0; i < 4 * 4096; ++i) {
		if (i % 4096 == 0) {
			const auto [lo, hi] = settings[i / 4096];
			frozen.setLowCrossover(lo, 48000.0f);
			frozen.setHighCrossover(hi, 48000.0f);
			moved.setLowCrossover(lo, 48000.0f);
			moved.setHighCrossover(hi, 48000.0f);
			corners[0] = Split::coeffsFor(lo, 48000.0f);
			corners[1] = Split::coeffsFor(hi, 48000.0f);
		}
		seed = seed * 1664525u + 1013904223u;
		const float x = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
		CrossoverBands want {};
		frozen.process(x, want);
		const float wantDry = frozen.alignDry(x);
		CrossoverBands gotMoved {};
		moved.process(x, gotMoved);
		const float gotMovedDry = moved.alignDry(x);
		float bands[3] = {};
		multi.process(x, corners, 3, bands);
		const float gotDry = dry.process(x, corners, 3);
		// BIT FOR BIT: `==` on floats, not Approx.
		const bool same = bands[0] == want.low && bands[1] == want.mid && bands[2] == want.high && gotDry == wantDry
		                  && gotMoved.low == want.low && gotMoved.mid == want.mid && gotMoved.high == want.high
		                  && gotMovedDry == wantDry;
		mismatches += same ? 0 : 1;
	}
	CHECK(mismatches == 0);
}

} // namespace

TEST_CASE("Rack stage 2 / §4b: MultiSplit<Split, 3> is OWLTT's crossover bit for bit — LR2 and LR4, compensated and Fast, the dry allpasses too, with the corners moving") {
	provedBitForBit<Lr2Split, true>();
	provedBitForBit<Lr2Split, false>();
	provedBitForBit<Lr4Split, true>();
	provedBitForBit<Lr4Split, false>();
}

TEST_CASE("Rack stage 2 / §4b: a K-band cascade sums to Π AP of its corners, and BandPath k of x is band k of the cascade, operation for operation") {
	// FIVE BANDS, LR4 and LR2: the sum against the allpass chain of the same corners, and each band path against the band.
	const auto check = [](auto split) {
		using Split = decltype(split);
		MultiSplit<Split, 5> multi;
		AllpassChain<Split, 5> dry;
		BandPath<Split, 5> paths[5];
		const typename Split::Coeffs corners[4] = { Split::coeffsFor(120.0f, 48000.0f), Split::coeffsFor(500.0f, 48000.0f),
		                                            Split::coeffsFor(2000.0f, 48000.0f), Split::coeffsFor(6000.0f, 48000.0f) };
		uint32_t seed = 777u;
		float worst = 0.0f;
		size_t pathMismatches = 0;
		for (size_t i = 0; i < 48000; ++i) {
			seed = seed * 1664525u + 1013904223u;
			const float x = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
			float bands[5] = {};
			multi.process(x, corners, 5, bands);
			const float sum = bands[0] + bands[1] + bands[2] + bands[3] + bands[4];
			worst = std::max(worst, std::fabs(sum - dry.process(x, corners, 5)));
			for (int k = 0; k < 5; ++k) {
				pathMismatches += paths[k].process(x, corners, 5, k) == bands[k] ? 0 : 1;
			}
		}
		CHECK(pathMismatches == 0);
		// FLOAT ROUNDING THROUGH FOUR SPLITS AND SIX COMPENSATIONS of a full-scale noise: MEASURED 6.6e-5 at worst (LR4).
		CAPTURE(worst);
		CHECK(worst < 1.0e-4f);
	};
	check(Lr4Split {});
	check(Lr2Split {});
}
