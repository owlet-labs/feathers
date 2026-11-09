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

#include "backplane/device_host.h"
#include "mod/stepseq/stepseq_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <memory>
#include <vector>

#include <visage/ui.h>

// THE STEP SEQUENCER'S CARD (backlog MX): THE CARD IS THE STEP GRID. The bars are the steps, drawn and edited on
// the face — a drag paints the lane the tabs above them show (Value, Gate, Prob, Ratchet, Slew), the strip over the
// bars locks a step, the strip under them is its gate. Each page has its own Randomise and Trig, which act on that
// lane whichever page is shown (backlog NT); the controls below the grid are the ones that are not a step. The
// Advance and Reset pads sit at the grid's left, where a pattern starts.
//
// **EVERY EDIT OF THE STEPS IS ONE UNDO STEP** — a drag from press to release, a lock, a gate, a Randomise — recorded
// as the pattern before and after (`StepsEdit`), the MSEG card's arrangement for its shape.

namespace feathers::ui {

class Notifier;

/// THE GRID: the steps of one device, read from and written to its row's extra state through the host.
class StepGrid final : public visage::Frame, public TooltipSource {
public:
	StepGrid(backplane::DeviceHost& host, DeviceInstanceId id, EditHistory* history);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;
	/// NO DEVICE CONTROL: the steps are row state, not parameters.
	void offeredControls(std::vector<Offered>&) const override {}

	/// ITS OWN LINES, one per band — no control declares them, so the tooltip sweep asks for these by name.
	static constexpr const char* kTabsTip =
	    "Which part of each step the bars show and a drag paints \xE2\x80\x94 and the page whose Randomise is shown";
	static constexpr const char* kLockTip = "Lock a step: Randomise and the Turing machine leave it as it is";
	static constexpr const char* kGateTip =
	    "The step's gate: click for on or a rest; right-click for a tie, which holds the note before into it";
	static constexpr const char* kBarsTip =
	    "The steps. Drag to draw the lane shown; right-click a step for its gate and lock";

	void setHistory(EditHistory* history) noexcept { history_ = history; }

	[[nodiscard]] mod::stepseq::Lane lane() const noexcept { return lane_; }
	void setLane(mod::stepseq::Lane lane);
	/// TOLD WHEN THE PAGE CHANGES — the card shows that page's Randomise (backlog NT).
	void onLaneChanged(std::function<void()> changed) { laneChanged_ = std::move(changed); }

	/// The pattern as the host holds it now, and writing one (only the floats that move).
	[[nodiscard]] mod::stepseq::Pattern pattern() const;
	/// Writes `pattern` as a hand's edit (a fresh basis when any step moves) and returns what it wrote.
	mod::stepseq::Pattern writePattern(const mod::stepseq::Pattern& pattern);
	[[nodiscard]] size_t steps() const;

	/// THE BANDS, in the grid's own coordinates.
	static constexpr float kTabHeight = 12.0f;
	static constexpr float kLockHeight = 7.0f;
	static constexpr float kGateHeight = 9.0f;
	static constexpr float kBarsHeight = 56.0f;
	static constexpr float kHeight = kTabHeight + kLockHeight + kBarsHeight + kGateHeight;
	[[nodiscard]] visage::Bounds tabBounds(mod::stepseq::Lane lane) const;
	[[nodiscard]] visage::Bounds barsBounds() const;
	[[nodiscard]] visage::Bounds stepBounds(size_t step) const;
	[[nodiscard]] float lockTop() const { return kTabHeight; }
	[[nodiscard]] float gateTop() const { return kTabHeight + kLockHeight + kBarsHeight; }

	/// Whether the last frame's playhead moved; true when it redrew.
	bool poll();

private:
	enum class Drag { None, Paint, Gate, Lock };
	[[nodiscard]] int stepAt(float x) const;
	void paintAt(visage::Point p, bool first);
	void finishGesture(const char* verb);
	void showStepMenu(size_t step, visage::Point at);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_;
	EditHistory* history_ = nullptr;
	mod::stepseq::Lane lane_ = mod::stepseq::Lane::Value;
	std::function<void()> laneChanged_;

	Drag drag_ = Drag::None;
	mod::stepseq::Pattern before_ {};
	int lastStep_ = -1;
	float lastUnit_ = 0.0f;
	/// THE GATE STRIP'S DRAG SETS EVERY STEP IT CROSSES TO WHAT THE FIRST ONE BECAME.
	mod::stepseq::Gate paintGate_ = mod::stepseq::Gate::On;
	bool paintLock_ = false;

