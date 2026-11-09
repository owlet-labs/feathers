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

#include "plugin/ui/control_face.h"

#include "plugin/ui/widget_registry.h"
#include "fx/drawn/drawn_types.h"
#include "plugin/ui/controls.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/fx_control.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

// THE DRAWN-CURVE FILTER'S EDITOR — where the curves are actually drawn. Design §6's phase 4.
//
// **THIS IS THE SURFACE THE OTHER NINETY-SIX CONTROLS LIVE ON.** The card carries seven; every pole
// and zero position is here, and tests/test_gui_widgets.cpp's reachability entry for `drwn` loses its
// gap list in the same change that adds this file. That entry is pinned both ways, so it fails if
// this panel gains a control it does not claim OR claims one it does not reach.
//
// THE PLANE IS FREQUENCY AGAINST RADIUS, which is the design's reading of the reference: the only
// thing left to place is the POLE — one handle, X = frequency, Y = Q. Radius IS Q, and the
// frequency axis is the stored `angleLog2` directly, so the plane is the state rather than a
// rendering of it.
//
// AND THE UNIT CIRCLE IS A HORIZONTAL LINE, at radius 1. That is the one drawing decision worth
// arguing for: **§3b's asymmetry becomes visible geometry.** Poles must stay below the line —
// stability constrains them — and zeros may sit on it (a Notch, a Lowpass), above it (an Allpass
// mirror at 1/r) or anywhere below. A z-plane disc would have drawn the same state and hidden that,
// because in a disc "inside" is a distance from a centre rather than a side of a line.
//
// WHAT REACHES WHAT:
//   - a band's POLE radius and frequency — dragging its filled handle;
//   - a band's ZERO radius and frequency — dragging its hollow handle, and the type menu, which
//     writes a placement through `zeroPlacementFor`;
//   - the frame — the four tabs, which are editor state and not a device control.
//
// **THE ZERO IS ALWAYS DRAGGABLE, AND THAT IS FORCED RATHER THAN CHOSEN.** Serum shows the red dot
// for `Free` alone; Serum can do that because it STORES a type. This device deliberately does not
// (Free is not a special mode — it is the OTHER types that are constrained), so there is no state in
// which the editor could know the zero is not the player's to move. Dragging it IS what makes a band free.

namespace feathers::ui {

class DrawnExpanded final : public ExpandedView, public ControlFace {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	DrawnExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	              Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	DrawnExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	/// Backlog EI: WIDE WITHOUT LIMIT, since the plane's frequency axis and the knob row take the width;
	/// the height is capped at the natural size plus one `kPad`, since the plane's height is fixed.
	visage::Point maximumSize() const override;

	/// THE ROW BENEATH THE PLANE, so a case can ask what it holds rather than photograph it.
	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	[[nodiscard]] visage::Bounds knobRowBounds() const;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	static constexpr float kPad = 12.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kTabHeight = 22.0f;
	/// THE PLANE, LARGE, because this is where every aiming gesture in the device happens. OWLTT
	/// measured the lesson at 48px — "clunky" meant targets that could not be aimed.
	static constexpr float kPlaneHeight = 300.0f;

	/// **THE CARD'S SIX CONTROLS, WHICH THIS WINDOW DID NOT HAVE.** The rule: an expanded window is
	/// always a superset of the controls on the card. Drawn's
	/// window was a pole-zero editor and nothing else, so opening it put its card's six controls out of
	/// reach — found by generalising the superset test rather than by anyone noticing, and carried on
	/// its accounted-for list since.
	static constexpr size_t kKnobCells = 6;
	static constexpr float kCellWidth = 84.0f;
	/// THE BAND LETTER'S SIZE AND ITS OFFSET FROM THE POLE. Small enough to read as a legend rather
	/// than a control, and placed up-and-left so it clears `kGrabRadius` (13px) — a label under the
	/// cursor during a drag is a label in the way.
	static constexpr float kBandLabelSize = 9.0f;
	static constexpr float kBandLabelDx = 7.0f;
	static constexpr float kBandLabelDy = -14.0f;
	static constexpr float kPoleRadius = 5.0f;
	static constexpr float kZeroRadius = 4.5f;
	/// A GRAB RADIUS LARGER THAN THE INK, for the reason OWLTT's graph established and the waypoint
	/// card restates: the thing you aim at is the target, not the dot.
	static constexpr float kGrabRadius = 13.0f;
	/// THE TOP OF THE RADIUS AXIS. Two, so an `Allpass`'s mirror zero at 1/r has somewhere to be —
	/// a pole at r = 0.5 mirrors to exactly the top of the plane — and so the unit circle sits
	/// below the ceiling rather than on it, where it could not be told from the border.
	static constexpr float kRadiusCeiling = 2.0f;

