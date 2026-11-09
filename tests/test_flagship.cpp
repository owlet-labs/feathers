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

#include "dsp/character_bank.h"
#include "dsp/flagship.h"
#include "dsp/morph_renderer.h"
#include "dsp/osc_voice.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

// THE OPERATOR TIER'S PROOF — plan §S2's required cases, each with the known-bad that shows the
// case has teeth. The claims that carry the most weight:
//
//   - The identity corner is Morph TO THE BIT, by construction — and forcing a depth breaks it,
//     which is what makes the equality evidence rather than tautology.
//   - Position purity: the bank route (a one-sound Character) and the sliced route (the terrain's
//     entry) produce the same operators to the bit, so the map and the sound cannot drift apart.
//   - The fields have exactly the designed duties — quantize wide, sync moderate, feedback SPARSE —
//     and the sparsity is bought by the threshold, shown by measuring the same triangle without it.

using namespace feathers;
using owlet::dsp::PhiMorphWavetable;

namespace {

bool sameBits(const PhiMorphWavetable& a, const PhiMorphWavetable& b) {
	// Whole-struct memcmp is meaningful because both sides are value-initialised at the top of
	// their builder, so padding is zero on both rather than whatever the stack held.
	return std::memcmp(&a, &b, sizeof(PhiMorphWavetable)) == 0;
}

/// A one-sound flagship character at `zone`/`offset`, standing at a travel — the shape every case
/// here starts from. Weight 1.0 by hand, which is what blendWeights computes for a lone sound.
Character oneSound(uint16_t zone, float offset, double gammaBase, double tx, double ty) {
	Character c;
	c.engine = OscEngine::Morph;
	c.count = 1;
	c.sounds[0].zone = zone;
	c.sounds[0].phaseOffset = offset;
	c.sounds[0].gammaBase = gammaBase;
	c.sounds[0].travelX = tx;
	c.sounds[0].travelY = ty;
	c.sounds[0].weight = 1.0;
	return c;
}

/// Scan offsets until the fields at the sound's coordinate satisfy `want`. The stride is an odd
/// irrational-ish number so the scan does not alias with any field's period; the cases below only
/// need SOME position with the property, and say so if the space refuses to provide one.
template <typename Want>
float findOffset(uint16_t zone, double gammaBase, double tx, double ty, Want&& want) {
	for (int i = 0; i < 200000; ++i) {
		const float offset = static_cast<float>(i) * 0.0137f;
		const double home = sliceHome(zone, offset, gammaBase);
		const MorphBasis morphFrame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
		const StairBasis stairFrame = sliceFrameFor<kStairTriangles>(0.0, home, {});
		if (want(flagshipAuthorsAt(morphFrame, stairFrame, home, tx, ty))) {
			return offset;
		}
	}
	REQUIRE_MESSAGE(false, "no offset in range satisfies the predicate — the field layout changed");
	return 0.0f;
}

} // namespace

TEST_CASE("the operator fields have the designed duties, and the feedback threshold is load-bearing") {
	// Uniform-enough sampling over hundreds of field periods (the slowest period is ~4.8 home
	// units, the scan spans ~2300). A unipolar triangle is EXACTLY zero outside its duty, so the
	// zero-fractions are the duties read back: 1−0.85, 1−0.50, and for feedback the thresholded
	// 1 − duty·(1−threshold) = 0.90 the grey-goo decision asks for.
	int qZero = 0;
	int sZero = 0;
	int kZero = 0;
	int kRawZero = 0;
	constexpr int kSamples = 30000;
	for (int i = 0; i < kSamples; ++i) {
		const double at = static_cast<double>(i) * 0.0777;
		const FlagshipFields f = flagshipFieldsAt(at);
		qZero += f.quantize == 0.0f;
		sZero += f.sync == 0.0f;
		kZero += f.feedback == 0.0f;
		kRawZero += owlet::dsp::phi::evalTriangle(at, 1.0f, kFlagshipFieldBank[2]) == 0.0f;
	}
	const double n = kSamples;
	CHECK(qZero / n == doctest::Approx(0.15).epsilon(0.15));
	CHECK(sZero / n == doctest::Approx(0.50).epsilon(0.06));
	CHECK(kZero / n == doctest::Approx(0.90).epsilon(0.03));

	// THE KNOWN-BAD: the same triangle WITHOUT the threshold is zero on only ~60% of the terrain —
	// its duty — nowhere near sparse. The 90% above is bought by max(0, tri − 0.75), not by the
	// duty alone, and a build that dropped the threshold would fail the band above.
	CHECK(kRawZero / n < 0.70);
	// And the fields are not degenerate: each reaches well up its range somewhere.
	float qMax = 0.0f;
	float sMax = 0.0f;
	float kMax = 0.0f;
	for (int i = 0; i < kSamples; ++i) {
		const FlagshipFields f = flagshipFieldsAt(static_cast<double>(i) * 0.0777);
		qMax = std::max(qMax, f.quantize);
		sMax = std::max(sMax, f.sync);
		kMax = std::max(kMax, f.feedback);
	}
	CHECK(qMax > 0.9f);
	CHECK(sMax > 0.9f);
	CHECK(kMax > 0.9f);
}

TEST_CASE("the identity corner is Morph to the bit, and a forced depth breaks it") {
	// A place where all three fields are exactly zero — a real region (~4% of the space), not a
	// contrived corner: the quantize duty deliberately leaves pure morph real territory.
	const float offset = findOffset(300, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize == 0.0f && a.fields.sync == 0.0f && a.fields.feedback == 0.0f;
	});
	const Character c = oneSound(300, offset, 0.0, 0.0, 0.0);

	FlagshipRecipe r = buildFlagshipFor(c);
	CHECK_FALSE(r.operated);
	CHECK(sameBits(r.morph, buildMorphFor(c)));

	// The one decode, on the untouched path: the same function morph renders through — THROUGH THE
	// SAME NORMALISATION every other position gets, which is the whole of what changed here. The
	// corner is still the morph path and this still fails if it stops being one; what it no longer
	// claims is that the corner is the one place a DC offset survives.
	for (int i = 0; i < 512; ++i) {
		const double phase = static_cast<double>(i) / 512.0;
		REQUIRE(evalFlagship(r, phase)
		        == flagshipNormalised(r, evalMorphTable(r.morph, phase)));
	}
	// And the bake leaves the flag down, so a voice never reads a grid here.
	FlagshipTable table {};
	bakeFlagshipTable(r, table);
	CHECK_FALSE(table.operated);

	// THE KNOWN-BAD the plan requires: force q > 0 and the identity MUST fail. If this half ever
	// passes with the equality above intact, the operators are not reaching the evaluation.
	r.quantize = 0.5f;
	prepareFlagshipRecipe(r);
	CHECK(r.operated);
	float maxDiff = 0.0f;
	for (int i = 0; i < 512; ++i) {
		const double phase = static_cast<double>(i) / 512.0;
		maxDiff = std::max(maxDiff, std::abs(evalFlagship(r, phase) - evalMorphTable(r.morph, phase)));
	}
	CHECK(maxDiff > 0.01f);
}

TEST_CASE("position purity: the bank route and the sliced route agree to the bit") {
	// An operated position with ALL THREE fields up (~4% of the space), at a nonzero travel and a
	// nonzero gamma base, so every input the two routes could disagree on is in play.
	const double gammaBase = 3.25;
	const double tx = 0.3;
	const double ty = -0.2;
	const float offset = findOffset(400, gammaBase, tx, ty, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.05f && a.fields.sync > 0.05f && a.fields.feedback > 0.05f;
	});

	const Character c = oneSound(400, offset, gammaBase, tx, ty);
	const FlagshipRecipe viaBank = buildFlagshipFor(c);

	const double home = sliceHome(400, offset, gammaBase);
	const FlagshipRecipe viaSlice =
	    buildFlagshipSliced(400, offset, tx, ty, sliceFrameFor<kMorphTriangles>(0.0, home, {}),
	                        sliceFrameFor<kStairTriangles>(0.0, home, {}), gammaBase);

	REQUIRE(viaBank.operated);
	REQUIRE(viaSlice.operated);
	// The operator half, to the bit. (The morph halves agree in every SEGMENT — checked through
	// the eval below — but not in the excite metadata: a blend-of-one consumes those factors where
	// the sliced build carries them, which is morph's pre-existing shape, not the operators'.)
	CHECK(viaBank.quantize == viaSlice.quantize);
	CHECK(viaBank.syncRatio == viaSlice.syncRatio);
	CHECK(viaBank.feedback == viaSlice.feedback);
	CHECK(viaBank.stepCount == viaSlice.stepCount);
	CHECK(viaBank.riserSlope == viaSlice.riserSlope);
	REQUIRE(viaBank.steps == viaSlice.steps);
	for (int i = 0; i < viaBank.steps; ++i) {
		CHECK(viaBank.held[static_cast<size_t>(i)] == viaSlice.held[static_cast<size_t>(i)]);
	}
	// The sound itself, to the bit, at phases that share no structure with the slot grid.
	for (int i = 0; i < 1013; ++i) {
		const double phase = static_cast<double>(i) / 1013.0;
		REQUIRE(evalFlagship(viaBank, phase) == evalFlagship(viaSlice, phase));
	}
}

