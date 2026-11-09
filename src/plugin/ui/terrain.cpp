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

#include "plugin/ui/terrain.h"

#include <span>

#include "plugin/ui/feature_norm.h"

#include "dsp/drum_picture.h"
#include "dsp/ks_picture.h"
#include "dsp/flagship.h"
#include "dsp/phase_warp.h"
#include "dsp/morph_renderer.h"
#include "dsp/slice_build.h"
#include "dsp/osc_voice.h"
#include "dsp/stair_renderer.h"
#include "dsp/vox_bake.h"

#include <feathers/tokens.h>

#include <algorithm>
#include <tuple>
#include <chrono>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace {

constexpr double kTau = 6.283185307179586;
constexpr double kPhi = 1.618033988749895;

/// How far Offset can carry home past the zone axis, and therefore where the space stops. The
/// parameter's own range, read once here rather than guessed: a scrub that ran past it would post a
/// value the engine clamps and the picture does not.
double offsetCeiling() {
	const ParamDesc* desc = findParam(static_cast<clap_id>(ParamId::AOffset));
	return desc != nullptr ? desc->max : 2.0;
}

double zoneCeiling() {
	const ParamDesc* desc = findParam(static_cast<clap_id>(ParamId::AZone));
	return desc != nullptr ? desc->max : 1023.0;
}

//-------------------//
// The colour ramp   //
//-------------------//

struct Rgb {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

constexpr Rgb rgbOf(uint32_t argb) {
	return { static_cast<float>((argb >> 16) & 0xFFu), static_cast<float>((argb >> 8) & 0xFFu),
		     static_cast<float>(argb & 0xFFu) };
}

constexpr Rgb kRidge = rgbOf(tokens::kNebCoolRidge);

/// THE TRANSFER FUNCTIONS, AND THEY ARE CONSTANTS. Every one of these is what makes the map a pure
/// function of position: nothing here is fitted to a window, a session or a patch. Changing one
/// changes every colour on every map, which is the same weight as regenerating a golden.
/// CALIBRATED AGAINST THE MEASURED SPREAD, not chosen. Sampled over the whole zone axis at several
/// travels, the three features land at (p05 / p50 / p95): energy 0.25/0.40/0.62 for morph and
/// 0.12/0.27/0.28 for stair; centroid 0.10/0.24/0.36 and 0.06/0.25/0.43; roughness 0.05/0.16/0.31
/// and 0.04/0.20/0.37. The gains put the median near the middle of each channel's travel, so the map
/// is neither a white sheet nor a black one — the first calibration saturated luminance everywhere
/// and produced exactly the white sheet, which is what a photograph is for.
///
/// ONE SET OF GAINS FOR BOTH ENGINES, on purpose: stair really does sit lower in RMS, so it really
/// does draw darker, and a per-engine stretch would be the same view-dependent refit the design
/// forbids wearing a different hat.
constexpr float kEnergyGain = 1.1f;   ///< RMS at which luminance saturates
constexpr float kCentroidGain = 2.4f; ///< centroid at which the hue reaches the far family
constexpr float kRoughGain = 2.6f;    ///< roughness at which the grain is at full depth
constexpr float kGlowFloor = 0.10f;   ///< the mock's `.10 + 1.05*e`
constexpr float kGlowRange = 1.05f;
/// THE RIDGE HIGHLIGHT'S DEPTH, and 3.0 was a stacking mistake the stair photograph caught. It was
/// set against the RAW roughness; kRoughGain then multiplied roughness by 2.6 and the ember squares
/// it, so the term grew by 6.8x and the 3.0 rode on top of that. At about twenty times its intended
/// depth the gold ridge stopped being a highlight and became the ground — PhiStair, whose roughness
/// runs higher, drew as an almost entirely warm map, which is the modulator land's colour and not
/// sound land's.
///
/// At 1.0 the term lands where it was meant to for BOTH engines without a per-engine number: median
/// ember 0.08 morph and 0.08 stair, p95 0.44 and 0.29. A tint on the roughest, brightest ground.
constexpr float kEmberGain = 1.0f;
/// How much of a STAIR map's hue is its pattern family and how much is the local measurement. Only
/// stair has families; a morph map is all local measurement now — see terrainColour.
constexpr float kFamilyShare = 0.62f;
/// How deep the grain cuts into luminance at full roughness.
constexpr float kGrainDepth = 0.22f;

/// The DFT's twiddles, built once.
///
/// The analysis is a fixed size by design — see the note on kTerrainCycleSamples — so the cosines and
/// sines it needs are a constant table rather than 768 transcendental calls per texel. Measured: a
/// 128x128 map went from 107 ms to the number in the perf case, which is the difference between a
/// zoom that fills and a zoom that stalls.
struct Twiddles {
	std::array<float, kTerrainHarmonics * kTerrainCycleSamples> cosine {};
	std::array<float, kTerrainHarmonics * kTerrainCycleSamples> sine {};
	Twiddles() {
		for (int k = 1; k <= kTerrainHarmonics; ++k) {
			for (int i = 0; i < kTerrainCycleSamples; ++i) {
				const double angle = kTau * static_cast<double>(k) * static_cast<double>(i)
				                     / static_cast<double>(kTerrainCycleSamples);
				const size_t at = static_cast<size_t>(k - 1) * kTerrainCycleSamples
				                  + static_cast<size_t>(i);
				cosine[at] = static_cast<float>(std::cos(angle));
				sine[at] = static_cast<float>(std::sin(angle));
			}
		}
	}
};

/// A plain aggregate of floats — no visage objects, nothing owned by another library's static — so
/// the destruction-order trap that cost this project a font cache does not apply.
const Twiddles& twiddles() {
	static const Twiddles table;
	return table;
}

float clamp01(double v) {
	return static_cast<float>(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
}

/// The fast φ texture, as a pure function of position.
///
/// THREE OCTAVES AT INCOMMENSURATE RATES, which is the same trick the character layer itself is made
/// of — landscapes that never realign. Three because one fixed frequency can only read at one zoom:
/// the coarse octave carries the picture when zoomed out and the fine one when zoomed in, and neither
/// is ever refitted to the view. Quoted in zones and travel units so the numbers say what they mean.
/// **BOTH ARGUMENTS ARE TRAVEL NOW** (backlog GY). This was `grainAt(home, travel)` with a cycle set
/// per axis, which was right while the vertical axis WAS home. By design home is now constant over
/// the whole map — so leaving it in would have made the grain a function of travelX alone and drawn
/// VERTICAL stripes, the same defect the refactor exists to remove, rotated ninety degrees.
///
/// THE TWO RATE SETS ARE INCOMMENSURATE RATHER THAN EQUAL, which matters for the same reason the
/// engine's own rates are phi-spaced: give both axes the same cycles and the texture gains a diagonal
/// symmetry — a privileged direction, reintroduced by the fix. The second set is the first scaled by
/// phi, so the pattern does not repeat along any line.
///
/// IT IS A PICTURE-ONLY TEXTURE — `terrainColour` is not on any audio road — so this changes what the
/// chart looks like and nothing about what it sounds like.
double grainAt(double travelX, double travelY) {
	constexpr double kHomeCycles[3] = { 2.0 * kPhi, 13.7 * kPhi, 93.8 * kPhi };
	constexpr double kTravelCycles[3] = { 2.0, 13.7, 93.8 };
	double sum = 0.0;
	double weight = 0.0;
	for (int octave = 0; octave < 3; ++octave) {
		const double a = std::sin(kTau * (travelY * kHomeCycles[octave] + travelX * kTravelCycles[octave]));
		const double b = std::sin(kTau * (travelY * kHomeCycles[octave] * kPhi
		                                  - travelX * kTravelCycles[octave] * kPhi));
		const double w = 1.0 / (1.0 + static_cast<double>(octave));
		sum += a * b * w;
		weight += w;
	}
	return sum / weight;
}

} // namespace

//-----------------//
// Home arithmetic //
//-----------------//

double homeOf(double zone, double offset, double gammaSum) {
	return zone / 1023.0 + offset + gammaSum;
}

double homeFloor(double gammaSum) {
	return gammaSum;
}

double homeCeiling(double gammaSum) {
	return zoneCeiling() / 1023.0 + offsetCeiling() + gammaSum;
}

// `homeToCoord` LIVED HERE, and its deletion is the shape of the whole change. It split a home into
// the zone and offset that reach it, clamping at the ends of what those two parameters can hold — the
// rule the chart drew with and the rule a scrub wrote with, deliberately one rule so the colour under
// the star was the colour of the sound the star would become. What it also was, unavoidably, was the
// wall, and the world is meant to be infinite. So the design is ONE VALUE instead — a scrub moves the
// sound's unbounded gamma base, the two dials are bounded knobs summing on top the way gamma does on
// the hardware (voice.cpp:1688) — and with nothing bounded left in the path there is nothing to
// decompose and nothing to clamp. Both callers went; so did this.

//----------------//
// Span averaging //
//----------------//

namespace {
/// The finest feature each engine draws, in zones — worst case over its banks at rest. Measured
/// rather than remembered: see the wavelength case in the slice-build suite, and the header for what
/// travel does to these.
constexpr double kMorphFinestZones = 169.0;
constexpr double kStairFinestZones = 34.5;
/// Resonate's, from the same MEASURE case (2026-09-25): its fastest bank is PluckPos at φ^3.75, 169.3 zones —
/// Morph's own number, since the two banks share that frequency.
constexpr double kResonateFinestZones = 169.3;

/// TWO, AND TWO IS A TASTE NUMBER: the unsmoothed view is preferred, so smoothing comes all the way
/// down to 2.
///
/// The schedule reached 5 and then 17 samples, because that is where the measured error stops falling
/// — and it was RIGHT about the arithmetic and wrong about the picture. Seventeen samples is a
/// seventeen-times build, which shows as a settle crawling down the chart for two thirds of a second,
/// and it was rejected twice as too slow. Then, between the two pictures, the rough one was chosen.
/// Both halves of that are a taste decision and neither is arguable from a number: what averaging buys
/// is a quieter map, and a quieter map is not the one wanted.
///
/// SO THE CAP IS A PREFERENCE AND THE TRIGGER IS STILL THE MEASUREMENT. Where a cell cannot alias
/// there is still exactly one sample and the picture is bit-identical to everything shipped so far;
/// where it can, there are two rather than seventeen — the first step of the same curve, taking about
/// a third of what the full mean took (measured: row-to-row step 26.3 rough → 18.8 at two → 9.0 at
/// seventeen, morph at the widest stop) for a settle that arrives in one extra sweep instead of
/// sixteen. Parallelising the old settle was specced and deliberately NOT built: two sweeps do not
/// need eight threads.
constexpr int kSampleTiers[] = { 1, 2 };

/// AND THE CAP IS ONE — decided after looking at two.
///
/// THE WHOLE CHAIN, because it is a chain of TASTE rather than of findings and the record should read
/// as one: seventeen samples shipped (where the measured error stops falling) → rejected as a settle
/// too slow to watch → two (the first step of the same curve, a third of the benefit for one extra
/// sweep) → one, pure point sampling, no averaging anywhere. Compared side by side with the smoothed
/// picture, the rough one kept being chosen.
///
/// WHAT IS LEFT AND WHY. The schedule below still computes what a cell can and cannot resolve, the
/// pattern still knows how to place a pair, and the measured curve is still in the record (26.3 rough
/// → 18.1 at two → 9.0 at seventeen, morph at the widest stop). NOTHING RUNS: with the cap at one the
/// refinement pass never starts and every texel is the point in the middle of its cell, exactly as it
/// was before any of this. It stays because the measurement behind it is true whatever the taste is —
/// stair's glyph pattern really does fold at the stop we ship — and because raising it again is this
/// one number rather than a rewrite.
constexpr int kSampleCap = 1;
} // namespace

int terrainSamplesFor(OscType engine, double cellZones) {
	// ADDITIVE TAKES MORPH'S FINEST (2026-10-10): its fields sit on V5's band (0828abfa), V5 being on the morph schedule,
	// and with the cap at one nothing here runs anyway.
	const double finest = engine == OscType::Stair      ? kStairFinestZones
	                      : engine == OscType::Resonate ? kResonateFinestZones
	                                                    : kMorphFinestZones;
	if (!(cellZones > 0.0) || !(finest > 0.0)) {
		return kSampleTiers[0];
	}
	// A cell smaller than half the finest feature cannot alias: there is nothing inside it to average
	// that one sample in the middle does not already stand for. This is the trigger, and it is the
	// measurement rather than the taste — every rung this surface shipped with lands here for morph,
	// and all but the widest for stair.
	if (cellZones <= finest * 0.5) {
		return kSampleTiers[0];
	}
	// The trigger still fires — this is where a cell stops being able to resolve what is inside it —
	// and the cap is what says how much is done about it. Keeping the two separate is the point: one is
	// a measurement and one is a matter of taste, and only the second of them moves.
	return std::min(kSampleCap, kSampleTiers[1]);
}

TerrainSampleOffset terrainSampleOffset(int index, int count) {
	if (count <= 1) {
		// THE CENTRE, which is the map this surface has always drawn — and the property that keeps every
		// shipped zoom bit-identical, since a one-sample build takes exactly this point.
		return {};
	}
	if (count == 2) {
		// A DIAGONAL PAIR, AND THE DIRECTION IS THE WHOLE OF THE CHOICE — measured, after I got it
		// backwards by reasoning.
		//
		// The character's iso-lines run diagonally: phase is `home + x·travel`, so a place keeps its
		// character along a line where the two move together, which is the banding every wide-stop
		// photograph shows. Two samples ALONG a band average nothing; two across it straddle two
		// characters, and that is the only reason a pair is worth twice the work.
		//
		// I read the direction off a photograph, chose the anti-diagonal, and the numbers said it did
		// nothing: row-to-row step 26.3 → 25.3 of 255, a four percent reduction where two independent
		// samples should give thirty. The photograph had been written out with its rows reversed, so
		// the banding I took the direction from was the mirror of the banding on screen. THIS direction
		// measures 26.3 → 18.1 on morph and 13.5 → 10.4 on stair, and 18.6 is exactly what two
		// INDEPENDENT samples predict — so the pair is now doing everything a pair can do. The grain
		// direction of the map is a measurement, not a geometry argument, and it took one run to settle.
		//
		// Symmetric about the centre, so the mean of the two is unbiased over the cell — a pair
		// containing the centre would lean toward wherever its partner sat.
		const double at = index == 0 ? -0.25 : 0.25;
		return { at, at };
	}
	// Beyond a pair: the stratified n×n lattice behind the centre, kept because the schedule reached it
	// once and the reasoning survives the cap being a taste rather than a measurement.
	if (index <= 0) {
		return {};
	}
	const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(count - 1))));
	if (side <= 0) {
		return {};
	}
	const int at = (index - 1) % (side * side);
	const int row = at / side;
	const int column = at % side;
	const double step = 1.0 / static_cast<double>(side);
	// Stratified: one sample per sub-cell, at its centre...
	const double home = (static_cast<double>(row) + 0.5) * step - 0.5;
	// ...shifted along travel by the golden fraction of a sub-cell per lattice row, so the pattern
	// cannot line up with a periodic field the way a plain grid does.
	constexpr double kGolden = 0.6180339887498949;
	double travel = (static_cast<double>(column) + 0.5) * step - 0.5
	                + std::fmod(static_cast<double>(row) * kGolden, 1.0) * step;
	// Wrapped back inside the cell, so a shifted sample never lands in the neighbour's territory.
	if (travel > 0.5) {
		travel -= 1.0;
	}
	return { home, travel };
}

//----------//
// Features //
//----------//

