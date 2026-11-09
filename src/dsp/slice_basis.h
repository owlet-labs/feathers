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

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

// The slice, and the basis that aims it.
//
// A sound sits at a HOME — its zone, offset and gamma, which is the one coordinate the engine has
// always had — and TRAVELS from there across a 2-D slice through the character layer's full bank-phase
// space. The basis decides which plane that slice is. Travel is moving within it; rotate is re-aiming
// it; local density is the rate half of it. One mechanism, three gestures — the model, and the
// reason it is worth the machinery below.
//
// HOME AND TRAVEL ARE SEPARATE COORDINATES, and that separation is the whole design rather than an
// implementation detail. if rotation changes the sound its redundant with gamma. it needs to
// keep the sound the same unless you move in xy/gamma. That is right, and it is the sharpest test the
// mechanism has to pass. Gamma already means "give me a different sound along the old line"; a rotate
// that also changed the sound would be a second, worse spelling of it. So the deviation this basis
// decodes is multiplied by the TRAVEL, and travel is zero until someone moves — which makes rotation
// exactly inaudible until it has something to aim. Not approximately: the travel terms are multiplied
// by zero and added, so home is returned bit for bit.
//
// Travel is `Pos X` and `Pos Y`, which have been in the parameter table since v1 — declared, bipolar,
// centre-detented, dormant. They were reserved for exactly this and cost no new id, which is the
// constraint set.
//
// THE FULL SPACE IS 2N-DIMENSIONAL for an engine with N triangles: N phase coordinates and N rate
// coordinates. That is what lets a single rotation produce both halves. Rotating purely within the
// phase coordinates would only ever re-aim WHERE you are going; rotating through the rate
// coordinates is what makes one region of the plane dense and another calm, because a triangle's
// rate is how fast its contribution varies as you move. Keep them in one space and rotation mixes
// them for free; split them and you need a second mechanism and a second set of decisions.
//
// THE GENERATOR IS THE STATE OF RECORD, and the basis is a pure function of it — never the other way
// round. A basis rotated in place accumulates float error, so retracing a breadcrumb lands somewhere
// subtly different from where it was recorded, and the further back you go the worse it is.
// Rewindability is a correctness property here, not a nicety: undo and breadcrumbs are how the
// design says you explore without fear of losing what you had.

namespace feathers {

/// How many triangles each engine's character layer evaluates, which is the N of its 2N-dimensional
/// space. Here rather than beside the builders so that a test of the basis alone — no firmware
/// headers, no character library — can still speak about the real engines; slice_build.cpp holds the
/// static_asserts that tie these to the firmware's own bank sizes, so a bank that grows breaks the
/// build rather than silently leaving a triangle un-sliced.
///
/// Morph: 8 phase deltas + 7 amplitudes + 8 curvatures + 12 single-triangle banks.
/// Stair: count, slope, tilt, asym, land-cycles, height landscape, asymmetry landscape, and the two
/// spatial patterns families 5 and 6 evaluate.
/// Vox: the two formant wanders, two pulse counts, two decays, balance, two polarities, the
/// coherent shift, breath, articulation — the twelve φ banks phi_vox.hpp declares, every one a slot
/// from birth (V22c: articulation's slot exists now so the residue rows and the wire count never
/// grow twice; its product waits for the motion arc).
/// Weave (the Resonate engine): the twenty-seven φ banks phi_weave.hpp declares, in its declaration
/// order — the three physics bases, the two spatial cycle counts, the five landscapes, the five
/// home-shape partials and phases, the five bow banks, travel, the three pluck banks, shimmer, morph
/// bow and output gain.
/// Partials (the additive engine, design 2026-10-08 §2): thirteen shape fields (rolloff, two formants
/// of three, odd/even, comb spacing and depth, detail, off-grid, smoothness) and four detail GROUPS —
/// one row each for harmonics 1-2, 3-8, 9-32 and 33 up, every harmonic in a group reading the group's
/// row at its own φ rate. Rows per group rather than per harmonic, because a row is also residue
/// that rides the stance wire: 320 rows would have been 640 stored numbers a sound. See partial_slot.
inline constexpr size_t kMorphTriangles = 35;
inline constexpr size_t kStairTriangles = 9;
inline constexpr size_t kVoxTriangles = 12;
inline constexpr size_t kWeaveTriangles = 27;
inline constexpr size_t kPartialTriangles = 17;

/// A sound's orientation, as the thing that is stored: THE ONE SCALAR M READS.
///
/// This was seven numbers — an aperture and six angles — for exactly as long as it took the stage-2
/// shrink to land. M(rotate, home) reads one turn; the six angles were always ONE degree of freedom
/// through the UI (the corner spread a scalar at fixed ratios with ratio[0] = 1), so `rotate` here IS
/// `angles[0]` of every patch the old layout wrote, exactly, and the loaders read the legacy blocks
/// that way — see the raw-byte migration cases in the bridge suite.
///
/// Zero is not the degenerate orientation — it is the plane M gives at rotate 0, which is an open
/// one, because by design a sound is BORN OPEN. The degenerate plane did not stop being
/// load-bearing; it stopped being reachable by accident, and `degenerateSliceBasis` is where the
/// firmware-parity cases construct it. What every orientation still guarantees, and what remains the
/// correctness proof of the decode, is that a sound which has not travelled returns
/// `zone/1023 + offset` bit for bit whatever the plane — see phaseFor.
///
/// THE APERTURE IS GONE. It existed because a rotation applies the same linear map to both rows, so
/// the degenerate basis — whose rows are identical — could never be prised open by turning; something
/// had to open it first. M has no such problem: it writes both rows from independent readings of a φ
/// field, so being open is what it is rather than a stage it has to be put through.
struct SliceGenerator {
	float rotate = 0.0f;

