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

#include "fx/automod/automod_derived.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace feathers::fx::automod;

namespace {

/// Walk the coordinate cube coarsely — enough that a bank retune moves at least one sample, without
/// making every case a minute long.
constexpr int kSteps = 17;

AutomodCoords at(float t, float f, float m, float hz = 1.0f) {
	AutomodCoords c;
	c.type = t;
	c.flavor = f;
	c.mod = m;
	c.lfoHz = hz;
	return c;
}

template <typename Fn> void overCube(Fn&& fn) {
	for (int i = 0; i < kSteps; ++i) {
		for (int j = 0; j < kSteps; ++j) {
			for (int k = 0; k < kSteps; ++k) {
				const float t = static_cast<float>(i) / static_cast<float>(kSteps - 1);
				const float f = static_cast<float>(j) / static_cast<float>(kSteps - 1);
				const float m = static_cast<float>(k) / static_cast<float>(kSteps - 1);
				fn(t, f, m);
			}
		}
	}
}

} // namespace

TEST_CASE("automod derive: the filter mix is constant power everywhere in the cube") {
	// CONSTANT POWER, NOT SUM-TO-ONE, and it is the difference between a Type knob that slides the
	// character and one that is also a volume control. The three bands are SUMMED, so equal perceived
	// loudness across the blend needs the squares to sum to one.
	//
	// ASSERTED AS A VALUE AT EVERY COORDINATE rather than at one: the normalisation divides by a root
	// that could go wrong only where the raw weights are small, which is exactly where a spot check
	// would not look.
	float worstError = 0.0f;
	float smallestLow = 1.0f;
	overCube([&](float t, float f, float m) {
		const AutomodDerived d = deriveAutomod(at(t, f, m));
		const float sumSquares = d.filterMix.low * d.filterMix.low + d.filterMix.band * d.filterMix.band
		                         + d.filterMix.high * d.filterMix.high;
		worstError = std::max(worstError, std::fabs(sumSquares - 1.0f));
		smallestLow = std::min(smallestLow, d.filterMix.low);
	});
	CHECK(worstError < 1e-4f);

	// AND THE LOWPASS FLOOR IS DOING ITS JOB: there is no coordinate at which the device mutes,
	// which is the whole reason the firmware adds an epsilon before normalising.
	CHECK(smallestLow > 0.0f);
}

TEST_CASE("automod derive: the comb's reach is bounded at the top by construction and at the bottom by the clamp") {
	// THIS CASE WAS WRITTEN TO SAY THE CLAMP WAS ONLY A GUARD, AND IT PROVED THE OPPOSITE.
	//
	// The base delay is placed in what is left after the modulation range is reserved AT THE TOP, so
	// `base + range` really does stay inside the line by construction. The bottom is not reserved:
	// measured over the cube, `base - range` reaches **-194** at the worst corner, so the clamp in
	// `Comb::delayFor` is LOAD-BEARING there and the bottom of a wide sweep flattens against the
	// floor. That is the firmware's behaviour, ported rather than introduced.
	float lowestRequested = 1e9f;
	float highestRequested = -1e9f;
	float lowestActual = 1e9f;
	float highestActual = -1e9f;

	Comb probe;
	overCube([&](float t, float f, float m) {
		const AutomodDerived d = deriveAutomod(at(t, f, m));
		lowestRequested = std::min(lowestRequested, d.comb.baseDelaySamples - d.comb.modRangeSamples);
		highestRequested = std::max(highestRequested, d.comb.baseDelaySamples + d.comb.modRangeSamples);

		// What the comb WILL actually run, which is the number that matters for safety.
		probe.setSettings(d.comb);
		lowestActual = std::min(lowestActual, probe.delayFor(-1.0f));
		highestActual = std::max(highestActual, probe.delayFor(1.0f));
	});

	// THE TOP IS BY CONSTRUCTION — the request itself never exceeds the line.
	CHECK(highestRequested <= kCombMaxDelaySamples);

	// THE BOTTOM IS BY THE CLAMP, and the case records BOTH: what was asked for, and what happens.
	CHECK(lowestRequested < 0.0f); // the flattening is real and this is where it is recorded
	CHECK(lowestActual >= kCombMinDelaySamples);
	CHECK(highestActual <= kCombMaxDelaySamples);

	// NON-VACUITY: the comb really does use most of the line.
	CHECK(highestActual > kCombMaxDelaySamples * 0.5f);
	CHECK(lowestActual < kCombMaxDelaySamples * 0.5f);
}