	//--------------------------------------------------------------------------------------------//
	// THE VERTICAL AXIS — logarithmic in distance from the unit circle                             //
	//--------------------------------------------------------------------------------------------//
	//
	// **THIS AXIS WAS LINEAR IN RADIUS AND IT MADE THE EDITOR UNUSABLE** — the pop-out editor was
	// twitchy. A pole's resonance goes as `Q ~ 1/(2(1-r))`, so
	// a LINEAR radius axis is a reciprocal Q axis, and one pixel of a 300px plane was 0.0067 of
	// radius wherever you were:
	//
	//     r = 0.90   Q =   5  -> 5.4        r = 0.99   Q = 50 -> 150        (tripled, by one pixel)
	//
	// Q from 50 to 500 occupied **1.4 pixels**, and since a pole clamps at `kMaxPoleRadius` while
	// the plane ran to 2, half the panel could not hold one at all.
	//
	// SO THE AXIS IS LOG IN `|r - 1|` — equal pixels, equal RATIO of distance-from-the-circle, and
	// therefore equal ratio of Q. MEASURED at these constants, on this 300px plane: 19.15 px per
	// octave of Q, constant to **1.9% over Q in [0.5, 100]** and to 1.42x out to Q = 500. The same
	// pixel that used to take Q from 50 to 150 now takes it from 50 to 51.9.
	//
	// AND THE ANGLE DOES NOT ENTER IT. Q is `theta / (2(1 - r))` for a real resonator, so at a fixed
	// frequency `d(log Q) = -d(log(1 - r))` exactly — the number above is not a linearisation around
	// one frequency, it holds at every frequency the plane draws.
	//
	// **THE SHAPE IS `asinh`, NOT `log`, AND THAT IS THE WHOLE TRICK.** `log|r - 1|` is a logarithm
	// with a pole at r = 1 and no answer at all for the sign change, and the sign change is not
	// hypothetical here: §3b's asymmetry puts zeros ABOVE the circle (an `Allpass` mirror at 1/r).
	// `asinh(x)` is `sign(x) * ln(2|x|)` once `|x| > 1` and a straight line through the origin
	// inside it, so one continuous, strictly increasing, exactly invertible function is a log axis
	// on BOTH sides and joins them smoothly across r = 1. It stays finite and monotone through
	// r = 0 and below.
	//
	// WHAT IT BOUGHT ABOVE THE LINE, which was not the point but is the most visible change: an
	// `Allpass` mirror of a pole at r = 0.99 sits at 1.0101, which the linear axis drew **1.5px**
	// above the circle — indistinguishable from on it. It is now 35.6px above.
	//
	// **AND IT IS STILL NOT A REFLECTION, which the first draft of this comment claimed and the
	// measurement refuted.** That pole is 82.8px BELOW the line while its mirror is 35.6px above:
	// `asinh` is odd, so the two are at equal WARPED distances, but `kCircleHeight` gives the two
	// sides different pixels per warped unit and the drawing is asymmetric by exactly that ratio.
	// Setting `kCircleHeight` to 0.5 would make the mirror a true reflection near the circle, and
	// would also crowd the default lowpass pair inside one grab radius — see below. The pair is
	// legible and ordered, not symmetric.

	/// **THE SHAPING CONSTANT — THIS IS THE ONE TO TURN.** The distance from the unit circle at
	/// which the axis stops being logarithmic and becomes linear: above `Q ~ 1/(2 * kCircleSoftness)`
	/// — 500 here — the top of the range is compressed rather than expanded.
	///
	/// SMALLER IS MORE LOG. At 1e-4 (which is `1 - kMaxPoleRadius`, the smallest distance the state
	/// can represent) the axis is uniform to 1.37x over the WHOLE pole range out to Q = 5000 — but
	/// it spends 20px of the plane on radii no pole can reach, and it draws the two default lowpass
	/// sections 13.6px apart against a 13px `kGrabRadius`, which is a 0.6px margin on a case that
	/// exists because two handles once landed on top of each other. 1e-3 is the measured compromise:
	/// uniform where a filter is played, gently compressed in the last 22px of knife-edge Q, and the
	/// default handles 17.7px apart. Tune by ear; the numbers above are all in `axis` cases in
	/// tests/test_gui_widgets.cpp and will move with it.
	static constexpr float kCircleSoftness = 0.001f;

