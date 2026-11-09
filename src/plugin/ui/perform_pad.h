/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/control_face.h"
#include "fx/sklitter/perform.h"
#include "plugin/ui/theme.h"

#include "visage_graphics/shader.h"
#include "visage_ui/frame.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

// **THE PERFORM CIRCLE'S GEOMETRY, AS FREE FUNCTIONS OVER A DISK — "ONE DRAG ROAD, TWO SURFACES".**
//
// The hit-test, the weight computation and the drag-to-parameter decomposition are one free function
// over a disk and a rectangle, called by both surfaces. Only the expanded panel draws the circle today;
// the card is deliberately empty of it for now. **The road is built shared anyway, and that is not speculation:** `sklitter_expanded.h`
// records that this device has paid FOUR times for a fix that reached one surface of two — density
// reaching the slice modes and not Grain, the hard cut reaching the looper renderer and not the grain
// renderer, the repaint reaching the card and not the panel, and the panel's own frame request. A second
// copy of "where is the puck" written when the card gets one is the fifth.
//
// **THE WEIGHTS ARE NOT HERE, AND THAT IS THE POINT.** A reader that wants to draw how much each
// waypoint is contributing calls `fx::sklitter::performWeightsOf` — the engine's own function, the one
// the sound is made from (`perform.h` states the contract). Nothing in this header recomputes a weight;
// it converts between PIXELS and the two axes, which is the only thing a surface knows that the engine
// does not.
namespace feathers::ui::perform_pad {

/// THE DISK A SURFACE DREW, in whatever pixel space that surface is laying out in.
struct Disk {
	float cx = 0.0f;
	float cy = 0.0f;
	float radius = 0.0f;

	[[nodiscard]] constexpr bool valid() const noexcept { return radius > 0.0f; }
};

/// THE LARGEST DISK THAT FITS A RECTANGLE, centred in it, less `inset` on every side.
///
/// `inset` is what a caller reserves for the things drawn AT the rim — a waypoint mark, a label — so
/// the disk is the PLAYABLE area and not the ink. A caller that wants them flush passes 0.
[[nodiscard]] inline Disk diskIn(float x, float y, float width, float height, float inset = 0.0f) {
	Disk d;
	d.cx = x + width * 0.5f;
	d.cy = y + height * 0.5f;
	d.radius = std::max(0.0f, std::min(width, height) * 0.5f - inset);
	return d;
}

/// WHERE A PUCK AT `(ax, ay)` IS DRAWN. x right, y DOWN — every screen space in this editor, and
/// `perform.h`'s own convention, so N is `ay = -1` and sits at the TOP.
///
/// **IT DOES NOT CLAMP, AND THAT IS DELIBERATE.** `Perf X` and `Perf Y` are modulation destinations, so
/// an automated or routed pair can be (1, 1) — which `performWeightsOf` accepts (it clamps to the
/// SQUARE) and which lies outside the drawn disk by a factor of √2. Drawing it where the parameters say
/// is the honest picture of a state the engine can really be in; moving it onto the rim would be the
/// surface telling a story about a value it did not have. The DRAG clamps (see `axesAt`); the drawing
/// does not.
[[nodiscard]] inline std::pair<float, float> pointOf(const Disk& d, float ax, float ay) {
	return { d.cx + ax * d.radius, d.cy + ay * d.radius };
}

/// WHERE A WAYPOINT'S MARK GOES — the four DIAGONAL rim points, design §2 as amended 2026-09-19.
///
/// **THE AXES ARE `perform.h`'s, RESTATED AS POINTS RATHER THAN RE-DECIDED.** `kWaypointAxis` says
/// where a waypoint is; this says where to DRAW it, and the two must agree or the mark would be a
/// picture of a weight the engine does not give. The component is `fl(1/√2)` because a rim point on a
/// diagonal is at 45°, which is exactly the puck position that reads as that waypoint whole.
///
/// **AND THE ORDER IS THE CELLS' ORDER.** The cardinals sit 45 degrees round, to match the cells'
/// locations — `kCfg1` top-LEFT, `kCfg2` top-right, `kCfg3` bottom-right, `kCfg4` bottom-left,
/// each mark beside the square that configures it. `CircleRail::resized` places the four cells from
/// the same reading, and a case asserts the mark and its cell are on the same side in BOTH axes.
[[nodiscard]] inline std::pair<float, float> waypointPoint(const Disk& d, fx::sklitter::Corner corner) {
	// **ASKED, NOT RESTATED.** A second copy of the four directions here is a second thing to rotate,
	// and the copy that is not rotated draws a mark where no waypoint is.
	const auto [ax, ay] = fx::sklitter::waypointAxis(corner);
	return pointOf(d, ax, ay);
}

/// THE AXES A DRAG AT `(px, py)` MEANS — **clamped to the unit DISK, not to the square.**
///
/// **CLAMPING TO THE DISK LOSES NOTHING, and that is arithmetic rather than taste.** The weights depend
/// on `(x, y)` in two regimes (`perform.h`): INSIDE the centre region the centre keeps a share, and
/// OUTSIDE it that share has clamped to zero and the normalise divides by the corners' sum, which
/// leaves the weights a function of the DIRECTION alone — so every point beyond the region has a point
/// on this disk's rim with identical weights. The square's corners are therefore reachable in effect
/// without being reachable by a hand, and a circular pad is a faithful instrument for a square-clamped
/// parameter pair.
///
/// **THE REGION ROTATED WITH THE WAYPOINTS (2026-09-19) AND THE ARGUMENT SURVIVES UNCHANGED** — which
/// is the point of stating it as a property rather than as a picture. It was the diamond
/// `|x| + |y| <= 1`, whose vertices were the four cardinal rim points; it is now the square
/// `max(|x|, |y|) <= √2/2`, whose CORNERS are the four diagonal rim points. Either way its extreme
/// points are at distance exactly 1, so the whole region is inside this disk and nothing the centre
/// share can express is out of a hand's reach.
///
/// A degenerate disk (zero radius, a surface laid out at no size) answers the CENTRE rather than
/// dividing by it.
[[nodiscard]] inline std::pair<float, float> axesAt(const Disk& d, float px, float py) {
	if (!d.valid()) {
		return { 0.0f, 0.0f };
	}
	float ax = (px - d.cx) / d.radius;
	float ay = (py - d.cy) / d.radius;
	const float len = std::hypot(ax, ay);
	if (len > 1.0f) {
		ax /= len;
		ay /= len;
	}
	return { ax, ay };
}

/// WHETHER A POINT IS ON THE PAD AT ALL. `slack` widens the disk for the grab — a puck sitting ON the
/// rim is easier to take hold of than a rim with no thickness — and a caller that means the drawn disk
/// exactly passes 0.
[[nodiscard]] inline bool hits(const Disk& d, float px, float py, float slack = 0.0f) {
	if (!d.valid()) {
		return false;
	}
	const float reach = d.radius + std::max(0.0f, slack);
	return std::hypot(px - d.cx, py - d.cy) <= reach;
}

} // namespace feathers::ui::perform_pad

