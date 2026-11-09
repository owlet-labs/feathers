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

#include "plugin/ui/widget_registry.h"
#include "fx/shaper/shaper_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/gamma_box.h"
#include "plugin/ui/shaper_pad.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/switch_chip.h"

#include <array>
#include <span>
#include <memory>
#include <string>
#include <vector>

// THE SHAPER'S PANEL — the colour field as a plane, and the switches the firmware kept behind a
// modifier key.
//
// IT IS THE COMPLETE DEVICE. A standing rule: the expanded view carries the COMPLETE device with full
// controls, so the user never has to toggle between card and window to edit every param. All twelve controls are here — the card's four, plus Gamma, Harmonics and the five
// extras.
//
// THE PAD IS THE POINT OF OPENING IT. See `shaper_pad.h`: Colour keeps its zone knob and gains an
// axis, because MEASURED, the field travels 13.9 across gamma at fixed colour, which is more than six
// of the eight colour zones travel across their own width. That dimension existed in the firmware and
// was reachable only by pushing and twisting an encoder.
//
// THE EXTRAS ARE FIVE SWITCHES AND NO KNOBS, WHICH IS A DESIGN AND NOT AN OMISSION.
//
// Every extra has a depth, a rate or a ratio, and every one of those is read off the colour field at
// the current position — sub interval and stride and comb depth and rotation amount out of
// `kExtrasBank`, slew intensity out of its own triangle. Giving each an amount knob would be nine
// more controls AND would break the single claim the device makes: that the position IS the sound.
//
// SO WHAT REPLACES THE KNOBS IS A READOUT. The panel prints what the field is currently saying —
// interval, stride, depth, rotation, slew, hysteresis — with each line live or greyed according to
// its switch. That is the honest presentation of an amount you do not control: show it, say whether
// it is being heard, and do not draw a knob that would have to lie about who is in charge. It also
// makes the sparseness legible, which is the one thing a player cannot otherwise discover: a switch
// that appears to do nothing is usually a field that is asking for nothing THERE.

namespace feathers::ui {

class ShaperExpanded final : public ExpandedView {
public:
	ShaperExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	               Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	ShaperExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// ASK FOR A FRAME WHEN THE CURVE THIS PANEL DRAWS HAS MOVED — the card's `update()`, on the
	/// panel, for the reason the card got one.
	///
	/// THE SYMPTOM: the table display did not always update with puck and knob changes in the
	/// window view. The card had shown the identical symptom the day before — the shaper curve not
	/// redrawing — and was given this road; the panel draws the same curve from the same settings
	/// and had only the state channel, which reaches it by SUBSCRIPTION and a dirty flag drained once
	/// a tick. This road asks a different question every tick — "is the screen showing what I read?"
	/// — and depends on neither. Every other surface here that draws a picture of its settings has
	/// one (`OwlttGraph`, `AutomodStrip`, `ShaperCard`); this was the last that did not.
	///
	/// MEASURED BEFORE IT WAS ADDED, and stated so the fix is not mistaken for a diagnosis: on the
	/// private display the state channel alone carried a pad write and a knob drag to this plot every
	/// time — in-process, with a real pointer, and through the shipped .clap under guihost. The
	/// report is from Bitwig on a real GPU and could not be reproduced here. What this gives the
	/// panel is a road whose delivery a case can COUNT (`Gui::TickReport::deviceFrames`) and whose
	/// value a case can READ (`drawnSettingsForTest`), where the channel's could only be inferred.
	bool updateContent() override;

	/// WHETHER THIS DEVICE REACHES THE SOUND — false at Knee 0, where the shaper is a true bypass and
	/// consults no table. Everything but Knee greys; see `ShaperCard::showInSignal` for the full
	/// argument and for why Knee itself stays lit.
	void showInSignal(bool live);

	/// WHAT `showInSignal` LAST SAID — the extras are PAINTED, so their greying is read at draw time
	/// rather than carried on a widget flag.
	[[nodiscard]] bool inSignalForTest() const noexcept { return inSignal_; }
	/// THE SIX FADER PILLS, in `kExtraParams` order (backlog IK).
	[[nodiscard]] const std::vector<std::unique_ptr<FaderPill>>& pillsForTest() const { return pills_; }
	[[nodiscard]] const SwitchChip* polyForTest() const { return poly_.get(); }

	visage::Point minimumSize() const override;
	/// Backlog EI: CAPPED BOTH WAYS at the natural size plus one `kPad`. The pad, the plot and the
	/// extras are fixed; the knob rows spread their cells over a wider window, which moves nothing
	/// into the room.
	visage::Point maximumSize() const override;
	std::string title() const override;

