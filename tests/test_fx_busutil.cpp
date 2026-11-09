/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/busutil/busutil_meter.h"
#include "fx/busutil/busutil_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace bu = feathers::fx::busutil;

namespace {

/// THE RATE EVERY `Instance` IS PREPARED AT — named because the ramp length is DERIVED from it, and
/// a case that hard-coded 240 samples would be pinning an arithmetic coincidence rather than a rule.
///
/// DECLARED BEFORE THE CLASS BECAUSE A MEMBER FUNCTION BODY LOOKS NAMES UP AT THE END OF THE CLASS,
/// not at the end of the file — a constant declared below the class is not visible to a constructor
/// defined inside it.
constexpr double kSampleRate = 48000.0;

/// AN OPS INSTANCE, CONSTRUCTED THE WAY A HOST CONSTRUCTS ONE — into storage the test owns, because
/// that is the contract `AudioOps` states and a test that new'd the struct itself would be exercising
/// a road no host uses.
class Instance {
public:
	Instance() {
		REQUIRE(bu::kAudioOps.stateBytes <= storage_.size());
		REQUIRE(bu::kAudioOps.stateAlign <= alignof(std::max_align_t));
		bu::kAudioOps.construct(storage_.data());
		bu::kAudioOps.prepare(storage_.data(), kSampleRate, 4096);
	}
	~Instance() { bu::kAudioOps.destruct(storage_.data()); }

	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	void set(const bu::Settings& s) {
		const bu::Row row = bu::rowOf(s);
		bu::kAudioOps.setFields(storage_.data(), row.data(), row.size());
	}

	/// PREPARED AGAIN AT ANOTHER RATE — what a host does when its rate changes (backlog EH).
	void prepareAt(double sampleRate) { bu::kAudioOps.prepare(storage_.data(), sampleRate, 4096); }

	/// A SHORT ROW ON PURPOSE — the road that decides whether an absent field lands on its DECLARED
	/// default or on zero.
	void setPartial(const bu::Row& row, size_t fieldsPresent) {
		bu::kAudioOps.setFields(storage_.data(), row.data(), fieldsPresent);
	}

	void process(std::vector<float>& left, std::vector<float>& right) {
		REQUIRE(left.size() == right.size());
		bu::kAudioOps.processStereo(storage_.data(), left.data(), right.data(),
		                            static_cast<int>(left.size()));
	}

	void reset() { bu::kAudioOps.reset(storage_.data()); }

	[[nodiscard]] bu::Matrix matrix() const { return bu::matrixForTest(storage_.data()); }
	/// THE OPS STORAGE, for the readings the meter holds — the same seam `matrixForTest` uses, and
	/// for the same reason: what a meter has measured is not recoverable from the audio it measured.
	[[nodiscard]] const void* storageForTest() const { return storage_.data(); }

private:
	// THE METER'S WINDOWS AND HISTOGRAM MAKE THIS DEVICE FOUR KILOBYTES, not the four coefficients
	// it started as. The REQUIRE in the constructor is what would say so if it grew past here.
	alignas(std::max_align_t) std::array<std::byte, 16384> storage_ {};
};

/// THE REFERENCE MATRIX, COMPUTED BY AN INDEPENDENT RESTATEMENT OF THE INTENDED PATH — not by calling
/// `matrixOf`, which is the thing under test. CLAUDE.md: compute the reference by an independent
/// restatement, and keep a difference check only as a SECOND assertion proving the first is not
/// vacuous.
bu::Matrix reference(const bu::Settings& s) {
	if (!s.enabled) {
		return bu::Matrix {};
	}
	// Written as four scalars per stage and multiplied out by hand, in the order the scoping doc
	// names: I, then W, then D, then P, then G.
	const double iL = s.invertL ? -1.0 : 1.0;
	const double iR = s.invertR ? -1.0 : 1.0;

	const double w = static_cast<double>(s.width);
	const double wSum = (1.0 + w) * 0.5;
	const double wDif = (1.0 - w) * 0.5;

	const auto place = [](double pos) {
		if (pos <= -1.0) {
			return std::pair<double, double> { 1.0, 0.0 };
		}
		if (pos >= 1.0) {
			return std::pair<double, double> { 0.0, 1.0 };
		}
		const double theta = (pos + 1.0) * (std::acos(-1.0) / 4.0);
		return std::pair<double, double> { std::cos(theta), std::sin(theta) };
	};
	const auto [dLl, dLr] = place(static_cast<double>(s.panL));
	const auto [dRl, dRr] = place(static_cast<double>(s.panR));

	const double p = static_cast<double>(s.pan);
	const double pL = p <= 0.0 ? 1.0 : 1.0 - p;
	const double pR = p >= 0.0 ? 1.0 : 1.0 + p;

	// THE FADER CURVE, RESTATED — `(2v)^2`, not a call to `faderGain`, which is the thing under test.
	const double g = (2.0 * static_cast<double>(s.gain)) * (2.0 * static_cast<double>(s.gain));

	// W . I
	const double a11 = wSum * iL, a12 = wDif * iR;
	const double a21 = wDif * iL, a22 = wSum * iR;
	// D . (W . I) — the left input's placement is the left COLUMN of D
	const double b11 = dLl * a11 + dRl * a21, b12 = dLl * a12 + dRl * a22;
	const double b21 = dLr * a11 + dRr * a21, b22 = dLr * a12 + dRr * a22;
	// G . P . (…)
	return bu::Matrix { .ll = static_cast<float>(g * pL * b11),
		                .lr = static_cast<float>(g * pL * b12),
		                .rl = static_cast<float>(g * pR * b21),
		                .rr = static_cast<float>(g * pR * b22) };
}

/// A SIGNAL WHOSE TWO CHANNELS ARE DIFFERENT AND NEITHER IS ZERO, so no assertion below can pass by a
/// channel happening to be silent and no swapped-channel bug can hide behind symmetry.
void fill(std::vector<float>& left, std::vector<float>& right) {
	for (size_t i = 0; i < left.size(); ++i) {
		left[i] = 0.25f + 0.5f * static_cast<float>(i % 7);
		right[i] = -0.75f + 0.25f * static_cast<float>(i % 5);
	}
}

/// **THE FADER POSITION THAT DOUBLES.** `(2v)^2 == 2` at `v = 1/sqrt(2)`, so this is the position
/// the old `kSixDb` was: the same +6.02 dB, expressed in what the control now IS.
const float kDoublePosition = 0.5f * static_cast<float>(std::sqrt(2.0));

} // namespace

TEST_CASE("a Bus Utility at its defaults renders the bits that were there before it existed") {
	// **THE CLAIM THIS DEVICE MOST NEEDS TO BE TRUE**, and it is asserted SAMPLE-EXACT rather than
	// within a tolerance. By design an added effect arrives switched ON, so a fresh Bus
	// Utility is in the signal from the moment it is dropped in — and if its defaults were merely
	// nearly the identity, adding one would quietly change the mix.
	//
	// A tolerance here would also be a number nobody could justify, and it is exactly the sort of
	// slack that later hides a real defect.
	Instance inst;
	inst.set(bu::Settings {});

	std::vector<float> left(64), right(64);
	fill(left, right);
	const std::vector<float> beforeL = left;
	const std::vector<float> beforeR = right;

	inst.process(left, right);

	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == beforeL[i]);
		CHECK(right[i] == beforeR[i]);
	}
}