namespace feathers::ui {

class EditHistory;

/// **THE PERFORM CIRCLE AS A SURFACE: a disk, four waypoint marks and a DRAGGABLE puck.**
///
/// It is the PLAIN version on purpose: the GPU work had landed, and anything fancier (flying sparks
/// and the like) was deliberately left for later. So there is no field, no wash, no glow and no trail — `perform_stage.h` argues the general case for exactly
/// this restraint, and the cool version lands on top of these parts without moving them.
///
/// **THE PUCK IS DERIVED, NEVER STORED.** It is `Perf X` and `Perf Y` read at the moment it is drawn,
/// so there is no second copy of the position to fall out of step with the parameter — `perform_stage.h`'s
/// first rule, restated by the design because this surface is where it bites. The pad holds no `x_`/`y_`
/// of its own, and a reader looking for one should not find one.
///
/// **AND IT IS READ THROUGH `performWeightsOf`, NOT OFF THE CONTROLS DIRECTLY.** That function returns
/// the SANITISED axes beside the weights (`PerformWeights::x`/`y`) precisely so a second reader does not
/// have to de-NaN and clamp for itself — a second road is the copy that goes stale. So a NaN axis draws
/// as centred, which is what it SOUNDS like, and the drawn puck and the played push are one value's two
/// halves.
///
/// **THE WEIGHTS THE MARKS ARE DRAWN AT ARE THE ENGINE'S**, from the same call: a mark's fill is
/// `w.corner[i]`, the share that waypoint has of the sound right now. Nothing here recomputes a weight,
/// which is `perform.h`'s stated contract and is what keeps the picture and the sound the same numbers.
///
/// **AND SINCE 2026-09-20 IT HAS A STARFIELD, WHICH IS FEATHERS' OWN SKY AND NOT A SECOND ONE** — a
/// starfield like Feathers' perform surface inside the circle, without the twinkle, the stars moving
/// warped by the puck's position. The stars are
/// `performSky()` — the same 110 on seed 55 that `PerformStage` draws, shared out of `celestial.h`
/// exactly so a second table never gets written.
///
/// **THE FIELD HAS NO CLOCK, AND THAT IS THE WHOLE DESIGN OF IT.** A twinkle is a function of TIME: it
/// dirties a frame forever with no hand on anything, which is what backlog GJ found the Explore chart
/// doing at rest for weeks. A WARP is a function of the PUCK, so the field is perfectly still while the
/// hand is still and moves only during a drag — and the puck moving already dirties that frame, so the
/// warp costs nothing at rest and nothing extra in motion. `drawSky` reads two parameters and one
/// immutable table, and that has not changed.
///
/// **SO THE FIELD IS A FUNCTION OF `(performSky(), puck)` AND OF NOTHING ELSE**, and that sentence is
/// the honest description of what it encodes: not the blend, not the waypoints, not which sounds are
/// playing — the instrument's main gesture, and only that. It is worth saying plainly because a field
/// that LOOKS like it carries information and does not is the honesty rule pointed at a picture.
///
/// **AND SINCE 2026-09-21 THERE IS A SECOND LAYER OVER IT THAT DOES HAVE A CLOCK: THE SPARKS (backlog
/// HR).** The puck emits sparks as a Poisson process driven by the instantaneous level, each with a
/// momentum set by the puck's velocity vector at the moment it is emitted. So this class now has `updateSparks()`, which the panel's tick calls — the thing the
/// paragraph above said it did not have. **The paragraph is not deleted, because the property it was
/// protecting SURVIVED and is now the sparks' hardest requirement:**
///
/// **WITH NO SPARK ALIVE THIS PAD ASKS FOR NOTHING, and it is `updateSparks`'s RETURN VALUE that says
/// so.** No sound means no emission means no live spark means no frame requested — byte-identical to
/// the pad's behaviour before any of this, and asserted by a case that ticks the pad a hundred times
/// at silence and sums what it answered. **That is a count produced BY the asking**, which is the shape
/// CLAUDE.md calls for where a repaint cannot be observed headlessly; a counter standing beside the
/// `redraw()` could be incremented by a build that had stopped calling it.
///
/// **AND THE SPARKS CARRY LEVEL, WHICH IS NEW INFORMATION AND IS THE POINT.** The field says where the
/// puck is; the sparks say how loud what the instrument is making is, and how fast the hand was moving
/// when it made it. Both are true of the device — `Reading::OutLevel` is the WET times the MIX, so a
/// bypassed device on a loud track sparks not at all (that enumerator argues the choice) — so this
/// surface still claims exactly what it can back.
///
/// **THE LEVEL IS AN INPUT, NEVER A READ.** `updateSparks(level)` is HANDED the number by whoever owns
/// the device handle; this pad holds no `DeviceHost` and asks nothing. It is the same reason the puck
/// is two `Modulatable&`s rather than a device id: a widget that went and fetched its own state is a
/// second road to it, and the panel already has the one road.
///
/// **THE WARP IS A MAPPING, NEVER AN ACCUMULATION.** Every draw starts from the seed positions and
/// applies the pull once, so returning the puck to where it was returns the field to where it was, by
/// construction rather than by care. That is the property the there-and-back case pins: a field that
/// integrated its own displacement would be right in every single frame and drift across a session,
/// which is the ABA shape wearing a picture.
///
/// **WHAT IS STILL DELIBERATELY NOT HERE: THE WAYPOINTS' OWN SPARKS.** The earlier framing was that
/// either the puck or the waypoints send out sparks when things happen, and what was built is the
/// PUCK. A waypoint emitter would be a different process about a different quantity (an
/// event — a slice landing — rather than a level), so it is a second thing and not a parameter of this
/// one.
///
/// **AND THE SPARKS ARE ORDINARY QUADS, NOT A SHADER, WHICH IS A MEASUREMENT AND NOT A PREFERENCE.**
/// visage batches by shape TYPE (`ShapeBatcher::autoBatchIndex`), so N `canvas.fill` calls with one
/// blend mode and one brush kind coalesce into ONE `bgfx::submit` with one transient vertex buffer, up
/// to `kMaxQuadsPerBatch` = **16384 quads**. `kMaxSparks` is 192 against the 110 stars this same pad
/// already submits every frame, so the whole layer is about 1% of one batch. **A custom `visage::Shader`
/// would be slower to write and could not do the job**: `ShaderWrapper` uses the `Shader*` itself as the
/// batch id and `submitShader` reads the uniforms once off `batches[0]`, so every quad in the batch
/// shares one set of uniforms and carries no per-quad payload (`shader.h`'s own note). A Poisson process
/// with a per-spark BIRTH VELOCITY is per-particle state by construction; without a state texture, which
/// we have no road to, it cannot live in a fragment shader. So the state is on the CPU and the drawing is
/// on the GPU, which is the only place "GPU accelerated" was ever going to land here.
class PerformPad final : public visage::Frame, public ControlFace {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	/// @param x the `Perf X` control, and @param y `Perf Y` — the two the gesture writes and the one
	///          place the puck is read from.
	PerformPad(device::Modulatable& x, device::Modulatable& y);
	/// Closes a gesture a hand was still holding, so the history's group does not stay open for ever.
	~PerformPad() override;

	/// **THE HISTORY A MOVE IS ONE STEP OF.** A point is two controls, and each `DeviceControl` records
	/// its own step at `endGesture`; the pad brackets the pair as one group so one move is one undo.
	/// Null (the default) leaves the controls recording on their own.
	void setHistory(EditHistory* history);

	/// **BACK TO THE REST POINT — both axes to their DECLARED defaults, as one gesture.** What a
	/// double-click anywhere on the pad does. The BASE only: routes into `Perf X`/`Perf Y` are left alone. Returns
	/// whether anything moved — a puck already at rest spends no undo step.
	bool centre();

	void draw(visage::Canvas& canvas) override;

	/// **THE GEOMETRY THIS PAD IS CURRENTLY LAID OUT AS**, in its own local space — the same `Disk`
	/// every function above takes, so a case asking "what does a click here write" computes the answer
	/// with the production arithmetic rather than a second copy of it.
	[[nodiscard]] perform_pad::Disk disk() const;

	/// TEST SEAMS. The puck's drawn point and the weights the marks were drawn at — both DERIVED on
	/// every call, exactly as `draw` derives them, so a case cannot read a cached copy that `draw` does
	/// not use.
	[[nodiscard]] std::pair<float, float> puckPointForTest() const;
	[[nodiscard]] fx::sklitter::PerformWeights weightsForTest() const;

	/// ONE STAR AS IT WAS ACTUALLY DRAWN: which of `performSky()`'s stars it is, and where in this
	/// pad's own pixels it landed.
	struct DrawnStar {
		size_t seed = 0;
		float x = 0.0f;
		float y = 0.0f;
	};

	/// **THE FIELD THE LAST `draw` SUBMITTED, recorded BY the draw rather than recomputed beside it.**
	/// Empty until the pad has painted once — a case must REQUIRE against that or it is questioning a
	/// surface that never drew. It is what the determinism claims are made against: a field is STILL
	/// only if two draws in the same state agree BIT FOR BIT, and a field is a MAPPING rather than an
	/// accumulation only if going away and coming back lands on the same points.
	[[nodiscard]] const std::vector<DrawnStar>& starsForTest() const noexcept { return drawnStars_; }

	//----------------------------------------------------------------------------------------------//
	// THE SPARKS — backlog HR. See the class header for the zero-repaint requirement and for why     //
	// this is CPU state drawn as batched quads rather than a shader.                                 //
	//----------------------------------------------------------------------------------------------//

	/// **ONE SPARK, IN AXIS UNITS — disk radii and disk radii per second, never pixels.** The pad
	/// scales (`scale()`), so a velocity in pixels would mean a different gesture at every panel size
	/// and the same drag would fling further on a big window. Axis units are the space `pointOf` maps
	/// from, so the conversion happens once, in `draw`, exactly where the stars' does.
	struct Spark {
		/// **WHERE IT WAS BORN, AND IT NEVER CHANGES.** Since backlog HR's GPU pass a spark is written
		/// once and never touched again: its position at any instant is a CLOSED FORM of this, its birth
		/// velocity and the clock (`sparkOffsetAt`), evaluated in the vertex shader. There is no
		/// integration and therefore no per-frame state to be stepped, in the right order, exactly once.
		float x = 0.0f;
		float y = 0.0f;
		/// **THE VELOCITY IT WAS BORN WITH, plus whatever drag has since taken off it.** Momentum is
		/// set by the puck's velocity vector at the moment of emission — so this is a per-spark quantity and not a property of the emitter, and a spark flung off a fast drag
		/// keeps going after the hand has stopped. That contrast IS the feature.
		float vx = 0.0f;
		float vy = 0.0f;
		/// **WHEN IT WAS BORN, on the pad's own clock, seconds since this pad's epoch.** It replaced an
		/// `age` that had to be advanced every tick — which is the one field that made a spark STATE
		/// rather than a constant, and the reason the CPU had to walk the population.
		float birth = 0.0f;
		float life = 0.0f; ///< seconds it will last; `(now − birth)/life` is its fade
		/// **THIS SPARK'S OWN RANDOM NUMBER, 0..1 — and it is a stored float rather than a hash of
		/// something else.** The first GPU version hashed `(vx, vy)` in the shader, which was free and
		/// good enough for a twinkle. A shape that EVOLVES wants better: hashing the velocity ties every
		/// shape decision to the direction the spark happens to be flying, which is the correlation a
		/// random number exists to break. It is also the one number a future SPLIT derives its age, its
		/// angle and its kick speed from, so spending a float on it now is what keeps that possible.
		float seed = 0.0f;
		/// **WHICH OF THE TWO EMITTERS MADE IT**, which is the whole of the second colour: the wet output
		/// and the grain fx output emit two different colours — so a spark
		/// carries WHICH ROAD its sound took, and the two populations divide the picture exactly as
		/// the two levels divide the sound (`Reading::WetLevel`).
		bool fromLane = false;
		/// **A GRAIN ONSET'S SPARK, OR A LEVEL SPARK — backlog HR round 2.** 0 for the two level emitters;
		/// otherwise one plus the grain's gate class (`fx::sklitter::grainOnsetClass`), which is what its
		/// ink and its shape are chosen from. Grain onsets emit larger sparks, coloured by
		/// the grain's sampled gates.
		uint8_t onset = 0;
		/// **A REPEAT'S SPARK** (repeat slices get the split spark, splitting roughly every repeat) —
		/// thrown each time Repeat restarts its slice, and it ALWAYS splits, paced so its
		/// child parts about when the next repeat fires (`sparkRepeatKick`). Exclusive with `onset`.
		bool repeat = false;
		/// **THE SPLIT — the spare per-quad float, spent.** A SIGNED child kick in axis units per second;
		/// zero means this spark never splits. The sign is the side the child leaves on. Decided once,
		/// at birth, by `sparkSplitFor`; everything after — when it splits, where the child goes, how the
		/// lobe grows and pinches off — is a closed form of this, the seed, the velocity and the clock,
		/// evaluated in `fs_spark.sc` and restated in `sparkChildPointAt`.
		float split = 0.0f;
	};

