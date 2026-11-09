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

// THE ADDITIVE ENGINE'S ROTATOR BANK, asserted by
// VALUE: the bank's own seams, or a quantity a single sinusoid determines exactly. Every reference is restated here
// (the sinusoid in double, the Newman phase, the linear ramp, the linear Nyquist fade), never read from the bank.
// Tolerances are absolute: doctest's Approx adds scale 1.0 and would pass small values against small targets.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/partial_bank.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>

using namespace feathers::partials;

namespace {

constexpr double kPi = std::numbers::pi;

/// The Newman phase of partial k for a note of N partials, restated: π·k²/N (in double, unreduced).
double newman(int k, int n = kMaxPartials) {
	return kPi * double(k) * double(k) / double(n);
}

/// Per-partial inputs, zero by default; the bank lives in static storage (it is ~13 KB).
struct Inputs {
	std::array<float, kMaxPartials> inc {}, l {}, r {};
};

void zero(float* p, int n, float v = 0.0f) {
	for (int i = 0; i < n; ++i)
		p[i] = v;
}

} // namespace

TEST_CASE("one partial at a known increment rotates by exactly that angle per sample") {
	// A sweep of increments across the band below the fade, none chosen for luck.
	const float incs[] = {0.003f, 0.0137f, 0.05f, 0.21f, 0.4f, 0.7853f, 1.0f, 1.3f, 1.5708f, 1.9f, 2.3f, 2.7f};
	static PartialBank bank;
	Inputs in;
	for (const float w : incs) {
		CAPTURE(w);
		bank.reset(kMaxPartials);
		in.inc[0] = w;
		in.l[0] = 1.0f;
		bank.snapNextTargets();
		// The rotator's own angle is the requested one (the coefficient's sincos, read back).
		bank.setTargets(in.inc.data(), in.l.data(), nullptr, 1, kControlBlock, false);
		CHECK(std::abs(bank.incrementForTest(0) - w) < 2e-7f);
		// Four control blocks, renormalised between: out[n] = sin(φ0 + (n+1)·w), φ0 = 0 for partial 0.
		double maxErr = 0.0;
		for (int b = 0; b < 4; ++b) {
			if (b > 0)
				bank.setTargets(in.inc.data(), in.l.data(), nullptr, 1, kControlBlock, false);
			float out[kControlBlock];
			zero(out, kControlBlock);
			bank.render(out, nullptr, kControlBlock);
			for (int s = 0; s < kControlBlock; ++s) {
				const int n = b * kControlBlock + s;
				const double want = std::sin(newman(0) + double(n + 1) * double(w));
				maxErr = std::max(maxErr, std::abs(double(out[s]) - want));
			}
		}
		CHECK(maxErr < 1e-5);
	}
}

TEST_CASE("renormalisation holds the rotator's magnitude at 1 over 2,000 control blocks") {
	// The magnitude, not the phase: over 64,000 samples the phase carries the float rounding of the increment
	// itself, which no renormalisation can (or should) correct. The amplitude A of a unit sinusoid is recovered
	// exactly from two consecutive samples and the rotator's own angle w: A² = (y0² + y1² − 2·y0·y1·cos w)/sin² w.
	// Increments are kept away from 0 and π, where sin² w would amplify the samples' rounding.
	const float incs[] = {0.3f, 0.61f, 0.9f, 1.17f, 1.4f, 1.7f, 2.05f, 2.4f, 2.6f};
	static PartialBank bank;
	Inputs in;
	for (const float w : incs) {
		CAPTURE(w);
		bank.reset(kMaxPartials);
		in.inc[0] = w;
		in.l[0] = 1.0f;
		bank.snapNextTargets();
		float out[kControlBlock];
		for (int b = 0; b < 2000; ++b) {
			bank.setTargets(in.inc.data(), in.l.data(), nullptr, 1, kControlBlock, false);
			zero(out, kControlBlock);
			bank.render(out, nullptr, kControlBlock);
		}
		const double wt = bank.incrementForTest(0);
		double maxErr = 0.0;
		for (int s = 0; s + 1 < kControlBlock; ++s) {
			const double y0 = out[s], y1 = out[s + 1];
			const double a2 = (y0 * y0 + y1 * y1 - 2.0 * y0 * y1 * std::cos(wt)) / (std::sin(wt) * std::sin(wt));
			maxErr = std::max(maxErr, std::abs(std::sqrt(a2) - 1.0));
		}
		CHECK(maxErr < 1e-5);
	}
}

