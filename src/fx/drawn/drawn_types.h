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

#pragma once

#include "device/registry.h"
#include "fx/fx_tenant.h"

#include <array>
#include <cstddef>
#include <optional>

// THE DRAWN-CURVE FILTER — PHASE 1: the types and the blend, and nothing that renders a sample.
//
// Four frames, each a drawn filter response; an XY puck blends all four. This header is the design's
// phase 1 exactly: `Band`, `Frame`, `Settings`,
// `zeroPlacementFor` and `blend`, plus the closed-form magnitude the assertions need. There is no
// descriptor, no registry entry, no `AudioOps` and no card — §7 forbids all four here.
//
// WHY POLES AND ZEROS RATHER THAN (type, frequency, Q) — a band's type is independent
// across frames. If band 1 is a Notch at one corner and an LPF at another there is nothing halfway
// between the two TYPES, so a representation carrying a type cannot be blended at all. Pole and zero
// POSITIONS blend continuously and a type becomes what it always was in Serum's PZ SVF: a right-click
// preset that writes where the zero goes. `Free` is not a special mode — it is the OTHER types that
// are constrained.
//
// AND A CORRECTION TO THE DESIGN NOTE, MEASURED RATHER THAN ARGUED — this matters because it is the
// reason the plan gives for the representation, and half of it is not true:
//
//   §3b: "blending biquad COEFFICIENTS ... can leave the stable region in the middle of a morph
//   between two perfectly stable filters."
//
// FOR ONE BAND'S DENOMINATOR THAT IS FALSE, and provably so. A monic real quadratic z² + a1 z + a2
// has both roots inside the unit disc iff |a2| < 1 and |a1| < 1 + a2 — three half-planes, so the
// stable set in (a1, a2) is a TRIANGLE and triangles are convex. A convex combination of stable
// denominators is therefore stable, exactly like the polar blend.
//
// MEASURED. Every number below comes from a generator and seed tests/test_fx_drawn.cpp also uses,
// so the suite recomputes them on each run rather than trusting this comment:
// 484,000 coefficient blends of four randomised stable corners over an 11x11 puck grid gave ZERO
// unstable, worst |pole| 0.999882. (A wider offline sweep with a different angle distribution —
// 24,200,000 blends — also gave zero.) The polar blend the device ships gave zero over 1,764,000
// blends on a 21x21 grid, worst radius 0.999891, which is the same answer.
//
// IT IS TRUE OF THE CASCADE, and dramatically so. Expand the six bands into ONE polynomial and blend
// THAT, and the Schur region for degree >= 3 is not convex any more. Same generator, its own seed,
// same 11x11 grid: 48,400 blends, 16,154 of them UNSTABLE — 33.4%, against zero for corners blended
// per band. Every corner is Schur-checked first, so the instability is the blend's and not the
// generator's. (The offline sweep puts the trend at 7.7% for two bands, 19.1% for three and 49.2% for six.)
//
// So the stability argument survives, but it is about the CHAIN and not about a band. What the
// pole-zero representation actually buys per band is what §3b's FIRST argument says: a type that
// blends at all, plus the two global transforms of §3c (Cutoff scales angles, Res scales radii),
// which are one multiply each in polar and have no closed form in coefficient space.
//
// tests/test_fx_drawn.cpp holds both measurements as cases, so nobody re-derives the wrong half.