TEST_CASE("automod derive: the feedback it asks for is always inside what the comb will accept") {
	// A DERIVE THAT ASKED FOR AN UNSTABLE COMB WOULD BE CLAMPED SILENTLY, and the symptom would be a
	// range of the Type knob that all sounds the same. Asserted against the comb's OWN ceiling, so
	// the two cannot drift apart.
	float highest = 0.0f;
	overCube([&](float t, float f, float m) {
		highest = std::max(highest, std::fabs(deriveAutomod(at(t, f, m)).comb.feedback));
	});
	CHECK(highest <= kCombMaxFeedback);
	CHECK(highest > 0.5f); // and it does use the range
}

TEST_CASE("automod derive: the springs stay inside what the spring will accept, at every rate") {
	// THE SPRING FREQUENCY SCALES WITH THE LFO RATE, so this has to be swept over rates as well as
	// coordinates — a bound that held only at 1 Hz would be no bound at all.
	for (float hz : { 0.01f, 1.0f, 5.0f, 20.0f }) {
		float highest = 0.0f;
		float lowestZeta = 10.0f;
		overCube([&](float t, float f, float m) {
			const AutomodDerived d = deriveAutomod(at(t, f, m, hz));
			highest = std::max(highest, std::max(d.springHz, d.combSpringHz));
			lowestZeta = std::min(lowestZeta, d.springZeta);
		});
		CAPTURE(hz);
		CHECK(highest <= kSpringFreqMax);
		CHECK(lowestZeta >= kSpringDampingMin);
		// A ZETA FLOOR OF 0.15 IS THE FIRMWARE'S AND IT IS WHAT STOPS THE SPRING RINGING FOREVER.
		CHECK(lowestZeta >= 0.15f - 1e-5f);
	}
}

TEST_CASE("automod derive: a faster LFO gets a faster spring, which is what keeps bounces-per-cycle fixed") {
	// THE MECHANISM, ASSERTED AS A RATIO. A spring at a fixed frequency would be smoothed away by a
	// fast LFO and would ring under a slow one; the firmware scales it so the CHARACTER is a number of
	// bounces per cycle. The comb spring is exactly four times the rate, which is a value.
	const AutomodDerived slow = deriveAutomod(at(0.4f, 0.6f, 0.7f, 1.0f));
	const AutomodDerived fast = deriveAutomod(at(0.4f, 0.6f, 0.7f, 4.0f));

	CHECK(fast.springHz > slow.springHz);
	CHECK(fast.combSpringHz == doctest::Approx(4.0f * slow.combSpringHz).epsilon(1e-4));
	CHECK(slow.combSpringHz == doctest::Approx(4.0f).epsilon(1e-4)); // 4x a 1 Hz LFO
}

TEST_CASE("automod derive: the envelope times land inside their declared range and run the right way") {
	// THE ONE MAPPING IN THE FILE CHOSEN BY EAR RATHER THAN PORTED, so the case pins BOTH ends and
	// the direction. The firmware stores a per-buffer coefficient, which is not a duration at all —
	// there was no faithful conversion to copy, so these two ranges are a decision and this is where
	// a retune would be caught.
	float fastestAttack = 1e9f;
	float slowestAttack = 0.0f;
	float fastestRelease = 1e9f;
	float slowestRelease = 0.0f;
	overCube([&](float t, float f, float m) {
		const AutomodDerived d = deriveAutomod(at(t, f, m));
		fastestAttack = std::min(fastestAttack, d.envAttackSeconds);
		slowestAttack = std::max(slowestAttack, d.envAttackSeconds);
		fastestRelease = std::min(fastestRelease, d.envReleaseSeconds);
		slowestRelease = std::max(slowestRelease, d.envReleaseSeconds);
	});

	CHECK(fastestAttack >= kEnvAttackFastSeconds - 1e-9f);
	CHECK(slowestAttack <= kEnvAttackSlowSeconds + 1e-9f);
	CHECK(fastestRelease >= kEnvReleaseFastSeconds - 1e-9f);
	CHECK(slowestRelease <= kEnvReleaseSlowSeconds + 1e-9f);

	// A RELEASE IS SLOWER THAN AN ATTACK AT THE SAME FIELD VALUE, which is what makes it an envelope
	// follower rather than a symmetric smoother — the ranges have to be ordered, not merely valid.
	CHECK(kEnvReleaseSlowSeconds > kEnvAttackSlowSeconds);
	CHECK(kEnvReleaseFastSeconds > kEnvAttackFastSeconds);

	// AND THE DIRECTION: a higher field value is a FASTER response, as it was in the firmware where a
	// higher scalar meant a higher one-pole coefficient.
	CHECK(envTimeFor(1.0f, kEnvAttackFastSeconds, kEnvAttackSlowSeconds)
	      == doctest::Approx(kEnvAttackFastSeconds).epsilon(1e-4));
	CHECK(envTimeFor(0.0f, kEnvAttackFastSeconds, kEnvAttackSlowSeconds)
	      == doctest::Approx(kEnvAttackSlowSeconds).epsilon(1e-4));
	// The midpoint is the geometric mean, which pins the taper as log rather than linear.
	CHECK(envTimeFor(0.5f, kEnvAttackFastSeconds, kEnvAttackSlowSeconds)
	      == doctest::Approx(std::sqrt(kEnvAttackFastSeconds * kEnvAttackSlowSeconds)).epsilon(1e-4));
}