	uint32_t drawnOrdinal_ = 0xFFFFFFFFu;
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
};

/// A PRESS THAT RUNS ONE ACT — Randomise. Not a control: it moves no parameter of its own.
class ActButton final : public visage::Frame, public TooltipSource {
public:
	ActButton(std::string label, const char* tooltip, std::function<void()> act);
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override { return tooltip_; }
	void offeredControls(std::vector<Offered>&) const override {}
	void press() const {
		if (act_) {
			act_();
		}
	}

private:
	std::string label_;
	const char* tooltip_;
	std::function<void()> act_;
};

class StepSeqCard final : public FxCard {
public:
	StepSeqCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr, ModState* mod = nullptr,
	            EditHistory* history = nullptr);
	StepSeqCard(backplane::DeviceHost&, size_t, Notifier* = nullptr, ModState* = nullptr,
	            EditHistory* = nullptr) = delete;
	~StepSeqCard() override;

	void resized() override;
	/// THE PADS' CAPTIONS, under each: which Trig moves the pattern on and which sends it home.
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	float laneHeightAt(float cardWidth) const override;
	/// THE OPEN CARD AT `outputRows` rows of output chips — what the strip asks before the card exists.
	static float openHeight(size_t outputRows);
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	bool headSourceFocused() const override;
	bool headArmed() const override;
	const device::Descriptor* descriptor() const override { return &mod::stepseq::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	/// RANDOMISE ONE LANE, by Rand Amt, from the Seed — one undo step; false when nothing moved. The no-argument form is the
	/// page shown, whose button is the one on the face. A Trig on that lane's pad does the same on the audio thread
	/// (`Device::roll`) whichever page is shown (backlog NT).
	bool randomise();
	bool randomise(mod::stepseq::Lane lane);

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kPadColumn = 18.0f;
	static constexpr float kKnobDiameter = 24.0f;
	static constexpr float knobRowHeight() { return Knob::columnHeight(kKnobDiameter); }
	static constexpr float bodyHeight() {
		return kGap + StepGrid::kHeight + kGap + ValueBox::kHeight + kGap + knobRowHeight() + kGap + ValueBox::kHeight
		       + kGap + ValueBox::kHeight + kGap;
	}

	[[nodiscard]] StepGrid& grid() const { return *grid_; }
	[[nodiscard]] TrigPad& advancePad() const { return *advancePad_; }
	[[nodiscard]] TrigPad& resetPad() const { return *resetPad_; }
	[[nodiscard]] TrigPad& randomisePad(mod::stepseq::Lane lane) const {
		return *randomisePads_[static_cast<size_t>(lane)];
	}
	[[nodiscard]] ActButton& randomiseButton(mod::stepseq::Lane lane) const {
		return *randomiseButtons_[static_cast<size_t>(lane)];
	}
	/// The shown page's button.
	[[nodiscard]] ActButton& randomiseButton() const { return randomiseButton(grid_->lane()); }
	[[nodiscard]] ValueBox& randomAmountBox() const { return *amount_; }
	[[nodiscard]] ValueBox& amountBox() const { return *outAmount_; }
	[[nodiscard]] Knob& rateKnob() const { return *rate_; }
	[[nodiscard]] Knob& lockKnob() const { return *lock_; }
	[[nodiscard]] ValueBox& timeBaseBox() const { return *timeBase_; }
	[[nodiscard]] SwitchChip& polyChip() const { return *poly_; }
	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// Every control a widget on this card addresses, asked of the widgets.
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;

private:
	device::Modulatable* enableModulatable() override;
	/// SHOW EXACTLY THE CONTROLS THAT CHANGE SOUND NOW (the honesty rule): Rate and Time Base under Clock = Rate,
	/// Lock under Turing, Poly under Retrig.
	void applyRelevance();
	[[nodiscard]] std::vector<visage::Frame*> widgets() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	mutable std::array<std::unique_ptr<DeviceControl>, mod::stepseq::kParamCount> controls_;
	std::unique_ptr<StepGrid> grid_;
	std::unique_ptr<TrigPad> advancePad_;
	std::unique_ptr<TrigPad> resetPad_;
	std::array<std::unique_ptr<TrigPad>, mod::stepseq::kLaneCount> randomisePads_;
	std::unique_ptr<ValueBox> steps_;
	std::unique_ptr<ValueBox> direction_;
	std::unique_ptr<ValueBox> clock_;
	std::unique_ptr<ValueBox> timeBase_;
	std::unique_ptr<Knob> rate_;
	std::unique_ptr<Knob> gateLength_;
	std::unique_ptr<Knob> lock_;
	std::unique_ptr<ValueBox> seed_;
	std::array<std::unique_ptr<ActButton>, mod::stepseq::kLaneCount> randomiseButtons_;
	std::unique_ptr<ValueBox> amount_;
	std::unique_ptr<ValueBox> outAmount_;
	std::unique_ptr<SwitchChip> turing_;
	std::unique_ptr<SwitchChip> bipolar_;
	std::unique_ptr<SwitchChip> retrig_;
	std::unique_ptr<SwitchChip> poly_;
	std::vector<Watch> watches_;

	std::array<double, mod::stepseq::kParamCount + mod::stepseq::kExtraFloats> drawn_ {};
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
};

} // namespace feathers::ui
