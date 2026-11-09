/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/fader_pill.h"
#include "plugin/ui/shaper_plot.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/widget_registry.h"
#include "fx/shaper/shaper_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/mode_value_cell.h"

#include <functional>
#include <memory>
#include <vector>

// THE SHAPER'S CARD — the curve, and the four controls that shape and play it.
//
// THE CARD IS THE GAMMA-ZERO LINE. It has no Gamma control, so a Shaper edited only from here stays
// at gamma 0 — which is exactly the state in which Colour's zone names mean what they say and zone 6
// is the Oxford inflator. That is not a limitation dressed up: it is the more USEFUL half of the
// device, because it is the half with named places in it. The panel's pad is where the field stops
// having names and starts having territory.
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word for it and the cases
// behind it are what stop the word being a wish:
//   - Colour — a `ui::ZoneCoordCell`: one unbounded box with its zone dropdown. The device
//     declares the region names and gets the coordinate readout, the wrap and the carry for nothing.
//   - Knee — an ordinary knob. It was zoned once; its eight steps are a monotone drive ladder
//     (measured 3.3% to 23.3% THD), so it is a bounded quantity and keeps the dial that says so.
//   - Drive, Threshold (once Mix), HPF, LPF — ordinary knobs, and modulation
//     destinations. The crossfade Mix (backlog FA) is the window's.
//   - Enabled — the square in the head, drawn and answered HERE. `FxCard` derives the rectangle and
//     nothing else; a card that does not draw it and does not test a press against it has a control
//     the device declares and no hand can reach, which is what the Waypoint card shipped with.
//   - Gamma, Harmonics and the five extras switches are NOT on this card and are not pretended at.
//     They live on the panel, and `expandBounds` is the way to them.
//
// THE HEAD CARRIES FOUR OCCUPANTS, in the order a press is tested against them: the slot menu, the
// enable square, the expand button, then the head itself, whose own verb is FOLD. The order is
// load-bearing for the first three — a button inside the head's rectangle is unclickable if the head
// is tested first — and it is the order every sibling card already uses.

namespace feathers::ui {

class ShaperCard final : public FxCard {
public:
	ShaperCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	           Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	ShaperCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// THE BODY. The surface, the border and the name are `FxCard::draw`'s — see its note on why the
	/// standardized header is the default rather than a convention each card is trusted to follow.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	/// WHETHER THIS DEVICE IS IN THE SIGNAL — the head's switch asks, and `FxCard` draws it. The
	/// drawing was this card's until it was seven identical copies; only the READ is per-card,
	/// because only the card knows its own control road.
	bool headEnabled() const override;

