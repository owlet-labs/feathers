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

#include "dsp/slice_build.h"

#include "dsp/flagship.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

// THE DECODE'S PROOF, at the level that matters. test_slice_basis proves the arithmetic; this proves
// the WHOLE character follows it — every waypoint, every segment, every modifier depth, for both
// engines.
//
// The claim that carries the most weight is the first one: a sound that has not travelled is the
// firmware's sound, bit for bit, WHATEVER the orientation. That is a design rule — a rotation that
// changed the sound would be a second spelling of gamma — and it is also the drift detector the
// duplicated assembly needs, since a firmware change that moves any float fails it.
//
// Bits, not tolerances. A near-identical wavetable is a changed sound that a listener may or may not
// catch this week, and the whole reason the goldens exist is that nobody can hear 1e-7 until it
// compounds. A failure here is a stop-and-report.

using namespace feathers;
using owlet::dsp::buildPhiMorphWavetable;
using owlet::dsp::buildPhiStairParams;
using owlet::dsp::PhiMorphWavetable;
using owlet::dsp::PhiStairParams;

namespace {

bool sameBits(const PhiMorphWavetable& a, const PhiMorphWavetable& b) {
	// Whole-struct memcmp is meaningful here because both are value-initialised at the top of their
	// builder, so any padding is zero on both sides rather than whatever the stack held.
	return std::memcmp(&a, &b, sizeof(PhiMorphWavetable)) == 0;
}

bool sameBits(const PhiStairParams& a, const PhiStairParams& b) {
	return std::memcmp(&a, &b, sizeof(PhiStairParams)) == 0;
}

bool sameBits(const VoxParams& a, const VoxParams& b) {
	return std::memcmp(&a, &b, sizeof(VoxParams)) == 0;
}

/// Field by field rather than memcmp: PhiWeaveParams has padding after its bow mode, and the identity
/// is about the laws, not about bytes nobody reads.
bool sameBits(const owlet::dsp::PhiWeaveParams& a, const owlet::dsp::PhiWeaveParams& b) {
	const auto same = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof(x)) == 0; };
	return same(a.stiffness, b.stiffness) && same(a.coupling, b.coupling) && same(a.damping, b.damping)
	       && same(a.bowBalance, b.bowBalance) && a.bowMode == b.bowMode && same(a.home, b.home)
	       && same(a.bowDepth, b.bowDepth) && same(a.bowRate, b.bowRate) && same(a.bowPos, b.bowPos)
	       && same(a.bowSpread, b.bowSpread) && same(a.travelRate, b.travelRate) && same(a.pluckPos, b.pluckPos)
	       && same(a.pluckWidth, b.pluckWidth) && same(a.pluckAmp, b.pluckAmp)
	       && same(a.morphBowGain, b.morphBowGain) && same(a.outGain, b.outGain)
	       && same(a.shimmerAlpha, b.shimmerAlpha);
}

/// The firmware's buildPhiVoxParams, restated from its source for the identity test — NOT linked,
/// unlike morph's and stair's, and the boundary is worth its length: the vox builder shares its TU
/// with the render layer, whose sine table drags the argon dependency the host build has no reason
/// to grow. The twelve bank configs are the firmware header's own constants on both sides, so a
/// retune there flows through both; the anchors and the clamp are that TU's private numbers, quoted
/// here from its source. What this case therefore proves is that buildVoxSliced at zero travel IS
/// the firmware's arithmetic as written; what it cannot see is a silent change to the firmware's
/// PRIVATE constants — said here rather than pretended otherwise (the header comment on
/// buildVoxSliced carries the same boundary).
VoxParams firmwareVoxReference(uint16_t zone, float phaseOffset) {
	using owlet::dsp::phi::evalTriangle;
	VoxParams p {};

	double phase = static_cast<double>(zone) / 1023.0 + static_cast<double>(phaseOffset);

	constexpr float kAnchorF1[8] = {300.0f, 350.0f, 450.0f, 750.0f, 550.0f, 900.0f, 250.0f, 700.0f};
	constexpr float kAnchorF2[8] = {2200.0f, 800.0f, 900.0f, 1150.0f, 1900.0f, 1600.0f, 600.0f, 3400.0f};
	float zonePos = static_cast<float>(zone) / 1023.0f * 7.0f;
	int32_t zi = std::min(static_cast<int32_t>(zonePos), static_cast<int32_t>(6));
	float zf = zonePos - static_cast<float>(zi);
	float f1Anchor = kAnchorF1[zi] + (kAnchorF1[zi + 1] - kAnchorF1[zi]) * zf;
	float f2Anchor = kAnchorF2[zi] + (kAnchorF2[zi + 1] - kAnchorF2[zi]) * zf;

	float shiftMul = std::exp2(evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxShift) * 0.6f);
	float f1 = f1Anchor * std::exp2(evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxF1Wander) * 0.6f) * shiftMul;
	float f2 = f2Anchor * std::exp2(evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxF2Wander) * 0.8f) * shiftMul;
	p.formant[0].frequencyHz = std::clamp(f1, 60.0f, 8000.0f);
	p.formant[1].frequencyHz = std::clamp(f2, 60.0f, 8000.0f);

	float n1 = 1.0f + evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxN1) * 11.0f;
	float n2 = 1.0f + evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxN2) * 11.0f;
	float b1 = 0.30f + evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxDecay1) * 0.88f;
	float b2 = 0.30f + evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxDecay2) * 0.88f;
	float pol1 = evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxPolarity1);
	float pol2 = evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxPolarity2);

	float balance = 0.15f + evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxBalance) * 0.85f;
	float gains[kVoxNumFormants] = {0.62f, 0.62f * balance};
	float counts[kVoxNumFormants] = {n1, n2};
	float decays[kVoxNumFormants] = {b1, b2};
	float pols[kVoxNumFormants] = {pol1, pol2};

	for (size_t f = 0; f < kVoxNumFormants; f++) {
		float count = std::min(counts[f], static_cast<float>(kVoxMaxPulses));
		int32_t nFull = static_cast<int32_t>(count);
		float partial = count - static_cast<float>(nFull);
		float mag = 1.0f;
		float peak = 0.0f;
		for (int32_t k = 0; k < nFull + (partial > 0.0f ? 1 : 0) && k < static_cast<int32_t>(kVoxMaxPulses);
		     k++) {
			float sign = 1.0f - 2.0f * static_cast<float>(k & 1) * pols[f];
			float frac = (k < nFull) ? 1.0f : partial;
			p.formant[f].pulseGain[static_cast<size_t>(k)] = mag * sign * frac;
			peak = std::max(peak, std::abs(mag) * frac);
			mag *= decays[f];
		}
		float norm = (peak > 0.0001f) ? gains[f] / peak : 0.0f;
		for (size_t k = 0; k < kVoxMaxPulses; k++) {
			p.formant[f].pulseGain[k] *= norm;
		}
	}

	p.breath = evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxBreath);
	p.breath = p.breath * p.breath * 0.30f;
	p.articulation = 0.5f + evalTriangle(phase, 1.0f, owlet::dsp::kPhiVoxArticulation) * 3.5f;

	return p;
}

/// A spread of orientations to quantify over, including the born rotate 0 — what every unpinned
/// sound plays — and turns of both signs, small and large. Written once because every claim about
/// "whatever the orientation" should mean the same set.
std::vector<SliceGenerator> orientations() {
	std::vector<SliceGenerator> all;
	all.emplace_back(); // rotate 0, the born plane
	for (float rotate : { -1.4f, -0.7f, 0.2f, 0.8f, 1.5f, 4.1f }) {
		all.push_back(SliceGenerator { rotate });
	}
	return all;
}

/// Every morph coordinate's own φ frequency, in slot order — the ladder the plane's rate row is
/// judged against. Gathered from the banks rather than recalled, so a bank that is retuned moves the
/// measurement with it.
std::array<float, kMorphTriangles> morphSlotFrequencies() {
	std::array<float, kMorphTriangles> freq {};
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMaxSegments); ++i) {
		freq[morph_slot::kPhaseDelta + i] = owlet::dsp::kPhiMorphPhaseBank[i].phiFreq;
		freq[morph_slot::kCurvature + i] = owlet::dsp::kPhiMorphCurvatureBank[i].phiFreq;
	}
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMovableWaypoints); ++i) {
		freq[morph_slot::kAmplitude + i] = owlet::dsp::kPhiMorphAmplitudeBank[i].phiFreq;
	}
	freq[morph_slot::kGain] = owlet::dsp::kPhiMorphGainBank[0].phiFreq;
	freq[morph_slot::kEndpoint] = owlet::dsp::kPhiMorphEndpointBank[0].phiFreq;
	freq[morph_slot::kAmpOvershoot] = owlet::dsp::kPhiMorphAmpOvershootBank[0].phiFreq;
	freq[morph_slot::kCurvBoost] = owlet::dsp::kPhiMorphCurvBoostBank[0].phiFreq;
	freq[morph_slot::kPhaseDistort] = owlet::dsp::kPhiMorphPhaseDistortBank[0].phiFreq;
	freq[morph_slot::kSineBlend] = owlet::dsp::kPhiMorphSineBlendBank[0].phiFreq;
	freq[morph_slot::kOddSym] = owlet::dsp::kPhiMorphOddSymBank[0].phiFreq;
	freq[morph_slot::kWindow] = owlet::dsp::kPhiMorphWindowBank[0].phiFreq;
	freq[morph_slot::kSlopeMatch] = owlet::dsp::kPhiMorphSlopeMatchBank[0].phiFreq;
	freq[morph_slot::kPhaseJitter] = owlet::dsp::kPhiMorphPhaseJitterBank[0].phiFreq;
	freq[morph_slot::kAmpNoise] = owlet::dsp::kPhiMorphAmpNoiseBank[0].phiFreq;
	freq[morph_slot::kAsymGain] = owlet::dsp::kPhiMorphAsymGainBank[0].phiFreq;
	return freq;
}

/// How wide a ladder is, in the two units the whole rotate arc has been measured in: how many
/// twelfth-of-an-octave bins its values fall in (how many DISTINCT scales the eye and ear get), and
/// how many octaves the middle 80% spans (where the bulk sits, ignoring one outlier at each end).
std::pair<double, size_t> ladderStats(std::vector<double> values) {
	std::sort(values.begin(), values.end());
	std::set<long> bins;
	for (const double v : values) {
		bins.insert(std::lround(std::log2(v) * 12.0));
	}
	const size_t lo = values.size() / 10;
	const size_t hi = values.size() - 1 - values.size() / 10;
	return { std::log2(values[hi] / values[lo]), bins.size() };
}

} // namespace

TEST_CASE("a sound that has not travelled is the firmware's, bit for bit, whatever the plane") {
	for (const SliceGenerator& generator : orientations()) {
		for (int zone = 0; zone <= 1023; zone += 13) {
			for (int step = 0; step < 5; ++step) {
				const auto z = static_cast<uint16_t>(zone);
				const float offset = static_cast<float>(step) * 0.237f - 0.5f;
				// DERIVED AT THE PLACE THE CHARACTER IS BUILT AT, which M made necessary and which makes
				// this case stronger than it was. The plane is a function of the home now, so one basis
				// carried across a thousand zones would have been testing the guarantee for one sound and
				// nine hundred of its neighbours — and it would still have PASSED, because zero travel
				// returns home whatever the plane. That is exactly why it needed changing rather than
				// watching: a case that cannot fail for the reason it names is not guarding it.
				const double home = sliceHome(z, offset, 0.0);
				CHECK(sameBits(
				    buildMorphSliced(z, offset, 0.0, 0.0,
				                     sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), home), 0.0),
				    buildPhiMorphWavetable(z, offset)));
				CHECK(sameBits(
				    buildStairSliced(z, offset, 0.0, 0.0,
				                     sliceBasisFor<kStairTriangles>(sliceRotateOf(generator), home), 0.0),
				    buildPhiStairParams(z, offset)));
			}
		}
	}
}

TEST_CASE("an untravelled vox sound is the firmware's arithmetic, bit for bit, whatever the plane") {
	// The vox spelling of the case above, against the restated reference rather than the linked
	// builder — firmwareVoxReference's comment carries the boundary. The quantifier is the same:
	// zero travel returns home whatever the orientation, so every plane must build the same recipe.
	for (const SliceGenerator& generator : orientations()) {
		for (int zone = 0; zone <= 1023; zone += 13) {
			for (int step = 0; step < 5; ++step) {
				const auto z = static_cast<uint16_t>(zone);
				const float offset = static_cast<float>(step) * 0.237f - 0.5f;
				const double home = sliceHome(z, offset, 0.0);
				CHECK(sameBits(
				    buildVoxSliced(z, offset, 0.0, 0.0,
				                   sliceBasisFor<kVoxTriangles>(sliceRotateOf(generator), home), 0.0),
				    firmwareVoxReference(z, offset)));
			}
		}
	}
}


TEST_CASE("an untravelled Resonate sound is the firmware's laws, bit for bit, whatever the plane") {
	// The Resonate spelling of the identity above, against the linked port of the firmware builder
	// (character/phi_weave.cpp): the slice road restates that builder, and this is what stops the two
	// drifting. At zero travel every plane must hand the string the laws buildPhiWeaveLaws gives at the
	// sound's home phase — which is `zone/1023 + offset`, the firmware caller's own sum.
	for (const SliceGenerator& generator : orientations()) {
		for (int zone = 0; zone <= 1023; zone += 13) {
			for (int step = 0; step < 5; ++step) {
				const auto z = static_cast<uint16_t>(zone);
				const float offset = static_cast<float>(step) * 0.237f - 0.5f;
				const double home = sliceHome(z, offset, 0.0);
				CHECK(sameBits(
				    buildWeaveSliced(z, offset, 0.0, 0.0,
				                     sliceBasisFor<kWeaveTriangles>(sliceRotateOf(generator), home), 0.0),
				    owlet::dsp::buildPhiWeaveLaws(static_cast<double>(z) / 1023.0 + static_cast<double>(offset))));
			}
		}
	}
}

