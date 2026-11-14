/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/control_face.h"

#include "plugin/ui/widget_registry.h"
#include "fx/owltt/owltt_types.h"
#include "plugin/ui/owltt_graph.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/pill_stack.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/vertical_fader.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

// OWLTT'S EXPANDED VIEW — the nine controls the card does not keep.
//
// IN USE, THE UI WAS CLUNKY: the graph is made larger and some of the knobs move to the expanded
// view. Ratio, Threshold and Up/Down stay on the card.
//
// THOSE THREE ARE ONE IDEA AND THAT IS WHY THEY ARE THE RIGHT THREE. Threshold, Ratio and Up/Down Skew
// are all *how the compression behaves, across all three bands* — and `owltt.cpp:648-663` computes
// `band + (global - 0.5)`, so they are literally macros over the per-band controls the graph draws.
// The card is therefore the picture plus the three global versions of what the picture shows, and
// Up/Down is the one of the three the picture CANNOT show, which is why it earns a circle rather
// than a handle. An earlier proposal put Blend and Output there too; that mixed compression shape
// with output staging, which is two ideas in one row.
//
// WHY THE GRAPH DID NOT COME WITH THEM. HARM's split moved the PICTURE to the panel because the card
// had no room for it. OWLTT's goes the other way: the picture stays and grows, because the card's
// problem was never that the graph was present, it was that the graph was 48px tall in a 410px card
// — MEASURED, 12% of the card's height for the only part that says what the device is doing, while a
// twelve-knob list took 73%. Two 16px bar hit-bands in a 48px box is 67% of the height claimed
// before a strip or a ring gets a pixel, which is the whole of the clunkiness: not the control count, but
// targets that cannot be aimed.
//
// SO THIS PANEL IS DELIBERATELY PLAIN. It is knobs in captioned groups and nothing else. The
// elaborate half of the original brief — draggable crossover boundaries, an analyzer behind the band
// columns — is NOT here and is not pretended at; it is a separate piece of work against a picture
// this panel does not yet own. What this delivers is the half that was asked for by name.

namespace feathers::ui {

class OwlttExpanded final : public ExpandedView, public ControlFace {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	OwlttExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	             Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	OwlttExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	visage::Point minimumSize() const override;
	/// Backlog EI: WIDE WITHOUT LIMIT, since the graph takes the width; the height is capped at the
	/// natural size plus one `kPad`, since the graph's height and the knob columns are fixed.
	visage::Point maximumSize() const override;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kGroupGap = 16.0f;
	static constexpr float kCaptionHeight = 14.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// FOUR CELLS, which is what a BAND needs: threshold, bandwidth, ratio and level in one row, so a
	/// band reads as a row rather than as four loose circles. The shorter groups leave cells empty
	/// rather than re-spacing — HARM's rule, and for its reason: a row that re-spaced itself would
	/// move controls under the pointer for no reason a player can see.
	static constexpr int kRowCells = 4;
	static constexpr float kCellWidth = 72.0f;
	static constexpr float kColumnGap = 20.0f;

	/// THE MODE PICKER — the crossover type, which was selectable NOWHERE.
	///
	/// The crossover type could not be selected anywhere. It is a dropdown, and need not be modulatable.
	/// Ten types, so a dropdown rather than a segmented strip; and not modulatable by design, which
	/// matches what it IS — a choice of filter topology is not a
	/// quantity to sweep, the same argument that took host automation off the engine.
	static constexpr float kModeWidth = 108.0f;

	/// THE PANEL'S OWN GRAPH — the same class the card holds, twice the height: the pop-out carries a
	/// larger version of the graph.
	///
	/// TWICE, BECAUSE THAT IS WHAT THE HEIGHT BUYS. On the card at 160 the two 16px bar hit-bands take
	/// 20% of the plot; at 320 they take 10%, and the threshold sweep gets twice the resolution again.
	/// The panel is not short of space and the picture is the reason a hand opened it.
	/// MEASURED AND CUT BACK. At 320 with the panel spanning four columns the plot came out 1212x320
	/// — the full width of the editor — while the twelve knobs occupied 302px and left 938px of empty
	/// canvas beside them: the graph had grown too big and the knobs were clustered together with lots
	/// of blank space.
	///
	/// THE BLANK SPACE WAS NOT A SPACING BUG, it was a leftover: the body spanned four columns because
	/// the panel USED to have four groups, and three of them left when the per-band knobs did. The
	/// width is derived from the columns actually occupied now, so a group leaving takes its column
	/// with it.
	///
	/// 220 RATHER THAN 320 because the height only ever bought aiming, and it has bought enough: the
	/// two 16px bar hit-bands are 15% of the plot here against 20% on the card and 67% at the original
	/// 48. Past that it is spending the panel's whole vertical budget on a picture that is not getting
	/// any easier to hit.
	static constexpr float kGraphHeight = 220.0f;