TEST_CASE("amplitudes ramp linearly to their targets over the block") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 2;
	const float w = 0.37f;
	in.inc[k] = w;
	in.l[k] = 0.5f;
	in.r[k] = 0.25f;
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, kControlBlock, true);
	// Nothing has moved before render: the ramp starts from the current amplitude (zero after reset).
	CHECK(bank.amplitudeForTest(k, 0) == 0.0f);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	CHECK(std::abs(bank.amplitudeForTest(k, 0) - 0.5f) < 1e-6f);
	CHECK(std::abs(bank.amplitudeForTest(k, 1) - 0.25f) < 1e-6f);
	// Sample n carries amplitude target·(n+1)/frames: the ramp's last sample lands on the target.
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		const double s = std::sin(newman(k) + double(n + 1) * double(w));
		const double gain = double(n + 1) / double(kControlBlock);
		maxErr = std::max(maxErr, std::abs(double(outL[n]) - 0.5 * gain * s));
		maxErr = std::max(maxErr, std::abs(double(outR[n]) - 0.25 * gain * s));
	}
	CHECK(maxErr < 1e-6);
	// A ramp over a SHORTER block than kControlBlock still lands on its target on its own last sample.
	in.l[k] = 0.1f;
	in.r[k] = 0.4f;
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, 8, true);
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, 8);
	CHECK(bank.amplitudeForTest(k, 0) == 0.1f);
	CHECK(bank.amplitudeForTest(k, 1) == 0.4f);
}

TEST_CASE("snap makes the first targets immediate") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 5;
	const float w = 0.9f;
	in.inc[k] = w;
	in.l[k] = 0.7f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, k + 1, kControlBlock, false);
	CHECK(bank.amplitudeForTest(k, 0) == 0.7f);
	float out[kControlBlock];
	zero(out, kControlBlock);
	bank.render(out, nullptr, kControlBlock);
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n)
		maxErr = std::max(maxErr, std::abs(double(out[n]) - 0.7 * std::sin(newman(k) + double(n + 1) * double(w))));
	CHECK(maxErr < 1e-6);
	// The snap is consumed: the next targets ramp.
	in.l[k] = 0.3f;
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, k + 1, kControlBlock, false);
	CHECK(bank.amplitudeForTest(k, 0) == 0.7f);
	zero(out, kControlBlock);
	bank.render(out, nullptr, kControlBlock);
	const double s0 = std::sin(newman(k) + double(kControlBlock + 1) * double(w));
	CHECK(std::abs(double(out[0]) - (0.7 + (0.3 - 0.7) / kControlBlock) * s0) < 1e-6);
	CHECK(bank.amplitudeForTest(k, 0) == 0.3f);
}