TEST_CASE("the identity is exact in the MATRIX too, which is where cos(pi/2) would have shown") {
	// THE VALUE, NOT THE RENDER. The case above could pass with an off-diagonal of 6e-17 if the
	// signal happened to be small enough to round it away; this one cannot.
	//
	// THE DEFAULT DUAL PAN IS THE PLACE THIS NEARLY WENT WRONG. Pan L at -1 and Pan R at +1 built
	// from cos/sin alone gives `cos(pi/2) = 6.1e-17`, not 0 — the endpoints are special-cased in
	// `placeMono` for exactly this, and this assertion is what pins that.
	Instance inst;
	inst.set(bu::Settings {});

	const bu::Matrix m = inst.matrix();
	CHECK(m.ll == 1.0f);
	CHECK(m.lr == 0.0f);
	CHECK(m.rl == 0.0f);
	CHECK(m.rr == 1.0f);
}

TEST_CASE("a disabled Bus Utility passes every sample through untouched") {
	Instance inst;
	bu::Settings s;
	s.enabled = false;
	// SETTINGS THAT WOULD BE LOUD IF THEY LEAKED — a disabled device holding an identity matrix is
	// only interesting if the state it is ignoring would have been audible.
	s.gain = 1.0f;
	s.width = 0.0f;
	s.pan = 0.8f;
	s.invertL = true;
	inst.set(s);

	std::vector<float> left(32), right(32);
	fill(left, right);
	const std::vector<float> beforeL = left;
	const std::vector<float> beforeR = right;

	inst.process(left, right);

	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == beforeL[i]);
		CHECK(right[i] == beforeR[i]);
	}
}

TEST_CASE("the fader is unity at its CENTRE, silent at the bottom, and +12 dB at the top") {
	// **THE TAPER'S WHOLE CLAIM, AS THREE NUMBERS:** unity at 12:00 with boost above. The
	// centre of a 0..1 range is where `Knob` puts the pointer straight up, so "twelve o'clock" is a
	// fact about the DEFAULT and the RANGE rather than anything the card draws.
	Instance inst;
	bu::Settings s;
	s.gain = kDoublePosition;
	inst.set(s);
	CHECK(inst.matrix().ll == doctest::Approx(2.0).epsilon(1e-6));
	CHECK(inst.matrix().lr == 0.0f);

	// **THE CENTRE IS EXACTLY UNITY, NOT 0.9999999**, and every sample-exact claim in this file rests
	// on it — `(2 * 0.5)^2` is exact in float, which the header pins with a static_assert as well.
	bu::Settings unity;
	unity.gain = static_cast<float>(bu::kFaderUnity);
	Instance atUnity;
	atUnity.set(unity);
	CHECK(atUnity.matrix().ll == 1.0f);

	// **THE BOTTOM IS SILENCE, AND IT IS EXACTLY ZERO** — the thing a dB-valued control could not
	// reach without a discontinuity spliced onto the end of its travel. This assertion is the reason
	// the unit exists.
	bu::Settings down;
	down.gain = 0.0f;
	Instance atZero;
	atZero.set(down);
	CHECK(atZero.matrix().ll == 0.0f);
	CHECK(atZero.matrix().rr == 0.0f);

	// AND THE TOP IS +12 dB, a gain of four.
	bu::Settings up;
	up.gain = 1.0f;
	Instance atTop;
	atTop.set(up);
	CHECK(atTop.matrix().ll == doctest::Approx(4.0f));

	// **THE TAPER IS MONOTONE AND HAS NO STEP IN IT**, which is the property required by name: no
	// discontinuity. Walked across the whole travel rather than sampled at the
	// ends, because a discontinuity is exactly what endpoints cannot see.
	float previous = -1.0f;
	for (int i = 0; i <= 100; ++i) {
		bu::Settings step;
		step.gain = static_cast<float>(i) / 100.0f;
		Instance at;
		at.set(step);
		const float g = at.matrix().ll;
		CAPTURE(i);
		CHECK(g >= previous);
		previous = g;
	}
}

TEST_CASE("Width is a mid/side scale: 0 is mono, 2 doubles the side") {
	// ASSERTED AS THE M/S ALGEBRA, not as "narrower". A test that checked the two channels grew more
	// similar would pass for a device that simply mixed them at the wrong ratio, or that attenuated
	// the side by leaking gain — the partial-effect blindness the value rule is about.
	Instance mono;
	bu::Settings s;
	s.width = 0.0f;
	mono.set(s);

	// At width 0 every coefficient is 1/2: both outputs are the mid, exactly.
	const bu::Matrix m = mono.matrix();
	CHECK(m.ll == 0.5f);
	CHECK(m.lr == 0.5f);
	CHECK(m.rl == 0.5f);
	CHECK(m.rr == 0.5f);

	// At width 2 the side doubles: out = M +- 2S, so 1.5 on the diagonal and -0.5 off it.
	s.width = 2.0f;
	Instance wide;
	wide.set(s);
	const bu::Matrix w = wide.matrix();
	CHECK(w.ll == 1.5f);
	CHECK(w.lr == -0.5f);
	CHECK(w.rl == -0.5f);
	CHECK(w.rr == 1.5f);
}

TEST_CASE("the mono collapse reads both inputs before it writes either output") {
	// THE ALIASING BUG THIS SHAPE INVITES, and it is SILENT AT THE DEFAULTS. With the off-diagonals
	// at zero — every default setting — writing `left[i]` and then reading it back for `right[i]`
	// gives the right answer. It only goes wrong once a control that couples the channels is moved,
	// which is why this case moves one.
	//
	// At width 0 both outputs must be (L+R)/2. A device that wrote left first and then used the NEW
	// left would produce (L+R)/2 on the left and ((L+R)/2 + R)/2 on the right.
	Instance inst;
	bu::Settings s;
	s.width = 0.0f;
	inst.set(s);

	std::vector<float> left { 1.0f, 0.5f, -0.25f };
	std::vector<float> right { -1.0f, 0.25f, 0.75f };
	const std::vector<float> beforeL = left;
	const std::vector<float> beforeR = right;
	inst.process(left, right);

	for (size_t i = 0; i < left.size(); ++i) {
		const float mid = 0.5f * (beforeL[i] + beforeR[i]);
		CHECK(left[i] == doctest::Approx(mid));
		CHECK(right[i] == doctest::Approx(mid));
	}
	// AND THE CHECK IS NOT VACUOUS: the two inputs differ everywhere, so a device that returned its
	// input unchanged would fail every line above.
	for (size_t i = 0; i < beforeL.size(); ++i) {
		CHECK(beforeL[i] != beforeR[i]);
	}
}

TEST_CASE("Invert L flips the left channel alone") {
	// TWO SWITCHES RATHER THAN ONE, which is the point: inverting BOTH is inaudible on a stereo bus,
	// so a single combined polarity control would be a control that does nothing. This case pins the
	// asymmetry that makes them worth having.
	Instance inst;
	bu::Settings s;
	s.invertL = true;
	inst.set(s);

	const bu::Matrix m = inst.matrix();
	CHECK(m.ll == -1.0f);
	CHECK(m.rr == 1.0f);
	CHECK(m.lr == 0.0f);
	CHECK(m.rl == 0.0f);

	// AND BOTH INVERTED IS A SIGN FLIP ON THE PAIR — still a real matrix, still not the identity,
	// which is what makes "inaudible in isolation" a statement about hearing rather than about maths.
	s.invertR = true;
	Instance both;
	both.set(s);
	CHECK(both.matrix().ll == -1.0f);
	CHECK(both.matrix().rr == -1.0f);
}

