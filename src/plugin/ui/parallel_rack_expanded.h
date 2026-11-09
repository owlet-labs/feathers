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
#include "plugin/ui/arm_relay.h"
#include "plugin/ui/container_presets.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/parallel_rack_card.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/rack_mode_views.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <vector>

// THE RACK'S WINDOW (backlog NO, stage 1): the rack's own controls on the left — the Mode and Position (stage 2) on a
// row of their own, then Mix, Gain and the eight macros —
// and one rail per chain to their right, SKLITTER's and Resonate's `FxLaneRail` over that chain, headed by the chain's
// level, mute, solo, a small vertical level meter and a × that removes it (backlog OC: a channel strip, which works
// while the chain is collapsed). Mix and Gain stand on top, then an H-line, then the macros 2 across and 4 tall; "+ Chain"
// is a small button under them and adds a chain. Everything the card offers is here (window ⊇ card).
//
// The chains are re-read through the host's handle every tick (CLAUDE.md's ABA rule, trap T9): a chain added or
// removed — here, on the card, or by a load — rebuilds the rails, and each rail follows its own chain by stable id.

namespace feathers::ui {

/// **A ROLE NO CHAIN FILLS, AS A RAIL** (§2a) — built exactly as a real chain's rail is (`FxLaneRail` headed by a
/// `ChainStrip`), with no chain behind it: its strip makes the chain on a first edit, and its column is an empty chain's
/// (+) and what passes (`FxLaneRail::setPhantom`).
struct PhantomRail {
	std::string label;
	std::string passes;
	std::function<void()> onZone;
	std::unique_ptr<ChainStrip> strip;
	std::unique_ptr<FxLaneRail> rail;
	[[nodiscard]] ChainStrip& stripForTest() const { return *strip; }
	[[nodiscard]] FxLaneRail& railForTest() const { return *rail; }
	[[nodiscard]] const std::string& labelForTest() const noexcept { return label; }
	[[nodiscard]] const std::string& passesForTest() const noexcept { return passes; }
	/// Press the (+), as a hand does.
	void pressZoneForTest() const {
		if (onZone) {
			onZone();
		}
	}
};

class ParallelRackExpanded final : public ExpandedView {
public:
	ParallelRackExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                     Notifier* notifier = nullptr);
	ParallelRackExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;
	~ParallelRackExpanded() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override;
	std::string title() const override { return "Rack"; }
	/// **THE RACK'S PRESETS** (backlog OU): `Racks/` in the title row. A load replaces the whole rack — its controls, its
	/// mode and its chains. A rail has no browser of its own (you add a chain and use ITS browser).
	[[nodiscard]] visage::Frame* titleAccessory() const override { return presets_.browser.get(); }
	[[nodiscard]] PresetBrowser& presetBrowserForTest() const { return *presets_.browser; }
	void modulationBound(ModState* mod, EditHistory* history) override;
	void setPresentedOn(SurfaceLayers* host) override;
	bool updateContent() override;

	static constexpr float kPad = 14.0f;
	/// TWO MACROS ACROSS AND NO WIDER (backlog OC, corrected): Mix and Gain on the top row, an H-line, the eight macros 2 across and 4 tall.
	static constexpr float kColumnWidth = 2.0f * 56.0f;
	static constexpr float kAddWidth = 64.0f;
	static constexpr float kAddHeight = 20.0f;

	/// THE RACK'S OWN CONTROLS THE PANEL SEATS: Mix, Gain and the eight macros.
	[[nodiscard]] std::vector<rack::parallel::Param> addressedParamsForTest() const;
	[[nodiscard]] size_t railCountForTest() const noexcept { return rails_.size(); }
	/// The knob at `i` in `addressedParamsForTest` order (Mix, Gain, M1..M8), and the divider and "+ Chain" boxes.
	[[nodiscard]] visage::Bounds knobBoundsForTest(size_t i) const { return knobs_[i]->bounds(); }
	[[nodiscard]] visage::Bounds dividerBoundsForTest() const { return dividerBounds(); }
	[[nodiscard]] visage::Bounds addBoundsForTest() const { return addBounds(); }
	[[nodiscard]] FxLaneRail& railForTest(size_t i) const { return *rails_[i]->rail; }
	[[nodiscard]] ChainStrip& stripForTest(size_t i) const { return *rails_[i]->strip; }
	/// THE MODE'S DROPDOWN AND THE POSITION'S KNOB (stage 2), on the row above Mix and Gain.
	[[nodiscard]] ValueBox& modeBoxForTest() const { return *modeBox_; }
	[[nodiscard]] Knob& positionKnobForTest() const { return *position_; }
	[[nodiscard]] SegmentedControl& unusedChipForTest() const { return *unused_; }
	[[nodiscard]] RackSpectrumStrip& spectrumForTest() const { return *spectrum_; }
	[[nodiscard]] RackPositionBar& positionBarForTest() const { return *positionBar_; }
	[[nodiscard]] size_t placeholderCountForTest() const noexcept { return phantoms_.size(); }
	[[nodiscard]] PhantomRail& phantomForTest(size_t m) const { return *phantoms_[m]; }
	/// A missing role's placeholder, after the last rail.
	static constexpr float kPlaceholderWidth = 150.0f;
	/// The slope's row in the bands column.
	static constexpr float kSegmentRow = 18.0f + 7.0f;
	[[nodiscard]] Knob& cutKnobForTest(size_t k) const { return *cuts_[k]; }
	[[nodiscard]] SwitchChip& keyTrackForTest() const { return *keyTrack_; }
	[[nodiscard]] ValueBox& bandsBoxForTest() const { return *bandsBox_; }
	[[nodiscard]] ValueBox& filteringBoxForTest() const { return *filteringBox_; }
	[[nodiscard]] SegmentedControl& slopeForTest() const { return *slope_; }
	/// Press "+ Chain", as a hand does.
	void addChainForTest() { addChain(); }
	/// Press a chain's ×, as a hand does.
	void removeChainForTest(uint16_t chain) { removeChain(chain); }
	/// Runs the relay now, as the tick does (`updateContent`).
	bool relayArmForTest() { return relayArm(); }

protected:
	void onInnerNestChanged() override;

private:
	struct ChainRail {
		uint16_t id = 0;
		std::string caption;
		std::unique_ptr<ChainStrip> strip;
		std::unique_ptr<FxLaneRail> rail;
	};