TEST_CASE("automod derive: the device gamma moves all three coordinates and an offset moves one") {
	// THE REASON FOR KEEPING BOTH, AS AN ASSERTION: "move all three together" is a different
	// gesture from "move one". A gamma that only reached one field, or an offset that reached all
	// three, would satisfy any "the sound changed" check.
	// **EXACTLY-REPRESENTABLE COORDINATES, AND THE COLLAPSE IS WHY (BJ piece 3).** These were
	// `0.3, 0.4, 0.5`. The identity below is an EXACT equality, and it used to be exact for free: the
	// offset lived in its own field and the derive summed the pair in DOUBLE, so no rounding could
	// come between the two sides. With the pair collapsed there is one float per coordinate, so
	// "add 0.25 to type" now rounds once when it is stored — and `float(0.3f + 0.25f)` is not
	// `double(0.3f) + 0.25`.
	//
	// **THAT IS A REAL CONSEQUENCE OF THE COLLAPSE AND NOT A REASON TO WEAKEN THE ASSERTION.** Eighths
	// are exact in binary, so on these coordinates the store rounds to nothing and the identity is
	// bit-exact again — which keeps the case saying what it was written to say instead of saying it
	// approximately. The deadzone note below is unaffected: it is about which OBSERVABLE to probe,
	// not about which coordinates to stand on.
	const AutomodCoords base = at(0.25f, 0.375f, 0.5f);
	const AutomodDerived plain = deriveAutomod(base);

	// The gamma is EXACTLY equivalent to adding the same offset to all three, which is the identity
	// that says it is one shared phase rather than a fourth field.
	AutomodCoords gammaed = base;
	gammaed.gammaPhase = 0.25f;
	AutomodCoords allThree = base;
	// **THE PER-COORDINATE PHASES COLLAPSED INTO THE COORDINATES (BJ piece 3), so "add 0.25 to type's
	// phase" is now spelled "add 0.25 to type".** That is not a weakening of the case: the derive
	// summed the two on the way in, so this was always the arithmetic being asserted.
	allThree.type = base.type + 0.25f;
	allThree.flavor = base.flavor + 0.25f;
	allThree.mod = base.mod + 0.25f;
	CHECK(deriveAutomod(gammaed) == deriveAutomod(allThree));

	// A SINGLE OFFSET REACHES ONLY ITS OWN FIELD. Type's offset moves the comb's feedback and the
	// filter mix, and leaves the flavour-derived and mod-derived numbers alone.
	// MEASURED ON THE FILTER MIX RATHER THAN ON THE COMB'S FEEDBACK, and the first draft got that
	// wrong: the feedback triangle has a 20% deadzone, and both 0.3 and 0.55 happened to land in it,
	// so the assertion compared 0 against 0 and failed for a reason that had nothing to do with the
	// offset. The mix is normalised to constant power and is therefore never all-zero at any
	// coordinate, which makes it the honest probe for "did this field move".
	AutomodCoords typeOnly = base;
	typeOnly.type = base.type + 0.25f;
	const AutomodDerived t = deriveAutomod(typeOnly);
	CHECK_FALSE(t.filterMix == plain.filterMix);
	CHECK(t.filterCutoffBase == plain.filterCutoffBase);   // flavour's
	CHECK(t.stereoPhaseOffset == plain.stereoPhaseOffset); // mod's

	// And the mirror, so "nothing moves" cannot pass: flavour's offset moves the flavour numbers and
	// leaves type's alone.
	AutomodCoords flavorOnly = base;
	flavorOnly.flavor = base.flavor + 0.25f;
	const AutomodDerived fl = deriveAutomod(flavorOnly);
	CHECK(fl.filterCutoffBase != doctest::Approx(plain.filterCutoffBase));
	CHECK(fl.filterMix == plain.filterMix); // type's, untouched
}