	/// **WHERE THE UNIT CIRCLE SITS**, as a fraction of the plane height measured from the BOTTOM.
	/// It used to be exactly a half and that was an accident of the linear axis, not a choice: on a
	/// log axis r = 0 and r = 2 are equally far from the circle, so a half would hand the top half
	/// of the panel to the one thing that lives up there.
	///
	/// SEVEN TENTHS, BECAUSE THE FILTER IS ASYMMETRIC AND §3b IS THE REASON. Everything a POLE can
	/// be is below the line — stability constrains them and nothing constrains zeros — and above it
	/// there is essentially one inhabitant, the allpass mirror. The pole side needs the pixels: at
	/// a half the default lowpass pair drew 12.7px apart, inside one grab radius of each other.
	static constexpr float kCircleHeight = 0.7f;

	/// THE AXIS ITSELF, AS TWO PURE FUNCTIONS, and they are **exact inverses** — that is the property
	/// a hand notices, because it is what puts the handle under the pointer that dragged it.
	/// `t` is a height fraction: 0 at the bottom of the plane, `kCircleHeight` on the unit circle,
	/// 1 at `kRadiusCeiling`.
	///
	/// `radiusAtHeight` CLAMPS ITS RESULT TO [0, kRadiusCeiling], and the LOWER clamp is there because
	/// `sinh` at the bottom of the axis recovers `1/kCircleSoftness` only to within a ulp, so the
	/// floor of the plane can come back as a very small NEGATIVE radius — which is not a small error,
	/// it is the split-real branch: `zeroIsSplitReal` reads the band as a `Bandpass` and draws two
	/// marks on the border instead of the handle the hand was holding.
	///
	/// **MEASURED, BECAUSE THE FIRST DRAFT OF THIS COMMENT CALLED IT LOAD-BEARING AND THE KNOWN-BAD
	/// CAME BACK GREEN.** At the constants shipped here the unclamped floor is **+5.36e-7 —
	/// POSITIVE** — so deleting the clamp today changes nothing and no case can see it. It is not
	/// decoration either: over a swept grid of 16 `kCircleSoftness` values against 19
	/// `kCircleHeight` values, **175 of 304 pairs put the unclamped floor below zero**, worst
	/// -1.55e-6; 0.7 against 1e-3 is one of the lucky ones. Since the note above invites turning both
	/// constants, this clamp is protecting the TUNING rather than the current build. The case that
	/// guards it was shown able to fire by moving `kCircleHeight` to 0.8 — where the floor is
	/// -3.58e-7 — and reddens with the clamp gone, stays green with it kept.
	[[nodiscard]] static float radiusAtHeight(float t);
	/// The inverse. A radius outside [0, kRadiusCeiling] is clamped, which is why a split-real zero
	/// (radius < 0) has no position here and is drawn by `splitZeroCentres` instead.
	[[nodiscard]] static float heightForRadius(float radius);

	//------------------//
	// Geometry, asked  //
	//------------------//

	[[nodiscard]] visage::Bounds planeBounds() const;
	[[nodiscard]] visage::Bounds tabBounds(size_t frame) const;
	/// WHAT A TAB SAYS — `"Frame "` and the frame's numeral, composed from
	/// `fx::drawn::kFrameNumerals`, which `DrawnCard::frameLabelText` also composes ITS corner marker
	/// from. **The composition is the point:** the card's plane and this panel's tabs are the only two
	/// places in the plugin that name a frame, and two tables would let them drift into calling one
	/// frame by two names — which is what a corner marker exists to prevent.
	[[nodiscard]] static std::string frameTabText(size_t frame);
	/// The letter drawn beside band `b`'s pole — `a`..`f`. Public because it is the only handle a case
	/// has on a label: the picture cannot be read, so the text it is made from is what gets asserted.
	[[nodiscard]] static std::string bandLabelText(size_t band);

