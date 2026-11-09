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
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/ui/widget_registry.h"
#include "fx/owltt/owltt_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/owltt_graph.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/fx_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/vertical_fader.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// OWLTT'S CARD — the 3-band multiband compressor's editor.
//
// THE CROSSOVER IS THE INSTRUMENT. A multiband compressor is defined by where it splits the
// spectrum, so the card leads with a crossover visualization: two vertical lines at the low and high
// crossover frequencies, with the three bands labelled below. Per-band gain reduction meters sit
// beside the visualization — the part that is actually a live reading, not a setting.
//
// LIVE METERING: the card reads per-band gain reduction and input level from the DSP through atomic
// variables. The DSP writes to these in processStereo; the card reads them in draw. No shared state
// beyond three atomics per band — the UI polls at frame rate, the DSP writes at audio rate.

namespace feathers::ui {

class OwlttCard final : public FxCard {
public:
	/// Build a OWLTT card for the device `id`.
	OwlttCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	         Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — a `size_t` would narrow into it silently; see `GenericCard`.
	OwlttCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	/// THE BODY. The surface, the border and the name are `FxCard::draw`'s — see its note on why the
	/// standardized header is the default rather than a convention each card is trusted to follow.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;

	/// WHETHER THIS DEVICE IS IN THE SIGNAL — the head's switch asks, and `FxCard` draws it. The
	/// drawing was this card's until it was seven identical copies; only the READ is per-card,
	/// because only the card knows its own control road.
	bool headEnabled() const override;

	/// Forwarded to the graph, which is the frame that draws the meters. See `OwlttGraph::update`.
	bool updateContent() override;
	void resized() override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kPad = 10.0f;

	/// The crossover visualization: shows where the spectrum is split.
	/// THE GRAPH, AND ITS HEIGHT IS THE WHOLE FIX. In use the card felt clunky: the graph is larger and
	/// some of the knobs moved to the expanded view, while the graph keeps its controls. So the picture
	/// stays interactive and grows; it is the knob list that leaves.
	///
	/// MEASURED, WHICH IS WHAT SETTLED THE NUMBER. At 48 the card was 236x410 and the graph was
	/// 216x48 — 12% of the card's height for the only part that says what the device is DOING, while
	/// a twelve-knob list took 73%. Worse per band: two bar hit-bands of 16px each claimed 32 of the
	/// 48 vertical pixels, so a press anywhere in a band was inside a bar's grab zone TWO THIRDS of
	/// the time before a strip or a ring got a pixel. That is what "clunky" was — not the number of
	/// controls, but targets that cannot be aimed.
	///
	/// AT 160 the same two bands claim 20% instead of 67%, the threshold sweep gets 3.3x the
	/// resolution, and the card comes to 280 — SHORTER than before while the picture goes from 12%
	/// to 57% of it. The knobs pay for it: twelve in four rows become three in one.
	static constexpr float kCrossoverHeight = 160.0f;
	static constexpr float kCrossoverPad = 8.0f;

	/// Per-band gain reduction meters.
	static constexpr float kMeterWidth = 8.0f;
	static constexpr float kMeterGap = 4.0f;

	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;

	/// HOW MANY KNOBS THE CARD CARRIES, AND HOW THEY GRID — one place, because three functions need
	/// to agree about it and only one of them can count them.
	///
	/// `layOut` divides the list into rows; `openHeight` is STATIC and reserves the space, so it
	/// cannot ask `knobs_` and has to be told. Those two disagreeing is a card that clips its own
	/// bottom row, and the old code had the number `2` written into both of them separately while the
	/// constructor called `addKnob` six times — three independent statements of one fact, none of
	/// which was checked against the others. Adding a seventh knob drew it on top of the fourth.
	static constexpr size_t kKnobsPerRow = 3;
	/// THREE, AND THEY ARE ONE IDEA: ratio, threshold and up/down stay on the card.
	/// All three are *how the compression behaves across all three bands* — and `owltt.cpp:648-663`
	/// computes `band + (global - 0.5)`, so they are literally macros over the per-band controls the
	/// graph draws. The card is therefore the picture plus the global versions of what the picture
	/// shows, with Up/Down as the one of the three that has NO picture, which is why it earns a
	/// circle rather than a handle.
	///
	/// AND THREE IS EXACTLY ONE ROW of the 3-per-row grid. A fourth would cost a whole 80px row for
	/// one knob, which is what excluded an earlier proposal that added Blend and Output — that also
	/// mixed compression shape with output staging, two ideas in one row. Both are in `OwlttExpanded`.
	static constexpr size_t kCardKnobCount = 3;
	/// **THE STEREO STRIP (backlog LQ)**, between the picture and the knobs: the L/R-M/S chip and a compact Link box,
	/// one 16 px line rather than a fourth knob — the card's three are one idea (how the bands compress) and a fourth
	/// would cost an 80 px row; these two are a second idea (how each band LISTENS to its two channels), and a
	/// strip under the picture says so. Both are in `OwlttExpanded` too (the window is a superset). The global Width
	/// (backlog LR) joins them: how wide each band SENDS, the other stereo question.
	static constexpr float kStripHeight = 16.0f;
	static constexpr float kChipWidth = 36.0f;
	static constexpr float kLinkBoxWidth = 96.0f;
	static constexpr size_t kKnobRows = (kCardKnobCount + kKnobsPerRow - 1) / kKnobsPerRow;
	/// **THE DRIVE (backlog NB)**, a bar the height of the picture at its left: there is no room for another knob, so it
	/// is a vertical bar left of the graph. The picture gives up the width.
	static constexpr float kDriveWidth = 24.0f;
	static constexpr float kDriveGap = 6.0f;