namespace {

/// THE FOUR SHAPE TARGETS, built once per process rather than once per texel (quality review, 2026-10-10): a shape
/// spectrum depends on nothing but its kind, and an Additive texel with a Shape Amount reads one every time.
const partials::PartialSpectrum& additiveShapeSpectrum(partials::ShapeKind kind) {
	static const auto shapes = [] {
		std::array<partials::PartialSpectrum, static_cast<size_t>(partials::ShapeKind::kCount)> built {};
		for (size_t i = 0; i < built.size(); ++i) {
			partials::buildShapeSpectrum(static_cast<partials::ShapeKind>(i), built[i]);
		}
		return built;
	}();
	const auto i = static_cast<size_t>(kind);
	return shapes[i < shapes.size() ? i : static_cast<size_t>(partials::ShapeKind::Saw)];
}

} // namespace

TerrainFrames terrainFramesAt(const SliceOrientationPin& pin, double home,
                              const PlaneResidue& residue) {
	// The two arguments M is a function of: the turn the pin carries — none, for the unpinned map that
	// draws what a place gives itself — and this row's own home.
	const double rotate = rotateFor(pin);
	return { sliceFrameFor<kMorphTriangles>(rotate, home, residue.morph),
		     sliceFrameFor<kStairTriangles>(rotate, home, residue.stair),
		     sliceFrameFor<kVoxTriangles>(rotate, home, residue.vox),
		     sliceFrameFor<kWeaveTriangles>(rotate, home, residue.weave),
		     sliceFrameFor<kPartialTriangles>(rotate, home, residue.partials) };
}

TerrainFeatures terrainFeaturesAt(OscType engine, const SliceOrientationPin& pin, uint16_t zone,
                                  float offset, double travelX, double travelY,
                                  const PlaneResidue& residue,
                                  double gammaBase, feathers::FieldScales scales,
                                  feathers::WarpMode warpMode, feathers::WeaveModel weaveModel,
                                  const feathers::ResonateShaping& resonateShaping,
                                  const feathers::ShapesMix& shapesMix, const AdditivePicture& additive) {
	return terrainFeaturesOn(engine, terrainFramesAt(pin, sliceHome(zone, offset, gammaBase), residue),
	                         zone, offset, travelX, travelY, gammaBase, scales, warpMode, weaveModel, resonateShaping,
	                         shapesMix, additive);
}

void terrainCycleOn(OscType engine, const TerrainFrames& frames, uint16_t zone, float offset, double travelX, double travelY,
                    double gammaBase, feathers::FieldScales scales, feathers::WarpMode warpMode,
                    feathers::WeaveModel weaveModel, const feathers::ResonateShaping& resonateShaping,
                    const feathers::ShapesMix& shapesMix, const AdditivePicture& additive, std::span<float> cycle,
                    const feathers::PhaseWarp* warp) {
	// THE LIKES' PHASE WARP (P-MÖBIUS): every site this place's build reads goes through the sound's warp.
	const feathers::AngleWarpScope likes(warp);
	// THE PLANE THIS PLACE GIVES ITSELF. Derived from the texel's own home, never transported and
	// never taken from the selected sound — §7.1b's hard constraint, and what makes the picture
	// independent of what is on screen. A pinned orientation overrides it, because the pin is a
	// deliberate experimental override of exactly this and the map should show what is being heard.
	const int n = static_cast<int>(cycle.size());
	if (engine == OscType::Stair) {
		// A FRAME, not a bare plane: the plane says which way travel leads, the residue says where it
		// leads FROM, and the engine's own builder takes both. The preview took only the first.
		const owlet::dsp::PhiStairParams params =
		    buildStairSliced(zone, offset, travelX, travelY, frames.stair, gammaBase);
		for (int i = 0; i < n; ++i) {
			// evalStairTable rather than the 128-slot grid the renderer plays, for the reason the scope
			// gives: the grid's mip is chosen by the fundamental and a picture has no fundamental. This
			// is the shape those slots are built from.
			cycle[static_cast<size_t>(i)] = evalStairTable(
			    params, static_cast<double>(i) / static_cast<double>(n));
		}
	}
	else if (engine == OscType::Resonate) {
		// THE STRING'S REST POSE AT THIS COORDINATE — one sound's laws read through this row's plane
		// (buildWeaveSliced, the engine's own road), the pose a parked unbowed string of this model
		// comes to rest in, at its settled loudness (weaveRestCycle, which the wave display draws too).
		// The live ring wanders around it; what a PLACE selects is the pose. Its 32 path points are
		// this lens's 32 samples one for one. Coloured through RESONATE'S OWN knots (featureNormFor,
		// 2026-09-25): through the shared ones these dark, smooth poses drew in one hue.
		static_assert(kTerrainCycleSamples == WeaveString::kNodes);
		owlet::dsp::PhiWeaveParams laws = buildWeaveSliced(zone, offset, travelX, travelY, frames.weave, gammaBase);
		shapeWeaveLaws(resonateShaping, laws); // Resonate's Stiffness and Coupling move the pose
		if (n == WeaveString::kNodes) {
			weaveRestCycle(weaveModel, WeaveCharacter { laws, laws, 0.0f }, cycle.data());
		}
		else {
			// A FINER CYCLE (the likes' spectra) READS THE POSE'S 32 NODES PERIODICALLY, linearly between them: the pose has no
			// more detail than its nodes, and a finer analysis of it must not invent any.
			std::array<float, WeaveString::kNodes> pose {};
			weaveRestCycle(weaveModel, WeaveCharacter { laws, laws, 0.0f }, pose.data());
			for (int i = 0; i < n; ++i) {
				const double at = static_cast<double>(i) * WeaveString::kNodes / n;
				const int lo = static_cast<int>(at);
				const double t = at - lo;
				cycle[static_cast<size_t>(i)] = static_cast<float>((1.0 - t) * pose[static_cast<size_t>(lo % WeaveString::kNodes)]
				                                                 + t * pose[static_cast<size_t>((lo + 1) % WeaveString::kNodes)]);
			}
		}
	}
	else if (engine == OscType::V5) {
		// THE OPERATED BUILD, through the same sliced entry the position-purity case pins against
		// the bank's route — so a texel's colour is a measurement of the sound the flagship makes
		// there, operators and all, not of the morph table underneath them. Drawn through
		// evalFlagship for the reason the scope and the stair branch give: the baked grid's mip is
		// chosen by the fundamental, and a picture has no fundamental. At identity-corner texels
		// this IS the morph branch below, to the bit. Features then normalise through the existing
		// knots (decision V10c) — if operated territory saturates the map, that is scale-split
		// evidence to report, not something to refit here.
		//
		// AND THE VOWEL, at the same nominal C3 the scope sings at (kVoxPictureKey) and through the
		// same three-statement idiom. V22c puts the vox crossfade at the operator chain's SOURCE read,
		// so in vowel country the blend IS the cycle the voices make, and a texel built without it was
		// colouring a waveform nobody plays.
		//
		// THE LENS FOLDS SOME OF IT, AND THAT IS THE RIGHT TRADE BECAUSE THE ALTERNATIVE IS NOT ZERO.
		// This is a 32-sample, 12-harmonic reduction and it genuinely cannot RESOLVE a vowel: a second
		// formant sits above the twelve harmonics measured here and folds down into them. That fact
		// was once the argument for leaving the vowel out, and it answers the wrong question. What
		// decides a colour is not whether the vowel is resolved but whether including it makes this
		// reduction a better measurement of the sound — and that is measurable rather than arguable.
		//
		// MEASURED: 18 vowel regions across 6 zones over the whole reachable home window (−8..+11),
		// 1370 positions, each compared against a 256-point reference for the same twelve harmonics.
		// Drawing the vowel misses by 0.0133 mean centroid; omitting it misses by 0.0471. That is 3.5x
		// closer, on a scale where a whole triangle reads 0.0599 — so the error being replaced is
		// about the size of the entire distance from a sine to a triangle. EVERY ONE of the 18 regions
		// improved, best 7.6x, worst 2.3x, none worse.
		//
		// A TEXEL IS ONE POSITION, so this is the SLICED vox build rather than buildVoxFor's blend —
		// the same single-sound-versus-constellation distinction the flagship build below already
		// makes. Presence is the species field at decision V7's lead coordinate, where every other field
		// on this surface is read.
		const double voxHome = sliceHome(zone, offset, gammaBase);
		const float presence = voxPresenceAt(flagshipLeadCoordinate(frames.morph, voxHome, travelX, travelY));
		VoxCycle vowel;
		if (presence > 0.0f) {
			const VoxParams params = buildVoxSliced(zone, offset, travelX, travelY, frames.vox, gammaBase);
			if (voxParamsVoiced(params)) {
				bakeVoxCycle(params, pitchToHz(kVoxPictureKey), vowel);
			}
		}
		const FlagshipSource source =
		    vowel.voiced ? FlagshipSource { &vowel, presence } : FlagshipSource {};

		// THE PREPARE TAKES THE SOURCE TOO, and nothing will tell you if it does not: `held` is the
		// sync+feedback chain sampled at the step centres, and buildFlagshipSliced sampled it with no
		// source at all. Handing the vowel only to evalFlagship would quantize the blend onto a
		// staircase built from the vowel-free cycle — the vowel attenuated by (1 − quantize) and gone
		// entirely at quantize 1. A plausible colour, a half-applied blend, nothing failing.
		FlagshipRecipe recipe =
		    buildFlagshipSliced(zone, offset, travelX, travelY, frames.morph, frames.stair, gammaBase,
		                        scales, kFlagshipNormalisePictureSamples, warpMode, warp);
		// THE PICTURE'S PROBE, coarser than the audio's on purpose — see kFlagshipNormalisePictureSamples
		// for the two measurements that set the number. Four of the five features a colour is made from
		// are ratios and cannot see the gain at all.
		prepareFlagshipRecipe(recipe, source, kFlagshipNormalisePictureSamples);
		// THE MAP PICTURES FM, and this is the honest statement of how much of it.
		//
		// The FM doc's §8 called this the picture problem and deferred it: FM is a per-VOICE operation
		// and a subharmonic composite's period is q carrier cycles, which a single-cycle lens cannot
		// resolve. The record then said the map simply would not show FM — and that was caught at
		// once, because a chart of the carrier is not a chart of the instrument.
		//
		// WHAT IS DRAWN IS ONE CYCLE OF THE REAL COMPOSITE, not an invented waveform: the modulator
		// starts at zero and advances at the interval the fields give, exactly as a voice's does from
		// a note-on, so this is the FIRST carrier cycle of the sound that would play here. It is a
		// pure function of position, which is this surface's binding rule, and it is deterministic.
		//
		// AND IT IS NOT THE WHOLE TRUTH, which is the part to say out loud rather than discover: where
		// the interval is not an integer the sound does not repeat every cycle, so later cycles differ
		// from this one and the map shows the first. What it captures reliably is that FM country is
		// BRIGHTER and where its edges are; what it cannot capture is the slow beat of a subharmonic
		// composite. Drawing that needs a window of q cycles, and q moves with position — a different
		// picture rather than a longer one. MEASURED baseline for whoever takes that on: one 128x128
		// V5 map costs 104 ms today.
		//
		// The pitch is the note the picture has no other way to get, and it is the one the vowel road
		// two branches up already uses for the same reason (kVoxPictureKey), so the two halves of this
		// surface at least imagine the same note.
		const float fmIndex = recipe.fmIndexCyclesFor(pitchToHz(kVoxPictureKey));
		for (int i = 0; i < n; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(n);
			double read = phase;
			if (fmIndex > 0.0f) {
				// The left ear's polarity, +1: the map is one picture, and a chart that averaged two
				// modulator polarities would be drawing a stereo image nobody asked it for.
				read += static_cast<double>(
				    fmOffsetCycles(fmIndex, phase * static_cast<double>(recipe.fmFrequencyRatio()), 1.0));
			}
			cycle[static_cast<size_t>(i)] = evalFlagship(recipe, read, source);
		}
	}
	else if (engine == OscType::BasicShapes) {
		// THE BLEND, READ THROUGH THIS PLACE'S FM AND WARP — V5's FM picture above, with the carrier swapped for
		// the shape mix, because on Basic Shapes the place decides the FM and nothing else about the cycle. The
		// recipe is V5's sliced build at this coordinate, which is where the bank's FM triple and warp depth
		// come from; at FM depth 0 every texel is the same cycle, and the map says so by being one colour.
		// The mip is the lens's: 16 partials, the most 32 samples can hold without folding.
		// THE TERRAIN'S OPERATOR AMOUNTS DO NOT ACT ON BASIC SHAPES, ONLY ITS KNOBS (backlog KV, flagship.h): `true`.
		FlagshipRecipe recipe =
		    buildFlagshipSliced(zone, offset, travelX, travelY, frames.morph, frames.stair, gammaBase, scales,
		                        kFlagshipNormalisePictureSamples, warpMode, warp, true);
		warmShapeTables(); // the builder's thread may get here before any bank has
		const ShapesWeights weights = normaliseShapes(shapesMix);
		const int mip = shapesMipFor(1.0, 16.0);
		prepareShapesRecipe(recipe, weights, mip, kFlagshipNormalisePictureSamples);
		const float fmIndex = recipe.fmIndexCyclesFor(pitchToHz(kVoxPictureKey));
		for (int i = 0; i < n; ++i) {
			const double phase = static_cast<double>(i) / static_cast<double>(n);
			double read = phase;
			if (fmIndex > 0.0f) {
				read += static_cast<double>(
				    fmOffsetCycles(fmIndex, phase * static_cast<double>(recipe.fmFrequencyRatio()), 1.0));
			}
			cycle[static_cast<size_t>(i)] = evalShapesCycle(recipe, weights, mip, read);
		}
	}
	else if (engine == OscType::Additive) {
		// THE PLACE'S SPECTRUM, SUMMED AS ONE CYCLE — A MINIMAL ARM, said so (plan Task 9b; design §4.4 wants luminance =
		// energy, hue = the spectrum's own centroid, texture = Detail and off-grid as twinkle, read straight off the
		// spectrum with no DFT, and a baked feature-norm row of its own: backlog LY). What it does, honestly: the sound
		// this place gives (buildPartialSound through this row's plane, at the selected sound's Smoothness and bias), pulled
		// toward the Shape by the selected sound's Shape Amount as the bank pulls it (blendTowardShape), summed over the
		// partials BELOW the 32-sample lens's Nyquist (ratio 16 sits on it, where a sine samples as a phase-dependent
		// alias, so the sum stops at the fifteenth partial: ratio ≤ 15.25 even off the grid), at Newman's flat phases
		// (partials::cycleOf, the wave display's sum), then reduced like every other engine's cycle. So the map's energy and
		// centroid ARE this spectrum's lowest fifteen partials, never a table the engine does not play. Colours go through
		// the shared knots until the engine has its own row.
		partials::PartialSpectrum spectrum;
		buildPartialSound(frames.partials, sliceHome(zone, offset, gammaBase), travelX, travelY, spectrum,
		                  scales.smoothness, additive.bias, warp);
		if (additive.shapeAmount > 0.0f) {
			partials::blendTowardShape(spectrum, additiveShapeSpectrum(additive.shape), additive.shapeAmount);
		}
		// THE PARTIALS BELOW THIS CYCLE'S NYQUIST: the 32-sample lens's fifteen, or a finer cycle's n/2 - 1 (the likes' spectra).
		const int lensPartials = n == kTerrainCycleSamples ? kAdditiveLensPartials : std::min(n / 2 - 1, partials::kMaxPartials);
		partials::cycleOf(spectrum, lensPartials, lensPartials, nullptr, n, static_cast<double>(n), cycle.data());
	}
	else if (engine == OscType::Drum) {
		// THE DRUM AS HEARD AT THIS PLACE (drum proposal §3.4): the selected sound's kind with this place's seven fields through
		// its scalers, the bank's road (readDrumFields, drumFieldsWithOverrides), each component weighted by how long it rings
		// (drum::heardSpectrum), summed as Additive's lens sums.
		const double home = sliceHome(zone, offset, gammaBase);
		const drum::Fields fields =
		    drumFieldsWithOverrides(drumFieldsWithOverrides(readDrumFields(frames.partials, home, travelX, travelY, warp), additive.drum),
		                            additive.drumKit);
		const int lensPartials = n == kTerrainCycleSamples ? kAdditiveLensPartials : std::min(n / 2 - 1, partials::kMaxPartials);
		partials::PartialSpectrum heard;
		drum::heardSpectrum(additive.drumKind, fields, lensPartials, heard, static_cast<double>(n) / 2.0 - 0.75);
		partials::cycleOf(heard, lensPartials, lensPartials, nullptr, n, static_cast<double>(n), cycle.data());
	}
	else if (engine == OscType::Pluck) {
		// THE STRING AS HEARD AT THIS PLACE (KS design §2.3): the pluck's tone is this place's Additive spectrum (the bank's
		// road, at the selected sound's Smoothness and neutral bias, as a Pluck sound reads it) and the material its five
		// fields through the selected sound's scalers; ks::heardSpectrum weighs each harmonic by how long the loop lets it
		// ring, through the pick and the pickup, its frequency the loop's own partial. So dark, damped country reads dark
		// under a bright pluck, and stretched country rough (its partials do not close on the cycle). Summed as Additive's
		// lens sums, at the same count. At C3 and the default controls: a picture has no note.
		const double home = sliceHome(zone, offset, gammaBase);
		partials::PartialSpectrum tone;
		buildPartialSound(frames.partials, home, travelX, travelY, tone, scales.smoothness, {}, warp);
		const StringFields material =
		    stringFieldsWithOverrides(readStringFields(frames.partials, home, travelX, travelY, warp), additive.string);
		const int lensPartials = n == kTerrainCycleSamples ? kAdditiveLensPartials : std::min(n / 2 - 1, partials::kMaxPartials);
		partials::PartialSpectrum heard;
		ks::heardSpectrum(tone, material, lensPartials, heard, {}, static_cast<double>(n) / 2.0 - 0.75);
		partials::cycleOf(heard, lensPartials, lensPartials, nullptr, n, static_cast<double>(n), cycle.data());
	}
	else {
		const owlet::dsp::PhiMorphWavetable table =
		    buildMorphSliced(zone, offset, travelX, travelY, frames.morph, gammaBase);
		for (int i = 0; i < n; ++i) {
			cycle[static_cast<size_t>(i)] = evalMorphTable(
			    table, static_cast<double>(i) / static_cast<double>(n));
		}
	}

}

