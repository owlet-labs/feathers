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
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/character_bank.h"
#include "dsp/flagship.h"
#include "dsp/slice_build.h"
#include "dsp/warp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// WARP'S CASES — the shapes, and what they do to the engine.
//
// THE RULE THIS FILE IS WRITTEN UNDER is CLAUDE.md's: a claim about a value cannot be inferred from a
// rendered buffer. Warp has ten shapes and every one of them changes the cycle, so "the buffer moved"
// says nothing about WHICH shape moved it or whether it moved it correctly. Nearly every case below
// reads a number — a warped phase, a peak, a recipe flag.

using namespace feathers;

namespace {

/// Every mode, so a case cannot quietly test nine of ten.
constexpr std::array<WarpMode, 9> kShapes {
    WarpMode::BendPlus,  WarpMode::BendMinus,  WarpMode::BendBipolar,
    WarpMode::Pwm,       WarpMode::AsymPlus,   WarpMode::AsymMinus,
    WarpMode::AsymBipolar, WarpMode::Flip,     WarpMode::Mirror,
};

/// The knob position at which a mode does nothing — 0.5 for the bipolar pair, 0 for the rest.
float restingKnob(WarpMode mode) {
	return warpIsBipolar(mode) ? 0.5f : 0.0f;
}

FlagshipRecipe recipeAt(uint16_t zone, WarpMode mode, float depth,
                        int probe = kFlagshipNormaliseSamples) {
	FieldScales scales {};
	scales.warpDepth = depth;
	const MorphBasis morph = sliceFrameFor<kMorphTriangles>(0.0, 0.0, {});
	const StairBasis stair = sliceFrameFor<kStairTriangles>(0.0, 0.0, {});
	return buildFlagshipSliced(zone, 0.0f, 0.0, 0.0, morph, stair, 0.0, scales, probe, mode);
}

std::vector<float> cycleOf(const FlagshipRecipe& r, int n = 1024) {
	std::vector<float> out(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		out[static_cast<size_t>(i)] = evalFlagship(r, static_cast<double>(i) / n);
	}
	return out;
}


} // namespace

//--------------------------------------------------------------------------------------------//
// THE SHAPES                                                                                   //
//--------------------------------------------------------------------------------------------//

TEST_CASE("every warp mode is the exact identity at its own resting knob") {
	// THE PROPERTY THE WHOLE FEATURE RESTS ON. `FlagshipRecipe::operated` stays false when no warp is
	// doing anything, which is what keeps V5's identity corner coinciding with Morph BIT FOR BIT. A
	// warp that was merely *nearly* the identity at rest would have cost that quietly.
	//
	// AND THE RESTING KNOB IS NOT THE SAME NUMBER FOR EVERY MODE — 0.5 for the bipolar pair, 0 for
	// the rest — which is exactly the trap a case that only ever tested knob 0 would fall into.
	for (int m = 0; m < static_cast<int>(WarpMode::kCount); ++m) {
		const auto mode = static_cast<WarpMode>(m);
		CAPTURE(kWarpModeNames[m]);
		for (int i = 0; i <= 512; ++i) {
			const double p = static_cast<double>(i) / 512.0;
			const WarpRead w = warpRead(mode, p, restingKnob(mode));
			REQUIRE(w.phase == p); // exact, not Approx
			REQUIRE(w.sign == 1.0f);
		}
	}
}

TEST_CASE("Off is the identity at EVERY knob position, which is what makes the dropdown safe") {
	// A player who turns the depth up and then selects Off must hear nothing, without having to put
	// the knob back. That is a different claim from the case above and it is the one a mode menu
	// needs: `Off` ignores the knob entirely rather than resting at a particular value.
	for (const float knob : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
		CAPTURE(knob);
		for (int i = 0; i <= 256; ++i) {
			const double p = static_cast<double>(i) / 256.0;
			REQUIRE(warpRead(WarpMode::Off, p, knob).phase == p);
		}
	}
}