TEST_CASE("q = 1 is piecewise-flat within the risers, and q = 0.5 is not") {
	// A position whose step count is comfortably inside range and whose cycle actually varies —
	// any operated position does; sync and feedback are forced off so the flat claim is about
	// quantize alone.
	const float offset = findOffset(520, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.5f && a.stepCount > 4.0f && a.stepCount < 14.0f;
	});
	const Character c = oneSound(520, offset, 0.0, 0.0, 0.0);
	FlagshipRecipe r = buildFlagshipFor(c);
	r.quantize = 1.0f;
	r.syncRatio = 1.0f;
	r.feedback = 0.0f;
	prepareFlagshipRecipe(r);
	REQUIRE(r.steps >= 2);

	// Outside the risers every sample is the held value — flat to the last addition's rounding.
	// Inside a riser the value is pinned between the two held values it interpolates.
	const double sc = static_cast<double>(r.stepCount);
	double flatSpread = 0.0;
	for (int i = 0; i < 4096; ++i) {
		const double phase = static_cast<double>(i) / 4096.0;
		int step = static_cast<int>(phase * sc);
		if (step >= r.steps) {
			step = r.steps - 1;
		}
		const double u = phase * sc - static_cast<double>(step);
		const float value = evalFlagship(r, phase);
		// THE HELD VALUES THROUGH THE SAME NORMALISATION, because evalFlagship's output now carries a
		// gain and an offset that `held[]` does not. The claim is unchanged — the staircase is flat
		// where it should be flat and monotone across a riser — and comparing a normalised number
		// against a raw one would have been the case quietly measuring the normaliser instead.
		if (u >= static_cast<double>(r.riserSlope)) {
			flatSpread = std::max(
			    flatSpread,
			    std::abs(static_cast<double>(
			        value - flagshipNormalised(r, r.held[static_cast<size_t>(step)]))));
		}
		else {
			const float a =
			    flagshipNormalised(r, r.held[static_cast<size_t>((step + r.steps - 1) % r.steps)]);
			const float b = flagshipNormalised(r, r.held[static_cast<size_t>(step)]);
			CHECK(value >= std::min(a, b) - 1e-6f);
			CHECK(value <= std::max(a, b) + 1e-6f);
		}
	}
	CHECK(flatSpread < 1e-6);

	// THE KNOWN-BAD: at q = 0.5 the same measurement is nowhere near flat — the cycle shows
	// through the blend. A quantizer that snapped to the staircase at every depth would pass the
	// endpoint and fail this.
	r.quantize = 0.5f;
	prepareFlagshipRecipe(r);
	double halfSpread = 0.0;
	for (int i = 0; i < 4096; ++i) {
		const double phase = static_cast<double>(i) / 4096.0;
		int step = static_cast<int>(phase * sc);
		if (step >= r.steps) {
			step = r.steps - 1;
		}
		const double u = phase * sc - static_cast<double>(step);
		if (u >= static_cast<double>(r.riserSlope)) {
			halfSpread = std::max(halfSpread, std::abs(static_cast<double>(
			                                      evalFlagship(r, phase)
			                                      - r.held[static_cast<size_t>(step)])));
		}
	}
	CHECK(halfSpread > 1e-3);
}

TEST_CASE("author values blend by weight before any rounding") {
	// Two sounds far apart, even weights. The blended step count must be the weighted average of
	// the two authored counts — computed here with the builder's own accumulation order so the
	// claim is exact — and the integer step count must be the ceiling of THAT, never a blend of
	// per-sound ceilings.
	Character c = oneSound(100, 0.5f, 0.0, 0.0, 0.0);
	c.count = 2;
	c.sounds[1] = c.sounds[0];
	c.sounds[1].zone = 900;
	c.sounds[1].phaseOffset = 7.3f;
	c.sounds[0].weight = 0.5;
	c.sounds[1].weight = 0.5;

	auto authorsOf = [](const SoundCoord& s) {
		const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
		return flagshipAuthorsAt(sliceFrameFor<kMorphTriangles>(0.0, home, {}),
		                         sliceFrameFor<kStairTriangles>(0.0, home, {}), home, s.travelX,
		                         s.travelY);
	};
	const FlagshipAuthors a = authorsOf(c.sounds[0]);
	const FlagshipAuthors b = authorsOf(c.sounds[1]);

	const FlagshipRecipe r = buildFlagshipFor(c);
	const float expectCount = 0.5f * a.stepCount + 0.5f * b.stepCount;
	CHECK(r.stepCount == expectCount);
	CHECK(r.quantize == 0.5f * a.fields.quantize + 0.5f * b.fields.quantize);
	CHECK(r.riserSlope == 0.5f * a.riserSlope + 0.5f * b.riserSlope);
	// REQUIRED, not if-guarded (review-flagship-s2, NOTE 4): a field relayout that zeroed the
	// blended quantize at this pair would have let the rounding half pass vacuously while looking
	// covered. If this REQUIRE reds after a deliberate field retune, move the fixture to a pair of
	// positions the new fields quantize — do not put the if back.
	REQUIRE(r.operated);
	REQUIRE(r.quantize > 0.0f);
	CHECK(r.steps == static_cast<int>(std::ceil(static_cast<double>(expectCount))));
}

TEST_CASE("a turn at a drift leaves the operators where they stood — the residue protects the tier") {
	// V7's headline property, pinned directly rather than inherited (review-flagship-s2, LOW 2):
	// the operator fields read the morph frame's LEAD coordinate and the quantize authors read the
	// stair frame, both residue-protected — so rotating a DRIFTED sound with the fold banked must
	// leave every author exactly where it stood, the same guarantee the character itself carries
	// (A26). Deterministic fixture, exact comparison: one fold's rounding sits some nine orders
	// below what a float author can resolve, and a deterministic case that passes once passes
	// forever.
	const uint16_t zone = 512;
	const double gammaBase = 2.5;
	const double tx = 0.28;
	const double ty = -0.19;
	// A position whose quantize is up and away from its extremes, so the known-bad below has a
	// live triangle to move.
	const float offset = findOffset(zone, gammaBase, tx, ty, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.1f && a.fields.quantize < 0.7f && a.fields.sync > 0.05f;
	});
	const double home = sliceHome(zone, offset, gammaBase);
	const double turn = 1.31;

	const FlagshipAuthors before =
	    flagshipAuthorsAt(sliceFrameFor<kMorphTriangles>(0.0, home, {}),
	                      sliceFrameFor<kStairTriangles>(0.0, home, {}), home, tx, ty);

	// The turn, with its residue folded — both engines' banks, exactly as the rotate verb banks
	// them, at the drift the sound is standing at.
	const SliceResidue<kMorphTriangles> foldedMorph =
	    foldResidue<kMorphTriangles>({}, 0.0, turn, tx, ty, home);
	const SliceResidue<kStairTriangles> foldedStair =
	    foldResidue<kStairTriangles>({}, 0.0, turn, tx, ty, home);
	const FlagshipAuthors after =
	    flagshipAuthorsAt(sliceFrameFor<kMorphTriangles>(turn, home, foldedMorph),
	                      sliceFrameFor<kStairTriangles>(turn, home, foldedStair), home, tx, ty);

	CHECK(after.fields.quantize == before.fields.quantize);
	CHECK(after.fields.sync == before.fields.sync);
	CHECK(after.fields.feedback == before.fields.feedback);
	CHECK(after.stepCount == before.stepCount);
	CHECK(after.riserSlope == before.riserSlope);

	// KNOWN-BAD: the same turn WITHOUT the fold — the A10 shape — moves the operators, which is
	// what shows the equalities above are protection rather than indifference.
	const FlagshipAuthors unfolded =
	    flagshipAuthorsAt(sliceFrameFor<kMorphTriangles>(turn, home, {}),
	                      sliceFrameFor<kStairTriangles>(turn, home, {}), home, tx, ty);
	CHECK((unfolded.fields.quantize != before.fields.quantize
	       || unfolded.fields.sync != before.fields.sync
	       || unfolded.stepCount != before.stepCount));
}

