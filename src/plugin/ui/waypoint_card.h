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

#include "plugin/ui/widget_registry.h"
#include "fx/waypoint/waypoint_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>

// THE WAYPOINT FILTER'S CARD — four filter states in a plane, and a Morph that walks between them.
//
// THE AXES ARE A DECISION, NOT A DEFAULT. X is log frequency and **Y is RESONANCE**, decided
// 2026-08-26 for this reason: resonance is the only quantity all eight
// filter types have. Gain exists on Bell and the two shelves alone and the type-following macro is
// `None` on five types, so a Y coordinate meaning nothing on five types would be the
// present-and-inert control the honesty rule forbids — **arriving as geometry rather than as a knob,
// which is worse, because a knob can be undrawn and a position cannot.**
//
// THE PICTURE CARRIES TWO VERTICAL SCALES AND THAT IS THE COST, said here so nobody "fixes" it: the
// ghosted curves are drawn in dB, the dots sit on a resonance axis, and they share the frequency
// axis and NOTHING ELSE. **A dot is not a point on its own curve and must not be drawn as if it
// were** — no leader line to the curve, no snapping onto it.
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word and the cases behind it
// are what stop the word being a wish:
//   - Cutoff n / Resonance n — DRAGGING dot n. One gesture, four dots.
//   - Macro 1 n / Macro 2 n — the knob row, RETARGETED to the selected waypoint.
//   - Morph, Type, Keytrack, Poly — knobs of their own.
//   - Enabled — the square in the head, drawn and answered by THIS CARD. `FxCard` derives the
//     square's rectangle and nothing else; a card that does not draw it and does not test a press
//     against it has a control the device declares and no hand can reach, which is what this one
//     shipped with.
//
// THE HEAD CARRIES FOUR OCCUPANTS, in the order a press is tested against them: the slot menu, the
// enable square, the expand button, and then the head itself, whose own verb is FOLD. The order is
// load-bearing for the first three — a button inside the head's rectangle is unclickable if the head
// is tested first — and it is the order every sibling card already uses.