TEST_CASE("with the plane closed, a travelling Resonate sound is the firmware's laws at the offset it travelled") {
	// The control from outside: a closed plane turns travel into gamma, so the laws must be the
	// firmware's at home + (tx + ty) — that association, the decode's own. A builder that ignored the basis would pass the identity above and
	// fail here at the first non-zero travel.
	const WeaveBasis basis = degenerateSliceBasis<kWeaveTriangles>();
	for (int zone = 0; zone <= 1023; zone += 29) {
		const auto z = static_cast<uint16_t>(zone);
		CHECK(sameBits(buildWeaveSliced(z, 0.5f, 0.25, 0.125, basis, 0.0),
		               owlet::dsp::buildPhiWeaveLaws(sliceHome(z, 0.5f, 0.0) + 0.375)));
	}
}
TEST_CASE("with the plane closed, travelling is indistinguishable from gamma") {
	// The control with an answer from OUTSIDE this file: close the plane and travel, and the character
	// must be the firmware's at the offset that travel amounts to. If the builders quietly ignored the
	// basis and summed their coordinates, the test above would still pass — the sum is what a closed
	// plane computes — and this one would fail at the first non-zero travel.
	//
	// Dyadic travel so the sum is exact in float and double alike, and the comparison is about the
	// decode rather than about rounding.
	const MorphBasis morphBasis = degenerateSliceBasis<kMorphTriangles>();
	const StairBasis stairBasis = degenerateSliceBasis<kStairTriangles>();
	for (int zone = 0; zone <= 1023; zone += 29) {
		const float offset = 0.5f;
		const auto z = static_cast<uint16_t>(zone);
		CHECK(sameBits(buildMorphSliced(z, offset, 0.25, 0.125, morphBasis, 0.0),
		               buildPhiMorphWavetable(z, offset + 0.375f)));
		CHECK(sameBits(buildStairSliced(z, offset, 0.25, 0.125, stairBasis, 0.0),
		               buildPhiStairParams(z, offset + 0.375f)));
		// And the two directions are interchangeable while the plane is shut, which is the collapse
		// this stage exists to end.
		CHECK(sameBits(buildMorphSliced(z, offset, 0.375, 0.0, morphBasis, 0.0),
		               buildMorphSliced(z, offset, 0.0, 0.375, morphBasis, 0.0)));
	}
}

TEST_CASE("every coefficient addresses its own triangle, and no two share one") {
	// Per-triangle addressing is the claim the whole slice rests on. If a coefficient bled across
	// slots — the plausible failure is a copy-paste in the slot table, one bank reading another's
	// index — the plane would still be a plane, the identity test would still pass, and the geometry
	// would be nothing like the one the generator describes. A breadcrumb would then not lead back,
	// which is the property this stage is supposed to buy.
	//
	// So the check is exhaustive rather than illustrative: perturb ONE slot at a time and require that
	// all N perturbations are distinguishable from each other and from the untouched table. Two slots
	// wired to the same coefficient produce the same table for both, everywhere, and that is exactly
	// what this refuses to accept. An earlier version poked a single slot and watched a few fields; it
	// passed a deliberately collided build.
	//
	// "Somewhere in the sweep" rather than "at a point": a perturbed triangle sitting in the silent
	// part of its duty cycle changes nothing there, which is the character layer being itself.
	auto exhaustive = [](auto build, auto makeBasis, size_t n, const std::vector<size_t>& inert) {
		std::vector<std::vector<bool>> distinct(n, std::vector<bool>(n, false));
		std::vector<bool> reachedPhase(n, false);
		std::vector<bool> reachedRate(n, false);

		for (int zone = 5; zone <= 1023; zone += 53) {
			for (int step = 1; step <= 3; ++step) {
				const double travel = static_cast<double>(step) * 0.29;
				auto base = build(static_cast<uint16_t>(zone), 0.1f, travel, makeBasis(n, n, 0.0f));

				std::vector<decltype(base)> perturbed;
				perturbed.reserve(n);
				for (size_t k = 0; k < n; ++k) {
					perturbed.push_back(
					    build(static_cast<uint16_t>(zone), 0.1f, travel, makeBasis(k, n, 0.5f)));
					if (!sameBits(perturbed.back(), base)) {
						reachedPhase[k] = true;
					}
					// The rate half is wired separately and has to be shown separately: a build that
					// decoded phases correctly and dropped freqMult on the floor would pass everything
					// above and deliver half a plane.
					if (!sameBits(build(static_cast<uint16_t>(zone), 0.1f, travel, makeBasis(n + k, n, 0.5f)),
					              base)) {
						reachedRate[k] = true;
					}
				}
				for (size_t a = 0; a < n; ++a) {
					for (size_t b = a + 1; b < n; ++b) {
						if (!sameBits(perturbed[a], perturbed[b])) {
							distinct[a][b] = true;
						}
					}
				}
			}
		}

		for (size_t k = 0; k < n; ++k) {
			const bool expectInert = std::find(inert.begin(), inert.end(), k) != inert.end();
			INFO("slot ", k);
			CHECK(reachedPhase[k] == !expectInert);
			CHECK(reachedRate[k] == !expectInert);
			for (size_t b = k + 1; b < n; ++b) {
				if (expectInert || std::find(inert.begin(), inert.end(), b) != inert.end()) {
					continue; // an inert slot moves nothing, so it cannot be told from another one
				}
				INFO("slots ", k, " and ", b);
				CHECK(distinct[k][b]);
			}
		}
	};

	SUBCASE("morph") {
		// ONE TRIANGLE IN THE MORPH BANK IS DEAD, and it is dead in the firmware rather than here:
		// kPhiMorphPhaseBank declares kPhiMorphMaxSegments (8) phase deltas, the builder evaluates all
		// eight, and the accumulation that consumes them runs over kPhiMorphMovableWaypoints (7). So
		// deltas[7] is computed and never read, on hardware too. The plane keeps the slot so that slot
		// index and bank index stay the same number — a renumbering would be a bug factory — and the
		// test pins the inertness so that a firmware change which starts reading it is announced here
		// rather than discovered as a mystery. It is also a spare dimension somebody may want later,
		// which is a sound-affecting decision and not this stage's to take.
		exhaustive(
		    [](uint16_t z, float o, double t, const MorphBasis& b) {
			    return buildMorphSliced(z, o, t, t * 0.5, b, 0.0);
		    },
		    [](size_t coord, size_t n, float amount) {
			    MorphBasis basis = degenerateSliceBasis<kMorphTriangles>();
			    if (coord < 2 * n) {
				    basis.y[coord] += amount;
			    }
			    return basis;
		    },
		    kMorphTriangles, { morph_slot::kPhaseDelta + 7 });
	}
	SUBCASE("stair") {
		exhaustive(
		    [](uint16_t z, float o, double t, const StairBasis& b) {
			    return buildStairSliced(z, o, t, t * 0.5, b, 0.0);
		    },
		    [](size_t coord, size_t n, float amount) {
			    StairBasis basis = degenerateSliceBasis<kStairTriangles>();
			    if (coord < 2 * n) {
				    basis.y[coord] += amount;
			    }
			    return basis;
		    },
		    kStairTriangles, {});
	}
	SUBCASE("vox") {
		// No inert slot, and that is a V22c claim as much as a wiring one: articulation's slot must
		// be LIVE from birth — evaluated, reaching the output, addressable by residue and rotation —
		// even though nothing consumes its product until the motion arc. A dead slot here would mean
		// the wire and residue rows grow again when the motion arc lands, which is exactly what
		// carrying it now exists to prevent.
		exhaustive(
		    [](uint16_t z, float o, double t, const VoxBasis& b) {
			    return buildVoxSliced(z, o, t, t * 0.5, b, 0.0);
		    },
		    [](size_t coord, size_t n, float amount) {
			    VoxBasis basis = degenerateSliceBasis<kVoxTriangles>();
			    if (coord < 2 * n) {
				    basis.y[coord] += amount;
			    }
			    return basis;
		    },
		    kVoxTriangles, {});
	}
	SUBCASE("weave") {
		// No inert slot: every one of the twenty-seven banks reaches the laws the string plays under.
		exhaustive(
		    [](uint16_t z, float o, double t, const WeaveBasis& b) {
			    return buildWeaveSliced(z, o, t, t * 0.5, b, 0.0);
		    },
		    [](size_t coord, size_t n, float amount) {
			    WeaveBasis basis = degenerateSliceBasis<kWeaveTriangles>();
			    if (coord < 2 * n) {
				    basis.y[coord] += amount;
			    }
			    return basis;
		    },
		    kWeaveTriangles, {});
	}
}

TEST_CASE("with the plane open, the two travel directions lead to different characters") {
	// The gate question in the only form a test can ask it. With the plane shut, a step in Pos X and a
	// step in Pos Y build the same character — that IS the collapse — so this is what separates the
	// new engine from the old one rather than a restatement of it.
	const SliceGenerator generator { 0.5f };
	int morphDiffers = 0;
	int stairDiffers = 0;
	for (int zone = 0; zone <= 1023; zone += 37) {
		const auto z = static_cast<uint16_t>(zone);
		// The plane this zone's own home gives it — M reads both, so a basis derived once and carried
		// across the sweep would be asking the question about one place and answering it at twenty-eight.
		const double home = sliceHome(z, 0.0f, 0.0);
		const MorphBasis morphBasis = sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), home);
		const StairBasis stairBasis = sliceBasisFor<kStairTriangles>(sliceRotateOf(generator), home);
		if (!sameBits(buildMorphSliced(z, 0.0f, 0.4, 0.0, morphBasis, 0.0),
		              buildMorphSliced(z, 0.0f, 0.0, 0.4, morphBasis, 0.0))) {
			morphDiffers++;
		}
		if (!sameBits(buildStairSliced(z, 0.0f, 0.4, 0.0, stairBasis, 0.0),
		              buildStairSliced(z, 0.0f, 0.0, 0.4, stairBasis, 0.0))) {
			stairDiffers++;
		}
	}
	CHECK(morphDiffers > 20);
	CHECK(stairDiffers > 20);
}

TEST_CASE("travel goes somewhere the old line never went") {
	// Not merely "different from where I started" — a step of gamma is different from where you
	// started too, and if that were all travel did it would be a redundant second gamma. So the
	// measure is against the WHOLE old line: no amount of gamma reproduces where travel arrives.
	const SliceGenerator generator { 0.5f };
	const uint16_t zone = 512;
	const MorphBasis basis =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), sliceHome(zone, 0.0f, 0.0));
	const PhiMorphWavetable travelled = buildMorphSliced(zone, 0.0f, 0.0, 0.35, basis, 0.0);

	// Every home the old engine can reach at this zone, finely swept. None of them is the sound that
	// travelling arrived at.
	bool foundOnTheLine = false;
	for (int step = -2000; step <= 2000; ++step) {
		const float offset = static_cast<float>(step) * 0.001f;
		if (sameBits(buildPhiMorphWavetable(zone, offset), travelled)) {
			foundOnTheLine = true;
		}
	}
	CHECK_FALSE(foundOnTheLine);
}

TEST_CASE("a pin overrides the born rotate, and nothing else does") {
	// The experimental handle, which by design stays and may never become a CLAP parameter.
	// Since the shrink there is no derivation to override: an unpinned sound is simply NOT TURNED —
	// rotate 0, at every home, always — and where the sound is enters the decode as home instead. The
	// φ-derived orientation field this pin used to displace is deleted, with its reasons on record at
	// rotateFor.
	SliceOrientationPin empty;
	CHECK(rotateFor(empty) == 0.0);

	SliceOrientationPin held;
	held.pinned = true;
	held.generator.rotate = 0.5f;
	CHECK(rotateFor(held) == 0.5);
	// A pin is one rotate for every sound, which is the whole point of pinning one: two sounds compared
	// under different planes would tell you nothing about the plane. rotateFor takes nothing but the
	// pin, so this holds by signature — stated here so a future home-dependent argument has to answer
	// to it.
	CHECK(rotateFor(held) == sliceRotateOf(held.generator));
}