TEST_CASE("dual pan places each input channel by the constant-power law") {
	// THE LAW ITSELF, as a number. 0.7071 at the centre is what constant power MEANS, and a linear
	// law would put 0.5 there — a difference no rendered-buffer comparison distinguishes from a
	// slightly quieter mix.
	Instance inst;
	bu::Settings s;
	s.panL = 0.0f; // left input to the centre
	s.panR = 0.0f; // right input to the centre
	inst.set(s);

	const float centre = static_cast<float>(std::cos(std::acos(-1.0) / 4.0));
	CHECK(centre == doctest::Approx(0.70710678f));

	const bu::Matrix m = inst.matrix();
	CHECK(m.ll == doctest::Approx(centre));
	CHECK(m.lr == doctest::Approx(centre));
	CHECK(m.rl == doctest::Approx(centre));
	CHECK(m.rr == doctest::Approx(centre));

	// AND IT IS NOT THE LINEAR LAW. Stated as its own assertion because "0.707 not 0.5" is the whole
	// content of the choice.
	CHECK(m.ll != doctest::Approx(0.5f));
}

TEST_CASE("Pan is a BALANCE — unity at centre, attenuating one side") {
	// **THE SECOND LAW, AND THE REASON IT IS NOT THE FIRST ONE.** Constant power applied to a pair
	// that is already placed would drop a centred bus by 3 dB before a hand had touched the knob.
	// This case asserts the centre is unity, which is precisely what a shared helper would have
	// broken.
	Instance centred;
	bu::Settings s;
	s.pan = 0.0f;
	centred.set(s);
	CHECK(centred.matrix().ll == 1.0f);
	CHECK(centred.matrix().rr == 1.0f);
	CHECK(centred.matrix().ll != doctest::Approx(0.70710678f));

	// HARD RIGHT: the left is gone and the right is UNCHANGED, not boosted. A balance attenuates;
	// it does not make up the difference on the other side.
	s.pan = 1.0f;
	Instance right;
	right.set(s);
	CHECK(right.matrix().ll == 0.0f);
	CHECK(right.matrix().rr == 1.0f);

	// HALF LEFT: the right side is at 0.5 and the left is untouched.
	s.pan = -0.5f;
	Instance half;
	half.set(s);
	CHECK(half.matrix().ll == 1.0f);
	CHECK(half.matrix().rr == doctest::Approx(0.5f));
}

TEST_CASE("the composition order is polarity, width, dual pan, pan, gain — and a reorder shows") {
	// **THE ORDER IS AUDIBLE AND IT IS A CHOICE.** With a Stereo/Dual MODE switch it would never
	// arise, because only one pan would be live; the decision that both are live (pan is a knob of
	// its own) is what makes the order a fact a user can hear, and therefore a fact a test must pin.
	//
	// The setting below is chosen so the two candidate orders give DIFFERENT numbers. Width 0 makes
	// the mono matrix, Pan +0.5 makes a diagonal that is not a scalar — and a diagonal that is not a
	// scalar does not commute with an off-diagonal matrix. Both references are computed here by hand.
	bu::Settings s;
	s.width = 0.0f;
	s.pan = 0.5f;

	Instance inst;
	inst.set(s);
	const bu::Matrix m = inst.matrix();

	// P . W — the order this device declares: mono first, then the balance scales the two OUTPUTS.
	CHECK(m.ll == doctest::Approx(0.25f));
	CHECK(m.lr == doctest::Approx(0.25f));
	CHECK(m.rl == doctest::Approx(0.5f));
	CHECK(m.rr == doctest::Approx(0.5f));

	// W . P — the other order, where the balance scales the INPUTS and the mono collapse follows.
	// Different in three of four coefficients, which is what makes the assertions above a real claim
	// about order rather than about the numbers happening to work out.
	CHECK(m.lr != doctest::Approx(0.5f));
	CHECK(m.rl != doctest::Approx(0.25f));
}

TEST_CASE("every control together matches an independent restatement of the intended path") {
	// THE WHOLE COMPOSITION, against a reference written from the scoping doc rather than from the
	// implementation. Deliberately a setting where nothing is at a default and no two stages are
	// symmetric, so a transposed matrix, a swapped column or a dropped stage cannot survive.
	bu::Settings s;
	s.gain = 0.42f;
	s.pan = 0.37f;
	s.panL = -0.4f;
	s.panR = 0.62f;
	s.width = 1.6f;
	s.invertL = true;
	s.invertR = false;

	Instance inst;
	inst.set(s);
	const bu::Matrix m = inst.matrix();
	const bu::Matrix want = reference(s);

	CHECK(m.ll == doctest::Approx(want.ll).epsilon(1e-6));
	CHECK(m.lr == doctest::Approx(want.lr).epsilon(1e-6));
	CHECK(m.rl == doctest::Approx(want.rl).epsilon(1e-6));
	CHECK(m.rr == doctest::Approx(want.rr).epsilon(1e-6));

	// AND THE REFERENCE IS NOT VACUOUS — all four coefficients are distinct and none is zero, so a
	// device that returned the identity, a scalar, or a symmetric matrix would fail above.
	CHECK(want.ll != doctest::Approx(want.lr));
	CHECK(want.ll != doctest::Approx(want.rl));
	CHECK(want.ll != doctest::Approx(want.rr));
	CHECK(want.lr != doctest::Approx(want.rl));
}

TEST_CASE("a row that omits fields lands on DECLARED defaults, and here that is audible") {
	// device_row.h's rule, on the device where breaking it is loudest: a row read as ZEROS would put
	// Pan L and Pan R both at the centre and Width at 0, which is a mono, 3 dB-boosted collapse
	// rather than a pass-through. On most devices "absent means default" is a tidiness rule; on this
	// one it is the difference between a device that is not there and a device that reshapes the mix.
	Instance inst;
	bu::Row row {};
	// ONLY THE ENABLE IS PRESENT. Everything after it must come from `kParamDescs`.
	row[static_cast<size_t>(bu::Param::Enabled)] = 1.0f;
	inst.setPartial(row, 1);

	const bu::Matrix m = inst.matrix();
	CHECK(m.ll == 1.0f);
	CHECK(m.lr == 0.0f);
	CHECK(m.rl == 0.0f);
	CHECK(m.rr == 1.0f);
}

TEST_CASE("a settings round trips through its row") {
	bu::Settings s;
	s.enabled = true;
	s.gain = 0.31f;
	s.pan = -0.3f;
	s.panL = 0.15f;
	s.panR = -0.8f;
	s.width = 1.35f;
	s.invertL = false;
	s.invertR = true;

	const bu::Row row = bu::rowOf(s);
	const bu::Settings back = bu::stateOf(row, row.size());
	CHECK(back == s);
}

TEST_CASE("the first block after seating starts AT its target rather than fading in") {
	// WITHOUT THE PRIMED FLAG a freshly seated device ramps from an invented origin, so its first
	// block is quieter than asked. It is audible on a chain added mid-note and INVISIBLE to any test
	// that only looks at settled output — which is most of the file above.
	Instance inst;
	bu::Settings s;
	s.gain = kDoublePosition;
	inst.set(s);

	std::vector<float> left(4, 1.0f), right(4, 1.0f);
	inst.process(left, right);
	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == doctest::Approx(2.0f).epsilon(1e-6));
		CHECK(right[i] == doctest::Approx(2.0f).epsilon(1e-6));
	}
}

