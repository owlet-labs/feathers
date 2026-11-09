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

#include "dsp/flagship.h"

#include "dsp/fm_carrier.h"
#include "dsp/phase_warp.h"

#include "dsp/character_bank.h"
#include "dsp/morph_renderer.h"
#include "dsp/vox_bake.h"

#include <algorithm>
#include <cmath>

namespace feathers {

namespace {

using namespace owlet::dsp;

/// THE SOURCE READ — the one line rule V22c is about, and every read the chain makes goes
/// through it.
///
/// With no vowel this is `evalMorphTable(r.morph, p)`: the same call, the same argument, the same
/// bits. That is not an optimisation of a blend at weight zero — it is the absence of the blend,
/// which is what lets the identity claim be structural rather than a measurement that happens to
/// come out even.
/// A WRAPPED UNIFORM CATMULL-ROM through `n` control points, at phase `p`.
///
/// THE WRAP IS THE INDEXING, not a repair. Every one of the four taps goes through the same
/// `((i % n) + n) % n`, so the segment spanning the last point and the first reaches back into the
/// cycle for its outer neighbours exactly as any interior segment does. There is no seam to fix
/// afterwards because the arithmetic never makes one — which is the half of the brief that a
/// "some kind of fix would address the wrap" spelling would have got wrong.
///
/// UNIFORM, because the points have already been renormalised to even spacing — that is the other
/// half of the instruction, and it is what makes this a plain `p · n` rather than a segment search.
///
/// AT n = 2 THIS IS A SINE, to the resolution the terrain lens can measure (centroid 0.0032 against
/// a triangle's 0.0599, 100% of positions inside the siney threshold). Two points, each reached
/// twice per cycle as its own neighbour, give the cubic exactly the symmetry a half-period has.
double smoothCurveAt(const float* amp, int n, double p) {
	const double scaled = p * static_cast<double>(n);
	const auto cell = static_cast<int>(std::floor(scaled));
	const double u = scaled - static_cast<double>(cell);
	// THE SAME INDEX AS `((i % n) + n) % n`, WITHOUT THE DIVISIONS (backlog PB): a wrapped phase puts `cell` in
	// [0, n], so a tap is all but always within one period of the table and one add or subtract lands it. Sixteen integer
	// divisions per smooth read, 256 reads per bake: MEASURED in Book of Dreams, about 3 us of a 64 us vowel bake (the
	// smooth read went 12.4 to 9.5 us). The modulo stays the road outside that range, so every `i` gets the old index.
	const auto at = [amp, n](int i) {
		const int k = i < 0 ? (i >= -n ? i + n : ((i % n) + n) % n) : (i < n ? i : (i < 2 * n ? i - n : i % n));
		return static_cast<double>(amp[static_cast<size_t>(k)]);
	};
	const double a = at(cell - 1);
	const double b = at(cell);
	const double c = at(cell + 1);
	const double d = at(cell + 2);
	return 0.5
	       * ((2.0 * b) + (-a + c) * u + (2.0 * a - 5.0 * b + 4.0 * c - d) * u * u
	          + (-a + 3.0 * b - 3.0 * c + d) * u * u * u);
}

/// The smooth source at `p`: the curve through `pointCount` control points, with the fractional part
/// of that count crossfading between the two whole-number curves either side of it.
///
/// WHY A CROSSFADE RATHER THAN A GROWING SEGMENT. The quantizer solves the same continuity problem
/// with a fractional stepCount whose last step is truncated to zero width, and that idiom does NOT
/// transfer here: a step of zero width contributes nothing, but a CONTROL POINT of zero width still
/// appears in its neighbours' tangents — a cubic reads four points per segment — so K would still
/// jump. Two curves and a lerp is the honest fix. It costs a second evaluation, paid 256 times per
/// bake rather than per sample, because any position with smoothness in it is on the grid road.
double smoothSourceAt(const FlagshipRecipe& r, double p) {
	const double wrapped = p - std::floor(p);
	if (r.smoothFromShapes) {
		// BASIC SHAPES (backlog KV): the mix sampled at K even points, a row per K (FlagshipRecipe::smoothShapes).
		const double want = std::clamp(static_cast<double>(r.pointCount), static_cast<double>(kFlagshipMinSmoothPoints),
		                               static_cast<double>(kFlagshipMaxSmoothPoints));
		const auto lo = static_cast<int>(std::floor(want));
		const double frac = want - static_cast<double>(lo);
		const double low = smoothCurveAt(r.smoothShapes[static_cast<size_t>(lo)].data(), lo, wrapped);
		if (frac <= 0.0 || lo >= kFlagshipMaxSmoothPoints) {
			return low;
		}
		return low + frac * (smoothCurveAt(r.smoothShapes[static_cast<size_t>(lo + 1)].data(), lo + 1, wrapped) - low);
	}
	const int most = std::min(r.smoothPoints, kFlagshipMaxSmoothPoints);
	const double want =
	    std::clamp(static_cast<double>(r.pointCount), static_cast<double>(kFlagshipMinSmoothPoints),
	               static_cast<double>(most));
	const auto lo = static_cast<int>(std::floor(want));
	const double frac = want - static_cast<double>(lo);
	const double low = smoothCurveAt(r.smoothAmp.data(), lo, wrapped);
	if (frac <= 0.0 || lo >= most) {
		return low;
	}
	return low + frac * (smoothCurveAt(r.smoothAmp.data(), lo + 1, wrapped) - low);
}

double flagshipSourceAt(const FlagshipRecipe& r, const FlagshipSource& source, double p) {
	// BASIC SHAPES (backlog KV): the shape mix IS the source, in place of the morph table.
	if (source.shapes != nullptr) {
		return static_cast<double>(evalShapes(*source.shapes, source.shapesMip, p));
	}
	double morph = static_cast<double>(evalMorphTable(r.morph, p));
	// THE SMOOTH SOURCE IS NO LONGER HERE — it moved to the END of evalFlagship, after quantize. See
	// the note there. What stays in this function is the vowel, which genuinely is a SOURCE: it is one
	// of the two authors of the cycle the operators then work on.
	if (source.vox == nullptr) {
		return morph;
	}
	// The crossfade, at cycle-grid level: the two authors of one cycle, mixed before a single
	// operator has looked at it. Both grids are the same 256 slots (kVoxSlots is kFlagshipSlots
	// deliberately), so there is no resampling step hiding in here.
	//
	// AND IT COMPOSES WITH THE SMOOTH SOURCE WITHOUT A RULE ABOUT ORDER, which is worth saying
	// because it looks like it should need one. The interpolant does not compete with the vowel for
	// this seam: it changes what the MORPH HALF of this blend is, and the vowel is then blended
	// toward from wherever that half landed. Three sources, one line, no branch between them.
	const double vox = static_cast<double>(evalVoxCycle(*source.vox, p));
	return morph + static_cast<double>(source.presence) * (vox - morph);
}

/// The sync + feedback half of the chain — everything the quantizer holds and blends against.
///
/// Sync is the classic wavetable spelling: the cycle read at `ratio` times its own rate, resetting
/// where the outer phase wraps. evalMorphTable wraps its argument, so `p * ratio` needs no frac of
/// its own — and at ratio exactly 1.0 the product is `p` bit for bit, which is half of how the
/// identity corner stays exact.
///
/// Feedback folds the SYNCED cycle through itself: y ← C(p + depth·y), a fixed number of times.
/// Bounded and deterministic — a pure function of `p` with no per-sample state, which is what lets
/// the bake, the wave display and the terrain all call it and mean the same waveform. Skipped
/// entirely at zero depth: `p + 0·y` would come back exact anyway, but three evaluations of it
/// would not come back free, and zero is what the field says across ~90% of the terrain.
/// THE SOURCE, READ THROUGH THE WARP — the one place a warp enters the engine.
///
/// IT WRAPS `flagshipSourceAt` RATHER THAN LIVING INSIDE IT, so the identity is structural: at
/// `WarpMode::Off` this is a branch and a call, and the call is the same call the engine has always
/// made with the same argument. Nothing downstream can tell warp exists.
///
/// WRAPPED HERE, because `warpRead` wants a phase already in [0,1) and the chain hands it phases
/// that are not — `p * ratio` for sync, `(p + depth*y) * ratio` for feedback. `evalMorphTable` wraps
/// its own argument, which is what let the chain be careless about it until now; a warp cannot be,
/// because a bend of 1.7 is not a bend of 0.7.
double warpedSourceAt(const FlagshipRecipe& r, const FlagshipSource& source, double x) {
	if (r.warpMode == WarpMode::Off) {
		return flagshipSourceAt(r, source, x);
	}
	const double wrapped = x - std::floor(x);
	const WarpRead w = warpRead(r.warpMode, wrapped, r.warpDepth);
	return static_cast<double>(w.sign) * flagshipSourceAt(r, source, w.phase);
}

double flagshipChain(const FlagshipRecipe& r, const FlagshipSource& source, double p) {
	const double ratio = static_cast<double>(r.syncRatio);
	// WARP INSIDE THE SYNC, NOT OUTSIDE IT — `warp(frac(p·ratio))`, so each sync repetition is bent
	// the same way. Warping the OUTER phase instead would bend the sequence of repetitions rather
	// than the cycle they read, which is a different effect and not the one Serum has.
	double y = warpedSourceAt(r, source, p * ratio);
	if (r.feedback > 0.0f) {
		const double depth = static_cast<double>(r.feedback) * static_cast<double>(kFlagshipFeedbackMax);
		for (int i = 0; i < kFlagshipFeedbackIterations; ++i) {
			// AND INSIDE THE FEEDBACK, so the loop folds the WARPED cycle through itself. That is the
			// composition the operator tier already has for sync, and warp joins it rather than
			// sitting in front of the whole chain.
			y = warpedSourceAt(r, source, (p + depth * y) * ratio);
		}
	}
	return y;
}

/// One stair-machinery triangle, read where the sound's stair frame says it should be read — the
/// expression evalSlot spells in slice_build.cpp, restated because that one is file-local and the
/// position-purity case pins the two routes to the bit.
float stairAuthorRead(const StairBasis& frame, size_t slot, double home, double tx, double ty,
                      const phi::PhiTriConfig& cfg) {
	return phi::evalTriangle(frame.phaseFor(slot, home, tx, ty), frame.rateFor(slot, tx, ty), cfg);
}

/// The circular three-tap binomial the stair mips use, on the flagship's wider grid. Same shape,
/// same guard-slot lean; the response at harmonic k is cos²(π k / 256).
void flagshipBinomialPass(const float* src, float* dst) {
	for (int j = 0; j < kFlagshipSlots; j++) {
		const int prev = (j == 0) ? kFlagshipSlots - 1 : j - 1;
		dst[j] = 0.25f * (src[prev] + 2.0f * src[j] + src[j + 1]);
	}
	dst[kFlagshipSlots] = dst[0];
}


} // namespace

/// The two coefficients applied, and the only place they are. One subtract and one multiply.
float flagshipNormalised(const FlagshipRecipe& r, double raw) {
	return static_cast<float>((raw - static_cast<double>(r.dcOffset)) * static_cast<double>(r.gain));
}

float fmOffsetCycles(float indexCycles, double modulatorPhase, double polarity) {
	return static_cast<float>(polarity * static_cast<double>(indexCycles)
	                          * static_cast<double>(fm::evalCarrier(modulatorPhase)));
}

FlagshipFields flagshipFieldsAt(double coordinate) {
	FlagshipFields fields;
	// freqMult 1.0f, as the blue-shift read and the orientation passes: the rate half of the slice
	// belongs to a sound's travel, and a field over the space is not travelling.
	fields.quantize = phi::evalTriangle(coordinate, 1.0f, kFlagshipFieldBank[0]);
	fields.sync = phi::evalTriangle(coordinate, 1.0f, kFlagshipFieldBank[1]);
	// The grey-goo threshold: only the top quarter of the triangle survives, rescaled so a peak is
	// still a whole peak. max(0, tri − t)/(1 − t), continuous at the threshold and zero below it.
	const float raw = phi::evalTriangle(coordinate, 1.0f, kFlagshipFieldBank[2]);
	fields.feedback = std::max(0.0f, raw - kFlagshipFeedbackThreshold)
	                  * (1.0f / (1.0f - kFlagshipFeedbackThreshold));
	// The smooth source's two, read at the SAME coordinate as the operator depths and the species —
	// rule V7's lead row, because these are terrain like everything else here.
	fields.smoothness = phi::evalTriangle(coordinate, 1.0f, kFlagshipSmoothField);
	// K rises with its field: at the trough the cycle keeps two points (the sine end) and at the
	// crest it keeps every point the morph table authored. Continuous in between, which is what the
	// fractional crossfade in smoothSourceAt exists to honour.
	fields.pointCount =
	    static_cast<float>(kFlagshipMinSmoothPoints)
	    + static_cast<float>(kFlagshipMaxSmoothPoints - kFlagshipMinSmoothPoints)
	          * phi::evalTriangle(coordinate, 1.0f, kFlagshipPointField);
	// THE FM TRIPLE, at the same coordinate for the same reason — FM doc §9 settled that every FM
	// quantity except the depth knob is terrain. Mapped THROUGH a range rather than used raw: a ratio
	// of zero is a modulator at DC and a denominator of zero is a division, and neither is reachable
	// when both ends of the map are at or above one.
	fields.fmRatio = kFlagshipFmRatioMin
	                 + (kFlagshipFmRatioMax - kFlagshipFmRatioMin)
	                       * phi::evalTriangle(coordinate, 1.0f, kFlagshipFmRatioField);
	fields.fmDenominator = kFlagshipFmDenomMin
	                       + (kFlagshipFmDenomMax - kFlagshipFmDenomMin)
	                             * phi::evalTriangle(coordinate, 1.0f, kFlagshipFmDenomField);
	fields.fmStereoWidth = phi::evalTriangle(coordinate, 1.0f, kFlagshipFmWidthField);
	return fields;
}

FlagshipAuthors flagshipAuthorsAt(const MorphBasis& morphFrame, const StairBasis& stairFrame,
                                  double home, double travelX, double travelY) {
	const AngleKindScope sites(AngleKind::Authors); // the phase warp's operator-author sites (dsp/phase_warp.h)
	FlagshipAuthors authors;
	// THE LEAD COORDINATE (rule V7): home + residue.morph[0] + x[0]·tx + y[0]·ty — the same
	// arithmetic every character triangle reads through, on the row that swings most per unit of
	// anything. Residue-protected, so rotate-at-drift leaves the standing sound's operators alone.
	authors.lead = flagshipLeadCoordinate(morphFrame, home, travelX, travelY);
	authors.fields = flagshipFieldsAt(authors.lead);

	// The quantize grid, authored by the stair machinery's own triangles through this sound's own
	// stair frame — buildStairSliced's formulas exactly, minus the zone-family special cases, which
	// belong to the stair engine (the flagship has no family). Count is continuous in [2, 16]; the
	// slope is squared so most of the map stays steppy, exactly as the staircase itself does.
	const float triCount = stairAuthorRead(stairFrame, stair_slot::kCount, home, travelX, travelY,
	                                       kPhiStairCount);
	authors.stepCount = 2.0f + triCount * 14.0f;
	const float triSlope = stairAuthorRead(stairFrame, stair_slot::kSlope, home, travelX, travelY,
	                                       kPhiStairSlope);
	authors.riserSlope = triSlope * triSlope * 0.9f;
	return authors;
}

namespace {

/// prepareFlagshipRecipe's first half: everything it derives BEFORE the normalisation measures the cycle.
void prepareFlagshipShape(FlagshipRecipe& r, const FlagshipSource& source) {
	// THE CONTROL POINTS THE SMOOTH SOURCE READS, extracted before anything evaluates — `held` below
	// samples the chain, and the chain reads the source, so a recipe prepared without these would
	// quantize the smoothed cycle onto a staircase built from the unsmoothed one. Exactly the trap
	// the vowel road documents two paragraphs up, for exactly the same reason.
	r.smoothPoints = 0;
	r.smoothFromShapes = false;
	if (r.smoothness > 0.0f && source.shapes != nullptr) {
		// BASIC SHAPES (backlog KV): the mix at K even points for every K — see FlagshipRecipe::smoothShapes.
		for (int k = kFlagshipMinSmoothPoints; k <= kFlagshipMaxSmoothPoints; ++k) {
			for (int j = 0; j < k; ++j) {
				r.smoothShapes[static_cast<size_t>(k)][static_cast<size_t>(j)] = evalShapes(
				    *source.shapes, source.shapesMip, static_cast<double>(j) / static_cast<double>(k));
			}
		}
		r.smoothFromShapes = true;
		r.smoothPoints = kFlagshipMaxSmoothPoints;
	}
	else if (r.smoothness > 0.0f) {
		// The endpoint, then each movable waypoint in order, stopping where the builder's own
		// ordering stops — a table can author fewer points than the ceiling, and a non-increasing
		// phase is how it says so. The PHASES are then thrown away: renormalising to even spacing is
		// the instruction and it is what makes the curve uniform.
		r.smoothAmp[0] = r.morph.endpointAmp;
		r.smoothPoints = 1;
		for (int w = 0; w < owlet::dsp::kPhiMorphMovableWaypoints; ++w) {
			if (w > 0 && !(r.morph.phases[w] > r.morph.phases[w - 1])) {
				break;
			}
			r.smoothAmp[static_cast<size_t>(r.smoothPoints++)] = r.morph.amplitudes[w];
		}
		// Two points is the floor the curve needs; a table that somehow authored fewer cannot be
		// smoothed, and turning the feature off there is better than reading past the end.
		if (r.smoothPoints < kFlagshipMinSmoothPoints) {
			r.smoothPoints = 0;
			r.smoothness = 0.0f;
		}
	}

	// SMOOTHNESS COUNTS AS OPERATED. The flag means "not the plain morph path", and a replaced source
	// makes that true whether or not an operator depth did — so the bake, the grid road and
	// readsGrid all follow with nothing of their own to change. Same move vox needed, one level down.
	//
	// AND SO DOES WARP, for the same reason and with one extra condition. `warpAmount` is what
	// decides whether a warp is doing anything: the bipolar modes rest at knob 0.5, so `warpDepth > 0`
	// would have called a resting Bend +/- operated and pushed an untouched cycle onto the baked path.
	// Asking the shape's own neutral, rather than the knob's number, is what keeps the identity corner
	// exact under every mode.
	const bool warped = r.warpMode != WarpMode::Off && warpAmount(r.warpMode, r.warpDepth) != 0.0f;
	r.operated = r.quantize > 0.0f || r.syncRatio > 1.0f || r.feedback > 0.0f || r.smoothness > 0.0f
	             || warped;
	r.steps = 0;
	if (r.operated && r.quantize > 0.0f) {
		// The integer ceiling of the blended count — rounding happens HERE, after the blend (rule
		// V10a), never per sound. The clamp guards hand-built recipes; the author formula cannot leave
		// [2, 16].
		r.steps = std::clamp(static_cast<int>(std::ceil(static_cast<double>(r.stepCount))), 2,
		                     kFlagshipMaxSteps);
		// Held values: the sync+feedback chain sampled at each step's centre — the centre of the step's
		// ACTUAL span, so the truncated last step of a fractional count holds a value it really covers.
		const double sc = static_cast<double>(r.stepCount);
		for (int i = 0; i < r.steps; ++i) {
			const double begin = static_cast<double>(i) / sc;
			const double end = std::min(static_cast<double>(i + 1) / sc, 1.0);
			r.held[static_cast<size_t>(i)] =
			    static_cast<float>(flagshipChain(r, source, 0.5 * (begin + end)));
		}
	}

	// AND THE NORMALISATION, LAST, because it is a measurement OF everything above. See the two
	// fields' declaration for what it is fixing and the figures that found it.
	//
	// The coefficients are cleared first so the evaluation that measures it is the raw cycle: a recipe
	// is re-prepared carrying a previous position's numbers, and measuring a cycle through them would
	// compound the correction instead of replacing it.
	r.dcOffset = 0.0f;
	r.gain = 1.0f;
}

/// How many points of the cycle the normalisation probes, at phases i / count.
int flagshipProbeCount(const FlagshipRecipe& r, int normaliseSamples) {
	const bool warped = r.warpMode != WarpMode::Off && warpAmount(r.warpMode, r.warpDepth) != 0.0f;
	// A WARPED RECIPE PROBES AT THE BAKE'S OWN GRID, which makes the peak exact rather than merely
	// closer — see kFlagshipWarpedNormaliseSamples for the measurements and for why a smaller bump
	// would not have done. Unwarped recipes keep 128 so that no sound nobody asked to change gets
	// re-levelled by a warp feature landing.
	//
	// THE PICTURE'S COARSE PROBE IS NOT PROMOTED, AND THE FIRST VERSION OF THIS SAID SO WHILE DOING
	// THE OPPOSITE. `const int wanted = warped ? kFlagshipWarpedNormaliseSamples : normaliseSamples`
	// promoted EVERY caller, so the terrain's 32 became 256 and a warped map paid eight times over —
	// the exact cost the comment above it was citing as the reason not to. It was felt immediately:
	// with warp on, the terrain rendered much more slowly.
	//
	// MEASURED, per texel at the picture's probe count: 2.84 us unwarped against 25.82 us warped,
	// **9.1x**, which is a 128x128 map going from 47 ms to 423 ms.
	//
	// A COMMENT THAT DESCRIBES THE INTENT OF THE LINE BELOW IT IS NOT A GUARD, and this file has now
	// paid for that twice. The promotion is conditional on WHO IS ASKING: a caller that requested the
	// full audio probe is asking for an accurate peak and gets an exact one; a caller that explicitly
	// asked for fewer points has chosen a cheaper answer and keeps it.
	//
	// SO THE PICTURE'S KNOWN GAP STANDS AS RECORDED: four of the five features a colour is made from
	// are ratios and cannot see the gain, and under warp the luminance can read high — measured up to
	// 2.9x. That is a picture defect, not an audio one, and it is left rather than paid for at 9x.
	const bool wantsAudioProbe = normaliseSamples >= kFlagshipNormaliseSamples;
	const int wanted = (warped && wantsAudioProbe) ? kFlagshipWarpedNormaliseSamples : normaliseSamples;
	return std::clamp(wanted, 2, kFlagshipWarpedNormaliseSamples);
}

/// The normalisation's two coefficients, measured off the raw cycle's `probeCount` probes.
void normaliseFromProbe(FlagshipRecipe& r, const float* probe, int probeCount) {
	double sum = 0.0;
	for (int i = 0; i < probeCount; ++i) {
		sum += static_cast<double>(probe[static_cast<size_t>(i)]);
	}
	const double dc = sum / static_cast<double>(probeCount);
	double peak = 0.0;
	for (int i = 0; i < probeCount; ++i) {
		peak = std::max(peak, std::abs(static_cast<double>(probe[static_cast<size_t>(i)]) - dc));
	}
	r.dcOffset = static_cast<float>(dc);
	// A SILENT CYCLE IS LEFT SILENT. Dividing by a peak of nothing would turn the quietest ground in
	// the engine into full scale, which is the one failure mode a normaliser must not have — and DC
	// alone is not a sound, so removing it from a cycle with no AC in it correctly leaves silence.
	r.gain = peak > kFlagshipNormaliseFloor ? static_cast<float>(1.0 / peak) : 1.0f;
}

} // namespace

void prepareFlagshipRecipe(FlagshipRecipe& r, const FlagshipSource& source,
                           int normaliseSamples) {
	prepareFlagshipShape(r, source);
	const int probeCount = flagshipProbeCount(r, normaliseSamples);
	std::array<float, kFlagshipWarpedNormaliseSamples> probe {};
	for (int i = 0; i < probeCount; ++i) {
		probe[static_cast<size_t>(i)] =
		    evalFlagship(r, static_cast<double>(i) / static_cast<double>(probeCount), source);
	}
	normaliseFromProbe(r, probe.data(), probeCount);
}

namespace {

/// evalFlagship short of its last act: the RAW cycle at `phase01`, before the normalisation's offset and gain.
/// `smoothHere`, when given, is `smoothSourceAt(r, wrapped)` read beforehand (FlagshipSlotSmooth) — the same number.
double evalFlagshipRaw(const FlagshipRecipe& r, double phase01, const FlagshipSource& source,
                       const double* smoothHere = nullptr) {
	// The identity corner IS the morph path — same function, same bits. Everything below exists
	// only where some field said so. WITH A VOWEL IN IT the corner is still not the operator road,
	// but it is no longer the plain morph table either: the source read is what the voice plays,
	// which is why this goes through the seam rather than round it.
	if (!r.operated) {
		return flagshipSourceAt(r, source, phase01);
	}
	const double wrapped = phase01 - std::floor(phase01);
	double y = flagshipChain(r, source, wrapped);

	if (r.quantize > 0.0f && r.steps > 0) {
		const double sc = static_cast<double>(r.stepCount);
		// A wrapped phase of 1 − 1e-17 rounds to sc exactly, and a fractional count's last step is
		// truncated — both land on the last held step, same clamp evalStairGrid stands on.
		int i = static_cast<int>(wrapped * sc);
		if (i >= r.steps) {
			i = r.steps - 1;
		}
		// Position within the step, against the NOMINAL width 1/sc, so the riser fraction means the
		// same thing on every step including the truncated one.
		const double u = wrapped * sc - static_cast<double>(i);
		float stepValue = r.held[static_cast<size_t>(i)];
		const float slope = r.riserSlope;
		if (slope > 0.0f && u < static_cast<double>(slope)) {
			// The riser climbs from the PREVIOUS step's held value — step 0's from the last step's,
			// which is what joins the end of the cycle to its start, exactly as the stair renderer
			// joins its wrap.
			const float prev = r.held[static_cast<size_t>((i + r.steps - 1) % r.steps)];
			stepValue = prev + (stepValue - prev) * static_cast<float>(u / static_cast<double>(slope));
		}
		// The blend, not a switch: q carries the cycle toward its own staircase, and at q exactly 1
		// what remains is the staircase alone (to a rounding of the last addition).
		y += static_cast<double>(r.quantize) * (static_cast<double>(stepValue) - y);
	}

	// SMOOTHNESS LAST, AFTER QUANTIZE — is smoothness post-quantize? it should be.
	//
	// It used to be a SOURCE replacement, mixed in before a single operator had looked at the cycle,
	// and the operator tier then put the corners straight back: a staircase built from a smooth curve
	// is a staircase. MEASURED across 220 positions, the knob's reach from neutral to maximum — as a
	// fraction of the spectral centroid it starts from — was 53.6% where quantize is below 0.15 and
	// 31.4% where it is above 0.60, and at one heavily quantized position (home 4.0, quantize 0.95)
	// only 6.7%. The control was being eaten by roughly two fifths wherever the terrain was operated,
	// and almost entirely where it was operated hard. That is also, at last, the mechanism behind the
	// siney doc's "on V5 the operator tier eats it".
	//
	// APPLIED LAST, IT OVERRIDES. At smoothness 1.0 the output IS the cubic, so the operators are
	// bypassed rather than smoothed — which is what "make this smooth" has to mean if the knob is to
	// mean the same thing everywhere. A crossfade that smoothed the operators' own output would need
	// a cubic through the CHAIN's points, and there is no such curve: smoothSourceAt interpolates the
	// morph table's control points, which is the only place control points exist.
	//
	// THE POINT COUNT IS IN THE CONDITION, not left to an invariant held two functions away. prepare
	// zeroes `smoothness` when a table authored fewer than two points, so `smoothness > 0` alone is
	// true whenever this is safe TODAY — but smoothSourceAt clamps into [2, smoothPoints], and with
	// no points that is a clamp whose low bound exceeds its high one, which is undefined behaviour
	// reached through a division by zero. A known-bad run found it by crashing rather than failing.
	if (r.smoothness > 0.0f && r.smoothPoints >= kFlagshipMinSmoothPoints) {
		y += static_cast<double>(r.smoothness) * ((smoothHere != nullptr ? *smoothHere : smoothSourceAt(r, wrapped)) - y);
	}
	return y;
}

} // namespace

float evalFlagship(const FlagshipRecipe& r, double phase01, const FlagshipSource& source) {
	return flagshipNormalised(r, evalFlagshipRaw(r, phase01, source));
}

FlagshipRecipe buildFlagshipSliced(uint16_t zone, float phaseOffset, double travelX, double travelY,
                                   const MorphBasis& morphFrame, const StairBasis& stairFrame,
                                   double gammaBase, FieldScales scales, int normaliseSamples,
                                   WarpMode warpMode, const PhaseWarp* warp, bool operatorFieldsOff) {
	const AngleWarpScope likes(warp); // the phase warp, on every site this recipe reads
	FlagshipRecipe r;
	r.morph = buildMorphSliced(zone, phaseOffset, travelX, travelY, morphFrame, gammaBase, scales.grit);

	const double home = sliceHome(zone, phaseOffset, gammaBase);
	FlagshipAuthors authors = flagshipAuthorsAt(morphFrame, stairFrame, home, travelX, travelY);
	// BASIC SHAPES (backlog KV): the terrain's four operator AMOUNTS do not act, only the knobs — see flagship.h.
	if (operatorFieldsOff) {
		authors.fields.quantize = authors.fields.sync = authors.fields.feedback = authors.fields.smoothness = 0.0f;
	}
	// THE FIVE FIELD MODIFIER KNOBS (2026-08-25). THIS ENTRY AND buildFlagshipFor MUST BOTH APPLY
	// THEM — an effect applied at one of the two renders ATTENUATED rather than absent, and a
	// difference check passes on it. CLAUDE.md names this exact pair as the shape that got past two
	// cases written to catch it.
	r.quantize = fieldWithOverride(authors.fields.quantize, scales.quantize);
	r.syncRatio =
	    1.0f + (kFlagshipSyncMaxRatio - 1.0f) * fieldWithOverride(authors.fields.sync, scales.sync);
	// FEEDBACK'S FIELD IS ALREADY POST-THRESHOLD — kFlagshipFeedbackThreshold has been applied by
	// flagshipAuthorsAt, so what the knob scales is the DEPTH where feedback happens, not where it
	// happens at all. At knob 1.0 that lifts the §5.3 grey-goo guard for this sound, deliberately:
	// the depth is still bounded by kFlagshipFeedbackMax, and a player asking for it is the same
	// argument that moved the smoothness knob off multiply.
	r.feedback = fieldWithOverride(authors.fields.feedback, scales.feedback);
	r.stepCount = authors.stepCount;
	r.riserSlope = authors.riserSlope;
	// THE SMOOTHNESS SCALE (doc §11) — the per-sound override knob biases the field at read time, so
	// the scaled value feeds both the audio decode and the terrain feature measurement. Neutral = the
	// field as-is, so a caller that does not pass scales (the existing tests, single-sound paths) is
	// unchanged. 0.0 defeats smoothing (plain morph).
	r.smoothness = fieldWithOverride(authors.fields.smoothness, scales.smoothness);
	// POINTS MODIFIES THE FIELD; SMOOTHNESS STILL WALKS THE RESULT. The order is the decision recorded
	// in the plan doc §3 — the reverse also compiles and sounds different at the extremes. At both
	// knobs neutral this is pointCountWithOverride(field, 0.5) = field, i.e. today exactly.
	r.pointCount = pointCountWithOverride(
	    pointFieldWithOverride(authors.fields.pointCount, scales.points), scales.smoothness);
	// THE FM TRIPLE, carried as terrain. `fmDepth` stays at its zero default here: this entry serves
	// the terrain and the single-sound tests, and FM is deliberately invisible to the map (FM doc §8's
	// picture problem — a subharmonic composite's period is q cycles and the single-cycle lens cannot
	// resolve it). A caller that wants FM passes it.
	r.fmRatio = authors.fields.fmRatio;
	r.fmDenominator = authors.fields.fmDenominator;
	r.fmStereoWidth = authors.fields.fmStereoWidth;
	r.fmDepth = scales.fmDepth;
	// WARP: the mode is engine-wide and arrives on the recipe's caller, the depth is this sound's
	// knob. No field, no bias — the knob IS the value (see FieldScales::warpDepth).
	r.warpDepth = scales.warpDepth;
	r.warpMode = warpMode;
	// THE PROBE COUNT TRAVELS WITH THE CALL, because a caller that is going to re-prepare this recipe
	// with a source — the terrain does, one line after it builds — would otherwise pay for an
	// accurate normalisation it is about to throw away. Measured at 239.6 ms per map before this was
	// threaded through, against 72.7 with no normalisation at all.
	prepareFlagshipRecipe(r, {}, normaliseSamples);
	return r;
}

FlagshipRecipe buildFlagshipFor(const Character& c) {
	FlagshipRecipe r;
	// The morph half is the bank's own build, called rather than restated — blend rule, excite,
	// blue shift and all. The operators ride on top of whatever it produced.
	r.morph = buildMorphFor(c);

	// AUTHOR VALUES BLEND BY WEIGHT, per sound at each sound's own coordinate — Σ w·field(home),
	// never field(Σ w·home), for the reason blueShiftInto records: an averaged coordinate can land
	// in territory neither neighbour occupies, and two calm sounds either side of a feedback peak
	// would conjure a phantom scream between them. Interpolated values are bounded by what the
	// neighbours actually are, and standing on an anchor gives that sound's operators exactly.
	//
	// The pin/home/frame derivation restates oneMorph's (file-local in character_bank.cpp); the
	// position-purity case pins this route against buildFlagshipSliced to the bit, which is what
	// catches drift between the two spellings.
	const size_t n = std::min<size_t>(c.count, kMaxSounds);
	float quantize = 0.0f;
	float sync = 0.0f;
	float feedback = 0.0f;
	float stepCount = 0.0f;
	float riserSlope = 0.0f;
	float smoothness = 0.0f;
	float pointCount = 0.0f;
	float fmRatio = 0.0f;
	float fmDenominator = 0.0f;
	float fmStereoWidth = 0.0f;
	float fmDepth = 0.0f;
	float warpDepth = 0.0f;
	for (size_t i = 0; i < n; ++i) {
		const SoundCoord& s = c.sounds[i];
		const SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
		const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
		const MorphBasis morphFrame =
		    sliceFrameFor<kMorphTriangles>(rotateFor(pin), home, s.residue.morph);
		const StairBasis stairFrame =
		    sliceFrameFor<kStairTriangles>(rotateFor(pin), home, s.residue.stair);
		const AngleWarpScope likes(c.likes.of(s.slot)); // this sound's phase warp on its operator-author sites
		FlagshipAuthors authors = flagshipAuthorsAt(morphFrame, stairFrame, home, s.travelX, s.travelY);
		// BASIC SHAPES (backlog KV): the terrain's four operator AMOUNTS do not act, only the knobs — see flagship.h.
		if (c.engine == OscEngine::Shapes) {
			authors.fields.quantize = authors.fields.sync = authors.fields.feedback = authors.fields.smoothness = 0.0f;
		}
		const auto w = static_cast<float>(s.weight);
		// THE FIVE FIELD MODIFIER KNOBS (2026-08-25), each scaling ITS OWN sound's field before the
		// blend — Σ w·override(field_k), never override(Σ w·field). Same house rule the smoothness
		// comment below states, and the same reason: one sound can be pushed into quantize country
		// while its neighbour keeps the terrain's own answer, and the crossfade between them is
		// continuous because both ends are real characters.
		//
		// THE SLICED ENTRY ABOVE APPLIES THE IDENTICAL FIVE. If it ever stops, this blend keeps
		// working and the terrain quietly draws a different sound from the one being played — the
		// partial-effect shape CLAUDE.md names, which passes a difference check.
		quantize += w * fieldWithOverride(authors.fields.quantize, s.quantizeScale);
		sync += w * fieldWithOverride(authors.fields.sync, s.syncScale);
		feedback += w * fieldWithOverride(authors.fields.feedback, s.feedbackScale);
		stepCount += w * authors.stepCount;
		riserSlope += w * authors.riserSlope;
		// Σ w·field, per sound at its OWN coordinate — the house rule, and for smoothness the reason
		// is the sharpest it gets anywhere: field(Σ w·home) could put a blend of two segment-rule
		// sounds inside smooth country and hand back a waveform neither neighbour makes.
		// THE SMOOTHNESS SCALE (doc §11) multiplies the field per sound: a sound's override knob scales
		// its own smoothness contribution before the blend, so one sound can be smoothed while a
		// neighbour keeps the segment rule. Default 1.0 = the field as-is, so the blend is unchanged.
		smoothness += w * fieldWithOverride(authors.fields.smoothness, s.smoothnessScale);
		// POINTS MODIFIES THE FIELD, SMOOTHNESS WALKS THE RESULT — the composition order of plan doc
		// §3, spelled here exactly as it is spelled in the sliced entry. Both knobs neutral is
		// pointCountWithOverride(field, 0.5) = field, today to the bit.
		pointCount += w
		              * pointCountWithOverride(
		                  pointFieldWithOverride(authors.fields.pointCount, s.pointScale),
		                  s.smoothnessScale);
		// THE FM TRIPLE BLENDS THE SAME WAY, Σ w·field per sound at its own coordinate, for the reason
		// spelled out for smoothness above. The DEPTH blends too, because it is per sound: a
		// constellation where one sound has FM and its neighbour does not crossfades between them
		// rather than switching, which is what makes the puck's travel through FM country continuous.
		fmRatio += w * authors.fields.fmRatio;
		fmDenominator += w * authors.fields.fmDenominator;
		fmStereoWidth += w * authors.fields.fmStereoWidth;
		fmDepth += w * s.fmDepth;
		// WARP DEPTH BLENDS LIKE FM DEPTH, and for the same reason its comment gives: a constellation
		// where one sound is warped and its neighbour is not crossfades between them rather than
		// switching, which is what makes the puck's travel through warped country continuous. The
		// MODE cannot blend and does not live here — see FlagshipRecipe::warpMode.
		warpDepth += w * s.warpDepth;
	}
	r.quantize = quantize;
	// Depths blend, then the ratio is derived — one spelling shared with the sliced entry, so a
	// single sound at weight 1.0 comes out identical to the bit.
	r.syncRatio = 1.0f + (kFlagshipSyncMaxRatio - 1.0f) * sync;
	r.feedback = feedback;
	// An empty constellation blends to a zero step count; the prepare clamp would save it, but the
	// honest value is the author floor, matching what any real sound carries.
	r.stepCount = n == 0 ? 2.0f : stepCount;
	r.riserSlope = riserSlope;
	// An empty constellation blends the interval fields to zero, and zero is not a legal interval —
	// a denominator of zero is a division. The author floors are what any real sound carries, and are
	// what the sliced entry would have produced, so they are the honest empty answer here too.
	r.fmRatio = n == 0 ? kFlagshipFmRatioMin : fmRatio;
	r.fmDenominator = n == 0 ? kFlagshipFmDenomMin : fmDenominator;
	r.fmStereoWidth = fmStereoWidth;
	r.fmDepth = fmDepth;
	// THE MODE IS THE CHARACTER'S, NOT A SOUND'S — engine-wide, because an enum cannot blend. The
	// depth is the weighted blend above.
	r.warpMode = c.warpMode;
	r.warpDepth = warpDepth;
	r.smoothness = smoothness;
	// As with the step count: an empty constellation blends to zero, and the honest value there is
	// the ceiling every real sound's field is measured against, not a count of nothing.
	r.pointCount = n == 0 ? static_cast<float>(kFlagshipMaxSmoothPoints) : pointCount;
	prepareFlagshipRecipe(r);
	return r;
}

void bakeFlagshipTable(const FlagshipRecipe& r, FlagshipTable& out) {
	out.operated = r.operated;
	if (!r.operated) {
		// The grids are left as they are: the flag is what gates every read, and an identity build
		// must not pay 256 evaluations for slots nobody will look at.
		return;
	}
	for (int j = 0; j < kFlagshipSlots; ++j) {
		out.slots[j] = evalFlagship(r, static_cast<double>(j) / static_cast<double>(kFlagshipSlots));
	}
	out.slots[kFlagshipSlots] = out.slots[0];
	flagshipBinomialPass(out.slots, out.mip1);
	flagshipBinomialPass(out.mip1, out.mip2);
}

void bakeFlagshipGridAtMip(const FlagshipRecipe& r, const FlagshipSource& source, int mipLevel,
                           float* out) {
	// The base grid, exactly bakeFlagshipTable's loop — same expression, same order, so the level-0
	// answers cannot drift apart.
	for (int j = 0; j < kFlagshipSlots; ++j) {
		out[j] = evalFlagship(r, static_cast<double>(j) / static_cast<double>(kFlagshipSlots), source);
	}
	out[kFlagshipSlots] = out[0];

	const int passes = std::clamp(mipLevel, 0, 2);
	if (passes == 0) {
		return;
	}
	// One grid of scratch, on the stack: the passes ping-pong, and an odd number of them has to be
	// copied back so the answer always lands in `out`. Two hundred and fifty-seven floats is a
	// kilobyte of stack in a function the audio thread calls once per block, which is nothing, and
	// it is not an allocation — rule V10b is about the heap.
	float scratch[kFlagshipSlots + 1];
	flagshipBinomialPass(out, scratch);
	if (passes == 1) {
		std::copy(std::begin(scratch), std::end(scratch), out);
		return;
	}
	flagshipBinomialPass(scratch, out);
}

void bakeFlagshipLadder(const FlagshipRecipe& r, const FlagshipSource& source, float* base,
                        float* mip1, float* mip2) {
	// Same loop and same order as bakeFlagshipTable's, so the two cannot drift apart.
	for (int j = 0; j < kFlagshipSlots; ++j) {
		base[j] = evalFlagship(r, static_cast<double>(j) / static_cast<double>(kFlagshipSlots), source);
	}
	base[kFlagshipSlots] = base[0];
	flagshipBinomialPass(base, mip1);
	flagshipBinomialPass(mip1, mip2);
}

void prepareAndBakeFlagshipLadder(FlagshipRecipe& r, const FlagshipSource& source, float* base, float* mip1,
                                  float* mip2, const FlagshipSlotSmooth* shared) {
	prepareFlagshipShape(r, source);
	// THE SHARED SMOOTH SOURCE, only where the chain reads it: `prepareFlagshipShape` re-derives the control points from
	// the morph table alone, so a recipe copied off the table `shared` rides on reads the same curve the bank read.
	const bool smoothShared = shared != nullptr && shared->live;
	// THE RAW CYCLE ON THE BAKE'S GRID, ONCE. The probe's phases i / count are slot phases (count divides the slot
	// count: the static_asserts in the header), and an exact dyadic i / count IS (i · stride) / 256 as a double, so
	// each probe is the very evaluation the slot makes. Before the normalisation is measured the recipe's offset and
	// gain are 0 and 1, so `flagshipNormalised` of a raw value is the probe prepareFlagshipRecipe takes, to the bit.
	const int probeCount = flagshipProbeCount(r, kFlagshipNormaliseSamples);
	const int stride = kFlagshipSlots / probeCount;
	std::array<double, kFlagshipSlots> raw {};
	for (int j = 0; j < kFlagshipSlots; ++j) {
		raw[static_cast<size_t>(j)] = evalFlagshipRaw(r, static_cast<double>(j) / static_cast<double>(kFlagshipSlots),
		                                              source, smoothShared ? &shared->at[static_cast<size_t>(j)] : nullptr);
	}
	std::array<float, kFlagshipWarpedNormaliseSamples> probe {};
	for (int i = 0; i < probeCount; ++i) {
		probe[static_cast<size_t>(i)] = flagshipNormalised(r, raw[static_cast<size_t>(i * stride)]);
	}
	normaliseFromProbe(r, probe.data(), probeCount);
	// And the ladder, bakeFlagshipLadder's own arithmetic on the same raw values: evalFlagship is
	// flagshipNormalised(raw).
	for (int j = 0; j < kFlagshipSlots; ++j) {
		base[j] = flagshipNormalised(r, raw[static_cast<size_t>(j)]);
	}
	base[kFlagshipSlots] = base[0];
	flagshipBinomialPass(base, mip1);
	flagshipBinomialPass(mip1, mip2);
}

void fillFlagshipSlotSmooth(const FlagshipRecipe& r, FlagshipSlotSmooth& out) {
	// evalFlagshipRaw's own condition for reading it, on the operated road it is read on.
	out.live = r.operated && r.smoothness > 0.0f && r.smoothPoints >= kFlagshipMinSmoothPoints;
	if (!out.live) {
		return;
	}
	// The argument evalFlagshipRaw passes: the slot phase, wrapped — and j / 256 is already in [0, 1), so it is the
	// slot phase itself.
	for (int j = 0; j < kFlagshipSlots; ++j) {
		out.at[static_cast<size_t>(j)] = smoothSourceAt(r, static_cast<double>(j) / static_cast<double>(kFlagshipSlots));
	}
}

double flagshipSmoothCurveForTest(const float* amp, int n, double p) {
	return smoothCurveAt(amp, n, p);
}

const float* flagshipMipSlots(const FlagshipTable& table, int mipLevel) {
	// A real clamp, unlike stairMipSlots' default-to-bright: past the dull end of this ladder the
	// honest neighbour is the dullest grid, not the brightest. stairMipForFrequency never asks for
	// anything outside 0..2, so the difference is a guard rather than a behaviour.
	if (mipLevel >= 2) {
		return table.mip2;
	}
	if (mipLevel == 1) {
		return table.mip1;
	}
	return table.slots;
}

float evalFlagshipGrid(const float* slots, double phase01) {
	const double wrapped = phase01 - std::floor(phase01);
	const double u = wrapped * static_cast<double>(kFlagshipSlots);
	// Same clamp evalStairGrid explains: a wrapped phase of 1 − 1e-17 rounds up to exactly 256.0,
	// and the guard slot covers idx + 1 from there.
	int idx = static_cast<int>(u);
	if (idx >= kFlagshipSlots) {
		idx = kFlagshipSlots - 1;
	}
	const float frac = static_cast<float>(u - static_cast<double>(idx));
	return slots[idx] + (slots[idx + 1] - slots[idx]) * frac;
}

} // namespace feathers

namespace feathers {

void prepareShapesRecipe(FlagshipRecipe& r, const ShapesWeights& weights, int mip, int normaliseSamples) {
	if (!shapesOperated(r)) {
		return;
	}
	FlagshipSource source;
	source.shapes = &weights;
	source.shapesMip = mip;
	prepareFlagshipRecipe(r, source, normaliseSamples);
}

float evalShapesCycle(const FlagshipRecipe& r, const ShapesWeights& weights, int mip, double phase01) {
	if (!shapesOperated(r)) {
		return evalShapesWarped(weights, mip, phase01, r.warpMode, r.warpDepth);
	}
	FlagshipSource source;
	source.shapes = &weights;
	source.shapesMip = mip;
	return evalFlagship(r, phase01, source);
}

} // namespace feathers