namespace feathers::ui {

class WaypointCard final : public FxCard {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	WaypointCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	             Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	WaypointCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// THE BODY. The surface, the border and the name are `FxCard::draw`'s — see its note on why the
	/// standardized header is the default rather than a convention each card is trusted to follow.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	/// WHETHER THIS DEVICE IS IN THE SIGNAL — the head's switch asks, and `FxCard` draws it. The
	/// drawing was this card's until it was seven identical copies; only the READ is per-card,
	/// because only the card knows its own control road.
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	const device::Descriptor* descriptor() const override { return &fx::waypoint::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	/// TALL ENOUGH TO AIM AT. OWLTT's graph is the precedent and the lesson is measured there: at 48px
	/// its two hit-bands claimed two thirds of the vertical pixels, and the result felt clunky —
	/// which turned out to mean targets that cannot be aimed rather than too many
	/// controls. Four draggable dots need the same room.
	static constexpr float kPlaneHeight = 150.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr size_t kKnobsPerRow = 3;

	/// **WIDER THAN ONE UNIT, BECAUSE THE LABELS NEED IT.** In a horizontal lane a card may grow to more
	/// than one unit WIDE, but its height is locked to one unit. Three cells at one unit gave
	/// each knob about 57px and `MACRO 1` overran its neighbour — so the fix goes into the axis that is
	/// FREE. Adding a row instead would spend the axis that is locked.
	float laneExtent(float cross) const override;
	/// SIX CELLS IN TWO ROWS — Morph, the selected waypoint's two macros, then Type, Keytrack and
	/// Poly. Twelve knobs EXIST (four device-wide plus a pair per waypoint) and six are visible; the
	/// count here is the LAYOUT's, because `layOut` and `laneHeight` must agree about how many rows to
	/// reserve and only one of them can count. OWLTT's card records what happens when that number is
	/// written in two places: a seventh knob drawn on top of the fourth.
	static constexpr size_t kKnobCells = 6;

	/// How tall the card is when open, and when folded — derived from the parts above, the way every
	/// sibling derives its own, so a row added to the layout moves the card instead of overflowing it.
	///
	/// FOLDED IS THE HEAD AND NOTHING ELSE, and it is `FxCard::kSlotHeadHeight` rather than a local
	/// 24: this card measures its plane from that constant already, so a second spelling would be a
	/// second thing to keep in step.
	[[nodiscard]] static float openHeight();
	/// THE PLANE'S HEIGHT AT THIS CARD'S SIZE: `kPlaneHeight`, less whatever the body is short of the open height
	/// (backlog KQ's sweep) — the rack's lane grants one unit (336) and the second knob row was clipped by 16px.
	[[nodiscard]] float planeHeight() const;
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	/// Fold the card away to its head. Tells the LANE, because a fold changes this card's height and
	/// the lane places cards by asking for it — `redraw()` alone repaints at the size it already had,
	/// which is the whole of the bug where a fold hid the UI but left the card's size unchanged.
	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	/// THE HEAD, whose own verb is fold. Asked rather than recomputed, the rule this repo states about
	/// believing a layout.
	[[nodiscard]] visage::Bounds headBounds() const;

	/// THE WAY INTO THE PANEL, and EMPTY when there is nothing to open — `hasExpandedView` answers for
	/// what this build can CONSTRUCT rather than for what the device declares, so the button cannot
	/// appear a moment before the panel it promises exists. An empty rectangle contains no point, so
	/// the press falls through to the fold without a second branch asking whether there is a panel.

	/// WHERE A WAYPOINT'S DOT SITS, in this card's coordinates.
	[[nodiscard]] visage::Point dotCentre(size_t waypoint) const;
	[[nodiscard]] visage::Bounds planeBounds() const;

	/// WHICH WAYPOINT THE KNOB ROW IS POINTED AT.
	[[nodiscard]] size_t selected() const noexcept { return selected_; }
	void select(size_t waypoint);

	/// WHICH DOT A POINT LANDS ON, or kWaypoints for none — the hit test the press uses, exposed so a
	/// case can ask it directly rather than inferring reach from a value that moved.
	[[nodiscard]] size_t dotAt(visage::Point position) const;

	/// EVERY CONTROL THIS CARD REACHES, one entry per control and a comment per GESTURE. The
	/// reachability guard takes this at its word; the cases that assert the value each gesture writes
	/// are what stop it being a wish.
	[[nodiscard]] std::vector<fx::waypoint::Param> addressedParamsForTest() const;

	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	[[nodiscard]] ValueBox* typeForTest() const { return type_.get(); }
	[[nodiscard]] SwitchChip* keytrackForTest() const { return keytrack_.get(); }
	[[nodiscard]] SwitchChip* polyForTest() const { return poly_.get(); }

private:
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt — one line, so the toggle, the
	/// gesture that records the undo step, and the redraw all live once in the base.
	device::Modulatable* enableModulatable() override;

	[[nodiscard]] DeviceControl& control(fx::waypoint::Param p) const;
	[[nodiscard]] fx::waypoint::Settings settings() const;

	/// ASK FOR A FRAME WHEN THE PICTURE THIS CARD DRAWS HAS MOVED.
	///
	/// **THE CARD HAD NO SUCH ROAD, SO A KNOB MOVED THE VALUE AND NOT THE CURVE.** Moving the cutoff
	/// knob did not move the drawn curve, though both moved with the XY pad and with modulation.
	/// **That difference IS the diagnosis**: a drag on a pad repaints the card as
	/// a side effect of the gesture, and modulation arrives through a road that already redraws — while
	/// a knob repaints only itself. So the picture followed two of its three inputs.
	bool updateContent() override;

private:
	/// WHAT THE PICTURE ON SCREEN WAS DRAWN FROM — compared against `settings()` once a tick.
	mutable fx::waypoint::Settings drawnSettings_ {};
	mutable bool haveDrawn_ = false;

public:
	void layOut();
	/// Show the selected waypoint's macro knobs and hide the rest.
	void retarget();
	void drawPlane(visage::Canvas& canvas);
	void dragDot(visage::Point position);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	size_t selected_ = 0;
	/// WHICH DOT THE HAND HAS, or kWaypoints for none.
	size_t held_ = fx::waypoint::kWaypoints;

	/// HOW FAR THE DOT IS FROM THE POINTER WHEN IT IS GRABBED, so a drag moves it BY the hand rather
	/// than TO the hand. `dotAt` takes a press anywhere within `kGrabRadius`, and `dragDot` maps the
	/// pointer straight onto the plane — so without this the press offset is spent all at once on the
	/// first drag event. Measured before it existed: a press 10 px off centre followed by one pixel
	/// of hand movement moved the dot 10.73 px.
	///
	/// `DrawnExpanded::grabOffset_` is the same field for the same reason and
	/// `HarmScope::anchorNotchDx_` is where the rule was first written down.
	visage::Point grabOffset_ {};

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	/// The three device-wide choices, which are not knobs: the filter type's list, and two chips.
	std::unique_ptr<ValueBox> type_;
	std::unique_ptr<SwitchChip> keytrack_;
	std::unique_ptr<SwitchChip> poly_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