	friend bool operator==(const SliceGenerator&, const SliceGenerator&) = default;
};

/// A turn of the plane, as a delta rather than as a destination.
///
/// The corner encoder Explore will grow sends increments, and increments are what a generator can
/// accumulate without ever being read back out of a basis — which is the §1.2 rule that makes
/// breadcrumbs retraceable. Every field is zero for "no turn", so a rotation nobody asked for is
/// exactly the identity.
struct SliceRotation {
	float rotate = 0.0f;
};

/// The generator turned by `delta`, cumulatively.
///
/// A PURE FUNCTION OF THE STORED GENERATOR, never of the basis. Turning a BASIS in place is the thing
/// §1.2 forbids: each turn would carry the last one's float error, so retracing a breadcrumb would
/// land near where it was recorded rather than on it, and the further back you went the worse it
/// would get. Accumulating in the generator instead means a rewind is a stored number restored, which
/// is exact by construction rather than by arithmetic.
///
/// WHAT A ROTATION CANNOT REACH, which is the guarantee that makes the verb honest: a sound at its
/// home. The decode multiplies every basis component by the TRAVEL, so a sound that has not travelled
/// returns its home bit for bit however far the plane is turned — this is the same structure as the
/// home/travel separation the design rests on, and it is why "the held sound sits on the rotation axis" is a
/// property of the arithmetic rather than a thing the caller has to arrange.
///
/// A DRIFTED SOUND IS ALSO PRESERVED NOW, and this function is not what does it: turning the generator
/// is only half of a rotation since A10 was reversed. The other half is banking what the turn would
/// have moved — see foldResidue, and the verbs in plugin/sound_residue.h that do both together. A
/// caller that turns a plane without folding gets the old A10 behaviour, which is why the surface calls
/// the pair rather than this.
inline SliceGenerator rotateBy(const SliceGenerator& from, const SliceRotation& delta) {
	return { from.rotate + delta.rotate };
}

/// WHAT PAST ROTATIONS BANKED, so that turning the plane does not move the sound you are standing on.
///
/// A10 was reversed on 2026-08-13: rotate must not change the sound when sitting on a single point.
/// A10 said a drifted sound legitimately moves when its plane is re-aimed, because it is
/// out in the plane rather than at its home — true of the arithmetic, and wrong as an instrument. On
/// the 2-D chart every drag that is not exactly vertical commits drift, so under A10 rotate preserved
/// the standing sound almost never, which is the opposite of what a re-aiming verb is for.
///
/// So a rotation BANKS THE DIFFERENCE it would otherwise have made. One number per coordinate of the
/// full 2N space — phase rows and rate rows alike — holding `Σ over rotations (oldBasis − newBasis)·drift`.
/// The decode adds it, so the sound the patch is standing on comes out where it was while everything
/// travel reaches from there follows the NEW plane, which is the half of the verb that has to keep
/// working.
///
/// IT CANNOT BE FOLDED INTO A TRAVEL and that is why it is 2N numbers rather than two. The banked
/// difference is a combination of the old plane's directions and the new one's, and after a second turn
/// of up to four independent directions in a 2N-dimensional space; no pair of travel coordinates under
/// the current basis can name it. Per triangle is the honest size of the thing.
///
/// ZERO IS THE MIGRATION GUARANTEE, and it is exact rather than approximate: the decode adds these
/// inside the same parenthesis the travel products already sat in, and `0.0 + term` is `term` for every
/// finite term, so a patch that has never rotated a drifted sound builds precisely the character it
/// built before this existed. Checked, not asserted — see the zero-residue cases in the blend suite.
///
/// DOUBLES, and the width is not a habit. The value IS a double difference: the contributions it is
/// made of are accumulated in double by the decode below, and the whole point of banking them is to
/// reproduce `home + oldContribution` through `home + (residue + newContribution)`. Stored as floats
/// the standing point would be quantised at ~1e-7 in phase, which the character banks' rate multipliers
/// carry straight into the last bits of the float table — floats would break the guarantee this exists
/// to give.
template <size_t N>
struct SliceResidue {
	/// Indexed exactly as the basis is: `[0, N)` are the phase rows, `[N, 2N)` the rate rows.
	std::array<double, 2 * N> offset {};