namespace feathers::fx::drawn {

/// HOW MANY BANDS, FIXED — plan §5.2. Six is what the field budget reached: `16 * 6 + 10 = 106` of
/// the 128 fields §5.1 raised `kMaxDeviceFields` to in phase 2 (since 127 with the stored type, and 129 of 160
/// with the slot's Mix and wet Gain, backlog NR 2026-10-28).
inline constexpr size_t kBands = 6;

/// FOUR FRAMES, one per corner of the puck. Indexed by `Corner` below, and the order is the blend's
/// contract rather than a detail: `frame[Corner::X0Y1]` IS the top-left corner.
inline constexpr size_t kFrames = 4;

/// THE FOUR CORNERS, NAMED. A bare `0..3` invites exactly the off-by-one the corner-exactness case
/// exists to catch, and the names make the bilinear weights below readable as geometry.
enum Corner : size_t {
	X0Y0 = 0,
	X1Y0 = 1,
	X0Y1 = 2,
	X1Y1 = 3,
};

/// WHAT THE FOUR FRAMES ARE CALLED, AND IT IS ONE TABLE BECAUSE TWO WOULD DRIFT.
///
/// The device has two surfaces that name frames and until now only one of them did: the expanded
/// editor's tabs spell *"Frame 1"*..*"Frame 4"*, and the card's morph plane — whose four CORNERS are
/// those same four frames — said nothing at all, so a hand dragging the puck could not tell what it
/// was morphing between. we should float the waypoint id near the waypoint so we can be
/// cognicent of whats going to morph into what.
///
/// **THE NUMERAL IS THE NAME AND THE PREFIX IS EACH SURFACE'S OWN.** The panel has room for
/// `"Frame " + numeral`; the card's corner has room for the numeral and a letter. Both compose from
/// THIS array, so the two surfaces cannot end up calling one frame by two names — which would defeat
/// the whole point of labelling the corners.
///
/// INDEXED BY `Corner`, like everything else here: `kFrameNumerals[X0Y1]` belongs to the top-left
/// corner of the card's plane, and to the third tab of the editor.
inline constexpr std::array<const char*, kFrames> kFrameNumerals { "1", "2", "3", "4" };

//------------------------------------------------------------------------------------------------
// THE COORDINATE, and it is the one thing in this header worth reading twice.
//------------------------------------------------------------------------------------------------

/// THE FLOOR AND THE CEILING OF THE FREQUENCY COORDINATE, in the log units `Point::angleLog2` uses.
/// Twelve octaves below Nyquist is below the bottom of hearing at every sample rate we run at
/// (5.9 Hz at 48 kHz, 5.4 Hz at 44.1 kHz).
inline constexpr float kMinAngleLog2 = -12.0f;
inline constexpr float kMaxAngleLog2 = 0.0f;

/// THE POLE RADIUS BOUND, AND ITS MARGIN IS DELIBERATE. Convexity says a blended radius is at most
/// the largest corner radius, so `< 1` at every corner gives `< 1` everywhere — but the four
/// bilinear weights sum to 1 only to within a couple of ulp in float, so the blend can overshoot the
/// largest corner by a relative ~1e-7. A ceiling of 0.9999 leaves 1e-4 of margin against a 1e-7
/// error, which is three orders of headroom rather than a coincidence.
inline constexpr float kMaxPoleRadius = 0.9999f;

/// THE ZERO RADIUS BOUND. An `Allpass` mirrors its zero to 1/r, so a pole at r = 0.125 wants a zero
/// at 8. Poles closer to the origin than that stop being allpass — said out loud rather than
/// silently clamped, because a clamped allpass is no longer flat.
inline constexpr float kMaxZeroRadius = 8.0f;

/// WHERE AN UNCONFIGURED BAND SITS: five octaves below Nyquist, 750 Hz at 48 kHz. Nothing depends on
/// the number — a default `Band` has its zero on its pole, so it is flat wherever it is.
inline constexpr float kDefaultAngleLog2 = -5.0f;

/// A CONJUGATE PAIR OF POINTS IN THE Z PLANE, stored POLAR — plan §5.2, *"poles blend in POLAR,
/// frequency in LOG"*. Radius is Q, angle is frequency, so the two stay separable and the global
/// Cutoff and Res of §3c are a single multiply each.
///
/// `angleLog2` IS THE LOG ALREADY, and storing it that way rather than storing radians is
/// load-bearing rather than a convenience — it is the same reason `waypoint::Waypoint` stores
/// `cutoffLog2Hz`. A LINEAR blend of a stored LOG **is** the log blend the plan asked for, so:
///
///   - the morph between two corners an octave apart is even in OCTAVES, which is the musical one;
///   - the blend is a plain lerp on four floats, so a corner comes back EXACTLY (no exp/log round
///     trip, which would cost a ulp and make the corner-exactness case untestable);
///   - and the convexity argument that gives stability applies to the stored field directly.
///
/// THE UNITS: `nu = exp2(angleLog2)` is frequency normalised so that **1 is Nyquist**, and the angle
/// in radians is `pi * nu`. So `angleLog2 == 0` is Nyquist, `-1` is an octave below it, `-10` is ten
/// octaves below. Nothing here knows the sample rate, which is why the coordinate is normalised.
///
/// AND THE FLOOR IS A REAL LIMIT, NOT A CLAMP FOR TIDINESS: `log2(0)` is negative infinity, so **DC
/// is not representable** and `kMinAngleLog2` is as close as this coordinate reaches. It costs one
/// thing and it is worth naming: a `Highpass`'s zeros belong exactly at z = 1, and land instead at
/// Nyquist/4096 (5.9 Hz at 48 kHz), so a highpass has a very small finite gain at DC rather than a
/// true null. That is inaudible and it is the price of the musical blend; the alternative — storing
/// the angle linearly — reaches DC exactly and makes every morph even in HERTZ, which §5.2 rejected
/// by ear. Flagged rather than buried.
struct Point {
	/// |z|. For a POLE this is bounded by `kMaxPoleRadius` and that bound is what makes the filter
	/// stable. For a ZERO it is unbounded above (an allpass's zero is at 1/r, OUTSIDE the circle)
	/// and may be NEGATIVE — see the split-real note on `Bandpass` below.
	float radius = 0.0f;
	/// log2 of normalised frequency; 0 is Nyquist. See the paragraphs above.
	float angleLog2 = kDefaultAngleLog2;

	friend bool operator==(const Point&, const Point&) = default;
};

/// THE ANGLE IN RADIANS, `pi * exp2(angleLog2)`, with the log clamped to the representable range
/// first. The only place the log coordinate is unwound, so it is the only place the floor bites.
float angleRadians(const Point& point);

/// A BAND IS ONE BIQUAD SECTION: a conjugate pole pair and a zero pair. Four floats, which is the
/// budget plan §5.1 sized the device against. A default-constructed one is FLAT — its zero sits on
/// its pole, which is `Bypass`.
/// WHAT A BAND IS, AND IT IS **STORED STATE** SINCE 2026-08-29 — types should stick, free
/// releases the constraint.
///
/// THE COMMENT THAT STOOD HERE SAID THE OPPOSITE and cited §3b for it: *"This is not stored in
/// `Settings` and must not become stored: choosing a type is an EDIT that writes `Band::zero`, and
/// the moment it were state the blend would be back to interpolating a discrete thing."* **§3b does
/// not say that.** What §3b says is *"the type is only where that corner started"* — which is a
/// description of a stored annotation, not an argument against one. The inference is true only if
/// the BLEND consumes the type, and it does not: `realisedAt` blends `pole` and `zero` and has no
/// access to this field. §3b's argument is about what is INTERPOLATED, and nothing here is.
///
/// **THE CASE THAT HOLDS THAT LINE IS `two frames whose bands differ only by TYPE sound identical`**
/// — bit-identical realised cascades at every puck position. If anyone ever wires the type into the
/// blend, that goes red. It is the assertion §3b always wanted and could not have, because until now
/// there was no type to wire in.
enum class Type : int {

	/// Both zeros at Nyquist (z = -1).
	Lowpass,
	/// Both zeros at DC (z = +1) — as near as the log coordinate reaches; see `Point`.
	Highpass,
	/// One zero at DC and one at Nyquist, which is the ONE placement a conjugate pair cannot hold:
	/// `{+1, -1}` is a SPLIT REAL pair. It is reachable in these same four floats by letting the zero
	/// radius go NEGATIVE — see `coefficientsOf()` in the .cpp for the two-branch rule and why the sign
	/// is the only place a fifth float was avoidable. **Poles never take the negative branch**, and
	/// that asymmetry is the physics: stability constrains poles and nothing constrains zeros.
	Bandpass,
	/// Zeros on the unit circle at the pole's own angle: a true null that TRACKS the pole, because
	/// pole and zero angles blend through the identical arithmetic.
	Notch,
	/// Zeros at the pole's angle, radius pulled off the circle by the gain — see `kBellDefaultGain`.
	Bell,
	/// Zeros at the mirror of the pole, 1/r at the same angle. Magnitude is constant (1/r², since
	/// nothing here is gain-normalised — see `magnitudeAt`).
	Allpass,
	/// THE ZERO ON TOP OF THE POLE: they cancel and the section is flat. A type rather than a
	/// checkbox because it is an ordinary point in this space and therefore blends continuously with
	/// everything else — §3b calls that evidence for the representation, and the test that four
	/// DIFFERENT bypassed corners stay exactly flat at every puck position is where it is cashed.
	Bypass,
	/// NO PLACEMENT RULE. The zero is the user's second draggable point, so `zeroPlacementFor`
	/// returns nothing for it — the absence IS the meaning, and an `optional` says so where a
	/// returned copy of the current zero would look like a rule.
	Free,