TEST_CASE("the Nyquist fade is 1 below 0.90π, 0 above 0.98π, and continuous between") {
	// Restated: linear in the increment between the two edges; no partial at all for an increment <= 0.
	auto want = [](double w) {
		const double a = 0.90 * kPi, b = 0.98 * kPi;
		if (w <= 0.0)
			return 0.0;
		if (w <= a)
			return 1.0;
		if (w >= b)
			return 0.0;
		return (b - w) / (b - a);
	};
	const int steps = 20000;
	const double top = 1.5 * kPi;
	CHECK(nyquistFade(0.0f) == 0.0f);
	CHECK(nyquistFade(-0.5f) == 0.0f);
	CHECK(nyquistFade(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
	double prev = nyquistFade(float(top / steps));
	double maxJump = 0.0, maxErr = 0.0;
	for (int i = 1; i <= steps; ++i) {
		const float w = float(top * i / steps);
		const double g = nyquistFade(w);
		maxErr = std::max(maxErr, std::abs(g - want(w)));
		maxJump = std::max(maxJump, std::abs(g - prev));
		prev = g;
		if (w < 0.90f * float(kPi))
			CHECK(g == 1.0);
		if (w > 0.98f * float(kPi))
			CHECK(g == 0.0);
	}
	CHECK(maxErr < 1e-5);
	// Continuous: no step larger than the line's slope allows for one sweep step (plus rounding).
	const double slopeStep = (top / steps) / (0.08 * kPi);
	CHECK(maxJump < slopeStep * 1.01 + 1e-6);

	// And setTargets applies it: a snapped partial lands on target × fade, read from the bank.
	static PartialBank bank;
	const float probes[] = {0.5f, 0.92f * float(kPi), 0.94f * float(kPi), 0.97f * float(kPi), 0.99f * float(kPi)};
	for (const float w : probes) {
		CAPTURE(w);
		bank.reset(kMaxPartials);
		Inputs in;
		in.inc[0] = w;
		in.l[0] = 0.8f;
		in.r[0] = 0.6f;
		bank.snapNextTargets();
		bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 1, kControlBlock, true);
		CHECK(std::abs(bank.amplitudeForTest(0, 0) - 0.8 * want(w)) < 1e-5);
		CHECK(std::abs(bank.amplitudeForTest(0, 1) - 0.6 * want(w)) < 1e-5);
		// At or above the top edge the partial is silent and not rendered.
		if (w >= 0.98f * float(kPi))
			CHECK(bank.liveForTest() == 0);
		else
			CHECK(bank.liveForTest() == 4);
	}
}

TEST_CASE("a partial whose target is exactly zero in both ears and has ramped to zero is not counted live") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	CHECK(bank.liveForTest() == 0);
	Inputs in;
	for (int k = 0; k < kMaxPartials; ++k)
		in.inc[k] = 0.01f * float(k + 1);
	in.l[0] = 1.0f;
	in.r[9] = 0.5f; // the highest sounding partial is 9, in the R ear only
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), kMaxPartials, kControlBlock, true);
	CHECK(bank.liveForTest() == 12);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	// Partial 9's target goes to zero: it is still live while it ramps out...
	in.r[9] = 0.0f;
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), kMaxPartials, kControlBlock, true);
	CHECK(bank.liveForTest() == 12);
	bank.render(outL, outR, kControlBlock);
	CHECK(bank.amplitudeForTest(9, 1) == 0.0f);
	// ...and once at zero in both ears it is not counted.
	CHECK(bank.liveForTest() == 4);
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), kMaxPartials, kControlBlock, true);
	CHECK(bank.liveForTest() == 4);
	// A count that shrinks past a sounding partial ramps it out rather than cutting it.
	in.l[6] = 0.3f;
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), kMaxPartials, kControlBlock, true);
	bank.render(outL, outR, kControlBlock);
	CHECK(bank.liveForTest() == 8);
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 4, kControlBlock, true);
	CHECK(bank.liveForTest() == 8);
	bank.render(outL, outR, kControlBlock);
	CHECK(bank.amplitudeForTest(6, 0) == 0.0f);
	CHECK(bank.liveForTest() == 4);
}

TEST_CASE("mono renders only outL; stereo writes outR from the R amplitudes") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 1;
	const float w = 0.45f;
	in.inc[k] = w;
	in.l[k] = 1.0f;
	in.r[k] = 0.0f;
	// Mono: outR is not touched (a sentinel survives), and the R array is ignored.
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, k + 1, kControlBlock, false);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock, 0.25f);
	bank.render(outL, outR, kControlBlock);
	for (int n = 0; n < kControlBlock; ++n)
		CHECK(outR[n] == 0.25f);
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n)
		maxErr = std::max(maxErr, std::abs(double(outL[n]) - std::sin(newman(k) + double(n + 1) * double(w))));
	CHECK(maxErr < 1e-6);
	// In mono the R ear IS the L ear.
	CHECK(bank.amplitudeForTest(k, 1) == 1.0f);

	// Stereo with L 1 and R 0: outR is exactly zero, outL is the sinusoid. Switching from mono, R ramps from
	// where L was, so snap to put R straight on its target.
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, kControlBlock, true);
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		CHECK(outR[n] == 0.0f);
		const double s = std::sin(newman(k) + double(kControlBlock + n + 1) * double(w));
		maxErr = std::max(maxErr, std::abs(double(outL[n]) - s));
	}
	CHECK(maxErr < 1e-5);

	// Mono -> stereo without a snap: R starts from L's amplitude (in mono R is L) and ramps to its own target.
	bank.reset(kMaxPartials);
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, k + 1, kControlBlock, false);
	bank.render(outL, nullptr, kControlBlock);
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, kControlBlock, true);
	CHECK(bank.amplitudeForTest(k, 1) == 1.0f);
	bank.render(outL, outR, kControlBlock);
	CHECK(bank.amplitudeForTest(k, 1) == 0.0f);
}