TEST_CASE("determinism: the same character bakes the same table, to the bit") {
	const float offset = findOffset(700, 0.0, 0.15, 0.4, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.05f && a.fields.feedback > 0.05f;
	});
	const Character c = oneSound(700, offset, 0.0, 0.15, 0.4);

	FlagshipTable first {};
	FlagshipTable second {};
	bakeFlagshipTable(buildFlagshipFor(c), first);
	bakeFlagshipTable(buildFlagshipFor(c), second);
	REQUIRE(first.operated);
	// MEMBER-WISE, not memcmp. The line this replaces said "both value-initialised, so the memcmp
	// covers padding as zero on both sides" — aggregate initialisation does not promise that, and
	// the three bytes after `operated` are exactly what Apple clang/arm64 filled differently in the
	// same shape (`AutomodDerived`, the one red of the first green macOS build, 2026-09-07). This
	// suite is platform-pinned and never ran there; the accident was the same accident.
	CHECK(first == second);
}

TEST_CASE("the baked grid is the eval at the slot phases, guards and mips in place") {
	const float offset = findOffset(640, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.3f && a.fields.sync > 0.3f;
	});
	const FlagshipRecipe r = buildFlagshipFor(oneSound(640, offset, 0.0, 0.0, 0.0));
	FlagshipTable table {};
	bakeFlagshipTable(r, table);
	REQUIRE(table.operated);

	for (int j = 0; j < kFlagshipSlots; ++j) {
		REQUIRE(table.slots[j]
		        == evalFlagship(r, static_cast<double>(j) / static_cast<double>(kFlagshipSlots)));
	}
	CHECK(table.slots[kFlagshipSlots] == table.slots[0]);
	CHECK(table.mip1[kFlagshipSlots] == table.mip1[0]);
	CHECK(table.mip2[kFlagshipSlots] == table.mip2[0]);

	// The mip ladder only ever dulls: total absolute slot-to-slot variation must not grow.
	auto variation = [](const float* slots) {
		double sum = 0.0;
		for (int j = 0; j < kFlagshipSlots; ++j) {
			sum += std::abs(static_cast<double>(slots[j + 1] - slots[j]));
		}
		return sum;
	};
	CHECK(variation(table.mip1) <= variation(table.slots));
	CHECK(variation(table.mip2) <= variation(table.mip1));

	// The mip selector hands out exactly the three grids, clamped at the ends.
	CHECK(flagshipMipSlots(table, 0) == table.slots);
	CHECK(flagshipMipSlots(table, 1) == table.mip1);
	CHECK(flagshipMipSlots(table, 2) == table.mip2);
	CHECK(flagshipMipSlots(table, -1) == table.slots);
	CHECK(flagshipMipSlots(table, 7) == table.mip2);

	// And the grid eval interpolates this grid exactly at slot phases.
	for (int j = 0; j < kFlagshipSlots; ++j) {
		REQUIRE(evalFlagshipGrid(table.slots,
		                         static_cast<double>(j) / static_cast<double>(kFlagshipSlots))
		        == table.slots[j]);
	}
}

TEST_CASE("the three-rung ladder is the three single-mip bakes, to the bit — and one third the work") {
	// THE SAVING THAT MUST NOT COST AN ANSWER. A voice needs all three rungs, because a glide can
	// cross a mip boundary after the bake; a first draft got them by calling the single-mip bake
	// three times, which rebuilds the base from scratch each time and runs the operator chain 768
	// times for an answer that needs 256. On a sixteen-voice drag through vowel country that was
	// most of the cost — 41.8% of a core, down to 17.0% once the base was evaluated once.
	//
	// This is what says the cheaper route is the SAME route: identical grids, not merely similar
	// ones, so the choice between them is never a choice about sound.
	const float operatedOffset = findOffset(640, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.3f && a.fields.sync > 0.3f;
	});
	const float cornerOffset = findOffset(300, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize == 0.0f && a.fields.sync == 0.0f && a.fields.feedback == 0.0f;
	});
	for (const auto& spot : { std::pair<uint16_t, float> { 640, operatedOffset },
		                        std::pair<uint16_t, float> { 300, cornerOffset } }) {
		const FlagshipRecipe r = buildFlagshipFor(oneSound(spot.first, spot.second, 0.0, 0.0, 0.0));
		std::array<float, kFlagshipSlots + 1> base {};
		std::array<float, kFlagshipSlots + 1> mip1 {};
		std::array<float, kFlagshipSlots + 1> mip2 {};
		bakeFlagshipLadder(r, {}, base.data(), mip1.data(), mip2.data());

		std::array<float, kFlagshipSlots + 1> one {};
		for (int level = 0; level <= 2; ++level) {
			bakeFlagshipGridAtMip(r, {}, level, one.data());
			const std::array<float, kFlagshipSlots + 1>& want =
			    level == 0 ? base : (level == 1 ? mip1 : mip2);
			CAPTURE(level);
			REQUIRE(one == want);
		}
		// And against the shared three-level bake as well, so all three routes agree rather than
		// two of them agreeing with each other.
		FlagshipTable full {};
		bakeFlagshipTable(r, full);
		if (full.operated) {
			for (int j = 0; j <= kFlagshipSlots; ++j) {
				REQUIRE(base[static_cast<size_t>(j)] == full.slots[j]);
				REQUIRE(mip1[static_cast<size_t>(j)] == full.mip1[j]);
				REQUIRE(mip2[static_cast<size_t>(j)] == full.mip2[j]);
			}
		}
	}
}

TEST_CASE("LZ: the fused prepare-and-bake is prepare then the ladder, to the bit — recipe and grids, warped or not, with or without a vowel") {
	// THE VOWEL BAKE'S ROAD (backlog LZ). `prepareAndBakeFlagshipLadder` measures the normalisation off the 256 raw slots
	// instead of evaluating the cycle again at 128 (or 256) probe phases. Restated against the pair it replaced, on
	// recipes the terrain authored and on recipes forced into every operator and every warp mode, so the warped probe
	// (256, every slot) and the unwarped one (128, every other slot) are both held.
	VoxCycle vowel;
	vowel.voiced = true;
	for (int j = 0; j <= kVoxSlots; ++j) {
		const double p = static_cast<double>(j % kVoxSlots) / static_cast<double>(kVoxSlots);
		vowel.slots[static_cast<size_t>(j)] =
		    static_cast<float>(0.6 * std::sin(6.283185307179586 * p) + 0.3 * std::sin(6.283185307179586 * 7.0 * p));
	}
	int unoperated = 0;
	int unwarped = 0;
	int warped = 0;
	int compared = 0;
	const auto check = [&](const FlagshipRecipe& start, const FlagshipSource& source) {
		FlagshipRecipe pair = start;
		prepareFlagshipRecipe(pair, source);
		std::array<float, kFlagshipSlots + 1> base {};
		std::array<float, kFlagshipSlots + 1> mip1 {};
		std::array<float, kFlagshipSlots + 1> mip2 {};
		bakeFlagshipLadder(pair, source, base.data(), mip1.data(), mip2.data());

		FlagshipRecipe fused = start;
		std::array<float, kFlagshipSlots + 1> fusedBase {};
		std::array<float, kFlagshipSlots + 1> fusedMip1 {};
		std::array<float, kFlagshipSlots + 1> fusedMip2 {};
		prepareAndBakeFlagshipLadder(fused, source, fusedBase.data(), fusedMip1.data(), fusedMip2.data());

		// BITS, not values: memcmp sees a sign of zero and a NaN's payload that == would not.
		REQUIRE(std::memcmp(base.data(), fusedBase.data(), sizeof base) == 0);
		REQUIRE(std::memcmp(mip1.data(), fusedMip1.data(), sizeof mip1) == 0);
		REQUIRE(std::memcmp(mip2.data(), fusedMip2.data(), sizeof mip2) == 0);
		// And the recipe the bake leaves: the normalisation it measured, and what the shape half derived.
		REQUIRE(std::memcmp(&pair.dcOffset, &fused.dcOffset, sizeof pair.dcOffset) == 0);
		REQUIRE(std::memcmp(&pair.gain, &fused.gain, sizeof pair.gain) == 0);
		REQUIRE(pair.operated == fused.operated);
		REQUIRE(pair.steps == fused.steps);
		REQUIRE(std::memcmp(pair.held.data(), fused.held.data(), sizeof pair.held) == 0);
		++compared;
		if (!pair.operated) {
			++unoperated;
		}
		else if (pair.warpMode != WarpMode::Off && warpAmount(pair.warpMode, pair.warpDepth) != 0.0f) {
			++warped;
		}
		else {
			++unwarped;
		}
	};
	for (int zone = 7; zone < 1000; zone += 61) {
		for (const float offset : { 0.0f, 0.37f, 1.3563f }) {
			CAPTURE(zone);
			CAPTURE(offset);
			const FlagshipRecipe authored = buildFlagshipFor(oneSound(static_cast<uint16_t>(zone), offset, 0.0, 0.1, -0.2));
			for (const float presence : { 0.0f, 0.45f }) {
				const FlagshipSource source = presence > 0.0f ? FlagshipSource { &vowel, presence } : FlagshipSource {};
				check(authored, source);
				// The identity corner, forced: the constellations above are all operated somewhere.
				FlagshipRecipe plain = authored;
				plain.quantize = 0.0f;
				plain.syncRatio = 1.0f;
				plain.feedback = 0.0f;
				plain.smoothness = 0.0f;
				plain.warpMode = WarpMode::Off;
				check(plain, source);
				FlagshipRecipe forced = authored;
				forced.quantize = 0.4f;
				forced.stepCount = 5.5f;
				forced.riserSlope = 0.2f;
				forced.syncRatio = 1.7f;
				forced.feedback = 0.6f;
				forced.smoothness = 0.3f;
				check(forced, source);
				for (int mode = 1; mode < static_cast<int>(WarpMode::kCount); ++mode) {
					FlagshipRecipe bent = forced;
					bent.warpMode = static_cast<WarpMode>(mode);
					bent.warpDepth = 0.8f;
					CAPTURE(mode);
					check(bent, source);
				}
			}
		}
	}
	MESSAGE("compared " << compared << ": " << unoperated << " unoperated, " << unwarped << " unwarped, " << warped
	                    << " warped");
	// NOT VACUOUS: all three roads through the probe were taken.
	CHECK(unoperated > 0);
	CHECK(unwarped > 0);
	CHECK(warped > 0);
}