TEST_CASE("the blue shift's two reactivity fields are φ fields, and they are orthogonal") {
	// §2b.1's depths. They decorate a motion effect, so a wrong field is not heard as a wrong number —
	// it is heard as a region that reacts when it should have been the neighbouring one, which is
	// exactly the kind of wrong that gets called "character".
	//
	// Unipolar, because a reactivity is an amount. A bipolar field would make some regions push the
	// overshoot BACKWARDS along the velocity, which is not a thing anybody described.
	for (double at = -3.0; at <= 3.0; at += 0.017) {
		const BlueShiftFields f = blueShiftFieldsAt(at);
		CAPTURE(at);
		CHECK(f.overshoot >= 0.0f);
		CHECK(f.overshoot <= 1.0f);
		CHECK(f.doppler >= 0.0f);
		CHECK(f.doppler <= 1.0f);
	}

	// Pure and deterministic: the same position gives the same field on every call, which is what
	// makes it part of the space rather than a decoration on top of one.
	CHECK(blueShiftFieldsAt(0.371).overshoot == blueShiftFieldsAt(0.371).overshoot);
	CHECK(blueShiftFieldsAt(0.371).doppler == blueShiftFieldsAt(0.371).doppler);

	// THE PERIODS, which are what "medium-long" means in numbers. Measured in zones, since that is how
	// the plan quotes them and how a player would meet them: ~1470 zones for the overshoot field and
	// ~2370 for the Doppler one — both far longer than the zone axis itself, so what you cross inside
	// one patch is a region rather than a cycle.
	//
	// **THIS ASSERTION USED TO BE INERT, AND IT IS WORTH SAYING HOW.** It read:
	//
	//     const double overshootPeriodZones = 1023.0 / 0.6975135;
	//     const double dopplerPeriodZones   = 1023.0 / 0.4310870;
	//     CHECK(overshootPeriodZones == doctest::Approx(1467.0).epsilon(0.01));
	//     CHECK(dopplerPeriodZones   == doctest::Approx(2373.0).epsilon(0.01));
	//
	// — two literals divided by two literals, compared against two more. **It never called the code.**
	// MEASURED: multiplying `kOvershootRate` by 1.7 in `slice_build.cpp` leaves all 34 suites green.
	//
	// Its own comment explained the choice — *"Read off the triangles' own rates rather than by
	// hunting for repeats, because a triangle read at a position IS periodic by construction and a
	// search would only be re-deriving the constant"* — and that is the reasoning that made it
	// decoration. Re-deriving the constant FROM THE CODE is precisely what turns a number into a test;
	// transcribing it by hand is what leaves the source free to move underneath.
	//
	// **AND THE SOURCE COMMENT SAYS SO ITSELF, one file away.** `slice_build.cpp` derives the pair
	// rather than writing them out, and says why: *"Writing the two decimals out by hand would have
	// made that ratio approximate — φ to four places divided by φ to four places is not φ."* The test
	// then wrote the two decimals out by hand.
	//
	// SO THE PERIODS ARE FOUND FROM THE FIELD. A unipolar triangle is exactly zero outside its duty,
	// so the position where it leaves zero is a sharp, bisectable event; the spacing between the first
	// such onset and the twentieth is the period, to a part in 1e8.
	const auto onsetAfter = [](double from, bool overshoot) {
		const auto value = [overshoot](double at) {
			const BlueShiftFields f = blueShiftFieldsAt(at);
			return static_cast<double>(overshoot ? f.overshoot : f.doppler);
		};
		// **THROUGH THE GAP FIRST.** These duties are 0.9 and 0.85, so the field is non-zero for most
		// of its period and "the next position where it is above zero" is almost always the position
		// you are standing on. The first version did exactly that and measured both periods as 0.0009
		// — the coarse step — with a ratio of 1. So: walk until it is exactly zero, THEN find where it
		// leaves zero.
		double at = from;
		const double limit = from + 16.0;
		while (at < limit && value(at) > 0.0) {
			at += 1.0e-4;
		}
		double lo = at;
		while (at < limit && value(at) == 0.0) {
			at += 1.0e-4;
			lo = at - 1.0e-4;
		}
		// THEN BISECT THE ZERO/NON-ZERO EDGE. Exact for a piecewise-linear field: there is one
		// crossing in the bracket and it is a step in the derivative, not a root of a curve.
		double hi = lo + 1.0e-4;
		for (int i = 0; i < 60; ++i) {
			const double mid = 0.5 * (lo + hi);
			if (value(mid) > 0.0) {
				hi = mid;
			}
			else {
				lo = mid;
			}
		}
		return 0.5 * (lo + hi);
	};
	const auto periodOf = [&](bool overshoot) {
		constexpr int kPeriods = 20;
		const double first = onsetAfter(0.0, overshoot);
		double at = first;
		for (int k = 0; k < kPeriods; ++k) {
			// Step clear of the duty window before hunting the next onset, or the search finds the
			// edge it is standing on.
			at = onsetAfter(at + 1.0e-3, overshoot);
		}
		return (at - first) / static_cast<double>(kPeriods);
	};
	const double overshootPeriod = periodOf(true);
	const double dopplerPeriod = periodOf(false);
	const double overshootPeriodZones = 1023.0 * overshootPeriod;
	const double dopplerPeriodZones = 1023.0 * dopplerPeriod;
	CAPTURE(overshootPeriodZones);
	CAPTURE(dopplerPeriodZones);
	CHECK(overshootPeriodZones == doctest::Approx(1467.0).epsilon(0.01));
	CHECK(dopplerPeriodZones == doctest::Approx(2373.0).epsilon(0.01));

	// **AND THE RATIO IS φ, WHICH IS THE CLAIM THE SOURCE ACTUALLY MAKES.** The two periods above are
	// where the pair happens to sit; their RATIO is why the pair was chosen — §2b.1's φ^-1.75 and
	// φ^-0.75, whose quotient is φ^1, "the canonical incommensurate choice in this codebase and the
	// same trick the character layer uses to keep its landscapes from ever realigning". A tolerance
	// loose enough to accept two hand-written decimals would accept exactly the mistake the derivation
	// exists to prevent, so this is asserted to a part in 1e5 — which the measurement supports and
	// four-decimal transcription would not.
	const double ratio = dopplerPeriod / overshootPeriod;
	CAPTURE(ratio);
	CHECK(ratio == doctest::Approx(1.6180339887).epsilon(1.0e-5));

	// ORTHOGONAL, which is the property the two rates were chosen for: a region reactive to one is not
	// thereby reactive to the other. Sampled across the whole zone axis and past it, the two fields
	// must not track each other — a correlation near 1 would mean one field wearing two names.
	double sumA = 0.0;
	double sumB = 0.0;
	double sumAA = 0.0;
	double sumBB = 0.0;
	double sumAB = 0.0;
	constexpr int kSamples = 4001;
	for (int i = 0; i < kSamples; ++i) {
		const double at = 3.0 * static_cast<double>(i) / static_cast<double>(kSamples - 1);
		const BlueShiftFields f = blueShiftFieldsAt(at);
		const auto a = static_cast<double>(f.overshoot);
		const auto b = static_cast<double>(f.doppler);
		sumA += a;
		sumB += b;
		sumAA += a * a;
		sumBB += b * b;
		sumAB += a * b;
	}
	const double n = kSamples;
	const double covariance = sumAB / n - (sumA / n) * (sumB / n);
	const double spreadA = std::sqrt(sumAA / n - (sumA / n) * (sumA / n));
	const double spreadB = std::sqrt(sumBB / n - (sumB / n) * (sumB / n));
	REQUIRE(spreadA > 0.05); // a field that never varies is not a field
	REQUIRE(spreadB > 0.05);
	const double correlation = covariance / (spreadA * spreadB);
	CAPTURE(correlation);
	CHECK(std::abs(correlation) < 0.35);
}

TEST_CASE("MEASURE: the finest feature wavelength, worst case over travel" * doctest::skip()) {
	// THE RIDER'S NUMBER, and it is the same number as the zoom stop. A triangle is
	// read at `wrapPhase((phase + offset) * phiFreq * freqMult)` (phi_triangle.hpp:168), and `phase`
	// advances one for one with home — so triangle i repeats every `1 / (phiFreq_i * rate_i)` of home,
	// and `rate_i` is `rateFor(i, tx, ty)`, which MOVES WITH TRAVEL. The finest wavelength on the chart
	// is therefore a function of WHERE you are, and the honest bound is the worst case over the travel
	// a view can reach rather than the value at travel zero.
	//
	// EXACT PER SLOT rather than (max phiFreq) x (max rate), which would be a bound on a bound: the
	// slot that is fastest at rest need not be the one the travel accelerates most.
	std::array<float, kMorphTriangles> morphFreq {};
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMaxSegments); ++i) {
		morphFreq[morph_slot::kPhaseDelta + i] = owlet::dsp::kPhiMorphPhaseBank[i].phiFreq;
		morphFreq[morph_slot::kCurvature + i] = owlet::dsp::kPhiMorphCurvatureBank[i].phiFreq;
	}
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMovableWaypoints); ++i) {
		morphFreq[morph_slot::kAmplitude + i] = owlet::dsp::kPhiMorphAmplitudeBank[i].phiFreq;
	}
	morphFreq[morph_slot::kGain] = owlet::dsp::kPhiMorphGainBank[0].phiFreq;
	morphFreq[morph_slot::kEndpoint] = owlet::dsp::kPhiMorphEndpointBank[0].phiFreq;
	morphFreq[morph_slot::kAmpOvershoot] = owlet::dsp::kPhiMorphAmpOvershootBank[0].phiFreq;
	morphFreq[morph_slot::kCurvBoost] = owlet::dsp::kPhiMorphCurvBoostBank[0].phiFreq;
	morphFreq[morph_slot::kPhaseDistort] = owlet::dsp::kPhiMorphPhaseDistortBank[0].phiFreq;
	morphFreq[morph_slot::kSineBlend] = owlet::dsp::kPhiMorphSineBlendBank[0].phiFreq;
	morphFreq[morph_slot::kOddSym] = owlet::dsp::kPhiMorphOddSymBank[0].phiFreq;
	morphFreq[morph_slot::kWindow] = owlet::dsp::kPhiMorphWindowBank[0].phiFreq;
	morphFreq[morph_slot::kSlopeMatch] = owlet::dsp::kPhiMorphSlopeMatchBank[0].phiFreq;
	morphFreq[morph_slot::kPhaseJitter] = owlet::dsp::kPhiMorphPhaseJitterBank[0].phiFreq;
	morphFreq[morph_slot::kAmpNoise] = owlet::dsp::kPhiMorphAmpNoiseBank[0].phiFreq;
	morphFreq[morph_slot::kAsymGain] = owlet::dsp::kPhiMorphAsymGainBank[0].phiFreq;

	std::array<float, kStairTriangles> stairFreq {};
	stairFreq[stair_slot::kCount] = owlet::dsp::kPhiStairCount.phiFreq;
	stairFreq[stair_slot::kSlope] = owlet::dsp::kPhiStairSlope.phiFreq;
	stairFreq[stair_slot::kTilt] = owlet::dsp::kPhiStairTilt.phiFreq;
	stairFreq[stair_slot::kAsym] = owlet::dsp::kPhiStairAsym.phiFreq;
	stairFreq[stair_slot::kLandCycles] = owlet::dsp::kPhiStairLandCycles.phiFreq;
	stairFreq[stair_slot::kHeightLand] = owlet::dsp::kPhiStairHeightLand.phiFreq;
	stairFreq[stair_slot::kAsymLand] = owlet::dsp::kPhiStairAsymLand.phiFreq;
	// The two pattern slots are read through stairSpatialSliced with their own multipliers (7 and 3 in
	// the builder), which multiply the rate exactly as a phiFreq would.
	stairFreq[stair_slot::kGlyph] = 7.0f * owlet::dsp::phi::kPhi300;
	stairFreq[stair_slot::kShard] = 3.0f * owlet::dsp::phi::kPhi200;

	// Resonate: every bank is read at `phase * phiFreq * rate` (the landscapes add their node term after
	// the multiply, which does not move with home), so its frequency is its phiFreq, slot for slot.
	const std::array<float, kWeaveTriangles> weaveFreq {
		owlet::dsp::kPhiWeaveStiffBase.phiFreq,  owlet::dsp::kPhiWeaveCoupleBase.phiFreq,
		owlet::dsp::kPhiWeaveDampBase.phiFreq,   owlet::dsp::kPhiWeaveStiffCycles.phiFreq,
		owlet::dsp::kPhiWeaveDampCycles.phiFreq, owlet::dsp::kPhiWeaveStiffLand1.phiFreq,
		owlet::dsp::kPhiWeaveStiffLand2.phiFreq, owlet::dsp::kPhiWeaveDampLand1.phiFreq,
		owlet::dsp::kPhiWeaveDampLand2.phiFreq,  owlet::dsp::kPhiWeaveCoupleLand.phiFreq,
		owlet::dsp::kPhiWeaveHomeP2.phiFreq,     owlet::dsp::kPhiWeaveHomeP3.phiFreq,
		owlet::dsp::kPhiWeaveHomeP5.phiFreq,     owlet::dsp::kPhiWeaveHomePh2.phiFreq,
		owlet::dsp::kPhiWeaveHomePh3.phiFreq,    owlet::dsp::kPhiWeaveBowDepth.phiFreq,
		owlet::dsp::kPhiWeaveBowRate.phiFreq,    owlet::dsp::kPhiWeaveBowPos.phiFreq,
		owlet::dsp::kPhiWeaveBowSpread.phiFreq,  owlet::dsp::kPhiWeaveBowMode.phiFreq,
		owlet::dsp::kPhiWeaveTravel.phiFreq,     owlet::dsp::kPhiWeavePluckPos.phiFreq,
		owlet::dsp::kPhiWeavePluckWidth.phiFreq, owlet::dsp::kPhiWeavePluckAmp.phiFreq,
		owlet::dsp::kPhiWeaveShimmer.phiFreq,    owlet::dsp::kPhiWeaveMorphBow.phiFreq,
		owlet::dsp::kPhiWeaveOutGain.phiFreq,
	};

	// THE PLANE A REAL SOUND IS ON: the born plane at its home — rotate 0 is what an unpinned sound
	// gets. Taken at several homes because M varies the plane with the place.
	for (const double home : { 0.0, 0.37, 1.9 }) {
		const MorphBasis morph = sliceBasisFor<kMorphTriangles>(0.0, home);
		const StairBasis stair = sliceBasisFor<kStairTriangles>(0.0, home);
		const WeaveBasis weave = sliceBasisFor<kWeaveTriangles>(0.0, home);
		// The travel magnitudes a view can actually reach: zero, one x1 chart width, and the half-spans
		// of the old widest stop and of spans past it.
		for (const double travel : { 0.0, 0.0626, 0.5, 1.5, 3.0 }) {
			const auto worst = [&](const auto& basis, const auto& freq, size_t n) {
				double fastest = 0.0;
				for (size_t i = 0; i < n; ++i) {
					// The worst direction: both travel coordinates pushing the same way.
					const double rate = std::abs(static_cast<double>(basis.rateFor(i, travel, travel)));
					fastest = std::max(fastest, static_cast<double>(freq[i]) * rate);
				}
				return fastest;
			};
			const double m = worst(morph, morphFreq, kMorphTriangles);
			const double s = worst(stair, stairFreq, kStairTriangles);
			const double w = worst(weave, weaveFreq, kWeaveTriangles);
			const auto report = [&](const char* name, double fastest) {
				const double lambdaZones = 1023.0 / fastest;
				// Half a wavelength per ROW is the Nyquist criterion, and a map is 128 rows.
				MESSAGE(name << " home " << home << " travel " << travel << ": fastest "
				             << fastest << " cycles/home, lambda " << lambdaZones
				             << " zones, Nyquist span " << 128.0 * lambdaZones * 0.5 << " zones");
			};
			report("morph", m);
			report("stair", s);
			report("resonate", w);
		}
	}
}