TEST_CASE("stereo -> mono -> stereo leaves no stale R amplitude behind") {
	// Partial 9 sounds in R only; the voice goes mono with its target zero. In mono R is L, so once the ramp is done
	// partial 9 is zero in both ears: not live, and back in stereo its R ramps from L's zero, not from the old 0.5.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const float w0 = 0.2f, w9 = 0.83f;
	in.inc[0] = w0;
	in.inc[9] = w9;
	in.l[0] = 1.0f;
	in.r[0] = 1.0f;
	in.r[9] = 0.5f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 10, kControlBlock, true);
	CHECK(bank.liveForTest() == 12);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock); // partial 9 rendered: 32 samples
	// Mono, partial 9's target zero: live while the ramp runs, then gone.
	in.r[9] = 0.0f;
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 10, kControlBlock, false);
	CHECK(bank.liveForTest() == 12);
	bank.render(outL, nullptr, kControlBlock); // partial 9 rendered: 64 samples
	CHECK(bank.liveForTest() == 4);
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 10, kControlBlock, false);
	CHECK(bank.liveForTest() == 4);
	bank.render(outL, nullptr, kControlBlock); // partial 9 not rendered: its rotator holds
	CHECK(bank.liveForTest() == 4);
	// Stereo again, partial 9 back in R at 0.3 (a ramp, no snap): it starts from L's amplitude, zero.
	in.r[9] = 0.3f;
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 10, kControlBlock, true);
	CHECK(bank.amplitudeForTest(9, 1) == bank.amplitudeForTest(9, 0));
	CHECK(bank.amplitudeForTest(9, 1) == 0.0f);
	CHECK(bank.amplitudeForTest(0, 1) == bank.amplitudeForTest(0, 0));
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	// By value: R = partial 0 at 1 + partial 9 ramping 0 -> 0.3; L = partial 0 alone (partial 9's L is zero).
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		const double s0 = std::sin(newman(0) + double(3 * kControlBlock + n + 1) * double(w0));
		const double s9 = std::sin(newman(9) + double(2 * kControlBlock + n + 1) * double(w9));
		const double gain = double(n + 1) / double(kControlBlock);
		maxErr = std::max(maxErr, std::abs(double(outR[n]) - (s0 + 0.3 * gain * s9)));
		maxErr = std::max(maxErr, std::abs(double(outL[n]) - s0));
	}
	CHECK(maxErr < 1e-5);
}

TEST_CASE("a mono sum never cancels") {
	// L and R targets differ; the phase is shared, so outL + outR is ONE sinusoid of amplitude ampL + ampR.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 7;
	const float w = 1.234f;
	in.inc[k] = w;
	in.l[k] = 0.8f;
	in.r[k] = 0.3f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, kControlBlock, true);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		const double s = std::sin(newman(k) + double(n + 1) * double(w));
		maxErr = std::max(maxErr, std::abs(double(outL[n]) + double(outR[n]) - (0.8 + 0.3) * s));
		maxErr = std::max(maxErr, std::abs(double(outR[n]) - 0.3 * s));
	}
	CHECK(maxErr < 1e-6);
}