	void modulationBound(ModState* mod, EditHistory* history) override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 14.0f;
	static constexpr float kRowGap = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr float kSwitchHeight = 20.0f;

	/// THE PAD AND THE PLOT SIT SIDE BY SIDE, and the pad is the wider of the two because it is the
	/// thing being aimed at. The plot is read, not touched.
	static constexpr float kPadWidth = 420.0f;
	static constexpr float kPadHeight = 300.0f;
	static constexpr float kPlotWidth = 300.0f;
	static constexpr float kPlotHeight = 190.0f;

	/// SIX CELLS IN ONE ROW: Knee, Colour, Gamma, Drive, Mix, Harmonics. Counted once — `layOut` and
	/// `minimumSize` must agree about how many rows to reserve and only one of them can count.
	/// **CELLS IN TOTAL, ACROSS TWO ROWS.** Eight sit in the top row; Aggression sits in a second row
	/// directly beneath Mix, by design. Two
	/// numbers because a row's CELL WIDTH is a function of how many share that row, and the array's
	/// length is a function of how many controls there are; conflating them is how a nine-wide row
	/// would have squeezed the top row for a knob that is not in it.
	static constexpr size_t kKnobCells = 17; // + HPF, LPF and the crossfade Mix (backlog FA)
	/// How many of those share the TOP row. `kKnobCells - kTopRowCells` is the second row's tenancy.
	static constexpr size_t kTopRowCells = 9; // + HPF and LPF at the right end (backlog FA)
	/// Where the second row's cells sit: Aggression under Mix (index 4 of the top row) and Trim under
	/// Drive (index 3). Both are level controls, under the two controls above that decide the same
	/// things — what reaches the curve, and how abruptly the curve takes over.
	static constexpr size_t kAggressionColumn = 4;
	/// **THE WHOLE SECOND ROW, BY COLUMN, in `kPanelKnobs` order** — Aggression, Trim, Cheby, Stair, Bloom,
	/// Sag, Loop. Seven columns since Rotate moved into the terrain box (backlog IJ): Trim and Aggression
	/// stay where they were placed, under Drive and Mix; the three memory stages sit together on the left
	/// (under Knee, Colour, Gamma) and the two read shapes on the right (under Emphasis and Hysteresis).
	/// **AND THE CROSSFADE MIX UNDER LPF, IN THE CORNER (backlog FA)** — the last thing the device does, at the
	/// end of the last row; the column under HPF stays empty.
	static constexpr std::array<size_t, 8> kSecondRowColumns { 4, 3, 5, 6, 0, 1, 2, 8 };
	static constexpr size_t kTrimColumn = 3;
	static constexpr float kCellWidth = 84.0f;

	/// THE FIVE EXTRAS, in their declared order.
	static constexpr size_t kExtraCount = fx::shaper::kExtraSwitchCount;
	static constexpr float kExtraWidth = 96.0f;
	static constexpr float kExtraGap = 8.0f;
	static constexpr float kPolyWidth = 44.0f;

	[[nodiscard]] visage::Bounds padBounds() const;
	[[nodiscard]] visage::Bounds plotBounds() const;
	[[nodiscard]] visage::Bounds readoutBounds() const;
	/// The rectangle of extras switch `i`, in declaration order.
	[[nodiscard]] visage::Bounds extraBounds(size_t i) const;

	/// WHICH CONTROL EACH EXTRAS SWITCH EDITS — **the DEVICE's table, not this panel's.** It moved to
	/// `shaper_types.h` when the card gained these switches too; an alias rather than a second array,
	/// so the two surfaces cannot come to disagree about which five they are or what order they sit
	/// in. See `fx::shaper::kExtraSwitches` for why it belongs beside the controls.
	static constexpr std::span<const fx::shaper::Param> kExtraParams { fx::shaper::kExtraSwitches };

	/// EVERY CONTROL THIS PANEL REACHES — the knobs it built, plus the pad's two, plus the enable and
	/// the five switches. The reachability guard takes this at its word; the cases that assert what
	/// each gesture WRITES are what stop it being a wish.
	[[nodiscard]] std::vector<fx::shaper::Param> addressedParamsForTest() const;

	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// EVERY CELL THE LAYOUT PLACED, both kinds. A case checking that the layout fits wants what was
	/// PLACED, not what happened to be a `Knob` — the two diverged the day the zoned controls stopped
	/// being knobs, and a case walking `knobs_` then silently stopped checking those cells.
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }
	/// COLOUR'S MODE CELL (backlog IW) — Inflate or Free, and the live mode's value.
	[[nodiscard]] const ModeValueCell* colourCellForTest() const { return colourCell_.get(); }
	[[nodiscard]] const std::vector<std::unique_ptr<ZoneCoordCell>>& coordsForTest() const {
		return coords_;
	}