	/// THE FRAME-EXPLICIT ROADS, public because the frame menu's whole point is editing a frame that
	/// is NOT the one on screen — and a case has to be able to drive that from outside.
	[[nodiscard]] size_t fieldIndex(size_t frame, size_t band, fx::drawn::BandField field) const;
	[[nodiscard]] fx::drawn::Band frameBand(size_t frame, size_t band) const;
	[[nodiscard]] fx::drawn::Frame frameAt(size_t frame) const;
	void flattenFrame(size_t frame);
	void resetFrame(size_t frame);
	void copyFrame(size_t from, size_t to);
	/// **COPY THIS FRAME ONTO ALL THREE OTHERS, AS ONE UNDO STEP** — Drawn's 'copy to all others'.
	///
	/// THE WHOLE FRAME, WHICH IS THE SAME THING `copyFrame` COPIES — all six bands' pole, zero and
	/// type. It is the plural of the entry sitting beside it in the same menu, and two adjacent items
	/// spelling the same verb with different scopes would be worse than not having the second. There
	/// is also nothing else it could mean: a frame IS its six bands, and Cutoff, Res, Drive, Wake and
	/// the two Morph axes are device-global rather than per-frame.
	void copyFrameToAll(size_t from);

	/// THE THREE VERBS THAT ARE NOT A FRAME INDEX. Negative, because a non-negative id in this menu IS
	/// the frame to copy to.
	static constexpr int kFrameMenuFlatten = -1;
	static constexpr int kFrameMenuReset = -2;
	static constexpr int kFrameMenuCopyAll = -3;

	/// WHAT THE FRAME TAB'S MENU OFFERS — each entry's id and the label it draws.
	///
	/// **ONE LIST, TWO READERS.** `showFrameMenu` builds the popup from this and a case reads the same
	/// function, so a case cannot go on passing against a menu that has quietly stopped offering the
	/// entry. That seam is needed rather than tidy: a shown `visage::PopupMenu` is not something the
	/// widget suite can press, so without it the only assertion available would be about the verb
	/// underneath — and a verb no menu offers is the honesty failure this panel's neighbours have
	/// shipped twice.
	[[nodiscard]] static std::vector<std::pair<int, std::string>> frameMenuOptions(size_t frame);

	/// ONE FIELD'S BEFORE-AND-AFTER, so a menu click can be ONE undo step instead of twenty-four.
	///
	/// **THIS EXISTS BECAUSE A MENU CLICK IS ONE ACTION AND WAS NOT ONE STEP.** Every one of the frame
	/// verbs used to bracket each changed field as its own `DeviceControl` gesture, and
	/// `DeviceControl::endGesture` records a `DeviceValueEdit` per gesture — so "Copy to Frame 2" was
	/// up to twenty-four presses of undo to take back, and it was never possible to take back HALF of
	/// one. By design a menu click is one action and must be one undo step.
	///
	/// A LIST OF FIELDS RATHER THAN A WHOLE-SETTINGS SNAPSHOT (`MsegShapeEdit`'s shape), because these
	/// verbs already compute exactly which fields differ in order to skip the writes they do not need,
	/// and because a copy to all others spans three frames — so the natural unit is the changes, not
	/// the frame.
	///
	/// The shared record's unit (`DeviceFieldsEdit`, device_control.h), named here for this panel's own code.
	using FieldChange = DeviceFieldChange;
	/// Where band `band`'s pole is drawn, in panel coordinates.
	[[nodiscard]] visage::Point poleCentre(size_t band) const;
	/// Where its zero is drawn. **Meaningless when `zeroIsSplitReal(band)`** — that pair is two points
	/// at the ends of the frequency axis and is drawn by `splitZeroCentres` instead.
	[[nodiscard]] visage::Point zeroCentre(size_t band) const;
	/// A NEGATIVE ZERO RADIUS IS THE SPLIT-REAL PAIR a `Bandpass` needs — one zero at DC and one at
	/// Nyquist. There is no single point to draw, and drawing one at a negative height would be a
	/// picture of a number rather than of the filter.
	[[nodiscard]] bool zeroIsSplitReal(size_t band) const;
	/// The two marks a split-real zero draws: the ends of the frequency axis, on the unit circle.
	[[nodiscard]] std::pair<visage::Point, visage::Point> splitZeroCentres(size_t band) const;

	/// WHICH HANDLE A PRESS TAKES. Nearest wins; a tie goes to the POLE.
	enum class Grab : int { None, Pole, Zero };
	struct Hit {
		Grab what = Grab::None;
		size_t band = fx::drawn::kBands;
	};
	[[nodiscard]] Hit hitAt(visage::Point position) const;

	[[nodiscard]] size_t selectedFrame() const noexcept { return frame_; }
	void selectFrame(size_t frame);
	[[nodiscard]] size_t selectedBand() const noexcept { return band_; }