TEST_CASE("after amplitudes reach zero the output is EXACTLY 0.0f, not a denormal") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	// Several partials at amplitudes whose ramp to zero does not accumulate to zero in float.
	const float amps[] = {0.7123f, 0.3331f, 0.0917f, 0.55555f, 0.123457f, 0.98765f};
	for (int k = 0; k < 6; ++k) {
		in.inc[k] = 0.11f * float(k + 1);
		in.l[k] = amps[k];
		in.r[k] = amps[5 - k];
	}
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 6, kControlBlock, true);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	// Ramp everything to zero: the ramp's last sample carries amplitude exactly 0, so it is exactly 0.0f...
	Inputs off;
	for (int k = 0; k < 6; ++k)
		off.inc[k] = in.inc[k];
	bank.setTargets(off.inc.data(), off.l.data(), off.r.data(), 6, kControlBlock, true);
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	for (int n = 0; n < kControlBlock; ++n) {
		CHECK(std::fpclassify(outL[n]) != FP_SUBNORMAL);
		CHECK(std::fpclassify(outR[n]) != FP_SUBNORMAL);
	}
	CHECK(outL[kControlBlock - 1] == 0.0f);
	CHECK(outR[kControlBlock - 1] == 0.0f);
	for (int k = 0; k < 6; ++k) {
		CHECK(bank.amplitudeForTest(k, 0) == 0.0f);
		CHECK(bank.amplitudeForTest(k, 1) == 0.0f);
	}
	// ...and every block after is exactly zero, with nothing live.
	for (int b = 0; b < 4; ++b) {
		bank.setTargets(off.inc.data(), off.l.data(), off.r.data(), 6, kControlBlock, true);
		zero(outL, kControlBlock);
		zero(outR, kControlBlock);
		bank.render(outL, outR, kControlBlock);
		for (int n = 0; n < kControlBlock; ++n) {
			CHECK(outL[n] == 0.0f);
			CHECK(outR[n] == 0.0f);
		}
	}
	CHECK(bank.liveForTest() == 0);

	// A target too small to hear (and small enough to become a denormal on the way) is exactly zero.
	in.l[0] = 1e-40f;
	in.r[0] = 1e-12f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 1, kControlBlock, true);
	CHECK(bank.amplitudeForTest(0, 0) == 0.0f);
	CHECK(bank.amplitudeForTest(0, 1) == 0.0f);
	CHECK(bank.liveForTest() == 0);
}

TEST_CASE("a ramp split across two renders carries on where the first left off") {
	// A 32-frame ramp rendered as 16 + 16, then a steady 16: amplitude 0.5·(t+1)/32 for t < 32, then 0.5.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 3;
	const float w = 0.6f;
	in.inc[k] = w;
	in.l[k] = 0.5f;
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, k + 1, kControlBlock, false);
	float out[48];
	zero(out, 48);
	bank.render(out, nullptr, 16);
	CHECK(std::abs(bank.amplitudeForTest(k, 0) - 0.25f) < 1e-6f);
	bank.render(out + 16, nullptr, 16);
	CHECK(bank.amplitudeForTest(k, 0) == 0.5f);
	bank.render(out + 32, nullptr, 16);
	CHECK(bank.amplitudeForTest(k, 0) == 0.5f);
	double maxErr = 0.0;
	for (int t = 0; t < 48; ++t) {
		const double amp = t < 32 ? 0.5 * double(t + 1) / 32.0 : 0.5;
		maxErr = std::max(maxErr, std::abs(double(out[t]) - amp * std::sin(newman(k) + double(t + 1) * double(w))));
	}
	CHECK(maxErr < 1e-6);
}

TEST_CASE("a ramp shorter than the render lands on its target and the rest of the render holds it") {
	// setTargets over 8 frames, then one 32-sample render: 8 ramped samples, then 24 steady ones, in place.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 2;
	const float w = 0.47f;
	in.inc[k] = w;
	in.l[k] = 0.2f;
	in.r[k] = 0.9f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, kControlBlock, true);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	in.l[k] = 0.6f;
	in.r[k] = 0.1f;
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), k + 1, 8, true);
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		const double s = std::sin(newman(k) + double(kControlBlock + n + 1) * double(w));
		const double g = n < 8 ? double(n + 1) / 8.0 : 1.0;
		maxErr = std::max(maxErr, std::abs(double(outL[n]) - (0.2 + (0.6 - 0.2) * g) * s));
		maxErr = std::max(maxErr, std::abs(double(outR[n]) - (0.9 + (0.1 - 0.9) * g) * s));
	}
	CHECK(maxErr < 1e-6);
}

TEST_CASE("a partial dropped from count ramps out at the increment it had") {
	// Partial 5 at 0.3, then count drops to 4. The caller's array now holds a different increment for 5, which the
	// bank must not read: out[n] = 0.3·(1 − (n+1)/32)·sin(φ5 + t·w), t counting on from the first block.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	const int k = 5;
	const float w = 0.77f;
	in.inc[k] = w;
	in.l[k] = 0.3f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, k + 1, kControlBlock, false);
	float out[kControlBlock];
	zero(out, kControlBlock);
	bank.render(out, nullptr, kControlBlock);
	in.inc[k] = 2.0f;
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 4, kControlBlock, false);
	CHECK(std::abs(bank.incrementForTest(k) - w) < 2e-7f);
	zero(out, kControlBlock);
	bank.render(out, nullptr, kControlBlock);
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		const double want = 0.3 * (1.0 - double(n + 1) / kControlBlock)
		                    * std::sin(newman(k) + double(kControlBlock + n + 1) * double(w));
		maxErr = std::max(maxErr, std::abs(double(out[n]) - want));
	}
	CHECK(maxErr < 1e-6);
	CHECK(bank.liveForTest() == 0);
}