	/// A WIDER PANEL, so the graph has room and the bands can sit under it. The editor is 1240 logical pixels; this asks for
	/// well under that and gains the whole of it for the plot.
	static constexpr float kBandCells = 2;
	static constexpr float kSwitchHeight = 20.0f;

	/// ONE GROUP, DECLARED. The constructor builds from these, the layout walks them and
	/// `minimumSize` counts them, so a control moving between groups is one line and the height
	/// follows it — HARM's rule, and the reason its panel could not disagree with its own extent.
	struct KnobEntry {
		fx::owltt::Param param;
		const char* label;
		/// **A SET OF THREE, ONE PER BAND, AS A SLIDER BANK (backlog NK)** — slider banks spread to more cards and windows,
		/// and OWLTT is a prime candidate because it has several sets of three band controls. Set, the
		/// entry is a `PillStack` of amount-only pills over these three (low, mid, high) in ONE cell, titled `label`, and
		/// `param` is the first of them; `kCount` throughout is an ordinary knob.
		std::array<fx::owltt::Param, 3> bands { fx::owltt::Param::kCount, fx::owltt::Param::kCount,
			                                    fx::owltt::Param::kCount };
		[[nodiscard]] constexpr bool isStack() const { return bands[0] != fx::owltt::Param::kCount; }
	};
	struct KnobGroup {
		const char* caption;
		std::span<const KnobEntry> knobs;
		/// WHICH COLUMN THIS GROUP SITS IN. Declared rather than computed by a wrapping rule, because
		/// the grouping is meaningful: column 0 is the three BANDS, column 1 is everything global. A
		/// height-based wrap would put a band in with the output staging the day a knob moved.
		///
		/// TWO COLUMNS BECAUSE ONE DOES NOT FIT. The panel carries all 24 knobs now, and the overlay
		/// CLAMPS its content rather than scrolling it (`expanded_overlay.cpp:169`) — so a panel
		/// taller than the editor does not get a scrollbar, it gets its bottom groups silently cut
		/// off. That is this card's recurring bug in a new place: drawn where it cannot be reached.
		int column;
	};

	static std::span<const KnobGroup> groups();

	/// **THE ENABLE SQUARE AND ITS "Active" CAPTION ARE GONE FROM THIS BODY** (2026-09-26): the window
	/// header carries one enable chip for every device, beside the device's name — which is the subject
	/// that was missing here (the active toggle had no label in the expanded view), in the
	/// arrangement the card's head already had.

	/// The crossover-type picker, in the modal row.
	visage::Bounds modeBounds() const;
	/// THE L/R-M/S CHIP (backlog LQ), in the modal row just left of the crossover picker: it is the other
	/// choice about how the whole device is built (what each band's two channels ARE), so it sits with the first.
	visage::Bounds stereoBounds() const;
	static constexpr float kChipWidth = 36.0f;
	[[nodiscard]] SwitchChip* stereoForTest() const { return stereo_.get(); }
	[[nodiscard]] VerticalFader* driveForTest() const { return drive_.get(); }
	/// What it currently reads, from the same names the card's folded head prints.
	std::string modeText() const;
	/// Open it. Public so a case can drive the choice without synthesising a platform menu.
	void showModeMenu();
	/// Choose by index into `kModeNames`, which is what the menu's callback does.
	void chooseMode(size_t index);

	//-----------//
	// For tests //
	//-----------//

	/// EVERY CONTROL THIS SURFACE PUTS A HAND ON — the same question the card answers, so the
	/// reachability invariant can take a union over the two without either knowing about the other.
	std::vector<fx::owltt::Param> addressedParamsForTest() const;

	/// HOW MANY KNOBS THIS PANEL BUILDS — from the declared groups, so a case cannot assert a number
	/// that has quietly stopped being true.
	static size_t knobCount();
	/// THE CELLS THE GRID PLACES — knobs, mode cells and slider banks. +Soft Clip, backlog EL (2026-09-16); +Link, backlog
	/// LQ; +4 Width, backlog LR; the three band Widths one bank, backlog NK (18 -> 16).
	static constexpr size_t kPanelKnobCount = 16;
	/// The slider banks (backlog NK), in declaration order.
	[[nodiscard]] const std::vector<std::unique_ptr<PillStack>>& stacksForTest() const { return stacks_; }
	/// WHICH CONTROLS A BANK'S PILLS ARE, top to bottom — recorded as it was built.
	[[nodiscard]] const std::vector<std::array<fx::owltt::Param, 3>>& stackParamsForTest() const { return stackParams_; }
	/// THE LOOKAHEAD (backlog ND): a box with its menu, in the modal row left of the L/R-M/S chip.
	[[nodiscard]] ValueBox* lookaheadForTest() const { return lookahead_.get(); }
	[[nodiscard]] SwitchChip* keyTrackForTest() const { return keyTrack_.get(); }
	visage::Bounds lookaheadBounds() const;
	/// Key Track's chip, left of the lookahead box in the modal row.
	visage::Bounds keyTrackBounds() const;
	/// Wide enough for "Key Track", the Rack's own words for the same switch.
	static constexpr float kKeyTrackWidth = 64.0f;
	static constexpr float kLookaheadWidth = 112.0f;