TEST_CASE("baking ONE mip gives the same grid the three-level bake gives, to the bit") {
	// The saving a voice that must bake for itself lives on: it knows its own note, so it knows its
	// own level, and two of the bank's three grids would be work nobody reads. The claim that makes
	// that safe is bit-identity with the shared bake — anything less and a voice on the vox road
	// would render a quietly different waveform from the same recipe, which is exactly the kind of
	// difference that gets blamed on the vowel.
	//
	// Positions on BOTH sides of decision V8's mechanism switch, because the single-mip bake evaluates
	// through evalFlagship and an unoperated recipe takes a different branch of it.
	const float operatedOffset = findOffset(640, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.3f && a.fields.sync > 0.3f;
	});
	const float cornerOffset = findOffset(300, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize == 0.0f && a.fields.sync == 0.0f && a.fields.feedback == 0.0f;
	});

	for (const auto& spot : { std::pair<uint16_t, float> { 640, operatedOffset },
		                        std::pair<uint16_t, float> { 300, cornerOffset } }) {
		const FlagshipRecipe r = buildFlagshipFor(oneSound(spot.first, spot.second, 0.0, 0.0, 0.0));
		FlagshipTable full {};
		bakeFlagshipTable(r, full);
		for (int level = 0; level <= 2; ++level) {
			std::array<float, kFlagshipSlots + 1> one {};
			bakeFlagshipGridAtMip(r, {}, level, one.data());
			// The reference is the three-level bake's own grid for an OPERATED recipe. An identity
			// corner never fills those grids at all — the flag is what gates the read — so the
			// reference there is the eval itself, which is what the voice would be playing.
			if (full.operated) {
				const float* want = flagshipMipSlots(full, level);
				for (int j = 0; j <= kFlagshipSlots; ++j) {
					REQUIRE(one[static_cast<size_t>(j)] == want[j]);
				}
			}
			else if (level == 0) {
				for (int j = 0; j < kFlagshipSlots; ++j) {
					REQUIRE(one[static_cast<size_t>(j)]
					        == evalFlagship(r, static_cast<double>(j)
					                               / static_cast<double>(kFlagshipSlots)));
				}
			}
			CHECK(one[kFlagshipSlots] == one[0]);
		}
		// Levels outside the ladder clamp, as flagshipMipSlots does — a guard rather than a
		// behaviour, since stairMipForFrequency never asks.
		std::array<float, kFlagshipSlots + 1> low {};
		std::array<float, kFlagshipSlots + 1> high {};
		std::array<float, kFlagshipSlots + 1> base {};
		std::array<float, kFlagshipSlots + 1> dullest {};
		bakeFlagshipGridAtMip(r, {}, -3, low.data());
		bakeFlagshipGridAtMip(r, {}, 9, high.data());
		bakeFlagshipGridAtMip(r, {}, 0, base.data());
		bakeFlagshipGridAtMip(r, {}, 2, dullest.data());
		CHECK(low == base);
		CHECK(high == dullest);
	}
}