TEST_CASE("automod derive: the LFO's waveform is authored by the MOD coordinate and no other") {
	// FOUND BY A KNOWN-BAD, AND IT IS THE CENTREPIECE. Swapping `in.mod` for `in.type` in the one line
	// that builds the shape left all ten cases GREEN — nothing anywhere asserted which coordinate
	// draws the waveform. The pinned-values case did not catch it either, because at (0.5, 0.5, 0.5)
	// the three coordinates are equal and the swap is invisible. THE FIXTURE'S OWN SYMMETRY WAS
	// HIDING IT, which is why this case uses three DIFFERENT coordinates.
	const AutomodCoords c = at(0.2f, 0.5f, 0.8f);
	const AutomodDerived d = deriveAutomod(c);

	// **THE PHASE IS SUMMED IN DOUBLE NOW, AND THAT IS A DELIBERATE, TINY CHANGE OF SOUND.**
	//
	// This case used to compare against `lfoShapeAt(mod, modPhase + gammaPhase)` — where the two
	// offsets are added AS FLOATS and the coordinate is added as a double inside. Every other bank in
	// the same derive used `double(mod) + double(modPhase) + double(gammaPhase)`. **So the LFO shape
	// bank was evaluated at a slightly different phase from the mod scalars it shares a coordinate
	// with**, by a float rounding — around 1e-9 on the phase, inaudible, and wrong.
	//
	// Moving the derive onto one phase per triangle removed the inconsistency by construction: the
	// nine shape triangles now take the same Mod phase everything else in that group takes. **This is
	// the one place the 39-coordinate change is NOT bit-identical**, and it is called out here rather
	// than absorbed, because "bit-identical at a rank-1 home" was the premise the whole change was
	// argued on and one exception to it is a fact, not a footnote.
	const auto phaseOf = [](const AutomodCoords& in) {
		return static_cast<double>(in.mod)
		       + static_cast<double>(in.gammaPhase);
	};
	const auto shapeAtDoublePhase = [](double phase) {
		std::array<double, 9> nine {};
		nine.fill(phase);
		return feathers::character::lfoShapeAtPhases(nine);
	};
	CHECK(d.shape == shapeAtDoublePhase(phaseOf(c)));

	// AND THE OFFSET RIDES IN THE SAME PLACE: mod's own phase and the device gamma both reach it.
	AutomodCoords shifted = c;
	shifted.mod = c.mod + 0.17f;
	shifted.gammaPhase = 0.06f;
	CHECK(deriveAutomod(shifted).shape == shapeAtDoublePhase(phaseOf(shifted)));

	// NON-VACUITY, BOTH WAYS. Moving mod redraws it...
	AutomodCoords movedMod = c;
	movedMod.mod = 0.35f;
	CHECK_FALSE(deriveAutomod(movedMod).shape == d.shape);
	// ...and moving the other two does not touch it, which is the half the known-bad exposed.
	AutomodCoords movedType = c;
	movedType.type = 0.9f;
	movedType.flavor = 0.1f;
	CHECK(deriveAutomod(movedType).shape == d.shape);
}

TEST_CASE("automod derive: it is pure — the same coordinates give the same answer, bit for bit") {
	// PURITY IS THE WHOLE ARGUMENT FOR RETURNING A VALUE RATHER THAN MUTATING A CACHE, so it is
	// asserted rather than assumed. A hidden static, a lazily-built table, or a field read from
	// somewhere else would all break this.
	const AutomodCoords c = at(0.61f, 0.37f, 0.83f, 2.5f);
	const AutomodDerived a = deriveAutomod(c);
	const AutomodDerived b = deriveAutomod(c);
	CHECK(a == b);

	// And interleaving a DIFFERENT coordinate between two calls does not change the answer, which is
	// what catches a cache that remembered the last one.
	(void)deriveAutomod(at(0.1f, 0.9f, 0.2f));
	CHECK(deriveAutomod(c) == a);
}