TEST_CASE("the monotonic shapes stay monotonic, and Mirror is the one that folds") {
	// MONOTONICITY IS NOT DECORATION. A phase warp that goes backwards reads the cycle in reverse for
	// part of the output, which is a fold — and a fold is a different effect with a different harmonic
	// signature. Bend, Asym and PWM are reparametrisations and must never fold; Mirror is a fold and
	// must, or it is not Mirror.
	for (const WarpMode mode : kShapes) {
		CAPTURE(kWarpModeNames[static_cast<int>(mode)]);
		for (const float knob : {0.0f, 0.35f, 1.0f}) {
			CAPTURE(knob);
			double previous = -1.0;
			bool folded = false;
			for (int i = 0; i <= 4096; ++i) {
				const WarpRead w = warpRead(mode, static_cast<double>(i) / 4096.0, knob);
				// IN RANGE, ALWAYS. A phase outside [0,1] would be wrapped by evalMorphTable into a
				// place nobody chose, so the shape would be right and the position wrong.
				REQUIRE(w.phase >= -1e-12);
				REQUIRE(w.phase <= 1.0 + 1e-12);
				if (w.phase < previous - 1e-12) {
					folded = true;
				}
				previous = w.phase;
			}
			if (mode == WarpMode::Mirror && knob != 0.0f) {
				CHECK(folded);
			}
			else {
				CHECK_FALSE(folded);
			}
		}
	}
}

TEST_CASE("Bend keeps the cycle's ends AND its middle; Asym moves the middle") {
	// THE DIFFERENCE BETWEEN THE TWO FAMILIES, asserted rather than described. Both are monotonic
	// reparametrisations fixed at the edges, and the ONLY thing that separates them is what happens at
	// 0.5 — Bend is symmetric about it, Asym leans through it. A port that got this backwards would
	// pass every other case in this file.
	for (const WarpMode mode : {WarpMode::BendPlus, WarpMode::BendMinus}) {
		CAPTURE(kWarpModeNames[static_cast<int>(mode)]);
		CHECK(warpRead(mode, 0.0, 1.0f).phase == doctest::Approx(0.0));
		CHECK(warpRead(mode, 1.0, 1.0f).phase == doctest::Approx(1.0));
		CHECK(warpRead(mode, 0.5, 1.0f).phase == doctest::Approx(0.5));
		// AND IT IS GENUINELY BENT AWAY FROM THE MIDDLE — otherwise "fixed at three points" would be
		// satisfied by the identity, and this case would be vacuous.
		CHECK(std::fabs(warpRead(mode, 0.25, 1.0f).phase - 0.25) > 0.05);
	}
	for (const WarpMode mode : {WarpMode::AsymPlus, WarpMode::AsymMinus}) {
		CAPTURE(kWarpModeNames[static_cast<int>(mode)]);
		CHECK(warpRead(mode, 0.0, 1.0f).phase == doctest::Approx(0.0));
		CHECK(warpRead(mode, 1.0, 1.0f).phase == doctest::Approx(1.0));
		CHECK(std::fabs(warpRead(mode, 0.5, 1.0f).phase - 0.5) > 0.1);
	}
	// AND THE TWO DIRECTIONS GO OPPOSITE WAYS, which is the whole of the `+` / `-` distinction.
	CHECK(warpRead(WarpMode::AsymPlus, 0.5, 1.0f).phase
	      > warpRead(WarpMode::AsymMinus, 0.5, 1.0f).phase);
	CHECK(warpRead(WarpMode::BendPlus, 0.25, 1.0f).phase
	      > warpRead(WarpMode::BendMinus, 0.25, 1.0f).phase);
}

TEST_CASE("the bipolar modes reach both directions from one knob") {
	// WHY THE PAIR EXISTS AT ALL. Serum's `+/-` modes sweep through both signs on one control, which
	// is why their resting place is the MIDDLE. A bipolar mode whose two ends did the same thing would
	// be a unipolar mode with half the travel wasted.
	for (const WarpMode mode : {WarpMode::BendBipolar, WarpMode::AsymBipolar}) {
		CAPTURE(kWarpModeNames[static_cast<int>(mode)]);
		const double low = warpRead(mode, 0.25, 0.0f).phase;
		const double high = warpRead(mode, 0.25, 1.0f).phase;
		CHECK(low < 0.25);
		CHECK(high > 0.25);
		CHECK(warpIsBipolar(mode));
	}
	for (const WarpMode mode : {WarpMode::BendPlus, WarpMode::Pwm, WarpMode::Mirror}) {
		CHECK_FALSE(warpIsBipolar(mode));
	}
}