	kCount,
};


struct Band {
	Point pole{};
	Point zero{};

	/// WHICH PRESET THIS CORNER STARTED AS, and what its zero is still bound to. Stored, per band,
	/// per frame — type is independent across frames is why the last of those is not a
	/// saving to be found later.
	///
	/// DEFAULTED TO `Bypass` BECAUSE THE DEFAULT FRAME IS FLAT: `kDefaultFrames` puts every zero
	/// exactly on its pole, which IS Bypass, and the shipped gesture already keeps it there when the
	/// pole is dragged. So the default reproduces today's behaviour rather than changing it — a
	/// default chosen because it is right, which is the one thing the compatibility relaxation does
	/// not relax.
	Type type = Type::Bypass;

	friend bool operator==(const Band&, const Band&) = default;
};

/// ONE DRAWN RESPONSE: the six bands in series. A frame is what one corner of the puck holds.
struct Frame {
	std::array<Band, kBands> band{};

	friend bool operator==(const Frame&, const Frame&) = default;
};

//------------------------------------------------------------------------------------------------
// THE TYPES — presets that write a zero, NOT state
//------------------------------------------------------------------------------------------------

/// THE TYPE MENU, IN MENU ORDER, and the array is index-aligned with `Type` so a menu can be built
/// from it without a second table to keep in step.
///
/// **`Free` IS IN IT NOW, AND IT IS THE ENTRY THE REST OF THE MENU NEEDS.** It used to be absent, on
/// the reasoning that it *"would write nothing and change nothing"* — true while nothing was stored,
/// because a band was then free exactly when its zero sat somewhere no preset would have put it. It
/// is false the moment the type sticks: every other entry now BINDS the zero to the pole, so `Free`
/// is the only way to unbind it, and it changes the one thing that is now state. free
/// releases the constraint.
///
/// A menu that could enter six constraints and leave none of them would be the control that lies —
/// the same objection the old comment raised, pointing the other way.
inline constexpr const char* const kTypeNames[] {
    "Lowpass", "Highpass", "Bandpass", "Notch", "Bell", "Allpass", "Bypass", "Free",
};
static_assert(std::size(kTypeNames) == static_cast<size_t>(Type::kCount),
              "the menu covers EVERY type — a type a hand cannot pick is a type it cannot leave");

/// THE BELL PRESET'S STARTING GAIN, LINEAR. A peaking section's gain at its own corner is
/// approximately `(1 - rz) / (1 - rp)`, so a zero FURTHER from the circle than the pole is a boost:
/// `rz = 1 - gain * (1 - rp)`. 2.0 is +6 dB, which is a bell you can hear the moment you pick the
/// type; phase 4 gives the gesture that changes it.
inline constexpr float kBellDefaultGain = 2.0f;

/// WHERE THE TYPE PUTS THE ZERO, given the pole it belongs to. Most placements are RELATIVE to the
/// pole (Notch, Bell, Allpass and Bypass all sit at the pole's angle), which is why this takes one —
/// a `zeroPlacementFor(type)` with no pole could only answer for Lowpass, Highpass and Bandpass.
///
/// Returns nothing for `Free`: see the enum.
std::optional<Point> zeroPlacementFor(Type type, const Point& pole, float bellGain = kBellDefaultGain);

//------------------------------------------------------------------------------------------------
// THE FOUR DEFAULT FRAMES
//------------------------------------------------------------------------------------------------

/// THE FOUR CORNERS A FRESH DEVICE HOLDS, AND THEY ARE FLAT.
///
/// **A DEVICE YOU ADD TO A CHAIN MUST NOT CHANGE YOUR SOUND** — the safety property, and it is the
/// one that wins here. lets make the default flat.
///
/// THIS TABLE HELD FOUR AUDIBLY DIFFERENT FILTERS FOR ONE DAY, and that was a deliberate, correct
/// decision for the job it had: registration had just moved forward a phase so the device could be
/// AUDITIONED, and blending four identical identities is the identity — MEASURED on the flat table,
/// the puck at 25 positions, Cutoff across ±4 octaves and Res across its whole range all produced
/// **0.000000 dB** of deviation from flat, so there was nothing to hear and the concept would have
/// been judged on a default patch that could not express it. **That job is done.** The author has heard
/// the device and approved the concept, so the demonstration has no one left to convince and the
/// safety property gets its default back.
///
/// SO: EVERY BAND IS `Bypass` — the zero exactly on the pole — in every frame, which makes the
/// device flat BY CONSTRUCTION rather than by cancellation:
///
///   - at every puck position, because a bilinear blend of four identical frames is that frame, and
///     because a blend of coincident pole/zero pairs is itself coincident whatever the weights are;
///   - at every `Cutoff`, because Cutoff is an ADDITION in the log coordinate applied to pole and
///     zero alike, so a coincident pair stays coincident;
///   - at every `Res`, because Res is the magnitude warp `r -> m + (1-m)(1-k)` applied to pole and
///     zero alike — which is exactly the uniformity rule §3c had to learn the hard way, when a
///     poles-only Res measured **148.2 dB** of resonance on a patch with nothing drawn on it.
///
/// **`enabled` IS STILL FALSE, and that is a SECOND guard rather than this one.** A flat default and
/// an off default protect the same property by different means, and the reason to hold both is that
/// the moment somebody switches a fresh device on it should still be silent — an off switch alone
/// makes "add it and turn it on" a change of sound.
///
/// THE SIX BANDS ARE SPREAD ACROSS THE SPECTRUM rather than stacked — 2^-11 .. 2^-1 of Nyquist,
/// which is 11.7 Hz .. 12 kHz at 48 kHz — and that is load-bearing for the phase-4 editor, not
/// decoration: it draws a handle per band on a frequency-against-radius plane, and two bands with
/// identical numbers draw on top of each other, leaving one of the six **unreachable by a hand**.
/// Nothing about flatness depends on where they sit; a bypassed band is flat wherever it is.
///
/// ONE TABLE, READ TWICE. `kParamDescs` declares its 96 band defaults FROM this array, and
/// `Settings` is constructed from it, so the two cannot drift. What that does NOT make safe is the
/// INDEXING — the descriptor macro could still pair frame 2's numbers with frame 3's row slots — so
/// the case "a row of declared defaults is a default device" is still doing real work.
///
/// **AND NO CASE IN THIS REPO LEANS ON THIS TABLE'S SOUND.** Three did once, silently, and the
/// change that made these frames audible repointed all three at a bypassed `Settings` they build
/// themselves — including the `Drive` case, which had borrowed the default cascade as something to
/// observe a waveshaper through and which nobody predicted. Those stay repointed: a test that leans
/// on a default is a test that changes meaning when the default does, and this table has now moved
/// twice.
inline constexpr std::array<Frame, kFrames> kDefaultFrames {
	// FOUR IDENTICAL FLAT FRAMES. Identical is the point — the puck has nothing to morph between
	// until a hand draws something, which is what "adding the device changes nothing" means at every
	// position rather than only at the corners.
	Frame { { Band { Point { 0.5f, -11.0f }, Point { 0.5f, -11.0f } },
	          Band { Point { 0.5f, -9.0f }, Point { 0.5f, -9.0f } },
	          Band { Point { 0.5f, -7.0f }, Point { 0.5f, -7.0f } },
	          Band { Point { 0.5f, -5.0f }, Point { 0.5f, -5.0f } },
	          Band { Point { 0.5f, -3.0f }, Point { 0.5f, -3.0f } },
	          Band { Point { 0.5f, -1.0f }, Point { 0.5f, -1.0f } } } },
	Frame { { Band { Point { 0.5f, -11.0f }, Point { 0.5f, -11.0f } },
	          Band { Point { 0.5f, -9.0f }, Point { 0.5f, -9.0f } },
	          Band { Point { 0.5f, -7.0f }, Point { 0.5f, -7.0f } },
	          Band { Point { 0.5f, -5.0f }, Point { 0.5f, -5.0f } },
	          Band { Point { 0.5f, -3.0f }, Point { 0.5f, -3.0f } },
	          Band { Point { 0.5f, -1.0f }, Point { 0.5f, -1.0f } } } },
	Frame { { Band { Point { 0.5f, -11.0f }, Point { 0.5f, -11.0f } },
	          Band { Point { 0.5f, -9.0f }, Point { 0.5f, -9.0f } },
	          Band { Point { 0.5f, -7.0f }, Point { 0.5f, -7.0f } },
	          Band { Point { 0.5f, -5.0f }, Point { 0.5f, -5.0f } },
	          Band { Point { 0.5f, -3.0f }, Point { 0.5f, -3.0f } },
	          Band { Point { 0.5f, -1.0f }, Point { 0.5f, -1.0f } } } },
	Frame { { Band { Point { 0.5f, -11.0f }, Point { 0.5f, -11.0f } },
	          Band { Point { 0.5f, -9.0f }, Point { 0.5f, -9.0f } },
	          Band { Point { 0.5f, -7.0f }, Point { 0.5f, -7.0f } },
	          Band { Point { 0.5f, -5.0f }, Point { 0.5f, -5.0f } },
	          Band { Point { 0.5f, -3.0f }, Point { 0.5f, -3.0f } },
	          Band { Point { 0.5f, -1.0f }, Point { 0.5f, -1.0f } } } },
};

//------------------------------------------------------------------------------------------------
// THE DEVICE'S STATE
//------------------------------------------------------------------------------------------------

/// THE WHOLE DEVICE. Four frames, the puck, and the four globals of §3c that sit ON TOP of whatever
/// the frames blend to.
///
/// **The globals are STORED HERE AND APPLIED IN PHASE 2**, which is where §6 puts them — `blend()`
/// below returns the blended frame and nothing else. They are declared now because the struct is the
/// thing phase 2's descriptor is written against, and a struct that grows fields later is a struct
/// every phase-1 test has to be revisited for.
struct Settings {
	/// DEFAULT ON, like every other device. See `kParamDescs` for the decision and
	/// the static_assert below that pins this to it.
	bool enabled = true;