TEST_CASE("a changed setting slews over a fixed TIME and lands exactly on its target") {
	// THE ZIPPER GUARD, asserted as the intermediate VALUES rather than as "it is smooth". A check
	// that only asked whether the output changed would pass for a device that stepped.
	//
	// **AND THE TIME IS THE POINT.** This case asserted a ramp that crossed exactly one BLOCK, which
	// is what made the device partition-dependent — every number below is derived from
	// `kRampSeconds` and the sample rate instead. A DC input makes the reading direct: with the
	// input at 1.0 the output IS the coefficient, so nothing here is inferred from a buffer.
	Instance inst;
	inst.set(bu::Settings {}); // identity
	std::vector<float> warmL(4, 1.0f), warmR(4, 1.0f);
	inst.process(warmL, warmR); // primed, current == identity

	bu::Settings s;
	s.gain = kDoublePosition; // target 2.0
	inst.set(s);

	const int ramp = static_cast<int>(std::lround(bu::kRampSeconds * kSampleRate));
	REQUIRE(ramp > 8); // or the assertions below are about a ramp shorter than the block they use
	const float step = (bu::faderGain(kDoublePosition) - 1.0f) / static_cast<float>(ramp);

	std::vector<float> left(4, 1.0f), right(4, 1.0f);
	inst.process(left, right);
	for (int i = 0; i < 4; ++i) {
		CAPTURE(i);
		CHECK(left[static_cast<size_t>(i)]
		      == doctest::Approx(1.0f + static_cast<float>(i + 1) * step));
	}

	// IT ARRIVES, AND EXACTLY. The remaining samples finish the slew, and the last one is the target
	// to the bit rather than wherever `ramp` float additions happened to land — without the explicit
	// landing a settled device would sit a few ULPs off unity forever.
	std::vector<float> rest(static_cast<size_t>(ramp), 1.0f), restR(static_cast<size_t>(ramp), 1.0f);
	inst.process(rest, restR);
	// **AGAINST THE TARGET, NOT AGAINST 2.0f.** The claim is that the slew LANDS exactly, and the
	// thing it lands on is the coefficient this position composes to — `(2/sqrt2)^2` is 1.9999999 in
	// float, not 2. Comparing to the round number tested the taper's rounding rather than the slew's
	// arrival, and failed reading `2 == 2`.
	const float target = bu::faderGain(kDoublePosition);
	CHECK(rest.back() == target);
	CHECK(restR.back() == target);
}

TEST_CASE("the same samples in a different partition are the same samples out") {
	// **THE DEFECT THIS DEVICE SHIPPED WITH FOR ONE BUILD, PINNED LOCALLY.** `test_fx_devices` sweeps
	// every registered device for this and is what caught it — worst |diff| 0.0844 at partition 2 —
	// but a device-local case names the mechanism rather than the symptom, and fails in the suite
	// somebody runs while editing this file.
	//
	// THE SLEW MUST BE IN FLIGHT or the case is vacuous: a settled device is partition-independent
	// however it is written, which is exactly why the whole-suite sweep is worth having too.
	const auto run = [](int chunk) {
		Instance inst;
		inst.set(bu::Settings {});
		std::vector<float> warmL(8, 1.0f), warmR(8, 1.0f);
		inst.process(warmL, warmR); // primed

		bu::Settings s;
		s.gain = kDoublePosition;
		s.pan = 0.4f;
		s.width = 1.7f;
		s.panR = 0.3f;
		inst.set(s); // slew now in flight

		std::vector<float> left(64), right(64);
		fill(left, right);
		for (size_t off = 0; off < left.size(); off += static_cast<size_t>(chunk)) {
			std::vector<float> cl(left.begin() + static_cast<long>(off),
			                      left.begin() + static_cast<long>(off) + chunk);
			std::vector<float> cr(right.begin() + static_cast<long>(off),
			                      right.begin() + static_cast<long>(off) + chunk);
			inst.process(cl, cr);
			std::copy(cl.begin(), cl.end(), left.begin() + static_cast<long>(off));
			std::copy(cr.begin(), cr.end(), right.begin() + static_cast<long>(off));
		}
		return std::pair<std::vector<float>, std::vector<float>> { left, right };
	};

	const auto whole = run(64);
	const auto halves = run(32);
	const auto sixteenths = run(16);

	// BITWISE, NOT APPROXIMATELY. The per-sample arithmetic is the same operations in the same order
	// whatever the partition, so anything less than equality would be hiding a real difference.
	for (size_t i = 0; i < whole.first.size(); ++i) {
		CAPTURE(i);
		CHECK(whole.first[i] == halves.first[i]);
		CHECK(whole.second[i] == halves.second[i]);
		CHECK(whole.first[i] == sixteenths.first[i]);
		CHECK(whole.second[i] == sixteenths.second[i]);
	}

	// **NOT VACUOUS, AND STATED AS THE FACT THAT MAKES IT SO**: the ramp is longer than the whole
	// window, so the slew is still in flight at the last sample and every comparison above is about
	// a moving coefficient. A settled device is partition-independent however it is written, which is
	// the way this case would rot into a formality.
	CHECK(std::lround(bu::kRampSeconds * kSampleRate) > 64);
}

TEST_CASE("reset makes the next block start at its target again") {
	// `AudioOps::reset` means "as if just switched on", and for this device that is exactly the
	// primed flag. The weaker reading — leave the ramp where it was — would make a re-seated device
	// slide in from the previous patch's matrix.
	Instance inst;
	inst.set(bu::Settings {});
	std::vector<float> warmL(4, 1.0f), warmR(4, 1.0f);
	inst.process(warmL, warmR);

	bu::Settings s;
	s.gain = kDoublePosition;
	inst.set(s);
	inst.reset();

	std::vector<float> left(4, 1.0f), right(4, 1.0f);
	inst.process(left, right);
	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == doctest::Approx(2.0f).epsilon(1e-6));
	}
}

TEST_CASE("the device declares no mono road, and that is a claim rather than a gap") {
	// `registry.h`: a host holding a mono buffer for a device with no mono road knows to hand it two
	// channels instead of guessing. Pan, dual pan and width have no meaning on one channel, so a
	// mono road here could only be a gain stage wearing this device's name.
	CHECK(bu::kAudioOps.processMono == nullptr);
	// AND NO PER-VOICE ROAD — bus only, by design. device.h's rule is that a device which
	// has not implemented a per-voice road must not claim one.
	CHECK(bu::kTraits.perVoiceControl == -1);
}

TEST_CASE("the highpass is OFF by default, and Off is a true bypass rather than a corner set low") {
	// **THE IDENTITY CLAIM IS WHY THIS CONTROL HAS AN `Off` AT ALL.** A filter running at any corner
	// changes the samples; a highpass parked at 5 Hz would be inaudible and would still break
	// "adding this device renders the bits that were there before it existed" — which this file
	// asserts SAMPLE-EXACT, and "inaudible" is not something a test can assert.
	Instance inst;
	inst.set(bu::Settings {});
	REQUIRE(bu::Settings {}.highpass == 0);

	// A SIGNAL WITH A LARGE DC OFFSET, which is precisely what the stage would remove if it ran.
	std::vector<float> left(256), right(256);
	fill(left, right);
	for (size_t i = 0; i < left.size(); ++i) {
		left[i] += 0.5f;
		right[i] += 0.5f;
	}
	const std::vector<float> beforeL = left;
	const std::vector<float> beforeR = right;
	inst.process(left, right);
	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == beforeL[i]);
		CHECK(right[i] == beforeR[i]);
	}
}

TEST_CASE("the highpass removes a DC offset, and the METER sees the filtered signal") {
	// **THE HIGHPASS IS BEFORE THE METER — ASSERTED AS A VALUE, not as an ordering.**
	// The meter's job is to report what LEAVES the device, so a filter downstream of it would leave
	// the numbers describing a signal nobody receives: the amplitude readout would carry the offset,
	// and the LED would light on subsonic energy the output does not have.
	Instance inst;
	bu::Settings s;
	s.highpass = 4; // 50 Hz
	REQUIRE(bu::highpassHz(s.highpass) == doctest::Approx(50.0f));
	inst.set(s);

	// PURE DC AT +0.6. A highpass must walk this to zero; the reading must follow it there.
	std::vector<float> left(24000, 0.6f), right(24000, 0.6f);
	inst.process(left, right);

	// THE SIGNAL: the tail is settled near zero rather than at the offset it came in with.
	CHECK(std::fabs(left.back()) < 0.02f);
	CHECK(std::fabs(right.back()) < 0.02f);
	// AND NOT VACUOUS — it really did start at 0.6, so "it is near zero" is a fact about the filter.
	CHECK(left.front() == doctest::Approx(0.6f).epsilon(0.02));

	// **THE METER AGREES WITH THE OUTPUT.** Read through the device's own reading rather than
	// recomputed here: a raw peak of ~0.6 would mean the meter was measuring ahead of the filter.
	const bu::Meter& m = bu::meterForTest(inst.storageForTest());
	CHECK(m.reading(bu::Reading::RawPeak) < 0.1f);
}