TEST_CASE("Flip inverts a tail of the cycle and warps no phase at all") {
	// FLIP IS THE ONE MODE THAT IS NOT A PHASE WARP — Serum calls it "an instantaneous polarity flip"
	// — and forcing it into the phase domain would have been a different effect wearing its name. So
	// it hands back the phase untouched and carries its whole effect in the sign.
	for (int i = 0; i <= 512; ++i) {
		const double p = static_cast<double>(i) / 512.0;
		CHECK(warpRead(WarpMode::Flip, p, 1.0f).phase == p);
	}
	// EXACTLY ONE EDGE, and it is in the second half — at full depth the flip covers the last half of
	// the cycle, so the waveform steps once rather than chattering.
	int edges = 0;
	float last = warpRead(WarpMode::Flip, 0.0, 1.0f).sign;
	for (int i = 1; i <= 4096; ++i) {
		const float s = warpRead(WarpMode::Flip, static_cast<double>(i) / 4096.0, 1.0f).sign;
		if (s != last) {
			++edges;
			CHECK(static_cast<double>(i) / 4096.0 > 0.4);
		}
		last = s;
	}
	CHECK(edges == 1);
	// AND AT REST NOTHING IS INVERTED.
	for (int i = 0; i <= 256; ++i) {
		CHECK(warpRead(WarpMode::Flip, static_cast<double>(i) / 256.0, 0.0f).sign == 1.0f);
	}
}

//--------------------------------------------------------------------------------------------//
// THE ENGINE                                                                                   //
//--------------------------------------------------------------------------------------------//

TEST_CASE("a warp at rest leaves the recipe and the cycle exactly as they were") {
	const FlagshipRecipe plain = recipeAt(0, WarpMode::Off, 0.0f);
	const std::vector<float> reference = cycleOf(plain);

	SUBCASE("Off ignores the depth knob entirely") {
		const std::vector<float> hot = cycleOf(recipeAt(0, WarpMode::Off, 1.0f));
		for (size_t i = 0; i < reference.size(); ++i) {
			REQUIRE(hot[i] == reference[i]);
		}
	}

	SUBCASE("a bipolar mode parked at its neutral is inert too") {
		// THE GUARD IN `prepareFlagshipRecipe` ASKS `warpAmount`, NOT `warpDepth > 0` — because a
		// resting Bend +/- sits at knob 0.5. Asking the knob's number instead of the shape's neutral
		// would call it operated and push an untouched cycle onto the baked path.
		for (const WarpMode mode : {WarpMode::BendBipolar, WarpMode::AsymBipolar}) {
			CAPTURE(kWarpModeNames[static_cast<int>(mode)]);
			const std::vector<float> rest = cycleOf(recipeAt(0, mode, 0.5f));
			for (size_t i = 0; i < reference.size(); ++i) {
				REQUIRE(rest[i] == reference[i]);
			}
		}
	}
}

TEST_CASE("warp alone is enough to make a recipe operated") {
	// `operated` IS THE GATE ON THE WHOLE BAKED PATH. A warp that changed the cycle without setting it
	// would be rendered through the continuous morph path — unbaked, unmipped, and aliasing — which is
	// precisely the failure this port claims to avoid. So it has to be asserted on a position where
	// nothing ELSE is operating, or the flag would read true for someone else's reason.
	uint16_t idle = 0;
	bool found = false;
	for (uint16_t z = 0; z < 4096 && !found; ++z) {
		if (!recipeAt(z, WarpMode::Off, 0.0f).operated) {
			idle = z;
			found = true;
		}
	}
	REQUIRE_MESSAGE(found, "no unoperated zone in reach — this case cannot see the flag it is about");
	CAPTURE(idle);
	CHECK_FALSE(recipeAt(idle, WarpMode::Off, 1.0f).operated);
	// A MODE WITH NO DEPTH IS STILL NOTHING, and a depth with no mode is too.
	CHECK_FALSE(recipeAt(idle, WarpMode::BendPlus, 0.0f).operated);
	CHECK(recipeAt(idle, WarpMode::BendPlus, 0.6f).operated);
}