	std::array<Frame, kFrames> frame = kDefaultFrames;

	/// WHERE THE PUCK IS. Centre, so a fresh device is BETWEEN all four corners rather than sitting
	/// on one of them — which is the state the card is drawn to show, and it costs nothing because
	/// the default frames are identical.
	float morphX = 0.5f;
	float morphY = 0.5f;

	/// SLIDES THE WHOLE DRAWN SHAPE ALONG THE SPECTRUM, IN OCTAVES, by adding to every angle in the
	/// log coordinate. 0 is "as drawn". PHASE 2.
	float cutoff = 0.0f;
	/// PULLS EVERY POLE TOWARD THE UNIT CIRCLE, sharpening every resonance the drawing has without
	/// moving any of them. 0 is "as drawn". PHASE 2.
	float res = 0.0f;
	/// `multimode_filter`'s existing nonlinearity. PHASE 2.
	float drive = 0.0f;
	/// **HOW FAR THE ZEROS LAG THE POLES WHILE THE PUCK MOVES** — design §8, and it is the one global
	/// that does nothing at all until something moves.
	///
	/// The device smooths in the pole-zero domain because the puck is modulatable. `Wake` SPLITS that
	/// time constant: poles keep the fast one, zeros get a slower one this knob sets. At 0 the two are
	/// the same and the behaviour is bit-identical to a device that had never heard of it; turned up,
	/// a move sends the poles ahead and the zeros trail, then they arrive.
	///
	/// A SEPARATING POLE/ZERO PAIR IS EXACTLY WHAT "MORE FILTER" MEANS, which is why this is the axis
	/// worth having: while the zeros trail, a `Bypass` band briefly stops being transparent, a `Notch`
	/// opens out of its null, and the whole curve smears toward more resonant and then re-forms into
	/// exactly what was drawn. The other three globals are STATIC shapers — they do the same thing
	/// whether you are moving or still — and this device's identity is the morph.
	///
	/// **IT IS SAFE BY THE ASYMMETRY §3b ESTABLISHED**: stability constrains poles and nothing
	/// constrains zeros, so a lagging ZERO cannot make an unstable filter. A lagging pole would have
	/// been a different proposition and is deliberately not what this is.
	///
	/// AND IT RETURNS TO THE TRUTH. At rest the zeros arrive, so the response is the drawn curve —
	/// `Wake` colours the journey and never the destination. That is asserted, because a control that
	/// permanently coloured the curve would just be a second `Res`.
	float wake = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;

