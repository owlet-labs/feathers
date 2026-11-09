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

#include "backplane/device_host.h"
#include "fx/comp/comp_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/comp_plot.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// COMP'S CARD — a mode switch, the mode's knobs, and a meter column that shows what the gain computer
// is doing. `BusUtilCard` is the template; its header carries the reasoning this one inherits.
//
// **GATED-OUT CONTROLS ARE HIDDEN, NOT GREYED.** In Limit, Threshold/Ratio/Knee/Attack/Detect/Makeup/Mix
// change nothing (`comp_types.h`'s gate masks, the honesty rule), and Ceiling changes nothing in
// Compress — so each mode shows only its own knobs and the row closes up. The gate is asked of the
// descriptor exactly as `LfoCard::boxIsLive` asks it, and re-asked whenever the Mode field moves.
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Mode, Detect — a segmented control each (every choice on the face; CLAUDE.md's toggle rule).
//   - Drive (both modes, backlog MI), Threshold, Ratio, Knee, Attack, Release, Makeup, Mix, Ceiling — a knob each,
//     shown by mode;
//     Stereo Link (backlog LQ) — a knob, in both modes, closing each mode's grid.
//   - Stereo Mode (backlog LQ) — a `SwitchChip` (L/R | M/S) at the right end of the Detector row (backlog QG moved it
//     off the mode row, which needs the card's whole width for three names).| M/S) at the right end of the mode row, CLIP's
//     arrangement for its oversampling chip: a second choice about the whole device sits beside the first.
//   - ScHpf — a compact value box, whose nine-entry menu opens itself.
//   - Enabled — the square in the head, drawn by `FxCard` and answered here.
// The meter reaches nothing: it is a reading.

namespace feathers::ui {

class CompCard final : public FxCard {
public:
	CompCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	         Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	CompCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;
	~CompCard() override;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::comp::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// A KNOB CELL'S HEIGHT — the knob's own column at this face, not `Knob::kHeight`, which is the
	/// column of a 52 px face and would leave 14 px of nothing under each row.
	static constexpr float kCellHeight = Knob::columnHeight(kKnobDiameter);
	static constexpr float kSwitchHeight = 16.0f;
	/// THE GAP UNDER A ROW OF SWITCHES (backlog QG): 6, not the knob rows' 8 — the Topology row cost a row, and the standalone
	/// rack hands a card 336 px, which every control must stay inside (test_rack_editor's KQ).
	static constexpr float kSwitchGap = 6.0f;
	/// THREE TO A ROW, AND THREE ROWS: Compress has nine knobs (Drive and Link included), Limit four. The card's height is the
	/// taller mode's, so switching mode does not move every card below it.
	///
	/// **NOT FOUR, BECAUSE THE METER COLUMN TAKES A FIFTH.** Photographed at four in a 256 px lane the
	/// cells were 39 px and THRESH, RELEASE and MAKEUP ran into each other — Bus Utility's four fit
	/// because it has no column beside them.
	static constexpr size_t kKnobsPerRow = 3;
	static constexpr size_t kKnobRows = 3;
	/// THE METER COLUMN, on the right: three bars and their captions.
	static constexpr float kMeterWidth = 54.0f;
	static constexpr float kMeterGap = 8.0f;
	/// THE STEREO CHIP AT THE MODE ROW'S RIGHT END (backlog LQ) — CLIP's oversampling chip, at the width "M/S" needs.
	static constexpr float kChipWidth = 36.0f;
	/// THE TRANSFER PLOT (backlog QG): input dB across, output dB up, from `kPlotFloorDb` to 0 dBFS on both axes.
	static constexpr float kPlotHeight = 64.0f;
	static constexpr float kPlotFloorDb = -60.0f;
	/// THE FIELDS THE CURVE IS DRAWN FROM — `updateContent` compares them, so a turned knob redraws the plot.
	static constexpr size_t kCurveFields = 13;
	/// THE LANE CHIP (backlog QG) — the Delay's `FB FX` chip's words and size, for the Compander's lane.
	static constexpr const char* kLaneChipEmpty = "LANE +";
	static constexpr const char* kLaneChipPrefix = "LANE · ";
	static constexpr float kLaneChipWidth = 54.0f;
	static constexpr float kLaneChipHeight = 13.0f;
	static constexpr const char* kLaneChipTip
	    = "THE COMPANDER'S LANE: effects between the halves, hearing what the first half made of the signal. Opens the window.";

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds headBounds() const;
	[[nodiscard]] visage::Bounds meterBounds() const;

	/// ONE READING, BY NAME, or 0 when nothing is published — see `BusUtilCard::reading`.
	[[nodiscard]] float reading(fx::comp::Reading r) const;
	/// Is anything publishing at all? A card with no data draws no meter rather than a meter of zeros.
	[[nodiscard]] bool hasReadings() const;

	/// IS THIS CONTROL LIVE IN THE CURRENT MODE? The descriptor's gate, asked with the Mode field.
	[[nodiscard]] bool isLive(fx::comp::Param p) const;

	/// THE THREE BAR FRACTIONS THE METER DRAWS — `drawMeter` reads these and nothing else, so a case
	/// asserting them asserts the picture's numbers.
	struct MeterFractions {
		float in = 0.0f;
		float out = 0.0f;
		float gr = 0.0f;
	};
	[[nodiscard]] MeterFractions meterForTest() const { return meterFractions(); }
	/// **THE PLOT'S CURVE, BY VALUE** (backlog QG): the output level in dB the plot draws for an input level `xDb` —
	/// the device's static curve in the current mode, Drive, makeup and Mix included. `drawTransfer` reads this and
	/// nothing else, so a case asserting it asserts the picture's numbers.
	[[nodiscard]] float transferDb(float xDb) const;
	[[nodiscard]] visage::Bounds plotBounds() const { return plot_; }
	/// THE PLOT'S CURVES, BY VALUE — the net, and each half's (`comp_plot::Curves`).
	[[nodiscard]] comp_plot::Curves curves() const;
	[[nodiscard]] SegmentedControl* topologyForTest() const { return topology_.get(); }
	/// THE LANE CHIP: the frame (its press opens the window) and what it says.
	[[nodiscard]] visage::Frame& laneChipForTest() const;
	[[nodiscard]] std::string laneChipTextForTest() const { return laneChipText(); }

	[[nodiscard]] std::vector<fx::comp::Param> addressedParamsForTest() const;
	/// THE KNOB ON `p`, or null — so a case can ask whether it is shown rather than infer it.
	[[nodiscard]] Knob* knobForTest(fx::comp::Param p) const;
	[[nodiscard]] SegmentedControl* modeForTest() const { return mode_.get(); }
	[[nodiscard]] SegmentedControl* detectForTest() const { return detect_.get(); }
	[[nodiscard]] SegmentedControl* detectorForTest() const { return detector_.get(); }
	/// **"SIDECHAIN SILENT"** (backlog JO, the honesty rule): Detector is Side, shown, and nothing sounds on the plugin's Sidechain
	/// input (`Reading::SidechainSounding`). With no data published it is false: the card claims nothing it was not told.
	[[nodiscard]] bool sidechainSilent() const;
	[[nodiscard]] visage::Bounds sidechainNoteBounds() const { return sideNote_; }
	[[nodiscard]] ValueBox* scHpfForTest() const { return scHpf_.get(); }
	[[nodiscard]] SwitchChip* stereoForTest() const { return stereo_.get(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(fx::comp::Param p) const;
	[[nodiscard]] MeterFractions meterFractions() const;
	[[nodiscard]] int modeNow() const;

	[[nodiscard]] float contentRight() const;
	void layOut();
	void drawMeter(visage::Canvas& canvas);
	[[nodiscard]] comp_plot::Meter meter() const;
	void drawTransfer(visage::Canvas& canvas);
	[[nodiscard]] size_t laneDevices() const;
	[[nodiscard]] bool plotFits() const;
	[[nodiscard]] std::string laneChipText() const;
	[[nodiscard]] float field(fx::comp::Param p) const;
	[[nodiscard]] std::array<float, kCurveFields> curveFields() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	/// THE MODE THE WIDGETS WERE LAST LAID OUT FOR — compared once a tick, so an automated or undone
	/// mode change re-hides the right knobs without anyone telling the card.
	int laidOutMode_ = -1;
	/// WHAT THE METER ON SCREEN WAS DRAWN FROM — `updateContent`'s contract.
	float drawnIn_ = -1.0f;
	float drawnOut_ = -1.0f;
	float drawnGr_ = -1.0f;
	/// WHETHER THE METER ON SCREEN WAS DRAWN FROM READINGS AT ALL — see `updateContent`.
	bool drawnHadReadings_ = false;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<SegmentedControl> detect_;
	std::unique_ptr<SegmentedControl> detector_;
	/// WHERE "sidechain silent" DRAWS, beside the Detector control; laid out with it.
	visage::Bounds sideNote_ {};
	/// WHETHER THE LINE ON SCREEN WAS DRAWN AS SILENT — `updateContent`'s contract.
	bool drawnSilent_ = false;
	/// WHAT THE PLOT ON SCREEN WAS DRAWN FROM — `updateContent`'s contract, as for the meter.
	std::array<float, kCurveFields> drawnCurve_ {};
	visage::Bounds plot_ {};
	std::unique_ptr<ValueBox> scHpf_;
	std::unique_ptr<SwitchChip> stereo_;
	std::unique_ptr<SegmentedControl> topology_;
	class LaneChip;
	std::unique_ptr<LaneChip> laneChip_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