TerrainFeatures terrainFeaturesOn(OscType engine, const TerrainFrames& frames, uint16_t zone,
                                  float offset, double travelX, double travelY, double gammaBase,
                                  feathers::FieldScales scales, feathers::WarpMode warpMode,
                                  feathers::WeaveModel weaveModel, const feathers::ResonateShaping& resonateShaping,
                                  const feathers::ShapesMix& shapesMix, const AdditivePicture& additive,
                                  float* partialsOut, const feathers::PhaseWarp* warp) {
	std::array<float, kTerrainCycleSamples> cycle {};
	terrainCycleOn(engine, frames, zone, offset, travelX, travelY, gammaBase, scales, warpMode, weaveModel, resonateShaping,
	               shapesMix, additive, cycle, warp);
	// One cycle, reduced — see terrainFeaturesOfCycle, which is this and nothing else.
	return terrainFeaturesOfCycle(cycle, partialsOut);
}

TerrainFeatures terrainFeaturesOfCycle(std::span<const float> cycle, float* partialsOut) {
	// RMS is the level; the magnitude spectrum gives the other two channels, so
	// hue and grain are measurements of the SOUND rather than of the coefficients that made it — which
	// is what lets one set of channels describe both engines honestly.
	//
	// OF THE CYCLE IT IS GIVEN, which is the whole of the promise: this function reduces a waveform
	// and knows nothing about where it came from. Whether that waveform is the sound being played is
	// the CALLER's claim — every branch above now makes it, the V5 one at a nominal pitch it names.
	double sumSquares = 0.0;
	for (const float sample : cycle) {
		sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
	}

	const Twiddles& table = twiddles();
	double total = 0.0;
	double weighted = 0.0;
	double high = 0.0;
	// TWO MORE READINGS OF THE SAME SPECTRUM, and they cost two accumulators rather than a transform.
	// Odd/even balance might look as though it needs an FFT, which would cost CPU — it does not: the
	// per-harmonic magnitudes are already resolved right here, and centroid and roughness are just two
	// reductions over them. Parity and the fundamental's share are two more.
	double odd = 0.0;
	double fundamental = 0.0;
	for (int k = 1; k <= kTerrainHarmonics; ++k) {
		const size_t base = static_cast<size_t>(k - 1) * kTerrainCycleSamples;
		double real = 0.0;
		double imaginary = 0.0;
		for (int i = 0; i < kTerrainCycleSamples; ++i) {
			const double sample = static_cast<double>(cycle[static_cast<size_t>(i)]);
			real += sample * static_cast<double>(table.cosine[base + static_cast<size_t>(i)]);
			imaginary -= sample * static_cast<double>(table.sine[base + static_cast<size_t>(i)]);
		}
		const double m = std::hypot(real, imaginary);
		if (partialsOut != nullptr) {
			partialsOut[k - 1] = static_cast<float>(m);
		}
		total += m;
		weighted += m * static_cast<double>(k - 1);
		if (k > kTerrainHarmonics / 2) {
			high += m;
		}
		if (k % 2 == 1) {
			odd += m;
		}
		if (k == 1) {
			fundamental = m;
		}
	}

	TerrainFeatures features;
	features.energy =
	    static_cast<float>(std::sqrt(sumSquares / static_cast<double>(kTerrainCycleSamples)));
	if (total > 0.0) {
		features.centroid =
		    static_cast<float>(weighted / total / static_cast<double>(kTerrainHarmonics - 1));
		features.roughness = static_cast<float>(high / total);
		features.parity = static_cast<float>(odd / total);
		features.fundamental = static_cast<float>(fundamental / total);
	}
	return features;
}

TerrainChannels terrainChannels(const TerrainFeatures& measured, OscType engine) {
	const TerrainFeatures features = channelFeaturesFor(engine, measured);
	TerrainChannels channels;
	channels.luminance = clamp01(features.energy * kEnergyGain);
	channels.hue = clamp01(features.centroid * kCentroidGain);
	channels.grain = clamp01(features.roughness * kRoughGain);
	channels.ember = channels.grain * channels.grain * channels.luminance * kEmberGain;
	return channels;
}

int stairFamilyAt(uint16_t zone) {
	// The engine's own line, `min(7, zone >> 7)` — phi_stair.cpp:76 — rather than a second reading of
	// it. A family drawn one zone away from where the engine changes it would be a colour boundary in
	// the wrong place, which is worse than no boundary at all.
	return std::min(kStairFamilies - 1, static_cast<int>(zone >> 7));
}

float terrainFamilyTint(int stairFamily) {
	// WHAT SURVIVES OF THE COUNTRY, and it is the half that was never the orientation field.
	//
	// DECISION: the waveform owns the picture; the decoding scheme can be revisited if need be.
	// So the φ macro field — the drifting "country" that coloured morph — LEAVES THE DRAW entirely. It
	// was measured out rather than argued out: opening it made it genuinely two-dimensional and moved
	// the drawn map by 1.75 of 255, because it reached a pixel only through a hue mix between two ends
	// 40 of 255 apart and touched no luminance. A layer that cannot be seen is a layer that should not
	// be in the way of one that can.
	//
	// PhiStair's families are a different thing wearing the same variable's name and they stay. The
	// pattern family is `zone >> 7` — a DISCRETE region of the world, not a field over it — and §4a
	// rules those may anchor the colour families honestly, which is what tells you which staircase you
	// are standing on before you have played a note. It is also the one part of this that has already
	// been reviewed on screen and accepted. If stair is ever to go pure-waveform too, it is this function
	// and one line at the call site.
	return static_cast<float>(stairFamily) / static_cast<float>(kStairFamilies - 1);
}

ColourMap colourMapFor(OscType engine) {
	switch (engine) {
	case OscType::Morph: return ColourMap::NebulaLit;
	case OscType::Stair: return ColourMap::Orchid;
	case OscType::V5: return kShippedMap; // V5 keeps its original colours, which never had the dimness: exactly the shipped look
	case OscType::Resonate: return ColourMap::Gilt;
	case OscType::BasicShapes: return ColourMap::Spectrum;
	case OscType::Additive: return kAdditiveMap;
	case OscType::Pluck: return kPluckMap;
	case OscType::Drum: return ColourMap::Ember; // the drum's own (drum proposal §3.4; to be confirmed the colours)
	case OscType::kCount: break;
	}
	return kShippedMap;
}

MapGlow terrainGlowFor(ColourMap map) {
	switch (map) {
	case ColourMap::Synthwave:
	case ColourMap::Aurora:
	case ColourMap::Crt:
	case ColourMap::NebulaLit:
	case ColourMap::Orchid:
	case ColourMap::Gilt:
	case ColourMap::Spectrum:
	case ColourMap::Grove:
		// LIFTED: the map read dim overall. The floor takes quiet country off black and the range keeps the loud end
		// from clipping flat (the corners already run to 255 in their strongest channel).
		return { 0.34f, 0.92f };
	case ColourMap::Nebula:
	case ColourMap::Ember:
	case ColourMap::Tide: break;
	}
	return { kGlowFloor, kGlowRange };
}

MapRgb colourMapAt(ColourMap map, float tilt, float second) {
	// THE MAPS, as bilinear blends of four corner colours. Four corners is the smallest thing that is
	// genuinely two-dimensional — a map built from two independent ramps would be a plane with no
	// corner of its own, which is a 1-D map wearing a square's clothes.
	//
	// READ THE CORNERS AS SENTENCES, because that is what a vibe prior is: with parity on the second
	// axis, LOW tilt is dark and settled, HIGH tilt is bright and busy, LOW parity is full and reedy,
	// HIGH parity is hollow and clarinet-ish. Each map answers "what should full-and-dark look like"
	// differently, and that answer is the identity.
	struct Corners {
		MapRgb lowTiltLowSecond;
		MapRgb highTiltLowSecond;
		MapRgb lowTiltHighSecond;
		MapRgb highTiltHighSecond;
	};
	// THE CORNERS HOLD THEIR BRIGHTNESS ROUGHLY EQUAL, and that is a constraint rather than a taste:
	// LUMINANCE IS ALREADY SPENT. The character's energy drives `glow`, which multiplies whatever the
	// map returns, so a map whose axes also moved brightness would be arguing with the loudness channel
	// — two things saying "bright" and no way to read which. My first draft of Nebula ran its tilt axis
	// from a deep slate to a pale cyan, which is mostly a brightness ramp, and the stair family case
	// caught it immediately: the eight families stopped separating in hue because they were separating
	// in a channel the map does not own. Each corner below sits near 120-140 in Rec.709 luma; what
	// changes across the square is HUE and SATURATION.
	constexpr Corners kNebula { // the cool family this surface has always drawn, opened into a plane
		{ 96.0f, 120.0f, 200.0f },  // settled and full: deep blue
		{ 60.0f, 150.0f, 170.0f },  // busy and full: sea green
		{ 170.0f, 100.0f, 200.0f }, // settled and hollow: violet
		{ 205.0f, 120.0f, 150.0f }, // busy and hollow: dusty rose
	};
	constexpr Corners kEmber { // warm: ash and rust rather than sea and violet
		{ 150.0f, 96.0f, 74.0f },   // settled and full: banked coal
		{ 190.0f, 130.0f, 60.0f },  // busy and full: rust
		{ 130.0f, 110.0f, 140.0f }, // settled and hollow: cold ash
		{ 210.0f, 140.0f, 110.0f }, // busy and hollow: hot brick
	};
	constexpr Corners kTide { // a crossing: cold on one side of the square, warm on the other
		{ 60.0f, 140.0f, 150.0f },  // settled and full: deep teal
		{ 120.0f, 150.0f, 100.0f }, // busy and full: kelp
		{ 110.0f, 120.0f, 180.0f }, // settled and hollow: wet slate
		{ 210.0f, 130.0f, 90.0f },  // busy and hollow: amber shore
	};
	// THE ADDITIVE CANDIDATES: retro-futuristic purples, blues and greens, and BRIGHTER than the three above (their corners sit
	// near luma 120-140 on purpose, see above; these keep the HUE rules but let the tilt axis carry some brightness, so a bright
	// partial-rich country glows against a deep-indigo floor. The brightness the whole map lacked comes from terrainGlowFor).
	constexpr Corners kSynthwave { // indigo floor, magenta flank, neon cyan crown
		{ 84.0f, 56.0f, 220.0f },   // settled and full: deep indigo
		{ 40.0f, 225.0f, 255.0f },  // busy and full: neon cyan
		{ 215.0f, 64.0f, 235.0f },  // settled and hollow: magenta-violet
		{ 150.0f, 120.0f, 255.0f }, // busy and hollow: lavender neon
	};
	constexpr Corners kAurora { // navy through teal to aurora green, violet on the hollow side
		{ 36.0f, 70.0f, 190.0f },   // settled and full: navy blue
		{ 40.0f, 235.0f, 170.0f },  // busy and full: teal-green
		{ 130.0f, 80.0f, 225.0f },  // settled and hollow: violet
		{ 150.0f, 255.0f, 130.0f }, // busy and hollow: aurora green
	};
	constexpr Corners kCrt { // deep purple, electric blue, phosphor green
		{ 96.0f, 40.0f, 200.0f },   // settled and full: deep purple
		{ 60.0f, 150.0f, 255.0f },  // busy and full: electric blue
		{ 165.0f, 70.0f, 245.0f },  // settled and hollow: purple
		{ 90.0f, 255.0f, 110.0f },  // busy and hollow: phosphor green
	};
	constexpr Corners kNebulaLit { // Morph: the Nebula family, lifted
		{ 110.0f, 130.0f, 255.0f }, // settled and full: bright blue
		{ 60.0f, 215.0f, 230.0f },  // busy and full: sea cyan
		{ 200.0f, 110.0f, 255.0f }, // settled and hollow: violet
		{ 255.0f, 140.0f, 190.0f }, // busy and hollow: rose
	};
	constexpr Corners kOrchid { // Stair: magenta, hot pink, violet
		{ 205.0f, 55.0f, 175.0f },  // settled and full: magenta
		{ 255.0f, 115.0f, 175.0f }, // busy and full: hot pink
		{ 150.0f, 85.0f, 235.0f },  // settled and hollow: violet
		{ 255.0f, 195.0f, 240.0f }, // busy and hollow: pale orchid
	};
	constexpr Corners kGilt { // Resonate: deep red lows, amber and gold mids, bright yellow highs
		{ 205.0f, 35.0f, 40.0f },   // settled and full: deep red
		{ 255.0f, 238.0f, 80.0f },  // busy and full: bright yellow
		{ 240.0f, 95.0f, 35.0f },   // settled and hollow: red-orange
		{ 255.0f, 190.0f, 45.0f },  // busy and hollow: gold
	};
	constexpr Corners kGrove { // Pluck: walnut lows, leaf-green busy country, moss and birch on the hollow side
		{ 165.0f, 92.0f, 45.0f },   // settled and full: walnut
		{ 150.0f, 235.0f, 75.0f },  // busy and full: leaf green
		{ 70.0f, 165.0f, 115.0f },  // settled and hollow: moss
		{ 250.0f, 228.0f, 165.0f }, // busy and hollow: pale birch
	};
	const Corners& c = [&]() -> const Corners& {
		switch (map) {
		case ColourMap::Ember: return kEmber;
		case ColourMap::Tide: return kTide;
		case ColourMap::Synthwave: return kSynthwave;
		case ColourMap::Aurora: return kAurora;
		case ColourMap::Crt: return kCrt;
		case ColourMap::NebulaLit: return kNebulaLit;
		case ColourMap::Orchid: return kOrchid;
		case ColourMap::Gilt: return kGilt;
		case ColourMap::Grove: return kGrove;
		case ColourMap::Spectrum: return kNebula; // Spectrum is a hue sweep, taken before this table is read
		case ColourMap::Nebula: break;
		}
		return kNebula;
	}();
	if (map == ColourMap::Spectrum) {
		// THE RGB SWEEP (Basic Shapes): hue runs red -> yellow -> green -> cyan -> blue -> magenta -> red along the tilt (a full circle, so the
		// seam is none); the second axis only softens the saturation toward pastel, never the hue.
		const float hue = std::clamp(tilt, 0.0f, 1.0f) * 6.0f; // sextant 0..6: the whole circle, so a position-keyed sweep wraps without a seam
		const float sat = 1.0f - 0.30f * std::clamp(second, 0.0f, 1.0f);
		const float x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
		const float sextant[6][3] = { { 1, x, 0 }, { x, 1, 0 }, { 0, 1, x }, { 0, x, 1 }, { x, 0, 1 }, { 1, 0, x } };
		const auto k = static_cast<size_t>(std::min(5.0f, std::floor(hue)));
		const auto channel = [&](float v) { return 255.0f * (1.0f - sat + sat * v); };
		return { channel(sextant[k][0]), channel(sextant[k][1]), channel(sextant[k][2]) };
	}
	const auto blend = [&](float a, float b, float t) { return a + (b - a) * t; };
	const auto axis = [&](float lowSecond, float highSecond) {
		return blend(lowSecond, highSecond, second);
	};
	return { axis(blend(c.lowTiltLowSecond.r, c.highTiltLowSecond.r, tilt),
	              blend(c.lowTiltHighSecond.r, c.highTiltHighSecond.r, tilt)),
		     axis(blend(c.lowTiltLowSecond.g, c.highTiltLowSecond.g, tilt),
		          blend(c.lowTiltHighSecond.g, c.highTiltHighSecond.g, tilt)),
		     axis(blend(c.lowTiltLowSecond.b, c.highTiltLowSecond.b, tilt),
		          blend(c.lowTiltHighSecond.b, c.highTiltHighSecond.b, tilt)) };
}