TEST_CASE("the operators reach the voice, and a builder that skips them is heard not to") {
	// The whole chain: parameters' Character in, audio out. The flagship flag is the ONLY
	// difference between the two characters below, so any difference in the rendered note is the
	// operator tier and nothing else — and the KNOWN-BAD is the same comparison with the flag down
	// on both sides, which is exactly "a builder that skips operators" and must render identically.
	const float offset = findOffset(640, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.3f && a.fields.sync > 0.3f;
	});
	Character flagged = oneSound(640, offset, 0.0, 0.0, 0.0);
	flagged.flagship = true;
	Character skipped = flagged;
	skipped.flagship = false;

	SUBCASE("backlog EH: a plain morph build in a slot that last held a flagship carries NO recipe — FM does not leak across the slot reuse") {
		// The four FM accessors gate on the ENGINE alone, and a plain morph build reset the operated flag
		// and the vox in the reused slot but not the recipe — so the previous flagship's FM depth, ratio
		// and width rendered on a sound that declares none. Two slots flip, so the third build lands in
		// the first's slot.
		Character fm = flagged;
		fm.sounds[0].fmDepth = 0.6f;
		CharacterBank bank;
		bank.setSampleRate(48000.0);
		bank.setCharacter(fm);
		REQUIRE(bank.current().fmDepth() > 0.0f);
		// THE FADE GUARD IS LET RUN OUT BETWEEN BUILDS (`bank.advance`), or a request inside it is only
		// PENDING and the build below would never happen — which is how a first draft of this subcase
		// passed with the recipe still leaking.
		bank.advance(48000);
		bank.setCharacter(skipped); // the other slot
		bank.advance(48000);
		CHECK(bank.current().fmDepth() == 0.0f);
		// A DIFFERENT plain character, so this is a real build and not a dedup of the last — it lands in
		// the slot the flagship left.
		Character skippedAgain = skipped;
		skippedAgain.sounds[0].phaseOffset += 0.01f;
		bank.setCharacter(skippedAgain);
		bank.advance(48000);
		REQUIRE(bank.current().buildSerial >= 3);
		REQUIRE(bank.current().engine == OscEngine::Morph);
		REQUIRE_FALSE(bank.current().flagship.operated);
		CHECK(bank.current().fmDepth() == 0.0f);
		CHECK(bank.current().fmStereoWidth() == 0.0f);
	}

	constexpr double kRate = 48000.0;
	constexpr int kSamples = 2048;
	auto renderOf = [&](const Character& c) {
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(c);
		OscVoice voice;
		voice.setCharacterBank(&bank);
		voice.setSampleRate(kRate);
		voice.setFrequency(220.0);
		std::array<float, kSamples> out {};
		voice.render(out.data(), kSamples);
		return out;
	};

	// The table the voice reads carries the operator: the bank's live flagship grid is up, and it
	// is the recipe's own bake to the bit.
	{
		CharacterBank bank;
		bank.setSampleRate(kRate);
		bank.setCharacter(flagged);
		REQUIRE(bank.current().flagship.operated);
		FlagshipTable expected {};
		bakeFlagshipTable(buildFlagshipFor(flagged), expected);
		CHECK(bank.current().flagship == expected); // member-wise; see FlagshipTable::operator==
		// And the identity anchor's other half: the morph table underneath is the plain build.
		CHECK(sameBits(bank.current().morph, buildMorphFor(flagged)));
	}

	const auto operated = renderOf(flagged);
	const auto plain = renderOf(skipped);
	float maxDiff = 0.0f;
	for (int i = 0; i < kSamples; ++i) {
		maxDiff = std::max(maxDiff, std::abs(operated[static_cast<size_t>(i)]
		                                     - plain[static_cast<size_t>(i)]));
	}
	CHECK(maxDiff > 0.05f);

	// THE KNOWN-BAD: skip the operators on both sides and the difference vanishes — bit for bit,
	// not approximately, which is what pins the difference above ON the operator tier.
	const auto plainAgain = renderOf(skipped);
	CHECK(std::memcmp(plain.data(), plainAgain.data(), sizeof(plain)) == 0);

	// And at an identity-corner position the flag changes NOTHING: V5 there IS morph, to the bit,
	// through the whole voice path — oversampling, decimation, modifiers and all.
	//
	// THE CORNER NOW HAS TWO HALVES, and this case is where that arrived. Until vox could be heard,
	// three field zeros were the whole of it. They are not any more: V22c blends the vowel into the
	// cycle BEFORE the operator chain, so a place with no operators but a throat in it plays the
	// blend rather than the plain morph table — correctly, and by design. The first position
	// this scan used to land on is one of them (offset 2.0824, presence 0.56 MEASURED), which is why
	// the predicate now asks for both. The vowel-free half is not a corner either: the species duty
	// is 0.5, so roughly half the operator-free country is also throatless.
	//
	// AND NOW A THIRD, by exactly the same mechanism and for exactly the same reason: the smooth
	// source replaces what the chain READS, so a place with no operators and no throat but some
	// smoothness in it plays a cubic through renormalised control points rather than the segment
	// rule. The corner is "all four zero" now. This is the second time this predicate has grown, and
	// the growth is the feature landing rather than the test being loosened — the case below, which
	// demands that V5 is NOT morph in vowel country, is what keeps that distinction honest, and the
	// smooth road has its own such case beside it.
	const float idOffset = findOffset(300, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize == 0.0f && a.fields.sync == 0.0f && a.fields.feedback == 0.0f
		       && a.fields.smoothness == 0.0f && voxPresenceAt(a.lead) == 0.0f;
	});
	Character idFlagged = oneSound(300, idOffset, 0.0, 0.0, 0.0);
	idFlagged.flagship = true;
	Character idPlain = idFlagged;
	idPlain.flagship = false;
	const auto idA = renderOf(idFlagged);
	const auto idB = renderOf(idPlain);
	CHECK(std::memcmp(idA.data(), idB.data(), sizeof(idA)) == 0);

	// THE KNOWN-BAD FOR THE OTHER HALF: the same three field zeros WITH a vowel, where V5 must NOT
	// be morph. Without this the widened predicate above would look like a test being quietly
	// loosened until it passed, and it is the opposite — the corner really did grow a second
	// condition, and here is the country that proves the condition is load-bearing.
	const float vowelOffset = findOffset(300, 0.0, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.quantize == 0.0f && a.fields.sync == 0.0f && a.fields.feedback == 0.0f
		       && voxPresenceAt(a.lead) > 0.2f;
	});
	Character vowelFlagged = oneSound(300, vowelOffset, 0.0, 0.0, 0.0);
	vowelFlagged.flagship = true;
	Character vowelPlain = vowelFlagged;
	vowelPlain.flagship = false;
	REQUIRE(buildVoxFor(vowelFlagged).audible);
	CHECK_FALSE(buildFlagshipFor(vowelFlagged).operated);
	const auto vowelA = renderOf(vowelFlagged);
	const auto vowelB = renderOf(vowelPlain);
	CHECK(std::memcmp(vowelA.data(), vowelB.data(), sizeof(vowelA)) != 0);
}

TEST_CASE("MEASURE: per-build cost beside the 0.40 µs morph baseline") {
	// Reported, and bounded only against pathology: the budget is "within reason above v1", and
	// the numbers below are the reason made visible. Rebuilds happen per character change — at
	// worst once per block under automation — never per sample.
	using clock = std::chrono::steady_clock;
	constexpr int kRounds = 400;

	const Character plain = oneSound(300, findOffset(300, 0.0, 0.0, 0.0,
	                                                 [](const FlagshipAuthors& a) {
		                                                 return a.fields.quantize == 0.0f
		                                                        && a.fields.sync == 0.0f
		                                                        && a.fields.feedback == 0.0f;
	                                                 }),
	                                 0.0, 0.0, 0.0);
	const Character operated = oneSound(640, findOffset(640, 0.0, 0.0, 0.0,
	                                                    [](const FlagshipAuthors& a) {
		                                                    return a.fields.quantize > 0.3f
		                                                           && a.fields.sync > 0.3f
		                                                           && a.fields.feedback == 0.0f;
	                                                    }),
	                                    0.0, 0.0, 0.0);
	const Character feedback = oneSound(700, findOffset(700, 0.0, 0.15, 0.4,
	                                                    [](const FlagshipAuthors& a) {
		                                                    return a.fields.quantize > 0.05f
		                                                           && a.fields.feedback > 0.05f;
	                                                    }),
	                                    0.0, 0.15, 0.4);

	volatile float sink = 0.0f;
	auto time = [&](auto&& body) {
		const auto start = clock::now();
		for (int i = 0; i < kRounds; ++i) {
			sink = sink + body();
		}
		const auto end = clock::now();
		return std::chrono::duration<double, std::micro>(end - start).count()
		       / static_cast<double>(kRounds);
	};

	const double morphOnly = time([&] { return buildMorphFor(plain).gain; });
	const double identity = time([&] {
		FlagshipTable t {};
		bakeFlagshipTable(buildFlagshipFor(plain), t);
		return t.slots[0];
	});
	const double baked = time([&] {
		FlagshipTable t {};
		bakeFlagshipTable(buildFlagshipFor(operated), t);
		return t.slots[0];
	});
	const double bakedFeedback = time([&] {
		FlagshipTable t {};
		bakeFlagshipTable(buildFlagshipFor(feedback), t);
		return t.slots[0];
	});

	std::printf("[flagship cost] morph build %.3f us | V5 identity %.3f us | V5 operated (q+s) "
	            "%.3f us | V5 operated (+feedback) %.3f us\n",
	            morphOnly, identity, baked, bakedFeedback);

	// Generous ceilings: a regression that blows these is a bug, not a tuning question.
	CHECK(identity < 50.0);
	CHECK(baked < 200.0);
	CHECK(bakedFeedback < 400.0);
}

