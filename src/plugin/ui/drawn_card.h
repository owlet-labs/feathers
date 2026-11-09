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
#include "fx/drawn/drawn_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// THE DRAWN-CURVE FILTER'S CARD — the morph puck, the curve it produces, and the four globals.
//
// **IT DOES NOT EDIT BANDS**, by design. Ninety-six
// of this device's 103 controls are pole and zero positions, and they belong to the phase-4 editor;
// this card carries the seven that are not. tests/test_gui_widgets.cpp's reachability table records
// the other ninety-six as known gaps, each naming phase 4, and it is pinned BOTH ways — the entry
// fails the moment that editor makes them reachable and nobody updates it.
//
// THE PLANE IS THE MORPH, NOT A SPECTRUM, and that is the difference from `WaypointCard` worth
// stating plainly because the two cards look alike. There, X is frequency and a dot IS a filter
// state. Here, the plane's four CORNERS are the four frames and the puck's position is a bilinear
// weight over all four at once — X and Y mean nothing on their own, and no axis is a frequency. So
// the curve is drawn in a strip of its OWN above the plane rather than behind the puck: they share no
// axis, and a picture that put them on one would be claiming a relationship that does not exist.
// (WaypointCard carries the same warning about its two vertical scales, and it is the same trap.)
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Morph X / Morph Y — DRAGGING the puck, and each also has a knob.
//     **The knobs are not redundant.** A modulation route is armed by dragging a source onto a
//     `Knob`; a plane that is not a widget cannot accept one (`owltt_graph` and `harm_scope` re-ask
//     `Knob::mappable`'s question so their pictures can, and this card does not). Without the pair,
//     the two controls the design calls *"the morph, modulatable"* would be modulatable in the
//     descriptor and unroutable by hand — the honesty rule from the other side.
//   - Cutoff / Res / Drive / Wake — knobs. `Wake` was `Smooth`, drawn and DISABLED while nobody knew
//     what it should do; design §8 gave it a job — the zeros lag the poles while the puck moves — so
//     it answers the hand now and is claimed rather than recorded as a gap.
//   - Enabled — the square in the head, drawn and answered by THIS CARD. `FxCard` derives the
//     square's rectangle and nothing else.
//
// THE HEAD CARRIES ITS OCCUPANTS in the order a press is tested against them: the slot menu, the
// enable square, the expand button, and then the head itself, whose own verb is FOLD. The order is
// load-bearing — a button inside the head's rectangle is unclickable if the head is tested first.