	/// WHICH CONTROL A KNOB WAS BUILT FOR. The constructor records it as it builds; this is what lets
	/// a case ask "is any knob here a duplicate of the graph" without naming the knobs.
	fx::owltt::Param paramOfKnobForTest(const Knob* knob) const;

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// EVERY CELL THE LAYOUT PLACED, both kinds. A case checking that the layout fits wants what was
	/// PLACED, not what happened to be a `Knob` — the two diverged the day the zoned controls stopped
	/// being knobs, and a case walking `knobs_` then silently stopped checking those cells.
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }
	/// Character's and Vibe's mode cells (backlog IW), in that order.
	[[nodiscard]] const std::vector<std::unique_ptr<ModeValueCell>>& modeCellsForTest() const { return modeCells_; }
	[[nodiscard]] const std::vector<std::unique_ptr<ZoneCoordCell>>& coordsForTest() const {
		return coords_;
	}
	const std::vector<visage::Bounds>& captionBoundsForTest() const { return captionBounds_; }
	OwlttGraph* graph() const noexcept { return graph_.get(); }

	/// Forwarded to this panel's own graph. See `OwlttGraph::update`.
	bool updateContent() override;
	visage::Bounds graphBounds() const;
	/// THE DRIVE BAR (backlog NB): left of the graph, as tall as it; the graph gives up the width.
	visage::Bounds driveBounds() const;
	static constexpr float kDriveWidth = 28.0f;
	static constexpr float kDriveGap = 8.0f;
	float modalBottom() const;
	/// The x of a column's left edge, and how wide a column is — asked by the layout and the cases.
	float columnX(int column) const;
	float columnWidth(int column) const;
	float bodyWidth() const;

private:
	DeviceControl& control(fx::owltt::Param param) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::owltt::Param::kCount)>
	    controls_ {};
	std::vector<std::unique_ptr<Knob>> knobs_;
	/// THE ZONED CONTROLS, WHICH ARE NO LONGER KNOBS. A `ZoneCoordCell` is two stacked boxes and a
	/// zone dropdown, not
	/// a dial, so it cannot live in `knobs_` — and that is the structural cost of the premise
	/// correction rather than an inconvenience to route around. See `zone_coord_box.h`.
	std::vector<std::unique_ptr<ZoneCoordCell>> coords_;
	/// CHARACTER'S AND VIBE'S MODE CELLS (backlog IW) — the eight behaviours then Free, and the unbounded value.
	std::vector<std::unique_ptr<ModeValueCell>> modeCells_;
	std::vector<fx::owltt::Param> coordParams_;
	/// EVERY CELL IN DRAW ORDER, knobs and coordinate boxes alike. The layout walks THIS rather than
	/// `knobs_`, because the grid's cells are now two different kinds of widget and the walk has to
	/// stay in step with `groups()` regardless of which kind each one is.
	std::vector<visage::Frame*> cells_;
	/// CONTROLS A WIDGET REACHES THAT ARE NOT ITS OWN PARAMETER — today, the phase offset inside each
	/// the retired `ZonePhaseKnob`. Recorded AS THE WIDGET IS BUILT, in the branch that knows which
	/// bound, so the reachability answer is a record of what was constructed rather than a list kept
	/// beside it. `knobParams_` cannot carry these: it is 1:1 with `knobs_` and `paramOfKnobForTest`
	/// reads it positionally.
	std::vector<fx::owltt::Param> extraReached_;
	std::vector<fx::owltt::Param> knobParams_;
	std::unique_ptr<OwlttGraph> graph_;
	std::vector<visage::Bounds> captionBounds_;
	std::unique_ptr<SwitchChip> stereo_;
	/// The grid's two-state controls (the soft clipper), seated as cells in place of a dial.
	std::vector<std::unique_ptr<SwitchChip>> chips_;
	/// BACKLOG NB: the Drive, a bar left of the graph — the card's, so the window stays a superset.
	std::unique_ptr<VerticalFader> drive_;
	/// BACKLOG NK: the band slider banks, each a cell of the grid, and the three controls each one's pills are.
	std::vector<std::unique_ptr<PillStack>> stacks_;
	std::vector<std::array<fx::owltt::Param, 3>> stackParams_;
	/// BACKLOG ND: the Lookahead's box.
	std::unique_ptr<ValueBox> lookahead_;
	std::unique_ptr<SwitchChip> keyTrack_;
};

} // namespace feathers::ui
