/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "mod/lfo/lfo_types.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/modulated_row.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <memory>
#include "plugin/ui/rack_card.h"

#include <visage/ui.h>

namespace feathers::ui {

/// THE LFO'S CARD. A waveform display with a playhead star, value boxes for the
/// sync/rate/skew controls, menu boxes for waveform/mode, and chips for the switches. Based on the
/// MSEG card but simpler — the waveform is drawn, not edited by dragging.
///
/// THE WAVEFORM IS A READOUT, not a control. The skew knobs and waveform selector are the
/// editors; the curve shows what those produce. Random walk shows the stepped values.
class LfoCard : public FxCard {
public:
	LfoCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history);
	LfoCard(backplane::DeviceHost&, size_t, ModState&, EditHistory&) = delete;

	void drawContent(visage::Canvas& canvas) override;

	bool headSourceFocused() const override;
	bool headArmed() const override;
	void resized() override;

	/// **THE TRIG PAD** (backlog MV), at the drawn wave's left, where a reset sends it — the DAHDSR's seat.
	[[nodiscard]] visage::Bounds trigPadBounds() const;
	[[nodiscard]] TrigPad* trigPadForTest() const noexcept { return cur().trigPad.get(); }
	/// THE SLOT MENU'S DOTS, WHEN A HOST BINDS ONE (a lane: Feathers' fx lane holds modulators since 2026-10-04) —
	/// `drawSlotMenu` draws nothing where none is bound, so the strip, which draws its own head mark, is unchanged.
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }

	float bodyTop() const { return bodyTopEdge(); }

	/// **THE FOOT WAS A FLAT `kCardOutputStripHeight` — ONE ROW, WHATEVER THE DEVICE PUBLISHES.** That
	/// is the same one-row assumption `EnvCard::laneHeightAt` records being deleted rather than copied,
	/// and this card was the last copy of it. The LFO declares one output today, so the number does
	/// not move; what changes is that it is now COUNTED, at the width being asked about, rather than
	/// assumed.
	float laneHeightAt(float cardWidth) const override {
		return folded() ? kCardHeadHeight
		                : kCardHeadHeight + height() + outputStripHeightAt(cardWidth);
	}
	void foldedChanged() override;

	const device::Descriptor* descriptor() const override { return &mod::lfo::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	backplane::Source sourceOfOutput(size_t output) const {
		return backplane::Source::output(device_, output);
	}

	/// ITS DESIGN WIDTH IN A HORIZONTAL LANE, AND ITS HEIGHT IN A VERTICAL ONE — see
	/// `EnvCard::laneExtent`, which carries the whole note for all three modulator cards.
	float laneExtent(float cross) const override {
		if (folded()) {
			return kCardHeadHeight;
		}
		if (laneAxis() == LaneAxis::Horizontal) {
			return kModCardWidth;
		}
		// THE BASE ANSWERS THE VERTICAL CASE — `laneHeightAt(cross)`. See `EnvCard::laneExtent`.
		return FxCard::laneExtent(cross);
	}

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	/// The Trig pad's column at the wave's left — the pad and a gap.
	static constexpr float kTrigColumn = TrigPad::kDiameter + 6.0f;
	static constexpr float kRowGap = 4.0f;
	static constexpr float kEditorHeight = 80.0f;
	static constexpr float kChipHeight = 16.0f;
	/// **THE TAB STRIP** along the top of the body: four tabs, each a whole LFO (`mod::lfo::kTabs`), the selected one's control
	/// set shown below and the others' hidden, never re-pointed. Each tab carries its own On square.
	static constexpr float kTabStrip = 16.0f;
	static constexpr float kTabGap = 4.0f;
	static constexpr size_t kTabCount = mod::lfo::kTabs;

	static constexpr float height() {
		return kTabStrip + kGap + kEditorHeight + kGap + kBoxRows * (ValueBox::kHeight + kRowGap) - kRowGap + kGap
		       + kChipHeight + kGap;
	}

	/// **THE VALUE BOXES, IN TWO ROWS, AND THE SPLIT IS WHAT EACH ROW IS ABOUT.** The first is the
	/// MSEG's own five — Base, Rate, Smoothing, Amount, Phase — because those are the questions every
	/// modulator answers and a hand that has learned one card should not relearn them here. The second
	/// is what this device adds: the three skew weights and the seed.
	///
	/// NINE, WHERE THE FIRST VERSION HAD FOUR AND DOCUMENTED THEM AS SOMETHING ELSE ENTIRELY. Its
	/// header read "Rate, Skew A, Skew B, Skew C" and its `layout()` returned Rate, Base, Smoothing,
	/// Amount — so two of the three skews were reachable from NOWHERE on the card, and the third was
	/// wired to a toggle that flipped a 0..100% weight between 50 and 100. Every control the device
	/// declares now has somewhere a hand can put a number.
	static constexpr size_t kBoxRows = 2;
	static constexpr size_t kBoxesPerRow = 5;
	static constexpr size_t kBoxCount = 10; ///< Base, Rate, Sm, Amt, Ph | Pk, Cr, Tr, Seed, St
	static const std::array<mod::lfo::Param, kBoxCount>& layout();

	/// The switches: Ping-Pong, Bipolar. TWO-STATE ONLY — see `toggleChip`, and the MSEG's chip list,
	/// which is the shape this follows. It was five; the Smoothing switch went with its param (decided
	/// 2026-09-19 — the box's bottom is its off), and Waveform and Mode left for `choices()`.
	static constexpr size_t kChipCount = 4;
	/// HOW MANY OF THEM SIT IN THE CHIP ROW — the first three. The fourth, Carry, sits in the second box row's last
	/// column (see `chipBounds`).
	static constexpr size_t kRowChips = 3;
	struct Chip {
		mod::lfo::Param param;
		const char* label;
	};
	static const std::array<Chip, kChipCount>& chips();

	bool chipOn(size_t index) const;
	/// WHETHER A CHIP CHANGES ANYTHING NOW — the honesty rule. Poly is dead in Free, which is one shape whatever
	/// the switch says (`mod::PolyShape::poly`): drawn dim and not pressable there.
	bool chipLive(size_t index) const;
	std::string chipLabel(size_t index) const;
	void toggleChip(size_t index);

	/// **THE CHOICES: Waveform and Mode, as value boxes that open a menu (backlog GG).** They were
	/// chips that showed the current value and advanced on a press, so reaching Once from Loop went
	/// THROUGH Free, applied — the multivalue toggle the UI rules
	/// out. A `ValueBox` over a named `Unit::Choice` opens its list from the descriptor, so the pick is
	/// one value and the options are read before any of them is applied — `GenericCard`'s rule.
	/// They sit in the chip row, in the first two of its four cells, where the chips were.
	static constexpr size_t kChoiceCount = 2;
	static const std::array<mod::lfo::Param, kChoiceCount>& choices();
	const ValueBox* choiceBoxForTest(size_t index) const {
		return index < kChoiceCount ? cur().choiceBoxes[index].get() : nullptr;
	}
	ValueBox* choiceBoxForTest(size_t index) {
		return index < kChoiceCount ? cur().choiceBoxes[index].get() : nullptr;
	}
	/// The cell a choice sits in; its box is inset one pixel so the cell's outline shows.
	visage::Bounds choiceBounds(size_t index) const;
	/// The cell a chip sits in — public so the fit case can measure its caption against it.
	visage::Bounds chipBounds(size_t index) const;
	/// What the face prints for a choice — the declared name, shortened only where a cell cannot hold
	/// it. The menu always lists the declared names.
	static std::string choiceFace(mod::lfo::Param param, double value);
	/// WHAT THE RATE BOX PRINTS for a Rate of `value`: Hz on the Hz base, the declared multiple on any other (NJ).
	std::string rateFace(double value) const;

	/// WHERE THE WAVEFORM IS DRAWN.
	visage::Bounds editorBounds() const;
	visage::Bounds editorGrabBounds() const;

	/// **THE TABS.** `selectedTab` is 0-based; `selectTab` shows that tab's set and hides the rest. `tabBounds` is where a
	/// tab is drawn and hit (the strip's own arithmetic, shared with the press); `tabOnBounds` the On square inside it.
	[[nodiscard]] size_t selectedTab() const noexcept { return tab_; }
	/// HOW MANY OF ALL THE TABS' CHOICE BOXES AND TRIG PADS ARE SHOWN: one set's, never more.
	[[nodiscard]] size_t visibleSetWidgetsForTest() const {
		size_t n = 0;
		for (const TabSet& set : sets_) {
			for (const auto& box : set.choiceBoxes) {
				n += box->isVisible() ? 1u : 0u;
			}
			n += set.trigPad->isVisible() ? 1u : 0u;
		}
		return n;
	}
	void selectTab(size_t tab);
	[[nodiscard]] visage::Bounds tabBounds(size_t tab) const;
	[[nodiscard]] visage::Bounds tabOnBounds(size_t tab) const;
	/// The tab a point is over, or `kTabCount` for none.
	[[nodiscard]] size_t tabAt(visage::Point position) const;
	/// WHETHER TAB `tab` IS SWITCHED ON — its Enabled field as stored (tab 1 is always drawn lit by the head too).
	[[nodiscard]] bool tabOn(size_t tab) const;
	void toggleTabOn(size_t tab);

	bool updateContent() override;

	/// WHERE THE PLAYHEAD IS DRAWN.
	struct Marker {
		visage::Point at { 0.0f, 0.0f };
		bool drawn = false;
	};
	[[nodiscard]] Marker marker() const;

	/// THE WAVEFORM POLYLINE in the unit box.
	std::vector<std::pair<float, float>> curve(bool modulated = false) const;
	/// **AND THE RIGHT EAR'S** (backlog PA): the same window, the wave read `Settings::stereoLead` further on — empty
	/// while Stereo Phase is 0, and then nothing but the left ear is drawn.
	std::vector<std::pair<float, float>> curveRight(bool modulated = false) const;
	/// HOW MANY STROKES THE LAST PAINT GAVE THE RIGHT EAR'S LINE — a count made by the paint itself, so a case can ask
	/// whether the trace was drawn rather than whether it could have been (a repaint is not observable headlessly).
	[[nodiscard]] size_t rightStrokesDrawnForTest() const noexcept { return rightStrokes_; }

	std::vector<mod::lfo::Param> addressedParamsForTest() const;
	/// EVERY DEVICE FIELD A HAND CAN REACH ON THE CARD, all four tabs (what the reachability guard reads).
	[[nodiscard]] std::vector<size_t> addressedFieldsForTest() const;

	std::string boxCaption(size_t index) const;
	const visage::Frame* boxFrame(size_t index) const;
	const ValueBox* boxForTest(size_t index) const {
		return index < kBoxCount ? cur().boxes[index].get() : nullptr;
	}

	device::Modulatable* enableModulatable() override;
	/// The hand-drawn parts' tooltips — see `FxCard::contentTooltipAt`.
	const char* contentTooltipAt(visage::Point local) const override;

	/// WHAT THE PLOT DRAWS FROM, for a case that asks by value.
	[[nodiscard]] mod::lfo::Settings drawnFromForTest() const { return shownSettings(); }
	[[nodiscard]] mod::lfo::Settings restFromForTest() const { return settings(); }
	[[nodiscard]] bool drawsModulatedShapeForTest() const { return shapeMoved(); }

private:
	std::vector<Watch> watches_;
	void bindBox(size_t tab, size_t index);
	/// The top of a row of value boxes, and where one box sits. `resized` places the boxes through
	/// `boxBounds` so the placement and any hit-test are one arithmetic rather than two.
	float boxRowTop(size_t row) const;
	/// Whether the box at `index` addresses a control that does anything in the current waveform —
	/// `device::relevantFor` against the gate this card reads. A dead box is not drawn.
	bool boxIsLive(size_t index) const;
	/// The gate's current value (the waveform), watched so the row reflows when anything moves it.
	float gateValue() const;
	visage::Bounds boxBounds(size_t index) const;
	/// ONE ROW OF `kChoiceCount + kChipCount` EQUAL CELLS — the choices first, then the chips.
	visage::Bounds cellBounds(size_t cell) const;

	std::pair<float, float> toUnit(visage::Point point) const;
	visage::Point fromUnit(float x, float y) const;

	mod::lfo::Settings settings() const;
	/// **THE SAME ROW WHERE MODULATION LEFT IT** — what the plot draws, so the picture follows a routed control as the knob's ring does. `settings()` stays the STORED row: an edit starts from it, and an edit that started from a modulated one would write the modulation into the patch.
	mod::lfo::Settings shownSettings() const;
	/// Modulation moved the shape: the LFO's Settings has no `==`, so the rows (what the device reads) are compared.
	bool shapeMoved() const { return mod::lfo::rowOf(settings()) != mod::lfo::rowOf(shownSettings()); }
	void writeSettings(const mod::lfo::Settings& settings);
	mod::Playhead playhead() const;
	/// The drawn window with the wave read `lead` cycles (steps, for a walk) further on — `curve` at 0, `curveRight` past it.
	std::vector<std::pair<float, float>> curveAt(float lead, bool modulated = false) const;
	/// See `rightStrokesDrawnForTest`.
	size_t rightStrokes_ = 0;

	mod::Playhead drawnPlayhead_ {};
	/// The waveform the current layout was built for; see `updateContent`.
	float gateDrawn_ = -1.0f;

	backplane::DeviceHost& host_;

	DeviceInstanceId device_ = kNoInstance;
	ModState& mod_;
	EditHistory& history_;

	/// **ONE TAB'S WHOLE SET OF CONTROLS**, built once for each tab bound to that tab's own fields — a tab press shows one
	/// set and hides the others (the HARM card's arrangement); nothing is ever re-pointed.
	struct TabSet {
		std::array<std::unique_ptr<DeviceControl>, kBoxCount> controls;
		std::array<std::unique_ptr<ValueBox>, kBoxCount> boxes;
		std::array<std::unique_ptr<DeviceControl>, kChoiceCount> choiceControls;
		std::array<std::unique_ptr<ValueBox>, kChoiceCount> choiceBoxes;
		std::unique_ptr<DeviceControl> trigControl;
		/// "NOTES FIRE TRIG" (backlog NU), the pad's normal — declared before the pad, which holds it.
		std::unique_ptr<DeviceControl> notesTrigControl;
		std::unique_ptr<TrigPad> trigPad;
		/// THIS TAB'S ON SQUARE, on its tab (tab 1's is the head's Enable too: one control).
		std::unique_ptr<DeviceControl> onControl;
	};
	std::array<TabSet, kTabCount> sets_;
	mutable std::unique_ptr<DeviceControl> enableControl_;
	/// THE SELECTED TAB. A card-local view choice, not patch state.
	size_t tab_ = 0;
	/// The On squares as last drawn, a bit a tab; see `updateContent`.
	unsigned tabsOnDrawn_ = ~0u;
	[[nodiscard]] TabSet& cur() noexcept { return sets_[tab_]; }
	[[nodiscard]] const TabSet& cur() const noexcept { return sets_[tab_]; }
	/// THE DEVICE FIELD OF control `p` ON TAB `tab` (the selected one by default).
	[[nodiscard]] size_t fieldOf(mod::lfo::Param p) const noexcept { return mod::lfo::tabParam(tab_, p); }
	[[nodiscard]] size_t fieldOf(size_t tab, mod::lfo::Param p) const noexcept { return mod::lfo::tabParam(tab, p); }
	/// WHAT THE RATE BOX OF TAB `tab` PRINTS — `rateFace`, for a tab other than the selected one.
	std::string rateFaceOf(size_t tab, double value) const;
	void drawTabs(visage::Canvas& canvas);
	void showTabOutputs();
};

} // namespace feathers::ui
