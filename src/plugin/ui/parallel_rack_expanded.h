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
#include "plugin/ui/macro_polarity.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/parallel_rack_card.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/rack_mode_views.h"
#include "plugin/ui/rack_view_adapter.h"
#include "plugin/ui/rack_pad_add.h"
#include "plugin/ui/rack_list_view.h"
#include "plugin/ui/rack_views.h"
#include "plugin/ui/rack_window_pane.h"
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
	static constexpr float kColumnWidth = 2.0f * 76.0f;
	/// A MACRO'S KNOB CELL, and beside it the arrow that arms it (`macroArmForTest`): the cell is `kColumnWidth / 2` wide.
	static constexpr float kMacroKnobWidth = 52.0f;
	/// Mix and Gain, the knobs before the macros in `knobBoundsForTest` order.
	static constexpr size_t kTopKnobCount = 2;
	static constexpr float kAddWidth = 64.0f;
	static constexpr float kAddHeight = 20.0f;

	/// THE RACK'S OWN CONTROLS THE PANEL SEATS: Mix, Gain and the eight macros.
	[[nodiscard]] std::vector<rack::parallel::Param> addressedParamsForTest() const;
	[[nodiscard]] size_t railCountForTest() const noexcept { return rails_.size(); }
	/// The knob at `i` in `addressedParamsForTest` order (Mix, Gain, M1..M8), and the divider and "+ Chain" boxes.
	[[nodiscard]] visage::Bounds knobBoundsForTest(size_t i) const {
		return i < kTopKnobCount ? knobs_[i]->bounds() : macroSlots_[i - kTopKnobCount]->bounds();
	}
	/// **THE MACRO AT `n` (0-based) AS THE SYNTH'S MACROS ARE SEATED**: one `MacroSlot` — the right-click menu (type, polarity,
	/// Rename), the field the name is typed into, the pad a Latch or Momentary becomes. The name, type and polarity are the rack's
	/// (`Rack::setMacroName`), saved in its chains' bodies.
	[[nodiscard]] MacroSlot& macroSlotForTest(size_t n) const { return *macroSlots_[n]; }
	/// The arrow of macro `n` (0-based), and the state it arms.
	[[nodiscard]] SourceArm& macroArmForTest(size_t n) const { return *macroArms_[n]; }
	[[nodiscard]] ModState& macroModForTest() noexcept { return macroMod_; }
	/// Runs the macro lend now, as the tick does.
	void syncMacroArmForTest() { syncMacroArm(); }
	[[nodiscard]] visage::Bounds dividerBoundsForTest() const { return dividerBounds(); }
	[[nodiscard]] visage::Bounds addBoundsForTest() const { return addBounds(); }
	[[nodiscard]] FxLaneRail& railForTest(size_t i) const { return *rails_[i]->rail; }
	[[nodiscard]] ChainStrip& stripForTest(size_t i) const { return *rails_[i]->strip; }
	/// THE MODE'S DROPDOWN AND THE POSITION'S KNOB (stage 2), on the row above Mix and Gain.
	/// THE TABS AND PADS VIEWS (`View`): the bar, the pads panel, the model, and which rails are shown.
	[[nodiscard]] RackTabBar& tabBarForTest() const { return *tabs_; }
	[[nodiscard]] RackListView& listForTest() const { return *list_; }
	[[nodiscard]] RackPadsPanel& padsForTest() const { return *pads_; }
	[[nodiscard]] RackViewAdapter& viewModelForTest() const { return *model_; }
	[[nodiscard]] RackPadAdd& padAddForTest() const { return *padAdd_; }
	[[nodiscard]] RackChainRename& chainRenameForTest() const { return *chainRename_; }
	/// **THE VIEWING AND PLAYING BOXES OPEN A MENU OF CHAIN NAMES, not a typed number**: the menu as built (rows in chain order; Playing has Position first).
	[[nodiscard]] visage::PopupMenu chainPickerMenuForTest(bool playing) { return chainPickerMenu(playing); }
	[[nodiscard]] RackStrikePulse& pulseForTest() { return pulse_; }
	[[nodiscard]] SegmentedControl& viewSegForTest() const { return *viewSeg_; }
	[[nodiscard]] SwitchChip& followChipForTest() const { return *followChip_; }
	[[nodiscard]] SegmentedControl& viewPairForTest() const { return *viewPair_; }
	/// **WHAT THE TABS VIEW PUTS IN FRONT** (`rack_front`): the control, the pane that holds the first device's window, and whether the window
	/// is showing it now (Tabs, Window chosen, and not itself held in a pane).
	[[nodiscard]] SegmentedControl& frontSegForTest() const { return *frontSeg_; }
	[[nodiscard]] RackWindowPane& paneForTest() const { return *pane_; }
	[[nodiscard]] bool paneOnForTest() const { return paneOn(); }
	/// **A PANE NEVER NESTS**: the pane that holds this window sets it, and the window then lays out as with Cards in front (and offers no choice).
	void setHostedInPane(bool hosted) {
		if (hosted != hostedInPane_) {
			hostedInPane_ = hosted;
			minimumSizeChanged();
			layOut();
		}
	}
	[[nodiscard]] bool hostedInPaneForTest() const noexcept { return hostedInPane_; }
	/// Runs the pane's follow as the tick does.
	bool syncPaneForTest() { return syncPane(); }
	[[nodiscard]] bool railShownForTest(size_t i) const { return rails_[i]->rail->isVisible(); }
	[[nodiscard]] ValueBox& modeBoxForTest() const { return *modeBox_; }
	[[nodiscard]] Knob& positionKnobForTest() const { return *position_; }
	[[nodiscard]] SegmentedControl& unusedChipForTest() const { return *unused_; }
	[[nodiscard]] ValueBox& playingBoxForTest() const { return *playingBox_; }
	[[nodiscard]] ValueBox& viewedBoxForTest() const { return *viewedBox_; }
	[[nodiscard]] ValueBox& pageBoxForTest() const { return *pageBox_; }
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
	[[nodiscard]] ValueBox& slopeForTest() const { return *slope_; }
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
		/// Declared AFTER the rail it borrows, so it lets go first.
		BorrowedArmRelay macroRelay;
	};

	void layOut();
	[[nodiscard]] visage::PopupMenu chainPickerMenu(bool playing);
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
	[[nodiscard]] visage::Bounds leftColumnFoot() const;
	/// How long the Tabs view's column wants to be: every chain's tab at its cap (`RackTabBar::kMaxLength`), then limited to the window's height.
	[[nodiscard]] float tabsColumnLength() const;
	/// How wide the chain selector's column is: the tabs' thickness, or the list's width.
	[[nodiscard]] float selectorWidth() const;
	/// The width the chain in front's rail asks the window for: its cards' length along the flow (capped) in every view but Strips, else the standard column.
	[[nodiscard]] float frontRailMinWidth() const;
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
	/// Mix and Gain.
	std::vector<std::unique_ptr<Knob>> knobs_;
	/// **THE EIGHT MACROS, EACH A `MacroSlot`** over its control seen through the rack's polarity. The hooks and the naming hold
	/// `alive_` weakly: a menu is parented to the root frame and can be picked after this window is gone.
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
	std::vector<std::unique_ptr<BipolarMacro>> macroViews_;
	std::vector<std::unique_ptr<MacroSlot>> macroSlots_;
	[[nodiscard]] MacroMenuHooks macroHooks(size_t n);
	[[nodiscard]] MacroSlot::Naming macroNaming(size_t n);
	[[nodiscard]] rack::parallel::Rack* rackNow() const;
	/// Re-seat any slot whose type or name moved under it. True when one did.
	bool syncMacroSlots();
	std::unique_ptr<DeviceControl> modeControl_;
	std::unique_ptr<DeviceControl> positionControl_;
	std::unique_ptr<ValueBox> modeBox_;
	std::unique_ptr<DeviceControl> viewControl_;
	std::unique_ptr<DeviceControl> playingControl_;
	std::unique_ptr<SegmentedControl> viewSeg_;
	std::unique_ptr<RackViewPairControl> viewPairControl_;
	std::unique_ptr<SegmentedControl> viewPair_;
	std::unique_ptr<DeviceControl> frontControl_;
	std::unique_ptr<SegmentedControl> frontSeg_;
	std::unique_ptr<ValueBox> playingBox_;
	std::unique_ptr<DeviceControl> viewedControl_;
	std::unique_ptr<DeviceControl> pageControl_;
	std::unique_ptr<DeviceControl> followControl_;
	std::unique_ptr<SwitchChip> followChip_;
	std::unique_ptr<ValueBox> viewedBox_;
	std::unique_ptr<ValueBox> pageBox_;
	std::unique_ptr<Knob> position_;
	std::unique_ptr<DeviceControl> unusedControl_;
	std::unique_ptr<SegmentedControl> unused_;
	/// Bands' own controls (count, filtering, slope, cuts 1..4), in that order.
	std::vector<std::unique_ptr<DeviceControl>> bandControls_;
	std::unique_ptr<ValueBox> bandsBox_;
	std::unique_ptr<ValueBox> filteringBox_;
	std::unique_ptr<ValueBox> slope_;
	std::unique_ptr<RackSpectrumStrip> spectrum_;
	std::unique_ptr<RackPositionBar> positionBar_;
	/// THE TABS AND PADS VIEWS: the model and the strike pulse are declared BEFORE the widgets that read them, so they outlive them.
	std::unique_ptr<RackViewAdapter> model_;
	RackStrikePulse pulse_;
	std::unique_ptr<RackPadAdd> padAdd_;
	std::unique_ptr<RackChainRename> chainRename_;
	std::unique_ptr<RackTabBar> tabs_;
	std::unique_ptr<RackListView> list_; ///< after `chainRename_` (it holds a reference to it)
	std::unique_ptr<RackPadsPanel> pads_;
	int laidOutView_ = -1;
	size_t laidOutViewed_ = static_cast<size_t>(-1);
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
	/// **DECLARED AFTER THE RAILS, SO IT IS DESTROYED BEFORE THEM**: the device window it holds is on a rail's modulation list.
	std::unique_ptr<RackWindowPane> pane_;
	bool hostedInPane_ = false;
	visage::Point laidOutPane_ { -1.0f, -1.0f };
	/// Whether the Tabs view is showing the front device's window: Tabs, the control says Window, and this window is not itself in a pane.
	[[nodiscard]] bool paneOn() const;
	/// Follow the front chain's first device (and take the first card off the rail's column while the pane shows it). True when anything moved.
	bool syncPane();
	/// BEFORE THE RAILS FOLLOW THEIR CHAINS: drop pane content whose chain went or changed.
	bool validatePane();
	/// The rail of the viewed chain, or null.
	[[nodiscard]] ChainRail* viewedRail() const;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	SurfaceLayers* presenter_ = nullptr;
	/// **A MODULATOR ARMED IN ONE OF THE RACK'S CHAINS LIGHTS WHAT FOLLOWS THE RACK** (backlog NZ (b)): the rail's arm,
	/// mirrored into the host's state as the Rack's export — borrowed and put back as found (`BorrowedArmRelay`).
	BorrowedArmRelay relay_;
	/// **THE EIGHT MACROS' ARROWS ARM HERE, and the rails follow** (a Chain's macro row, for a rack of chains). A macro is the same
	/// source in every chain (`Rack::setMacro` fans it out), and each rail's state is its own, so the arrow arms this one state
	/// (a child of the host's: one hand, one arm) and `syncMacroArm` lends its source to every rail. Declared before the arrows.
	ModState macroMod_;
	std::vector<std::unique_ptr<SourceArm>> macroArms_;
	bool relayArm();
	/// **A MACRO ARMED HERE IS ARMED IN EVERY CHAIN** (see `macroMod_`): each rail's state is borrowed for the macro's source
	/// while the hand holds it, and put back as found when it lets go.
	void syncMacroArm();
	ContainerPresets presets_;
};

} // namespace feathers::ui