	void layOut();
	/// **A CARD DROPPED ON ANOTHER CHAIN** (backlog NQ): `ids`, dragged out of chain `from`, released at `inWindow` over
	/// another chain's open column, move into it at the gap under the hand. False when no chain is under the hand.
	bool moveAcrossChains(uint16_t from, std::span<const DeviceInstanceId> ids, visage::Point inWindow);
	/// **A CONTROL IS HIDDEN, NOT GREYED, WHERE THE MODE DOES NOT READ IT** — the card's rule. True when it changed.
	bool showForMode();
	[[nodiscard]] float dividerY() const;
	[[nodiscard]] float knobY(size_t i) const;
	[[nodiscard]] visage::Bounds dividerBounds() const;
	[[nodiscard]] float railsWidth() const;
	/// "+ Chain" — EMPTY (not drawn, not pressable) in a role mode (lead's call 2026-10-29, to be confirmed): there the
	/// mode decides how many chains are heard, a missing role is made by its phantom's first edit, and a chain past the
	/// roles would only be unrouted. Its slot stays reserved, so nothing below it moves with the mode.
	[[nodiscard]] visage::Bounds addBounds() const;
	[[nodiscard]] visage::Bounds addSlotBounds() const;
	[[nodiscard]] bool addShown() const;
	/// THE BANDS COLUMN beside the macros (Bands only): whether it is drawn, where it starts and ends, and where the rails
	/// start after the columns.
	[[nodiscard]] bool bandsColumn() const;
	[[nodiscard]] float bandsTop() const;
	[[nodiscard]] float bandsBottom() const;
	[[nodiscard]] float railsLeft() const;
	/// What the phantom rails take, at their own widths.
	[[nodiscard]] float phantomsWidth() const;
	/// How many roles the mode gives that no chain fills, and where the rails start (under the strip or bar).
	[[nodiscard]] size_t missingRoles() const;
	[[nodiscard]] float railsTop() const;
	void addChain();
	void removeChain(uint16_t id);
	/// Rebuild the rails when the chains changed, then follow each. True when anything moved.
	bool follow();
	/// Write every rail's open state to the rack by chain id (a hand's toggle); a no-op while `follow` applies the rack's.
	void keepOpenState();
	bool applying_ = false;

	backplane::DeviceHost& host_;
	WidgetRegistry& registry_;
	DeviceInstanceId id_;
	Notifier* notifier_ = nullptr;
	/// HELD BEFORE THE RAILS ARE BUILT OVER IT, AND DECLARED BEFORE THEM so it outlives their teardown (backlog JX).
	backplane::DeviceHost::SurfacePin pin_;
	std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::unique_ptr<DeviceControl> modeControl_;
	std::unique_ptr<DeviceControl> positionControl_;
	std::unique_ptr<ValueBox> modeBox_;
	std::unique_ptr<Knob> position_;
	std::unique_ptr<DeviceControl> unusedControl_;
	std::unique_ptr<SegmentedControl> unused_;
	/// Bands' own controls (count, filtering, slope, cuts 1..4), in that order.
	std::vector<std::unique_ptr<DeviceControl>> bandControls_;
	std::unique_ptr<ValueBox> bandsBox_;
	std::unique_ptr<ValueBox> filteringBox_;
	std::unique_ptr<SegmentedControl> slope_;
	std::unique_ptr<RackSpectrumStrip> spectrum_;
	std::unique_ptr<RackPositionBar> positionBar_;
	std::vector<std::unique_ptr<PhantomRail>> phantoms_;
	rack::parallel::Mode phantomMode_ = rack::parallel::Mode::Parallel;
	/// A drop zone pressed: the chain it made, whose add menu opens once its rail exists (0xFFFF: none).
	uint16_t pendingMenuFor_ = 0xFFFF;
	double laidOutMode_ = -1.0;
	double laidOutBands_ = -1.0;
	std::array<std::unique_ptr<Knob>, 4> cuts_;
	std::unique_ptr<SwitchChip> keyTrack_;
	std::vector<std::pair<visage::Frame*, rack::parallel::Param>> gated_;
	std::vector<std::unique_ptr<ChainRail>> rails_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	SurfaceLayers* presenter_ = nullptr;
	/// **A MODULATOR ARMED IN ONE OF THE RACK'S CHAINS LIGHTS WHAT FOLLOWS THE RACK** (backlog NZ (b)): the rail's arm,
	/// mirrored into the host's state as the Rack's export — borrowed and put back as found (`BorrowedArmRelay`).
	BorrowedArmRelay relay_;
	bool relayArm();
	ContainerPresets presets_;
};

} // namespace feathers::ui