	/// ASK FOR A FRAME WHEN THE CURVE THIS CARD DRAWS HAS MOVED.
	///
	/// **IT HAD NO SUCH ROAD AND THE CURVE SIMPLY DID NOT REDRAW.** Every other card that draws a
	/// picture of its settings has one — `OwlttCard`,
	/// `AutomodCard` — and this one relied on something else in the tree happening to repaint it,
	/// which is not a mechanism, it is a coincidence.
	bool updateContent() override;
	void resized() override;
	float laneHeightAt(float) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	const device::Descriptor* descriptor() const override { return &fx::shaper::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	/// TALL ENOUGH TO READ. OWLTT's graph is the precedent and its lesson is measured there: a picture
	/// too short to aim at is what "clunky" turned out to mean. Nothing on this plot is DRAGGED, so it
	/// needs less than OWLTT's or Waypoint's — but a waveshaper's curve has fine structure (the folders
	/// especially) and a 48px box turns a wavefolder into a smudge.
	/// 116 until the pills took a second row (2026-11-09): the 24px that row costs came out of the plot, because the
	/// card has to stay inside one lane unit in the rack (`test_rack_editor`'s KQ case) and had 8px to spare.
	static constexpr float kPlotHeight = 92.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// FOUR CELLS IN ONE ROW: Knee, Colour, Drive, Mix. Counted ONCE, here, because `layOut` and
	/// `openHeight` must agree about how many rows to reserve and only one of them can count — OWLTT's
	/// card records what happens when that number is written twice.
	/// **FIVE CELLS ACROSS TWO ROWS: Knee, Colour, Drive, Mix — then Aggression beneath Mix.**
	/// Counted ONCE, here, because `layOut`, `openHeight` and `extraBounds` must all agree about how
	/// many rows to reserve and only one of them can count — OWLTT's card records what happens when
	/// that number is written twice.
	/// **EIGHT SINCE BACKLOG FA**: the input HPF and LPF fill the second row's two empty columns.
	static constexpr size_t kKnobCells = 8;
	/// How many share the TOP row. The row's cell width is a function of THIS, not of the total: a
	/// five-wide top row would squeeze four knobs for a fifth that is not in it.
	static constexpr size_t kTopRowCells = 4;
	/// Where the second row's cells sit horizontally — Trim under Drive, Aggression under Threshold. Both
	/// are level controls and both belong under the level halves of the row above.
	static constexpr size_t kTrimColumn = 2;
	static constexpr size_t kAggressionColumn = 3;
	/// **THE WHOLE SECOND ROW, BY COLUMN, in `kCardKnobs` order** — Aggression, Trim, HPF, LPF.
	static constexpr std::array<size_t, 4> kSecondRowColumns { kAggressionColumn, kTrimColumn, 0, 1 };
	static_assert(kSecondRowColumns.size() == kKnobCells - kTopRowCells);
	/// The vertical the second row adds, spent by `openHeight` and by `extraBounds` alike.
	[[nodiscard]] static constexpr float secondRowHeight() {
		return Knob::columnHeight(kKnobDiameter) + kRowGap;
	}

	[[nodiscard]] static float openHeight();

	/// THE RECTANGLE OF EXTRAS PILL `i`, in the device's declaration order.
	///
	/// THE ORDER IS `fx::shaper::kExtraSwitches` AND NOT THIS FILE'S, so the card and the panel place
	/// the same five in the same sequence — a hand that learned them on one surface finds them in the
	/// same order on the other.
	[[nodiscard]] visage::Bounds extraBounds(size_t i) const;

	/// A CEILING, not a size: a pill takes its share of the row up to this. Raised from 46 with the second row (the
	/// widest caption, "Integrator", needs more than 46 with its margin).
	static constexpr float kExtraWidth = 72.0f;
	/// THE PILLS IN ROWS OF THREE — two rows for the six (`openHeight` and `extraBounds` both read these).
	static constexpr size_t kExtrasPerRow = 3;
	static constexpr float kExtraRows = 2.0f;
	static constexpr float kExtraGap = 4.0f;
	static constexpr float kExtraHeight = 20.0f;
	/// THE SIX EXTRAS AS FADER PILLS (backlog IK) — each an amount and its power.
	std::vector<std::unique_ptr<FaderPill>> pills_;
	/// **POLY (backlog OM): one shaper per voice, or one on the bus.** A two-state chip at the right end of the
	/// extras row, where both states are the choice (CLAUDE.md's multivalue rule). Its `DeviceControl` greys it
	/// with the host's reason where a per-voice road cannot exist (inside a Rack or a site).
	std::unique_ptr<SwitchChip> poly_;
	static constexpr float kPolyWidth = 40.0f;
	[[nodiscard]] visage::Bounds polyBounds() const;
	[[nodiscard]] const SwitchChip* polyForTest() const { return poly_.get(); }
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	[[nodiscard]] visage::Bounds headBounds() const;
	[[nodiscard]] visage::Bounds plotBounds() const;

	/// EVERY CONTROL THIS CARD REACHES — the knobs it actually built, plus the one it reaches by
	/// GESTURE rather than by widget.
	///
	/// THE KNOBS ARE ASKED OF THE WIDGET LIST rather than restated, because a list written by hand
	/// beside the constructor drifts from it. THE ENABLE IS ADDED BY HAND because there is no widget
	/// to ask: it is a rectangle in the head that this card draws and this card's hit test answers,
	/// and a gesture is invisible to a widget walk. That is exactly the shape the reachability guard
	/// exists for — the Waypoint card declared an enable, drew nothing, tested nothing, and was
	/// PERMANENTLY SILENT because `Enabled` defaults to 0.
	[[nodiscard]] std::vector<fx::shaper::Param> addressedParamsForTest() const {
		std::vector<fx::shaper::Param> out = knobParams_;
		// AND BOTH HALVES OF EVERY COORDINATE BOX. The card reaches Colour AND its coarse companion
		// now — the box is two editors, not a knob with a gesture — and a surface that claimed only
		// the half it used to have would understate what a hand can touch here.
		out.insert(out.end(), coordParams_.begin(), coordParams_.end());
		out.push_back(fx::shaper::Param::Enabled);
		// AND THE FIVE EXTRAS PILLS, WHICH ARE GESTURES TOO — drawn by `draw` and answered by this
		// card's hit test, so no widget walk can find them.
		//
		// **OMITTING THESE MADE THE SWEEP PASS BY LYING, WHICH IS WORSE THAN FAILING.** They were
		// added to the drawing and the hit test first and to this list second, and in between the
		// reachability case went GREEN — because it asks each surface what it reaches and the card
		// was still answering "not these five" while its hit test toggled them. A guard that takes a
		// surface at its word is only as good as the word; that is the same shape as the Waypoint
		// enable above, arriving from the opposite direction.
		out.insert(out.end(), fx::shaper::kExtraSwitches,
		           fx::shaper::kExtraSwitches + fx::shaper::kExtraSwitchCount);
		// AND EACH PILL'S POWER, which the same pill reaches by its click (backlog IK).
		out.insert(out.end(), std::begin(fx::shaper::kExtraPowers), std::end(fx::shaper::kExtraPowers));
		out.push_back(fx::shaper::Param::Poly);
		return out;
	}

	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	/// THE SIX FADER PILLS, in `kExtraSwitches` order (backlog IK).
	[[nodiscard]] const std::vector<std::unique_ptr<FaderPill>>& pillsForTest() const { return pills_; }

	/// EVERY CELL THE ROW PLACED, both kinds.
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }

	/// THE ZONED CONTROLS, WHICH ARE NO LONGER KNOBS — see `zone_coord_box.h`.
	/// COLOUR'S MODE CELL (backlog IW) — Inflate or Free, and the live mode's value.
	[[nodiscard]] const ModeValueCell* colourCellForTest() const { return colourCell_.get(); }
	[[nodiscard]] const std::vector<std::unique_ptr<ZoneCoordCell>>& coordsForTest() const {
		return coords_;
	}

	/// WHICH CONTROL A GIVEN KNOB WAS BUILT FOR — read off `knobParams_`, which the constructor fills
	/// in the one place that knows. A case that identified a knob by its POSITION in the row would
	/// pass unchanged the day the row is reordered, which is exactly when it should fail.
	[[nodiscard]] fx::shaper::Param paramOfKnobForTest(const Knob* knob) const {
		for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
			if (knobs_[i].get() == knob) {
				return knobParams_[i];
			}
		}
		return fx::shaper::Param::kCount;
	}

	/// WHAT THE FOLDED HEAD SAYS. A folded card is its head and nothing else, so the one fact it
	/// carries had better be the right one — and "which colour zone am I in" is the fact that says
	/// what this device sounds like.
	/// **WHETHER THIS DEVICE REACHES THE SOUND AT ALL**, which at Knee 0 it does not — see the
	/// definition for why the honest scope is every control but Knee. Public so a case can drive it
	/// without going through a settings round-trip.
	void showInSignal(bool live);

	/// WHAT `showInSignal` LAST SAID. The extras are PAINTED rather than child widgets, so their
	/// greying cannot live on a widget flag and has to be read at draw time.
	[[nodiscard]] bool inSignalForTest() const noexcept { return inSignal_; }

	[[nodiscard]] std::string foldedSummaryForTest() const;

	/// THE SETTINGS THIS CARD LAST ASKED FOR A FRAME WITH — see `ShaperExpanded::drawnSettingsForTest`.
	[[nodiscard]] fx::shaper::Settings drawnSettingsForTest() const noexcept { return drawnSettings_; }

private:
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt — one line, so the toggle, the
	/// gesture that records the undo step, and the redraw all live once in the base.
	device::Modulatable* enableModulatable() override;

	[[nodiscard]] DeviceControl& control(fx::shaper::Param p) const;
	[[nodiscard]] fx::shaper::Settings settings() const;
	void layOut();

	backplane::DeviceHost& host_;
	/// WHAT THE CURVE ON SCREEN WAS DRAWN FROM. Compared against `settings()` once a tick — one read,
	/// one comparison, a `redraw()` only when they differ. Copied from `OwlttGraph::update`, which is
	/// the shipped and measured version of this.
	mutable fx::shaper::Settings drawnSettings_ {};
	mutable bool haveDrawn_ = false;
	/// THE METER AS LAST DRAWN — a READING, so `drawnSettings_` cannot see it. See `update`.
	mutable float drawnDrive_ = 0.0f;
	mutable bool haveDrive_ = false;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<std::unique_ptr<ZoneCoordCell>> coords_;
	/// COLOUR'S MODE CELL (backlog IW) — Inflate or Free, and the live mode's value. See `makeColourModeCell`.
	std::unique_ptr<ModeValueCell> colourCell_;
	std::vector<fx::shaper::Param> coordParams_;
	/// EVERY CELL IN DRAW ORDER, both kinds. The row's layout walks this so it stays in step with
	/// `kCardKnobs` regardless of which kind each entry turned out to be.
	std::vector<visage::Frame*> cells_;
	std::vector<fx::shaper::Param> knobParams_;
	/// WHETHER THE DEVICE REACHES THE SOUND — false at Knee 0, where it is a true bypass.
	bool inSignal_ = true;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