	friend bool operator==(const SliceResidue&, const SliceResidue&) = default;
};

/// The decoded plane, cached from a generator, and what past rotations banked on it.
///
/// `x` and `y` are the two TRAVEL DIRECTIONS, each spanning the full 2N space: the first N components
/// say how far each triangle's phase moves per unit of travel, the last N how much its rate changes.
/// They are directions of departure from home, not positions.
///
/// The residue rides HERE, with the directions, rather than as a second argument threaded through the
/// builders — because the decode needs both and two arguments is one a caller can forget. Forgetting
/// this one is bounded: an unset residue is all zeros, which is the engine as it was before the fold
/// existed, so the failure mode is the old behaviour rather than a wrong number. `sliceBasisFor`
/// stays a pure function of the orientation and leaves it empty; `sliceFrameFor` is the one that
/// fills it in.
template <size_t N>
struct SliceBasis {
	std::array<float, 2 * N> x {};
	std::array<float, 2 * N> y {};
	SliceResidue<N> residue {};

	/// How far the travel carries coordinate `i` — one expression, used by the decode below AND by the
	/// fold that banks its difference.
	///
	/// SHARED ON PURPOSE. What the fold subtracts has to be the same arithmetic the decode will add
	/// back, down to the association, or the two would disagree in the last places and the standing
	/// point would drift by a little on every turn of the encoder. One function is how that is
	/// guaranteed rather than reviewed.
	double travelTerm(size_t i, double travelX, double travelY) const {
		return static_cast<double>(x[i]) * travelX + static_cast<double>(y[i]) * travelY;
	}

	/// Where triangle `i` is evaluated: the sound's home phase, plus what rotations banked, plus
	/// wherever the travel has carried this particular triangle.
	///
	/// AT ZERO TRAVEL AND ZERO RESIDUE THIS IS `home`, BIT FOR BIT, whatever the orientation. Both
	/// products are exactly ±0.0, `0.0 + 0.0` is exact, and adding that to a finite value is exact, so
	/// no rotation of any size can move a sound that has never been anywhere. That is the original
	/// requirement discharged by construction rather than by a test, and the test still checks it.
	///
	/// THE INNER SUM IS THE FOLD'S SUM. Residue and travel are added to each other FIRST and to home
	/// second, which is the association the fold banks its difference in — see foldResidue. Accumulated
	/// in double, and nothing here may be reassociated (-ffp-contract=off).
	double phaseFor(size_t i, double home, double travelX, double travelY) const {
		return home + (residue.offset[i] + travelTerm(i, travelX, travelY));
	}

