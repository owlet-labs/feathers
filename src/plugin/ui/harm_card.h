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

#include "plugin/ui/widget_registry.h"
#include "fx/harm/harm_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fx_card.h"
#include "backplane/device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <array>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

// HARM'S CARD — the fx lane's second tenant on the glass.
//
// THE CARD SHOWS HARM'S GLOBAL CONTROLS ONLY. Anything that addresses one partial (a partial's harmonic, its Mode, its Level,
// its Active switch) lives in the window, where the partials are shaped. The card holds the two master controls, the
// envelope's Attack, Release, Delay, Hold, Decay and Sustain, and the two device switches Poly and Env Depth. Porta stays in the
// window's master row.
//
// THREE THINGS THIS CARD MUST NOT DO:
//
//   - IT MUST NOT SAY "HPF". The firmware calls the notch's width knob `hpf`; it is WIDTH, and it is not on this card.
//   - IT MUST NOT IMPLY POLY WORKS when HARM declares no per-voice road. The switch is drawn disabled and captioned by the
//     device's own declaration (`perVoiceControl`), never by a hard-coded opinion.
//   - IT MUST NOT CUT A CAPTION. Every label is whole at the card's cell width; the tooltips suite measures it.

namespace feathers::ui {

class HarmCard final : public FxCard {
public:
	HarmCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	         Notifier* notifier = nullptr);
	HarmCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	const device::Descriptor* descriptor() const override { return &fx::harm::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool updateContent() override;
	void resized() override;

	//--------//
	// Layout //
	//--------//

	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kPad = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// The gap between the knob rows and the switch row.
	static constexpr float kActiveRowGap = 6.0f;

	struct CardKnob {
		fx::harm::Param param;
		/// Spelled out, not derived from the descriptor's qualified name. Short enough to fit a cell whole.
		const char* label;
	};

	/// THE GLOBAL KNOBS, in reading order. Two rows of four. The masters come first: they are what a hand reaches for on a
	/// device that has many partials.
	static constexpr CardKnob kCardKnobs[] = {
	    { fx::harm::Param::MasterLevel, "Master" },
	    { fx::harm::Param::MasterDepth, "Depth" },
	    { fx::harm::Param::Attack, "Attack" },
	    { fx::harm::Param::Release, "Release" },
	    { fx::harm::Param::Delay, "Delay" },
	    { fx::harm::Param::Hold, "Hold" },
	    { fx::harm::Param::Decay, "Decay" },
	    { fx::harm::Param::Sustain, "Sustain" },
	};
	static constexpr size_t kCardKnobCount = std::size(kCardKnobs);

	/// FOUR CELLS A ROW, and the knobs wrap onto a second row. Five cells cut "Attack" and "Release" at this card's width
	/// (tests/gui_widgets/test_gui_widgets_tooltips), so the reduction is the fix rather than a choice to trade away.
	static constexpr int kRowCells = 4;

	/// THE TWO DEVICE SWITCHES, under the knobs: Poly, then Env Depth.
	static constexpr fx::harm::Param kCardChips[] = { fx::harm::Param::Poly, fx::harm::Param::DepthFollowsEnv };
	static constexpr size_t kCardChipCount = std::size(kCardChips);

	static float openHeight();
	static constexpr float foldedHeight() { return kHeadHeight; }

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;
	float laneHeightAt(float) const override { return folded() ? foldedHeight() : openHeight(); }
	void setModulation(ModState* state, EditHistory* history) override;

	/// Geometry asked rather than photographed.
	visage::Bounds headBounds() const;
	visage::Bounds chipBounds(size_t index) const;

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }
	SwitchChip* chipForTest(fx::harm::Param param) const;

	/// EVERY CONTROL THIS CARD PUTS A HAND ON, the card's own answer. The window must offer each of these too.
	std::vector<fx::harm::Param> addressedParamsForTest() const;

	enum class Part { None, Fold, Enable, Expand, SlotMenu };
	Part partAt(visage::Point position) const;

private:
	void layOut();
	device::Modulatable* enableModulatable() override;
	void toggleEnabled();
	DeviceControl& control(fx::harm::Param param) const;

	backplane::DeviceHost& host_;
	Notifier* notifier_ = nullptr;
	DeviceInstanceId device_ = kNoInstance;
	bool drawnEnabled_ = false;
	bool haveDrawnEnabled_ = false;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)>
	    controls_ {};
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<fx::harm::Param> knobParams_;
	std::vector<std::unique_ptr<SwitchChip>> chips_;
};

} // namespace feathers::ui