	/// FOUR FLAT CORNERS — see `kDefaultFrames`, which is the one place the numbers live.
	static std::array<Frame, kFrames> defaultFrames() { return kDefaultFrames; }
};

//------------------------------------------------------------------------------------------------
// THE BLEND
//------------------------------------------------------------------------------------------------

/// THE BILINEAR WEIGHTS, in `Corner` order: `(1-x)(1-y)`, `x(1-y)`, `(1-x)y`, `xy`.
///
/// EXPOSED RATHER THAN PRIVATE because everything else rests on two properties of these four
/// numbers — non-negative, and summing to one — and a property nothing can assert directly is a
/// property nobody is holding. `x` and `y` are clamped to [0, 1] first; outside that range the
/// weights would go negative and the stability guarantee with them.
std::array<float, kFrames> bilinearWeights(float x, float y);

/// THE SAME CONVEX COMBINATION, applied to each stored float in turn. Every overload is this one
/// function; they are separate only so a caller does not have to unpack a frame to blend a band.
///
/// **IT DOES NOT CLAMP ITS RESULT, ON PURPOSE.** Clamping the blended pole radius here would make
/// the guarantee true by force and the case that checks it vacuous — a weight bug that pushed a pole
/// outside the circle would be silently pulled back in and the test would pass. The contract is
/// stated the other way round: *given corners that satisfy `poleIsStable`, the blend does too*, and
/// `sanitise()` is what makes the corners satisfy it.
Point blend(const std::array<Point, kFrames>& corners, float x, float y);
Band blend(const std::array<Band, kFrames>& corners, float x, float y);
Frame blend(const std::array<Frame, kFrames>& corners, float x, float y);

/// The device's own puck position, which is the only caller phase 2 will have.
Frame blend(const Settings& settings);

/// CLAMPS A FRAME INTO THE REPRESENTABLE SET — pole radius into [0, kMaxPoleRadius], zero radius into
/// [-kMaxZeroRadius, kMaxZeroRadius], both angles into [kMinAngleLog2, kMaxAngleLog2]. This is the
/// INGEST guard: the editor and the state reader call it, `blend` does not.
void sanitisePole(Point& pole);
void sanitiseZero(Point& zero);
void sanitise(Band& band);
void sanitise(Frame& frame);
void sanitise(Settings& settings);

/// Is this pole strictly inside the unit circle? The whole stability question, in one line.
bool poleIsStable(const Point& pole);

//------------------------------------------------------------------------------------------------
// THE CLOSED-FORM RESPONSE — for drawing and for asserting, NOT for rendering
//------------------------------------------------------------------------------------------------

/// ONE SECTION'S COEFFICIENTS, `(1 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)`. Both leading
/// coefficients are 1 and there is no gain normalisation, which is deliberate for phase 1: an
/// UNNORMALISED response is the one whose `Bypass` case is EXACTLY 1.0 in float, because numerator
/// and denominator are then computed from bit-identical inputs. Phase 2 owns normalisation, along
/// with the real render path.
struct Biquad {
	float b1 = 0.0f;
	float b2 = 0.0f;
	float a1 = 0.0f;
	float a2 = 0.0f;

	friend bool operator==(const Biquad&, const Biquad&) = default;
};

Biquad coefficientsOf(const Band& band);

/// MAGNITUDE AT NORMALISED FREQUENCY `nu` (1 is Nyquist), unnormalised — see `Biquad`.
float magnitudeAt(const Band& band, float nu);
/// The six sections in series: the product of their magnitudes.
float magnitudeAt(const Frame& frame, float nu);


//==================================================================================================
// PHASE 2 — THE GLOBALS, THE REALISATION, AND THE DEVICE
//==================================================================================================

/// HOW HARD `Res` PULLS. The warp is `r -> 1 - (1 - r) * k` with `k = 1 - res * kResDepth`, so
/// `res = 1` divides every pole's distance from the unit circle by ten — an order of magnitude of Q.
inline constexpr float kResDepth = 0.9f;

/// THE GLOBALS APPLIED TO A BLENDED FRAME — plan §3c, and the first of them is why the coordinate is
/// a log.
///
/// **CUTOFF IS AN ADDITION.** `angleLog2` is log2 of normalised frequency, so sliding the whole drawn
/// shape by N octaves is `angleLog2 += N`, with the internal spacing intact by construction. In
/// radians it would be a multiply that has to be re-clamped per point and stops being a rigid
/// translation at the top of the range.
///
/// **RES IS A RADIAL WARP WITH ITS FIXED POINT ON THE UNIT CIRCLE**, and both halves of that sentence
/// are load-bearing:
///
///   - **It is applied to POLES AND ZEROS ALIKE, which plan §3c does not say.** §3c says "scale every
///     pole's RADIUS"; doing only that BREAKS `Bypass`. A bypassed band is the zero sitting on the
///     pole, so moving the pole and not the zero turns a transparent band into a resonance — which
///     means raising Res on a DEFAULT patch would switch six resonances on. **A global has to be a
///     map on the z plane applied uniformly, or it destroys every type whose definition is a RELATION
///     between the pole and the zero** (`Bypass`, `Notch`, `Bell`, `Allpass`). Same argument as
///     Cutoff, which is uniform for free because it is an addition.
///   - **Its fixed point at |r| = 1 is what keeps the on-circle types intact AND what keeps the
///     filter stable.** `1 - (1 - r) * k` maps 1 to 1, so a `Notch`'s and a `Lowpass`'s zeros stay
///     exactly on the circle; and because it is a contraction toward 1 of a radius already strictly
///     below 1, the warped pole is strictly below 1 too — **no clamp is needed and none is applied**,
///     which matters because a clamp on the pole that did not also apply to the zero would break
///     `Bypass` all over again. Worst case: a pole at `kMaxPoleRadius` plus the blend's 1e-7, warped
///     at `k = 0.1`, lands at 0.99999 — 1e-5 of margin against a 1e-7 error.
///   - **The sign of a zero's radius is preserved**, so a `Bandpass`'s split-real `-1` stays `-1`
///     rather than being dragged to the conjugate branch. The warp acts on the magnitude.
///
/// `drive` and `smooth` are NOT applied here: drive is a waveshaper with no pole-zero meaning, and
/// `smooth` is not understood — see `Settings::smooth`.
[[nodiscard]] Frame withGlobals(const Frame& frame, float cutoffOctaves, float res);

/// ONE REALISED SECTION: the monic coefficients plus the `b0` that normalises it.
struct Section {
	Biquad c {};
	/// THE NUMERATOR'S LEADING COEFFICIENT, i.e. how much this section is scaled by. See `realise`.
	float gain = 1.0f;