TEST_CASE("switching the highpass off clears it, so switching back on does not replay a tail") {
	// A STAGE THAT KEEPS ITS STATE ACROSS A BYPASS REPLAYS IT ON THE WAY BACK IN — the poly-switch
	// defect `fx_runner` records, one device along. Asserted by driving the filter hard, bypassing,
	// and then switching back on into SILENCE: anything but silence out is the tail.
	Instance inst;
	bu::Settings on;
	on.highpass = 4;
	inst.set(on);
	std::vector<float> loud(2048, 0.9f), loudR(2048, 0.9f);
	inst.process(loud, loudR);

	inst.set(bu::Settings {}); // Off — clears the sections
	inst.set(on);              // and back on

	std::vector<float> quiet(64, 0.0f), quietR(64, 0.0f);
	inst.process(quiet, quietR);
	for (size_t i = 0; i < quiet.size(); ++i) {
		CHECK(quiet[i] == 0.0f);
		CHECK(quietR[i] == 0.0f);
	}
}

TEST_CASE("the highpass is 12 dB per octave — the slope a one-pole was short of") {
	// **THE CHANGE THIS CASE EXISTS FOR.** A brickwall was considered; the
	// answer was that at these corners the shortfall is SLOPE, not shape — one pole at a 30 Hz corner
	// leaves 15 Hz only 6 dB down. So the claim is a NUMBER of decibels per octave, and it is
	// measured off the rendered signal at three frequencies rather than asserted from the design.
	const auto amplitudeAt = [](double hz) {
		Instance inst;
		bu::Settings s;
		s.highpass = 4; // 50 Hz
		REQUIRE(bu::highpassHz(s.highpass) == doctest::Approx(50.0f));
		inst.set(s);
		const size_t n = static_cast<size_t>(kSampleRate);
		std::vector<float> l(n), r(n);
		for (size_t i = 0; i < n; ++i) {
			const auto v = static_cast<float>(
			    0.5 * std::sin(2.0 * std::acos(-1.0) * hz * static_cast<double>(i) / kSampleRate));
			l[i] = v;
			r[i] = v;
		}
		inst.process(l, r);
		// **MEASURED OVER THE LAST QUARTER**, after the filter has settled — the head of the buffer
		// carries the transient, and a peak taken across the whole thing would report it instead of
		// the steady state. `fx_tenant.h` records the same trap in a different measurement.
		float peak = 0.0f;
		for (size_t i = n * 3 / 4; i < n; ++i) {
			peak = std::max(peak, std::fabs(l[i]));
		}
		return peak;
	};

	const double corner = 20.0 * std::log10(static_cast<double>(amplitudeAt(50.0)) / 0.5);
	const double half = static_cast<double>(amplitudeAt(25.0));
	const double quarter = static_cast<double>(amplitudeAt(12.5));

	// **-3 dB AT THE CORNER, WHICH IS WHAT MAKES IT BUTTERWORTH** rather than two one-poles in
	// series — that cascade gives -6 dB there, and is the cheap version this deliberately is not.
	CHECK(corner == doctest::Approx(-3.0).epsilon(0.15));

	// AND 12 dB PER OCTAVE BELOW IT, measured between two octaves that are both well into the stop
	// band, where the asymptotic slope is what is being read.
	const double perOctave = 20.0 * std::log10(half / quarter);
	CHECK(perOctave == doctest::Approx(12.0).epsilon(0.05));

	// **NOT SIX**, said out loud: the previous filter would have measured ~6 here and passed every
	// other assertion in this file. The slope is the change.
	CHECK(perOctave > 9.0);
}

TEST_CASE("backlog EH: the highpass is designed at the host's rate — its corner is where it says at 96 kHz too, whichever order the rate and the setting arrive in") {
	// THE CACHE KEY WAS SHORT OF AN INPUT. The coefficient is a function of a corner AND a rate, and
	// the rate was a member initialised to 48 kHz that `prepare` never assigned — so at 96 kHz the
	// 50 Hz corner landed at 100 Hz (a 50 Hz tone read about -12 dB rather than -3), and a rate that
	// arrived after the setting was never designed in at all. Asserted BY VALUE at the corner, at
	// 96 kHz, with the rate arriving before the setting and after it.
	const auto cornerDbAt = [](double sampleRate, bool rateFirst) {
		Instance inst;
		bu::Settings s;
		s.highpass = 4; // 50 Hz
		if (rateFirst) {
			inst.prepareAt(sampleRate);
			inst.set(s);
		} else {
			inst.set(s);
			inst.prepareAt(sampleRate);
		}
		const size_t n = static_cast<size_t>(sampleRate);
		std::vector<float> l(n), r(n);
		for (size_t i = 0; i < n; ++i) {
			const auto v = static_cast<float>(
			    0.5 * std::sin(2.0 * std::acos(-1.0) * 50.0 * static_cast<double>(i) / sampleRate));
			l[i] = v;
			r[i] = v;
		}
		inst.process(l, r);
		float peak = 0.0f;
		for (size_t i = n * 3 / 4; i < n; ++i) {
			peak = std::max(peak, std::fabs(l[i]));
		}
		return 20.0 * std::log10(static_cast<double>(peak) / 0.5);
	};
	CHECK(cornerDbAt(96000.0, true) == doctest::Approx(-3.0).epsilon(0.15));
	CHECK(cornerDbAt(96000.0, false) == doctest::Approx(-3.0).epsilon(0.15));
	CHECK(cornerDbAt(44100.0, true) == doctest::Approx(-3.0).epsilon(0.15));
	// NON-VACUITY: the same tone an octave below the corner is well down, so -3 dB was measured
	// against a slope and not against a wire.
	{
		Instance inst;
		bu::Settings s;
		s.highpass = 4;
		inst.prepareAt(96000.0);
		inst.set(s);
		const size_t n = 96000;
		std::vector<float> l(n), r(n);
		for (size_t i = 0; i < n; ++i) {
			l[i] = r[i] = static_cast<float>(0.5 * std::sin(2.0 * std::acos(-1.0) * 25.0 * static_cast<double>(i) / 96000.0));
		}
		inst.process(l, r);
		float peak = 0.0f;
		for (size_t i = n * 3 / 4; i < n; ++i) {
			peak = std::max(peak, std::fabs(l[i]));
		}
		CHECK(20.0 * std::log10(static_cast<double>(peak) / 0.5) < -9.0);
	}
}

namespace {

/// THE HIGHPASS AS BUS UTILITY DESIGNED IT BEFORE BACKLOG JJ, RESTATED HERE: an RBJ highpass at
/// Q = 1/sqrt(2) in a transposed direct-form-II biquad, in double, the state carried across a
/// re-design. Written out rather than calling `dyn::Biquad`, which is the thing that replaced it.
struct ReferenceHighpass {
	double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
	double z1 = 0.0, z2 = 0.0;

	void design(double hz, double sampleRate) {
		const double w0 = 2.0 * std::numbers::pi_v<double> * hz / sampleRate;
		const double cosw = std::cos(w0);
		const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752);
		const double a0 = 1.0 + alpha;
		b0 = ((1.0 + cosw) * 0.5) / a0;
		b1 = -(1.0 + cosw) / a0;
		b2 = ((1.0 + cosw) * 0.5) / a0;
		a1 = (-2.0 * cosw) / a0;
		a2 = (1.0 - alpha) / a0;
	}
	float step(float in) {
		const double x = static_cast<double>(in);
		const double y = b0 * x + z1;
		z1 = b1 * x - a1 * y + z2;
		z2 = b2 * x - a2 * y;
		return static_cast<float>(y);
	}
};

} // namespace