	/// MILLISECONDS, the same `Clock` shape `PerformStage` and `ExploreStage` use — injectable for the
	/// same reason, which is that an animation nobody can pin the time of cannot be asserted at all.
	using Clock = std::function<int64_t()>;

	/// **HOW FAR A SPARK HAS TRAVELLED AFTER `t` SECONDS — THE CLOSED FORM, AND IT IS THE WHOLE OF THE
	/// GPU PASS.**
	///
	/// Under exponential drag `v' = −k·v` the velocity is `v0·e^(−k·t)` and its integral is exact:
	///
	///     p(t) − p0 = v0 · (1 − e^(−k·t)) / k
	///
	/// **A FUNCTION OF BIRTH DATA AND THE CLOCK, AND OF NOTHING ELSE** — no previous frame, no ordering,
	/// no state. That is precisely what lets a vertex shader compute it, and it is why moving spark
	/// translation purely onto the GPU is a structural change rather than a port: an integrator cannot
	/// move to the GPU without a place to keep the thing it is integrating.
	///
	/// **THIS FUNCTION AND `vs_spark.sc` ARE TWO HALVES OF ONE FORMULA AND MUST BE EDITED TOGETHER.**
	/// A case asserts this one against an independent restatement; the SHADER agreeing with it is shown
	/// by the photograph, not by any assertion we can make headlessly (see the class header).
	[[nodiscard]] static std::pair<float, float> sparkOffsetAt(float vx, float vy, float t,
	                                                           float drag) noexcept {
		const float k = std::max(1.0e-4f, drag);
		const float f = (1.0f - std::exp(-k * std::max(0.0f, t))) / k;
		return { vx * f, vy * f };
	}

	/// **WHERE A SPARK IS AT `now`, IN AXIS UNITS.** The one road both paths take, so the CPU reference
	/// render and the GPU render differ ONLY in who evaluates the formula — which is what makes an A/B
	/// photograph a test of the shader rather than of the arithmetic.
	[[nodiscard]] static std::pair<float, float> sparkPointAt(const Spark& s, float now) noexcept {
		const float t = std::min(std::max(0.0f, now - s.birth), s.life);
		const auto [dx, dy] = sparkOffsetAt(s.vx, s.vy, t, kSparkDrag);
		return { s.x + dx, s.y + dy };
	}

	//----------------------------------------------------------------------------------------------//
	// **THE SPLIT — a single spark with a special, growing shape.** Backlog HR (ii)/(iii). ONE       //
	// instance: the fragment stage grows a child lobe out of the parent and lets it drift off. These //
	// are the C++ halves of that shape's geometry and, like `sparkOffsetAt`, must be edited together //
	// with `fs_spark.sc`.                                                                            //
	//----------------------------------------------------------------------------------------------//

	/// **WHETHER A SPARK SPLITS, AND WHICH WAY — decided on the CPU, at birth, from the seed.** Returns
	/// the signed kick for `Spark::split`, zero for no split.
	///
	/// **WHY THE CPU DECIDES WHAT THE SHADER DRAWS:** the QUAD. A splitting spark needs a quad big enough
	/// to hold its child lobe at its farthest (`sparkQuadSide`), about eight times the area of a plain
	/// one, and the rectangle is fixed at submit — a vertex stage can move it, never grow it. So whoever
	/// sizes the quad must know, and a fragment stage cannot tell the CPU anything. The decision is a
	/// pure function of the seed, so it costs no random draw (every seeded count in the suites keeps its
	/// value) and no state; the shader receives its answer in the spare float.
	///
	/// **THIS IS THE RANDOM SPLIT, AND IT IS OFF BY DEFAULT (`kSparkSplitChance` 0).** Since repeat
	/// slices got the split spark, a split MEANS something — a repeat (`sparkRepeatKick`)
	/// — and a coin-flip split on grain onsets beside it would make the shape ambiguous. It is kept, as
	/// one constant, because the question is still open: raise the chance and grain onsets split at random too.
	///
	/// **ONLY GRAIN-ONSET SPARKS MAY SPLIT AT RANDOM.** They carry an EVENT; the level sparks' density is
	/// the reading of how much sound went each way, and a level spark that became two would double-count
	/// it. `fract(seed·16)` rather than `seed` itself, so the decision is not the same number as the
	/// split TIME (`sparkSplitAt`), which is linear in the seed.
	[[nodiscard]] static float sparkSplitFor(float seed, bool onset) noexcept {
		if (!onset) {
			return 0.0f;
		}
		const float scaled = seed * 16.0f;
		const float u = scaled - std::floor(scaled);
		if (!(u < kSparkSplitChance)) {
			return 0.0f;
		}
		return u < 0.5f * kSparkSplitChance ? kSparkSplitKick : -kSparkSplitKick;
	}

	/// **A REPEAT SPARK'S KICK — PACED BY THE REPEAT, so the child parts about when the next one fires.**
	///
	/// A repeat spark splits at birth (the repeat IS the split) and its child's centre drifts
	/// `kick·(1 − e^{−k·t})/k` from the parent's. "Parted" is the two centres `kSparkRepeatPart` apart,
	/// about the two lobes' radii plus the neck, so the kick that parts them at `t = period` is
	///
	///     kick = kSparkRepeatPart · k / (1 − e^{−k·period})
	///
	/// — fast repeats kick hard, slow ones drift. **CLAMPED to [`kSparkRepeatKickMin`, `…Max`]**, because
	/// the quad must hold the child at its farthest (`sparkQuadSide`) and a 20 ms repeat would otherwise
	/// ask for a quad the size of the disk; above the clamp the child parts a little LATER than the next
	/// repeat, which reads the same. An unknown period (0, nothing published) takes the minimum. The SIDE
	/// is from the seed. Axis units per second, signed — `Spark::split`.
	[[nodiscard]] static float sparkRepeatKick(float seed, float period) noexcept {
		const float k = std::max(1.0e-4f, kSparkDrag);
		float kick = kSparkRepeatKickMin;
		if (std::isfinite(period) && period > 0.0f) {
			kick = std::clamp(kSparkRepeatPart * k / (1.0f - std::exp(-k * period)), kSparkRepeatKickMin,
			                  kSparkRepeatKickMax);
		}
		return seed < 0.5f ? kick : -kick;
	}

	/// **WHEN IT SPLITS, seconds after birth** — for a random (onset) split a fraction of its own life,
	/// linear in the seed between `kSparkSplitAtEarliest` and `kSparkSplitAtLatest`; for a REPEAT spark,
	/// at birth. The shader computes the same (the repeat shader's `u_spark_split_at` is zero).
	[[nodiscard]] static float sparkSplitAt(const Spark& s) noexcept {
		if (s.repeat) {
			return 0.0f;
		}
		return s.life * (kSparkSplitAtEarliest + (kSparkSplitAtLatest - kSparkSplitAtEarliest) * s.seed);
	}
	/// HOW LONG THIS SPARK'S CHILD LOBE TAKES TO GROW — `kSparkRepeatGrow` for a repeat (it must be grown
	/// before the next repeat parts it), `kSparkSplitGrow` otherwise. The shader's `u_spark_split.y`.
	[[nodiscard]] static float sparkGrowOf(const Spark& s) noexcept {
		return s.repeat ? kSparkRepeatGrow : kSparkSplitGrow;
	}

	/// **WHERE THE CHILD IS RELATIVE TO ITS PARENT, AXIS UNITS, `t` seconds after birth** — zero before
	/// the split and for a spark that never splits. Parent and child share the drag, so the child's
	/// drift is `sparkOffsetAt` again, on the KICK and from the split instant:
	///
	///     c(t) = kick · (1 − e^(−k·(t − ts))) / k,   bounded by |kick| / k
	///
	/// and the kick leaves at `kSparkSplitAngle` off the parent's heading, on the side `split`'s sign
	/// names. **The heading is the birth velocity's direction at every age** (drag is isotropic), which
	/// is why no further state is needed.
	[[nodiscard]] static std::pair<float, float> sparkChildOffsetAt(const Spark& s, float t) noexcept {
		if (s.split == 0.0f) {
			return { 0.0f, 0.0f };
		}
		const float ts = sparkSplitAt(s);
		const float tt = std::min(std::max(0.0f, t), s.life);
		if (!(tt > ts)) {
			return { 0.0f, 0.0f };
		}
		const float speed = std::hypot(s.vx, s.vy);
		const float dx = speed > 1.0e-6f ? s.vx / speed : 1.0f;
		const float dy = speed > 1.0e-6f ? s.vy / speed : 0.0f;
		const float side = s.split > 0.0f ? 1.0f : -1.0f;
		const float c = std::cos(kSparkSplitAngle);
		const float sn = std::sin(kSparkSplitAngle) * side;
		// perp = (−dy, dx)
		const float kx = c * dx - sn * dy;
		const float ky = c * dy + sn * dx;
		const float mag = std::abs(s.split);
		return sparkOffsetAt(kx * mag, ky * mag, tt - ts, kSparkDrag);
	}