	friend bool operator==(const Section&, const Section&) = default;
};

/// THE SIX SECTIONS PLUS THE CASCADE TRIM.
struct Realised {
	std::array<Section, kBands> section {};
	/// 1 UNLESS THE SECTIONS' PEAKS MULTIPLY ABOVE THE CEILING — see `realise`.
	float trim = 1.0f;

	friend bool operator==(const Realised&, const Realised&) = default;
};

/// THE LARGEST |H| ANYWHERE ON THE UNIT CIRCLE, IN CLOSED FORM — no search, no probe grid.
///
/// `|H(e^{jw})|² = P(c)/Q(c)` with `c = cos w` and both P and Q QUADRATIC in c:
///
///     P(c) = (1 + b1² + b2² - 2 b2) + 2 b1 (1 + b2) c + 4 b2 c²
///
/// so `P'Q - PQ'` has its cubic term cancel and the extrema are the roots of a QUADRATIC. The answer
/// is the largest of `P/Q` at those roots inside [-1, 1] and at the two endpoints c = ±1.
///
/// A SAMPLED MAXIMUM WOULD NOT DO. A pole at radius 0.9999 has a peak narrower than any probe grid
/// this could afford, so a sampled max under-reads it — and this number is a SAFETY bound, where
/// under-reading is the direction that lets a cascade of six resonances through.
[[nodiscard]] float peakMagnitude(const Biquad& c);

/// A SECTION'S NORMALISATION POINT MAY SIT AT MOST THIS FAR BELOW ITS PEAK. See `realise`.
inline constexpr float kPeakHeadroom = 4.0f;
/// AND THE SIX SECTIONS' PEAKS MAY MULTIPLY TO AT MOST THIS. See `realise`.
inline constexpr float kCascadeCeiling = 4.0f;

/// HOW FINELY THE CASCADE IS SWEPT TO FIND ITS PEAK, when `realise` decides whether the chain needs
/// trimming at all. This replaced the product of the sections' peaks, which was an upper bound that
/// six non-coincident peaks made hopelessly loose — measured at 13.3 dB of unnecessary trim on six
/// lowpass bands at r = 0.95. Runs on a settings change beside the six root-solves already there,
/// never per sample.
inline constexpr int kTrimSweepPoints = 512;

// THE TWO HAVE TO AGREE, AND THEY DID NOT AT FIRST. A ceiling BELOW the headroom means a single
// resonant section is trimmed by the cascade cap the moment it exists — so the device would obey
// neither limit as stated: the per-section rule would say +12 dB and the chain rule would silently
// make it +6. Equal is the honest reading of both sentences at once: **the whole chain may boost by
// as much as one section may.** Caught by the case that asserts a lone Bandpass normalises to
// kPeakHeadroom, which measured 2 instead of 4.
static_assert(kCascadeCeiling >= kPeakHeadroom,
              "a cascade ceiling below the per-section headroom trims a single section, so neither "
              "limit means what it says");

/// COEFFICIENTS FOR A BLENDED FRAME, NORMALISED — the whole of what the audio path runs.
///
/// **THE NORMALISATION IS THE ONE REAL DECISION IN PHASE 2, and none of the obvious answers works.**
/// Phase 1's `magnitudeAt` is unnormalised because that is what makes `Bypass` exactly 1.0f in float;
/// an unnormalised LOWPASS, though, has a DC gain of `4 / |1 + a1 + a2|`, which is +39 dB at a pole
/// radius of 0.9 and +234 dB once six of them cascade. Something has to set `b0`. What was tried:
///
///   - **Unit peak per section** is safe and type-free, and it makes resonance DUCK the passband: a
///     lowpass normalised at its resonant peak sits at `1/Q` in the passband, so raising Res would
///     quieten the band it is supposed to emphasise.
///   - **Unit geometric mean** (`b0 = 1 / prod max(1, |zero|)`, exact by Jensen's formula) is
///     beautiful — it makes `Bypass` and `Allpass` exactly 1 with no evaluation at all — and it does
///     not BOUND anything: a lowpass's energy simply moves to the pole, +39 dB and all.
///   - **Unit gain at DC or at Nyquist**, which is what the familiar types want, divides by zero for a
///     `Bandpass`, whose response is null at BOTH.
///
/// SO IT IS THE EDGES, WITH THE PEAK AS A FLOOR:
///
///     b0 = 1 / clamp( max(|H(DC)|, |H(Nyquist)|),  peak / kPeakHeadroom,  peak )
///
/// which normalises at the edge the familiar types pass — a lowpass gets unity at DC, a highpass
/// unity at Nyquist, `Bypass` and a flattened `Allpass` exactly 1 — and falls back on the peak when
/// the edges are null or tiny, so a `Bandpass` is normalised to `kPeakHeadroom` rather than to
/// infinity, and a very resonant lowpass loses its passband gradually instead of without limit.
///
/// AND THEN THE CASCADE TRIM, because per-section headroom COMPOUNDS: six sections at +12 dB is
/// +72 dB. `trim` scales the whole chain so that the PRODUCT of the sections' normalised peaks is at
/// most `kCascadeCeiling`. That product is an upper bound on the cascade's true peak rather than the
/// peak itself — the six peaks sit at six different frequencies — which is the conservative direction
/// for a safety limit. **`Bypass` is untouched by it**: every normalised peak is exactly 1, the
/// product is exactly 1, and 1 is under the ceiling.
[[nodiscard]] Realised realise(const Frame& frame);

/// AND A LIMIT OF REALISATION ITSELF, MEASURED AND PINNED BY ITS OWN CASE. A biquad's denominator
/// holds a pole as `a1 = -2 r cos(theta)`, and at the bottom of the frequency range `cos(theta)` is
/// within a ulp of 1 — so a FLOAT `a1` cannot place the pole as finely as the stored polar coordinate
/// does. MEASURED across the whole range at four resonances: the realised frequency is within **0.1
/// octaves** of the stored one at the very floor and within **0.01 octaves** from ten octaves below
/// Nyquist upward; the radius, which is what stability depends on, survives to 1e-6.
///
/// **THIS IS AN ARGUMENT FOR THE POLE-ZERO STORAGE RATHER THAN AGAINST IT.** The state keeps the pole
/// exactly and only the realisation rounds, so nothing accumulates: every block re-realises from the
/// stored positions. It is a caution for anyone who moves `kMinAngleLog2` further down, which is
/// where the error grows.

/// THE NORMALISED CASCADE'S MAGNITUDE — what the card will draw and what the phase-2 cases assert.
[[nodiscard]] float magnitudeAt(const Realised& realised, float nu);

//------------------------------------------------------------------------------------------------
// THE DEVICE
//------------------------------------------------------------------------------------------------
/// WHICH OF A BAND'S FIVE FIELDS. The order is the row's contract and `bandFieldIndex` is the only
/// thing that should ever encode it.
///
/// DECLARED ABOVE `Param` BECAUSE `Param::kCount` IS COMPUTED FROM `kBandFields`. It used to say
/// `* 4` in a literal, which is the same number written twice — and the two would have drifted the
/// moment a band grew a field, which is exactly what happened when the type became state.
enum BandField : size_t {
	PoleRadius = 0,
	PoleAngle = 1,
	ZeroRadius = 2,
	ZeroAngle = 3,
	/// THE STORED TYPE. Named `TypeIndex` and not `Type` because this is an UNSCOPED enum and an
	/// enumerator called `Type` would collide with the enum class of that name in this same
	/// namespace — the kind of clash that reads as a mysterious error a hundred lines away.
	TypeIndex = 4,
	kBandFields = 5,
};


/// THE ROW'S LAYOUT. The seven globals first, then `kFrames * kBands * 4` band fields grouped
/// FRAME-MAJOR, so a row reads as four drawn curves rather than as ninety-six parallel arrays.
///
/// APPENDING IS STILL THE ONLY SAFE EDIT even though CLAUDE.md currently frees the patch format:
/// `kParamDescs` is index-aligned with this enum and the two are checked against each other by
/// static_assert, so a reorder that forgets the table is a build error and a reorder that remembers
/// it is a silent renumber of every saved row.
enum class Param : int {
	Enabled,
	/// THE PUCK. The two modulatable controls that matter — a drawn response is a fixed timbre until
	/// something moves these.
	MorphX,
	MorphY,
	/// §3c's four, sitting ON TOP of whatever the frames blend to.
	Cutoff,
	Res,
	Drive,
	Wake,
	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR), supplied and applied by the slot: the device
	/// never reads either, and neither the card nor the editor draws them (the site's head row does). Mix is a
	/// crossfade (100% the wet), the wet Gain a fader position (unity at 0.5) on the wet only; both defaults are Drawn
	/// as it was, to the bit. They waited on the row: 127 of 128 fields until `kMaxDeviceFields` went to 160.
	Mix,
	WetGain,

