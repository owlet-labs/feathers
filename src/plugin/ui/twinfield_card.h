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
#include "mod/twinfield/twinfield_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/twinfield_pad.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace feathers::ui {

class Notifier;
class WidgetRegistry;

/// **TWIN FIELD'S CARD — THE TERRAIN IS THE VISUALISER.**
///
/// Top to bottom: the A / B tabs, each carrying its own Solo chip; the pad (the shown layer's terrain,
/// its eight numbered points and its orbit, with the Crossfade knob on its corner); the shown layer's
/// view row (Pan X, Pan Y, Zoom, Rotate, Gamma); its motion row (Rate, Sync, Mode, Depth).
///
/// THE TAB IS THE CARD'S VIEW, not a param — Stereo Tool's scope tab is the precedent. BOTH layers'
/// widgets exist for the card's whole life and the tab shows one set, so no widget is ever re-bound.
class TwinFieldCard final : public FxCard {
public:
	TwinFieldCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	              Notifier* notifier = nullptr, ModState* mod = nullptr, EditHistory* history = nullptr);
	TwinFieldCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr, ModState* = nullptr,
	              EditHistory* = nullptr) = delete;
	~TwinFieldCard() override;

	void resized() override;
	/// THE SLOT MENU, AND THE 2x4 BLOCK'S VALUES — drawn here because `drawContent` is clipped to the body and the output
	/// strip lies outside it; this is the one hook `FxCard::draw` calls unclipped.
	void drawHeadExtras(visage::Canvas& canvas) override;
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	/// A CARD SCROLLED OUT OF THE LANE (or otherwise hidden) ENDS ITS PAD'S FOCUS — a hidden pad must not keep the wheel.
	void visibilityChanged() override;
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::twinfield::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kTabsHeight = 18.0f;
	static constexpr float kPadHeight = 150.0f;
	static constexpr float kKnobDiameter = 24.0f;
	static constexpr float kSoloWidth = 40.0f;
	static constexpr float rowHeight() { return Knob::columnHeight(kKnobDiameter); }
	static constexpr float bodyHeight() {
		return kGap + kTabsHeight + kGap + kPadHeight + kGap + rowHeight() + kGap + rowHeight() + kGap;
	}
	static float openHeight(size_t outputRows);

	/// WHICH LAYER THE TAB SHOWS.
	[[nodiscard]] size_t shown() const noexcept { return shown_; }
	void show(size_t layer);

	[[nodiscard]] TwinFieldPad& pad() const { return *pad_; }
	[[nodiscard]] SegmentedControl& tabs() const { return *tabs_; }
	[[nodiscard]] SwitchChip& solo(size_t layer) const { return *solo_[layer]; }
	[[nodiscard]] Knob& crossfade() const { return *crossfade_; }
	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] std::vector<mod::twinfield::Param> addressedParamsForTest() const;
	/// THE EIGHT OUTPUTS THE 2x4 BLOCK SHOWS, recomputed from the published orbit phases (the playhead channel: A as
	/// the phase, B as the level) and the MODULATED row, through the device's own `readLayer` and `blend`. False when the
	/// host publishes no playhead: the block then draws no values rather than a frozen guess.
	/// ONE LAYER'S PUBLISHED ORBIT PHASE (A on the playhead phase, B on its level); false when nothing is published.
	[[nodiscard]] bool publishedPhase(size_t layer, double* phase) const;
	[[nodiscard]] bool liveOutputs(std::array<float, mod::twinfield::kPoints>& out) const;
	[[nodiscard]] const std::array<float, mod::twinfield::kPoints>& shownOutputsForTest() const noexcept { return shownOutputs_; }

private:
	class ViewChoice final : public device::Modulatable {
	public:
		explicit ViewChoice(TwinFieldCard& card) : card_(card) {}
		const device::ParamDesc& desc() const override;
		double value() const override { return static_cast<double>(card_.shown_); }
		void setValue(double value) override;
		device::ModDisplay display() const override { return {}; }
		void setAmount(float) override {}
		void beginGesture() override {}
		void endGesture() override {}
		std::string text() const override;

	private:
		TwinFieldCard& card_;
	};

	/// One layer's widgets: the seven knobs (view row then Rate, Depth) and the two choice boxes.
	struct LayerWidgets {
		std::array<std::unique_ptr<Knob>, 5> view;
		std::unique_ptr<Knob> rate;
		std::unique_ptr<ValueBox> sync;
		std::unique_ptr<ValueBox> mode;
		std::unique_ptr<Knob> depth;
		/// THE RING'S TERRAIN SIZE (2026-11-08), beside Depth — the two geometry controls of the reading ring.
		std::unique_ptr<Knob> spread;
	};

	device::Modulatable* enableModulatable() override;
	void setShown(size_t layer);
	void showLayerWidgets();
	[[nodiscard]] std::vector<Knob*> allKnobs() const;
	[[nodiscard]] visage::PopupMenu guardedMenuFor(ValueBox& box);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
	size_t shown_ = 0;
	ViewChoice view_ { *this };
	mutable std::array<std::unique_ptr<DeviceControl>, mod::twinfield::at(mod::twinfield::Param::kCount)> controls_;
	std::unique_ptr<SegmentedControl> tabs_;
	std::array<std::unique_ptr<SwitchChip>, mod::twinfield::kLayers> solo_;
	std::unique_ptr<TwinFieldPad> pad_;
	std::unique_ptr<Knob> crossfade_;
	std::array<LayerWidgets, mod::twinfield::kLayers> layers_;
	std::array<double, mod::twinfield::at(mod::twinfield::Param::kCount)> drawn_ {};
	std::array<float, mod::twinfield::kPoints> shownOutputs_ {};
	bool haveOutputs_ = false;
	double drawnPhase_ = 0.0;
	bool haveDrawnPhase_ = false;
};

} // namespace feathers::ui