TEST_CASE("Newman phases follow the note's partial count, at high k too") {
	// One partial from reset(N), against π·k²/N restated in double without any reduction.
	struct Probe {
		int k, n;
	};
	const Probe probes[] = {{25, kMaxPartials}, {300, kMaxPartials}, {25, 40}, {7, 12}, {3, 0}};
	static PartialBank bank;
	for (const Probe& p : probes) {
		CAPTURE(p.k);
		CAPTURE(p.n);
		bank.reset(p.n);
		Inputs in;
		const float w = 0.4f;
		in.inc[p.k] = w;
		in.l[p.k] = 1.0f;
		bank.snapNextTargets();
		bank.setTargets(in.inc.data(), in.l.data(), nullptr, p.k + 1, kControlBlock, false);
		float out[kControlBlock];
		zero(out, kControlBlock);
		bank.render(out, nullptr, kControlBlock);
		const int n = p.n > 1 ? p.n : 1;
		double maxErr = 0.0;
		for (int s = 0; s < kControlBlock; ++s)
			maxErr = std::max(maxErr, std::abs(double(out[s]) - std::sin(newman(p.k, n) + double(s + 1) * double(w))));
		CHECK(maxErr < 1e-6);
	}
}

TEST_CASE("a zero or negative increment renders exact silence, not DC") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	in.inc[0] = -0.3f;
	in.inc[1] = 0.0f;
	in.inc[2] = std::numeric_limits<float>::quiet_NaN();
	in.l[0] = in.l[1] = in.l[2] = 1.0f;
	in.r[0] = in.r[1] = in.r[2] = 1.0f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 3, kControlBlock, true);
	CHECK(bank.liveForTest() == 0);
	float outL[kControlBlock], outR[kControlBlock];
	zero(outL, kControlBlock);
	zero(outR, kControlBlock);
	bank.render(outL, outR, kControlBlock);
	for (int n = 0; n < kControlBlock; ++n) {
		CHECK(outL[n] == 0.0f);
		CHECK(outR[n] == 0.0f);
	}
	// A non-finite target is no partial either.
	in.inc[0] = 0.5f;
	in.l[0] = std::numeric_limits<float>::infinity();
	in.r[0] = std::numeric_limits<float>::quiet_NaN();
	bank.setTargets(in.inc.data(), in.l.data(), in.r.data(), 1, kControlBlock, true);
	CHECK(bank.liveForTest() == 0);
}

TEST_CASE("a count drop 7 -> 5 inside one group keeps the dropped partials at the increments they had") {
	// Partials 0..6 sounding, then count 5: the group 4..7 is RAGGED (lane 4 named, lanes 5..7 not), so its new
	// coefficient is computed for the whole group and only the named lane may take it. Partial 6 ramps out at its OLD
	// increment: out[n] = 0.3·(1 − (n+1)/32)·sin(φ6 + t·w6), t counting on from the first block. Partial 6 alone is
	// given an amplitude, so the output is its sinusoid alone.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	for (int i = 0; i < 7; ++i)
		in.inc[i] = 0.1f + 0.05f * float(i);
	const int k = 6;
	const float w = in.inc[k];
	in.l[k] = 0.3f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 7, kControlBlock, false);
	float out[kControlBlock];
	zero(out, kControlBlock);
	bank.render(out, nullptr, kControlBlock);
	// The caller's array past the new count holds something else, which the bank must neither read nor compute from.
	for (int i = 0; i < 7; ++i)
		in.inc[i] = 2.5f;
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 5, kControlBlock, false);
	CHECK(std::abs(bank.incrementForTest(k) - w) < 2e-7f);
	CHECK(std::abs(bank.incrementForTest(5) - (0.1f + 0.05f * 5.0f)) < 2e-7f);
	CHECK(std::abs(bank.incrementForTest(4) - 2.5f) < 2e-7f); // the named lane DID move
	zero(out, kControlBlock);
	bank.render(out, nullptr, kControlBlock);
	double maxErr = 0.0;
	for (int n = 0; n < kControlBlock; ++n) {
		const double want = 0.3 * (1.0 - double(n + 1) / kControlBlock)
		                    * std::sin(newman(k) + double(kControlBlock + n + 1) * double(w));
		maxErr = std::max(maxErr, std::abs(double(out[n]) - want));
	}
	CHECK(maxErr < 1e-6);
}