	/// **HOW GROWN THE CHILD LOBE IS, 0..1**, `t` seconds after birth — a smoothstep over
	/// `sparkGrowOf` from the split. Zero before it and for a spark that never splits. The lobe's
	/// radius is this times `kSparkSplitChildScale` of the parent's round size.
	[[nodiscard]] static float sparkChildGrowthAt(const Spark& s, float t) noexcept {
		if (s.split == 0.0f) {
			return 0.0f;
		}
		const float tt = std::min(std::max(0.0f, t), s.life);
		const float x = std::clamp((tt - sparkSplitAt(s)) / std::max(1.0e-4f, sparkGrowOf(s)), 0.0f, 1.0f);
		return x * x * (3.0f - 2.0f * x);
	}

	/// **WHERE THE CHILD IS AT `now`, AXIS UNITS**, or `false` when there is none yet. The CPU reference
	/// path draws it and both paths record it (`drawnChildrenForTest`).
	[[nodiscard]] static bool sparkChildPointAt(const Spark& s, float now, std::pair<float, float>& out) noexcept {
		const float t = now - s.birth;
		if (sparkChildGrowthAt(s, t) <= 0.0f) {
			return false;
		}
		const auto [px, py] = sparkPointAt(s, now);
		const auto [cx, cy] = sparkChildOffsetAt(s, t);
		out = { px + cx, py + cy };
		return true;
	}

	/// **THE QUAD'S SIDE, PIXELS, for a spark of round size `sizePx` whose child kick is `kickPx` px/s.**
	///
	/// A plain spark's quad is `kSparkStreakMax` of its size — the streak's room, reserved at birth. A
	/// splitter's must ALSO hold the child lobe at its farthest: its centre never gets further than
	/// `|kick|/k` from the parent's (the bound on `sparkChildOffsetAt`), and its radius never exceeds
	/// `kSparkSplitChildScale` of the parent's round radius. The smooth union that draws the neck can
	/// reach past either lobe by at most a quarter of `kSparkSplitNeck` in the lobes' own units, so the
	/// whole is scaled by that. **Undersize this and the lobe CLIPS at the quad's edge** — which a
	/// case asserts cannot happen at any age.
	[[nodiscard]] static float sparkQuadSide(float sizePx, float kickPx) noexcept {
		const float streak = sizePx * kSparkStreakMax;
		if (kickPx == 0.0f) {
			return streak;
		}
		const float half = std::max(0.5f * streak,
		                            std::abs(kickPx) / std::max(1.0e-4f, kSparkDrag)
		                                + 0.5f * sizePx * kSparkSplitChildScale);
		return 2.0f * half * (1.0f + 0.25f * kSparkSplitNeck);
	}

	/// **THE INK A SPARK IS DRAWN IN** — the one mapping from emitter and gate class to colour, which
	/// both paths draw with and a case asserts. Wet, lane, or for a grain onset its DRY gate's hue.
	[[nodiscard]] static uint32_t sparkInk(const Spark& s) noexcept {
		if (s.repeat) {
			return kSparkRepeatInk;
		}
		if (s.onset != 0) {
			return fx::sklitter::grainOnsetClassIsDry(s.onset - 1u) ? kSparkOnsetDryInk : kSparkOnsetInk;
		}
		return s.fromLane ? kSparkLaneInk : kSparkInk;
	}
	/// **THE RING'S HOLE A SPARK IS DRAWN WITH**, 0 = filled — a grain onset whose REVERSE gate was set.
	[[nodiscard]] static float sparkHole(const Spark& s) noexcept {
		return s.onset != 0 && fx::sklitter::grainOnsetClassIsReversed(s.onset - 1u) ? kSparkOnsetReverseHole
		                                                                              : 0.0f;
	}

	/// THE GRAIN ONSET COUNTERS AS THE PAD IS HANDED THEM — `SklitterLevels::onsets`.
	using GrainOnsetCounts = std::array<uint32_t, fx::sklitter::kGrainOnsetClasses>;
	/// **THE DEVICE'S EVENT READINGS, AS ONE PUBLISH CARRIED THEM** — the per-gate-class grain onset
	/// counters, Repeat's restart counter, and the period of the last repeat. Counters, not events, for
	/// `Reading::GrainOnsets`' reason.
	struct SparkEvents {
		GrainOnsetCounts onsets {};
		uint32_t repeats = 0;
		float repeatPeriod = 0.0f;
	};

	/// **ADVANCE THE SPARKS ONE TICK AND ANSWER WHETHER A FRAME WAS ASKED FOR.**
	///
	/// @param wetLevel what came STRAIGHT THROUGH, linear, 0 at silence, and @param grainLevel what
	///        took the grain lane — `Reading::WetLevel`/`GrainLevel` through `sklitterLevels`. Handed
	///        in as a pair, never fetched (class header), because they are only a partition while they
	///        came from one publish.
	///
	/// **TWO EMITTERS, ONE PROCESS EACH, AND THE RATES ARE INDEPENDENT.** Each level drives its own
	/// Poisson draw at the same `kSparkRateMax`, so the two colours' densities stand in the same ratio
	/// as the two levels — which is the only reason the picture says anything. Sharing a rate and
	/// splitting it would make the total constant and the split a hue, which reads as "the sound moved
	/// sideways" rather than "more of it went that way".
	///
	/// **THE RETURN IS THE CLAIM.** `true` means this call requested a repaint; a caller sums it to
	/// find out whether the pad was silent, and a build that stopped calling `redraw()` cannot answer
	/// `true`. The panel ORs it into its own tick answer WITHOUT folding it into its own `redraw()` —
	/// the pad repaints itself, and a panel that repainted its whole subtree for a moving spark would
	/// be paying 60 Hz for the grid, the two fx rails and every card on them.
	///
	/// **AND TWO EVENT EMITTERS (backlog HR round 2) — @param events**, the device's counters
	/// (`SparkEvents`), or null where there are none. The pad differences them against the previous tick
	/// and emits one larger spark per GRAIN fired, in its gate class's ink, and one splitting spark per
	/// REPEAT, paced by the repeat's period. **Counters, not events**, so a tick that came late still sees
	/// every one. Two things it will not do: emit on the FIRST reading (that is a baseline, not a burst of
	/// everything since the device was built), or emit while both levels read zero — an onset of a device
	/// at Mix 0 is a grain nobody hears, and the sparks claim *this instrument is doing that*.
	bool updateSparks(float wetLevel, float grainLevel, const SparkEvents* events = nullptr);

	/// THE LIVE SPARKS, as the last `updateSparks` left them. A VALUE, which is what the emission-rate
	/// and birth-velocity claims are asserted against — `draw` submits a picture and a picture cannot
	/// be read back for a number (CLAUDE.md).
	/// **THE LIVE ONES, which is `sparks_` MINUS ITS RETIRED PREFIX.** Retirement advances an index
	/// rather than erasing (see `head_`), so the vector holds dead sparks between compactions and
	/// handing it over whole would report a population that includes them.
	[[nodiscard]] std::span<const Spark> sparksForTest() const noexcept {
		return { sparks_.data() + head_, sparks_.size() - head_ };
	}
	/// HOW MANY ARE ALIVE — O(1), and the thing the repaint decision is made from.
	[[nodiscard]] size_t liveSparks() const noexcept { return sparks_.size() - head_; }

	/// **WHERE THE LAST `draw` ACTUALLY PUT THEM, in this pad's own pixels, recorded BY the loop that
	/// submits them.** Exactly `starsForTest()`'s shape and for exactly its reason: a repaint cannot be
	/// observed headlessly, so the line that DELIVERS the picture is the line that reports it. Without
	/// this, `drawSparks` could be deleted outright — the simulation would go on being right, every
	/// number above would stay green, and nothing would be on screen.
	[[nodiscard]] const std::vector<std::pair<float, float>>& drawnSparksForTest() const noexcept {
		return drawnSparks_;
	}

	/// **THE NATIVE SIZE THE LAST SPARK WAS SUBMITTED AT — pixels, scale already applied.**
	///
	/// When the claim is about a scale, assert the NATIVE width: the development display is
	/// 1.75×, and "larger than a star" is a claim about what reaches the glass, not about a constant in
	/// this header. Recorded BY the `canvas.fill` that submits it, for the reason `drawnSparksForTest`
	/// gives. Zero until something has been painted.
	[[nodiscard]] float lastSparkNativeSizeForTest() const noexcept { return lastSparkSize_; }