TEST_CASE("MEASURE: which way each triangle's bands run, and how much they agree" * doctest::skip()) {
	// Most views showed a strong diagonal overlay pattern, where phi FM should look irregular. That
	// expectation is right — incommensurate φ rates are quasi-periodic and
	// should never line up into one strong direction — so a STRONG diagonal wants an author that is not
	// φ. This measures the most direct candidate straight out of the basis, with no picture in the way.
	//
	// WHY THIS IS THE FIRST TEST AND NOT A PHOTOGRAPH: the chart's vertical is home and its horizontal
	// is travelX, and triangle i is read at `phase_i = home + x_i·tx`. A place therefore keeps
	// triangle i's contribution along the line where that is constant — `dhome/dtx = -x_i` — so THE
	// BANDING DIRECTION OF EVERY TRIANGLE IS ONE COEFFICIENT OF THE BASIS. If the x row clusters, every
	// triangle bands the same way and the sum is one coherent diagonal; if it spreads, the bands cross
	// at many angles and the picture is the irregular one expected.
	//
	// And it is measured from VALUES rather than from an image, deliberately: an orientation read off a
	// picture already cost this arc a wrong answer, because the picture had been written out with its
	// rows reversed and the banding in it was the mirror of the banding on screen.
	for (const double home : { 0.0, 0.37, 1.9, 6.0 }) {
		const MorphBasis morph = sliceBasisFor<kMorphTriangles>(0.0, home);
		const StairBasis stair = sliceBasisFor<kStairTriangles>(0.0, home);
		const auto report = [&](const char* name, const auto& basis, size_t n) {
			// The phase rows only: x[0..n) are how far each triangle's PHASE moves per unit of travel,
			// which is what sets its iso-line. x[n..2n) are the rate rows and are a different question.
			std::vector<double> slopes;
			for (size_t i = 0; i < n; ++i) {
				slopes.push_back(-static_cast<double>(basis.x[i]));
			}
			double mean = 0.0;
			for (const double s : slopes) {
				mean += s;
			}
			mean /= static_cast<double>(slopes.size());
			double spread = 0.0;
			double lowest = slopes[0];
			double highest = slopes[0];
			for (const double s : slopes) {
				spread += std::abs(s - mean);
				lowest = std::min(lowest, s);
				highest = std::max(highest, s);
			}
			spread /= static_cast<double>(slopes.size());
			// The same thing as angles, because that is what an eye reads off the chart.
			const double meanAngle = std::atan(mean) * 180.0 / 3.14159265358979;
			const double lowAngle = std::atan(lowest) * 180.0 / 3.14159265358979;
			const double highAngle = std::atan(highest) * 180.0 / 3.14159265358979;
			// AND HOW MANY SIT AT EXACTLY -1, which is the degenerate plane's own value: slice_basis.h
			// defines the closed plane as "every phase coefficient 1", so a coefficient of 1 means
			// travel moves that triangle's phase exactly as far as home does — Pos X acting as a second
			// gamma for it, and an iso-line at exactly -45 degrees.
			int atMinusOne = 0;
			for (const double s : slopes) {
				atMinusOne += std::abs(s + 1.0) < 1e-3 ? 1 : 0;
			}
			MESSAGE(name << " home " << home << ": slope mean " << mean << " spread " << spread
			             << " (range " << lowest << ".." << highest << ") = angles " << meanAngle
			             << " deg, " << lowAngle << ".." << highAngle << " deg; " << atMinusOne
			             << " of " << slopes.size() << " sit at exactly -1");
		};
		report("morph", morph, kMorphTriangles);
		report("stair", stair, kStairTriangles);
	}
}

TEST_CASE("the phase plane is open: travel is not a second gamma any more") {
	// The phase plane is opened, at the rung chosen by eye from a ladder of pictures (0.4 or 0.6). This
	// is the acceptance check for what shipped, and it is the #29
	// diagnosis inverted — that case convicted the plane, this one certifies it.
	//
	// WHAT WAS WRONG: each stored angle turned exactly ONE phase coordinate, so 29 of morph's 35 sat
	// at exactly 1 — the degenerate value — and travel moved those triangles exactly as far as home
	// did. Pos X was a second gamma for five sixths of the bank, and every iso-line lay at −45°.
	//
	// RESTATED FOR M, and the change of decode does not relax it: M writes every coordinate of both
	// rows from its own φ triangle, so the count of degenerate coordinates is 0 or the odd
	// coincidence, and the slope spread is a property of `kPhaseSpread` rather than of how many angles
	// happened to reach a coordinate. What DID have to be restated is the zero-generator subcase
	// below, because by design a sound is born OPEN — see it for the reasoning.
	//
	// AND THE SPREAD IS CHECKED OVER A POPULATION, which is a restatement the small engine forced and
	// which says the thing more exactly anyway. The slope spread is a mean absolute deviation over an
	// engine's coordinates — an ESTIMATE of how far M swings, and stair takes it from nine samples
	// where morph takes it from thirty-five, so stair's number scatters either side of the same truth
	// (measured: it lands between 0.35 and 0.48 where morph sits near 0.45). Requiring every single
	// engine at every single home to clear 0.4 would therefore be a check on nine samples' luck rather
	// than on the decode. So: a floor at every home so no place is a near-degenerate accident, and the
	// 0.4 bar on the mean across a rotate sweep, where it is a statement about M.
	double meanSpread[2] = { 0.0, 0.0 };
	int spreadRuns = 0;
	for (const double home : { 0.0, 0.37, 1.9, 6.0 }) {
		// The plane an unpinned sound actually plays: rotate 0 at its own home.
		const MorphBasis morph = sliceBasisFor<kMorphTriangles>(0.0, home);
		const StairBasis stair = sliceBasisFor<kStairTriangles>(0.0, home);
		const auto describe = [&](const char* name, const auto& basis, size_t n) {
			size_t degenerate = 0;
			double mean = 0.0;
			for (size_t i = 0; i < n; ++i) {
				const double slope = -static_cast<double>(basis.x[i]);
				degenerate += std::abs(slope + 1.0) < 1e-3 ? 1u : 0u;
				mean += slope;
			}
			mean /= static_cast<double>(n);
			double spread = 0.0;
			for (size_t i = 0; i < n; ++i) {
				spread += std::abs(-static_cast<double>(basis.x[i]) - mean);
			}
			spread /= static_cast<double>(n);
			INFO(name << " at home " << home << ": " << degenerate << " of " << n
			          << " coordinates still at the degenerate value, slope spread " << spread);
			// AT MOST A COINCIDENCE, never a majority: a coordinate may land near −1 by chance now, but
			// the row cannot be pinned there. Before this, morph had 29 of 35 pinned at every home.
			CHECK(degenerate * 4 < n);
			// And no home is a near-degenerate accident.
			CHECK(spread > 0.3);
			return spread;
		};
		meanSpread[0] += describe("morph", morph, kMorphTriangles);
		meanSpread[1] += describe("stair", stair, kStairTriangles);
		++spreadRuns;
	}
	for (int engine = 0; engine < 2; ++engine) {
		meanSpread[engine] /= spreadRuns;
		INFO((engine == 0 ? "morph" : "stair") << " mean slope spread " << meanSpread[engine]);
		CHECK(meanSpread[engine] > 0.4);
	}

	// AND THE GUARANTEES THE OPENING MUST NOT COST, asserted rather than argued.
	SUBCASE("a zero generator is BORN OPEN, by design, and is derived to the bit") {
		// RESTATED, NOT RELAXED, and the design is the reason. This case used to require that a zero
		// generator reproduce the degenerate plane exactly, because the old decode turned a degenerate
		// basis by an angle and a zero angle had to be the identity. Under M there is no angle to be
		// zero: every coordinate is a φ triangle READ AT the rotate, and the design is born-open —
		// M(0) is a plane, not a line, because a sound with no second dimension until somebody turns
		// something is the one-dimensional world this whole stage exists to leave.
		//
		// SO WHAT IS CHECKED HERE IS WHAT THE DESIGN LEFT: that rotate 0 is a genuinely open plane, that
		// it is THE HOME-DERIVED one — a different plane at every place, which is what M(rotate, home)
		// means and what an M of the turn alone could not say — and that deriving it is bit-exact and
		// repeatable, which is the property retracing a breadcrumb actually rests on. The bit-exact
		// DEGENERATE plane did not go anywhere; it moved to `degenerateSliceBasis`, which the
		// firmware-parity cases construct on purpose.
		//
		// AND THIS IS THE MIGRATION'S PROMISE, stated in the only two readings it can honestly carry.
		// "Unrotated patches keep their born-open plane bit for bit" cannot mean "unchanged from the
		// build before this one": the whole decode changed, so every patch with travel or drift sounds
		// different by design and is meant to. What it CAN mean, and what is checked here — a saved
		// never-rotated patch derives exactly the plane a sound freshly made at that home is given, to
		// the bit; and that plane is OPEN rather than the degenerate line.
		const SliceGenerator zero {};
		const MorphBasis reference = degenerateSliceBasis<kMorphTriangles>();
		std::vector<std::array<float, 2 * kMorphTriangles>> bornAt;
		for (const double home : { 0.0, 0.37, 1.9, 6.0, 31.0 }) {
			CAPTURE(home);
			const MorphBasis basis = sliceBasisFor<kMorphTriangles>(sliceRotateOf(zero), home);
			size_t differ = 0;
			bool rateReached = false;
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				differ += basis.x[i] != basis.y[i] ? 1u : 0u;
				rateReached = rateReached || basis.x[kMorphTriangles + i] != 0.0f;
			}
			INFO(differ << " of " << kMorphTriangles << " coordinates lead somewhere different in X and Y");
			CHECK(differ == kMorphTriangles);
			CHECK(rateReached);
			CHECK(std::memcmp(basis.x.data(), reference.x.data(), sizeof(reference.x)) != 0);

			// Bit-exact and repeatable: a patch derives its plane fresh on every load, so it has to land
			// on the same one every time or a breadcrumb lands near where it was rather than on it.
			for (int repeat = 0; repeat < 3; ++repeat) {
				const MorphBasis again = sliceBasisFor<kMorphTriangles>(sliceRotateOf(zero), home);
				CHECK(std::memcmp(again.x.data(), basis.x.data(), sizeof(basis.x)) == 0);
				CHECK(std::memcmp(again.y.data(), basis.y.data(), sizeof(basis.y)) == 0);
			}
			bornAt.push_back(basis.x);
		}

		// AND BORN OPEN AT A PLACE, rather than born on one plane everywhere: no two of these homes may
		// be given the same plane. Without this the subcase would pass on a decode that ignored its home
		// argument entirely — which is the fork this design was chosen against, and one that nothing else
		// in the suite would notice.
		for (size_t a = 0; a < bornAt.size(); ++a) {
			for (size_t b = a + 1; b < bornAt.size(); ++b) {
				INFO("born planes " << a << " and " << b);
				CHECK(std::memcmp(bornAt[a].data(), bornAt[b].data(), sizeof(bornAt[a])) != 0);
			}
		}
	}

	SUBCASE("turning M moves the frequency ladder, and this is the bar the design was chosen on") {
		// BANK TUNING IS RETIRED: with rotation, bank frequency tuning is essentially useless —
		// rotation IS the tuning knob, so what it can do to the ladder is the acceptance
		// measure for the decode rather than a curiosity. The φ frequencies as shipped cluster: 35
		// triangles land in 17 distinct twelfth-octave bins across 2.94 octaves, which is what an ear
		// hears as "lots of morphology at around the same scale".
		//
		// THE BAR, AND THE UNIT IT IS TRUE IN. The prototype promised 30 distinct of 35 and 4.80 octaves
		// across the middle 80%, against 21 and 2.94 for the six-angle decode this replaces. Under
		// M(rotate, home) that promise becomes a DISTRIBUTION OVER PLACES rather than a single number,
		// and the first version of this measurement reported its maximum as though it were its floor —
		// pooled across homes, 4.89 and 32, which no single home delivers. So the bar is asserted three
		// ways, none of them a pooled maximum wearing a floor's clothes: a FLOOR every home must clear,
		// the prototype's own number POOLED across them, and a direct comparison against the decode
		// being replaced. Measured per home at travel 0.9: 4.28 to 4.89 octaves, 29 to 33 distinct.
		//
		// THE COMPARISON IS THE HONEST HEADLINE, because both older numbers are inflated by the guard
		// below: the shipped decode reaches 2.78 to 3.02 octaves with slowest rates of 0.001 to 0.015 at
		// five homes of seven, and the prototype's 4.80 came from a variant whose rate range was ±1.1.
		// The worst home under M beats the best home under the old decode on both axes, with nothing
		// frozen anywhere.
		const std::array<float, kMorphTriangles> freq = morphSlotFrequencies();
		for (const double travel : { 0.35, 0.9 }) {
			double pooledBestOctaves = 0.0;
			size_t pooledBestDistinct = 0;
			for (const double where : { 0.0, 0.37, 1.9, 2.7, 6.0, 11.5, 31.0 }) {
				CAPTURE(where);
				double bestOctaves = 0.0;
				size_t bestDistinct = 0;
				double meanDistinct = 0.0;
				int runs = 0;
				for (int step = 1; step <= 24; ++step) {
					const SliceGenerator generator { static_cast<float>(step) * 0.17f };
					const MorphBasis basis =
					    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), where);
					std::vector<double> effective;
					for (size_t i = 0; i < kMorphTriangles; ++i) {
						effective.push_back(std::max(
						    std::abs(static_cast<double>(freq[i]) * static_cast<double>(basis.rateFor(i, travel, 0.0))),
						    1e-6));
					}
					// AND THE SPAN HAS TO BE A REAL ONE. An effective frequency near zero is a triangle
					// frozen by its rate, which is a legitimate thing for the plane to do — but it would also
					// inflate an octave measure without the ladder having widened, so the metric would start
					// rewarding the wrong thing. None may be a rounding away from zero. (This guard is
					// impl-ui-12's, and it caught a rate spread of 1.2 that bought two extra octaves with a
					// triangle stopped dead. It is the reason the constant is 1.0.)
					CHECK(*std::min_element(effective.begin(), effective.end()) > 1e-3);
					const auto [octaves, distinct] = ladderStats(effective);
					bestOctaves = std::max(bestOctaves, octaves);
					bestDistinct = std::max(bestDistinct, distinct);
					meanDistinct += static_cast<double>(distinct);
					++runs;
				}
				meanDistinct /= runs;
				INFO("at travel " << travel << ", home " << where << ": " << bestDistinct << " distinct of "
				                  << kMorphTriangles << " (mean " << meanDistinct << "), " << bestOctaves
				                  << " octaves across the middle 80%");
				if (travel > 0.5) {
					// THE FLOOR EVERY PLACE CLEARS, and every one of them beats the best the old decode
					// managed anywhere (3.02 octaves, 21 distinct).
					CHECK(bestOctaves >= 4.2);
					CHECK(bestDistinct >= 28);
					CHECK(meanDistinct >= 25.0);
				}
				// At a small travel the ladder must still MOVE — the knob is not a switch that only does
				// something at the far edge of the plane.
				CHECK(bestDistinct >= 22);
				pooledBestOctaves = std::max(pooledBestOctaves, bestOctaves);
				pooledBestDistinct = std::max(pooledBestDistinct, bestDistinct);
			}
			INFO("pooled across homes at travel " << travel << ": " << pooledBestDistinct
			                                      << " distinct, " << pooledBestOctaves << " octaves");
			if (travel > 0.5) {
				// The prototype's own promise, in the unit the prototype measured it in.
				CHECK(pooledBestOctaves >= 4.8);
				CHECK(pooledBestDistinct >= 30);
			}
		}
	}

	SUBCASE("a sound that has not travelled cannot be moved by any orientation") {
		// Rotate-inaudible-until-travelled, which the opening leaves untouched because the decode
		// multiplies every basis component by the travel. This is the original design property
		// (the sound stays the same unless you move in xy/gamma) and it is worth
		// re-asserting HERE, at the moment the basis got thirty-five times more to say.
		const MorphBasis wild = sliceBasisFor<kMorphTriangles>(1.83, 2.7);
		const owlet::dsp::PhiMorphWavetable turned =
		    buildMorphSliced(512, 0.25f, 0.0, 0.0, wild, 0.0);
		const owlet::dsp::PhiMorphWavetable plain =
		    owlet::dsp::buildPhiMorphWavetable(512, 0.25f);
		CHECK(std::memcmp(&turned, &plain, sizeof(turned)) == 0);
	}
}