TEST_CASE("automod derive: non-finite coordinates cannot reach the sound") {
	// THE ROAD A PATCH LOAD TAKES. The playbook's 8a is exactly this shape — a guard on the gesture's
	// road and none on the file's — so every field is swept, not just the obvious one.
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	for (float bad : { nan, inf, -inf }) {
		AutomodCoords c = at(bad, bad, bad, bad);
		c.gammaPhase = bad;
		const AutomodDerived d = deriveAutomod(c);

		CAPTURE(bad);
		CHECK(std::isfinite(d.filterCutoffBase));
		CHECK(std::isfinite(d.filterResonance));
		CHECK(std::isfinite(d.filterMix.low));
		CHECK(std::isfinite(d.filterMix.band));
		CHECK(std::isfinite(d.filterMix.high));
		CHECK(std::isfinite(d.comb.baseDelaySamples));
		CHECK(std::isfinite(d.comb.feedback));
		CHECK(std::isfinite(d.envAttackSeconds));
		CHECK(std::isfinite(d.springHz));
		CHECK(std::isfinite(d.springZeta));
		CHECK(std::isfinite(d.combSpringHz));
		CHECK(std::isfinite(d.lfoStartPhase));
		for (int i = 0; i < 4; ++i) {
			CHECK(std::isfinite(d.shape.phase[i]));
			CHECK(std::isfinite(d.shape.amplitude[i]));
		}
		// AND THE DELAY IS STILL INSIDE THE LINE — a finite garbage number would still be an
		// out-of-bounds read, so finiteness alone is not the claim.
		CHECK(d.comb.baseDelaySamples >= kCombMinDelaySamples);
		CHECK(d.comb.baseDelaySamples <= kCombMaxDelaySamples);
	}

	// A good neighbour, so "it zeroed everything" cannot pass for a fix.
	const AutomodDerived good = deriveAutomod(at(0.5f, 0.5f, 0.5f));
	CHECK(good.comb.baseDelaySamples > kCombMinDelaySamples);
}

TEST_CASE("automod derive: one coordinate names one setting, and here is which") {
	// THE PINNED VALUES. Everything above is a property, and every property here is satisfied by a
	// DIFFERENT valid derive — this is the case that fails when a bank is retuned or a mapping drifts.
	const AutomodDerived d = deriveAutomod(at(0.5f, 0.5f, 0.5f, 2.0f));

	CHECK(d.filterCutoffBase == doctest::Approx(0.0776f).epsilon(1e-3));
	CHECK(d.filterResonance == doctest::Approx(0.1865f).epsilon(1e-3));
	CHECK(d.filterMix.low == doctest::Approx(1.0f).epsilon(1e-3));
	// MOVED WITH THE REST FLOOR (backlog IS.5): the same field value s = (986.18 - 6) / 1128 = 0.869 of the OLD
	// span, on the new one (1785.4 - 6 - 400 = 1379.4) is 6 + 0.869 * 1379.4 = 1204.6.
	CHECK(d.comb.baseDelaySamples == doctest::Approx(1204.6f).epsilon(1e-3));
	CHECK(d.comb.modRangeSamples == doctest::Approx(170.52f).epsilon(1e-3));
	// BIPOLAR SINCE BACKLOG IS.5: the same phase (x = 0.665 of a 0.8 duty) that read +0.2867 unipolar sits in the
	// bipolar triangle's NEGATIVE half-wave: -(0.8 - 0.665) / 0.2 * 0.85 = -0.5733.
	CHECK(d.comb.feedback == doctest::Approx(-0.5733f).epsilon(1e-3));
	CHECK(d.comb.mix == doctest::Approx(0.3666f).epsilon(1e-3));
	CHECK(d.springHz == doctest::Approx(1.0f).epsilon(1e-3));
	CHECK(d.combSpringHz == doctest::Approx(8.0f).epsilon(1e-3));
	CHECK(d.stereoPhaseOffset == doctest::Approx(0.0831f).epsilon(1e-3));
	CHECK(d.lfoStartPhase == doctest::Approx(0.6698f).epsilon(1e-3));
}