	/// **HOW MANY SPARKS HAVE EVER BEEN BORN — monotone, and the rate claim is made against it.**
	///
	/// **IT EXISTS BECAUSE `sparksForTest().size()` CANNOT ANSWER THE QUESTION.** That is a steady
	/// state: births minus deaths, so a run long enough for the statistics to mean anything is a run in
	/// which most of the evidence has already died, and a run short enough to keep them all gives a
	/// sample of about twenty-five — three sigma on which is a factor of two, which would pass a build
	/// whose rate law was wrong by that much. Counting births lets a case run sixteen seconds and hold
	/// the ratio between two levels to a few percent.
	///
	/// **AND IT IS INCREMENTED WHERE THE SPARK IS PUSHED, not beside the call that draws the count** —
	/// CLAUDE.md's distinction: a number produced BY the thing cannot be had without it, and a counter
	/// standing next to the emission would keep counting for a build that had stopped emitting.
	[[nodiscard]] size_t sparksEmittedForTest() const noexcept { return emitted_; }
	/// The same, for the GRAIN-LANE emitter alone. **Both counts, so a case can assert the RATIO** —
	/// which is what the second colour exists to show, and what a single total cannot see: a build
	/// that emitted the right number of sparks and gave them all one colour passes on the total.
	[[nodiscard]] size_t laneSparksEmittedForTest() const noexcept { return emittedLane_; }
	/// The same, for the ONSET emitter, per gate class. Incremented where the spark is pushed.
	[[nodiscard]] const std::array<size_t, fx::sklitter::kGrainOnsetClasses>& onsetSparksEmittedForTest()
	    const noexcept {
		return emittedOnset_;
	}
	/// The same, for the REPEAT emitter.
	[[nodiscard]] size_t repeatSparksEmittedForTest() const noexcept { return emittedRepeat_; }
	/// HOW MANY OF THE SPARKS EVER BORN WERE BORN SPLITTING — `Spark::split` non-zero.
	[[nodiscard]] size_t splitSparksEmittedForTest() const noexcept { return emittedSplit_; }
	/// **WHERE THE LAST `draw` PUT THE CHILD LOBES, pixels** — one entry per splitter whose lobe has
	/// begun to grow, recorded by the loop that submits them, for `drawnSparksForTest`'s reason.
	[[nodiscard]] const std::vector<std::pair<float, float>>& drawnChildrenForTest() const noexcept {
		return drawnChildren_;
	}
	/// **THE QUAD SIDES THE LAST GPU `draw` SUBMITTED, pixels, in birth order of the drawn sparks** — so
	/// a case can hold every splitter's child lobe inside the rectangle it was actually given.
	[[nodiscard]] const std::vector<float>& drawnQuadSidesForTest() const noexcept { return drawnQuads_; }

	/// **THE PROCESS IS RANDOM, SO A CASE PINS ITS SEED.** Without this a case could assert that SOME
	/// sparks appeared and nothing sharper; with it the same tick sequence emits the same count every
	/// run, and the rate claim becomes an arithmetic one. Default is `kSparkSeed`.
	void setSparkSeedForTest(uint32_t seed) noexcept;
	void setClockForTest(Clock clock);

	/// **WHICH OF THE TWO PATHS DRAWS — and the CPU one is kept as the KNOWN-GOOD REFERENCE rather than
	/// as a fallback for a machine that cannot manage the other.**
	///
	/// Both evaluate `sparkPointAt`; the only difference is WHO. The GPU path submits each spark's birth
	/// data and lets `vs_spark.sc` place it; the CPU path evaluates the same closed form here and
	/// submits an ordinary `canvas.fill` at the answer. **So a capture of one against the other is a
	/// test of the shader and of nothing else** — which is the only test of the shader available, since
	/// a fragment stage cannot be asserted headlessly.
	void setSparkGpuForTest(bool gpu) noexcept { gpu_ = gpu; }
	[[nodiscard]] bool sparkGpuForTest() const noexcept { return gpu_; }

	/// **THE PUCK VELOCITY THE LAST TICK MEASURED, AXIS UNITS PER SECOND** — what a spark emitted on
	/// that tick inherited `kSparkInherit` of. Zero before two ticks have been taken, because one
	/// sample is a position and not a motion.
	[[nodiscard]] std::pair<float, float> puckVelocityForTest() const noexcept {
		return { puckVx_, puckVy_ };
	}

	//----------------------------------------------------------------------------------------------//
	// **THE FEEL CONSTANTS. Every one of these is a taste number, expected to be moved by ear.**      //
	// They are each named and each used exactly once, so moving one moves one thing.                 //
	//----------------------------------------------------------------------------------------------//

	/// SPARKS PER SECOND at full level — the Poisson rate's ceiling. The per-tick draw is
	/// `Poisson(rate·dt)`, so at 60 fps this is about 1.5 sparks a frame at full tilt.
	static constexpr float kSparkRateMax = 400.0f;
	/// **THE CURVE FROM LEVEL TO RATE, AND IT IS NOT 1 FOR A MEASURED REASON.** `OutLevel` is a PEAK,
	/// and a peak sits near full scale for most of anything loud — so a linear map would run at the
	/// ceiling almost all the time and the sparks would carry no information beyond "on". Squaring it
	/// puts half-scale material at a quarter of the rate, which is where the contrast lives.
	static constexpr float kSparkLevelGamma = 2.0f;
	/// HOW MANY MAY BE ALIVE AT ONCE. **A ceiling, never the thing that sets the density** — a cap that
	/// bites would silently turn the Poisson process into a constant and the two colours' ratio into a
	/// tie, which is the picture lying about the one thing it exists to say. A population settles at
	/// `rate × life`, so two emitters at full level settle at `2 × 400 × 0.55 = 440`; this is that with
	/// ~75% headroom, and it exists only so a stalled tick cannot grow the vector without bound on the
	/// message thread.
	///
	/// **AND THE DRAW IS NOWHERE NEAR ITS OWN CEILING.** visage batches to `kMaxQuadsPerBatch` = 16384
	/// quads in one `bgfx::submit`; 1024 sparks plus 110 stars is 1134, about 7% of one batch.
	///
	/// **RAISED FROM 768 BY THE GPU PASS, AND THE REASON IS THE NEW RETIREMENT RULE RATHER THAN MORE
	/// SPARKS ON SCREEN.** Retirement is now by WORST-CASE life, because that is what makes the dead a
	/// prefix and the tick O(1) — so the vector holds a spark for `kSparkLife·(1+kSparkLifeJitter)`
	/// even though the shader stopped drawing it at its own shorter life. MEASURED: the population the
	/// vector carries went 437 → 642 with no visible change at all. 768 was 6σ of headroom on that and
	/// this is comfortable; the cap must never be what limits the density (see above).
	static constexpr size_t kMaxSparks = 1024;
	/// SECONDS A SPARK LASTS, before jitter.
	static constexpr float kSparkLife = 0.55f;
	/// **±40% ON THE LIFETIME.** Without it every spark from one burst dies on the same frame, which
	/// reads as a shutter rather than as a scatter.
	static constexpr float kSparkLifeJitter = 0.4f;
	/// HOW MUCH OF THE PUCK'S VELOCITY A SPARK IS BORN WITH. Below 1 so a spark trails the hand rather
	/// than racing it — at 1 the sparks sit exactly on the puck's future path and read as a smear.
	static constexpr float kSparkInherit = 0.6f;
	/// THE RANDOM KICK ADDED AT BIRTH, axis units per second, uniform in direction and in magnitude up
	/// to this. It is what makes a spark emitted while the hand is STILL go anywhere at all.
	/// **RAISED FROM 0.35 WITH THE FEEL PASS, AND IT IS PART OF THE SAME COMPLAINT.** The stars are
	/// STILL and the sparks MOVE, which is the strongest separation available — but a spark from a
	/// resting puck has only this kick, and at 0.35 radii/s under `kSparkDrag` it travelled about a
	/// tenth of a radius in its whole life. For its first frames it was a star: same size, same family
	/// of colour, and not visibly moving. At 0.8 it leaves the puck immediately.
	static constexpr float kSparkSpread = 0.8f;
	/// EXPONENTIAL DRAG, per second: `v *= exp(-kSparkDrag·dt)`. Nothing else acts on a spark — no
	/// gravity, because there is no down on a disk, and a settling spark reads as an ember.
	static constexpr float kSparkDrag = 2.2f;
	/// **THE CEILING ON THE MEASURED PUCK SPEED, AND IT IS NOT A FEEL NUMBER — IT IS THE ONE THAT SAYS
	/// A JUMP IS NOT A VELOCITY.** `Perf X`/`Perf Y` are modulation destinations and a click lands the
	/// puck anywhere on the disk in one tick, so the difference quotient can be 100 radii a second for
	/// something nobody moved. Clamped, a jump still throws sparks — it IS a move — but it throws them
	/// at the speed of a fast drag rather than off the surface.
	static constexpr float kSparkSpeedMax = 6.0f;
	/// HOW FAR FROM THE CENTRE A SPARK MAY GET BEFORE IT IS RETIRED, in axis units. **1.0 IS THE RIM,
	/// and it is the stars' own aperture** (`drawSky` culls at exactly this) — we have no clip path, so
	/// anything beyond it would paint on the rail outside the circle.
	static constexpr float kSparkReach = 1.0f;
	/// **A SPARK'S SIZE AT 1:1, AT BIRTH AND AT DEATH — it SHRINKS, which is what makes it an ember
	/// rather than a dot.** On the first build the sparks were hard to tell apart from the starfield
	/// background, so they gained more distinct colouring and a larger size.
	///
	/// **THE FLOOR IS THE POINT: `kSparkSizeDeath` IS STILL LARGER THAN `kBrightStar` (1.4).** A spark
	/// that shrank INTO the star range would spend its last frames looking exactly like the field it is
	/// flying over — which is the complaint, arriving late instead of immediately. So the whole ramp
	/// lives above the starfield, and a case asserts it against `kBrightStar` rather than against a
	/// number copied out of it.
	static constexpr float kSparkSizeBirth = 3.4f;
	static constexpr float kSparkSizeDeath = 1.8f;
	/// ITS ALPHA AT BIRTH. It fades to nothing over its life, so this is the top of the ramp. **Stars
	/// run 0.135 to 0.45** (`kStarAlpha` times a magnitude of 0.3–1.0), so a spark is more than twice
	/// the brightest star for most of its life — separation by value as well as by hue and size.
	static constexpr float kSparkAlpha = 0.95f;
	//----------------------------------------------------------------------------------------------//
	// **THE TWO INKS, AND THE FIRST PAIR WAS CHOSEN ON SEMANTICS AND WAS WRONG — MEASURED.**          //
	//----------------------------------------------------------------------------------------------//
	//
	// They were `kAccent` (the waypoint fill) and `kMod` ("something else is acting on this"), each
	// well argued and each, in fact, a PALE COOL BLUE:
	//
	//     kStar    #F2F4FA   (242, 244, 250)   the starfield
	//     kAccent  #7FA8E8   (127, 168, 232)
	//     kMod     #92A4F2   (146, 164, 242)
	//
	// **`kAccent` AND `kMod` DIFFER BY 19, 4 AND 10 PER CHANNEL — 33 OF A POSSIBLE 765.** Two colours
	// that far apart are one colour at 1.6 px and 85% alpha, and both of them sit in the same cool,
	// high-blue family as the stars. The build showed exactly that: sparks hard to tell apart from
	// the starfield, needing more distinct colouring and a larger size.
	//
	// **THE LESSON IS THE ORDER OF THE TEST.** A colour's MEANING is worth arguing about only once its
	// LEGIBILITY is settled; I argued the meaning of two inks I never compared as numbers. The case
	// *"the spark inks are legibly apart — from the starfield and from each other"* now asserts the
	// channel distances, because that is the reading that would have caught this before an eye did.
	//
	// **SO THEY ARE HOT AND SATURATED NOW, AGAINST A COOL PALE FIELD**, and the meaning survives the
	// change rather than being abandoned: an EMBER for the sound that came straight through, and a
	// hotter, stranger MAGENTA for the sound that was diverted down the grain lane and came back
	// altered. Both are from the seed palette, so neither is a hue invented here.
	//
	//     kSeed3   #E58B45   (229, 139,  69)   straight through
	//     kSeed4   #D94176   (217,  65, 118)   took the grain lane
	//
	// Against each other: 12, 74, 49 — **135**, four times the old pair. Against `kStar`: 194 and 348.
	static constexpr uint32_t kSparkInk = tokens::kSeed3;
	static constexpr uint32_t kSparkLaneInk = tokens::kSeed4;
	//
	// **WHITE-HOT-THEN-COLOURED WAS CONSIDERED AND REJECTED, AND THE REASON IS THE WHOLE COMPLAINT.**
	// An ember that starts white and cools to its hue is the physical picture and would separate the
	// sparks from the still stars by BEHAVIOUR. But `kStar` IS white (`#F2F4FA`), so the frame a spark
	// is brightest and largest would be the frame it most exactly resembles a star. The separation is
	// bought instead by size, by alpha and by a saturated hue held from birth.
	/// **HOW ELSE A LANE SPARK DIFFERS FROM A STRAIGHT ONE: NOT AT ALL, AND THAT IS A DECISION.** Nothing
	/// asked for more, and hue-only is the reading that keeps the picture quantitative — if lane sparks
	/// were also bigger or brighter, "more of them" and "more prominent" would be confounded and you
	/// could no longer read *how much* went each way off the density. It is a constant rather than a 1
	/// inline so that moving it is one edit if the lane should read louder.
	static constexpr float kSparkLaneSizeScale = 1.0f;
	//----------------------------------------------------------------------------------------------//
	// **THE SHAPE'S EVOLUTION OVER A LIFE, IN THE SHADER. All four are feel constants and all four    //
	// are uniforms.**                                                                                //
	//----------------------------------------------------------------------------------------------//
	//
	// A round hot point at birth, drawn OUT into a streak along the direction it is flying, thinning as
	// it dies. **AND THE STREAK IS A FUNCTION OF SPEED**, so a spark flung off a hard drag draws out
	// and one that fell off a resting puck stays round — the momentum feature made visible a second
	// way, which is why this is worth having rather than decoration.
	//
	// **THE DIRECTION NEEDS NO CLOSED FORM.** Drag is isotropic (`v' = −k·v`), so it scales the speed
	// and never turns it: the birth velocity's direction IS the current direction at every age.