TEST_CASE("rotate turns each named partial by ratio times 2π times the move, and nothing else") {
	// Partials at ratios a harmonic, an off-grid and a high one, then a move of `cycles`: each phase is its Newman
	// phase plus ratio·2π·cycles (mod 2π), restated in double. Partial 9 is past `count` and must not move; the bank's
	// increments and amplitudes are untouched.
	static PartialBank bank;
	const int n = 40;
	bank.reset(n);
	Inputs in;
	std::array<float, kMaxPartials> ratio {};
	const int probes[] = {0, 1, 4, 7, 8};
	ratio[0] = 1.0f;
	ratio[1] = 2.0f;
	ratio[4] = 5.37f;   // off the grid
	ratio[7] = 318.0f;  // high: the angle is hundreds of turns before the reduction
	ratio[8] = 141.25f; // off the grid and high
	ratio[9] = 3.0f;    // past count
	for (const int i : probes)
		in.inc[i] = 0.01f, in.l[i] = 0.5f;
	in.inc[9] = 0.01f, in.l[9] = 0.5f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 10, kControlBlock, false);
	const float before9 = bank.phaseForTest(9);
	for (const float cycles : {0.137f, -0.61f, 2.3f}) {
		CAPTURE(cycles);
		bank.reset(n);
		bank.snapNextTargets();
		bank.setTargets(in.inc.data(), in.l.data(), nullptr, 10, kControlBlock, false);
		bank.rotate(ratio.data(), cycles, 9);
		for (const int i : probes) {
			CAPTURE(i);
			const double want = newman(i, n) + 2.0 * kPi * double(ratio[i]) * double(cycles);
			const double got = double(bank.phaseForTest(i));
			const double d = std::remainder(got - want, 2.0 * kPi);
			// The float product ratio·cycles and its reduction each round in the last place of a number of TURNS as large
			// as ratio·cycles (731 turns for 318 × 2.3: an ulp of 6e-5 turns), so the bound scales with it.
			const double tol = 2.0 * kPi * std::abs(double(ratio[i]) * double(cycles)) * 1.2e-7 + 2e-5;
			CHECK(std::abs(d) < tol);
			CHECK(std::abs(bank.incrementForTest(i) - 0.01f) < 2e-7f);
			CHECK(bank.amplitudeForTest(i, 0) == 0.5f);
		}
		CHECK(bank.phaseForTest(9) == before9);
	}
}

TEST_CASE("the weighted reset is Schroeder's law, and flat power reduces it to Newman's quadratic mirrored") {
	// φ_k = −2π·Σ_{l<k} (k − l)·p_l restated by the double sum (not the running sums the bank uses), p = power / Σ power
	// over the first `count`. Flat: φ_k = −π·k(k+1)/N, i.e. −(Newman's π·k²/N) − π·k/N (a time reversal and a shift).
	static PartialBank bank;
	std::array<float, kMaxPartials> power {};
	SUBCASE("flat") {
		for (const int n : {12, 89, 320}) {
			CAPTURE(n);
			for (int k = 0; k < n; ++k)
				power[k] = 0.37f;
			bank.reset(power.data(), n);
			for (const int k : {0, 1, 5, 11, n - 1}) { // inside the count, where the flat law is the quadratic
				CAPTURE(k);
				const double flat = -kPi * double(k) * double(k + 1) / double(n);
				const double asNewman = -(newman(k, n) + kPi * double(k) / double(n));
				CHECK(std::abs(std::remainder(flat - asNewman, 2.0 * kPi)) < 1e-9);
				CHECK(std::abs(std::remainder(double(bank.phaseForTest(k)) - flat, 2.0 * kPi)) < 2e-6);
			}
		}
	}
	SUBCASE("weighted") {
		const int n = 38; // the last named group is 36..39: lanes 38 and 39 continue the law with no further power
		for (int k = 0; k < n; ++k)
			power[k] = 1.0f / float((k + 1) * (k + 1)) + (k % 3 == 0 ? 0.02f : 0.0f); // a falling, uneven spectrum
		bank.reset(kMaxPartials); // an earlier note's phases, which a partial past the named groups keeps
		const float before60 = bank.phaseForTest(60), before319 = bank.phaseForTest(319);
		bank.reset(power.data(), n);
		CHECK(bank.phaseForTest(60) == before60);
		CHECK(bank.phaseForTest(319) == before319);
		double total = 0.0;
		for (int k = 0; k < n; ++k)
			total += power[k];
		for (const int k : {0, 1, 2, 7, 23, 37, 38, 39}) {
			CAPTURE(k);
			double turns = 0.0;
			for (int l = 0; l < std::min(k, n); ++l)
				turns += double(k - l) * double(power[l]) / total;
			const double want = -2.0 * kPi * turns;
			CHECK(std::abs(std::remainder(double(bank.phaseForTest(k)) - want, 2.0 * kPi)) < 2e-6);
		}
	}
	SUBCASE("no power is the flat Newman reset") {
		bank.reset(power.data(), 17);
		for (const int k : {0, 3, 16})
			CHECK(std::abs(std::remainder(double(bank.phaseForTest(k)) - newman(k, 17), 2.0 * kPi)) < 2e-6);
	}
}