TEST_CASE("automod derive: the LFO rate reaches the springs and NOTHING else") {
	// **THIS IS THE LICENCE THE CARD'S STRIP DRAWS UNDER, and it is a claim rather than a
	// convenience.** A surface has no transport clock, so it derives at the Rate knob's free
	// frequency — which would be a lie about a synced device if the rate touched anything the picture
	// shows. It does not: `deriveAutomod` reads `lfoHz` only where the springs are computed, because
	// the firmware asks for a number of BOUNCES PER CYCLE rather than a fixed frequency.
	//
	// THREE DECADES APART, so a dependence too weak to see at neighbouring rates still shows.
	//
	// THE MOD COORDINATE IS 0.09 RATHER THAN THE STRIP FIXTURE'S 0.53, AND IT WAS MEASURED. At 0.53
	// the field's bounciness scalar is exactly zero, so `springZeta` is 1.0 at BOTH rates and the
	// "and the springs do move" half of this case was vacuous for a third of its claim — caught by
	// the check itself failing, which is the only reason it is not still vacuous. At 0.09 all three
	// spring outputs are live: zeta 0.522 -> 0.989 and springHz 0.500 -> 9.500 across these rates.
	AutomodCoords slow;
	slow.type = 0.76f;
	slow.flavor = 0.29f;
	slow.mod = 0.09f;
	slow.lfoHz = 0.02f;
	AutomodCoords fast = slow;
	fast.lfoHz = 19.0f;

	const AutomodDerived a = deriveAutomod(slow);
	const AutomodDerived b = deriveAutomod(fast);

	// EXACT EQUALITY, NOT A TOLERANCE. The claim is that the rate never enters these arithmetics at
	// all, and a tolerance would pass against a build where it enters them faintly.
	CHECK(a.shape == b.shape);
	CHECK(a.lfoStartPhase == b.lfoStartPhase);
	CHECK(a.filterCutoffBase == b.filterCutoffBase);
	CHECK(a.filterModDepth == b.filterModDepth);
	CHECK(a.filterMix == b.filterMix);
	CHECK(a.comb == b.comb);
	CHECK(a.combPhaseOffset == b.combPhaseOffset);
	CHECK(a.stereoPhaseOffset == b.stereoPhaseOffset);

	// AND THE SPRINGS DO MOVE, which is what stops every check above from being a statement about a
	// derive that ignores its argument entirely.
	CHECK(a.springHz != b.springHz);
	CHECK(a.combSpringHz != b.combSpringHz);
	CHECK(a.springZeta != b.springZeta);
}

TEST_CASE("automod derive: the device gamma moves EVERY axis, not only the waveform") {
	// **THE CLAIM A KNOWN-BAD FOUND UNGUARDED.** Zeroing the gamma inside `coordsOf` once left 555
	// cases green across four binaries — because the assembly ALSO passed gamma to the LFO by a
	// second road, so the honesty sweep still saw Gamma change the sound through the half that had
	// not been cut. That second road is gone (`Lfo::setShape` is handed the derive's own shape now),
	// and this case is the direct statement rather than the consequence: gamma is a phase into the
	// WHOLE field, so it must move the filter, the comb and the stereo picture too.
	AutomodCoords plain;
	plain.type = 0.76f;
	plain.flavor = 0.29f;
	plain.mod = 0.53f;
	AutomodCoords shifted = plain;
	shifted.gammaPhase = 3.5f;

	const AutomodDerived a = deriveAutomod(plain);
	const AutomodDerived b = deriveAutomod(shifted);

	CHECK(a.filterCutoffBase != b.filterCutoffBase);
	CHECK(a.comb.baseDelaySamples != b.comb.baseDelaySamples);
	CHECK(a.comb.feedback != b.comb.feedback);
	CHECK(a.stereoPhaseOffset != b.stereoPhaseOffset);
	CHECK_FALSE(a.shape == b.shape);

	// A WHOLE LAP IS NOT A NO-OP EITHER, which is what "keep turning past the last zone" means: the
	// field is not periodic in 1.0, so gamma 1 is a different place from gamma 0 rather than the same
	// place again.
	AutomodCoords lap = plain;
	lap.gammaPhase = 1.0f;
	const AutomodDerived c = deriveAutomod(lap);
	CHECK(a.filterCutoffBase != c.filterCutoffBase);
}