TEST_CASE("every warp shape changes the sound") {
	// A DIFFERENCE CHECK USED THE WAY CLAUDE.md ALLOWS: as the second assertion, proving the shapes
	// asserted above actually reach the samples. What each shape IS has been asserted by value.
	const std::vector<float> reference = cycleOf(recipeAt(0, WarpMode::Off, 0.0f));
	for (const WarpMode mode : kShapes) {
		CAPTURE(kWarpModeNames[static_cast<int>(mode)]);
		const std::vector<float> warped = cycleOf(recipeAt(0, mode, 1.0f));
		double sum = 0.0;
		for (size_t i = 0; i < reference.size(); ++i) {
			sum += std::fabs(warped[i] - reference[i]);
		}
		CHECK(sum / static_cast<double>(reference.size()) > 1e-4);
	}
}

TEST_CASE("a warped cycle is normalised to full scale and does not clip") {
	// THE DEFECT THIS PORT HAD TO FIX BEFORE IT COULD SHIP, and the reason `prepareFlagshipRecipe`
	// probes a warped recipe at the bake's own grid.
	//
	// A WARP COMPRESSES READ PHASE SOMEWHERE — that IS the effect — so a peak probe uniform in OUTPUT
	// phase becomes sparse in READ phase exactly where the curve is steepest. MEASURED at 128 points,
	// worst baked peak over 64 zones: PWM 2.881 (9 dB over full scale), Asym + 1.616, Bend - 1.659.
	// At 256 — `kFlagshipWarpedNormaliseSamples`, which IS `kFlagshipSlots` — every one is 1.000.
	const auto worstBakedPeak = [](WarpMode mode) {
		float worst = 0.0f;
		for (uint16_t z = 0; z < 64; ++z) {
			const FlagshipRecipe r = recipeAt(static_cast<uint16_t>(z * 16), mode, 1.0f);
			if (!r.operated) {
				continue;
			}
			FlagshipTable table {};
			bakeFlagshipTable(r, table);
			for (int i = 0; i < kFlagshipSlots; ++i) {
				worst = std::max(worst, std::fabs(table.slots[i]));
			}
		}
		return worst;
	};

	SUBCASE("every warp shape lands at full scale") {
		for (const WarpMode mode : kShapes) {
			CAPTURE(std::string(kWarpModeNames[static_cast<int>(mode)]));
			const float worst = worstBakedPeak(mode);
			CAPTURE(worst);
			CHECK(worst <= 1.02f);
		}
	}

	SUBCASE("AND `Off` DOES NOT, WHICH IS A PRE-EXISTING DEFECT THIS CHANGE DELIBERATELY LEFT ALONE") {
		// **THIS IS NOT WARP'S BUG AND IT IS NOT FIXED HERE.** With no warp at all the same 64 zones
		// reach 1.118 — `flagship.h`'s own failure criterion, "a cycle that peaks at 1.20 with the
		// Level fader at 0 dB is over full scale for a reason the musician cannot see or fix" —
		// because the unwarped probe still samples every other bake slot.
		//
		// FIXING IT IS ONE CONSTANT (`kFlagshipNormaliseSamples` 128 -> 256, measured: every mode
		// including Off goes to exactly 1.000) and it is NOT DONE, because it re-levels sounds nobody
		// asked to change and `test_vox_voice.cpp` pins those bits on purpose. A warp feature may not
		// re-level the whole engine on its way in.
		//
		// PINNED AS A NUMBER SO THE FINDING CANNOT BE LOST. If someone fixes it, this case fails and
		// tells them where the decision was recorded.
		const float worst = worstBakedPeak(WarpMode::Off);
		CAPTURE(worst);
		CHECK(worst > 1.05f);
		CHECK(worst == doctest::Approx(1.118f).epsilon(0.02));
	}
}