	/// THE FIRST BAND FIELD. Everything from here to kCount is `(frame, band, field)` — see
	/// `bandFieldIndex`.
	FirstBandField,

	kCount = FirstBandField + static_cast<int>(kFrames * kBands * kBandFields),
};

[[nodiscard]] constexpr size_t bandFieldIndex(size_t frame, size_t band, BandField field) {
	return static_cast<size_t>(Param::FirstBandField)
	       + ((frame * kBands + band) * kBandFields + static_cast<size_t>(field));
}

/// Row: the serialised form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

[[nodiscard]] Settings stateOf(const Row& row, size_t fieldsPresent);
[[nodiscard]] Row rowOf(const Settings& settings);

/// THE BLENDED, GLOBALS-APPLIED, REALISED CASCADE — the whole device as one function, and the shape
/// the audio path and the card both take.
///
/// **THE ORDER IS BLEND, THEN GLOBALS, THEN REALISE**, which plan §5.2 chose and flagged as an
/// assumption: four transforms collapse into one. It differs from transforming each frame first
/// whenever the frames disagree, and the cheaper one is almost certainly right.
[[nodiscard]] Realised realisedAt(const Settings& settings);

inline constexpr const char* const kOffOnNames[] {"Off", "On"};

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "filter", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "drwn",
	.name = "Drawn",
	.hasCard = true,
	/// THE EDITOR EXISTS NOW — `DrawnExpanded`, where the ninety-six pole and zero positions live.
	/// Declared true in the same change as the entry in `expanded_views.cpp` AND the arm in
	/// `fx_lane.cpp` that binds `onExpand`: OWLTT shipped the declaration without the entry and a tester
	/// got *"THERE IS NO EXPANDED VIEW AVAILABLE"*, and Waypoint shipped both without the arm and its
	/// panel was unopenable. `hasExpandedView` cannot see that third term, so a case presses the
	/// button at real window coordinates instead.
	.hasExpanded = true,
	/// NO PER-VOICE CONTROL — plan §5.2, *"bus only in v1"*. Six biquads per voice across twenty
	/// voices is a cost conversation of its own, and keytrack is what would force it. A device that
	/// has not implemented a per-voice road must not claim one.
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// THE SLOT BLENDS THESE (backlog NR): a plain crossfade around the device, which hands back the wet.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

/// ONE BAND'S FOUR DESCRIPTORS, so the table below cannot describe frame 3's band 5 with frame 1's
/// ranges. The names are qualified with the device, the frame and the band because a host's
/// parameter list is FLAT and ninety-six controls called "Pole Q" are ninety-six rows nobody can tell
/// apart — device.h's rule, and it bites hardest here of anywhere in the repo.
///
/// `ang` IS THE BAND'S DEFAULT FREQUENCY and it differs per band, which is why it is an argument:
/// `Settings::defaultFrames()` spreads the six handles across the spectrum, and a descriptor default
/// that disagreed with the constructed default would make an absent field decode to a DIFFERENT
/// device than a default-constructed one. The static_assert under `kParamDescs` checks exactly that.
///
/// THE ZERO DEFAULTS ON TOP OF THE POLE, which is `Bypass` — so a row of pure defaults is flat.
#define FEATHERS_DRAWN_BAND(f, b, fi, bi)                                                          \
	{f "_" b "_pole_q", f "." b " Pole Q", 0.0, static_cast<double>(kMaxPoleRadius),                         \
	 static_cast<double>(kDefaultFrames[fi].band[bi].pole.radius), false,                          \
	 device::Unit::Percent, {}, false, false,                                                      \
	 "How close band " b " of frame " f "'s pole sits to the unit circle — this is its Q"},        \
	{f "_" b "_pole_freq", f "." b " Pole Freq", static_cast<double>(kMinAngleLog2),                            \
	 static_cast<double>(kMaxAngleLog2),                                                           \
	 static_cast<double>(kDefaultFrames[fi].band[bi].pole.angleLog2), false,                       \
	 device::Unit::Number, {}, false, false,                                                       \
	 "Where band " b " of frame " f "'s pole sits, in octaves below Nyquist — 0 IS Nyquist"},      \
	{f "_" b "_zero_r", f "." b " Zero R", -static_cast<double>(kMaxZeroRadius),                             \
	 static_cast<double>(kMaxZeroRadius),                                                          \
	 static_cast<double>(kDefaultFrames[fi].band[bi].zero.radius), false,                          \
	 device::Unit::Number, {}, false, false,                                                       \
	 "Band " b " of frame " f "'s zero radius — on the pole is Bypass, 1 is a null on the circle, " \
	 "above 1 is an allpass mirror, and NEGATIVE is the split-real pair a bandpass needs"},         \
	{f "_" b "_zero_freq", f "." b " Zero Freq", static_cast<double>(kMinAngleLog2),                            \
	 static_cast<double>(kMaxAngleLog2),                                                           \
	 static_cast<double>(kDefaultFrames[fi].band[bi].zero.angleLog2), false,                       \
	 device::Unit::Number, {}, false, false,                                                       \
	 "Where band " b " of frame " f "'s zero sits, in octaves below Nyquist"},                      \
	{f "_" b "_type", f "." b " Type", 0.0, static_cast<double>(static_cast<int>(Type::kCount) - 1),        \
	 static_cast<double>(static_cast<int>(kDefaultFrames[fi].band[bi].type)), true,                \
	 device::Unit::Choice, kTypeNames, false, false,                                               \
	 "Which preset band " b " of frame " f " is bound to — every type but Free ties the zero to "   \
	 "the pole, so moving the pole carries the zero with it"}