TEST_CASE("the smoothness knob can ASK for smoothness, not only permit it") {
	// THE KNOB WAS FOUND DEAD, so the decision: above 50% the knob biases the field up, so some
	// smoothness is available even where the field says 0.
	//
	// IT WAS A MULTIPLIER, AND A MULTIPLIER CANNOT DO THAT. MEASURED before the change: over the first
	// eight offset steps of home the flagship smoothness field is alive over exactly ONE interval —
	// 4.6501 to 6.2002 — and is exactly 0.000000 at the default coordinate. So on the ground a new
	// patch starts on, and on four fifths of the ground anywhere, the knob was multiplying zero. It
	// worked hard where the field was alive (terrain centroid moved by up to 0.31 between scale 1 and
	// 0, on all 401 positions sampled) and did nothing at all everywhere else, which reads as broken
	// rather than as out of range.
	//
	// THE SHAPE: halfway is neutral, the bottom half attenuates as before, the top half lifts toward
	// full smoothness whatever the field says. Asserted as VALUES at the three points that define it
	// and for the property that motivated it.
	using feathers::fieldWithOverride;

	// Neutral in the middle: the field, untouched. This is what makes 0.5 the honest default and a
	// patch saved before the knob existed sound the same.
	for (float field : {0.0f, 0.13f, 0.5f, 0.87f, 1.0f}) {
		CAPTURE(field);
		CHECK(fieldWithOverride(field, 0.5f) == doctest::Approx(field));
	}

	// The bottom half is the old multiplier, on a doubled travel: zero defeats smoothing entirely.
	CHECK(fieldWithOverride(0.8f, 0.0f) == doctest::Approx(0.0f));
	CHECK(fieldWithOverride(0.8f, 0.25f) == doctest::Approx(0.4f));

	// THE POINT OF THE WHOLE CHANGE: above halfway, a field of zero still yields smoothness.
	CHECK(fieldWithOverride(0.0f, 0.5f) == doctest::Approx(0.0f)); // ...but not at neutral
	CHECK(fieldWithOverride(0.0f, 0.75f) == doctest::Approx(0.5f));
	CHECK(fieldWithOverride(0.0f, 1.0f) == doctest::Approx(1.0f));

	// And the top is full smoothness from anywhere, so the knob's ends mean the same thing whatever
	// ground you are standing on — which is what "a control rather than a permission" amounts to.
	for (float field : {0.0f, 0.4f, 1.0f}) {
		CAPTURE(field);
		CHECK(fieldWithOverride(field, 1.0f) == doctest::Approx(1.0f));
	}

	// MONOTONIC AND CONTINUOUS, including across the join at halfway — a control that jumped there
	// would be a click in the sound, and the join is exactly where a two-branch formula gets it wrong.
	for (float field : {0.0f, 0.3f, 1.0f}) {
		CAPTURE(field);
		float previous = -1.0f;
		for (int i = 0; i <= 1000; ++i) {
			const float knob = static_cast<float>(i) / 1000.0f;
			const float value = fieldWithOverride(field, knob);
			CHECK(value >= previous - 1e-6f);
			CHECK(std::abs(value - std::max(previous, 0.0f)) < 0.01f); // no step at the join
			previous = value;
		}
	}

	// AND IT REACHES THE RECIPE AT THE DEFAULT COORDINATE, which is the whole complaint: the field is
	// zero at home 0, so before this the recipe's smoothness was zero however the knob was set.
	const feathers::MorphBasis morph = feathers::sliceFrameFor<feathers::kMorphTriangles>(0.0, 0.0, {});
	const feathers::StairBasis stair = feathers::sliceFrameFor<feathers::kStairTriangles>(0.0, 0.0, {});
	REQUIRE(feathers::flagshipFieldsAt(0.0).smoothness == 0.0f);
	const feathers::FlagshipRecipe neutral =
	    feathers::buildFlagshipSliced(0, 0.0f, 0.0, 0.0, morph, stair, 0.0,
	                                  feathers::FieldScales { 0.5f });
	const feathers::FlagshipRecipe asked =
	    feathers::buildFlagshipSliced(0, 0.0f, 0.0, 0.0, morph, stair, 0.0,
	                                  feathers::FieldScales { 1.0f });
	CHECK(neutral.smoothness == 0.0f);
	CHECK(asked.smoothness == doctest::Approx(1.0f));
	// And the smooth source is actually ENGAGED rather than gated off by the point-count floor, which
	// is the other way this could be true on paper and silent in the air.
	CHECK(asked.smoothPoints >= feathers::kFlagshipMinSmoothPoints);
}

// =================================================================================================
// THE FIVE FIELD MODIFIER KNOBS (2026-08-25)
// =================================================================================================

TEST_CASE("each field modifier knob scales its own field, by value, and moves no other") {
	// AN ASSERTION ABOUT VALUES, NOT A DIFFERENCE CHECK. CLAUDE.md's rule and the reason for it: a
	// comparison between two recipes passes whenever ANYTHING differs, and every instance of that
	// failure in this repo so far was a PARTIAL effect surviving where a total one would have been
	// caught. The reference below is an INDEPENDENT restatement — flagshipFieldsAt at the same
	// coordinate, put through fieldWithOverride by hand — so it can disagree with the engine.
	const double gammaBase = 3.25;
	const double tx = 0.3;
	const double ty = -0.2;
	// All three operator fields comfortably alive, so a knob has something to scale in every case.
	const float offset = findOffset(400, gammaBase, tx, ty, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.05f && a.fields.sync > 0.05f && a.fields.feedback > 0.05f;
	});
	const double home = sliceHome(400, offset, gammaBase);
	const MorphBasis morphFrame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
	const StairBasis stairFrame = sliceFrameFor<kStairTriangles>(0.0, home, {});
	const FlagshipAuthors authors = flagshipAuthorsAt(morphFrame, stairFrame, home, tx, ty);

	const auto build = [&](feathers::FieldScales scales) {
		return buildFlagshipSliced(400, offset, tx, ty, morphFrame, stairFrame, gammaBase, scales);
	};

	for (const float knob : { 0.0f, 0.25f, feathers::kFieldNeutral, 0.75f, 1.0f }) {
		CAPTURE(knob);

		// QUANTIZE.
		feathers::FieldScales q {};
		q.quantize = knob;
		CHECK(build(q).quantize
		      == doctest::Approx(feathers::fieldWithOverride(authors.fields.quantize, knob)));

		// SYNC — asserted through the ratio, because that is the quantity the recipe carries and the
		// decode reads. r = 1 + (max - 1) * s, restated here rather than read off the engine.
		feathers::FieldScales sy {};
		sy.sync = knob;
		CHECK(build(sy).syncRatio
		      == doctest::Approx(1.0f
		                         + (feathers::kFlagshipSyncMaxRatio - 1.0f)
		                               * feathers::fieldWithOverride(authors.fields.sync, knob)));

		// FEEDBACK. The field is already past kFlagshipFeedbackThreshold when it arrives, so what the
		// knob scales is the DEPTH where feedback happens rather than where it happens at all.
		feathers::FieldScales fb {};
		fb.feedback = knob;
		CHECK(build(fb).feedback
		      == doctest::Approx(feathers::fieldWithOverride(authors.fields.feedback, knob)));
	}

	// AND NO KNOB MOVES A FIELD THAT IS NOT ITS OWN. This is the half a difference check cannot do:
	// the assertions above would all pass if every knob wrote every field. Each knob is taken to an
	// extreme and the OTHER two quantities are pinned to their neutral values, to the bit.
	const FlagshipRecipe neutral = build({});
	{
		feathers::FieldScales only {};
		only.quantize = 1.0f;
		const FlagshipRecipe r = build(only);
		CHECK(r.syncRatio == neutral.syncRatio);
		CHECK(r.feedback == neutral.feedback);
	}
	{
		feathers::FieldScales only {};
		only.sync = 1.0f;
		const FlagshipRecipe r = build(only);
		CHECK(r.quantize == neutral.quantize);
		CHECK(r.feedback == neutral.feedback);
	}
	{
		feathers::FieldScales only {};
		only.feedback = 1.0f;
		const FlagshipRecipe r = build(only);
		CHECK(r.quantize == neutral.quantize);
		CHECK(r.syncRatio == neutral.syncRatio);
	}

	// THE SECOND ASSERTION THAT PROVES THE FIRST IS NOT VACUOUS — the knobs must actually be capable
	// of moving these quantities, or every equality above is satisfied by a knob that does nothing.
	feathers::FieldScales off {};
	off.quantize = 0.0f;
	off.sync = 0.0f;
	off.feedback = 0.0f;
	const FlagshipRecipe defeated = build(off);
	CHECK(defeated.quantize < neutral.quantize);
	CHECK(defeated.syncRatio < neutral.syncRatio);
	CHECK(defeated.feedback < neutral.feedback);
}

TEST_CASE("neutral is the identity: every field modifier at rest leaves the field as authored") {
	// THE PROPERTY THE DEFAULTS ARE FOR. kFieldNeutral is the fixed point of fieldWithOverride, so a
	// SoundCoord nobody has touched must produce the field exactly as flagshipAuthorsAt wrote it —
	// TO THE BIT, not approximately, because "approximately the same sound" is what a default that
	// disagrees with the fixed point produces and it is the bug flagship.h records beside the
	// constant.
	const double gammaBase = 3.25;
	const double tx = 0.3;
	const double ty = -0.2;
	const float offset = findOffset(400, gammaBase, tx, ty, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.05f && a.fields.sync > 0.05f && a.fields.feedback > 0.05f;
	});
	const double home = sliceHome(400, offset, gammaBase);
	const MorphBasis morphFrame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
	const StairBasis stairFrame = sliceFrameFor<kStairTriangles>(0.0, home, {});
	const FlagshipAuthors authors = flagshipAuthorsAt(morphFrame, stairFrame, home, tx, ty);

	const FlagshipRecipe r =
	    buildFlagshipSliced(400, offset, tx, ty, morphFrame, stairFrame, gammaBase, {});
	CHECK(r.quantize == authors.fields.quantize);
	CHECK(r.feedback == authors.fields.feedback);
	CHECK(r.syncRatio
	      == 1.0f + (feathers::kFlagshipSyncMaxRatio - 1.0f) * authors.fields.sync);

	// And the SoundCoord's own defaults are that same value, so the two roads rest in one place.
	const SoundCoord fresh {};
	CHECK(fresh.quantizeScale == feathers::kFieldNeutral);
	CHECK(fresh.syncScale == feathers::kFieldNeutral);
	CHECK(fresh.feedbackScale == feathers::kFieldNeutral);
	CHECK(fresh.pointScale == feathers::kFieldNeutral);
	CHECK(fresh.gritScale == feathers::kFieldNeutral);
}