TEST_CASE("automod derive: a rank-1 home is the old device exactly, and a tilted one is not") {
	using namespace feathers::fx::automod;
	namespace ps = phase_space;

	AutomodCoords c {};
	c.type = 0.4321f;
	c.flavor = 0.6789f;
	c.mod = 0.2468f;
	c.gammaPhase = 0.1111f;
	c.lfoHz = 1.0f;

	const AutomodDerived viaCoords = deriveAutomod(c);

	const auto f = [](float v) { return static_cast<double>(v); };
	const double typePhase = f(c.type) + f(c.gammaPhase);
	const double flavourPhase = f(c.flavor) + f(c.gammaPhase);
	const double modPhase = f(c.mod) + f(c.gammaPhase);
	const ps::Phases rank1 = ps::homeFor(typePhase, flavourPhase, modPhase);

	// EVERY OBSERVABLE THE DERIVE PRODUCES, compared exactly. Not a spot check: the point of the claim
	// is that NOTHING moved, and a comparison of three fields would pass while the other twenty drifted.
	const auto same = [](const AutomodDerived& a, const AutomodDerived& b, bool exact) {
		size_t differences = 0;
		const auto cmp = [&](double x, double y) {
			if (exact ? (x != y) : (std::fabs(x - y) > 1e-12)) {
				++differences;
			}
		};
		cmp(a.filterCutoffBase, b.filterCutoffBase);
		cmp(a.filterResonance, b.filterResonance);
		cmp(a.filterModDepth, b.filterModDepth);
		cmp(a.svfFeedback, b.svfFeedback);
		for (int i = 0; i < 3; ++i) {
			cmp(a.bandResponse[i], b.bandResponse[i]);
			cmp(a.bandPhaseOffset[i], b.bandPhaseOffset[i]);
		}
		cmp(a.comb.mix, b.comb.mix);
		cmp(a.comb.feedback, b.comb.feedback);
		cmp(a.combPhaseOffset, b.combPhaseOffset);
		cmp(a.envAttackSeconds, b.envAttackSeconds);
		cmp(a.envReleaseSeconds, b.envReleaseSeconds);
		cmp(a.envDepthInfluence, b.envDepthInfluence);
		cmp(a.envPhaseInfluence, b.envPhaseInfluence);
		cmp(a.springHz, b.springHz);
		cmp(a.combSpringHz, b.combSpringHz);
		cmp(a.stereoPhaseOffset, b.stereoPhaseOffset);
		cmp(a.lfoStartPhase, b.lfoStartPhase);
		return differences;
	};

	// **THE PREMISE THE WHOLE 39-COORDINATE CHANGE WAS ARGUED ON.** `evalAt` handing every triangle in
	// a bank the same phase reproduces `evalTriangleBank` — that function's scalar path IS this loop
	// and NEON is defined nowhere in this repo — so the space can exist without the device moving.
	SUBCASE("a rank-1 home reproduces the three-coordinate derive, exactly") {
		CHECK(same(viaCoords, deriveAutomodAt(c, rank1), /*exact=*/true) == 0);
		CHECK(deriveAutomodAt(c, rank1).shape == viaCoords.shape);
	}

	// **AND THE SPACE IS LIVE, WHICH IS THE OTHER HALF.** A home that is NOT rank-1 — one triangle of
	// Type's six moved on its own — must produce a different device. Without this the case above would
	// pass just as well on an implementation that ignored the vector entirely and used the coordinates,
	// which is exactly the vacuous-green shape this repo keeps meeting.
	SUBCASE("moving ONE triangle off the group changes the derive") {
		ps::Phases tilted = rank1;
		tilted[ps::kTypeScalars + 1] += 0.25; // one of the comb's three, alone
		CHECK(same(viaCoords, deriveAutomodAt(c, tilted), /*exact=*/true) > 0);
	}

}

TEST_CASE("automod derive: EVERY ONE of the 39 coordinates reaches the sound") {
	using namespace feathers::fx::automod;
	namespace ps = phase_space;

	// **THE HONESTY CHECK ON N ITSELF.** Claiming a 39-dimensional space is claiming 39 coordinates
	// that do something. If some triangle's phase were wired to a shared value by accident — the nine
	// inside `lfoShapeAt` are the likeliest, being invisible from `automod_derived.h` — the space
	// would be smaller than advertised and a plane tilted into those directions would go nowhere.
	//
	// **SWEPT, NOT PROBED ONCE, AND THE FIRST VERSION OF THIS WAS PROBED ONCE.** At a single position
	// ten of the 39 read as dead — the banks are HALF-RECTIFIED, so a triangle contributes nothing
	// over much of its range and "dead here" is the normal state rather than a defect. Asking at one
	// point would have reported a third of the space inert and been wrong.
	//
	// **AND THE COMPARISON IS THE WHOLE STRUCT.** A hand-listed subset of `AutomodDerived` was the
	// first version and it missed `filterMix`, the envelope influences and the springs — reporting 22
	// dead where the answer is none. A check cannot see what it does not enumerate, twice in one case.
	std::array<int, ps::kTriangles> liveAt {};
	int probes = 0;
	for (float t = 0.05f; t < 1.0f; t += 0.17f) {
		for (float fl = 0.05f; fl < 1.0f; fl += 0.19f) {
			for (float m = 0.05f; m < 1.0f; m += 0.23f) {
				AutomodCoords c {};
				c.type = t;
				c.flavor = fl;
				c.mod = m;
				c.lfoHz = 1.0f;
				const ps::Phases base = ps::homeFor(t, fl, m);
				const AutomodDerived ref = deriveAutomodAt(c, base);
				++probes;
				for (size_t i = 0; i < ps::kTriangles; ++i) {
					ps::Phases x = base;
					x[i] += 0.37;
					const AutomodDerived d = deriveAutomodAt(c, x);
					// The struct's own `operator!=`, not memcmp: `AutomodDerived` has padding after its
					// `bool`, and over padding memcmp could count a coordinate as live when nothing
					// derived had moved (test_fx_automod.cpp:1581 went red on macOS for the mirror
					// reason, 2026-09-07).
					if (d != ref) {
						++liveAt[i];
					}
				}
			}
		}
	}

	CAPTURE(probes);
	REQUIRE(probes > 100);
	for (size_t i = 0; i < ps::kTriangles; ++i) {
		CAPTURE(i);
		CAPTURE(liveAt[i]);
		// REACHES THE SOUND SOMEWHERE — and comfortably, not once in a hundred. Measured: every one of
		// the 39 is live at more than a quarter of 150 probes.
		CHECK(liveAt[i] * 4 > probes);
	}
}