uint32_t terrainColour(const TerrainFeatures& features, double travelX, double travelY, int stairFamily,
                       ColourMap map, SecondAxis axis, OscType engine) {
	const FeatureNorm& norm = featureNormFor(engine);
	// THE WAVEFORM OWNS THE PICTURE — by design — and now it owns two colour axes rather than one.
	// Both coordinates are STANDARDISED first, through the baked table, so the map's whole square is
	// reachable instead of the corner the raw distributions happen to occupy: half the world's energy
	// sits inside a band 0.007 wide before standardising.
	const TerrainChannels channels = terrainChannels(features, engine);
	const float tilt = standardize(norm.centroid, features.centroid);
	// PARITY ARRIVES AS A RESIDUAL, which is what makes the map two-dimensional in practice rather than
	// on paper. Standardising each feature makes each one uniform and does nothing about the pair:
	// parity and centroid correlate at −0.72, so raw they walk a diagonal band and leave two thirds of
	// the square unvisited — that sheet read as a one-dimensional colour map, which is the correct
	// criticism of it. Subtracting the part the first axis already says
	// leaves an axis uncorrelated with it by construction. See residualParity.
	const float second =
	    axis == SecondAxis::Parity
	        ? residualParity(norm, features.centroid, features.parity)
	        : (axis == SecondAxis::Fundamental
	               ? standardize(norm.fundamental, features.fundamental)
	               : standardize(norm.roughness, features.roughness));

	// A stair map still anchors its FIRST axis on the pattern family it is standing in — eight discrete
	// regions of the world rather than a field over them. See terrainFamilyTint.
	const float firstAxis = stairFamily >= 0 ? clamp01(kFamilyShare * terrainFamilyTint(stairFamily)
	                                                   + (1.0f - kFamilyShare) * tilt)
	                                         : tilt;
	// BASIC SHAPES' RAINBOW IS KEYED TO POSITION as well as to the measurement: at FM depth 0 every place on that map is the same cycle
	// (one colour by the engine's own honesty), so the hue also walks the diagonal at a fixed spatial period (one circle per 1/1.5 of
	// travel); the measured tilt shifts it. The wrap is seamless because the Spectrum hue is a full circle.
	const double hueU = std::fmod(0.35 * static_cast<double>(firstAxis) + 1.5 * (travelX + travelY), 1.0);
	const float hueAxis = map == ColourMap::Spectrum ? static_cast<float>(hueU < 0.0 ? hueU + 1.0 : hueU) : firstAxis;
	const MapRgb base = colourMapAt(map, hueAxis, second);

	const MapGlow lift = terrainGlowFor(map);
	const float glow = lift.floor + lift.range * channels.luminance;
	const float ember = channels.ember;
	// And the grain, which is roughness made visible: the fast φ texture at a fixed spatial frequency,
	// cutting into luminance in proportion to how rough the place actually is.
	const float grain =
	    1.0f + kGrainDepth * channels.grain * static_cast<float>(grainAt(travelX, travelY));

	const auto channel = [&](float value, float ridge) {
		return static_cast<uint32_t>(std::clamp(value * glow * grain + ember * ridge, 0.0f, 255.0f));
	};
	return 0xff000000u | (channel(base.r, kRidge.r) << 16) | (channel(base.g, kRidge.g) << 8)
	       | channel(base.b, kRidge.b);
}



namespace {

/// CATMULL-ROM'S FOUR WEIGHTS at `t` in [0,1) between p1 and p2 — the interpolating cubic: at t=0 it is p1 exactly and
/// at t=1 it is p2 exactly, and between them it follows the slopes the neighbours imply.
struct CubicWeights {
	float w[4] {};
};

CubicWeights catmullRom(float t) {
	const float t2 = t * t;
	const float t3 = t2 * t;
	CubicWeights out;
	out.w[0] = 0.5f * (-t3 + 2.0f * t2 - t);
	out.w[1] = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
	out.w[2] = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
	out.w[3] = 0.5f * (t3 - t2);
	return out;
}

/// One feature-wise weighted sum, and the weight that actually landed — a tap on a texel nothing built contributes
/// nothing and is not counted, so the edge of the built world does not drag the void into the picture.
struct Accumulated {
	TerrainFeatures features {};
	float weight = 0.0f;
};

void addTap(Accumulated& into, const TerrainFeatures& tap, float weight, bool valid) {
	if (!valid || weight == 0.0f) {
		return;
	}
	into.features.energy += weight * tap.energy;
	into.features.centroid += weight * tap.centroid;
	into.features.roughness += weight * tap.roughness;
	into.features.parity += weight * tap.parity;
	into.features.fundamental += weight * tap.fundamental;
	into.weight += weight;
}

TerrainFeatures normalised(const Accumulated& sum) {
	TerrainFeatures out = sum.features;
	if (sum.weight != 0.0f && sum.weight != 1.0f) {
		const float scale = 1.0f / sum.weight;
		out.energy *= scale;
		out.centroid *= scale;
		out.roughness *= scale;
		out.parity *= scale;
		out.fundamental *= scale;
	}
	return out;
}

int clampIndex(int i) {
	return std::clamp(i, 0, TerrainMap::kResolution - 1);
}

} // namespace

void upsampleTerrain(const TerrainSnapshot& snapshot, int scale, uint32_t* out, size_t outCount) {
	const int resolution = TerrainMap::kResolution;
	if (out == nullptr || scale < 1 || snapshot.features.empty() || snapshot.texels.empty()) {
		return;
	}
	const int wide = resolution * scale;
	if (outCount < static_cast<size_t>(wide) * static_cast<size_t>(wide)) {
		return;
	}
	// SEPARABLE, because the 4x4 tap is the same answer four times the price: the rows are stretched once into a strip
	// `wide` across and `resolution` tall, and the columns of THAT are stretched into the picture.
	static thread_local std::vector<TerrainFeatures> strip;
	static thread_local std::vector<uint8_t> stripValid;
	strip.assign(static_cast<size_t>(resolution) * static_cast<size_t>(wide), TerrainFeatures {});
	stripValid.assign(strip.size(), 0u);

	const float step = 1.0f / static_cast<float>(scale);
	// THE SAMPLE LATTICE: output pixel `i` sits at texel `(i + 0.5)/scale - 0.5`, the centre of the cell it covers, so
	// a whole texel's worth of output straddles that texel rather than leaning off its corner.
	for (int row = 0; row < resolution; ++row) {
		for (int x = 0; x < wide; ++x) {
			const float at = (static_cast<float>(x) + 0.5f) * step - 0.5f;
			const int base = static_cast<int>(std::floor(at));
			const CubicWeights weights = catmullRom(at - static_cast<float>(base));
			Accumulated sum;
			for (int tap = 0; tap < 4; ++tap) {
				const int column = clampIndex(base - 1 + tap);
				addTap(sum, snapshot.featuresAt(column, row), weights.w[tap], snapshot.texel(column, row) != 0u);
			}
			const size_t at_ = static_cast<size_t>(row) * static_cast<size_t>(wide) + static_cast<size_t>(x);
			strip[at_] = normalised(sum);
			stripValid[at_] = sum.weight != 0.0f ? 1u : 0u;
		}
	}

	for (int y = 0; y < wide; ++y) {
		const float at = (static_cast<float>(y) + 0.5f) * step - 0.5f;
		const int base = static_cast<int>(std::floor(at));
		const CubicWeights weights = catmullRom(at - static_cast<float>(base));
		const double rowY = TerrainMap::travelYAtRow(snapshot.window, at);
		for (int x = 0; x < wide; ++x) {
			Accumulated sum;
			for (int tap = 0; tap < 4; ++tap) {
				const int row = clampIndex(base - 1 + tap);
				const size_t from = static_cast<size_t>(row) * static_cast<size_t>(wide) + static_cast<size_t>(x);
				addTap(sum, strip[from], weights.w[tap], stripValid[from] != 0u);
			}
			const size_t to = static_cast<size_t>(y) * static_cast<size_t>(wide) + static_cast<size_t>(x);
			if (sum.weight == 0.0f) {
				out[to] = 0u; // the void, as the texel was
				continue;
			}
			// COLOURED AT THIS PIXEL'S OWN PLACE, which is what makes the grain finer than the grid.
			const double columnX = TerrainMap::travelXAtColumn(snapshot.window,
			                                                   (static_cast<double>(x) + 0.5) / scale - 0.5);
			// **X THEN Y, and this call had them the other way round** (found 2026-09-21 by the backlog
			// GY case sweep). `terrainColour` takes `(travelX, travelY)`; this passed the ROW's Y as the
			// x argument and the COLUMN's x as the y, so every upsampled texel was coloured by the
			// transpose of its own position.
			//
			// IT HAS BEEN DORMANT, NOT HARMLESS. `upsampleTerrain` runs only under `fineWanted_`, which
			// ships OFF — so nothing anybody has looked at was wrong, and the defect was armed for
			// precisely the experiment the switch exists to invite. The header is what hid it: its
			// declaration still named the parameters `(home, travel)` from before the axes became two
			// travels, so a caller reading the header passed exactly what the header asked for. A
			// signature whose names outlive their meaning is worse than one with no names.
			out[to] = terrainColour(normalised(sum), columnX, rowY, snapshot.family, colourMapFor(snapshot.window.engine),
			                        kShippedSecondAxis, snapshot.window.engine);
		}
	}
}

//------------//
// The worker //
//------------//

double TerrainMap::anchoredTravelY(const TerrainWindow& window) {
	// THE SAMPLING LATTICE IS NAILED TO THE WORLD, NOT TO THE VIEW.
	//
	// THE BUG: drop the star in a dark region, let the display resettle after the recentre, and the
	// destination looked like different topography. It was not aliasing in the
	// usual sense: every texel's sample point used to be measured from the window's own centre, so the
	// whole lattice rode the view. Move the centre by a third of a texel — which is what any recentre
	// onto a star is — and sixteen thousand sample points move with it, each landing on a different
	// piece of a world finer than a texel, each coming back a different colour. MEASURED ON THE SHIPPED
	// BUILD: of 12,544 world positions visible in both maps, ONE kept its colour.
	//
	// So the centre is snapped to a whole number of cells before any sample is taken. The picture is
	// then a function of WHERE YOU ARE IN THE WORLD and how far you are zoomed, and of nothing else —
	// pan, recentre, jump, come back, and the same place answers with the same colour. That is the
	// file's binding rule ("a colour that meant one thing before you zoomed and another after is a map
	// that cannot be learned") finally holding under navigation as well as under zoom.
	//
	// ZOOM STILL CHANGES THE LATTICE, unavoidably: a rung is a different cell size and therefore a
	// different grid. What it buys everywhere else is worth naming — a pan reuses its overlap EXACTLY
	// rather than nearly, because both windows quantise onto the same world grid.
	const double cell = window.travelYSpan / static_cast<double>(kResolution);
	if (!(cell > 0.0)) {
		return window.travelYCentre;
	}
	return std::round(window.travelYCentre / cell) * cell;
}

double TerrainMap::anchoredTravelX(const TerrainWindow& window) {
	// The horizontal half of the same rule. Both axes are sampled, so both re-roll if either rides the
	// view — the reported case is a recentre, which moves both at once.
	const double cell = window.travelXSpan / static_cast<double>(kResolution);
	if (!(cell > 0.0)) {
		return window.travelXCentre;
	}
	return std::round(window.travelXCentre / cell) * cell;
}

double TerrainMap::travelYAtRow(const TerrainWindow& window, double row) {
	// Row 0 is the TOP, and the top is the higher home: the chart's primary direction is vertical and
	// drift flies you up over the landscape, which is the mock's own axis convention.
	const double t = (row + 0.5) / static_cast<double>(kResolution);
	return anchoredTravelY(window) + (0.5 - t) * window.travelYSpan;
}

double TerrainMap::travelXAtColumn(const TerrainWindow& window, double column) {
	const double t = (column + 0.5) / static_cast<double>(kResolution);
	// Around the window's own travel centre, not around zero. Drift is an absolute coordinate with no
	// band to be relative to, so unlike the vertical axis there is nothing to translate here — a map
	// built at another travel centre is simply drawn where it belongs and slides out of view.
	return anchoredTravelX(window) + (t - 0.5) * window.travelXSpan;
}

double TerrainMap::rowAtTravelY(const TerrainWindow& window, double home) {
	// The exact inverse of travelYAtRow, anchoring included — a lookup that used the raw centre while the
	// samples used the anchored one would be off by up to half a texel, which is how a carry smears.
	const double t = 0.5 - (home - anchoredTravelY(window)) / window.travelYSpan;
	return t * static_cast<double>(kResolution) - 0.5;
}

double TerrainMap::columnAtTravelX(const TerrainWindow& window, double travel) {
	// Anchored like its partner above, and for the same reason: an inverse that disagreed with the
	// forward map by half a texel would put every carried column half a texel out.
	const double t = (travel - anchoredTravelX(window)) / window.travelXSpan + 0.5;
	return t * static_cast<double>(kResolution) - 0.5;
}