	/// THREE cells per row — six knobs for the global controls.
	static constexpr int kRowCells = 3;

	static float openHeight();
	static constexpr float foldedHeight() { return kHeadHeight; }

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	float laneHeightAt(float) const override { return folded() ? foldedHeight() : openHeight(); }

	/// WHICH DEVICE THIS CARD IS THE FACE OF — and without it the base cannot answer anything.
	///
	/// THIS WAS MISSING AND IT IS WHY OWLTT HAD NO ENABLE SQUARE. `FxCard::enableControl` asks
	/// `descriptor()`, so a card that does not override it reports `hasEnable() == false` no matter
	/// what the device declares — the switch is not merely undrawn, it is unreachable through the
	/// shared seam entirely. `GenericCard` had the same omission and it was found the same way.
	///
	/// It also matters after the head unification, when the LANE draws one head for every device and
	/// derives the name and the square by asking. Without this line OWLTT loses both.
	const device::Descriptor* descriptor() const override { return &fx::owltt::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	/// THE GRAPH'S RECTANGLE, so a case can aim at a bar rather than recompute where one is. Asking
	/// the card is the rule this repo already states about believing a layout.
	/// WHERE THE PLOT SITS IN THE CARD. The plot's own geometry is the graph's business now; this is
	/// only the rectangle the card gives it.
	[[nodiscard]] visage::Bounds crossoverBoundsForTest() const { return crossoverBounds(); }

	/// THE X WHERE ONE BAND ENDS, for a case that needs to aim inside a band. The bands are split by
	/// crossover FREQUENCY on a log axis, not into equal thirds, so this cannot be computed outside.
	void setModulation(ModState* state, EditHistory* history) override;


	//-----------//
	// For tests //
	//-----------//

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// The param index each knob corresponds to, in the same order as knobsForTest.
	const std::vector<size_t>& paramIndicesForTest() const { return paramIndices_; }
	/// The stereo strip's two controls (backlog LQ).
	[[nodiscard]] SwitchChip* stereoForTest() const { return stereo_.get(); }
	[[nodiscard]] ValueBox* linkForTest() const { return link_.get(); }
	/// The global Width beside them (backlog LR).
	[[nodiscard]] ValueBox* widthForTest() const { return width_.get(); }
	/// The Drive bar left of the picture (backlog NB).
	[[nodiscard]] VerticalFader* driveForTest() const { return drive_.get(); }

	/// Geometry asked rather than photographed.
	visage::Bounds headBounds() const;
	visage::Bounds crossoverBounds() const;
	/// THE DRIVE BAR'S RECTANGLE (backlog NB): left of the picture, as tall as it.
	visage::Bounds driveBounds() const;

private:
	void layOut();
	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt.
	device::Modulatable* enableModulatable() override;

	DeviceControl& control(size_t index) const;

	/// THRESHOLD BAR HIT TESTING — returns which bar (if any) is at the given point.

public:
	/// THE WAY INTO THE PANEL. Empty when there is nothing to open — `hasExpandedView` answers for
	/// what this build can CONSTRUCT rather than for what the device declares, so the button cannot
	/// appear a moment before the panel it promises exists.

private:

public:
	/// THE PICTURE, WHICH IS A CHILD FRAME NOW. The pop-out has a larger one, and the
	/// only honest way to have two sizes of one graph is one class — see `owltt_graph.h`. The card
	/// holds a 160px instance; `OwlttExpanded` holds a taller one over the same device.
	///
	/// EXPOSED BECAUSE THE CASES AIM AT IT. A drawn handle is not observable headlessly, so what a
	/// case can do is ask the graph where it put a ring and press there — which is what makes the
	/// three "drawn here, grabbable there" bugs this card shipped catchable at all.
	OwlttGraph* graph() const noexcept { return graph_.get(); }

private:

	backplane::DeviceHost& host_;
	Notifier* notifier_ = nullptr;
	DeviceInstanceId device_ = kNoInstance;

	/// DRAG STATE — tracks which threshold bar is being dragged.

	/// SIZED FROM THE DEVICE'S OWN DECLARATION, NOT A LITERAL.
	///
	/// THIS WAS A HARDCODED 25 AND `Param::Blend` IS 25, so valid indices run 0..25 -- twenty-six of
	/// them. `addKnob(Blend)` wrote `controls_[25]` one past the end, corrupting whatever followed
	/// this member, and `Knob::draw`'s `control_->display()` then called through a garbage vtable.
	/// It showed as a crash on every editor open: the overflow happens while the card is built and
	/// the first paint is where it lands.
	///
	/// `control()` has no bounds check either, which is what made a literal fatal rather than merely
	/// wrong -- the guard is now the type. A count taken from `Param::kCount` cannot disagree with
	/// the enum the way a number typed beside it can, and this is the fourth time this week a
	/// hardcoded count has outlived the thing it counted.
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::owltt::Param::kCount)>
	    controls_ {};
	std::unique_ptr<OwlttGraph> graph_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<size_t> paramIndices_;
	std::unique_ptr<SwitchChip> stereo_;
	std::unique_ptr<ValueBox> link_;
	/// BACKLOG LR: the global Width, after Link in the stereo strip.
	std::unique_ptr<ValueBox> width_;
	/// BACKLOG NB: the Drive, a bar left of the picture.
	std::unique_ptr<VerticalFader> drive_;
};

} // namespace feathers::ui