TEST_CASE("the exact probe is what makes a warped peak right, and 256 is exact because it IS the grid") {
	// THE SECOND ASSERTION THAT STOPS THE ONE ABOVE BEING VACUOUS. If the peak were fine at any probe
	// density, the exact-grid rule would be machinery guarding nothing.
	//
	// IT CANNOT BE SHOWN BY ASKING FOR A COARSE PROBE — the override refuses one for a warped recipe,
	// which is the feature working. So the counterfactual is computed: the gain a 128-point probe
	// would have produced is `1/peak128`, and the true grid peak is `peak256`, so a 128-probe build
	// would have baked to `peak256 / peak128` instead of to 1.0.
	const auto probeEstimate = [](FlagshipRecipe r, int n) {
		r.dcOffset = 0.0f;
		r.gain = 1.0f;
		double sum = 0.0;
		for (int i = 0; i < n; ++i) {
			sum += evalFlagship(r, static_cast<double>(i) / n);
		}
		const double dc = sum / n;
		double peak = 0.0;
		for (int i = 0; i < n; ++i) {
			peak = std::max(peak, std::fabs(evalFlagship(r, static_cast<double>(i) / n) - dc));
		}
		return peak;
	};

	SUBCASE("a 128-point probe would have shipped PWM far over full scale") {
		// OVER THE ZONES, NOT AT ONE POSITION. The under-estimate depends on where the warp puts the
		// steep part relative to the probe grid, so a single position measures a coincidence. MEASURED
		// worst baked peak at 128 over these 64 zones: PWM 2.881, Bend - 1.659, Asym + 1.616.
		double worstRatio = 0.0;
		for (uint16_t z = 0; z < 64; ++z) {
			const FlagshipRecipe r = recipeAt(static_cast<uint16_t>(z * 16), WarpMode::Pwm, 1.0f);
			if (!r.operated) {
				continue;
			}
			const double coarse = probeEstimate(r, 128);
			const double exact = probeEstimate(r, kFlagshipWarpedNormaliseSamples);
			if (coarse > 1.0e-6) {
				worstRatio = std::max(worstRatio, exact / coarse);
			}
		}
		CAPTURE(worstRatio);
		CHECK(worstRatio > 1.5);
	}

	SUBCASE("AND 256 IS EXACT BECAUSE IT IS THE GRID THE VOICE PLAYS — not because it is denser") {
		// THE CLAIM THAT MAKES THIS A CEILING RATHER THAN A STEP ON A CURVE, and it is about the BAKED
		// signal rather than the continuous one. An operated voice renders the 256-slot grid, and
		// linear interpolation between slots cannot exceed them — so the peak over those slots IS the
		// peak of what is heard, and a probe on the same grid measures it with no error left.
		//
		// (The continuous function does peak higher BETWEEN slots — measured 0.817 against 0.580 at
		// one position. That value is never rendered, which is exactly why probing the continuum more
		// densely would be measuring a signal nobody plays.)
		for (const WarpMode mode : {WarpMode::Pwm, WarpMode::BendPlus, WarpMode::AsymMinus}) {
			CAPTURE(std::string(kWarpModeNames[static_cast<int>(mode)]));
			const FlagshipRecipe r = recipeAt(0, mode, 1.0f);
			REQUIRE(r.operated);
			FlagshipTable table {};
			bakeFlagshipTable(r, table);
			double bakedPeak = 0.0;
			for (int i = 0; i < kFlagshipSlots; ++i) {
				bakedPeak = std::max(bakedPeak, std::fabs(static_cast<double>(table.slots[i])));
			}
			CAPTURE(bakedPeak);
			CHECK(bakedPeak == doctest::Approx(1.0).epsilon(0.02));
		}
	}
}