bool TerrainMap::carry(TerrainSnapshot& into, const TerrainSnapshot& from, bool onlyEmpty) {
	if (from.rows <= 0 || from.window.travelYSpan <= 0.0 || from.window.travelXSpan <= 0.0
	    || into.window.travelYSpan <= 0.0 || into.window.travelXSpan <= 0.0) {
		return false;
	}
	// **IS THIS CARRY EXACT, OR IS IT A RESAMPLE?** Asked once for the whole map, because it is a
	// property of the two WINDOWS and not of any texel. Only an exact carry may mark a texel computed
	// (`TerrainSnapshot::computed`): the lookups below round to the nearest source texel, which is the
	// right thing to DRAW while a zoom settles and the wrong thing to call finished.
	//
	// The key comparison is everything but the two centres — pin, residue, gammas, dials, scales,
	// engine. If any of those moved, a texel at the same place is a different colour and geometry is
	// irrelevant. The slip is how far off the shared lattice the two grids sit; `anchoredTravelY` snaps
	// each centre to whole cells, so for a pan this is 0 and for anything else it is not.
	bool exact = false;
	{
		TerrainWindow keyOnly = into.window;
		keyOnly.travelYCentre = from.window.travelYCentre;
		keyOnly.travelXCentre = from.window.travelXCentre;
		const bool sameKey = keyOnly == from.window;
		const bool sameZoom = into.window.travelYSpan == from.window.travelYSpan
		                      && into.window.travelXSpan == from.window.travelXSpan;
		// AND THE SAME LIKES' DERIVATION: the refine re-derives the warp for the same window, so a map built through the round
		// lenses is the same key and not the same picture.
		if (sameKey && sameZoom && into.likeState == from.likeState) {
			const double cellHome = into.window.travelYSpan / static_cast<double>(kResolution);
			const double cellTravel = into.window.travelXSpan / static_cast<double>(kResolution);
			const double dRow = (anchoredTravelY(into.window) - anchoredTravelY(from.window)) / cellHome;
			const double dCol = (anchoredTravelX(into.window) - anchoredTravelX(from.window)) / cellTravel;
			exact = std::abs(dRow - std::round(dRow)) < 1e-6
			        && std::abs(dCol - std::round(dCol)) < 1e-6;
		}
	}
	const bool markComputed = exact && !from.computed.empty()
	                          && into.computed.size() == into.texels.size();
	bool carried = false;
	for (int row = 0; row < kResolution; ++row) {
		// THE ROW IS FOUND BY ITS PLACE WITHIN THE BAND, not by an offset between the two windows and
		// not by absolute home. Only a pan makes an offset the same number, and a zoom is the other half
		// of what this surface does — so it is a position lookup. Which position is the subtle part.
		//
		// `home - gammaSum` IS THE COORDINATE, because gamma translates the whole world: the reachable
		// band is `[gammaSum, gammaSum + 3]`, the zone and offset a home decomposes to are computed from
		// `home - gammaSum` (see homeToCoord), and a scrub writes exactly those. So this is the chart's
		// addressable coordinate, and it is the one that has to line up for a carried texel to be at the
		// same PLACE.
		//
		// WITHOUT THIS A GAMMA GESTURE CARRIES NOTHING. The view follows gamma one for one, so under a
		// sweep both the window's homeCentre and its gammaSum move together and absolute home reads it as
		// an endless pan. Measured at a real drag rate — the corner moves 8.0 of home per encoder travel,
		// so a hand at 100 px/s sweeps about 0.095 of home per frame, one and a half VIEWPORTS — nothing
		// ever overlaps, every build starts from nothing again, and the chart is 43% void on average with
		// worst frames at 87%. Which is the reported blank, arriving by a second road.
		//
		// Band-relative, the same sweep is what it actually is: the view is not moving through the space
		// at all (the sound's place within the band cannot change when gamma moves), the CHARACTER at
		// each place is changing. So every position has a colour to carry, the seed is always full, and
		// the sweep is a wipe-in over live content at any speed the hand can manage.
		//
		// When the two windows share a gamma — a re-centre, a zoom, a rotation, a selection change —
		// this is arithmetically identical to comparing absolute home, so none of those paths change.
		// **THE COORDINATE IS JUST THE DISPLACEMENT NOW** (backlog GY). This used to subtract
		// `into.window.viewGamma` and add `from.window.viewGamma` back, because the chart's addressable
		// coordinate was `home - gammaSum`: the view followed gamma one for one, so two maps a
		// gamma-tick apart had to be compared band-relative or a sweep would carry nothing and the
		// chart would scroll into an endless blank.
		//
		// Both axes are displacements in the decoded plane now. Gamma moves the plane and moves nothing
		// WITHIN it, so two windows are already in the same coordinate and there is no frame to convert
		// between. A carry across different gammas is refused where it matters rather than translated:
		// `gammaBase` is in the content key, so `sameKey` is false and no texel is marked computed.
		const double place = travelYAtRow(into.window, row);
		const long source = std::lround(rowAtTravelY(from.window, place));
		if (source < 0 || source >= from.rows) {
			// Off the end of the old map, or into rows it had not built. Nothing is invented to cover
			// that: it is what a strip filling in at the leading edge of a pan looks like.
			continue;
		}
		for (int column = 0; column < kResolution; ++column) {
			const long sourceColumn =
			    std::lround(columnAtTravelX(from.window, travelXAtColumn(into.window, column)));
			if (sourceColumn < 0 || sourceColumn >= kResolution) {
				continue;
			}
			const size_t at = static_cast<size_t>(row) * kResolution + static_cast<size_t>(column);
			// THE SECOND CARRY FILLS ONLY WHAT THE FIRST LEFT (the prefetched map, `onlyEmpty`): where the map being
			// replaced had ground, its ground is the better answer — same cell size, built for this very country.
			if (onlyEmpty && into.texels[at] != 0u) {
				continue;
			}
			const uint32_t texel = from.texel(static_cast<int>(sourceColumn), static_cast<int>(source));
			// A transparent texel is the old map saying "there is nothing here", which is not worth
			// carrying onto an already-empty buffer — and carrying it would make `carried` claim there
			// is something to draw where there is not.
			if (texel != 0u) {
				into.texels[at] = texel;
				// AND THE MEASUREMENT TRAVELS WITH ITS COLOUR, or a carried row has a colour and nothing to
				// interpolate (`TerrainSnapshot::features`).
				if (at < into.features.size() && !from.features.empty()) {
					into.features[at] = from.featuresAt(static_cast<int>(sourceColumn), static_cast<int>(source));
				}
				// AND ITS SCORE (backlog ON), which is exact wherever the carry is: the likes are in the content key.
				if (at < into.score.size() && !from.score.empty()) {
					into.score[at] = from.score[static_cast<size_t>(source) * kResolution + static_cast<size_t>(sourceColumn)];
				}
				// AND WHETHER IT WAS EVER COMPUTED travels with it, so a texel computed five builds ago
				// and carried exactly ever since is still known to be real. This is what makes a drag's
				// work cumulative: without it every build re-earns ground it already owns.
				if (markComputed) {
					const size_t fromAt = static_cast<size_t>(source) * kResolution
					                      + static_cast<size_t>(sourceColumn);
					if (fromAt < from.computed.size() && from.computed[fromAt] != 0u) {
						into.computed[at] = 1u;
					}
				}
				carried = true;
			}
		}
	}
	// **OR-ED IN, NEVER ASSIGNED — AND THIS LINE IS THE WHOLE OF A BUG** (on the build that fixed
	// the part-built map, the entire circle sometimes disappeared for about one frame, only on an
	// aggressive pan drag).
	//
	// `buildMap` calls this TWICE: once from the map being replaced, then again from the prefetched
	// one with `onlyEmpty`, which copies only where the first carry left a hole. A plain assignment
	// meant the SECOND call's local result overwrote the first's — so when the first carry filled the
	// buffer completely, the second found nothing to do, returned false, and cleared the flag that
	// said the buffer was full.
	//
	// THE CONSEQUENCE IS A BLANK CHART, NOT A SUBTLE ONE. `seeded` is computed as
	// `carry(prefetch) || seeded`, so it stays true and the snapshot IS published — carrying
	// `rows == 0` and `carried == false`. Every draw gate on the surface reads
	// `rows > 0 || carried` (drawGround, groundPaints, haveMap), so that one frame draws no terrain at
	// all: the entire circle vanishes and is back on the next publish.
	//
	// AND IT IS WORST WHEN THE CARRY IS BEST, which is why it survived the part-built fix and got MORE
	// likely afterwards: the more completely the first carry fills the map, the less the prefetch has
	// to do, and the more certain it is to report nothing and clear the flag.
	into.carried = carried || into.carried;
	return carried;
}

TerrainMap::TerrainMap() {
	// EMPTY, AND NO THREAD YET. An editor that never shows Explore never starts one — the surface is
	// what asks first, so request() is where the worker is born.
	auto empty = std::make_shared<TerrainSnapshot>();
	empty->texels.assign(static_cast<size_t>(kResolution) * kResolution, 0u);
	empty->features.assign(static_cast<size_t>(kResolution) * kResolution, TerrainFeatures {});
	current_ = std::move(empty);
}

TerrainMap::~TerrainMap() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stop_.store(true, std::memory_order_relaxed);
		haveWork_ = true;
	}
	wake_.notify_one();
	// JOINED, NOT DETACHED. A worker outliving the object it builds for is the shape of the exit crash
	// this project already carries one of; this one is ours and is closed here. It only ever touches
	// its own buffer and pure functions — and NEVER anything of visage's, which is the property that
	// keeps it clear of the reopen deadlock's class — so the join is short whatever it was doing.
	if (thread_.joinable()) {
		thread_.join();
	}
}

bool TerrainMap::workerStarted() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return started_;
}

std::shared_ptr<const TerrainSnapshot> TerrainMap::snapshot() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return current_;
}

void TerrainMap::publish(std::shared_ptr<const TerrainSnapshot> snapshot) {
	if (publishObserver_) {
		publishObserver_(snapshot);
	}
	std::lock_guard<std::mutex> lock(mutex_);
	current_ = std::move(snapshot);
}

void TerrainMap::observePublishesForTest(PublishObserver observer) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (!started_) {
		publishObserver_ = std::move(observer);
	}
}

void TerrainMap::request(const TerrainWindow& window) {
	if (window.travelYSpan <= 0.0 || window.travelXSpan <= 0.0) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (wanted_ == window) {
			return;
		}
		wanted_ = window;
		haveWork_ = true;
		if (!started_) {
			started_ = true;
			thread_ = std::thread([this] { worker(); });
		}
	}
	// Nothing in the old map belongs to the new window — the map is a pure function of position, so a
	// texel drawn at the wrong place would be a colour that lies about where it is. The snapshot in the
	// editor's hands stays valid and stays DRAWABLE until the first rows of the new one land, because
	// it carries its own window and is drawn in its own coordinates.
	generation_.fetch_add(1, std::memory_order_relaxed);
	wake_.notify_one();
}

namespace {

/// WHETHER TWO WINDOWS ARE OF THE SAME WORLD — everything but where they are looking and how far in. A map of another
/// engine, pin, residue or dial is a picture of somewhere else at the same coordinates, so it may not be carried from.
bool sameWorld(const TerrainWindow& a, const TerrainWindow& b) {
	TerrainWindow keyOnly = a;
	keyOnly.travelYCentre = b.travelYCentre;
	keyOnly.travelXCentre = b.travelXCentre;
	keyOnly.travelYSpan = b.travelYSpan;
	keyOnly.travelXSpan = b.travelXSpan;
	return keyOnly == b;
}

/// The dials every texel of a window is built at — `buildMap`'s own two lines, shared so a like is measured exactly as
/// the texel under it is.
uint16_t windowZone(const TerrainWindow& window) {
	return static_cast<uint16_t>(std::lround(std::clamp(window.zoneBaseline, 0.0, zoneCeiling())));
}

} // namespace

double terrainPlaneHome(const TerrainWindow& window) {
	return sliceHome(static_cast<uint16_t>(std::lround(window.zoneBaseline)), static_cast<float>(window.offsetBaseline),
	                 window.gammaBase);
}

namespace {

/// Every frame `engine` reads, in the full point's order, handed to `each(frame)`.
template <typename Each>
void forEachPlaneFrame(OscType engine, const TerrainFrames& frames, Each&& each) {
	switch (engine) {
	case OscType::Morph: each(frames.morph); break;
	case OscType::Stair: each(frames.stair); break;
	case OscType::V5:
		each(frames.morph);
		each(frames.stair);
		each(frames.vox);
		break;
	case OscType::BasicShapes:
		each(frames.morph);
		each(frames.stair);
		break;
	case OscType::Resonate: each(frames.weave); break;
	case OscType::Additive: each(frames.partials); break;
	case OscType::Pluck: each(frames.partials); break; // the pluck's tone and the string's material read one frame
	case OscType::Drum: each(frames.partials); break; // the drum's seven fields are guests of the Additive rows
	case OscType::kCount: break;
	}
}

size_t placePointOn(OscType engine, const TerrainFrames& frames, double home, double x, double y, std::span<double> out) {
	size_t at = 0;
	forEachPlaneFrame(engine, frames, [&](const auto& frame) {
		constexpr size_t n = std::tuple_size_v<decltype(frame.x)> / 2;
		for (size_t i = 0; i < n && at < out.size(); ++i) {
			out[at++] = frame.phaseFor(i, home, x, y);
		}
		for (size_t i = 0; i < n && at < out.size(); ++i) {
			out[at++] = frame.residue.offset[n + i] + frame.travelTerm(n + i, x, y);
		}
	});
	return at;
}

} // namespace

size_t terrainPlacePoint(const TerrainWindow& window, double x, double y, std::span<double> out) {
	const double home = terrainPlaneHome(window);
	return placePointOn(window.engine, terrainFramesAt(window.pin, home, window.residue), home, x, y, out);
}

PlanePlace terrainPlaceOn(const TerrainWindow& window, std::span<const double> point, double x, double y) {
	PlanePlace place { x, y, 0.0, false };
	const double home = terrainPlaneHome(window);
	const TerrainFrames frames = terrainFramesAt(window.pin, home, window.residue);
	std::array<double, kTerrainLikePoint> here {};
	const size_t count = placePointOn(window.engine, frames, home, x, y, here);
	if (count == 0 || count != point.size()) {
		return place; // no plane on this engine, or a point of another engine's shape: where it was stored
	}
	// THE PLANE IT WAS MADE ON, to the bit: nothing to project.
	if (std::equal(point.begin(), point.end(), here.begin())) {
		place.exact = true;
		return place;
	}
	// THE NEAREST PLACE ON THIS PLANE: least squares over every row, the two travel directions against what the point asks
	// of each row beyond the plane's own standing (home and residue on a phase row, residue on a rate row).
	double s11 = 0.0;
	double s12 = 0.0;
	double s22 = 0.0;
	double t1 = 0.0;
	double t2 = 0.0;
	size_t row = 0;
	const auto rowsOf = [&](const auto& frame, auto&& visit) {
		constexpr size_t n = std::tuple_size_v<decltype(frame.x)> / 2;
		for (size_t i = 0; i < 2 * n; ++i, ++row) {
			const double standing = i < n ? home + frame.residue.offset[i] : frame.residue.offset[i];
			visit(static_cast<double>(frame.x[i]), static_cast<double>(frame.y[i]), point[row] - standing);
		}
	};
	forEachPlaneFrame(window.engine, frames, [&](const auto& frame) {
		rowsOf(frame, [&](double a, double b, double r) {
			s11 += a * a;
			s12 += a * b;
			s22 += b * b;
			t1 += a * r;
			t2 += b * r;
		});
	});
	const double det = s11 * s22 - s12 * s12;
	if (!(det > 1e-300)) {
		return place;
	}
	place.x = (s22 * t1 - s12 * t2) / det;
	place.y = (s11 * t2 - s12 * t1) / det;
	double off = 0.0;
	row = 0;
	forEachPlaneFrame(window.engine, frames, [&](const auto& frame) {
		rowsOf(frame, [&](double a, double b, double r) {
			const double miss = r - (a * place.x + b * place.y);
			off += miss * miss;
		});
	});
	// IN TRAVEL UNITS: the miss against the plane's own scale (the RMS length of its two directions).
	place.offPlane = std::sqrt(off) / std::sqrt(std::max((s11 + s22) * 0.5, 1e-300));
	return place;
}