namespace feathers::ui {

class DrawnCard final : public FxCard {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	DrawnCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	          Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	DrawnCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

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
	const device::Descriptor* descriptor() const override { return &fx::drawn::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	/// THE RESPONSE STRIP. Shorter than the plane because nothing in it is a target — it is read, not
	/// aimed at, and the height a picture needs is the height it needs to be legible rather than the
	/// height a hand needs to hit something.
	static constexpr float kCurveHeight = 62.0f;
	/// THE MORPH PLANE, and it is sized by the same rule OWLTT's graph established: a target you can
	/// see and cannot hit is worse than no target. One puck needs less room than the waypoint card's
	/// four dots, which is the whole of why this is 120 and that is 150.
	static constexpr float kPlaneHeight = 120.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr size_t kKnobsPerRow = 3;
	/// SIX CELLS IN TWO ROWS — Morph X, Morph Y, Cutoff, then Res, Drive, Wake. The count lives here
	/// because `layOut` and `openHeight` must agree about how many rows to reserve and only one of
	/// them can count; OWLTT's card records what happens when that number is written twice.
	static constexpr size_t kKnobCells = 6;

	static constexpr float kPuckRadius = 5.0f;
	/// HOW FAR INBOARD OF ITS CORNER A FRAME LABEL FLOATS, in both axes.
	///
	/// **THE NUMBER IS SET BY THE PUCK, not by taste.** The puck's centre reaches the plane's corner
	/// exactly — `morphX` and `morphY` are clamped to [0, 1] and the mapping is the full box — so at
	/// each extreme there is a `kPuckRadius` disc of ink sitting ON a corner. A label the puck covers
	/// at exactly the moment you are looking at that corner is worse than no label, so the label's
	/// box is pushed diagonally inboard until it clears that disc: the nearest point of the box is
	/// `sqrt(2) * kFrameLabelInset` from the corner, which at 8 is 11.3px against a 5px puck. The
	/// clearance is asserted rather than eyeballed — see "the drawn card names its four frames".
	static constexpr float kFrameLabelInset = 8.0f;
	/// THE LABEL'S TYPE SIZE. `Knob::kHeight`'s own label size, because that is this card's smallest
	/// non-interactive ink and a frame marker is a hint rather than a control.
	static constexpr float kFrameLabelSize = 8.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	/// Fold the card away to its head. Tells the LANE, because a fold changes this card's height and
	/// the lane places cards by asking for it — `redraw()` alone repaints at the size it already had.
	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	[[nodiscard]] visage::Bounds headBounds() const;
	/// EMPTY WHILE THERE IS NO PANEL TO OPEN. An empty rectangle contains no point, so the press falls
	/// through to the fold without a second branch asking whether there is one — and the button cannot
	/// appear a moment before the phase-4 view it promises.
	[[nodiscard]] visage::Bounds curveBounds() const;
	[[nodiscard]] visage::Bounds planeBounds() const;
	/// Where the puck sits right now, in card coordinates.
	[[nodiscard]] visage::Point puckCentre() const;

	/// WHAT THE FRAME AT A CORNER IS CALLED ON THIS CARD — `"F"` and the frame's numeral, composed
	/// from `fx::drawn::kFrameNumerals`, which the expanded editor's tabs also compose their
	/// `"Frame N"` from. One table, two prefixes, so the two surfaces cannot name one frame twice.
	///
	/// **THE `F` IS NOT DECORATION.** A bare `1` in the corner of a plane whose axes both run 0 to 1
	/// is indistinguishable from an axis tick, and that is the one misreading a corner marker can
	/// afford least — it would say the opposite of what it is for.
	[[nodiscard]] static std::string frameLabelText(fx::drawn::Corner corner);
	/// The rectangle that label occupies, in card coordinates. Inboard of the corner in both axes;
	/// see `kFrameLabelInset` for what sets the distance.
	[[nodiscard]] visage::Bounds frameLabelBounds(fx::drawn::Corner corner) const;

	/// WHAT THIS CARD CLAIMS TO REACH — the list the reachability guard takes at its word, and every
	/// entry has a case behind it. `WaypointCard`'s comment records what that list is worth without
	/// them: it claimed `Enabled` while no hand could reach it, and the dead switch shipped.
	[[nodiscard]] std::vector<fx::drawn::Param> addressedParamsForTest() const;
	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

private:
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt — one line, so the toggle, the
	/// gesture that records the undo step, and the redraw all live once in the base.
	device::Modulatable* enableModulatable() override;

	[[nodiscard]] DeviceControl& control(fx::drawn::Param p) const;
	[[nodiscard]] fx::drawn::Settings settings() const;

	/// ASK FOR A FRAME WHEN THE CURVE THIS CARD DRAWS HAS MOVED.
	///
	/// **IT HAD NO SUCH ROAD.** On both Drawn and Harm, moving the cutoff knob did not move the drawn
	/// curve, though both moved with the xy pad and with real modulation in automod. That difference is
	/// the diagnosis: a drag on the pad repaints the CARD as a
	/// side-effect of the gesture, while a knob repaints only itself — so the picture moved for one
	/// input and not the other. Same defect `ShaperCard` had, found by the same report.
	bool updateContent() override;

private:
	/// WHAT THE CURVE ON SCREEN WAS DRAWN FROM — compared against `settings()` once a tick.
	mutable fx::drawn::Settings drawnSettings_ {};
	mutable bool haveDrawn_ = false;

public:
	void layOut();
	void drawCurve(visage::Canvas& canvas);
	void drawPlane(visage::Canvas& canvas);
	void drawFrameLabels(visage::Canvas& canvas);
	void dragPuck(visage::Point position);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	bool holdingPuck_ = false;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