TEST_CASE("MEASURE: how the phi frequency ladder is spread, per bank" * doctest::skip()) {
	// The question: are the phi frequencies drawn too close together, so that most morphology appears at
	// about the same scale?
	//
	// A LADDER'S JOB IS TO COVER SCALES. If the frequencies span many octaves the picture has coarse
	// shapes AND fine detail; if they cluster, everything varies at once and at the same rate, which is
	// exactly what an eye reads as "morphology at around the same scale". This measures the spread from
	// the banks themselves — no recollection, and reported in OCTAVES because that is the unit scale
	// perception actually works in.
	//
	// Context from the record, so the answer is read correctly either way: the banks were shipped
	// WITHOUT a tuning pass (the PHI_WEAVE-era note — untuned, benchmark pending), so clustering here
	// would be that debt surfacing rather than a new fault.
	std::array<float, kMorphTriangles> morphFreq {};
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMaxSegments); ++i) {
		morphFreq[morph_slot::kPhaseDelta + i] = owlet::dsp::kPhiMorphPhaseBank[i].phiFreq;
		morphFreq[morph_slot::kCurvature + i] = owlet::dsp::kPhiMorphCurvatureBank[i].phiFreq;
	}
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMovableWaypoints); ++i) {
		morphFreq[morph_slot::kAmplitude + i] = owlet::dsp::kPhiMorphAmplitudeBank[i].phiFreq;
	}
	morphFreq[morph_slot::kGain] = owlet::dsp::kPhiMorphGainBank[0].phiFreq;
	morphFreq[morph_slot::kEndpoint] = owlet::dsp::kPhiMorphEndpointBank[0].phiFreq;
	morphFreq[morph_slot::kAmpOvershoot] = owlet::dsp::kPhiMorphAmpOvershootBank[0].phiFreq;
	morphFreq[morph_slot::kCurvBoost] = owlet::dsp::kPhiMorphCurvBoostBank[0].phiFreq;
	morphFreq[morph_slot::kPhaseDistort] = owlet::dsp::kPhiMorphPhaseDistortBank[0].phiFreq;
	morphFreq[morph_slot::kSineBlend] = owlet::dsp::kPhiMorphSineBlendBank[0].phiFreq;
	morphFreq[morph_slot::kOddSym] = owlet::dsp::kPhiMorphOddSymBank[0].phiFreq;
	morphFreq[morph_slot::kWindow] = owlet::dsp::kPhiMorphWindowBank[0].phiFreq;
	morphFreq[morph_slot::kSlopeMatch] = owlet::dsp::kPhiMorphSlopeMatchBank[0].phiFreq;
	morphFreq[morph_slot::kPhaseJitter] = owlet::dsp::kPhiMorphPhaseJitterBank[0].phiFreq;
	morphFreq[morph_slot::kAmpNoise] = owlet::dsp::kPhiMorphAmpNoiseBank[0].phiFreq;
	morphFreq[morph_slot::kAsymGain] = owlet::dsp::kPhiMorphAsymGainBank[0].phiFreq;

	std::array<float, kStairTriangles> stairFreq {};
	stairFreq[stair_slot::kCount] = owlet::dsp::kPhiStairCount.phiFreq;
	stairFreq[stair_slot::kSlope] = owlet::dsp::kPhiStairSlope.phiFreq;
	stairFreq[stair_slot::kTilt] = owlet::dsp::kPhiStairTilt.phiFreq;
	stairFreq[stair_slot::kAsym] = owlet::dsp::kPhiStairAsym.phiFreq;
	stairFreq[stair_slot::kLandCycles] = owlet::dsp::kPhiStairLandCycles.phiFreq;
	stairFreq[stair_slot::kHeightLand] = owlet::dsp::kPhiStairHeightLand.phiFreq;
	stairFreq[stair_slot::kAsymLand] = owlet::dsp::kPhiStairAsymLand.phiFreq;
	stairFreq[stair_slot::kGlyph] = 7.0f * owlet::dsp::phi::kPhi300;
	stairFreq[stair_slot::kShard] = 3.0f * owlet::dsp::phi::kPhi200;

	const auto report = [](const char* name, std::vector<double> f) {
		std::sort(f.begin(), f.end());
		const double octaves = std::log2(f.back() / f.front());
		// Where the BULK sits, which is what an eye reads: the middle 80%.
		const size_t lo = f.size() / 10;
		const size_t hi = f.size() - 1 - f.size() / 10;
		const double bulk = std::log2(f[hi] / f[lo]);
		std::string ladder;
		for (const double v : f) {
			char one[24];
			std::snprintf(one, sizeof(one), "%.2f ", v);
			ladder += one;
		}
		MESSAGE(name << ": " << f.size() << " triangles, " << f.front() << " .. " << f.back()
		             << " cycles/home = " << octaves << " OCTAVES total, middle 80% spans " << bulk
		             << " octaves");
		MESSAGE(name << " ladder: " << ladder);
	};
	report("morph", std::vector<double>(morphFreq.begin(), morphFreq.end()));
	report("stair", std::vector<double>(stairFreq.begin(), stairFreq.end()));

	// AND WITH TRAVEL, because the rate rows multiply these — the question is whether MOVING spreads
	// the ladder or leaves it where it is. Measured on the opened plane that now ships.
	for (const double travel : { 0.0, 0.5, 1.5 }) {
		const MorphBasis basis = sliceBasisFor<kMorphTriangles>(0.37, 0.37);
		std::vector<double> effective;
		for (size_t i = 0; i < kMorphTriangles; ++i) {
			effective.push_back(std::abs(static_cast<double>(morphFreq[i])
			                             * static_cast<double>(basis.rateFor(i, travel, 0.0))));
		}
		char label[64];
		std::snprintf(label, sizeof(label), "morph at travel %.1f", travel);
		report(label, effective);
	}
}

TEST_CASE("MEASURE: does rotation split the duplicated frequencies" * doctest::skip()) {
	// THE CONTENTION: with rotation, bank frequency tuning is essentially useless.
	//
	// IT IS FALSIFIABLE AND THE EARLIER MEASUREMENT DID NOT TEST IT. That one swept TRAVEL at a fixed
	// rotation and found the octave span stretching at the tails only. But the ladder's real problem is
	// not span, it is DUPLICATION — 35 morph triangles carry only 17 distinct frequencies, three of
	// them sitting at 0.79 and three at 5.39. A rotation changes which rate coefficient multiplies
	// which rung, so three triangles sharing a rung can come out at three different effective
	// frequencies. If that happens the clustering dissolves without touching a bank, which is exactly
	// what the contention claims.
	//
	// So: effective ladders across a ROTATION sweep, at modest travel, on the plane that ships.
	std::array<float, kMorphTriangles> freq {};
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMaxSegments); ++i) {
		freq[morph_slot::kPhaseDelta + i] = owlet::dsp::kPhiMorphPhaseBank[i].phiFreq;
		freq[morph_slot::kCurvature + i] = owlet::dsp::kPhiMorphCurvatureBank[i].phiFreq;
	}
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMovableWaypoints); ++i) {
		freq[morph_slot::kAmplitude + i] = owlet::dsp::kPhiMorphAmplitudeBank[i].phiFreq;
	}
	freq[morph_slot::kGain] = owlet::dsp::kPhiMorphGainBank[0].phiFreq;
	freq[morph_slot::kEndpoint] = owlet::dsp::kPhiMorphEndpointBank[0].phiFreq;
	freq[morph_slot::kAmpOvershoot] = owlet::dsp::kPhiMorphAmpOvershootBank[0].phiFreq;
	freq[morph_slot::kCurvBoost] = owlet::dsp::kPhiMorphCurvBoostBank[0].phiFreq;
	freq[morph_slot::kPhaseDistort] = owlet::dsp::kPhiMorphPhaseDistortBank[0].phiFreq;
	freq[morph_slot::kSineBlend] = owlet::dsp::kPhiMorphSineBlendBank[0].phiFreq;
	freq[morph_slot::kOddSym] = owlet::dsp::kPhiMorphOddSymBank[0].phiFreq;
	freq[morph_slot::kWindow] = owlet::dsp::kPhiMorphWindowBank[0].phiFreq;
	freq[morph_slot::kSlopeMatch] = owlet::dsp::kPhiMorphSlopeMatchBank[0].phiFreq;
	freq[morph_slot::kPhaseJitter] = owlet::dsp::kPhiMorphPhaseJitterBank[0].phiFreq;
	freq[morph_slot::kAmpNoise] = owlet::dsp::kPhiMorphAmpNoiseBank[0].phiFreq;
	freq[morph_slot::kAsymGain] = owlet::dsp::kPhiMorphAsymGainBank[0].phiFreq;

	// Distinct at a TWELFTH OF AN OCTAVE, which is about the finest spacing worth calling two different
	// scales rather than one scale slightly detuned.
	const auto stats = [](std::vector<double> f) {
		std::sort(f.begin(), f.end());
		std::set<long> bins;
		for (const double v : f) {
			bins.insert(std::lround(std::log2(v) * 12.0));
		}
		const size_t lo = f.size() / 10;
		const size_t hi = f.size() - 1 - f.size() / 10;
		return std::pair<double, size_t> { std::log2(f[hi] / f[lo]), bins.size() };
	};

	const SliceGenerator base { 0.37f };
	for (const double travel : { 0.0, 0.35, 0.9 }) {
		double worstSpread = 1e9;
		double bestSpread = 0.0;
		size_t worstDistinct = 999;
		size_t bestDistinct = 0;
		double meanDistinct = 0.0;
		int runs = 0;
		// The sweep: turns of increasing size, which is what the rotate corner sends.
		for (int step = 0; step < 24; ++step) {
			SliceRotation delta;
			delta.rotate = static_cast<float>(step) * 0.13f;
			const MorphBasis basis =
			    sliceBasisFor<kMorphTriangles>(sliceRotateOf(rotateBy(base, delta)), 0.37);
			std::vector<double> effective;
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				const double f = std::abs(static_cast<double>(freq[i])
				                          * static_cast<double>(basis.rateFor(i, travel, 0.0)));
				effective.push_back(std::max(f, 1e-6));
			}
			const auto s = stats(effective);
			worstSpread = std::min(worstSpread, s.first);
			bestSpread = std::max(bestSpread, s.first);
			worstDistinct = std::min(worstDistinct, s.second);
			bestDistinct = std::max(bestDistinct, s.second);
			meanDistinct += static_cast<double>(s.second);
			++runs;
		}
		MESSAGE("travel " << travel << " over " << runs
		                  << " rotations: middle-80% octaves " << worstSpread << ".." << bestSpread
		                  << ", distinct frequencies (1/12 octave bins) " << worstDistinct << ".."
		                  << bestDistinct << ", mean " << meanDistinct / runs << " of "
		                  << kMorphTriangles);
	}
}

