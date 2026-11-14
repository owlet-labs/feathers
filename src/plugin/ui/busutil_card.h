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
#include "fx/busutil/busutil_meter.h"
#include "fx/busutil/busutil_types.h"
#include "plugin/ui/clip_led.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <vector>

// BUS UTILITY'S CARD — seven knobs and the meter that earns them a hand-written surface.
//
// **THE METER IS WHY THIS CARD EXISTS.** Eight controls fit the generic knob grid perfectly well, and
// the device shipped on it for one commit. What the grid cannot draw is a picture, and a utility is a
// device people insert to LOOK at: the level, the image, and whether anything went over.
//
// **IT IS A TAP AND NOT A MASTER METER.** A meter on the actual master may come later; this is a tap,
// so "master" terminology is loose here. It reads where the card
// sits in the chain.
//
// WHAT REACHES WHAT, because the reachability guard takes this card's word:
//   - Gain, Pan, Pan L, Pan R, Width, Invert L, Invert R — a knob each.
//   - Enabled — the square in the head, drawn by `FxCard` and answered here.
// The meter reaches nothing: it is a reading, not a control, and nothing on it is draggable.

namespace feathers::ui {

class BusUtilCard final : public FxCard {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	BusUtilCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	            Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	BusUtilCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	const device::Descriptor* descriptor() const override { return &fx::busutil::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// **FIVE KNOBS AND ONE CELL OF BUTTONS, IN TWO ROWS OF FOUR.** The count is the LAYOUT's and
	/// lives once — OWLTT's card records what a second spelling costs: a seventh knob drawn on top of
	/// the fourth.
	///
	/// THE POLARITY PAIR WAS TWO KNOBS AND IS NOW TWO BUTTONS SHARING ONE CELL: buttons, both fitting
	/// in the space of one knob. A two-state
	/// control drawn as a dial is a dial with two positions, which says "turn me" about something that
	/// only flips — and the pair cost two cells for one idea.
	static constexpr size_t kKnobsPerRow = 4;
	/// IN A HORIZONTAL LANE (the rack): its height is fixed and holds two rows, so the twelve cells go six across.
	static constexpr size_t kKnobsPerRowHorizontal = 6;
	static constexpr size_t kKnobCells = 7;
	/// WHICH CELL THE POLARITY PAIR OCCUPIES — straight after the knobs, so the row reads left to
	/// right without a hole in it.
	static constexpr size_t kPolarityCell = kKnobCells;
	/// **THE THIRD ROW (backlog DM, 2026-10-30): the Mid and Side trims, Monitor, Swap.** Knobs for the two levels, a
	/// value box for Monitor (a named list, so a press opens it as a menu and nothing is applied on the way past: the
	/// multivalue-toggle rule), and Swap as one button in its cell, drawn like the polarity pair.
	static constexpr size_t kMidCell = kPolarityCell + 1;
	static constexpr size_t kMonitorCell = kMidCell + 2;
	static constexpr size_t kSwapCell = kMonitorCell + 1;
	static constexpr size_t kCellCount = kSwapCell + 1;
	/// THE METER STRIP. Tall enough for two peak bars, the readouts beside them, and the LED.
	static constexpr float kMeterHeight = 88.0f;
	/// THE LED'S RADIUS AT ITS CORE. The indicator is meant to be PROMINENT; the glow reaches further.
	static constexpr float kLedRadius = 7.0f;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds headBounds() const;
	[[nodiscard]] visage::Bounds meterBounds() const;

	//-----------------------------------------------------------------------------------------//
	// THE LED'S TWO TIME CONSTANTS, AND THEY ARE BOTH DRAWING DECISIONS                        //
	//-----------------------------------------------------------------------------------------//

	/// **HOW LONG THE CORE STAYS LIT.** Long enough to be seen at any frame rate a host will run —
	/// a one-sample over is a quarter of a frame at 60 fps, so an indicator with no hold at all is
	/// an indicator nobody sees.
	static constexpr float kCoreHoldSeconds = 0.12f;
	/// **HOW LONG THE GLOW TAKES TO FADE.** The half that says an over happened A MOMENT AGO rather
	/// than right now, which is what a hard latch cannot tell you.
	static constexpr float kGlowFallSeconds = 1.2f;

	/// **BOTH DERIVED FROM ONE PUBLISHED NUMBER — `Reading::ClipAge`, seconds since the last over.**
	/// The audio thread records WHEN; this decides what that looks like. Keeping both constants here
	/// rather than in the DSP is what lets the look change without touching a measurement, and it is
	/// why no over can be missed: the age only grows, so a slow repaint reads a larger age rather
	/// than missing an event that has already been and gone.
	[[nodiscard]] bool ledCoreLit() const;
	[[nodiscard]] float ledGlow() const;
	/// THE LED'S STATE (backlog ML, `clip_led.h`): Clip while the core is lit, Near while the raw peak is within 2 dB of
	/// full scale, Normal otherwise. Nothing published is Normal.
	[[nodiscard]] ClipLedState ledState() const;