TEST_CASE("backlog JJ: the highpass renders the pre-JJ design bit for bit, at every corner and rate, across a corner change") {
	// **THE CLAIM IS "NOTHING MOVED", SO IT IS ASSERTED ON EVERY OUTPUT WORD.** Bus Utility's filter
	// moved onto `dyn::Biquad` (COMP's and CLIP's) rather than keeping its own copy; that is only a
	// tidy-up if the samples are the same ones. Every corner, at three rates, with the corner moved
	// mid-stream so the state carried across a re-design is inside the comparison too. The defaults
	// make the matrix the exact identity (asserted above), so the output IS the filter's.
	const double rates[] = { 44100.0, 48000.0, 96000.0 };
	const int corners = static_cast<int>(std::size(bu::kHighpassHz));
	for (const double rate : rates) {
		for (int corner = 1; corner < corners; ++corner) {
			CAPTURE(rate);
			CAPTURE(corner);
			const int next = corner + 1 < corners ? corner + 1 : 1;

			Instance inst;
			inst.prepareAt(rate);
			bu::Settings s;
			s.highpass = corner;
			inst.set(s);
			ReferenceHighpass refL, refR;
			refL.design(static_cast<double>(bu::kHighpassHz[static_cast<size_t>(corner)]), rate);
			refR.design(static_cast<double>(bu::kHighpassHz[static_cast<size_t>(corner)]), rate);

			size_t mismatches = 0;
			size_t moved = 0;
			for (int block = 0; block < 3; ++block) {
				if (block == 2) {
					s.highpass = next;
					inst.set(s);
					refL.design(static_cast<double>(bu::kHighpassHz[static_cast<size_t>(next)]), rate);
					refR.design(static_cast<double>(bu::kHighpassHz[static_cast<size_t>(next)]), rate);
				}
				std::vector<float> left(512), right(512);
				for (size_t i = 0; i < left.size(); ++i) {
					const double t = static_cast<double>(block * 512 + static_cast<int>(i));
					left[i] = static_cast<float>(0.4 + 0.5 * std::sin(0.013 * t) + 0.1 * std::sin(1.7 * t));
					right[i] = static_cast<float>(-0.3 + 0.6 * std::sin(0.0021 * t));
				}
				const std::vector<float> inL = left;
				const std::vector<float> inR = right;
				inst.process(left, right);
				for (size_t i = 0; i < left.size(); ++i) {
					const float wantL = refL.step(inL[i]);
					const float wantR = refR.step(inR[i]);
					mismatches += (left[i] != wantL) + (right[i] != wantR);
					moved += (left[i] != inL[i]) + (right[i] != inR[i]);
				}
			}
			CHECK(mismatches == 0);
			// NOT VACUOUS: the filter really ran, so equality is a fact about it and not about a wire.
			CHECK(moved > 3000);
		}
	}
}

TEST_CASE("the clipper: off passes an over through, a knee saturates, and Hard clamps") {
	// **THE THREE ENDS OF THE SPECIFIED TRAVEL, ASSERTED AS VALUES:** fully counter-clockwise reads
	// "off", then moving up "soft 100%" through "soft 1%", then "hard".
	const auto through = [](int step, float input) {
		Instance inst;
		bu::Settings s;
		s.clip = step;
		inst.set(s);
		std::vector<float> l(64, input), r(64, input);
		inst.process(l, r);
		return l.back();
	};

	// OFF IS A GENUINE BYPASS — an over passes at its full height rather than being caught by a curve
	// that happens to be nearly straight.
	REQUIRE(bu::Settings {}.clip == 0);
	CHECK(through(0, 1.5f) == 1.5f);
	CHECK(through(0, 0.4f) == 0.4f);

	// HARD IS THE TOP OF THE TRAVEL AND IT CLAMPS.
	const int hard = static_cast<int>(std::size(bu::kClipNames)) - 1;
	CHECK(std::string(bu::kClipNames[static_cast<size_t>(hard)]) == "Hard");
	CHECK(through(hard, 1.5f) == 1.0f);
	CHECK(through(hard, -1.5f) == -1.0f);
	// AND IT IS EXACT BELOW THE CEILING — a clamp is not a curve.
	CHECK(through(hard, 0.4f) == 0.4f);

	// **SOFT 100% IS PLAIN tanh**, which is the basic-tanh requirement arriving as the
	// widest step of the ladder rather than as its own mode.
	CHECK(std::string(bu::kClipNames[1]) == "Soft 100%");
	CHECK(bu::clipKnee(1) == 1.0f);
	CHECK(through(1, 0.8f) == doctest::Approx(std::tanh(0.8f)).epsilon(1e-5));
}

TEST_CASE("a knee leaves everything below it EXACTLY untouched") {
	// **THE PROPERTY THAT MAKES THIS USABLE ON A BUS**: linear up to the knee, so a 3% knee saturates
	// only the top 3% of the amplitude. Below the knee the
	// samples are not multiplied by anything, not rounded through a nearly-straight curve — they are
	// the input.
	const size_t threePercent = 6;
	REQUIRE(std::string(bu::kClipNames[threePercent]) == "Soft 3%");
	REQUIRE(bu::clipKnee(static_cast<int>(threePercent)) == doctest::Approx(0.03f));

	const float knee = bu::clipKnee(static_cast<int>(threePercent));
	const float start = 1.0f - knee;
	// SAMPLE-EXACT, at a value just under the knee's start.
	CHECK(bu::clipSample(start - 0.01f, knee) == start - 0.01f);
	CHECK(bu::clipSample(-(start - 0.01f), knee) == -(start - 0.01f));
	// AND AT THE KNEE'S EXACT START, which is the fencepost.
	CHECK(bu::clipSample(start, knee) == start);

	// ABOVE IT, IT BENDS — otherwise the assertions above would pass for a function that does nothing.
	CHECK(bu::clipSample(1.0f, knee) < 1.0f);
	CHECK(bu::clipSample(1.0f, knee) > start);
	// **AND IS BOUNDED BY THE CEILING, WHICH IS NOT THE SAME AS APPROACHING IT.** This case first
	// asserted `< 1.0f` on the reasoning that tanh is asymptotic — true in real arithmetic, FALSE in
	// float: with a 3% knee, `tanh((10 - 0.97) / 0.03)` is `tanh(301)`, which is exactly 1.0 in
	// single precision, so the output lands precisely on the ceiling. That is good behaviour (a
	// hard-driven soft clip becomes a hard clip) and a wrong claim, so the claim moved rather than
	// the code.
	CHECK(bu::clipSample(10.0f, knee) <= 1.0f);
	CHECK(bu::clipSample(10.0f, knee) > 0.99f);
	// THE BOUND HOLDS BOTH WAYS AND AT EVERY STEP OF THE LADDER — nothing this device emits with the
	// clipper engaged may exceed full scale, whatever the knee.
	for (size_t step = 1; step < std::size(bu::kClipNames); ++step) {
		const float k = bu::clipKnee(static_cast<int>(step));
		CAPTURE(std::string(bu::kClipNames[step]));
		CHECK(bu::clipSample(50.0f, k) <= 1.0f);
		CHECK(bu::clipSample(-50.0f, k) >= -1.0f);
	}
}