TEST_CASE("MEASURE: a rate-only variant of rotation, against the mixed one that ships"
          * doctest::skip()) {
	// THE QUESTION: could rotation impact ONLY frequency, and would the central point then remain
	// constant?
	//
	// THE CENTRAL POINT IS SAFE WHATEVER THE ANSWER, and that is worth settling first: `phaseFor` is
	// `home + (residue + travelTerm)` and every travelTerm is multiplied by the travel, so at zero
	// travel and zero residue the sound is `home` bit for bit no matter what any basis says. The
	// invariant is a property of the DECODE, not of the rotation, so it cannot be broken by changing
	// what rotation does. (Cased already: a wild orientation memcmps against the firmware builder.)
	//
	// THE MATH DOES NOT WORK OUT FOR A ROTATION, and here is why in one line: the degenerate basis has
	// every phase coefficient 1 and EVERY RATE COEFFICIENT 0. A rotation confined to rate-rate pairs
	// turns (0, 0) into (0, 0) — it does exactly nothing. Rate variation can only be created by mixing
	// a PHASE coefficient into a rate one, and an orthogonal rotation that does so necessarily shrinks
	// the phase coefficient by cos θ: the two halves are the same turn seen from two sides. So
	// "rotation only impacts frequency" is not reachable by rotating; it needs a SHEAR — rate gets a
	// multiple of phase while phase is left alone — which is a different animal with different
	// promises (see the note).
	//
	// WHAT THIS MEASURES is whether the shear is a BETTER TUNING KNOB than the mixed rotation: does it
	// reach the ladder's bulk, which today's rotation does not?
	std::array<float, kMorphTriangles> freq {};
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMaxSegments); ++i) {
		freq[morph_slot::kPhaseDelta + i] = owlet::dsp::kPhiMorphPhaseBank[i].phiFreq;
		freq[morph_slot::kCurvature + i] = owlet::dsp::kPhiMorphCurvatureBank[i].phiFreq;
	}
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMovableWaypoints); ++i) {
		freq[morph_slot::kAmplitude + i] = owlet::dsp::kPhiMorphAmplitudeBank[i].phiFreq;
	}
	freq[morph_slot::kGain] = owlet::dsp::kPhiMorphGainBank[0].phiFreq;
	freq[morph_slot::kEndpoint] = owlet::dsp::kPhiMorphEndpointBank[0].phiFreq;
	freq[morph_slot::kAmpOvershoot] = owlet::dsp::kPhiMorphAmpOvershootBank[0].phiFreq;
	freq[morph_slot::kCurvBoost] = owlet::dsp::kPhiMorphCurvBoostBank[0].phiFreq;
	freq[morph_slot::kPhaseDistort] = owlet::dsp::kPhiMorphPhaseDistortBank[0].phiFreq;
	freq[morph_slot::kSineBlend] = owlet::dsp::kPhiMorphSineBlendBank[0].phiFreq;
	freq[morph_slot::kOddSym] = owlet::dsp::kPhiMorphOddSymBank[0].phiFreq;
	freq[morph_slot::kWindow] = owlet::dsp::kPhiMorphWindowBank[0].phiFreq;
	freq[morph_slot::kSlopeMatch] = owlet::dsp::kPhiMorphSlopeMatchBank[0].phiFreq;
	freq[morph_slot::kPhaseJitter] = owlet::dsp::kPhiMorphPhaseJitterBank[0].phiFreq;
	freq[morph_slot::kAmpNoise] = owlet::dsp::kPhiMorphAmpNoiseBank[0].phiFreq;
	freq[morph_slot::kAsymGain] = owlet::dsp::kPhiMorphAsymGainBank[0].phiFreq;

	const auto stats = [](std::vector<double> f) {
		std::sort(f.begin(), f.end());
		std::set<long> bins;
		for (const double v : f) {
			bins.insert(std::lround(std::log2(v) * 12.0));
		}
		const size_t lo = f.size() / 10;
		const size_t hi = f.size() - 1 - f.size() / 10;
		return std::pair<double, size_t> { std::log2(f[hi] / f[lo]), bins.size() };
	};

	// A ROTATION CONFINED TO THE RATE ROWS, and the first version of this measurement asserted it was a
	// no-op. IT IS NOT, and the correction is the most useful thing here. On the DEGENERATE basis the
	// rate coefficients are all zero, so rotating rate against rate turns (0,0) into (0,0) — that much
	// is true and is why "rotation only impacts frequency" cannot BUILD rate variation from nothing.
	// But a real basis is not degenerate: M writes the rate rows populated (in this measurement's day
	// it was the aperture that opened them). Rotating them against each other then REDISTRIBUTES what
	// is there — which triangle carries which rate — while leaving every phase coefficient exactly
	// alone. Measured below at 15.9 total movement, not zero.
	//
	// So that world exists, with one precondition worth stating: rotation-only-frequency retunes an
	// ALREADY-OPEN plane and cannot open one. Under M every plane arrives open, so the precondition
	// is satisfied by construction now.
	for (const double travel : { 0.35, 0.9 }) {
		const MorphBasis shipped = sliceBasisFor<kMorphTriangles>(0.37, 0.37);
		double bestSpread = 0.0;
		size_t bestDistinct = 0;
		double meanDistinct = 0.0;
		double phaseMoved = 0.0;
		int runs = 0;
		for (int step = 0; step < 16; ++step) {
			MorphBasis turned = shipped;
			const float angle = static_cast<float>(step) * 0.21f;
			const float c = std::cos(angle);
			const float sn = std::sin(angle);
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				const size_t a = kMorphTriangles + i;
				const size_t b = kMorphTriangles + ((i * 7 + 3) % kMorphTriangles);
				const float xa = turned.x[a];
				const float xb = turned.x[b];
				turned.x[a] = c * xa - sn * xb;
				turned.x[b] = sn * xa + c * xb;
			}
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				phaseMoved += std::abs(static_cast<double>(turned.x[i] - shipped.x[i]));
			}
			std::vector<double> effective;
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				effective.push_back(std::max(std::abs(static_cast<double>(freq[i])
				                                      * static_cast<double>(turned.rateFor(i, travel, 0.0))),
				                             1e-6));
			}
			const auto st = stats(effective);
			bestSpread = std::max(bestSpread, st.first);
			bestDistinct = std::max(bestDistinct, st.second);
			meanDistinct += static_cast<double>(st.second);
			++runs;
		}
		MESSAGE("rate-only rotation at travel " << travel << ": best middle-80% " << bestSpread
		                                        << " octaves, distinct up to " << bestDistinct
		                                        << " (mean " << meanDistinct / runs << ") of "
		                                        << kMorphTriangles << "; phase row moved by "
		                                        << phaseMoved << " in total across the sweep");
	}

	// THEN: the shear — rate gets a multiple of phase, phase untouched — swept against the ladder.
	for (const double travel : { 0.35, 0.9 }) {
		const MorphBasis shipped = sliceBasisFor<kMorphTriangles>(0.37, 0.37);
		double bestSpread = 0.0;
		size_t bestDistinct = 0;
		double meanDistinct = 0.0;
		int runs = 0;
		for (int step = 1; step <= 12; ++step) {
			const double shear = static_cast<double>(step) * 0.12;
			std::vector<double> effective;
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				// The rate this triangle would get if the shear put a multiple of a DIFFERENT
				// triangle's phase coefficient into it, leaving every phase coefficient alone.
				const size_t from = (i * 7 + 3) % kMorphTriangles;
				const double rate = 1.0 + shear * static_cast<double>(shipped.x[from]) * travel;
				effective.push_back(std::max(std::abs(static_cast<double>(freq[i]) * rate), 1e-6));
			}
			const auto s = stats(effective);
			bestSpread = std::max(bestSpread, s.first);
			bestDistinct = std::max(bestDistinct, s.second);
			meanDistinct += static_cast<double>(s.second);
			++runs;
		}
		MESSAGE("shear at travel " << travel << ": best middle-80% " << bestSpread
		                           << " octaves, distinct up to " << bestDistinct << " (mean "
		                           << meanDistinct / runs << ") of " << kMorphTriangles);
	}
}

TEST_CASE("MEASURE: a prototype M(rotate) against the stack it would replace" * doctest::skip()) {
	// The proposal: the rotation stack may be more complex than it needs to be. Rotation could simply be
	// (x,y)*M(rotate), where M is decoded phi triangles feeding every contribution of x,y to every phi
	// phase and frequency multiplier.
	//
	// THE CLAIM TO VERIFY IS "SIMPLER AND AT LEAST AS GOOD", so this prototypes M and runs it through
	// the same instruments tonight's designs were judged by. M here is: every coordinate of both rows
	// gets its own phi triangle READ AT THE ROTATE VALUE, with the degenerate basis as the r=0 anchor.
	//
	// THE ANCHOR IS THE MIGRATION GUARANTEE and it is worth stating as arithmetic: each entry is
	// `tri(r*f + o) - tri(o)`, which is EXACTLY zero at r = 0 for every coordinate, so M(0) is the
	// degenerate plane bit for bit rather than approximately. No special case, no early-out.
	std::array<float, kMorphTriangles> freq {};
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMaxSegments); ++i) {
		freq[morph_slot::kPhaseDelta + i] = owlet::dsp::kPhiMorphPhaseBank[i].phiFreq;
		freq[morph_slot::kCurvature + i] = owlet::dsp::kPhiMorphCurvatureBank[i].phiFreq;
	}
	for (size_t i = 0; i < static_cast<size_t>(owlet::dsp::kPhiMorphMovableWaypoints); ++i) {
		freq[morph_slot::kAmplitude + i] = owlet::dsp::kPhiMorphAmplitudeBank[i].phiFreq;
	}
	freq[morph_slot::kGain] = owlet::dsp::kPhiMorphGainBank[0].phiFreq;
	freq[morph_slot::kEndpoint] = owlet::dsp::kPhiMorphEndpointBank[0].phiFreq;
	freq[morph_slot::kAmpOvershoot] = owlet::dsp::kPhiMorphAmpOvershootBank[0].phiFreq;
	freq[morph_slot::kCurvBoost] = owlet::dsp::kPhiMorphCurvBoostBank[0].phiFreq;
	freq[morph_slot::kPhaseDistort] = owlet::dsp::kPhiMorphPhaseDistortBank[0].phiFreq;
	freq[morph_slot::kSineBlend] = owlet::dsp::kPhiMorphSineBlendBank[0].phiFreq;
	freq[morph_slot::kOddSym] = owlet::dsp::kPhiMorphOddSymBank[0].phiFreq;
	freq[morph_slot::kWindow] = owlet::dsp::kPhiMorphWindowBank[0].phiFreq;
	freq[morph_slot::kSlopeMatch] = owlet::dsp::kPhiMorphSlopeMatchBank[0].phiFreq;
	freq[morph_slot::kPhaseJitter] = owlet::dsp::kPhiMorphPhaseJitterBank[0].phiFreq;
	freq[morph_slot::kAmpNoise] = owlet::dsp::kPhiMorphAmpNoiseBank[0].phiFreq;
	freq[morph_slot::kAsymGain] = owlet::dsp::kPhiMorphAsymGainBank[0].phiFreq;

	// M(rotate): one scalar in, 2N coefficients out. phi rates per coordinate so no two turn together.
	const auto M = [](double rotate, size_t i, size_t /*n*/, bool rateRow) {
		const double rate = std::pow(0.6180339887498949, 1.0 + static_cast<double>(i % 7));
		const double offset = static_cast<double>(i) * 0.6180339887498949;
		const auto tri = [](double at) {
			const double w = at - std::floor(at);
			return w < 0.5 ? 4.0 * w - 1.0 : 3.0 - 4.0 * w; // plain triangle, -1..1
		};
		const double amount = 0.55 * (tri(rotate * (rateRow ? 1.7 : 1.0) * rate + offset) - tri(offset));
		return (rateRow ? 0.0 : 1.0) + amount;
	};

	const auto stats = [](std::vector<double> f) {
		std::sort(f.begin(), f.end());
		std::set<long> bins;
		for (const double v : f) {
			bins.insert(std::lround(std::log2(v) * 12.0));
		}
		const size_t lo = f.size() / 10;
		const size_t hi = f.size() - 1 - f.size() / 10;
		return std::pair<double, size_t> { std::log2(f[hi] / f[lo]), bins.size() };
	};

	// The r = 0 anchor, asserted rather than hoped.
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		REQUIRE(M(0.0, i, kMorphTriangles, false) == 1.0);
		REQUIRE(M(0.0, i, kMorphTriangles, true) == 0.0);
	}

	for (const double travel : { 0.35, 0.9 }) {
		double bestSpread = 0.0;
		size_t bestDistinct = 0;
		double meanDistinct = 0.0;
		double phaseSpreadBest = 0.0;
		int runs = 0;
		for (int step = 1; step <= 24; ++step) {
			const double rotate = static_cast<double>(step) * 0.17;
			std::vector<double> effective;
			double mean = 0.0;
			std::vector<double> slopes;
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				const double rateCoefficient = M(rotate, i, kMorphTriangles, true);
				const double phaseCoefficient = M(rotate, i, kMorphTriangles, false);
				effective.push_back(std::max(
				    std::abs(static_cast<double>(freq[i]) * (1.0 + rateCoefficient * travel)), 1e-6));
				slopes.push_back(-phaseCoefficient);
				mean += -phaseCoefficient;
			}
			mean /= static_cast<double>(kMorphTriangles);
			double phaseSpread = 0.0;
			for (const double s : slopes) {
				phaseSpread += std::abs(s - mean);
			}
			phaseSpread /= static_cast<double>(kMorphTriangles);
			phaseSpreadBest = std::max(phaseSpreadBest, phaseSpread);
			const auto st = stats(effective);
			bestSpread = std::max(bestSpread, st.first);
			bestDistinct = std::max(bestDistinct, st.second);
			meanDistinct += static_cast<double>(st.second);
			++runs;
		}
		MESSAGE("M(rotate) at travel " << travel << ": best middle-80% " << bestSpread
		                              << " octaves, distinct up to " << bestDistinct << " (mean "
		                              << meanDistinct / runs << ") of " << kMorphTriangles
		                              << "; best phase-slope spread " << phaseSpreadBest);
	}
}

TEST_CASE("vox pulse counts are fractional: the last pulse fades in where an integer count pops") {
	// The firmware's own hard-won detail (its comment: the integer step WAS an audible discontinuity
	// at specific zone positions), pinned with its known-bad alongside: at a bisected integer
	// crossing of the pulse count, the shipping table moves by dust while a floored-count variant of
	// the same arithmetic jumps by a whole pulse. The known-bad is what makes the small number mean
	// something — without it, "continuous" is a threshold nobody has to defend.
	using owlet::dsp::phi::evalTriangle;
	const uint16_t zone = 300;
	const auto n1At = [&](double gamma) {
		return 1.0f + evalTriangle(sliceHome(zone, 0.0f, gamma), 1.0f, owlet::dsp::kPhiVoxN1) * 11.0f;
	};

	// Find an integer crossing of formant 0's count along gamma, then bisect it tight.
	double lo = 0.0;
	double hi = 0.0;
	for (double g = 0.0; g < 2.0; g += 1e-3) {
		if (static_cast<int>(n1At(g)) != static_cast<int>(n1At(g + 1e-3))) {
			lo = g;
			hi = g + 1e-3;
			break;
		}
	}
	REQUIRE(hi > lo); // no crossing in two whole gamma units would mean the bank is not evaluating
	for (int i = 0; i < 60; ++i) {
		const double mid = 0.5 * (lo + hi);
		(static_cast<int>(n1At(mid)) == static_cast<int>(n1At(lo)) ? lo : hi) = mid;
	}

	const auto tableAt = [&](double gamma, bool fractional) {
		const double home = sliceHome(zone, 0.0f, gamma);
		float count = std::min(1.0f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxN1) * 11.0f,
		                       static_cast<float>(kVoxMaxPulses));
		if (!fractional) {
			count = static_cast<float>(static_cast<int32_t>(count)); // the known-bad: floored
		}
		const float decay = 0.30f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxDecay1) * 0.88f;
		const float pol = evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxPolarity1);
		std::array<float, kVoxMaxPulses> table {};
		int32_t nFull = static_cast<int32_t>(count);
		float partial = count - static_cast<float>(nFull);
		float mag = 1.0f;
		float peak = 0.0f;
		for (int32_t k = 0; k < nFull + (partial > 0.0f ? 1 : 0) && k < static_cast<int32_t>(kVoxMaxPulses);
		     k++) {
			float sign = 1.0f - 2.0f * static_cast<float>(k & 1) * pol;
			float frac = (k < nFull) ? 1.0f : partial;
			table[static_cast<size_t>(k)] = mag * sign * frac;
			peak = std::max(peak, std::abs(mag) * frac);
			mag *= decay;
		}
		float norm = (peak > 0.0001f) ? 0.62f / peak : 0.0f;
		for (float& g : table) {
			g *= norm;
		}
		return table;
	};
	const auto tableGap = [](const std::array<float, kVoxMaxPulses>& a,
	                         const std::array<float, kVoxMaxPulses>& b) {
		float gap = 0.0f;
		for (size_t k = 0; k < kVoxMaxPulses; ++k) {
			gap = std::max(gap, std::abs(a[k] - b[k]));
		}
		return gap;
	};

	const double before = lo - 1e-6;
	const double after = hi + 1e-6;
	const VoxBasis basis = degenerateSliceBasis<kVoxTriangles>();

	// The shipping build steps by dust across the crossing; the floored variant of the SAME
	// arithmetic pops by a whole pulse. And the test-local fractional table is the builder's own,
	// bit for bit, so the known-bad is a variant of what ships rather than of a strawman.
	std::array<float, kVoxMaxPulses> shippedBefore = buildVoxSliced(zone, 0.0f, 0.0, 0.0, basis, before)
	                                                     .formant[0]
	                                                     .pulseGain;
	std::array<float, kVoxMaxPulses> shippedAfter = buildVoxSliced(zone, 0.0f, 0.0, 0.0, basis, after)
	                                                    .formant[0]
	                                                    .pulseGain;
	CHECK(tableGap(shippedBefore, tableAt(before, true)) == 0.0f);
	CHECK(tableGap(shippedAfter, tableAt(after, true)) == 0.0f);
	CHECK(tableGap(shippedBefore, shippedAfter) < 1e-3f);
	CHECK(tableGap(tableAt(before, false), tableAt(after, false)) > 0.02f);

	// The pulse born at the crossing enters at (nearly) nothing — the fade-in itself.
	const int nFullAfter = static_cast<int>(std::min(n1At(after), static_cast<float>(kVoxMaxPulses)));
	if (n1At(after) > n1At(before)) { // the crossing gained a pulse (a falling slope loses one)
		CHECK(std::abs(shippedAfter[static_cast<size_t>(nFullAfter)]) < 1e-3f);
	}
}