PlanePlace terrainLikePlaceOn(const TerrainWindow& window, const TerrainLike& like) {
	if (like.pointCount == 0) {
		return { like.x, like.y, 0.0, false };
	}
	return terrainPlaceOn(window, std::span(like.point).first(like.pointCount), like.x, like.y);
}

std::array<float, kTerrainHarmonics> terrainPartialsAt(const TerrainWindow& window, double travelX, double travelY) {
	std::array<float, kTerrainHarmonics> partials {};
	const TerrainFrames frames = terrainFramesAt(window.pin, terrainPlaneHome(window), window.residue);
	(void)terrainFeaturesOn(window.engine, frames, windowZone(window), static_cast<float>(window.offsetBaseline), travelX,
	                        travelY, window.gammaBase, window.scales(), window.warpMode, window.weaveModel,
	                        window.resonateShaping, window.shapesMix, window.additive, partials.data(), window.likeWarp.warp);
	return partials;
}

LikeSpectrum terrainFineSpectrumAt(const TerrainWindow& window, double travelX, double travelY, int samples) {
	const TerrainFrames frames = terrainFramesAt(window.pin, terrainPlaneHome(window), window.residue);
	std::vector<float> cycle(static_cast<size_t>(samples), 0.0f);
	terrainCycleOn(window.engine, frames, windowZone(window), static_cast<float>(window.offsetBaseline), travelX, travelY,
	               window.gammaBase, window.scales(), window.warpMode, window.weaveModel, window.resonateShaping,
	               window.shapesMix, window.additive, cycle, window.likeWarp.warp);
	return likeFineSpectrum(cycle);
}

namespace {

/// A control in [0, 1] moved by a twentieth of its travel, inward from the end it is nearest.
float nudgedUnit(float v) {
	return v + 0.05f <= 1.0f ? v + 0.05f : v - 0.05f;
}
/// A multiplicative control (Resonate's two shapers) moved by a quarter octave.
float nudgedRatio(float v) {
	return v * 1.189207115f;
}

/// **EVERY NON-POSITION CONTROL THAT FEEDS THE CHARACTER** — every field of the window the builder reads beside the two
/// travel coordinates, nudged one at a time (backlog ON's rule 1). NO HAND LIST OF WHAT TO REMOVE: the pre-pass measures
/// what each does and the model decides; a control the engine does not read moves nothing and is ignored there.
struct LikeNudge {
	const char* name;
	void (*apply)(TerrainWindow&);
};
constexpr LikeNudge kLikeNudges[] = {
	{ "smoothness", [](TerrainWindow& w) { w.smoothnessScale = nudgedUnit(w.smoothnessScale); } },
	{ "quantize", [](TerrainWindow& w) { w.quantizeScale = nudgedUnit(w.quantizeScale); } },
	{ "sync", [](TerrainWindow& w) { w.syncScale = nudgedUnit(w.syncScale); } },
	{ "feedback", [](TerrainWindow& w) { w.feedbackScale = nudgedUnit(w.feedbackScale); } },
	{ "points", [](TerrainWindow& w) { w.pointScale = nudgedUnit(w.pointScale); } },
	{ "grit", [](TerrainWindow& w) { w.gritScale = nudgedUnit(w.gritScale); } },
	{ "fm depth", [](TerrainWindow& w) { w.fmDepth = nudgedUnit(w.fmDepth); } },
	{ "warp", [](TerrainWindow& w) { w.warpDepth = nudgedUnit(w.warpDepth); } },
	// THE ZONE DIAL MOVES THE HOME THE PLANE IS TAKEN AT (offset and gamma move the same home, so one stands for all).
	{ "zone", [](TerrainWindow& w) { w.zoneBaseline += w.zoneBaseline + 8.0 <= 1023.0 ? 8.0 : -8.0; } },
	{ "stiffness", [](TerrainWindow& w) { w.resonateShaping.stiffness = nudgedRatio(w.resonateShaping.stiffness); } },
	{ "coupling", [](TerrainWindow& w) { w.resonateShaping.coupling = nudgedRatio(w.resonateShaping.coupling); } },
	{ "sine", [](TerrainWindow& w) { w.shapesMix.sine = nudgedUnit(w.shapesMix.sine); } },
	{ "triangle", [](TerrainWindow& w) { w.shapesMix.triangle = nudgedUnit(w.shapesMix.triangle); } },
	{ "saw", [](TerrainWindow& w) { w.shapesMix.saw = nudgedUnit(w.shapesMix.saw); } },
	{ "square", [](TerrainWindow& w) { w.shapesMix.square = nudgedUnit(w.shapesMix.square); } },
	{ "shape amount", [](TerrainWindow& w) { w.additive.shapeAmount = nudgedUnit(w.additive.shapeAmount); } },
	{ "rolloff", [](TerrainWindow& w) { w.additive.bias.rolloff = nudgedUnit(w.additive.bias.rolloff); } },
	{ "formant", [](TerrainWindow& w) { w.additive.bias.formant = nudgedUnit(w.additive.bias.formant); } },
	{ "odd/even", [](TerrainWindow& w) { w.additive.bias.oddEven = nudgedUnit(w.additive.bias.oddEven); } },
	{ "comb", [](TerrainWindow& w) { w.additive.bias.comb = nudgedUnit(w.additive.bias.comb); } },
	{ "detail", [](TerrainWindow& w) { w.additive.bias.detail = nudgedUnit(w.additive.bias.detail); } },
	{ "off-grid", [](TerrainWindow& w) { w.additive.bias.offGrid = nudgedUnit(w.additive.bias.offGrid); } },
	// PLUCK'S FIVE STRING SCALERS (2026-10-31): controls its picture reads, so the like surface can name them.
	{ "brightness", [](TerrainWindow& w) { w.additive.string.brightness = nudgedUnit(w.additive.string.brightness); } },
	{ "sustain", [](TerrainWindow& w) { w.additive.string.sustain = nudgedUnit(w.additive.string.sustain); } },
	{ "pick", [](TerrainWindow& w) { w.additive.string.pick = nudgedUnit(w.additive.string.pick); } },
	{ "noise", [](TerrainWindow& w) { w.additive.string.noise = nudgedUnit(w.additive.string.noise); } },
	{ "stretch", [](TerrainWindow& w) { w.additive.string.stretch = nudgedUnit(w.additive.string.stretch); } },
	// THE DRUM'S SEVEN SCALERS (2026-11-05), the same.
	{ "drum pitch", [](TerrainWindow& w) { w.additive.drum.pitch = nudgedUnit(w.additive.drum.pitch); } },
	{ "drum length", [](TerrainWindow& w) { w.additive.drum.length = nudgedUnit(w.additive.drum.length); } },
	{ "drum sweep", [](TerrainWindow& w) { w.additive.drum.sweep = nudgedUnit(w.additive.drum.sweep); } },
	{ "drum tone", [](TerrainWindow& w) { w.additive.drum.tone = nudgedUnit(w.additive.drum.tone); } },
	{ "drum noise", [](TerrainWindow& w) { w.additive.drum.noise = nudgedUnit(w.additive.drum.noise); } },
	{ "drum snap", [](TerrainWindow& w) { w.additive.drum.snap = nudgedUnit(w.additive.drum.snap); } },
	{ "drum metal", [](TerrainWindow& w) { w.additive.drum.metal = nudgedUnit(w.additive.drum.metal); } },
};
constexpr auto kLikeControlNames = [] {
	std::array<const char*, std::size(kLikeNudges)> names {};
	for (size_t i = 0; i < names.size(); ++i) {
		names[i] = kLikeNudges[i].name;
	}
	return names;
}();

/// The fingerprints of a window's world at each of `points`, through the builder's road from a FINE cycle
/// (kLikeFineCycle samples); the plane is derived once.
void fingerprintsAt(const TerrainWindow& window, std::span<const WarpPoint> points, std::vector<LikeVector>& out) {
	const TerrainFrames frames = terrainFramesAt(window.pin, terrainPlaneHome(window), window.residue);
	const uint16_t zone = windowZone(window);
	out.reserve(out.size() + points.size());
	std::array<float, kLikeFineCycle> cycle {};
	for (const WarpPoint& p : points) {
		terrainCycleOn(window.engine, frames, zone, static_cast<float>(window.offsetBaseline), p.x, p.y, window.gammaBase,
		               window.scales(), window.warpMode, window.weaveModel, window.resonateShaping, window.shapesMix,
		               window.additive, cycle, window.likeWarp.warp);
		out.push_back(likeFingerprint(likeFineSpectrum(cycle)));
	}
}

/// **THE CHARACTER TERRAIN AS A LIKE SURFACE** — the plane a window's travel spans, the partial envelope the bake
/// measures, and every non-position control the builder reads (kLikeNudges).
class CharacterSurface final : public LikeSurface {
public:
	explicit CharacterSurface(const TerrainWindow& window) : window_(window) {}
	void fingerprintsAt(std::span<const WarpPoint> points, std::vector<LikeVector>& out) const override {
		ui::fingerprintsAt(window_, points, out);
	}
	size_t controlCount() const override { return std::size(kLikeNudges); }
	const char* controlName(size_t control) const override { return kLikeNudges[control].name; }
	void nudgedFingerprintsAt(size_t control, std::span<const WarpPoint> points,
	                          std::vector<LikeVector>& out) const override {
		TerrainWindow moved = window_;
		kLikeNudges[control].apply(moved);
		ui::fingerprintsAt(moved, points, out);
	}

private:
	const TerrainWindow& window_;
};

} // namespace

std::span<const char* const> terrainLikeControls() {
	return kLikeControlNames;
}

std::shared_ptr<const TerrainLikeState> deriveTerrainLikes(const TerrainWindow& window) {
	if (window.likes == nullptr || window.likes->empty()) {
		return nullptr;
	}
	std::vector<WarpPoint> places;
	std::vector<LikeVector> prints;
	for (const TerrainLike& like : *window.likes) {
		// WHERE THE LIKE LANDS ON THIS PLANE (its stored point projected), never the (x, y) of the plane it was made on.
		const PlanePlace at = terrainLikePlaceOn(window, like);
		places.push_back({ at.x, at.y });
		prints.push_back(likeFingerprint(like.spectrum));
	}
	// FEATHERS_WARP_STRENGTH overrides kWarpStrength, for photographing one patch at several strengths side by side.
	double strength = kWarpStrength;
	if (const char* asked = std::getenv("FEATHERS_WARP_STRENGTH")) {
		strength = std::clamp(std::atof(asked), 1.0, 100.0);
	}
	return std::make_shared<const TerrainLikeState>(deriveLikes(CharacterSurface(window), places, prints, strength));
}

std::shared_ptr<const TerrainLikeState> refineTerrainLikes(const TerrainWindow& window, const TerrainLikeState& base,
                                                         const std::function<bool()>& abandon) {
	std::optional<LikeDerivation> refined = refineLikes(CharacterSurface(window), base, abandon);
	return refined ? std::make_shared<const TerrainLikeState>(std::move(*refined)) : nullptr;
}