TEST_CASE("Points modifies the field and Smoothness still walks the result — the composition order") {
	// PLAN DOC §3. Two controls, one quantity, and the ONE thing that makes that safe is a
	// composition order defined in a single place. The reverse order also compiles and sounds
	// different at the extremes, so this case is what pins which one shipped.
	//
	//   K = pointCountWithOverride( fieldWithOverride(field, points), smoothness )
	//
	// Asserted at four corners of the two-knob square, each against an INDEPENDENT restatement.
	const double gammaBase = 0.0;
	// Smooth country, so the point-count field is being read for something rather than ignored.
	const float offset = findOffset(400, gammaBase, 0.0, 0.0, [](const FlagshipAuthors& a) {
		return a.fields.smoothness > 0.05f;
	});
	const double home = sliceHome(400, offset, gammaBase);
	const MorphBasis morphFrame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
	const StairBasis stairFrame = sliceFrameFor<kStairTriangles>(0.0, home, {});
	const FlagshipAuthors authors = flagshipAuthorsAt(morphFrame, stairFrame, home, 0.0, 0.0);
	const float field = authors.fields.pointCount;

	for (const float points : { 0.0f, feathers::kFieldNeutral, 1.0f }) {
		for (const float smooth : { 0.0f, feathers::kFieldNeutral, 1.0f }) {
			CAPTURE(points);
			CAPTURE(smooth);
			feathers::FieldScales scales {};
			scales.points = points;
			scales.smoothness = smooth;
			const FlagshipRecipe r =
			    buildFlagshipSliced(400, offset, 0.0, 0.0, morphFrame, stairFrame, gammaBase, scales);
			const float want = feathers::pointCountWithOverride(
			    feathers::pointFieldWithOverride(field, points), smooth);
			CHECK(r.pointCount == doctest::Approx(want));
		}
	}

	// BOTH NEUTRAL IS TODAY, TO THE BIT — the property that lets this land without retuning anything.
	feathers::FieldScales rest {};
	const FlagshipRecipe asBefore =
	    buildFlagshipSliced(400, offset, 0.0, 0.0, morphFrame, stairFrame, gammaBase, rest);
	CHECK(asBefore.pointCount == doctest::Approx(pointCountWithOverride(field, feathers::kFieldNeutral)));
	// AND THE COUNT IS STILL A COUNT — the assertion that would have caught the bug this case DID
	// catch, stated directly so the next reader does not have to infer it. Points takes a [2,8]
	// quantity and must hand back a [2,8] quantity; the first draft ran it through fieldWithOverride,
	// whose [0,1] clamp turned 7.09 into 1.0 and collapsed every smooth waveform to a sine.
	CHECK(asBefore.pointCount >= static_cast<float>(feathers::kFlagshipMinSmoothPoints));
	CHECK(asBefore.pointCount <= static_cast<float>(feathers::kFlagshipMaxSmoothPoints));
	for (const float points : { 0.0f, 0.25f, feathers::kFieldNeutral, 0.75f, 1.0f }) {
		CAPTURE(points);
		const float k = feathers::pointFieldWithOverride(field, points);
		CHECK(k >= static_cast<float>(feathers::kFlagshipMinSmoothPoints));
		CHECK(k <= static_cast<float>(feathers::kFlagshipMaxSmoothPoints));
	}
	// The ends mean what the label says: knob 0 keeps two points (the sine end), knob 1 keeps all
	// eight the morph table authored.
	CHECK(feathers::pointFieldWithOverride(field, 0.0f)
	      == doctest::Approx(static_cast<float>(feathers::kFlagshipMinSmoothPoints)));
	CHECK(feathers::pointFieldWithOverride(field, 1.0f)
	      == doctest::Approx(static_cast<float>(feathers::kFlagshipMaxSmoothPoints)));

	// AND THE TWO KNOBS ARE GENUINELY SEPARABLE, which is the claim that makes a second control worth
	// its parameter ids at all: with Smoothness pinned, Points alone must move K.
	feathers::FieldScales low {};
	low.points = 0.0f;
	feathers::FieldScales high {};
	high.points = 1.0f;
	const float kLow =
	    buildFlagshipSliced(400, offset, 0.0, 0.0, morphFrame, stairFrame, gammaBase, low).pointCount;
	const float kHigh =
	    buildFlagshipSliced(400, offset, 0.0, 0.0, morphFrame, stairFrame, gammaBase, high).pointCount;
	CHECK(kLow != kHigh);
}

TEST_CASE("Grit scales the morph table's amp noise, and reaches plain Morph rather than only V5") {
	// THE ODD ONE OF THE FIVE. ampNoise is a MORPH TABLE member built in slice_build.cpp, not a
	// FlagshipFields member — so this case is aimed at buildMorphSliced directly, and at the claim
	// that made the knob's enablement rule different from its four neighbours.
	const uint16_t zone = 400;
	const double gammaBase = 0.0;
	// A zone position whose amp-noise field is actually alive; most are not (the field is
	// zone-derived and zero over most of the space), and a knob tested where the field is zero is a
	// knob tested where right and wrong answers agree.
	float offset = 0.0f;
	bool found = false;
	for (int i = 0; i < 200000 && !found; ++i) {
		const float candidate = static_cast<float>(i) * 0.0137f;
		const double home = sliceHome(zone, candidate, gammaBase);
		const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
		if (buildMorphSliced(zone, candidate, 0.0, 0.0, frame, gammaBase).ampNoise > 0.01f) {
			offset = candidate;
			found = true;
		}
	}
	REQUIRE_MESSAGE(found, "no offset in range has a live amp-noise field — the field layout changed");

	const double home = sliceHome(zone, offset, gammaBase);
	const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
	const auto build = [&](float grit) {
		return buildMorphSliced(zone, offset, 0.0, 0.0, frame, gammaBase, grit).ampNoise;
	};

	// THE FIELD ITSELF, recovered from the neutral build — the table stores raw * 0.25, and neutral
	// is the identity, so this is the triangle the knob is supposed to be biasing.
	const float neutral = build(feathers::kFieldNeutral);
	const float raw = neutral / 0.25f;
	REQUIRE(raw > 0.0f);

	// THE CURVE IS APPLIED TO THE TRIANGLE, NOT TO THE SCALED RESULT. This is the assertion that
	// catches the plausible wrong version: biasing after the 0.25 would lift a [0, 0.25] quantity
	// toward 1.0 and hand the renderer four times the grit the hardware can make.
	for (const float knob : { 0.0f, 0.25f, feathers::kFieldNeutral, 0.75f, 1.0f }) {
		CAPTURE(knob);
		CHECK(build(knob) == doctest::Approx(feathers::fieldWithOverride(raw, knob) * 0.25f));
	}
	// So the ceiling is the hardware's, at the top of the knob's travel.
	CHECK(build(1.0f) == doctest::Approx(0.25f));
	CHECK(build(0.0f) == doctest::Approx(0.0f));

	// AND IT TRAVELS WITH THE SOUND rather than with the blend: a SoundCoord carrying the scale must
	// reach the table through the plain-Morph road, which is the road a non-V5 patch takes and the
	// whole reason this knob is not greyed off-V5.
	Character c = oneSound(zone, offset, gammaBase, 0.0, 0.0);
	c.sounds[0].gritScale = 0.0f;
	CHECK(buildMorphFor(c).ampNoise == doctest::Approx(0.0f));
	c.sounds[0].gritScale = 1.0f;
	CHECK(buildMorphFor(c).ampNoise == doctest::Approx(0.25f));
}