	/// The frequency multiplier for triangle `i`. Exactly 1 at zero travel and zero residue, so the
	/// engine samples where it always did until someone moves.
	float rateFor(size_t i, double travelX, double travelY) const {
		return 1.0f
		       + static_cast<float>(residue.offset[N + i] + travelTerm(N + i, travelX, travelY));
	}
};

/// The degenerate plane: both travel directions identical, every phase coefficient 1, every rate
/// coefficient 0.
///
/// Identical rows mean Pos X and Pos Y carry you the same way — both of them acting as another gamma,
/// which is the one-dimensional world v1 lived in. Constructed explicitly by the tests that prove the
/// decode is right, and reached by no sound unless someone closes the plane on purpose: parity with
/// the hardware is a TEST CASE here rather than the shipping default (— the plugin is
/// the product, and firmware parity is a per-feature choice rather than a constraint).
template <size_t N>
SliceBasis<N> degenerateSliceBasis() {
	SliceBasis<N> basis;
	for (size_t i = 0; i < N; ++i) {
		basis.x[i] = 1.0f;
		basis.y[i] = 1.0f;
		basis.x[N + i] = 0.0f;
		basis.y[N + i] = 0.0f;
	}
	return basis;
}

namespace detail {

/// φ⁻¹ … φ⁻⁷, the seven rates a coordinate can turn at.
///
/// Tabulated rather than `pow`ed because the decode reads four entries per triangle and a basis is
/// built per row of a map. The same seven rates the character banks themselves are spaced on.
constexpr double kPlaneTurnRates[7] = { 0.6180339887498949, 0.3819660112501051, 0.2360679774997897,
	                                    0.1458980337503155, 0.0901699437494742, 0.0557280900008412,
	                                    0.0344418537486330 };

constexpr double kGolden = 0.6180339887498949;

/// The plain triangle M is decoded from: −1…1 over a unit period, continuous everywhere and
/// piecewise-linear in the turn, so a rotation always leaves the fold a well-behaved difference to
/// bank.
inline double planeTriangle(double at) {
	const double w = at - std::floor(at);
	return w < 0.5 ? 4.0 * w - 1.0 : 3.0 - 4.0 * w;
}

/// HOW FAR THE PLANE IS OPENED, on each half. Deviations of this size around the degenerate 1 and 0.
/// Both are tunings in the sense the record means — measured, reported, and meant to move by ear.
///
/// THE PHASE SPREAD IS THE PLANE'S OPENNESS, inheriting the deleted kPlaneSpread's job. A phase
/// coefficient of exactly 1 is the degenerate value: travel moves that triangle exactly as far as home
/// does, its iso-line lies at exactly −45°, and enough of them together is the coherent diagonal #29
/// convicted. At 1.0 the measured slope spread is 0.42 — the prototype's own number, well clear of the
/// 0.24 that produced the diagonal.
///
/// THE RATE SPREAD IS THE FREQUENCY LADDER'S TUNING KNOB — the decision that rotation, not a bank
/// retune, is where the spacing comes from (#33, #34). It is set against a GUARD rather than for the
/// biggest number, and the guard is the interesting part: a rate near zero FREEZES a triangle, which
/// inflates an octave span without the ladder having widened at all. Measured over seven homes and
/// twenty-four turns at travel 0.9, best middle-80% span / distinct of 35 / slowest rate reached:
///
///   spread 0.90   4.26 octaves   30 distinct   slowest rate 0.191   honest
///   spread 1.00   4.28–4.89      29–33         slowest rate 0.100   honest
///   spread 1.10   7.00 octaves   32 distinct   slowest rate 0.012   inflated
///   spread 1.20   6.98 octaves   32 distinct   slowest rate 0.000   a triangle stops dead
///
/// So 1.0 is the widest ladder that owes nothing to a stopped triangle. THE BAR WAS ITSELF INFLATED
/// and that is worth recording rather than quietly banking: the six-angle decode this replaces reaches
/// 2.78–3.02 octaves with slowest rates of 0.001 to 0.015 at five homes of seven, and the prototype's
/// 4.80 came from a variant whose rate range was ±1.1. The comparison that survives the guard is the
/// one to quote — the WORST home under M beats the BEST home under the old decode on both axes, 4.28
/// against 3.02 octaves and 29 against 21 distinct scales.
constexpr double kPhaseSpread = 1.00;
constexpr double kRateSpread = 1.00;

/// How much faster the rate rows turn than the phase rows: a triangle's "how far it moves" and "how
/// fast it varies" must not turn together, or the two halves of the space would be one control again.
/// Measured against 1.0 and 2.6; 1.7 was the best of the three on the distinct-frequency count.
constexpr double kRateRowTurn = 1.7;
constexpr double kRateRowOffset = 0.3819660112501051;

/// TWO LADDERS, AND THE SECOND IS WHY A TURN IS NOT A DISGUISED MOVE.
///
/// M reads one triangle per coordinate at `rotate·a_i + home·b_i + offset`. If a_i and b_i were the
/// same ladder, turning by Δ would be exactly the same as moving home by Δ for every coordinate at
/// once — rotate would be a second spelling of gamma, which was excluded from the beginning.
/// Different strides make the RATIO vary with the coordinate, so no home shift can imitate a turn.
///
/// AND EVERY COORDINATE GETS A DIFFERENT PAIR, by the Chinese remainder theorem rather than by luck:
/// 5 and 7 are coprime, so `(i mod 5, i mod 7)` runs through 35 distinct pairs across morph's 35
/// coordinates and 9 across stair's 9. That is the property the deleted `rotationPlaneA` stride only
/// claimed to have — it shared a factor with 35, so its orbit was 7 of 35 however many angles were
/// stored (A39). The bug is deleted here rather than fixed.
///
/// HOME STAYS SLOW: every home rate is φ⁻¹ or below, the same "long periods" rule the orientation
/// field was built under, so a neighbourhood keeps a look and gamma is what takes you to a different
/// country. The character banks run from 0.79 to 6.04 cycles per unit of home, so a plane drifting at
/// 0.62 and below puts nothing finer on the chart than the banks already do — which is what leaves
/// #27's wide-zoom Nyquist rider where it was.
inline double planeTurnRate(size_t i) {
	return kPlaneTurnRates[i % 5];
}
inline double planeHomeRate(size_t i) {
	return kPlaneTurnRates[i % 7];
}

/// Y READS THE SAME FIELD A QUARTER PERIOD ALONG, which is what makes the second travel direction a
/// genuinely different direction rather than a sign flip of the first. A HALF period would give
/// `y − 1 = −(x − 1)`, and travel would split into a sum that is pure gamma and a difference that is
/// pure deviation — the −45° diagonal again, wearing a hat, in the direction a hand reaches by moving
/// both knobs together.
///
/// AND THE OBVIOUS STATISTIC CANNOT SEE THAT, which is why the number is measured rather than argued.
/// The ANGLE between the two rows ranks the half period HIGHEST of every shift tried — 70.9°, against
/// 43.8° for a quarter — while it is the one catastrophic choice, because two rows can sit far apart
/// in the full space and still have a degenerate direction between them. The honest statistic is the
/// worst spread over EVERY direction in the plane: sweep α and β round the circle, take the mean
/// absolute deviation of the phase coefficients of αx+βy, keep the minimum. Measured over three homes
/// and twenty-five turns: **0.391 at a quarter period**, 0.225 at an eighth, 0.219 at φ⁻², and exactly
/// 0.000 at a half with its worst direction at exactly 45°. A metric that scores the known-bad highest
/// is not a metric — and the half period is now that case's known-bad rather than a footnote.
constexpr double kQuadrature = 0.25;

/// M's entry for one coordinate of one travel direction — the whole of the decode.
///
/// One φ triangle per coordinate per half, read at the ROTATE and at the HOME together, each at its
/// own pair of rates and its own offset so that no two coordinates turn together. `rateRow` picks
/// which half of the space the entry belongs to, `second` which travel direction.
///
/// THE BASE IS THE DEGENERATE VALUE and the triangle is the deviation from it: 1 for a phase
/// coordinate (travel advances it exactly as home does) and 0 for a rate coordinate (travel does not
/// change how fast it varies). So M is the degenerate plane plus a φ field, and the two spreads above
/// are how far that field swings.
inline double planeEntry(double rotate, double home, size_t i, bool rateRow, bool second) {
	const double turn = rotate * planeTurnRate(i) * (rateRow ? kRateRowTurn : 1.0);
	const double offset = std::fmod(static_cast<double>(i) * kGolden, 1.0)
	                      + (rateRow ? kRateRowOffset : 0.0) + (second ? kQuadrature : 0.0);
	const double spread = rateRow ? kRateSpread : kPhaseSpread;
	return (rateRow ? 0.0 : 1.0)
	       + spread * planeTriangle(turn + home * planeHomeRate(i) + offset);
}

} // namespace detail

/// M(rotate, home) FOR ONE COORDINATE — the decode, reachable without building a whole basis.
///
/// Rotation is (x,y)*M(rotate), where M is decoded phi triangles feeding every contribution of x,y to
/// every phi phase and frequency multiplier. This is that M, with the second
/// argument the born-open design requires: the plane a sound plays on is a function of WHERE IT IS as
/// well as of how far it has been turned. The orientation field used to carry that through a generator
/// of seven numbers; M carries it directly, and the other six go unread.
///
/// HOME IS ITS OWN ARGUMENT rather than folded into the rotate scalar, and the difference is not
/// cosmetic. Folded — reading M at a born rotate that is itself a φ triangle of home — two homes one
/// period apart would be born on exactly the SAME plane, because a triangle takes each value twice per
/// cycle; and a PINNED rotate would have no home dependence at all, so every row of a rotated sound's
/// terrain map would share one basis where today each row derives its own. Kept separate, each
/// coordinate reads home at its own rate and the plane goes on varying down the page however far the
/// sound has been turned.
///
/// The map's macro layer reads the orientation field AT THE PLANE'S COORDINATE — `home + slope ·
/// travel`, with the slope taken from a basis built here — which is what gives the drawn country forms
/// that turn when the plane turns. One decode, so the picture and the sound cannot come to disagree
/// about which way a rotation aimed things.
inline double sliceCoefficient(double rotate, double home, size_t i, bool rateRow, bool second) {
	return detail::planeEntry(rotate, home, i, rateRow, second);
}

/// Derive the plane from the orientation. PURE: the same generator always gives the same basis, on
/// any machine, however many times it has been asked — which is what makes a retrace exact.
///
/// M(rotate), which is what it should have been all along: rotation as (x,y)*M(rotate), where M is
/// decoded phi triangles feeding every contribution of x,y to every phi phase and frequency
/// multiplier. One scalar in, 4N triangle reads out, both halves of the space
/// fed by construction — three mechanisms fewer than the stack it replaced: an aperture with its own
/// weights, a stride pairing whose orbit covered 7 of morph's 35 coordinates however many angles were
/// stored, and a spread mixed from six of them. It also measures better than any of them: 30 distinct
/// effective frequencies against 21, and 4.80 octaves against 2.94.
///
/// BORN OPEN, which is the one real design question in the redesign: M(0) is a
/// plane, not a line. The alternative — an anchor making M(0) exactly degenerate — was arithmetically
/// prettier and audibly worse, because a sound would then have no second dimension until somebody
/// turned something, and travel would be a second gamma again for every unrotated patch. The
/// bit-exact degenerate plane keeps living where it belongs: `degenerateSliceBasis`, constructed by
/// the firmware-parity cases.
///
/// HOME IS AN ARGUMENT, not a thing folded into the turn, and the two are what M is a function OF.
/// A rotate of 0 is a real plane at every home — different at every home — so a fresh sound is born
/// open where it stands, and turning the corner moves it from there.
template <size_t N>
SliceBasis<N> sliceBasisFor(double rotate, double home) {
	SliceBasis<N> basis;
	for (size_t i = 0; i < N; ++i) {
		basis.x[i] = static_cast<float>(sliceCoefficient(rotate, home, i, false, false));
		basis.y[i] = static_cast<float>(sliceCoefficient(rotate, home, i, false, true));
		basis.x[N + i] = static_cast<float>(sliceCoefficient(rotate, home, i, true, false));
		basis.y[N + i] = static_cast<float>(sliceCoefficient(rotate, home, i, true, true));
	}
	return basis;
}

/// THE ROTATE A STORED GENERATOR CARRIES.
///
/// Under the pre-shrink layout this line WAS the migration: the corner had only ever driven the six
/// stored angles in lockstep with ratio[0] = 1 (explore_stage.cpp, A23), so the six were always ONE
/// degree of freedom and `angles[0]` was exactly the turn a hand produced. The generator now stores
/// that scalar and nothing else; the legacy blocks still load through the same reading (angle 0 is
/// the turn), pinned by the raw-byte cases in the bridge suite. A plane pinned by
/// FEATHERS_AXIS_ROTATION with angles chosen independently was accepted as non-migrating — a recorded
/// decision, and that env var is a debug handle rather than a patch format.
///
/// A SOUND WITH NO PIN HAS A ROTATE OF ZERO, which is `rotateFor` in slice_build.h rather than
/// anything here: unpinned means "not turned", and where the sound IS enters the decode as home.
inline double sliceRotateOf(const SliceGenerator& generator) {
	return static_cast<double>(generator.rotate);
}

/// THE MEAN DIRECTION THE PHASE ROW POINTS — the slope of the plane's own coordinate.
///
/// Triangle `i` keeps its value along `dhome/dtravel = −x[i]`, so the phase row IS the set of banding
/// directions the chart draws (#29 measured the picture with exactly this identity). The mean over the
/// bank is the direction the map as a whole leans, and the landscape reads its field along it — see
/// terrainMacroAt. Taken from a basis the caller already built rather than recomputed from M, so the
/// picture cannot come to disagree with the sound about which way the plane is aimed.
template <size_t N>
double meanPhaseSlope(const SliceBasis<N>& basis) {
	double sum = 0.0;
	for (size_t i = 0; i < N; ++i) {
		sum += static_cast<double>(basis.x[i]);
	}
	return sum / static_cast<double>(N);
}

/// THE SLOPE THE LANDSCAPE LEANS ALONG, and it is deliberately ONE coordinate rather than the mean.
///
/// MEASURED, because the obvious choice is the wrong one. The macro layer reads its field at
/// `home + slope·travel`, and taking the slope from `meanPhaseSlope` makes the country almost immobile:
/// a mean over thirty-five coefficients of a zero-mean field is a stable statistic, so it sits at
/// 1.0 ± 0.1 and a turn moves the drawn macro by 0.0012 per texel at a browsing zoom — one part in a
/// thousand of its range, which is nothing to an eye. One coordinate's own slope swings the full width
/// of the row instead: 0.018 per texel, fifteen times as much, and the country's bands swing through
/// forty-one degrees on the same turn.
///
/// COORDINATE 0 BY CONSTRUCTION RATHER THAN BY LUCK: `i mod 5` and `i mod 7` are both zero there, so it
/// sits on the FASTEST rung of both ladders — φ⁻¹ in the turn and φ⁻¹ in home — which makes it the
/// coordinate that swings most per unit of encoder and drifts most across a country. The landscape
/// wants the liveliest of the thirty-five, not their average.
///
/// WHAT THIS DOES NOT BUY, so nobody re-derives the hope: the macro stays a BAND field. Its coherence
/// is 0.97 at a browsing zoom under every variant tried, because the deleted orientation field was a
/// function of ONE scalar and the level sets of a one-scalar field are lines however parameterised.
/// Real two-dimensional forms need the macro's own components opened the way the character's were —
/// that is task #37 and the intended mechanism, not something a slope can reach.
template <size_t N>
double leadPhaseSlope(const SliceBasis<N>& basis) {
	return static_cast<double>(basis.x[0]);
}

/// The plane a sound plays on, with what its past rotations banked already attached — the one call
/// that gives a builder everything the decode needs. `sliceBasisFor` deliberately does not attach the
/// residue: it is the pure function of the orientation that §1.2 requires, and a basis that carried a
/// residue would be a basis nobody could compare against a freshly derived one.
template <size_t N>
SliceBasis<N> sliceFrameFor(double rotate, double home, const SliceResidue<N>& residue) {
	SliceBasis<N> basis = sliceBasisFor<N>(rotate, home);
	basis.residue = residue;
	return basis;
}

/// BANK WHAT THIS TURN WOULD OTHERWISE HAVE MOVED — the fold that makes rotate leave the standing
/// point alone.
///
/// Called with the plane the sound was on, the plane it is about to be on, and the drift it is
/// standing at. What comes back is the residue to store beside the new plane.
///
/// WHY IT IS DRIFT AND NOT TRAVEL (A26): drift is what the patch says this sound IS, and the puck's
/// Weather-scaled contribution is where you happen to be listening from. Banking the puck's position
/// would freeze a performance into the patch — turn Weather down afterwards and the sound would not
/// come back, because the residue would still be carrying a gesture. So with Weather up and the puck
/// off-anchor, rotating still changes what you hear: you are genuinely somewhere else, rather than
/// standing on the sound.
///
/// EXACT AT ZERO DRIFT, BY CONSTRUCTION. Both travel terms are exactly ±0.0 there, their difference is
/// exactly ±0.0, and adding it changes no bit — so the old held-at-home guarantee survives as this
/// one's corollary rather than as a separate mechanism.
///
/// AT NONZERO DRIFT IT IS ONE ROUNDING, and saying so is the honest version. The banked difference has
/// to land in a finite double, which is arithmetic rather than a design choice; what the code CAN do is
/// make the fold's expression the decode's own — `(banked + oldTerm) - newTerm` against the decode's
/// `residue + newTerm` — so the error is one ULP of the inner sum per rotation, around 1e-16, some nine
/// orders below what the float character table can resolve. The bit-identity of the standing point is
/// therefore a MEASURED property with a many-rotations control behind it, not a structural claim.
template <size_t N>
SliceResidue<N> foldResidue(const SliceResidue<N>& banked, const SliceBasis<N>& before,
                            const SliceBasis<N>& after, double driftX, double driftY) {
	SliceResidue<N> folded;
	for (size_t i = 0; i < 2 * N; ++i) {
		folded.offset[i] = (banked.offset[i] + before.travelTerm(i, driftX, driftY))
		                   - after.travelTerm(i, driftX, driftY);
	}
	return folded;
}

/// The same fold, from the two rotates rather than from planes already derived — which is what a
/// caller holding the state of record has, and one fewer place to derive a basis differently.
///
/// THE HOME IS THE SAME FOR BOTH PLANES, and that is the fold's whole premise rather than an
/// optimisation: a turn happens where the sound is standing, so the before and after planes are M read
/// at one place and two rotates. A fold given two homes would be banking the difference between a turn
/// and a move, which no rotation ever performed.
template <size_t N>
SliceResidue<N> foldResidue(const SliceResidue<N>& banked, double rotateBefore, double rotateAfter,
                            double driftX, double driftY, double home) {
	return foldResidue(banked, sliceBasisFor<N>(rotateBefore, home), sliceBasisFor<N>(rotateAfter, home),
	                   driftX, driftY);
}

/// One sound's banked residue, for BOTH engines.
///
/// A rotation folds both, whichever engine is playing at the time, because the oscillator type is a
/// parameter a player can change afterwards — and a morph patch that had been rotated at a drift, then
/// switched to stair, would otherwise find its stair character had never heard of the turns.
struct PlaneResidue {
	SliceResidue<kMorphTriangles> morph {};
	SliceResidue<kStairTriangles> stair {};
	/// The vox engine's rows, added by V-C2 — an engine a player can switch to after the fact is an
	/// engine whose rows a turn must already have folded, which is the same argument stair's rows
	/// were added under and not a new one.
	SliceResidue<kVoxTriangles> vox {};
	/// The Resonate engine's rows (2026-09-25), on that same argument once more: its laws are read
	/// through the plane like Stair's staircases, so a turn made while Morph was playing has to have
	/// folded them already.
	SliceResidue<kWeaveTriangles> weave {};
	/// The additive engine's rows (2026-10-08), on the same argument again: its spectrum is read
	/// through the plane, so a turn made while another engine was playing must have folded them.
	/// APPENDED LAST, after weave's, by the rule kPlaneResidueValues states.
	SliceResidue<kPartialTriangles> partials {};