	/// **HOW FAR INTO THE CLIPPER'S KNEE THE SIGNAL IS REACHING — 0 at the knee's start, 1 at full
	/// scale.** The clip LED turns red when FULLY clipped, keeps its colour while soft, and its opacity
	/// follows how far into the knee the signal is.
	///
	/// **DERIVED ON THE CARD RATHER THAN PUBLISHED, and that is deliberate.** Every input it needs is
	/// already here: the raw peak is a reading, the knee is a CONTROL this card can read, and the
	/// arithmetic between them is a drawing decision. Publishing a ninth number would put a fact about
	/// how the LED looks into the audio thread, where it could not be changed without touching the
	/// DSP — the same reason `ClipAge` publishes a TIME and lets the card decide what a fade is.
	///
	/// **THE PEAK IT READS IS PRE-CLIP**, because the meter runs ahead of the clipper. That is what
	/// makes this measurable at all: after the clipper nothing exceeds the ceiling, so the depth would
	/// be a constant.
	///
	/// Zero when the clipper is off, or when the signal has not reached the knee.
	[[nodiscard]] float kneeDepth() const;

	/// ONE READING, BY NAME — the enum is the contract with the device, so the card never indexes by
	/// a bare number. Returns the reading, or a sensible nothing when the host publishes none.
	[[nodiscard]] float reading(fx::busutil::Reading r) const;
	/// Is anything publishing at all? A card with no data draws no meter rather than a meter of zeros
	/// — `device_host.h`'s rule, and the difference between "silent" and "not measured".
	[[nodiscard]] bool hasReadings() const;

	/// WHERE THE TWO POLARITY BUTTONS ARE, in this card's coordinates. Exposed so a case can press
	/// them where they actually are rather than inferring reach from a value that moved.
	[[nodiscard]] visage::Bounds polarityBounds(bool right) const;
	/// AND THE SWAP BUTTON'S, for the same reason.
	[[nodiscard]] visage::Bounds swapBounds() const;
	[[nodiscard]] ValueBox* monitorForTest() const { return monitor_.get(); }

	[[nodiscard]] std::vector<fx::busutil::Param> addressedParamsForTest() const;
	[[nodiscard]] ClipLedState ledStateForTest() const { return ledState(); }
	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

private:
	device::Modulatable* enableModulatable() override;
	/// The hand-drawn parts' tooltips — see `FxCard::contentTooltipAt`.
	const char* contentTooltipAt(visage::Point local) const override;
	[[nodiscard]] DeviceControl& control(fx::busutil::Param p) const;

	[[nodiscard]] visage::Bounds cellBounds(size_t index) const;
	void layOut();
	void drawMeter(visage::Canvas& canvas);
	void drawPolarity(visage::Canvas& canvas);
	void drawSwap(visage::Canvas& canvas);
	/// THE TOGGLE, BRACKETED SO IT RECORDS AN UNDO STEP — `FxCard::toggleEnable`'s road for a control
	/// that is not the enable. A bare `setValue` writes the field and forgets it.
	bool polarityPressAt(visage::Point position);
	void drawLed(visage::Canvas& canvas, visage::Bounds where);

	/// **dBFS FROM A LINEAR PEAK, FLOORED.** The reading is linear because a reading is a number; a
	/// bar is drawn in dB because that is how a level reads to a hand. The floor is where the bar
	/// bottoms out rather than running to negative infinity and dividing by it.
	static constexpr float kMeterFloorDb = -60.0f;
	[[nodiscard]] static float barFractionForDb(float db);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;

	/// WHAT THE METER ON SCREEN WAS DRAWN FROM — compared once a tick, so a moving meter asks for a
	/// frame and a still one does not. `updateContent`'s contract.
	mutable float drawnPeakL_ = -1.0f;
	mutable float drawnPeakR_ = -1.0f;
	mutable float drawnMomentary_ = 0.0f;
	mutable float drawnClipAge_ = -1.0f;

	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<ValueBox> monitor_;
	/// THE NAMED CHOICES AMONG THE ROW'S CONTROLS (Highpass, Clip) as boxes, each with the cell it sits in; `knobCells_` is
	/// the same for `knobs_`.
	std::vector<std::unique_ptr<ValueBox>> choiceBoxes_;
	std::vector<size_t> choiceCells_;
	std::vector<size_t> knobCells_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
};

} // namespace feathers::ui