TEST_CASE("the knee joins the linear region with no corner in it") {
	// **C1 CONTINUITY, MEASURED RATHER THAN ARGUED.** `tanh'(0) == 1`, so the curve should leave the
	// straight part at unit slope — a corner there is what a listener hears as a click on material
	// grazing the threshold. Checked as a numerical derivative either side of the join.
	const float knee = 0.25f;
	const float start = 1.0f - knee;
	const float h = 1e-4f;
	const float below = (bu::clipSample(start - h, knee) - bu::clipSample(start - 2.0f * h, knee)) / h;
	const float above = (bu::clipSample(start + 2.0f * h, knee) - bu::clipSample(start + h, knee)) / h;
	CHECK(below == doctest::Approx(1.0f).epsilon(0.01));
	CHECK(above == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("the clipper is AFTER the meter, so the LED still reports an over it removed") {
	// **THE DECISION, AS A VALUE:** the clipper is after the meter, so a lit clip LED proves it is
	// active. A clipper ahead of the meter would make the indicator permanently dark,
	// because nothing downstream of one exceeds full scale.
	Instance inst;
	bu::Settings s;
	s.clip = static_cast<int>(std::size(bu::kClipNames)) - 1; // Hard
	inst.set(s);

	std::vector<float> l(512, 1.4f), r(512, 1.4f);
	inst.process(l, r);

	// THE OUTPUT IS CLAMPED...
	CHECK(l.back() == 1.0f);
	// ...AND THE METER SAW WHAT AROSE BEFORE IT. Both halves matter: a raw peak of 1.0 would mean the
	// meter was reading the clipper's output, and the LED would be reporting the clamp rather than
	// the signal that needed clamping.
	const bu::Meter& m = bu::meterForTest(inst.storageForTest());
	CHECK(m.reading(bu::Reading::RawPeak) == doctest::Approx(1.4f));
	CHECK(m.reading(bu::Reading::ClipAge) < 0.1f);
}

TEST_CASE("exactly the five continuous controls are modulatable, and the switches are not") {
	// **THE DECISION, AS A LIST RATHER THAN A COUNT:** the utility's knobs are modulatable, except the
	// channel polarity switches, the filter frequency and the clipper knee.
	//
	// **NAMED BOTH WAYS.** A count would pass if the right NUMBER of controls were modulatable and the
	// wrong ones — and the three exclusions are the whole content of the decision, so they are asserted
	// as explicitly as the inclusions. The `checked` tallies at the end are what stop either loop
	// passing by never running.
	const auto modulatable = [](bu::Param p) {
		return bu::kParamDescs[static_cast<size_t>(p)].modulatable;
	};

	CHECK(modulatable(bu::Param::Gain));
	CHECK(modulatable(bu::Param::Pan));
	CHECK(modulatable(bu::Param::PanL));
	CHECK(modulatable(bu::Param::PanR));
	CHECK(modulatable(bu::Param::Width));
	CHECK(modulatable(bu::Param::Mix));
	CHECK(modulatable(bu::Param::WetGain));
	// THE MID AND SIDE TRIMS (backlog DM) are levels, so they join the continuous side.
	CHECK(modulatable(bu::Param::MidTrim));
	CHECK(modulatable(bu::Param::SideTrim));

	// **THE THREE EXCLUSIONS, AND THE ENABLE.** Each is a stepped control whose values are STATES
	// rather than a quantity — a knee of "Soft 12%" has no meaning half way to "Soft 6%", and a
	// polarity has no meaning between its two positions. Modulating any of them would be modulating
	// an index into a list.
	CHECK_FALSE(modulatable(bu::Param::InvertL));
	CHECK_FALSE(modulatable(bu::Param::InvertR));
	CHECK_FALSE(modulatable(bu::Param::Highpass));
	CHECK_FALSE(modulatable(bu::Param::Clip));
	// THE ENABLE IS THE EXCEPTION, BY DESIGN (backlog KW): every device's enable is a destination, a gate at one half.
	CHECK(modulatable(bu::Param::Enabled));
	// AND MONITOR AND SWAP (backlog DM): states, not quantities.
	CHECK_FALSE(modulatable(bu::Param::Monitor));
	CHECK_FALSE(modulatable(bu::Param::Swap));

	// AND THE TOTAL AGREES WITH THE LIST, so a control added later cannot quietly join either side.
	CHECK(bu::kDescriptor.modulatableCount() == 10);
	CHECK(std::size(bu::kParamDescs) == 16);

	// **EVERY MODULATABLE CONTROL IS A STEPPED-FREE ONE AND EVERY EXCLUDED ONE IS STEPPED.** Not a
	// coincidence worth leaving unsaid: it is the rule the decision follows, so a future control lands
	// on the right side by construction rather than by somebody remembering this case.
	size_t checkedOn = 0;
	size_t checkedOff = 0;
	for (const feathers::device::ParamDesc& d : bu::kParamDescs) {
		CAPTURE(std::string(d.name));
		if (d.modulatable && &d != &bu::kParamDescs[static_cast<size_t>(bu::Param::Enabled)]) { // the enable: a stepped destination (backlog KW)
			CHECK_FALSE(d.stepped);
			++checkedOn;
		}
		else if (d.modulatable) {
			CHECK(d.stepped);
		}
		else {
			CHECK(d.stepped);
			++checkedOff;
		}
	}
	CHECK(checkedOn == 9);
	CHECK(checkedOff == 6);
}

TEST_CASE("tick windows: a Gain window is followed inside the call — no stair at the tick, landing on the fader law every 8 samples") {
	// The value claimed is the GAIN ON EACH SAMPLE, read as the left
	// output of a DC left input with the right silent — at the defaults every other term of the matrix is the
	// identity, so left out IS `current.ll`.
	Instance bus;
	bu::Settings s;
	bus.set(s); // gain at unity (0.5)
	std::vector<float> l(64, 1.0f);
	std::vector<float> r(64, 0.0f);
	bus.process(l, r); // primed and settled at unity
	REQUIRE(l.back() == doctest::Approx(1.0f));

	// THE TICK: Gain rises linearly 0.5 -> 0.75 across 32 samples of this call.
	std::array<float, 32> gainRun {};
	for (size_t i = 0; i < gainRun.size(); ++i) {
		gainRun[i] = 0.5f + 0.25f * static_cast<float>(i + 1) / 32.0f;
	}
	std::array<const float*, bu::kDescriptor.params.size()> runs {};
	runs[static_cast<size_t>(bu::Param::Gain)] = gainRun.data();
	bus.set(s); // setFields with the tick's starting value, as the engine does before the windows
	REQUIRE(bu::kAudioOps.setFieldWindows != nullptr);
	bu::kAudioOps.setFieldWindows(const_cast<void*>(bus.storageForTest()), runs.data(), runs.size(), 32);
	std::vector<float> wl(32, 1.0f);
	std::vector<float> wr(32, 0.0f);
	bus.process(wl, wr);

	// THE FADER LAW, RESTATED: (2p)^2 — `faderGain`'s own comment says the centre is (2 × 0.5)^2.
	const auto law = [](float p) { return (2.0f * p) * (2.0f * p); };
	float maxStep = std::abs(wl[0] - 1.0f);
	for (size_t i = 1; i < wl.size(); ++i) {
		maxStep = std::max(maxStep, std::abs(wl[i] - wl[i - 1]));
	}
	for (size_t end = 7; end < wl.size(); end += 8) {
		CAPTURE(end);
		CHECK(wl[end] == doctest::Approx(law(gainRun[end])).epsilon(1e-5));
	}
	CAPTURE(maxStep);
	// NOT VACUOUS: the call moved the gain by most of 0.5 -> 0.75's fader distance (1.0 -> 2.25)...
	CHECK(wl.back() - 1.0f > 1.0f);
	// ...a sample at a time: 1.25 over 32 samples is ~0.04 a sample; a stair at the tick would be the whole 1.25.
	CHECK(maxStep < 0.1f);
}

// ---------------------------------------------------------------------------------------------------------------------
// BACKLOG DM (2026-10-30): Swap, the Mid and Side trims, Monitor. Each asserted as the MATRIX the device applies, against
// numbers written out here rather than read back from `matrixOf`.
// ---------------------------------------------------------------------------------------------------------------------

namespace {
void checkMatrix(const bu::Matrix& m, double ll, double lr, double rl, double rr) {
	CHECK(std::abs(static_cast<double>(m.ll) - ll) <= 1e-6);
	CHECK(std::abs(static_cast<double>(m.lr) - lr) <= 1e-6);
	CHECK(std::abs(static_cast<double>(m.rl) - rl) <= 1e-6);
	CHECK(std::abs(static_cast<double>(m.rr) - rr) <= 1e-6);
}
} // namespace

TEST_CASE("DM: every new control at its default is the identity to the bit, and a busy patch's matrix is unchanged by them") {
	// THE NEW DEFAULTS ARE SKIPPED STAGES, so a patch that has never touched them renders what it rendered before they
	// existed. Pinned on a busy setting, against the same setting with the new fields written explicitly to their
	// defaults: the two must be EQUAL, not close.
	bu::Settings busy;
	busy.gain = 0.61f;
	busy.pan = -0.3f;
	busy.panL = -0.4f;
	busy.panR = 0.7f;
	busy.width = 1.37f;
	busy.invertR = true;
	bu::Settings explicitDefaults = busy;
	explicitDefaults.midTrim = 0.0f;
	explicitDefaults.sideTrim = 0.0f;
	explicitDefaults.monitor = 0;
	explicitDefaults.swap = false;
	CHECK(bu::matrixOf(explicitDefaults) == bu::matrixOf(busy));
	// AND IT AGREES WITH THE INDEPENDENT RESTATEMENT OF THE PRE-DM PATH, which knows nothing of the new stages.
	const bu::Matrix want = reference(busy);
	checkMatrix(bu::matrixOf(busy), want.ll, want.lr, want.rl, want.rr);
	// THE DESCRIPTOR'S DEFAULTS ARE THE SETTINGS' DEFAULTS (static_asserts pin it), and both are the identity.
	CHECK(bu::matrixOf(bu::Settings {}) == bu::Matrix {});
}

TEST_CASE("DM: Swap exchanges the input pair AHEAD of every control, so Invert L flips what is on the left by then") {
	bu::Settings s;
	s.swap = true;
	checkMatrix(bu::matrixOf(s), 0.0, 1.0, 1.0, 0.0);
	// WITH INVERT L: the left output is the RIGHT input, inverted.
	s.invertL = true;
	checkMatrix(bu::matrixOf(s), 0.0, -1.0, 1.0, 0.0);
	// AND THE RUNNING DEVICE APPLIES IT: a swapped bus's left output is its right input, sample for sample.
	Instance inst;
	bu::Settings swapOnly;
	swapOnly.swap = true;
	inst.set(swapOnly);
	std::vector<float> left(64), right(64);
	fill(left, right);
	const std::vector<float> inL = left;
	const std::vector<float> inR = right;
	inst.process(left, right);
	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == inR[i]);
		CHECK(right[i] == inL[i]);
	}
}