/// ONE MAP, BUILT — the worker's body, lifted out so a second, wider map can be built by the same code when nothing is
/// waiting (`kPrefetchSpread`). `publishing` is what tells the two apart: the view's map is published row by row as it
/// fills, and the prefetched one is not published at all — it is kept as somewhere for the NEXT window to carry from.
/// TERRAIN THREAD ONLY.
std::shared_ptr<const TerrainSnapshot> TerrainMap::buildMap(const TerrainWindow& window,
                                                            const std::shared_ptr<const TerrainSnapshot>& previous,
                                                            bool publishing) {
	// BUILT PRIVATELY. Nothing here is visible to the editor until it is handed over complete, so
	// there is no moment when a reader can see half a row, a row from the wrong window, or a count
	// that outlived its generation.
	auto building = std::make_shared<TerrainSnapshot>();
	building->window = window;
	building->texels.assign(static_cast<size_t>(kResolution) * kResolution, 0u);
	building->features.assign(static_cast<size_t>(kResolution) * kResolution, TerrainFeatures {});
	building->computed.assign(static_cast<size_t>(kResolution) * kResolution, 0u);
	// **WHAT THE LIKES MAKE OF THIS WORLD** (backlog ON) — derived once per world and set of likes and reused by every pan
	// and zoom of it (`likeKey_`), so a map pays only its per-texel score. Allocated before the carry, which brings exact
	// scores along with exact texels. Nothing at all without likes: the map is today's, to the bit.
	std::shared_ptr<const TerrainLikeState> likes;
	if (kLikesMoveThePlane && window.likes != nullptr && !window.likes->empty()) {
		TerrainWindow key = window;
		key.travelYCentre = key.travelXCentre = key.travelYSpan = key.travelXSpan = 0.0;
		if (likeState_ == nullptr || !(likeKey_ == key)) {
			likeState_ = deriveTerrainLikes(window);
			likeKey_ = key;
			if (std::getenv("FEATHERS_TRACE_TERRAIN_COST") != nullptr && likeState_ != nullptr) {
				std::fprintf(stderr, "[TERRAIN-COST] likes %.1f ms  %d cycles  removed %d  axes %d  effective %.2f\n",
				             likeState_->millis, likeState_->evaluations, likeState_->model.removed,
				             likeState_->model.axes, likeState_->model.effectiveDims);
				std::fflush(stderr);
			}
		}
		likes = likeState_;
		building->likeState = likes;
		building->score.assign(static_cast<size_t>(kResolution) * kResolution, 0.0f);
	}

	// AND IT STARTS FROM THE MAP IT IS REPLACING, placed by chart position — see TerrainSnapshot's
	// `carried`. Published straight away, before a single row is computed, because the whole point
	// is that no frame between here and the first row draws a blank chart. It is not a held map:
	// every row below overwrites it as soon as it is computed.
	bool seeded = carry(*building, *previous);
	// AND THE PREFETCHED MAP UNDER IT, for the country the view being replaced did not cover — see `prefetched_`. Only
	// where that carry left nothing, and only when it is the same world: a prefetch from another engine, pin or dial is
	// a picture of somewhere else and would be a lie at the same coordinates.
	if (prefetched_ && sameWorld(prefetched_->window, window)) {
		seeded = carry(*building, *prefetched_, true) || seeded;
	}
	if (seeded && publishing) {
		publish(std::make_shared<const TerrainSnapshot>(*building));
	}

	// THE SOUND'S OWN TWO DIALS, HELD FOR THE WHOLE MAP. Every row of this chart is a place a scrub
	// could put the selected sound, and a scrub now moves that sound's unbounded gamma base and
	// nothing else — so zone and offset are the same on every row and the
	// only thing that changes down the chart is the base. They are read once, here, for that reason
	// rather than for speed.
	//
	// AND SO THE STAIR FAMILY IS THE SAME ON EVERY ROW, which is the visible half of the design and
	// was decided with eyes open. `family = min(7, zone >> 7)` is a function of the raw dial — here and
	// on the hardware alike (phi_stair.cpp:74-76: gamma reaches the wavetable through the phase
	// offset and never touches the family) — so no scrub can change it and a map striped by family
	// would be advertising eight landscapes this chart cannot take you to. The dial picks the
	// family; the chart travels within it.
	const uint16_t zone = windowZone(window);
	const auto phaseOffset = static_cast<float>(window.offsetBaseline);
	// Negative under PhiMorph, which has no families to anchor to.
	const int family = window.engine == OscType::Stair ? stairFamilyAt(zone) : -1;
	building->family = family;

	// HOW MUCH WORLD ONE TEXEL COVERS, and therefore how many points it is built from. Both spans
	// divide by the same zoom (A13's matched-span rule), so the cell is square in the coordinate the
	// engine builds at and one schedule serves both axes — but they are read separately here rather
	// than assumed equal, because "they happen to match today" is how a window ends up keyed without
	// something it depends on.
	const double cellHome = window.travelYSpan / static_cast<double>(kResolution);
	const double cellTravel = window.travelXSpan / static_cast<double>(kResolution);

	// **THE ONE HOME THIS WHOLE MAP IS TAKEN AT, AND THE ONE PLANE THAT DECODES IT** (backlog GY).
	//
	// THIS IS THE REFACTOR IN TWO LINES. The chart used to draw home down its vertical axis, deriving a
	// plane per ROW from that row's own home — which made the basis a function of the vertical display
	// coordinate alone, and therefore made every structure the basis has run horizontally. Rotate could
	// reshuffle which plane each row got; it could never make the plane vary ALONG a row, so it could
	// never tilt a band. That is the mechanism behind bands that always stayed horizontal.
	//
	// By design the chart is a window on the 2-space `rotate` decodes: both axes are
	// displacements in it, home is where the whole plane is taken, and gamma is what moves that home.
	// So there is exactly ONE basis for the map, both coordinates enter it symmetrically
	// (`slice_basis.h`: `x[i]*travelX + y[i]*travelY`), and no direction in the picture is privileged
	// by construction rather than by averaging.
	//
	// THE COST FALLING FROM 128 BASIS BUILDS TO 1 IS A CONSEQUENCE, NOT THE REASON. It was tempting to
	// reach for it as the justification and it would have been the wrong argument: if the phi triangles
	// made the plane vary across the picture, one basis would draw a flat picture of a curved space and
	// be cheap about it. It is one basis because the home is one home.
	const double planeHome = terrainPlaneHome(window);
	const TerrainFrames planeFrames = terrainFramesAt(window.pin, planeHome, window.residue);
	const int target = terrainSamplesFor(window.engine, cellHome * 1023.0);

	// THE BUILD'S OWN CLOCK. Every terrain cost reported so far was INFERRED from a build RATE
	// under a saturating drag — builds-per-second inverted — which is only the build's duration if
	// the worker never idles, and says nothing about which pass or which engine spent the time.
	// This is the duration itself. CLOCK_MONOTONIC via steady_clock: a duration, never a wall
	// instant.
	const auto buildStarted = std::chrono::steady_clock::now();
	const bool traceCost = std::getenv("FEATHERS_TRACE_TERRAIN_COST") != nullptr;

	// TWO STATES, ROUGH THEN SETTLED, and the first is exactly the map this surface has always drawn.
	// A seventeen-sample texel is seventeen wavetables and a seventeen-times build, which at a wide
	// stop is nearly half a second before anything appears — and the anti-sawtooth machinery, the
	// carry, the whole live-under-a-gesture property rest on a build being 27ms. So the first pass is
	// always ONE sample, published as it goes exactly as before, and the refined pass follows only
	// when nothing else is waiting.
	//
	// A HAND ALWAYS GETS THE FIRST PASS AND NEVER WAITS FOR THE SECOND: the moment a new window
	// arrives the refinement is abandoned, so a drag is served at today's latency at every zoom and
	// the settle happens once the hand stops. The refined pass overwrites the rough map ROW BY ROW in
		// **A PREFETCH GIVES WAY THE INSTANT THE VIEW ASKS FOR ANYTHING.** It is work nobody is waiting for, and holding
		// the thread through it delays the map somebody IS waiting for — two cases went red saying exactly that (a held
		// gesture saw no rebuild at all). The view's own build is not interruptible here, and the note below says why.
		if (!publishing) {
			std::lock_guard<std::mutex> lock(mutex_);
			if (haveWork_ || stop_.load(std::memory_order_relaxed)) {
				return nullptr;
			}
		}
	// place, so what is on screen during it is refined rows above rough ones — a wipe, never a blank,
	// the same shape the rotation rebuild already has.
	// **AND UNDER THE LIKES' WARP THE COUNT FOLLOWS THE WARP'S LOCAL STRETCH** (resampling after the
	// expansion). A texel's cell covers the world its view cell maps to, which the warp stretches: its world span is the
	// view cell's through the warp's Jacobian there. So a squeezed texel, which covers more world, is meaned from more
	// points and does not alias, and an expanded one, which covers less, needs one. FEATHERS_TERRAIN_UNIFORM_K keeps the
	// zoom's schedule for an A/B.
	std::vector<int> localK;
	int widestK = 1;
	// NOT WHILE THE CAP IS ONE (the taste decision, kSampleCap): every count would be one, and the Jacobians cost a bake's
	// worth of warp inversions (MEASURED: +80 ms a map on Morph for nothing). Raising the cap turns it on.
	if (kSampleCap > 1 && likes != nullptr && std::getenv("FEATHERS_TERRAIN_UNIFORM_K") == nullptr) {
		localK.resize(static_cast<size_t>(kResolution) * kResolution, 1);
		// THE WORLD AT EVERY TEXEL CORNER, once — a quarter of the inversions four differences a texel would cost — and each
		// texel's world span from its own corner quad.
		const auto stride = static_cast<size_t>(kResolution + 1);
		std::vector<WarpPoint> corner(stride * stride);
		for (int row = 0; row <= kResolution; ++row) {
			const double v = travelYAtRow(window, row - 0.5);
			for (int column = 0; column <= kResolution; ++column) {
				corner[static_cast<size_t>(row) * stride + column] =
				    likes->warp.worldAt({ travelXAtColumn(window, column - 0.5), v });
			}
		}
		for (int row = 0; row < kResolution; ++row) {
			for (int column = 0; column < kResolution; ++column) {
				const WarpPoint a = corner[static_cast<size_t>(row) * stride + column];
				const WarpPoint b = corner[static_cast<size_t>(row) * stride + column + 1];
				const WarpPoint c = corner[static_cast<size_t>(row + 1) * stride + column + 1];
				const WarpPoint d = corner[static_cast<size_t>(row + 1) * stride + column];
				const double area = 0.5 * std::abs((c.x - a.x) * (d.y - b.y) - (c.y - a.y) * (d.x - b.x));
				const int k = terrainSamplesFor(window.engine, std::sqrt(area) * 1023.0);
				localK[static_cast<size_t>(row) * kResolution + static_cast<size_t>(column)] = k;
				widestK = std::max(widestK, k);
			}
		}
	}
	if (!localK.empty()) {
		building->texelSamples.assign(localK.begin(), localK.end());
	}
	const int refined = localK.empty() ? target : std::max(target, widestK);
	const int passes = refined > 1 ? 2 : 1;
	for (int pass = 0; pass < passes; ++pass) {
	const int samples = pass == 0 ? 1 : refined;

	for (int row = 0; row < kResolution; ++row) {
		// ONLY A SHUTDOWN STOPS A BUILD NOW. It used to abandon the moment the view moved, which is
		// what made a held gesture show nothing: a request per frame restarted the map faster than it
		// could finish, so it never finished. Running each build to completion and then picking up
		// the LATEST target is what turns that into live feedback — see the coalescing note below.
		if (stop_.load(std::memory_order_relaxed)) {
			break;
		}
		// **THIS ROW'S TRAVEL-Y, NOT ITS HOME** (backlog GY). It used to be the row's home, which is
		// what made the vertical display axis the gamma axis. Both axes are displacements in the
		// decoded plane now; the home is `planeHome` below and is the same for every texel of the map.
		const double travelY = travelYAtRow(window, row);
		// THERE IS NO ROW THAT DOES NOT EXIST. The world is meant to be infinite — and the
		// engine already agreed. buildMorphSliced reads `zone` ONLY through
		// `sliceHome(zone, phaseOffset, gammaBase)`, and every slot below it takes the scalar home, so
		// the morph character is a PURE FUNCTION OF HOME and is perfectly well defined anywhere on the
		// line. What `[gammaSum, gammaSum+3]` bounds is not the world; it is what zone and offset can
		// POINT AT while gamma sits where it does. An addressing limit, drawn as if it were the end of
		// the world — this used to paint alpha zero here and call it "nothing".
		//
		// SO THE OVERFLOW GOES INTO THE BASE instead of being clamped away. Zone and offset pin at the
		// edge they ran out at, and the home left over is added to this row's gamma base — which is
		// exactly the coordinate a hand would carry the sound along to stand here. The sum
		// `zone/1023 + phaseOffset + gammaBase` is preserved EXACTLY, so the row draws the true
		// character at its own home rather than the band edge repeated (which is what the old comment
		// here correctly said a clamped decomposition would give you).
		//
		// STAIR SATURATES OUT THERE, decided rather than overlooked: `family = min(7, zone >> 7)` is a
		// discrete function of raw zone, so past the ceiling every row is family 7 and below the floor
		// family 0. By decision, eight families is a real discrete axis with no ninth. The
		// continuous fields go on evolving with home inside that last family, which is what makes the
		// far world alive rather than flat.
		{

			// WHAT A SCRUB TO THIS ROW WOULD WRITE, and it is now one number: this row's own gamma
			// base. The whole difference between one row of the chart and the next lives in it.
			//
			// The two dials are subtracted in the order the builder will add them back, and the float
			// cast happens BEFORE the subtraction, so `sliceHome` reconstructs this row's home
			// exactly rather than nearly. That ordering is also why the base is a double and the
			// phase offset a float: the dials are small and bounded, the base is unbounded, and
			// folding the base into the float would quantise it at about eight zones per ULP by the
			// time it was worth having (sound_gamma.h).
			for (int column = 0; column < kResolution; ++column) {
				// **ALREADY OWNED — DO NOT RE-EARN IT.** An exact carry put a real computed texel here
				// (`TerrainSnapshot::computed`), and this map's window agrees with that one on every
				// field but its centres, on an integer texel offset. So the value this loop would
				// produce is the value already sitting in the buffer, bit for bit.
				//
				// MEASURED BEFORE IT WAS WRITTEN: during a drag pan ~90% of each build was exactly
				// this, recomputed every time. That is why a drag never finished a map and why the
				// chart was drawn part-built on every frame of every pan.
				//
				// PASS 0 ONLY. A refining pass exists to compute the SAME texel at more samples, so a
				// flag saying "computed at one sample" must not excuse it from being computed at
				// sixteen. `kSampleCap` makes that pass unreachable today; this is written so that
				// re-enabling it cannot quietly inherit a rough texel as a finished one.
				if (pass == 0 && !building->computed.empty()
				    && building->computed[static_cast<size_t>(row) * kResolution
				                          + static_cast<size_t>(column)]
				           != 0u) {
					continue;
				}
				// HOW MANY POINTS THIS TEXEL IS MEANED FROM: the schedule's, or under the likes' warp its own (below); on the
				// refining pass a texel whose own count is one already has its value from the first.
				int texelSamples = samples;
				if (pass == 1 && !localK.empty()) {
					texelSamples = localK[static_cast<size_t>(row) * kResolution + static_cast<size_t>(column)];
					if (texelSamples <= 1) {
						continue;
					}
				}
				const double travelX = travelXAtColumn(window, column);
				// THE TEXEL IS A CELL, NOT A POINT — by design, and it is an xy volume rather than a row. At a wide zoom this cell covers hundreds of zones of
				// home and the matching width of travel; one sample in the middle of it is a lucky
				// guess about all the rest. So it is MEANED over `samples` points inside itself, and
				// what comes out is honest blur instead of whichever feature the centre happened to
				// land on. At K=1 — every rung this surface shipped with, for morph — the single
				// sample IS the centre and this loop is exactly the old code.
				//
				// THE FEATURES ARE MEANED; THE GRAIN IS NOT. Averaging the finished pixels would erase
				// the grain, which is a deliberate fixed-frequency texture already undersampled at
				// every rung BY DESIGN (see `grainAt`) — it is not a feature being missed, it is a
				// texture doing its job. So the layer that lies gets summarised and the one that is
				// honest is left alone, at the cell's centre where it always was.
				//
				// THERE IS NO MACRO TO MEAN ANY MORE: the country left the draw by design, and
				// stair's family is constant across a map under the one-family rule, so the only thing
				// a cell can honestly summarise is the character.
				TerrainFeatures mean {};
				for (int sample = 0; sample < texelSamples; ++sample) {
					const TerrainSampleOffset at = terrainSampleOffset(sample, texelSamples);
					// **BOTH OFFSETS ARE TRAVEL NOW.** `at.home` moved the sample along the home axis
					// when that axis was the picture's vertical; it moves it along travel-Y instead,
					// which is what the vertical actually is. The cell is still a cell and the sample
					// pattern still straddles it.
					const double sampleTravelY = travelY + at.home * cellHome;
					const double sampleTravelX = travelX + at.travel * cellTravel;
					// **AND THROUGH THE LIKES' WARP** (backlog ON): the texel is a place in the VIEW's world, and what is built
					// there is the real world point the warp draws at it. The identity without likes, and not even a call.
					const WarpPoint place = likes != nullptr ? likes->warp.worldAt({ sampleTravelX, sampleTravelY })
					                                           : WarpPoint { sampleTravelX, sampleTravelY };
					// **ONE PLANE FOR THE WHOLE MAP, and it is not an optimisation.** M reads the turn
					// and the HOME, and by design every texel of this map shares one home — so
					// there is exactly one basis, and no direction in the picture can be the gamma
					// direction. The per-row derivation this replaces was the mechanism that pinned
					// the banding horizontal: a basis constant along a row can only band along a row,
					// whatever rotate does. It also happens to cost 1 basis build per map instead of
					// 128, which is a consequence rather than the reason.
				const TerrainFeatures one =
				    terrainFeaturesOn(window.engine, planeFrames, zone, phaseOffset, place.x,
				                      place.y, window.gammaBase, window.scales(),
				                      window.warpMode, window.weaveModel, window.resonateShaping, window.shapesMix,
				                      window.additive, nullptr, window.likeWarp.warp);
					mean.energy += one.energy;
					mean.centroid += one.centroid;
					mean.roughness += one.roughness;
					// EVERY FEATURE THE COLOUR READS, or a texel draws with a channel of zero. The two
					// spectral shares were added for the second colour axis and this loop did not carry
					// them at first: the map drew every texel at parity zero, and the reconstruction case
					// below caught it on the first run. A summariser that summarises a subset is worse
					// than one that summarises nothing, because it looks finished.
					mean.parity += one.parity;
					mean.fundamental += one.fundamental;
				}
				const auto scale = 1.0f / static_cast<float>(texelSamples);
				mean.energy *= scale;
				mean.centroid *= scale;
				mean.roughness *= scale;
				mean.parity *= scale;
				mean.fundamental *= scale;
				building->texels[static_cast<size_t>(row) * kResolution + column] =
				    terrainColour(mean, travelX, travelY, family, colourMapFor(window.engine), kShippedSecondAxis, window.engine);
				// AND THE MEASUREMENT ITSELF, for a reader that wants to interpolate what was measured rather
				// than what it was coloured (`TerrainSnapshot::features`).
				building->features[static_cast<size_t>(row) * kResolution + column] = mean;
				// AND HOW LIKE A LIKED SOUND IT IS (backlog ON) — READ OFF THE COARSE GRID the warp was solved on, at the real
				// place drawn at the texel's centre: the fine fingerprint is never taken per texel.
				if (likes != nullptr) {
					const WarpPoint real = likes->warp.worldAt({ travelX, travelY });
					building->score[static_cast<size_t>(row) * kResolution + column] = static_cast<float>(likes->scoreAt(real));
				}
				// AND IT IS NOW REAL, not inherited — the flag the next build will carry.
				building->computed[static_cast<size_t>(row) * kResolution + column] = 1u;
				texelsComputed_.fetch_add(1, std::memory_order_relaxed);
			}
			// Never shrinks: on the refining pass every row already exists and is being replaced,
			// so a reader keeps drawing a whole map throughout.
			building->rows = std::max(building->rows, row + 1);
		}

		// A copy every few rows rather than one per row: the fill stays visible and the builder
		// spends microseconds on it. The copy is what makes the handover safe — the editor gets a
		// map nobody will write to again.
		if ((row + 1) % kPublishEvery == 0 || row + 1 == kResolution) {
			// ONLY ONCE THE WHOLE MAP HOLDS IT. A refining pass replaces rows from the top down, so
			// until it reaches the bottom the map is part refined and part rough — and a snapshot
			// claiming the finer count mid-pass tells a reader the picture has settled while most of
			// it has not. Two red cases to say that precisely.
			if (row + 1 == kResolution) {
				building->samples = samples;
				// **THE CUBIC UPSAMPLE, BEHIND A LIVE SWITCH** (`TerrainMap::fineWanted`, a debug toggle
				// in the editor while the choice is open).
				//
				// IT WAS REMOVED ON A MEASUREMENT THAT COULD NOT SEE WHAT IT WAS FOR. Two STILL frames, cubic
				// against the atlas's own bilinear, RMSE 1.21 of 255 — and a still comparison is blind to the
				// thing a player reports, which is what happens while the picture MOVES. At 128 texels stretched
				// roughly four times, a sub-pixel pan resamples every texel edge differently on every frame; at
				// 512 the magnification is near 1:1 and it does not. So the number was right and the question was
				// wrong, which is the rule about difference checks being weakest exactly where a bug is subtle.
				//
				// A SWITCH RATHER THAN A RE-REMOVAL, because "is this what you are seeing" is an eye's question
				// and there is no instrument here that answers it. Default OFF — what is installed today — so
				// flipping it ON is the experiment.
				if (samples == refined && fineWanted_.load(std::memory_order_relaxed)) {
					const auto upsampleStarted = std::chrono::steady_clock::now();
					building->fineScale = kFineScale;
					building->fine.assign(static_cast<size_t>(kResolution * kFineScale)
					                          * static_cast<size_t>(kResolution * kFineScale),
					                      0u);
					upsampleTerrain(*building, kFineScale, building->fine.data(), building->fine.size());
					if (traceCost) {
						std::fprintf(stderr, "[TERRAIN-COST] upsample %.1f ms\n",
						             std::chrono::duration<double, std::milli>(
						                 std::chrono::steady_clock::now() - upsampleStarted)
						                 .count());
						std::fflush(stderr);
					}
				}
			}
			if (publishing) {
				publish(std::make_shared<const TerrainSnapshot>(*building));
			}
		}
	}

	// AND THE REFINEMENT YIELDS TO A HAND: if a new window is already waiting, there is no sense
	// spending sixteen more samples a texel on a view nobody is looking at any more.
	if (stop_.load(std::memory_order_relaxed)) {
		break;
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (haveWork_) {
			break;
		}
	}
	}


	if (traceCost) {
		const double ms =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStarted).count();
		std::fprintf(stderr, "[TERRAIN-COST] build %llu %s %.1f ms  homeSpan %.6f travelSpan %.6f homeCentre %.4f\n",
		             static_cast<unsigned long long>(buildsStarted_.load(std::memory_order_relaxed)),
		             publishing ? "view" : "prefetch", ms, window.travelYSpan, window.travelXSpan, window.travelYCentre);
		std::fflush(stderr);
	}
	return std::make_shared<const TerrainSnapshot>(*building);
}