TEST_CASE("a warp does not promote the PICTURE's probe — the terrain keeps the cheap answer it asked for") {
	// THE BUG THIS EXISTS TO STOP, and it shipped once: the promotion to the bake grid was written
	// `warped ? kFlagshipWarpedNormaliseSamples : normaliseSamples`, which promoted EVERY caller. The
	// terrain's 32 became 256 and a warped map paid eight times over — while the comment directly
	// above that line said the picture's probe was "deliberately not promoted here" and cited the
	// very cost it was incurring.
	//
	// Found by feel: with warp on, the terrain render slowed down a lot.
	// MEASURED per texel at the picture's probe count: 2.84 us unwarped against 25.82 us warped —
	// **9.1x**, a 128x128 map going 47 ms -> 423 ms. After the fix, 1.6x and 94 ms, and the residual
	// is warp genuinely turning operators on rather than probe work.
	//
	// **A COMMENT IS NOT A GUARD.** This is, and it is a VALUE assertion rather than a timing one: a
	// cost case would be flaky and would not say what went wrong. Under a warp the two probe counts
	// genuinely disagree about the peak — that disagreement IS the defect the audio path fixes — so if
	// the picture's recipe has the audio's gain, the promotion has leaked back.
	const auto gainAt = [](int probe) {
		FieldScales scales {};
		scales.warpDepth = 1.0f;
		const MorphBasis morph = sliceFrameFor<kMorphTriangles>(0.0, 0.0, {});
		const StairBasis stair = sliceFrameFor<kStairTriangles>(0.0, 0.0, {});
		return buildFlagshipSliced(0, 0.0f, 0.0, 0.0, morph, stair, 0.0, scales, probe,
		                           WarpMode::Pwm)
		    .gain;
	};

	const float picture = gainAt(kFlagshipNormalisePictureSamples);
	const float audio = gainAt(kFlagshipNormaliseSamples);
	CAPTURE(picture);
	CAPTURE(audio);

	// THE PICTURE KEPT ITS OWN ANSWER. Equal gains would mean 32 was silently turned into 256.
	CHECK(picture != audio);

	// NON-VACUITY: the two must differ for the reason claimed — a coarse probe UNDER-estimates the
	// peak, so it produces a LARGER gain. If they merely differed by rounding this case would pass on
	// a promotion that had leaked back and then been perturbed.
	CHECK(picture > audio);

	SUBCASE("and the AUDIO path is still promoted, which is the half that must not regress") {
		// The fix is conditional, so it has two ways to be wrong. This is the other one.
		FieldScales scales {};
		scales.warpDepth = 1.0f;
		const MorphBasis morph = sliceFrameFor<kMorphTriangles>(0.0, 0.0, {});
		const StairBasis stair = sliceFrameFor<kStairTriangles>(0.0, 0.0, {});
		const FlagshipRecipe r = buildFlagshipSliced(0, 0.0f, 0.0, 0.0, morph, stair, 0.0, scales,
		                                             kFlagshipNormaliseSamples, WarpMode::Pwm);
		REQUIRE(r.operated);
		FlagshipTable table {};
		bakeFlagshipTable(r, table);
		double peak = 0.0;
		for (int i = 0; i < kFlagshipSlots; ++i) {
			peak = std::max(peak, std::fabs(static_cast<double>(table.slots[i])));
		}
		CAPTURE(peak);
		CHECK(peak == doctest::Approx(1.0).epsilon(0.02));
	}
}

TEST_CASE("the warp mode is engine-wide and the depth blends per sound") {
	// THE SPLIT, ASSERTED. An enum cannot be blended and a depth can, which is why they live in
	// different places — `Character::warpMode` against `SoundCoord::warpDepth`.
	Character c {};
	c.flagship = true;
	c.warpMode = WarpMode::AsymPlus;
	c.count = 2;
	c.sounds[0].zone = 0;
	c.sounds[0].weight = 0.5;
	c.sounds[0].warpDepth = 1.0f;
	c.sounds[1].zone = 64;
	c.sounds[1].weight = 0.5;
	c.sounds[1].warpDepth = 0.0f;

	const FlagshipRecipe blended = buildFlagshipFor(c);
	CHECK(blended.warpMode == WarpMode::AsymPlus);
	// Σ w·depth — half of one and half of nothing.
	CHECK(blended.warpDepth == doctest::Approx(0.5f));

	SUBCASE("and the mode travels whatever the sounds say") {
		c.warpMode = WarpMode::Mirror;
		CHECK(buildFlagshipFor(c).warpMode == WarpMode::Mirror);
	}

	SUBCASE("two sounds both undepthed leave the engine unwarped") {
		c.sounds[0].warpDepth = 0.0f;
		const FlagshipRecipe none = buildFlagshipFor(c);
		CHECK(none.warpDepth == doctest::Approx(0.0f));
	}
}