TEST_CASE("the Phase control's bend turns the rotator by increment plus ratio times bend, the fade stays the increment's") {
	// Three partials: one well inside the band, one just below the fade (bent above 0.90π: still full), one past the fade
	// (bent below 0.98π: still silent). The rotator's angle is the bent one; the amplitude is the un-bent fade's.
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	std::array<float, kMaxPartials> ratio {};
	in.inc[0] = 0.3f, ratio[0] = 1.0f;
	in.inc[1] = 0.89f * float(kPi), ratio[1] = 9.0f;
	in.inc[2] = 0.99f * float(kPi), ratio[2] = 10.0f;
	in.l[0] = in.l[1] = in.l[2] = 0.5f;
	const float bend = 0.004f;
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 3, kControlBlock, false, ratio.data(), bend);
	CHECK(std::abs(bank.incrementForTest(0) - (0.3f + 1.0f * bend)) < 2e-7f);
	CHECK(std::abs(bank.incrementForTest(1) - (0.89f * float(kPi) + 9.0f * bend)) < 4e-7f);
	CHECK(bank.amplitudeForTest(0, 0) == 0.5f);
	CHECK(bank.amplitudeForTest(1, 0) == 0.5f); // 0.89π is below the fade, whatever the bend makes the slope
	CHECK(bank.amplitudeForTest(2, 0) == 0.0f); // 0.99π is past it
	// A partial up in the fade (0.97π, gain 0.125) bent past π: its rotation is the bent angle less 2π (wrapped), not π.
	in.inc[3] = 0.97f * float(kPi), ratio[3] = 30.0f, in.l[3] = 0.5f;
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 4, kControlBlock, false, ratio.data(), bend);
	const double bent3 = double(0.97f * float(kPi)) + 30.0 * double(bend);
	REQUIRE(bent3 > kPi);
	CHECK(std::abs(double(bank.incrementForTest(3)) - (bent3 - 2.0 * kPi)) < 1e-6);
	// And a zero bend (or none) is the increment exactly.
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 3, kControlBlock, false, ratio.data(), 0.0f);
	CHECK(std::abs(bank.incrementForTest(0) - 0.3f) < 2e-7f);
}

TEST_CASE("rotate turns only the live groups: a partial that is not rendered keeps its phase") {
	static PartialBank bank;
	bank.reset(kMaxPartials);
	Inputs in;
	std::array<float, kMaxPartials> ratio {};
	for (int i = 0; i < kMaxPartials; ++i)
		ratio[i] = float(i + 1);
	in.inc[1] = 0.2f, in.l[1] = 0.5f; // live_ = 4: partials 0..3
	bank.snapNextTargets();
	bank.setTargets(in.inc.data(), in.l.data(), nullptr, 2, kControlBlock, false);
	REQUIRE(bank.liveForTest() == 4);
	const float before8 = bank.phaseForTest(8), before1 = bank.phaseForTest(1);
	bank.rotate(ratio.data(), 0.1f, kMaxPartials);
	CHECK(bank.phaseForTest(8) == before8);
	CHECK(std::abs(std::remainder(double(bank.phaseForTest(1)) - double(before1) - 2.0 * kPi * 2.0 * 0.1, 2.0 * kPi)) < 2e-6);
}