	/// WHICH CONTROL A KNOB WAS BUILT FOR — off `knobParams_`, which the constructor fills where it
	/// knows. A case that identified a knob by its position in the row would pass unchanged the day
	/// the row is reordered, which is exactly when it should fail.
	[[nodiscard]] fx::shaper::Param paramOfKnobForTest(const Knob* knob) const {
		for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
			if (knobs_[i].get() == knob) {
				return knobParams_[i];
			}
		}
		return fx::shaper::Param::kCount;
	}

	[[nodiscard]] const ShaperPad* padForTest() const { return pad_.get(); }
	/// ROTATE, WHICH LIVES INSIDE THE TERRAIN BOX (backlog IJ) — not in `cellsForTest`, which is the knob rows.
	[[nodiscard]] const GammaBox* rotateForTest() const { return rotate_.get(); }

	/// HOW MANY TIMES THIS PANEL HAS DRAWN — counted BY `draw()`, so it is a count of frames visage
	/// actually painted, not of frames somebody asked for. A windowed case reads it around a write to
	/// say whether the plot was PAINTED, which no headless venue can say.
	[[nodiscard]] size_t drawsForTest() const noexcept { return draws_; }

	/// THE SETTINGS THIS PANEL LAST ASKED FOR A FRAME WITH — what `update()` compared and recorded.
	/// The plot is `deriveCurve` of four of these, so a case asserting the curve moved asserts on the
	/// values it moved TO, rather than on pixels it cannot see.
	[[nodiscard]] fx::shaper::Settings drawnSettingsForTest() const noexcept { return drawnSettings_; }

	/// WHAT THE FIELD READOUT SAYS RIGHT NOW, line by line. It is derived from the settings through
	/// `deriveCurve`, so a case can assert the panel prints the field's real answer rather than some
	/// text — the same argument every test seam in this device is written under.
	[[nodiscard]] std::vector<std::string> readoutLinesForTest() const;

private:
	[[nodiscard]] DeviceControl& control(fx::shaper::Param p) const;
	[[nodiscard]] fx::shaper::Settings settings() const;
	void layOut();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<GammaBox> gamma_;
	/// The plane's turn — a box rather than a knob, because it is unbounded. See `Param::Rotate`.
	std::unique_ptr<GammaBox> rotate_;
	std::vector<std::unique_ptr<ZoneCoordCell>> coords_;
	/// COLOUR'S MODE CELL (backlog IW) — see `makeColourModeCell`.
	std::unique_ptr<ModeValueCell> colourCell_;
	/// EVERY CELL IN DRAW ORDER, knobs and coordinate boxes alike — the row's layout walks this.
	std::vector<visage::Frame*> cells_;
	/// CONTROLS A WIDGET REACHES THAT ARE NOT ITS OWN PARAMETER — Colour's phase offset, which lives
	/// in the coordinate box beside it. Recorded as the widget is built; see OwlttExpanded for the
	/// same member and the reason it is not `knobParams_`.
	std::vector<fx::shaper::Param> extraReached_;
	/// THE SIX EXTRAS AS FADER PILLS (backlog IK) — each an amount and its power.
	std::vector<std::unique_ptr<FaderPill>> pills_;
	/// POLY (backlog OM) — the card's chip, after the extras: the window is a superset of the card.
	std::unique_ptr<SwitchChip> poly_;
	[[nodiscard]] visage::Bounds polyBounds() const;
	/// WHETHER THE DEVICE REACHES THE SOUND — false at Knee 0, where it is a true bypass.
	bool inSignal_ = true;
	std::vector<fx::shaper::Param> knobParams_;
	std::unique_ptr<ShaperPad> pad_;
	/// The pad's drag, held as one undo step (backlog JB).
	DeviceFieldsGesture padGesture_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	size_t draws_ = 0;
	/// WHAT THE CURVE ON SCREEN WAS ASKED TO BE DRAWN FROM — see `ShaperCard::drawnSettings_`, which
	/// this copies, including the reason it is recorded when the frame is ASKED FOR and not when it
	/// is painted: a headless case can then see the road settle, which a paint it cannot cause would
	/// keep from it.
	fx::shaper::Settings drawnSettings_ {};
	bool haveDrawn_ = false;
	/// THE METER AS LAST DRAWN — a READING, so `drawnSettings_` cannot see it. See `update`.
	float drawnDrive_ = 0.0f;
	bool haveDrive_ = false;
};

} // namespace feathers::ui