TEST_CASE("vox decay above one grows the burst, and the normalised peak is the firmware's gain") {
	// Decay maps 0.30..1.18, and the values above one are a CHARACTER — the growing "brass rasp"
	// burst the header advertises — not an overflow to clamp. Where decay exceeds one the pulse
	// magnitudes must actually grow to the last full pulse, and peak normalisation must land the
	// loudest pulse exactly on the 0.62 formant gain however the burst is shaped.
	using owlet::dsp::phi::evalTriangle;
	uint16_t foundZone = 0;
	double foundGamma = 0.0;
	int foundFull = 0;
	bool found = false;
	for (int step = 0; step < 5 && !found; ++step) {
		const double gamma = static_cast<double>(step) * 0.5;
		for (int zone = 0; zone <= 1023 && !found; ++zone) {
			const double home = sliceHome(static_cast<uint16_t>(zone), 0.0f, gamma);
			const float b1 = 0.30f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxDecay1) * 0.88f;
			const float n1 = 1.0f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxN1) * 11.0f;
			if (b1 > 1.05f && n1 >= 4.0f) {
				foundZone = static_cast<uint16_t>(zone);
				foundGamma = gamma;
				foundFull = static_cast<int>(std::min(n1, static_cast<float>(kVoxMaxPulses)));
				found = true;
			}
		}
	}
	REQUIRE(found); // growing-decay country must exist, or the 0.30..1.18 map has drifted

	const VoxParams p = buildVoxSliced(foundZone, 0.0f, 0.0, 0.0,
	                                   degenerateSliceBasis<kVoxTriangles>(), foundGamma);
	for (int k = 0; k + 1 < foundFull; ++k) {
		CHECK(std::abs(p.formant[0].pulseGain[static_cast<size_t>(k + 1)])
		      > std::abs(p.formant[0].pulseGain[static_cast<size_t>(k)]));
	}
	float peak = 0.0f;
	for (const float g : p.formant[0].pulseGain) {
		peak = std::max(peak, std::abs(g));
	}
	CHECK(peak == doctest::Approx(0.62f).epsilon(1e-6));
}

// ------------------------------------------------------------------------------------------------
// THE ADDITIVE ENGINE'S TERRAIN (design 2026-10-08 §2; plan Task 6). The references below are
// restatements written again here: the field bank from the design's ladder and duty table, a field's
// value as the triangle at the coordinate the row should carry it to.
// ------------------------------------------------------------------------------------------------

namespace {

namespace phi = owlet::dsp::phi;

/// The field bank as the design states it, restated (Task 6 fix round): Smoothness is V5's field itself;
/// the other twelve sit on V5's band at kFlagshipQuantizeRate · φ^(−0.15 · rank), the slowest ranks the
/// largest regions; their origin fractions and duties per the bank's tables, restated here by name.
phi::PhiTriConfig restatedPartialField(size_t row) {
	if (row == partial_slot::kSmoothness) {
		return kFlagshipSmoothField;
	}
	struct Entry {
		size_t row;
		int rank;
		float origin;
		float duty;
	};
	constexpr Entry kEntries[] = {
		{ partial_slot::kRolloff, 11, 0.25f, 1.0f },        { partial_slot::kFormant1Centre, 10, 0.62f, 1.0f },
		{ partial_slot::kFormant2Centre, 9, 0.08f, 1.0f },  { partial_slot::kDetail, 8, 0.81f, 1.0f },
		{ partial_slot::kOffGrid, 7, 0.45f, 0.35f },        { partial_slot::kFormant1Gain, 6, 0.95f, 0.45f },
		{ partial_slot::kFormant2Gain, 5, 0.60f, 0.45f },   { partial_slot::kOddEven, 4, 0.95f, 0.35f },
		{ partial_slot::kCombSpacing, 3, 0.37f, 1.0f },     { partial_slot::kCombDepth, 2, 0.25f, 0.20f },
		{ partial_slot::kFormant1Width, 1, 0.91f, 1.0f },   { partial_slot::kFormant2Width, 0, 0.17f, 1.0f },
	};
	for (const Entry& e : kEntries) {
		if (e.row == row) {
			const auto rate = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate)
			                                     * std::pow(std::numbers::phi, -0.15 * e.rank));
			return { rate, e.duty, e.origin / rate, false };
		}
	}
	return {};
}

/// What one shape field reads when its row's phase is `phase` and its rate multiplier `rate`: the
/// unipolar triangle of its config, in evalTriangle's own order of operations.
float restatedField(size_t row, double phase, float rate = 1.0f) {
	const phi::PhiTriConfig cfg = restatedPartialField(row);
	return owlet::dsp::triangleSimpleUnipolar(phi::wrapPhase((phase + cfg.phaseOffset) * cfg.phiFreq * rate),
	                                           cfg.duty);
}

/// The fields at per-row phases and rates, in partial_slot order.
partials::PartialFields restatedFields(const std::array<double, partial_slot::kFieldCount>& phase,
                                       const std::array<float, partial_slot::kFieldCount>& rate) {
	partials::PartialFields f;
	float* const members[] = { &f.rolloff,       &f.formant1Centre, &f.formant1Width, &f.formant1Gain,
		                       &f.formant2Centre, &f.formant2Width, &f.formant2Gain,  &f.oddEven,
		                       &f.combSpacing,    &f.combDepth,     &f.detail,        &f.offGrid,
		                       &f.smoothness };
	for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
		*members[r] = restatedField(r, phase[r], rate[r]);
	}
	// PAL STAGE 1'S GUESTS (design §3.7), restated from the design's words: each read at its HOST row's phase and rate, through its
	// own triangle on V5's band at rank r, its duty, and its origin fraction.
	struct Guest {
		float partials::PartialFields::* member;
		size_t host;
		double rank;
		float duty;
		float origin;
	};
	const Guest guests[] = {
		{ &partials::PartialFields::knee, partial_slot::kCombSpacing, 7.5, 0.35f, 0.90f },
		{ &partials::PartialFields::shelf, partial_slot::kFormant1Width, 4.5, 0.35f, 0.85f },
		{ &partials::PartialFields::shelfAt, partial_slot::kFormant2Width, 9.5, 1.0f, 0.40f },
		{ &partials::PartialFields::body, partial_slot::kFormant2Centre, 6.5, 0.30f, 0.70f },
		{ &partials::PartialFields::bodyAt, partial_slot::kFormant1Centre, 10.5, 1.0f, 0.30f },
		{ &partials::PartialFields::sparsity, partial_slot::kDetail, 8.5, 0.30f, 0.60f },
		{ &partials::PartialFields::sparseSet, partial_slot::kRolloff, 11.5, 1.0f, 0.15f },
	};
	for (const Guest& g : guests) {
		const auto r = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate) * std::pow(std::numbers::phi, -0.15 * g.rank));
		f.*g.member = owlet::dsp::triangleSimpleUnipolar(
		    phi::wrapPhase((phase[g.host] + g.origin / r) * r * rate[g.host]), g.duty);
	}
	return f;
}

partials::PartialFields restatedFields(const std::array<double, partial_slot::kFieldCount>& phase) {
	std::array<float, partial_slot::kFieldCount> one {};
	one.fill(1.0f);
	return restatedFields(phase, one);
}

/// Harmonic k's detail triangle read at `phase` and rate multiplier `rate`, through the config the
/// formula states.
float restatedDetail(int k, double phase, float rate = 1.0f) {
	const double own = static_cast<double>(kFlagshipFmWidthRate / phi::kPhi050 * phi::kPhi400)
	                   * std::pow(std::numbers::phi, 0.5 * std::log2(static_cast<double>(k)));
	const double periods = static_cast<double>(k) * std::numbers::phi * std::numbers::phi;
	const auto offset = static_cast<float>((periods - std::floor(periods)) / own);
	return owlet::dsp::triangleFloat(phi::wrapPhase((phase + offset) * static_cast<float>(own) * rate), 1.0f);
}

/// A plane on which only row `row`'s PHASE moves, one for one with travel X; every other row stands still.
PartialBasis onlyRowMoves(size_t row) {
	PartialBasis basis {};
	basis.x[row] = 1.0f;
	return basis;
}

/// A plane on which only row `row`'s RATE moves: its multiplier is 1 + travel X; no phase moves.
PartialBasis onlyRateMoves(size_t row) {
	PartialBasis basis {};
	basis.x[kPartialTriangles + row] = 1.0f;
	return basis;
}

/// Where a config's triangle peaks, in homes, over [lo, hi]: (n + duty/2) periods past the offset.
std::vector<double> peakHomes(const phi::PhiTriConfig& cfg, double lo, double hi) {
	std::vector<double> at;
	const double period = 1.0 / static_cast<double>(cfg.phiFreq);
	for (int n = -64; n <= 64; ++n) {
		const double h = (static_cast<double>(n) + 0.5 * static_cast<double>(cfg.duty)) * period
		                 - static_cast<double>(cfg.phaseOffset);
		if (h >= lo && h <= hi) {
			at.push_back(h);
		}
	}
	return at;
}

/// The reachable window (home −8..+11) and a margin of a home each side.
constexpr double kReachLo = -9.0;
constexpr double kReachHi = 12.0;

} // namespace

TEST_CASE("the partial field bank sits on V5's band, with the duties design §2.1 gives, and Smoothness is V5's field") {
	const float slowestOnBand = static_cast<float>(static_cast<double>(kFlagshipQuantizeRate)
	                                               * std::pow(std::numbers::phi, -0.15 * 11));
	for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
		CAPTURE(r);
		const phi::PhiTriConfig want = restatedPartialField(r);
		const phi::PhiTriConfig got = partialFieldBank()[r];
		CHECK(got.phiFreq == want.phiFreq);
		CHECK(got.duty == want.duty);
		CHECK(got.phaseOffset == want.phaseOffset);
		CHECK_FALSE(got.bipolar);
		if (r != partial_slot::kSmoothness) {
			// ON the band: no faster than V5's quantize field, no slower than rank 11, so every period
			// (about 3 to 6.5 homes) fits the reachable window several times over.
			CHECK(got.phiFreq <= kFlagshipQuantizeRate);
			CHECK(got.phiFreq >= slowestOnBand);
			CHECK(1.0 / static_cast<double>(got.phiFreq) < (kReachHi - kReachLo) / 3.0);
		}
	}
	// Smoothness IS V5's field (design §4.1), every member, by name.
	const phi::PhiTriConfig smooth = partialFieldBank()[partial_slot::kSmoothness];
	CHECK(smooth.phiFreq == kFlagshipSmoothField.phiFreq);
	CHECK(smooth.duty == kFlagshipSmoothField.duty);
	CHECK(smooth.phaseOffset == kFlagshipSmoothField.phaseOffset);
	CHECK(smooth.bipolar == kFlagshipSmoothField.bipolar);
}

TEST_CASE("the partial terrain's origin is clear: no gated field peaks there, rolloff is mid-range, gated peaks do not coincide") {
	// V5's house rule for offsets. At the origin every gated field (duty < 1) is in its dead zone, and no
	// gated peak lies within half a home of it; rolloff reads mid-range, not its brightest end; and over
	// the reachable window no two gated peaks fall within a fifth of a home of each other.
	const partials::PartialFields atOrigin = readPartialFields(PartialBasis {}, 0.0, 0.0, 0.0);
	CHECK(atOrigin.formant1Gain == 0.0f);
	CHECK(atOrigin.formant2Gain == 0.0f);
	CHECK(atOrigin.oddEven == 0.0f);
	CHECK(atOrigin.combDepth == 0.0f);
	CHECK(atOrigin.offGrid == 0.0f);
	CHECK(atOrigin.smoothness == 0.0f);
	CHECK(std::abs(atOrigin.rolloff - 0.5f) < 0.01f);
	// AND PAL STAGE 1'S GATED GUESTS: the knee, the shelf, the body and sparse country are all at rest at the origin.
	CHECK(atOrigin.knee == 0.0f);
	CHECK(atOrigin.shelf == 0.0f);
	CHECK(atOrigin.body == 0.0f);
	CHECK(atOrigin.sparsity == 0.0f);

	std::vector<std::pair<size_t, double>> peaks;
	for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
		const phi::PhiTriConfig cfg = partialFieldBank()[r];
		if (cfg.duty < 1.0f) {
			for (const double h : peakHomes(cfg, kReachLo, kReachHi)) {
				peaks.emplace_back(r, h);
			}
		}
	}
	REQUIRE(peaks.size() > 20); // non-vacuity: six gated fields, several periods each
	for (size_t i = 0; i < peaks.size(); ++i) {
		CAPTURE(peaks[i].first);
		CAPTURE(peaks[i].second);
		CHECK(std::abs(peaks[i].second) >= 0.5);
		for (size_t j = i + 1; j < peaks.size(); ++j) {
			if (peaks[i].first != peaks[j].first) {
				CAPTURE(peaks[j].first);
				CAPTURE(peaks[j].second);
				CHECK(std::abs(peaks[i].second - peaks[j].second) >= 0.2);
			}
		}
	}
}