TEST_CASE("IS.5: comb feedback is BIPOLAR, its sign the field's — 0, +max, 0, -max, 0, then the dead zone") {
	// By design comb feedback is phi-derived, not a negative switch: the feedback triangle is simply
	// bipolar. RESTATED from a literal config rather than the bank, so a bank
	// that quietly went back to unipolar reads a different number.
	namespace ps = phase_space;
	constexpr phi::PhiTriConfig kBipolarFeedback { phi::kPhi250, 0.8f, 0.0f, true };
	int positive = 0;
	int negative = 0;
	int silent = 0;
	float lowest = 0.0f;
	float highest = 0.0f;
	constexpr int kSteps = 2000;
	const double period = 1.0 / static_cast<double>(phi::kPhi250);
	for (int k = 0; k < kSteps; ++k) {
		ps::Phases ph {};
		ph[ps::kTypeScalars] = period * static_cast<double>(k) / kSteps;
		AutomodCoords c;
		c.lfoHz = 1.0f;
		const AutomodDerived d = deriveAutomodAt(c, ph);
		CHECK(d.comb.feedback == doctest::Approx(0.85f * phi::evalTriangle(ph[ps::kTypeScalars], 1.0f, kBipolarFeedback)));
		positive += d.comb.feedback > 1e-4f;
		negative += d.comb.feedback < -1e-4f;
		silent += std::fabs(d.comb.feedback) <= 1e-4f;
		lowest = std::min(lowest, d.comb.feedback);
		highest = std::max(highest, d.comb.feedback);
	}
	CAPTURE(positive);
	CAPTURE(negative);
	CAPTURE(silent);
	// THE TRIANGLE'S PROPORTIONS: 40% up, 40% down, 20% dead.
	CHECK(positive == doctest::Approx(0.4 * kSteps).epsilon(0.02));
	CHECK(negative == doctest::Approx(0.4 * kSteps).epsilon(0.02));
	CHECK(highest == doctest::Approx(0.85f).epsilon(1e-2));
	CHECK(lowest == doctest::Approx(-0.85f).epsilon(1e-2));
}

TEST_CASE("IS.5: the field's longest comb, with the LFO at rest, is D-flat 1") {
	// The comb reaches all the way down to D-flat 1 with the LFO at rest. SWEPT, not assumed: the
	// base-delay triangle is walked across a full cycle and its longest base read back as a pitch.
	namespace ps = phase_space;
	float longest = 0.0f;
	for (int k = 0; k < 4000; ++k) {
		ps::Phases ph {};
		ph[ps::kFlavourScalars + 5] = static_cast<double>(k) / 4000.0 * 4.0; // several cycles of any phi rate
		AutomodCoords c;
		c.lfoHz = 1.0f;
		longest = std::max(longest, deriveAutomodAt(c, ph).comb.baseDelaySamples);
	}
	const double hz = kCombReferenceRate / static_cast<double>(longest);
	CAPTURE(longest);
	CAPTURE(hz);
	// MIDI 25 = 34.648 Hz; within a few cents (the sweep's grid, not the design, sets the tolerance).
	CHECK(hz == doctest::Approx(34.6478).epsilon(2e-3));
	// AND THE LFO REACHES BELOW IT, not only to it: the longest delay leaves the full range beneath the base.
	CHECK(kCombMaxDelaySamples == doctest::Approx(longest + kCombMaxModRangeSamples).epsilon(1e-3));
}