	/// THE LONGEST A SPARK GETS, in units of its own round size, along its velocity. **The quad is
	/// submitted at this multiple of `kSparkSizeBirth`** — a vertex shader may move a quad but cannot
	/// make it bigger than the rectangle the rasteriser was handed, so the extent is reserved at birth.
	static constexpr float kSparkStreakMax = 2.6f;
	/// HOW FAR THROUGH ITS LIFE it is longest, 0..1. Round at birth, longest here, easing back after.
	static constexpr float kSparkStreakAt = 0.32f;
	/// THE SPEED AT WHICH IT STREAKS FULLY, in axis units per second. Below this the shape stays
	/// rounder in proportion. `kSparkSpread` is 0.8, so an undragged spark reaches about half.
	static constexpr float kSparkStreakSpeed = 1.6f;
	/// THE NARROWEST THE ACROSS-AXIS MAY GET, as a fraction of the round size. **A floor, because a
	/// needle thinner than a pixel stops being drawn at all** — which reads as the spark vanishing
	/// early rather than as a shape, and would put it back in star territory by disappearing.
	static constexpr float kSparkStreakWaist = 0.45f;

	/// **HOW MUCH A SPARK TWINKLES, 0..1 — the "glitter", and it is OFF by default.**
	/// A twinkle is a function of `(seed, age)` computed entirely in `fs_spark.sc`, where the seed is
	/// hashed from the spark's own birth velocity and costs no per-quad float. **Zero so that the GPU
	/// path and the CPU reference render IDENTICALLY** — an A/B photograph is the only evidence we have
	/// that the shader implements the closed form, and a term that exists on one side of it would spend
	/// that evidence. Raise it to see it; it is a feel constant.
	static constexpr float kSparkGlitter = 0.0f;
	/// The twinkle's rate, radians per second, when `kSparkGlitter` is above zero.
	static constexpr float kSparkGlitterRate = 11.0f;

	//----------------------------------------------------------------------------------------------//
	// **GRAIN ONSETS — backlog HR round 2.** Grains fire larger sparks, coloured by the grain's      //
	// sampled gates. All feel constants except the tick bound.                                       //
	//----------------------------------------------------------------------------------------------//

	/// HOW MUCH BIGGER AN ONSET SPARK IS THAN A LEVEL SPARK, at birth and at death. Size is what makes
	/// it read as an EVENT against the level sparks' shower, before its colour is even seen.
	static constexpr float kSparkOnsetSizeScale = 2.2f;
	/// **THE TWO ONSET INKS, ONE PER `useDry`.** HR's decision was to encode `useDry` and `shouldReverse`
	/// and nothing else, because a hue carrying more than a bit or so is a hue nobody can read back. So
	/// the DRY gate is the HUE and the REVERSE gate is the SHAPE (`kSparkOnsetReverseHole`), rather than
	/// four hues over two bits. GREEN for a grain read from the buffer, BLUE for a dry grain (it passes
	/// the input) — each ≥100/765 from the stars, from the level inks and from each other, asserted.
	/// kSeed5 `#5BBF39`, kSeed1 `#4685E8`: 21 + 58 + 175 = 254 apart. (kSeed7, the palette's gold, was the
	/// obvious other warm hue and is only 43 from the wet ember `kSeed3` — under the floor, asserted.)
	static constexpr uint32_t kSparkOnsetInk = tokens::kSeed5;
	static constexpr uint32_t kSparkOnsetDryInk = tokens::kSeed1;
	/// **A REVERSED GRAIN'S SPARK IS A RING**: its middle is cut out to this fraction of its radius, 0 =
	/// filled. Shape instead of a third and fourth hue, for the legibility reason above — "hollow means
	/// backwards" reads without a legend in a way "violet means dry-and-backwards" never would.
	static constexpr float kSparkOnsetReverseHole = 0.5f;
	/// **THE MOST ONSETS ONE TICK THROWS, PER CLASS — a cap, not a feel number.** Grains are at least
	/// ~23 ms long, two voices, so a 60 Hz tick sees one or two; but a UI that is merely SLOW (an
	/// occluded window, a software rasteriser — the xvfb venue ticks at a few Hz) must not lose them, and
	/// one that STALLED for seconds must not dump them all. So a large forward count is capped here,
	/// and only a count that went BACKWARDS (a reset or swapped device, which mod 2^24 reads as a jump
	/// of more than half the range) is discarded — see `emitOnsets`. MEASURED on xvfb: the first
	/// version DROPPED any jump over this, and the venue's slow ticks threw almost no onset sparks.
	static constexpr uint32_t kMaxOnsetsPerTick = 16;

	//----------------------------------------------------------------------------------------------//
	// **THE SPLIT — one spark, one instance, a lobe that grows out of it and drifts off.** All feel.  //
	//----------------------------------------------------------------------------------------------//