void TerrainMap::worker() {
	while (true) {
		TerrainWindow window;
		std::shared_ptr<const TerrainSnapshot> previous;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return haveWork_; });
			if (stop_.load(std::memory_order_relaxed)) {
				return;
			}
			haveWork_ = false;
			window = wanted_;
			// The map being replaced, taken under the same lock that hands out the window. Read-only
			// from here — it is what the editor is drawing this instant, and immutability is what makes
			// reading it from this thread safe at all.
			previous = current_;
		}
		buildsStarted_.fetch_add(1, std::memory_order_relaxed);

		// WHAT THIS BUILD COULD HAVE REUSED FROM THE ONE BEFORE IT — read-only, opt-in, and here
		// because the question "does a pan throw its overlap away" needs a NUMBER rather than a
		// reading of the loop below.
		//
		// EXACT COINCIDENCE ONLY. `anchoredTravelY`/`anchoredTravelX` snap the lattice to whole cells, so
		// two windows at the same zoom have grids that are an INTEGER number of texels apart and an
		// overlapping texel has bit-identical (home, travel). That is the only overlap a cache could
		// take without inventing a value, so it is the only one counted. A near-miss is a miss.
		//
		// AND THE CONTENT KEY GATES IT. A texel's value is a pure function of (home, travel) AND every
		// other field the builder reads — the pin, the residue, the dials, the scales, warp. If any of
		// those moved, every texel is stale wherever it sits, and geometry is irrelevant. Reporting a
		// geometric overlap without this gate would report a saving no cache could actually take.
		if (std::getenv("FEATHERS_TRACE_TERRAIN_REUSE") != nullptr) {
			const TerrainWindow& was = previous->window;
			TerrainWindow keyOnly = window;
			keyOnly.travelYCentre = was.travelYCentre;
			keyOnly.travelXCentre = was.travelXCentre;
			const bool sameKey = keyOnly == was;          // everything but the two centres
			const bool sameZoom = window.travelYSpan == was.travelYSpan
			                      && window.travelXSpan == was.travelXSpan;
			long rows = 0;
			long columns = 0;
			double rowSlip = 0.0;
			double columnSlip = 0.0;
			if (sameKey && sameZoom && previous->rows > 0) {
				const double cellHome = window.travelYSpan / static_cast<double>(kResolution);
				const double cellTravel = window.travelXSpan / static_cast<double>(kResolution);
				const double dRow = (anchoredTravelY(window) - anchoredTravelY(was)) / cellHome;
				const double dCol = (anchoredTravelX(window) - anchoredTravelX(was)) / cellTravel;
				// HOW FAR OFF THE LATTICE THE TWO WINDOWS ARE, reported rather than assumed: if the
				// snap is doing its job this is ~0, and if it is not then "exact reuse" is a fiction
				// and the number below is the one that says so.
				rowSlip = std::abs(dRow - std::round(dRow));
				columnSlip = std::abs(dCol - std::round(dCol));
				rows = std::max<long>(0, kResolution - std::lround(std::abs(std::round(dRow))));
				columns = std::max<long>(0, kResolution - std::lround(std::abs(std::round(dCol))));
			}
			std::fprintf(stderr,
			             "[TERRAIN-REUSE] build %llu sameKey %d sameZoom %d reusable %ld of %d "
			             "(%.1f%%) rows %ld cols %ld slip %.4f/%.4f\n",
			             static_cast<unsigned long long>(
			                 buildsStarted_.load(std::memory_order_relaxed)),
			             sameKey ? 1 : 0, sameZoom ? 1 : 0, rows * columns, kResolution * kResolution,
			             100.0 * static_cast<double>(rows * columns)
			                 / static_cast<double>(kResolution * kResolution),
			             rows, columns, rowSlip, columnSlip);
			std::fflush(stderr);
		}

		// THE VIEW'S MAP, published as it fills.
		std::shared_ptr<const TerrainSnapshot> built = buildMap(window, previous, true);

		// **AND THE LIKES' REFINE, WHEN NOTHING IS WAITING** (backlog ON): the first map after the likes change is drawn with
		// round lenses at once; the coarse-to-fine centres and the score-shaped lenses follow here, off every redraw's path,
		// giving way the moment the view asks for anything — and the same view is then built again through them.
		// FEATHERS_LIKES_NO_REFINE keeps the round lenses, for an A/B capture.
		if (likeState_ != nullptr && !likeState_->refined && built->rows == kResolution && window.likes != nullptr
		    && std::getenv("FEATHERS_LIKES_NO_REFINE") == nullptr
		    && !window.likes->empty()) {
			const auto abandon = [this] {
				std::lock_guard<std::mutex> lock(mutex_);
				return haveWork_ || stop_.load(std::memory_order_relaxed);
			};
			if (!abandon()) {
				if (std::shared_ptr<const TerrainLikeState> refined = refineTerrainLikes(window, *likeState_, abandon)) {
					likeState_ = std::move(refined);
					if (std::getenv("FEATHERS_TRACE_TERRAIN_COST") != nullptr) {
						std::fprintf(stderr, "[TERRAIN-COST] likes refine %.1f ms, %zu lenses\n", likeState_->refineMillis,
						             likeState_->warp.lenses.size());
						std::fflush(stderr);
					}
					if (!abandon()) {
						built = buildMap(window, built, true);
					}
				}
			}
		}

		// **AND THEN, WITH NOTHING WAITING, A WIDER ONE AT LOW PRIORITY — SO A PAN HAS GROUND ALREADY** (2026-09-17:
		// a low-priority pre-emptive render makes panning smoother). A pan asks for a window
		// the last map only partly covers, and the part it does not cover arrives row by row: that is the edge that
		// wipes in. This builds the same country at `kPrefetchSpread` times the span BEFORE it is asked for — same
		// texel count, so it is coarser, and it is never drawn directly. It is somewhere for the next window to CARRY
		// from, which is the difference between a pan exposing void and a pan exposing a softer version of what is
		// there. It runs only when the hand has stopped asking, and the first thing the loop does after it is check
		// whether it has started again.
		{
			// **AND ONLY AFTER A QUIET PAUSE.** Starting it the instant a build ended put a whole prefetch between the
			// hand and the map it was waiting for, and a case caught exactly that: a held gesture saw no build start at
			// all, because the worker was busy with work nobody had asked for. It waits for the view to stop asking
			// first, and gives way the moment it asks again (the row check in `buildMap`).
			bool idle = false;
			{
				std::unique_lock<std::mutex> lock(mutex_);
				const bool asked = wake_.wait_for(lock, std::chrono::milliseconds(kPrefetchQuietMs), [this] {
					return haveWork_ || stop_.load(std::memory_order_relaxed);
				});
				idle = !asked;
			}
			if (idle && !stop_.load(std::memory_order_relaxed) && built->rows == kResolution) {
				TerrainWindow wider = window;
				wider.travelYSpan = window.travelYSpan * kPrefetchSpread;
				wider.travelXSpan = window.travelXSpan * kPrefetchSpread;
				// A PREFETCH THAT GAVE WAY RETURNS NOTHING, and the one before it is better than none.
				if (std::shared_ptr<const TerrainSnapshot> wide = buildMap(wider, built, false)) {
					prefetched_ = std::move(wide);
					prefetches_.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}

		// COALESCED, AND `wanted_` IS WHERE THAT LIVES: it is a single slot, so a hundred requests during
		// one build leave exactly one window behind and `request()` has already set `haveWork_` for it.
		// The loop then picks up wherever the view is NOW rather than each place it passed through, one
		// build in flight, never a queue — and the reader keeps drawing the last completed map meanwhile,
		// which the immutable snapshot makes safe by construction.
		//
		// There used to be a re-arm here, and it was DEAD CODE: it set `haveWork_` under exactly the
		// condition in which `request()` had already set it. A reviewer tried twice to make it matter and
		// could not, and its comment claimed to be the whole throttle — so a future reader would have
		// protected the wrong lines. The throttle is the single slot, and it is above, not here.
		//
		// No timer, because the work IS the clock. If builds ever get cheap enough for this to be too
		// eager, `requests()` and `builds()` are there to say so first.
	}
}

} // namespace feathers::ui

namespace feathers::ui {

static_assert(kTerrainLikeAngles == kAngleSites, "terrain_likes.h restates dsp/phase_warp.h's site count");

void terrainLikeAngles(const TerrainWindow& window, double x, double y, std::array<float, kTerrainLikeAngles>& out) {
	const double home = terrainPlaneHome(window);
	const TerrainFrames frames = terrainFramesAt(window.pin, home, window.residue);
	const auto zone = static_cast<uint16_t>(std::lround(window.zoneBaseline));
	const auto offset = static_cast<float>(window.offsetBaseline);
	// THE BUILDS THE VOICE MAKES AT THIS PLACE, each recording its own sites (dsp/phase_warp.h's kind scopes).
	AngleRecorder record(out);
	const OscType e = window.engine;
	if (e == OscType::V5 || e == OscType::Morph || e == OscType::BasicShapes) {
		(void)buildMorphSliced(zone, offset, x, y, frames.morph, window.gammaBase, window.scales().grit);
	}
	if (e == OscType::V5 || e == OscType::BasicShapes) {
		(void)flagshipAuthorsAt(frames.morph, frames.stair, home, x, y);
	}
	if (e == OscType::V5) {
		(void)buildVoxSliced(zone, offset, x, y, frames.vox, window.gammaBase);
	}
	if (e == OscType::Additive) {
		partials::PartialSpectrum spectrum;
		buildPartialSound(frames.partials, home, x, y, spectrum, window.smoothnessScale, window.additive.bias);
	}
	if (e == OscType::Pluck) {
		// The pluck's tone, as the bank reads it on Pluck (neutral bias), and the string's material: both kinds' sites.
		partials::PartialSpectrum spectrum;
		buildPartialSound(frames.partials, home, x, y, spectrum, window.smoothnessScale, {});
		(void)readStringFields(frames.partials, home, x, y);
	}
	if (e == OscType::Drum) {
		// The drum's seven fields, its one set of sites.
		(void)readDrumFields(frames.partials, home, x, y);
	}
	if (e == OscType::Stair) {
		(void)buildStairSliced(zone, offset, x, y, frames.stair, window.gammaBase);
	}
	if (e == OscType::Resonate) {
		(void)buildWeaveSliced(zone, offset, x, y, frames.weave, window.gammaBase);
	}
}

std::array<bool, kAngleSites> likeLiveSites(OscType engine) {
	std::array<bool, kAngleSites> live {};
	const auto open = [&live](AngleKind kind) {
		const auto k = static_cast<size_t>(kind);
		for (size_t i = 0; i < kAngleSitesOf[k]; ++i) {
			live[kAngleOffsetOf[k] + i] = true;
		}
	};
	switch (engine) {
	case OscType::V5:
		open(AngleKind::Morph);
		open(AngleKind::Authors);
		open(AngleKind::Vox);
		break;
	case OscType::BasicShapes:
		open(AngleKind::Morph);
		open(AngleKind::Authors);
		break;
	case OscType::Morph: open(AngleKind::Morph); break;
	case OscType::Additive: open(AngleKind::Partials); break;
	case OscType::Pluck:
		open(AngleKind::Partials); // the pluck's tone
		open(AngleKind::String);   // and the string's material
		break;
	case OscType::Drum: open(AngleKind::Drum); break;
	case OscType::Stair: open(AngleKind::Stair); break;
	case OscType::Resonate: open(AngleKind::Weave); break;
	default: break;
	}
	return live;
}

LikeWarp deriveLikeWarp(const TerrainWindow& window) {
	if (window.likes == nullptr || window.likes->empty()) {
		return {};
	}
	const std::array<bool, kAngleSites> live = likeLiveSites(window.engine);
	if (std::none_of(live.begin(), live.end(), [](bool b) { return b; })) {
		return {};
	}
	std::vector<std::array<float, kAngleSites>> angles;
	for (const TerrainLike& like : *window.likes) {
		angles.push_back(like.angles);
	}
	auto warp = std::make_shared<PhaseWarp>(buildPhaseWarp(angles, live, window.likeStrength, window.likeParams));
	LikeWarp out;
	out.warp = warp.get();
	out.key = warp->contentKey();
	out.owner = std::move(warp);
	return out;
}

} // namespace feathers::ui