TEST_CASE("a partial sound at zero travel and zero residue reads its fields at home, bit for bit, whatever the orientation") {
	for (const double home : { 0.0, 0.37, 1.9, -3.25, 17.0, 63.5 }) {
		CAPTURE(home);
		std::array<double, partial_slot::kFieldCount> atHome {};
		atHome.fill(home);
		const partials::PartialFields wantFields = restatedFields(atHome);
		std::array<float, partials::kMaxPartials> wantDetail {};
		for (int k = 1; k <= partials::kMaxPartials; ++k) {
			wantDetail[static_cast<size_t>(k - 1)] = restatedDetail(k, home);
		}
		partials::PartialSpectrum want;
		partials::buildSound(wantFields, wantDetail, want);

		partials::PartialSpectrum first;
		buildPartialSound(sliceBasisFor<kPartialTriangles>(0.0, home), home, 0.0, 0.0, first);
		for (const SliceGenerator& generator : orientations()) {
			CAPTURE(generator.rotate);
			const PartialBasis frame = sliceBasisFor<kPartialTriangles>(sliceRotateOf(generator), home);
			CHECK(readPartialFields(frame, home, 0.0, 0.0) == wantFields);
			std::array<float, partials::kMaxPartials> detail {};
			readPartialDetail(frame, home, 0.0, 0.0, detail);
			CHECK(detail == wantDetail);
			partials::PartialSpectrum got;
			buildPartialSound(frame, home, 0.0, 0.0, got);
			CHECK(got == want);
			CHECK(got == first);
		}
	}
}

TEST_CASE("each partial field reaches its documented range") {
	// Over the REACHABLE window only (home −8..+11 and a home of margin each side, no gamma), every field
	// must reach its top and its floor: 0 for a gated field (the dead zone is exactly zero) and within a
	// hair of 0 for a duty-1 one. And every detail triangle reaches both ends of [-1, 1]. The window is
	// not widened to make a field pass: a field that cannot cover its range here is a field a hand
	// cannot find.
	constexpr int kSteps = 4000;
	std::array<float, partial_slot::kFieldCount> lo {};
	std::array<float, partial_slot::kFieldCount> hi {};
	lo.fill(2.0f);
	hi.fill(-2.0f);
	std::array<float, partials::kMaxPartials> detailLo {};
	std::array<float, partials::kMaxPartials> detailHi {};
	detailLo.fill(2.0f);
	detailHi.fill(-2.0f);
	const PartialBasis frame {};
	for (int s = 0; s <= kSteps; ++s) {
		const double home = kReachLo + (kReachHi - kReachLo) * static_cast<double>(s) / kSteps;
		const partials::PartialFields f = readPartialFields(frame, home, 0.0, 0.0);
		const float values[] = { f.rolloff,     f.formant1Centre, f.formant1Width, f.formant1Gain, f.formant2Centre,
			                     f.formant2Width, f.formant2Gain, f.oddEven,       f.combSpacing,  f.combDepth,
			                     f.detail,      f.offGrid,        f.smoothness };
		for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
			lo[r] = std::min(lo[r], values[r]);
			hi[r] = std::max(hi[r], values[r]);
		}
		std::array<float, partials::kMaxPartials> detail {};
		readPartialDetail(frame, home, 0.0, 0.0, detail);
		for (size_t i = 0; i < detail.size(); ++i) {
			detailLo[i] = std::min(detailLo[i], detail[i]);
			detailHi[i] = std::max(detailHi[i], detail[i]);
		}
	}
	for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
		CAPTURE(r);
		CHECK(hi[r] >= 0.95f);
		CHECK(hi[r] <= 1.0f);
		CHECK(lo[r] >= 0.0f);
		if (partialFieldBank()[r].duty < 1.0f) {
			CHECK(lo[r] == 0.0f);
		}
		else {
			CHECK(lo[r] <= 0.05f);
		}
	}
	// The detail triangles are bipolar and each reaches both ends.
	for (size_t i = 0; i < detailLo.size(); ++i) {
		CAPTURE(i);
		CHECK(detailLo[i] >= -1.0f);
		CHECK(detailLo[i] <= -0.95f);
		CHECK(detailHi[i] <= 1.0f);
		CHECK(detailHi[i] >= 0.95f);
	}
}

TEST_CASE("detail rates rise with k, by the formula the design gives") {
	CHECK(partialDetailConfig(2).phiFreq < partialDetailConfig(64).phiFreq);
	const double base = static_cast<double>(kFlagshipFmWidthRate / phi::kPhi050 * phi::kPhi400);
	CHECK(static_cast<double>(partialDetailBaseRate()) == base);
	for (int k = 1; k <= partials::kMaxPartials; ++k) {
		CAPTURE(k);
		const phi::PhiTriConfig cfg = partialDetailConfig(k);
		const double want = base * std::pow(std::numbers::phi, 0.5 * std::log2(static_cast<double>(k)));
		CHECK(std::abs(static_cast<double>(cfg.phiFreq) - want) <= 1e-6 * want);
		CHECK(cfg.duty == 1.0f);
		CHECK(cfg.bipolar);
		// The offset is frac(k·φ²) of the triangle's own period.
		const double periods = static_cast<double>(k) * std::numbers::phi * std::numbers::phi;
		CHECK(std::abs(static_cast<double>(cfg.phaseOffset) * static_cast<double>(cfg.phiFreq)
		               - (periods - std::floor(periods)))
		      < 1e-5);
		if (k > 1) {
			CHECK(cfg.phiFreq > partialDetailConfig(k - 1).phiFreq);
		}
		// Even the slowest detail triangle is faster than rolloff, the largest region.
		CHECK(cfg.phiFreq > partialFieldBank()[partial_slot::kRolloff].phiFreq);
	}
}

TEST_CASE("each harmonic reads its group's row: harmonics 1-2, 3-8, 9-32 and 33 up") {
	CHECK(partialDetailRow(1) == partial_slot::kDetailGroup);
	CHECK(partialDetailRow(2) == partial_slot::kDetailGroup);
	CHECK(partialDetailRow(3) == partial_slot::kDetailGroup + 1);
	CHECK(partialDetailRow(8) == partial_slot::kDetailGroup + 1);
	CHECK(partialDetailRow(9) == partial_slot::kDetailGroup + 2);
	CHECK(partialDetailRow(32) == partial_slot::kDetailGroup + 2);
	CHECK(partialDetailRow(33) == partial_slot::kDetailGroup + 3);
	CHECK(partialDetailRow(partials::kMaxPartials) == partial_slot::kDetailGroup + 3);
	CHECK(partial_slot::kDetailGroup + 3 == kPartialTriangles - 1);
}

TEST_CASE("each partial field and detail group reads its own basis row, and no two share one") {
	// One row at a time is given a travel direction and every other row none. The field (or group) on
	// that row must read the triangle at home + travel, and every other one the triangle at home —
	// values, restated, so a builder reading the wrong row fails on the value, not on "something moved".
	constexpr double kTravel = 0.29;
	for (size_t row = 0; row < kPartialTriangles; ++row) {
		CAPTURE(row);
		const PartialBasis frame = onlyRowMoves(row);
		bool moved = false; // non-vacuity: somewhere the travel must actually change this row's reading
		for (double home = 0.0; home < 130.0; home += 1.3) {
			CAPTURE(home);
			std::array<double, partial_slot::kFieldCount> phase {};
			for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
				phase[r] = r == row ? home + kTravel : home;
			}
			CHECK(readPartialFields(frame, home, kTravel, 0.0) == restatedFields(phase));
			std::array<float, partials::kMaxPartials> detail {};
			readPartialDetail(frame, home, kTravel, 0.0, detail);
			std::array<float, partials::kMaxPartials> want {};
			for (int k = 1; k <= partials::kMaxPartials; ++k) {
				want[static_cast<size_t>(k - 1)] = restatedDetail(k, partialDetailRow(k) == row ? home + kTravel : home);
			}
			CHECK(detail == want);
			if (row < partial_slot::kFieldCount) {
				moved = moved || restatedField(row, home + kTravel) != restatedField(row, home);
			}
			else {
				moved = true; // every group has a duty-1 harmonic, so a travel always moves one
			}
		}
		CHECK(moved);
	}
}

TEST_CASE("two detail groups move independently under a travel along one group's axis") {
	// Travel along the 3-8 group's axis alone: harmonics 3-8 move, and nothing else does. Then the same
	// along the 33-up group's. The claim is the INDEPENDENCE, so the unmoved groups are checked to the bit.
	for (const size_t group : { size_t { 1 }, size_t { 3 } }) {
		CAPTURE(group);
		const PartialBasis frame = onlyRowMoves(partial_slot::kDetailGroup + group);
		for (const double home : { 0.11, 2.5, 9.75 }) {
			CAPTURE(home);
			std::array<float, partials::kMaxPartials> still {};
			std::array<float, partials::kMaxPartials> travelled {};
			readPartialDetail(frame, home, 0.0, 0.0, still);
			readPartialDetail(frame, home, 0.41, 0.0, travelled);
			int movedCount = 0;
			for (int k = 1; k <= partials::kMaxPartials; ++k) {
				CAPTURE(k);
				const auto i = static_cast<size_t>(k - 1);
				if (partialDetailRow(k) == partial_slot::kDetailGroup + group) {
					CHECK(travelled[i] == restatedDetail(k, home + 0.41));
					movedCount += travelled[i] != still[i] ? 1 : 0;
				}
				else {
					CHECK(travelled[i] == still[i]);
				}
			}
			CHECK(movedCount > 0);
		}
	}
}

TEST_CASE("each partial field and detail group reads its own RATE row, and no two share one") {
	// The rate half of the addressing case above: one row's rate multiplier moves (1 + travel) and no
	// phase does. That field (or group) must read the triangle at home with that multiplier, everything
	// else at home with 1 — so a builder that fixes the rate at 1, or reads another row's rate, goes red.
	constexpr double kTravel = 0.29;
	const float moved = 1.0f + static_cast<float>(kTravel);
	for (size_t row = 0; row < kPartialTriangles; ++row) {
		CAPTURE(row);
		const PartialBasis frame = onlyRateMoves(row);
		bool changed = false;
		for (double home = kReachLo; home < kReachHi; home += 0.7) {
			CAPTURE(home);
			std::array<double, partial_slot::kFieldCount> phase {};
			phase.fill(home);
			std::array<float, partial_slot::kFieldCount> rate {};
			for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
				rate[r] = r == row ? moved : 1.0f;
			}
			CHECK(readPartialFields(frame, home, kTravel, 0.0) == restatedFields(phase, rate));
			std::array<float, partials::kMaxPartials> detail {};
			readPartialDetail(frame, home, kTravel, 0.0, detail);
			std::array<float, partials::kMaxPartials> want {};
			for (int k = 1; k <= partials::kMaxPartials; ++k) {
				want[static_cast<size_t>(k - 1)] = restatedDetail(k, home, partialDetailRow(k) == row ? moved : 1.0f);
			}
			CHECK(detail == want);
			if (row < partial_slot::kFieldCount) {
				changed = changed || restatedField(row, home, moved) != restatedField(row, home);
			}
			else {
				for (int k = 1; k <= partials::kMaxPartials; ++k) {
					changed = changed || (partialDetailRow(k) == row && restatedDetail(k, home, moved) != restatedDetail(k, home));
				}
			}
		}
		CHECK(changed);
	}
}

TEST_CASE("a partial sound reads its banked residue through the frame: at zero travel each row stands at home plus its residue") {
	// sliceFrameFor attaches what past turns banked; at zero travel row r reads at home + residue.offset[r]
	// with rate 1 + residue.offset[N + r], whatever the plane. Distinct per row, so a row reading
	// another's residue (or none) fails on the value.
	SliceResidue<kPartialTriangles> residue;
	for (size_t r = 0; r < kPartialTriangles; ++r) {
		residue.offset[r] = 0.013 * static_cast<double>(r + 1) - 0.1;
		residue.offset[kPartialTriangles + r] = 0.007 * static_cast<double>(r + 1) - 0.05;
	}
	const auto phaseOf = [&](size_t r, double home) { return home + residue.offset[r]; };
	const auto rateOf = [&](size_t r) { return 1.0f + static_cast<float>(residue.offset[kPartialTriangles + r]); };
	for (const SliceGenerator& generator : orientations()) {
		CAPTURE(generator.rotate);
		for (const double home : { -7.5, 0.37, 4.0, 10.25 }) {
			CAPTURE(home);
			const PartialBasis frame = sliceFrameFor<kPartialTriangles>(sliceRotateOf(generator), home, residue);
			std::array<double, partial_slot::kFieldCount> phase {};
			std::array<float, partial_slot::kFieldCount> rate {};
			for (size_t r = 0; r < partial_slot::kFieldCount; ++r) {
				phase[r] = phaseOf(r, home);
				rate[r] = rateOf(r);
			}
			CHECK(readPartialFields(frame, home, 0.0, 0.0) == restatedFields(phase, rate));
			std::array<float, partials::kMaxPartials> detail {};
			readPartialDetail(frame, home, 0.0, 0.0, detail);
			std::array<float, partials::kMaxPartials> want {};
			for (int k = 1; k <= partials::kMaxPartials; ++k) {
				const size_t row = partialDetailRow(k);
				want[static_cast<size_t>(k - 1)] = restatedDetail(k, phaseOf(row, home), rateOf(row));
			}
			CHECK(detail == want);
		}
	}
}