	/// **THE SHARE OF GRAIN-ONSET SPARKS THAT SPLIT AT RANDOM, 0..1 — ZERO, AND THAT IS A CHOICE.** Giving
	/// repeat slices the split spark gave the split a MEANING (a repeat); a coin-flip split beside it
	/// would blur that. It was 0.4 before the repeat emitter existed. **It can be turned back on** if
	/// grain onsets budding at random as well is wanted — the machinery is all still here.
	static constexpr float kSparkSplitChance = 0.0f;
	/// **A RANDOM SPLIT'S CHILD KICK, axis units per second, and it SETS THE QUAD.** The child drifts at
	/// most `kick/kSparkDrag` from its parent — 0.2 radii here — and the quad must hold that
	/// (`sparkQuadSide`). Used only while `kSparkSplitChance` is above zero.
	static constexpr float kSparkSplitKick = 0.45f;
	/// THE ANGLE THE CHILD LEAVES AT, radians off the parent's heading, on the side the sign names.
	static constexpr float kSparkSplitAngle = 0.9f;
	/// WHEN IN ITS LIFE A SPARK SPLITS, as fractions — linear in its seed between the two.
	static constexpr float kSparkSplitAtEarliest = 0.18f;
	static constexpr float kSparkSplitAtLatest = 0.42f;
	/// SECONDS FOR THE CHILD LOBE TO GROW from nothing to its full size — the "growing" of the
	/// special, growing shape. Over this the lobe swells out of the parent's side; the smooth union
	/// (`kSparkSplitNeck`) draws the pinch between them until the drift pulls them apart.
	static constexpr float kSparkSplitGrow = 0.12f;
	/// THE CHILD'S FULL RADIUS AS A SHARE OF THE PARENT'S ROUND RADIUS.
	static constexpr float kSparkSplitChildScale = 0.7f;
	/// **THE NECK**: the smooth-minimum width joining the two lobes, in the lobes' own units. Zero is two
	/// separate blobs from the first frame; larger is a longer, softer pinch.
	static constexpr float kSparkSplitNeck = 0.35f;

	//----------------------------------------------------------------------------------------------//
	// **REPEAT SPARKS — repeat slices get the split spark, splitting roughly every repeat.** One per  //
	// restart of Repeat's slice; every one splits, paced by the period.                              //
	//----------------------------------------------------------------------------------------------//

	/// A REPEAT SPARK'S SIZE, × a level spark — the onset sparks' size, since both are events.
	static constexpr float kSparkRepeatSizeScale = 2.2f;
	/// **ITS INK: VIOLET, `kSeed2` `#7241D9`** — the last palette hue that clears the 100/765 floor
	/// against the stars and all four other spark inks (asserted).
	static constexpr uint32_t kSparkRepeatInk = tokens::kSeed2;
	/// **HOW FAR APART THE TWO CENTRES ARE WHEN THE CHILD HAS "PARTED", radii** — about the two lobes'
	/// radii plus the neck at 1:1. The kick is chosen so this distance is reached one repeat period
	/// after birth (`sparkRepeatKick`).
	static constexpr float kSparkRepeatPart = 0.06f;
	/// THE KICK'S CLAMP, radii per second. The MAX sets a repeat spark's quad (`sparkQuadSide`): at 0.9
	/// the child can drift 0.41 radii, a ~140 px quad at 1:1. Repeats faster than ~70 ms hit it and part
	/// a little late. **The MIN is what makes a SLOW repeat's spark split at all**: a spark lives ~0.55 s,
	/// and a kick paced to a one-second repeat would part its child after the spark had died. At 0.25 the
	/// child parts ~0.34 s after birth, so repeats slower than that part EARLY — before the next repeat,
	/// which is the side to err on, since a split nobody sees is no split.
	static constexpr float kSparkRepeatKickMin = 0.25f;
	static constexpr float kSparkRepeatKickMax = 0.9f;
	/// SECONDS FOR A REPEAT SPARK'S CHILD LOBE TO GROW — short, because it splits at birth and a fast
	/// repeat parts it within a few tens of milliseconds.
	static constexpr float kSparkRepeatGrow = 0.05f;

	/// THE DEFAULT SEED. A fixed value rather than a clock, so two pads in one editor do not emit in
	/// lockstep only if they are ticked apart — and so a screenshot is reproducible.
	static constexpr uint32_t kSparkSeed = 0x5EED5A11u;
	/// **THE LONGEST TICK THAT COUNTS AS ONE.** `PerformStage::update`'s own 50 ms clamp, for its
	/// reason: a stalled host hands back a `dt` of seconds, and a spark integrated over it teleports.
	static constexpr double kSparkMaxDt = 0.05;


	/// **THE STAR INK — `--star`, the mock's own name for exactly this.** `theme.h` maps that token to
	/// the POINTER role for the reticle, which is a different job on a different surface, so it is named
	/// here for the job it is doing rather than borrowed through a role that does not fit.
	static constexpr uint32_t kStarInk = tokens::kStar;
	/// A star's alpha is its magnitude through this. `performSky()` magnitudes run 0.3 to 1.0, so the
	/// field runs 0.135 to 0.45 — a haze under the instrument, never competing with the marks.
	static constexpr float kStarAlpha = 0.45f;
	/// `PerformStage::drawSky`'s own two sizes, scaled with the disk like everything else here.
	static constexpr float kBrightStar = 1.4f;
	static constexpr float kFaintStar = 0.9f;
	/// Which magnitude counts as bright — `drawSky`'s number, restated because the two fields should
	/// grade the same way.
	static constexpr float kBrightAbove = 0.75f;

	/// **THE FURNITURE'S SIZES AT 1:1, AND EVERYTHING BELOW SCALES THEM.** They were the whole story
	/// while the disk was a compile-time 310; since the circle grows with the panel (2026-09-20: the
	/// whole perform circle scales as large as the player wants it) a fixed 6px
	/// puck on a 700px disk would be a speck. So these are the values at the REFERENCE diameter and the
	/// accessors beneath are what `draw` and the hit test use.
	static constexpr float kGrabSlack = 6.0f;
	static constexpr float kPuckRadius = 6.0f;
	static constexpr float kMarkRadius = 4.0f;
	/// Room reserved at the rim for the marks, so the disk `axesAt` maps onto is the PLAYABLE area.
	static constexpr float kRimInset = kMarkRadius + 2.0f;
	/// The ring's stroke, and the centre dot's radius.
	static constexpr float kRingWeight = 1.5f;
	static constexpr float kCentreRadius = 1.5f;

	/// **THE DIAMETER THOSE NUMBERS ARE 1:1 AT — `CircleRail::kDisk`, THE SMALLEST DISK THAT RAIL EVER
	/// LAYS OUT.** It cannot be spelled as that constant here: `sklitter_expanded.h` includes this
	/// header, so the dependency only runs one way. It is stated instead and PINNED BY A CASE — *"the
	/// pad's 1:1 size is the smallest disk its rail lays out"* — which is `kCollapsedLabelRun`'s
	/// arrangement one widget over: a number that must agree with another file's, checked rather than
	/// shared. The consequence of the floor is that `scale()` is never below 1, so a pad at its
	/// minimum draws exactly what it drew before any of this.
	///
	/// **WRITTEN AS THAT CONSTANT'S OWN ARITHMETIC RATHER THAN AS `310`, AND THE 0.44 IS NOT PEDANTRY.**
	/// The first draft said `310.0f`, which is `kDisk` rounded — and rounding it put the scale at the
	/// minimum panel at 1.0014 instead of 1, which moved the grab slack by 8 thousandths of a pixel and
	/// turned the existing corner-clearance case RED (`161.22 == Approx(161.212)`). A reference that is
	/// nearly the floor is not a floor. The three terms mirror `CircleRail`'s: a cell's inner corner at
	/// 114 on each axis, √2 to eight figures because `expandedWidth()` has to be a compile-time sum, and
	/// `kCornerClear`'s 6.
	///
	/// **DO NOT SIMPLIFY THIS BACK TO `310.0f`.** It is the shorter, more obvious line and it is the
	/// broken one; the two constants look identical on the page and differ by 0.44, which is exactly the
	/// kind of difference a tidy-up does not think to check. The red it produces names a pixel count and
	/// not this constant, so the trail back here is not short. It was tried; it failed; the case that
	/// pins it is *"the pad's 1:1 furniture size is the smallest disk its rail ever lays out"*.
	static constexpr float kReferenceDiameter = 2.0f * (114.0f * 1.41421356f - 6.0f);

	/// **HOW MUCH BIGGER THAN 1:1 THIS PAD IS**, from its own laid-out size — the one number every piece
	/// of furniture is multiplied by, so they cannot drift apart at size. Never below 1: the rail floors
	/// the disk at the reference, and a pad laid out at nothing draws nothing rather than shrinking.
	[[nodiscard]] float scale() const;

	/// The furniture at THIS pad's size. `rimInset()` and `grabSlack()` are deliberately the SAME
	/// number: `CircleRail::kCornerClear` clears the grab reach, and that reach is exactly half the
	/// pad's frame only while those two agree. Scaling one without the other would put a cell inside the
	/// gesture at every size but the reference.
	[[nodiscard]] float markRadius() const { return kMarkRadius * scale(); }
	[[nodiscard]] float puckRadius() const { return kPuckRadius * scale(); }
	[[nodiscard]] float rimInset() const { return kRimInset * scale(); }
	[[nodiscard]] float grabSlack() const { return kGrabSlack * scale(); }

private:
	/// The sanitised axes and the weights, read from the two controls at the instant they are asked for.
	[[nodiscard]] fx::sklitter::PerformWeights read() const;
	/// One gesture's worth of writing — both axes, always both. A drag that wrote one would move the
	/// puck along a line nobody dragged along.
	void writeAxes(float ax, float ay);
	/// Open and close BOTH controls' gestures, inside one history group. Every write goes between.
	/// `amends` is the step this move continues (`EditHistory::beginGroup`), 0 for a fresh one.
	void beginMove(uint64_t amends);
	void endMove();