TEST_CASE("the sliced entry and the blend entry apply the same five field modifiers") {
	// THE PAIR CLAUDE.md NAMES BY NAME. An effect applied at one of the two entry points and not the
	// other renders ATTENUATED rather than absent — the terrain quietly draws a different sound from
	// the one being played — and a difference check passes on it, because something did change.
	//
	// So: a single sound at weight 1.0 with all five knobs OFF NEUTRAL, through both roads, to the
	// bit. The existing position-purity case makes this claim at neutral; this is the same claim
	// where the knobs are doing something, which is where the two roads can drift apart.
	const double gammaBase = 3.25;
	const double tx = 0.3;
	const double ty = -0.2;
	const float offset = findOffset(400, gammaBase, tx, ty, [](const FlagshipAuthors& a) {
		return a.fields.quantize > 0.05f && a.fields.sync > 0.05f && a.fields.feedback > 0.05f;
	});

	feathers::FieldScales scales {};
	scales.quantize = 0.82f;
	scales.sync = 0.17f;
	scales.feedback = 0.93f;
	scales.points = 0.71f;
	scales.grit = 0.34f;
	scales.smoothness = 0.64f;

	Character c = oneSound(400, offset, gammaBase, tx, ty);
	c.sounds[0].quantizeScale = scales.quantize;
	c.sounds[0].syncScale = scales.sync;
	c.sounds[0].feedbackScale = scales.feedback;
	c.sounds[0].pointScale = scales.points;
	c.sounds[0].gritScale = scales.grit;
	c.sounds[0].smoothnessScale = scales.smoothness;
	const FlagshipRecipe viaBank = buildFlagshipFor(c);

	const double home = sliceHome(400, offset, gammaBase);
	const FlagshipRecipe viaSlice =
	    buildFlagshipSliced(400, offset, tx, ty, sliceFrameFor<kMorphTriangles>(0.0, home, {}),
	                        sliceFrameFor<kStairTriangles>(0.0, home, {}), gammaBase, scales);

	CHECK(viaBank.quantize == viaSlice.quantize);
	CHECK(viaBank.syncRatio == viaSlice.syncRatio);
	CHECK(viaBank.feedback == viaSlice.feedback);
	CHECK(viaBank.smoothness == viaSlice.smoothness);
	CHECK(viaBank.pointCount == viaSlice.pointCount);
	CHECK(viaBank.morph.ampNoise == viaSlice.morph.ampNoise);

	// AND THE SCALED VALUES ARE NOT THE NEUTRAL ONES — without this the six equalities above are
	// satisfied by two roads that both ignore the knobs entirely, which is precisely the failure
	// being guarded against.
	const FlagshipRecipe restBank = buildFlagshipFor(oneSound(400, offset, gammaBase, tx, ty));
	CHECK(viaBank.quantize != restBank.quantize);
	CHECK(viaBank.syncRatio != restBank.syncRatio);
	CHECK(viaBank.feedback != restBank.feedback);
	// GRIT IS DELIBERATELY NOT IN THE NON-VACUITY LIST. The offset above was chosen for the three
	// OPERATOR fields, and the amp-noise triangle is zero there — measured: both roads return 0.0, so
	// asserting a difference would fail and asserting equality proves nothing. Its own case
	// ("Grit scales the morph table's amp noise…") finds a coordinate where the field is alive and
	// makes the claim there. Recorded rather than quietly dropped, because a missing line in a
	// non-vacuity list is exactly what makes the list stop meaning anything.
	CHECK(viaBank.morph.ampNoise == restBank.morph.ampNoise);
}


TEST_CASE("the amp-noise field is in [0,1], which is what makes the Grit knob's curve legal") {
	// THE ASSUMPTION UNDER THE GRIT KNOB, ASSERTED RATHER THAN BELIEVED — and it is the assumption
	// whose twin was FALSE next door. fieldWithOverride clamps its field argument to [0,1] before
	// doing anything, so it is the identity at neutral only for a field that is already in that
	// interval. `fields.pointCount` is NOT (it is a count on [2,8]), and applying the same curve to
	// it collapsed every smooth waveform in the engine to a sine — see pointFieldWithOverride.
	//
	// MEASURED HERE, over 720 zones by 60 offsets: [0, 0.999622]. Consistent with modifiers.h's
	// independent measurement of the SCALED value ("sweeping all 720 zones across 200 phase offsets
	// tops out at 0.249997", i.e. a raw ceiling of 0.999988), which is what makes two sweeps agreeing
	// worth more than either alone.
	//
	// SO THE CLAMP IS A NO-OP TODAY AND THIS CASE IS WHAT KEEPS IT ONE. If the amp-noise bank is ever
	// re-ranged, Grit silently stops being the identity at neutral — every untouched patch changes
	// sound — and nothing else in the suite would say so.
	float lo = 1e9f;
	float hi = -1e9f;
	for (int z = 0; z < 720; z += 7) {
		for (int i = 0; i < 60; ++i) {
			const float offset = static_cast<float>(i) * 0.137f;
			const double home = sliceHome(static_cast<uint16_t>(z), offset, 0.0);
			const MorphBasis frame = sliceFrameFor<kMorphTriangles>(0.0, home, {});
			const float raw =
			    buildMorphSliced(static_cast<uint16_t>(z), offset, 0.0, 0.0, frame, 0.0).ampNoise
			    / 0.25f;
			lo = std::min(lo, raw);
			hi = std::max(hi, raw);
		}
	}
	MESSAGE("rawAmpNoise range: [" << lo << ", " << hi << "]");
	// Non-vacuity: the sweep must actually have found live ground, or the bounds below are a claim
	// about a field that was zero everywhere the scan looked.
	CHECK(hi > 0.5f);
	CHECK(lo >= 0.0f);
	CHECK(hi <= 1.0f);
}

TEST_CASE("the smooth curve's division-free wrap reads the very taps the modulo read (backlog PB)") {
	// THE MODULO, RESTATED HERE as the reference: the curve as it was before its index lost the divisions, so the
	// case stands still while the engine's spelling moves. Every tap through `((i % n) + n) % n`.
	const auto reference = [](const float* amp, int n, double p) {
		const double scaled = p * static_cast<double>(n);
		const auto cell = static_cast<int>(std::floor(scaled));
		const double u = scaled - static_cast<double>(cell);
		const auto at = [amp, n](int i) { return static_cast<double>(amp[static_cast<size_t>(((i % n) + n) % n)]); };
		const double a = at(cell - 1);
		const double b = at(cell);
		const double c = at(cell + 1);
		const double d = at(cell + 2);
		return 0.5
		       * ((2.0 * b) + (-a + c) * u + (2.0 * a - 5.0 * b + 4.0 * c - d) * u * u
		          + (-a + 3.0 * b - 3.0 * c + d) * u * u * u);
	};
	// Distinct points, so a tap read from the wrong index cannot come back with the right value.
	std::array<float, kFlagshipMaxSmoothPoints> amp {};
	for (size_t i = 0; i < amp.size(); ++i) {
		amp[i] = 0.37f * static_cast<float>(i) - 0.9f + 0.011f * static_cast<float>(i * i);
	}
	// THE EDGES ARE WHERE AN ADD-OR-SUBTRACT INDEX CAN DIFFER FROM A MODULO: the first cell (tap -1), the last (taps
	// n and n + 1), a phase that rounds up to exactly 1 (cell n, taps up to n + 2 — two periods out at n = 2), and a
	// phase just below 0 (cell -1, tap -2). Plus the slot phases the bake reads and a dense sweep between.
	std::vector<double> phases = { 0.0, 1.0, std::nextafter(1.0, 0.0), -1e-300, -0.25, 1.5, 2.75 };
	for (int j = 0; j <= 4096; ++j) {
		phases.push_back(static_cast<double>(j) / 4096.0);
	}
	int checked = 0;
	int wrapsNearEnd = 0;
	for (int n = kFlagshipMinSmoothPoints; n <= kFlagshipMaxSmoothPoints; ++n) {
		CAPTURE(n);
		for (const double p : phases) {
			CAPTURE(p);
			const double want = reference(amp.data(), n, p);
			const double got = flagshipSmoothCurveForTest(amp.data(), n, p);
			REQUIRE(std::memcmp(&want, &got, sizeof want) == 0);
			++checked;
			wrapsNearEnd += static_cast<int>(std::floor(p * static_cast<double>(n))) >= n - 1;
		}
	}
	// The sweep really did reach the cells whose taps wrap, at every n.
	CHECK(wrapsNearEnd >= (kFlagshipMaxSmoothPoints - kFlagshipMinSmoothPoints + 1) * 100);
	CHECK(checked > 4000 * (kFlagshipMaxSmoothPoints - kFlagshipMinSmoothPoints + 1));
}