	friend bool operator==(const PlaneResidue&, const PlaneResidue&) = default;
};

/// How many numbers one sound's residue is, and the five ends of the ONE flat order they are read
/// in.
///
/// The bridge's atomics and the patch's state block both have to lay these out, and two orderings
/// written independently would agree for as long as nobody added a triangle. Morph rows first, stair
/// rows after, vox rows next, weave rows after them, partial rows last, said once here.
///
/// VOX APPENDS, AND THAT IS THE MIGRATION GUARANTEE RATHER THAN A TIDINESS PREFERENCE. Every index
/// below 88 means exactly what it meant in every patch ever saved, so the stance wire's residue
/// members — which are this order, offset by eight — grow by pure append: a shipped record's numbers
/// land in the rows they were written from, and the twenty-four new ones take the zero fill that IS
/// "this sound never rotated a vox character", because it could not have. Insert vox anywhere else
/// and every stair row in every saved patch shifts under a loader that has no way to know it. WEAVE
/// APPENDS AFTER VOX for the same reason — but the stance wire had grown two orbit members after the
/// residue by then, so the wire places the weave rows after THEM (kPreWeaveResidueValues is where the
/// wire splits the order; see sound_variations.h). THE PARTIAL ROWS APPEND AFTER WEAVE (2026-10-08),
/// and on the wire after the weave rows too, which the wire's tail branch does by itself: its rows
/// past the orbit are this order's rows from kPreWeaveResidueValues on, whatever follows weave.
inline constexpr size_t kPlaneResidueValues = 2 * kMorphTriangles + 2 * kStairTriangles + 2 * kVoxTriangles
                                              + 2 * kWeaveTriangles + 2 * kPartialTriangles;

/// Where the stair rows begin and end in the flat order — named once, because the loader has to
/// place a patch's halves by the counts that were WRITTEN and open-coding the boundary twice is how
/// the two spellings drift.
inline constexpr size_t kMorphResidueValues = 2 * kMorphTriangles;
inline constexpr size_t kStairResidueValues = 2 * kStairTriangles;
inline constexpr size_t kVoxResidueValues = 2 * kVoxTriangles;
inline constexpr size_t kWeaveResidueValues = 2 * kWeaveTriangles;
/// Everything before the weave rows: the whole order as every record written before 2026-09-25 laid
/// it out.
inline constexpr size_t kPreWeaveResidueValues = kMorphResidueValues + kStairResidueValues + kVoxResidueValues;
inline constexpr size_t kPartialResidueValues = 2 * kPartialTriangles;
/// Everything before the partial rows: the whole order as every record written before 2026-10-08 laid
/// it out.
inline constexpr size_t kPrePartialResidueValues = kPreWeaveResidueValues + kWeaveResidueValues;

inline double planeResidueValue(const PlaneResidue& residue, size_t i) {
	if (i < kMorphResidueValues) {
		return residue.morph.offset[i];
	}
	if (i < kMorphResidueValues + kStairResidueValues) {
		return residue.stair.offset[i - kMorphResidueValues];
	}
	if (i < kPreWeaveResidueValues) {
		return residue.vox.offset[i - kMorphResidueValues - kStairResidueValues];
	}
	if (i < kPrePartialResidueValues) {
		return residue.weave.offset[i - kPreWeaveResidueValues];
	}
	return residue.partials.offset[i - kPrePartialResidueValues];
}

inline void setPlaneResidueValue(PlaneResidue& residue, size_t i, double value) {
	if (i < kMorphResidueValues) {
		residue.morph.offset[i] = value;
	}
	else if (i < kMorphResidueValues + kStairResidueValues) {
		residue.stair.offset[i - kMorphResidueValues] = value;
	}
	else if (i < kPreWeaveResidueValues) {
		residue.vox.offset[i - kMorphResidueValues - kStairResidueValues] = value;
	}
	else if (i < kPrePartialResidueValues) {
		residue.weave.offset[i - kPreWeaveResidueValues] = value;
	}
	else {
		residue.partials.offset[i - kPrePartialResidueValues] = value;
	}
}

/// An orientation held fixed by hand, or the absence of one.
///
/// A sound normally plays the turn its own state carries — none at all is rotate 0, the open plane
/// its home gives it (see rotateFor in slice_build.h). This is the experimental override: the debug
/// panel and FEATHERS_AXIS_ROTATION pin a turn to hear one deliberately. By design it stays, and it
/// may never become a CLAP param — it lives in the UI only, to pin values experimentally — which also
/// keeps rotate non-automatable, as the design always had it.
struct SliceOrientationPin {
	bool pinned = false;
	SliceGenerator generator {};

	friend bool operator==(const SliceOrientationPin&, const SliceOrientationPin&) = default;
};

} // namespace feathers