	/// Apply a type's zero placement to a band — what the right-click menu does, exposed so a case can
	/// assert the VALUE it writes rather than driving a popup.
	void applyType(size_t band, fx::drawn::Type type);

	/// EVERY CONTROL THIS PANEL REACHES — the claim the reachability guard reads. A hundred and twenty
	/// band fields and nothing else; the seven globals are the card's. (It said "ninety-six" here long
	/// after the body stopped agreeing: ninety-six is four fields a band, and the type made it five.)
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;

private:
	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] fx::drawn::Settings settings() const;
	[[nodiscard]] const fx::drawn::Band& band(size_t index) const;
	[[nodiscard]] size_t fieldIndex(size_t band, fx::drawn::BandField field) const;

	/// THE FRAME TAB'S OWN MENU — a right-click dropdown on the expanded editor's frame tabs, with
	/// options like flatten/reset and copy to (another frame).
	void showFrameMenu(size_t frame, visage::Point at);

	/// Write the fields of `frame` that differ from `wanted`, APPENDING each one's before-and-after to
	/// `into` rather than recording anything. The caller decides what one undo step is.
	void writeFrameInto(size_t frame, const fx::drawn::Frame& wanted, std::vector<FieldChange>& into);
	/// Record one step for a whole verb's worth of changes. An empty list records nothing, which is
	/// what keeps an identical copy off the undo stack.
	void recordFields(std::vector<FieldChange> changes, const char* verb);
	/// Writes only the fields that differ, as ONE undo step named `verb`.
	void writeFrame(size_t frame, const fx::drawn::Frame& wanted, const char* verb);
	[[nodiscard]] float angleForX(float x) const;
	[[nodiscard]] float radiusForY(float y) const;
	void dragPole(visage::Point position);
	void dragZero(visage::Point position);
	void showTypeMenu(size_t band, visage::Point at);
	void drawPlane(visage::Canvas& canvas);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	size_t frame_ = 0;
	size_t band_ = 0;
	Grab held_ = Grab::None;
	/// **HOW MUCH OF THE ZERO THIS BAND'S TYPE OWNS**, which is the whole of what "types stick" means.
	///
	/// THIS REPLACED A LATCH ON EXACT COINCIDENCE, and the comment that stood here said why it had to:
	/// *"A `Notch`'s zero also follows its pole's angle, and re-deriving that on drag would mean
	/// INFERRING the type from the numbers — which is the stored type §3b removed, reintroduced as a
	/// guess."* **The type is stored now, so the guess is a read**, and the special case for Bypass
	/// generalises to every type rather than sitting beside them.
	enum class ZeroBinding : int {
		/// The pole carries nothing. `Free`, and also the three whose placement does not depend on
		/// the pole at all — a lowpass's zeros stay at Nyquist whatever its corner does.
		None,
		/// The pole carries the whole zero: `Bypass`, `Notch`, `Allpass`.
		Full,
		/// The pole carries the ANGLE and the hand keeps the radius: `Bell` alone, because **a bell's
		/// zero radius IS its gain** — so a bell keeps its radius drag, by
		/// decision. Recomputing it from `kBellDefaultGain` on every pole drag would silently reset a
		/// gain the player had set.
		AngleOnly,
	};
	[[nodiscard]] static ZeroBinding bindingOf(fx::drawn::Type type);

	/// May a hand grab this band's zero at all? A bound zero is DERIVED, and a derived handle that
	/// answered a drag would be the honesty rule broken in the direction this file already guards
	/// against for the split-real case — see `hitAt`.
	[[nodiscard]] bool zeroDraggable(size_t band) const;

	/// Latched at the press so a type change cannot half-apply across one drag.
	ZeroBinding heldBinding_ = ZeroBinding::None;

	/// HOW FAR THE HANDLE IS FROM THE POINTER WHEN IT IS GRABBED, so a drag moves it BY the hand
	/// rather than TO the hand.
	///
	/// `hitAt` takes a press anywhere within `kGrabRadius`, and the drag road maps the pointer
	/// straight onto the axes — so without this the offset between the press and the handle is spent
	/// all at once on the FIRST drag event, and the handle snaps to the pointer instead of following
	/// it. Measured before this existed: a press 12 px off centre followed by one pixel of hand
	/// movement moved the handle 12.73 px. The result was "twitchy".
	///
	/// `HarmScope::anchorNotchDx_` is the same field for the same reason, and its comment is where
	/// this rule was already written down.
	visage::Point grabOffset_ {};

	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<fx::drawn::Param> knobParams_;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