/// ONE FRAME'S SIX BANDS, taking their declared defaults FROM `kDefaultFrames` rather than repeating
/// them. The numbers live in exactly one place; what still has to be checked is that the row slots
/// and the table are indexed the same way, which is what the declared-defaults case is for.
#define FEATHERS_DRAWN_FRAME(f, fi)                                                                \
	FEATHERS_DRAWN_BAND(f, "1", fi, 0), FEATHERS_DRAWN_BAND(f, "2", fi, 1),                        \
	    FEATHERS_DRAWN_BAND(f, "3", fi, 2), FEATHERS_DRAWN_BAND(f, "4", fi, 3),                    \
	    FEATHERS_DRAWN_BAND(f, "5", fi, 4), FEATHERS_DRAWN_BAND(f, "6", fi, 5)

// INLINE CONSTEXPR IN THE HEADER, NOT A DEFINITION IN THE .cpp, and this is a requirement rather
// than a style match: `device_registry.h`'s `widestDeviceControls()` and `device_row.h`'s width
// guard both walk every registered device AT COMPILE TIME, so a table that is merely `extern const`
// is not usable in a constant expression and neither static_assert can run.
inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    // See `fx::filter::kParamDescs` for the decision and what it replaced.
    {"enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Whether the drawn-curve filter is in the chain"},
    {"morph_x", "Morph X", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Where the puck sits between the left and right frames — bilinear, so all four contribute"},
    {"morph_y", "Morph Y", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Where the puck sits between the bottom and top frames"},
    {"cutoff", "Cutoff", -4.0, 4.0, 0.0, false, device::Unit::Number, {}, true, false,
     "Slides the whole drawn shape along the spectrum, in OCTAVES, with its spacing intact"},
    {"res", "Res", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Pulls every pole toward the unit circle — sharpens every resonance the drawing has without "
     "moving any of them"},
    {"drive", "Drive", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Saturation in front of the cascade — 0 is exactly the untouched signal"},
    // THE ONE GLOBAL THAT DOES NOTHING UNTIL SOMETHING MOVES — design §8. It was `Smooth`, declared
    // and inert while nobody knew which of two readings Serum's meant; the decision was not to copy
    // it at all but to make the knob do something sonically unique, and this is what it became.
    //
    // MODULATABLE, on the same terms as Drive: it is an amount that changes the sound, and an
    // envelope on it is a real gesture — more smear as a note starts, none once it settles. The
    // stride has room (the retired control stride is 32 and the widest device declares 24), so this costs
    // nothing structural.
    {"wake", "Wake", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "How far the zeros lag the poles while the puck moves — inaudible at rest, blooms on a sweep. "
     "At 0 the zeros track the poles exactly"},
    {"mix", "Drawn Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry against filtered, around the whole device"},
    {"wet_gain", "Drawn Wet Gain", 0.0, 1.0, 0.5, false, device::Unit::Fader, {}, true, false,
     "Level of the filtered signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},

    FEATHERS_DRAWN_FRAME("1", 0),
    FEATHERS_DRAWN_FRAME("2", 1),
    FEATHERS_DRAWN_FRAME("3", 2),
    FEATHERS_DRAWN_FRAME("4", 3),
};

#undef FEATHERS_DRAWN_FRAME
#undef FEATHERS_DRAWN_BAND

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE ENABLE'S TWO DECLARATIONS, PINNED TOGETHER — see `fx::owltt` for the full statement of why a
/// descriptor default and a struct initialiser that disagree give a device two different fresh
/// states.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

/// THE RENDER PATH — six biquads in series, driven by the realised cascade above.
extern const device::AudioOps kAudioOps;

/// TEST ONLY — WHAT THE DSP IS ACTUALLY RUNNING, read out of an ops instance.
///
/// IT EXISTS FOR THE REASON `waypoint::targetForTest` AND `Synth::voiceFilterTargetForTest` EXIST:
/// a pole position is not recoverable from a rendered buffer. A case asking "did the puck move the
/// filter" by comparing two renders passes whenever ANYTHING differs, which is the partial-effect
/// blindness CLAUDE.md's rule is about. If the claim is about a value, read the value.
[[nodiscard]] const Settings& targetForTest(const void* storage);
/// The COEFFICIENTS the sample loop is running, after blending, globals, realisation and smoothing.
[[nodiscard]] const Realised& realisedForTest(const void* storage);
/// The smoothed frame behind them — the pole and zero positions, before realisation.
[[nodiscard]] const Frame& smoothedFrameForTest(const void* storage);
/// The reference road (DrawnFilter::setReferenceForTest) and the realisation count (realisationsForTest).
void setReferenceForTest(void* storage, bool on);
[[nodiscard]] uint64_t realisationsForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: an effect, not a modulator. Spelled rather than defaulted because
	// -Werror=missing-field-initializers makes every field of this struct a DECISION.
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::FilterEq,
};


} // namespace feathers::fx::drawn