	/// **PAINTS THE FIELD AND RECORDS WHAT IT PAINTED.** The record is filled here, by the loop that
	/// submits the stars, so a build that stopped drawing them has nothing to answer `starsForTest()`
	/// with — the shape CLAUDE.md calls for where a repaint cannot be observed headlessly.
	void drawSky(visage::Canvas& canvas, const perform_pad::Disk& d, float px, float py);
	/// **PAINTS THE SPARKS, AFTER THE SKY AND BEFORE THE FURNITURE.** Over the field because they are
	/// the brighter thing; under the ring, the marks and the puck because those are the instrument and
	/// a spark crossing the puck should pass behind it rather than smear it.
	void drawSparks(visage::Canvas& canvas, const perform_pad::Disk& d);

	/// ONE DRAW FROM THE PAD'S OWN STREAM, `[0, 1)`. xorshift32 — a `std::mt19937` is 2.5 KB of state
	/// per pad for a stream nobody has to be able to reproduce across machines, only across runs.
	[[nodiscard]] float nextUnit() noexcept;
	/// HOW MANY EVENTS A POISSON PROCESS OF MEAN `mean` PRODUCED. Knuth's method, which is exact and
	/// costs `mean + 1` draws on average — at 60 fps the mean is about 1.5, so this is two draws.
	[[nodiscard]] size_t poissonDraw(float mean) noexcept;
	/// **ONE EMITTER'S TICK — called once per colour, with that colour's level.** The birth arithmetic
	/// lives here and only here, so the two populations cannot differ in anything but the level that
	/// drives them and the flag they carry.
	void emitFrom(float level, double dt, float nx, float ny, bool fromLane);
	/// **ONE SPARK'S BIRTH — the arithmetic every emitter shares** (angle, kick, life, seed, in that
	/// order, which is the random stream's meaning). Pushes it if the cap allows and counts it; answers
	/// whether it was pushed. `onset` is `Spark::onset`.
	/// A REPEAT spark is `repeat` with `repeatPeriod` pacing its split; `onset` is then 0.
	bool spawn(float nx, float ny, bool fromLane, uint8_t onset, bool repeat = false, float repeatPeriod = 0.0f);
	/// **THE EVENT EMITTERS' TICK** — differences the counters (`eventsSince`), and emits only while
	/// `sounding`. See `updateSparks`.
	void emitEvents(const SparkEvents& events, float nx, float ny, bool sounding);
	/// **HOW MANY EVENTS A COUNTER SAYS HAPPENED SINCE `last`** — mod `kGrainOnsetWrap`; a count that went
	/// BACKWARDS (more than half the range forward) is a reset and answers 0; the rest is capped at
	/// `kMaxOnsetsPerTick`.
	[[nodiscard]] static uint32_t eventsSince(uint32_t now, uint32_t last) noexcept;
	/// WHICH SHADER OBJECT A SPARK IS DRAWN WITH: 0 wet, 1 lane, 2 + gate class for an onset, 6 repeat.
	[[nodiscard]] static size_t inkOf(const Spark& s) noexcept {
		if (s.repeat) {
			return 2u + fx::sklitter::kGrainOnsetClasses;
		}
		return s.onset != 0 ? 2u + static_cast<size_t>(s.onset - 1u) : (s.fromLane ? 1u : 0u);
	}

	device::Modulatable& x_;
	device::Modulatable& y_;
	EditHistory* history_ = nullptr;
	/// The history's serial for the step this pad's last press made, 0 if it made none. What a
	/// double-click's centring asks to amend.
	uint64_t lastMove_ = 0;
	bool held_ = false;
	/// Cleared and refilled by every `draw`. A `vector` rather than a fixed array because the COUNT is
	/// part of the picture: a star the puck has pushed past the rim is not drawn, so how many survive is
	/// itself a function of where the puck is.
	std::vector<DrawnStar> drawnStars_;

	/// THE LIVE SPARKS. `reserve`d to `kMaxSparks` at construction, so the steady state never
	/// reallocates — this is the message thread, not the audio thread, but a per-frame allocation on
	/// the UI thread is still a per-frame allocation.
	std::vector<Spark> sparks_;
	/// Cleared and refilled by every `draw`. See `drawnSparksForTest()`.
	std::vector<std::pair<float, float>> drawnSparks_;
	/// See `lastSparkNativeSizeForTest()`.
	float lastSparkSize_ = 0.0f;
	/// See `sparksEmittedForTest()`. Written only by the `push_back` that makes a spark.
	size_t emitted_ = 0;
	size_t emittedLane_ = 0;
	std::array<size_t, fx::sklitter::kGrainOnsetClasses> emittedOnset_ {};
	size_t emittedSplit_ = 0;
	/// **THE ONSET COUNTERS AS THE LAST TICK SAW THEM** — what the next one differences against. Not
	/// valid until `haveOnsets_`: the first reading is a baseline.
	GrainOnsetCounts lastOnsets_ {};
	uint32_t lastRepeats_ = 0;
	bool haveOnsets_ = false;
	size_t emittedRepeat_ = 0;
	/// See `drawnChildrenForTest()` / `drawnQuadSidesForTest()`. Cleared and refilled by every `draw`.
	std::vector<std::pair<float, float>> drawnChildren_;
	std::vector<float> drawnQuads_;
	/// **THE DRAWN SPARKS' INDICES, in birth order** — the aperture pass writes it, the per-ink submit
	/// passes read it. `reserve`d to `kMaxSparks`, so a draw never allocates.
	std::vector<size_t> visible_;
	uint32_t rng_ = kSparkSeed;
	Clock clock_;

	/// **THE PUCK AS THE LAST TICK SAW IT, AND THE ONLY THING THE VELOCITY IS EVER COMPUTED FROM.**
	///
	/// **CONTEMPORANEITY, CLAUDE.md's RULE, POINTED AT A DERIVATIVE.** A velocity is a cross-time
	/// quantity by definition, so the rule cannot be "never compare across time"; it is that the two
	/// halves of the comparison must have been taken by ONE road at two known instants. So the position
	/// and the timestamp below are sampled together, in `updateSparks` and nowhere else, and the
	/// velocity is `(p_now − p_prev)` over the gap between those same two samplings. **What would be
	/// fatal is mixing roads**: a position remembered here against one `draw` read a moment later is a
	/// difference over an unknown interval, divided by a `dt` measured between two different events —
	/// and it would be wrong only while the two roads ran at different rates, which is exactly when
	/// anyone is looking. `draw` therefore never touches these.
	float puckX_ = 0.0f;
	float puckY_ = 0.0f;
	/// **ONE SAMPLE IS A POSITION, NOT A MOTION.** Until a second tick has been taken there is no
	/// interval to divide by, and a first tick that assumed the puck came from the centre would fling
	/// a burst of sparks every time the panel opened.
	bool havePuck_ = false;
	int64_t lastTickMs_ = 0;
	float puckVx_ = 0.0f;
	float puckVy_ = 0.0f;

	/// **THE PAD'S OWN EPOCH, so `Spark::birth` is a small number.** The clock is milliseconds since
	/// something far away; a `float` holding it directly would have tens of milliseconds of quantum by
	/// the time it reached the shader, which is most of a frame. Taken at the first tick.
	int64_t epochMs_ = 0;
	bool haveEpoch_ = false;
	/// Seconds since `epochMs_`, as of the last tick — what `draw` stamps into the shader's `u_spark_now`
	/// so that the picture and the retirement agree about what time it is.
	float nowSeconds_ = 0.0f;

	/// **WHERE THE LIVE SPARKS START IN `sparks_`.** Retirement is by worst-case life and sparks are
	/// appended in birth order, so the dead are always a PREFIX and retiring them is advancing this —
	/// no per-spark test, no erase, no reordering. The vector is compacted only when the dead half
	/// outgrows the live one, which is a memmove every `kMaxSparks/2` births rather than every tick.
	size_t head_ = 0;

	bool gpu_ = true;
	/// **TWO `visage::Shader` OBJECTS OVER ONE COMPILED PROGRAM, and the reason is the batch id.** A
	/// `Shader*` IS the batch id (`ShaderWrapper`), and `submitShader` reads the uniforms once off the
	/// batch's first shape — so one object per ink is what lets the two colours be two uniform values
	/// instead of two programs or a texture lookup. `ProgramCache` keys on the files, so the program is
	/// compiled once and shared.
	visage::Shader sparkShader_;
	visage::Shader sparkLaneShader_;
	/// **AND ONE PER ONSET GATE CLASS (backlog HR round 2)** — ink, size and ring are uniforms, so each
	/// class is its own shader object over the same program, for the reason above.
	std::array<visage::Shader, fx::sklitter::kGrainOnsetClasses> onsetShaders_;
	/// AND ONE FOR REPEAT SPARKS, whose split timing uniforms differ (they split at birth).
	visage::Shader repeatShader_;
};

} // namespace feathers::ui