TEST_CASE("DM: the Mid and Side trims scale M and S by their dB, after Width, and 0 dB is the identity") {
	const auto gain = [](double db) { return std::pow(10.0, db / 20.0); };
	SUBCASE("mid up, side down") {
		bu::Settings s;
		s.midTrim = 6.0f;
		s.sideTrim = -6.0f;
		const double m = gain(6.0);
		const double sd = gain(-6.0);
		checkMatrix(bu::matrixOf(s), (m + sd) / 2.0, (m - sd) / 2.0, (m - sd) / 2.0, (m + sd) / 2.0);
	}
	SUBCASE("the side alone, at the bottom of its range") {
		bu::Settings s;
		s.sideTrim = static_cast<float>(-bu::kTrimDb);
		const double sd = gain(-bu::kTrimDb);
		checkMatrix(bu::matrixOf(s), (1.0 + sd) / 2.0, (1.0 - sd) / 2.0, (1.0 - sd) / 2.0, (1.0 + sd) / 2.0);
	}
	SUBCASE("after Width: the side the trim scales is the side Width already scaled") {
		bu::Settings s;
		s.width = 0.5f;
		s.sideTrim = 6.0f;
		const double sd = gain(6.0) * 0.5; // the side gain is width * trim
		checkMatrix(bu::matrixOf(s), (1.0 + sd) / 2.0, (1.0 - sd) / 2.0, (1.0 - sd) / 2.0, (1.0 + sd) / 2.0);
	}
	SUBCASE("clamped to the declared range") {
		bu::Settings s;
		s.midTrim = 40.0f;
		const double m = gain(bu::kTrimDb);
		checkMatrix(bu::matrixOf(s), (m + 1.0) / 2.0, (m - 1.0) / 2.0, (m - 1.0) / 2.0, (m + 1.0) / 2.0);
	}
}

TEST_CASE("DM: Monitor sends one signal to both outputs, LAST, after the fader — each rung by value") {
	// THE FADER AT DOUBLE (+6 dB) AND A PAN, so a monitor applied anywhere but last would read different numbers.
	bu::Settings s;
	s.gain = kDoublePosition;
	s.pan = 0.5f; // the LEFT side at 0.5 (a positive pan attenuates the left)
	const bu::Matrix base = bu::matrixOf(s);
	checkMatrix(base, 1.0, 0.0, 0.0, 2.0);
	REQUIRE(std::size(bu::kMonitorNames) == 5);
	s.monitor = static_cast<int>(bu::Monitor::Mono);
	checkMatrix(bu::matrixOf(s), 0.5, 1.0, 0.5, 1.0);
	s.monitor = static_cast<int>(bu::Monitor::Left);
	checkMatrix(bu::matrixOf(s), 1.0, 0.0, 1.0, 0.0);
	s.monitor = static_cast<int>(bu::Monitor::Right);
	checkMatrix(bu::matrixOf(s), 0.0, 2.0, 0.0, 2.0);
	s.monitor = static_cast<int>(bu::Monitor::Side);
	checkMatrix(bu::matrixOf(s), 0.5, -1.0, 0.5, -1.0);
	s.monitor = static_cast<int>(bu::Monitor::Stereo);
	CHECK(bu::matrixOf(s) == base);
	// AND THE RUNNING DEVICE: Side at defaults, both outputs (L-R)/2.
	Instance inst;
	bu::Settings side;
	side.monitor = static_cast<int>(bu::Monitor::Side);
	inst.set(side);
	std::vector<float> left(64), right(64);
	fill(left, right);
	const std::vector<float> inL = left;
	const std::vector<float> inR = right;
	inst.process(left, right);
	for (size_t i = 0; i < left.size(); ++i) {
		CHECK(left[i] == 0.5f * (inL[i] - inR[i]));
		CHECK(right[i] == left[i]);
	}
}

TEST_CASE("DM: the new controls round trip through the row, and an old row without them lands on their defaults") {
	bu::Settings s;
	s.midTrim = -3.5f;
	s.sideTrim = 7.25f;
	s.monitor = static_cast<int>(bu::Monitor::Right);
	s.swap = true;
	const bu::Row row = bu::rowOf(s);
	CHECK(bu::stateOf(row, row.size()) == s);
	// A ROW CUT BEFORE THE NEW FIELDS (a patch saved before them) reads them at their declared defaults.
	const bu::Settings old = bu::stateOf(row, static_cast<size_t>(bu::Param::MidTrim));
	CHECK(old.midTrim == 0.0f);
	CHECK(old.sideTrim == 0.0f);
	CHECK(old.monitor == 0);
	CHECK_FALSE(old.swap);
}
