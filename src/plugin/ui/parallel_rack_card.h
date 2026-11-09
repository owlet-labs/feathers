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
#include "device/modulatable.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/card_meter.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/rack_mode_views.h"
#include "rack/parallel_rack.h"

#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// THE RACK'S CARD (backlog NO, stage 1): a row per chain — what it holds, its level, mute and solo — and the
// rack's Mix and Gain. The chains themselves are edited in the window (`ParallelRackExpanded`), one rail per chain.
//
// **A CHAIN'S CONTROLS ARE ITS BODY'S, AND ITS SLOT PARAMS ARE THEIR DESTINATIONS** (backlog NO item 2): `ChainControl`
// is a `device::Modulatable` over the rack's own surface, asked afresh through the host on every call (the handle is
// contemporaneous, CLAUDE.md's ABA rule), and addressed by the chain's STABLE id so a removal does not re-point it. Its
// value is the body's (the single truth); its ROUTES are the slot param the chain holds for life (`Rack::slotOf`), so
// the glow, the reach and the amount drag are a `DeviceControl`'s over that param. A chain past the last (`kChainSlots`) has no slot:
// it is not a destination, and its tooltip says why (`kNoSlotWhy`). Not a host parameter: no slot in the host's pool.

namespace feathers::ui {

class ChainControl final : public device::Modulatable, public RoutedControl {
public:
	enum class Kind : uint8_t { Level, Mute, Solo };
	ChainControl(backplane::DeviceHost& host, DeviceInstanceId rack, uint16_t chain, Kind kind) noexcept
	    : host_(host), rack_(rack), chain_(chain), kind_(kind) {}

	const device::ParamDesc& desc() const override;
	double value() const override;
	/// A HAND'S WRITE: through `apply`, and ONE UNDO STEP — a whole gesture is one, a write outside a gesture is one.
	void setValue(double value) override;
	/// THE SLOT PARAM'S (see the banner); nothing for a phantom or a chain with no slot.
	device::ModDisplay display() const override;
	float amountFor(device::Source source) const override;
	void setAmount(float amount) override;
	void beginAmountGesture() override;
	void endAmountGesture() override;
	void beginGesture() override;
	void endGesture() override;
	std::string text() const override;
	ModState* routingState() const override { return mod_; }
	/// **ITS HOST SLOT IS ITS SLOT PARAM'S** (backlog NO item 2 follow-up, the nested macros' `exposeToHost` road): a DAW
	/// automating it writes the slot param's base, which the Rack adopts into its body at the holder's service beat
	/// (`Rack::adoptOwnFields`). A chain with no slot answers "no pool", and every verb refuses.
	[[nodiscard]] device::HostSlotStatus hostSlot() const override;
	bool exposeToHost(std::string* displaced) override;
	bool unpinFromHost() override;
	[[nodiscard]] uint64_t hostSlotGeneration() const noexcept override { return host_.hostSlotGeneration(); }

	/// Where the undo steps go, and the panel's routing state (either may be null: no undo, nothing armed).
	void setModulation(ModState* mod, EditHistory* history) noexcept {
		mod_ = mod;
		history_ = history;
		if (slotControl_ != nullptr) {
			slotControl_->setModulation(mod, history); // re-pointed in place: a gesture may hold it
		}
	}
	/// This chain's published output peak (linear, held and falling), false before it has run or when it is gone.
	[[nodiscard]] bool peak(float& out) const;
	/// THE WRITE ITSELF, recording nothing — what an undo step replays.
	void apply(double value);

	[[nodiscard]] uint16_t chain() const noexcept { return chain_; }
	/// **THE CHAIN'S SLOT** (`Rack::slotOf`), `kNoChainSlot` for a phantom, a gone chain or one past the last slot.
	[[nodiscard]] uint8_t slot() const;

	/// **A ROLE NO CHAIN FILLS YET** (§2a): this control stands for a chain that does not exist, and `materialise` makes it
	/// — adds the chain and returns its id — on the FIRST EDIT. A click edits at once; a drag is held (`value` answers
	/// with it) until the hand lets go, so the strip a hand is dragging is not rebuilt under it mid-gesture.
	void setMaterialise(std::function<uint16_t()> materialise) { materialise_ = std::move(materialise); }
	[[nodiscard]] bool phantom() const;

private:
	[[nodiscard]] rack::parallel::Rack* rack() const;
	/// THE SLOT PARAM AS A `DeviceControl`, built when the chain's slot is first asked and kept (a chain's slot is its
	/// for life); null with no slot.
	[[nodiscard]] DeviceControl* slotControl() const;
	void record(double before, double after);
	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	uint16_t chain_;
	Kind kind_;
	EditHistory* history_ = nullptr;
	ModState* mod_ = nullptr;
	mutable std::unique_ptr<DeviceControl> slotControl_;
	mutable uint8_t slotControlFor_ = rack::parallel::kNoChainSlot;
	/// The slot control a gesture opened the host's half on, closed by the same one at its end.
	DeviceControl* hostGesture_ = nullptr;
	bool gesture_ = false;
	double before_ = 0.0;
	std::function<uint16_t()> materialise_;
	/// A phantom's value while a drag holds it (NaN: none).
	double pending_ = std::numeric_limits<double>::quiet_NaN();
};

/// **A CHAIN ADDED OR REMOVED, AS ONE UNDO STEP** — the chain whole (`Rack::ChainSnapshot`), so an undone removal
/// comes back with its devices, controls and id. The rack is re-asked through the host at every replay (its
/// handle is contemporaneous).
class ChainPresenceEdit final : public EditRecord {
public:
	/// A REMOVAL (`added` false) is recorded just after it, while the routes into the chain's slot are still in the
	/// host (they go at its next service beat): they are kept here, so an undo puts them back (backlog NO item 2).
	ChainPresenceEdit(backplane::DeviceHost& host, DeviceInstanceId rack, size_t index,
	                  rack::parallel::Rack::ChainSnapshot chain, bool added);
	void undo() const override { added_ ? take() : put(); }
	void redo() const override { added_ ? put() : take(); }
	const char* verb() const override { return added_ ? "Add chain" : "Remove chain"; }

	/// **A ROUTE INTO THE CHAIN'S LEVEL, MUTE OR SOLO** — by which of the three, not by slot: a chain put back may take
	/// another slot (its own is still awaiting the prune), and its routes follow it there.
	struct SlotRoute {
		backplane::Source source;
		rack::parallel::SlotControl which;
		float amount = 0.0f;
		device::Transfer transfer = device::Transfer::Lin;
	};
	[[nodiscard]] const std::vector<SlotRoute>& routesForTest() const noexcept { return routes_; }

private:
	void put() const;
	void take() const;
	/// The routes into slot `slot`'s three params, as the host holds them now.
	void capture(uint8_t slot) const;
	backplane::DeviceHost& host_;
	DeviceInstanceId rack_;
	size_t index_;
	rack::parallel::Rack::ChainSnapshot chain_;
	bool added_;
	mutable std::vector<SlotRoute> routes_;
};

/// **A VERTICAL LEVEL FADER** (backlog OC: the level fader is VERTICAL and sits side by side with the meter) —
/// the collapsed chain strip's fader. A track filled from its foot to the level, unity ticked; a vertical drag sets the
/// level (`kSpan` pixels for the whole range, up raises it), a double-click returns it to unity, and one gesture is one
/// undo step through the control (`ChainControl` records it). It offers the level control on its face and says its tooltip.
class ChainFader final : public visage::Frame, public TooltipSource {
public:
	explicit ChainFader(ChainControl& control);
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&control_); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	static constexpr float kSpan = 120.0f;

private:
	ChainControl& control_;
	float startY_ = 0.0f;
	double startValue_ = 0.0;
	bool dragging_ = false;
};

/// **ONE CHAIN'S STRIP** — its level as a pill (the readout in dB), its mute and its solo, and in the window a
/// small vertical level meter and a × that removes it (backlog OC: a CHANNEL STRIP, which works while the chain's rail
/// is collapsed). The card puts a label of what the chain holds to its left; the window's rail says it above.
///
/// **TWO LAYOUTS FROM ONE WIDTH.** A row (`kHeight` tall) when wide: pill, M, S, meter, ×. A column when narrower than
/// `kColumnBelow` — the collapsed rail's 34 px: the short label, M, S, then the VERTICAL level fader side by side with the 1 px meter, filling what is left,
/// and the × at the foot. The same controls and the same children either way; only their bounds move.
///
/// **THE METER IS MK's** (`card_meter.h`): the reading and marks arithmetic and the one-pixel `CardOutputMeter` frame,
/// one pixel thick as there, run vertically (the level rises from the foot, an over falls from the top). Its peak
/// is the rack's per-chain one (`Rack::chainPeak`), published by the audio thread; the card's strips draw it too.
class ChainStrip final : public visage::Frame, public TooltipSource {
public:
	ChainStrip(backplane::DeviceHost& host, DeviceInstanceId rack, uint16_t chain, std::string caption,
	           std::function<void(uint16_t)> onRemove = {});
	~ChainStrip() override;

	void resized() override;
	void draw(visage::Canvas& canvas) override;
	/// What the chain holds, drawn at the left; empty draws none (the window's rail says it).
	void setSummary(std::string summary);
	/// **WHAT THE SUMMARY'S HOVER SAYS** — a phantom's "empty, passes the side", which the row has no room for (§2a).
	void setSummaryTooltip(std::string tooltip) { summaryTooltip_ = std::move(tooltip); }
	/// **A STRIP HEADING A RAIL** lays itself out as the rail's column when narrow, × or not — a phantom's rail has
	/// nothing to remove, and must still be the same channel strip a real empty chain's is (§2a).
	void setInRail(bool inRail) {
		inRail_ = inRail;
		resized();
	}
	const char* tooltipAt(visage::Point p) const override {
		return !summaryTooltip_.empty() && p.x < summaryWidth() ? summaryTooltip_.c_str() : nullptr;
	}
	void offeredControls(std::vector<Offered>&) const override {} // its three controls offer themselves
	/// The summary's room, and the width its text takes — a case asks that the first holds the second.
	[[nodiscard]] float summaryWidth() const { return summary_.empty() ? 0.0f : std::round(static_cast<float>(width()) * kSummaryWidth); }
	[[nodiscard]] float summaryTextWidthForTest() const;
	/// The short name the column layout draws at its head ("1"); the row layout leaves it to the rail's heading.
	void setLabel(std::string label);
	/// **A PHANTOM STRIP** (§2a): its three controls make the chain on their first edit (`ChainControl::setMaterialise`).
	void setMaterialise(const std::function<uint16_t()>& materialise) {
		level_.setMaterialise(materialise);
		muted_.setMaterialise(materialise);
		soloed_.setMaterialise(materialise);
	}
	[[nodiscard]] ChainControl& muteControlForTest() noexcept { return muted_; }
	[[nodiscard]] ChainControl& levelControlForTest() noexcept { return level_; }
	[[nodiscard]] ChainControl& soloControlForTest() noexcept { return soloed_; }
	/// Where its three controls record their undo steps, and the panel state their routes are armed from.
	void setModulation(ModState* mod, EditHistory* history) {
		level_.setModulation(mod, history);
		muted_.setModulation(mod, history);
		soloed_.setModulation(mod, history);
		// AND THE WIDGETS RE-WATCH NOW (backlog PM): a pill or chip still on the OLD state's watcher list would unwatch
		// into it later, after its owner may have let it go — they otherwise re-subscribe only at their next paint.
		pill_->modulationChanged();
		mute_->modulationChanged();
		solo_->modulationChanged();
	}
	/// Re-read the level's readout and the meter; true when either changed.
	bool refresh();

	[[nodiscard]] uint16_t chain() const noexcept { return level_.chain(); }
	[[nodiscard]] FaderPill& levelPill() noexcept { return *pill_; }
	[[nodiscard]] ChainFader& levelFader() noexcept { return *fader_; }
	/// THE LEVEL CONTROL'S FACE AS LAID OUT NOW: the vertical fader in the column, the pill in the row.
	[[nodiscard]] ControlFace& levelFace() noexcept {
		return column_ ? static_cast<ControlFace&>(*fader_) : static_cast<ControlFace&>(*pill_);
	}
	[[nodiscard]] visage::Frame& levelFrame() noexcept {
		return column_ ? static_cast<visage::Frame&>(*fader_) : static_cast<visage::Frame&>(*pill_);
	}
	[[nodiscard]] SwitchChip& muteChip() noexcept { return *mute_; }
	[[nodiscard]] SwitchChip& soloChip() noexcept { return *solo_; }
	[[nodiscard]] const std::string& summaryForTest() const noexcept { return summary_; }
	/// WHAT THE METER IS DRAWING and where, for the cases that read the value instead of the picture.
	[[nodiscard]] const CardMeterMarks& meterMarksForTest() const noexcept { return meter_->marks(); }
	[[nodiscard]] CardMeterReading meterReadingForTest() const noexcept { return reading_; }
	[[nodiscard]] visage::Bounds meterBoundsForTest() const { return meter_->bounds(); }
	[[nodiscard]] bool meterShownForTest() const { return meter_->isVisible(); }
	[[nodiscard]] visage::Bounds removeBoundsForTest() const noexcept { return removeBounds_; }
	[[nodiscard]] bool columnLayoutForTest() const noexcept { return column_; }
	/// Press the ×, as a hand does (the window removes the chain on its next tick).
	void pressRemoveForTest() {
		if (onRemove_) {
			onRemove_(level_.chain());
		}
	}

	static constexpr float kHeight = 20.0f;
	static constexpr float kChipWidth = 22.0f;
	static constexpr float kGap = 4.0f;
	static constexpr float kSummaryWidth = 0.42f; ///< of the strip, when a summary is drawn
	static constexpr float kMeterWidth = 1.0f; ///< MK's one-pixel line, run vertically
	static constexpr float kFaderWidth = 14.0f; ///< the column's vertical fader, beside the meter
	/// Narrower than this and the strip lays itself out as a column.
	static constexpr float kColumnBelow = 80.0f;
	/// The column's rows: the label, then M, S and the pill, each `kColumnRow` apart, the × at the foot.
	static constexpr float kLabelHeight = 14.0f;
	static constexpr float kColumnRow = 20.0f;
	/// The shortest the column may be and still hold its rows and a meter worth reading.
	static constexpr float kColumnMinimumHeight = kLabelHeight + 2.0f * kColumnRow + kGap + 60.0f + kGap + kColumnRow;

private:
	ChainControl level_;
	ChainControl muted_;
	ChainControl soloed_;
	std::unique_ptr<FaderPill> pill_;
	std::unique_ptr<ChainFader> fader_;
	std::unique_ptr<SwitchChip> mute_;
	std::unique_ptr<SwitchChip> solo_;
	std::unique_ptr<CardOutputMeter> meter_;
	CardMeterReading reading_ {};
	std::function<void(uint16_t)> onRemove_;
	std::string summary_;
	std::string summaryTooltip_;
	bool inRail_ = false;
	std::string label_;
	bool column_ = false;
	visage::Bounds removeBounds_ {};
	visage::Bounds meterTrack_ {};
};

/// **A PHANTOM ROLE MADE REAL** (§2a) — the rack's chains filled up to `role` (each an undo step into `history`, which may
/// be null), and the id of the chain at `role`, or 0xFFFF. A hand's edit only; never a mode change (see its comment).
uint16_t materialiseRole(backplane::DeviceHost& host, DeviceInstanceId rack, size_t role, EditHistory* history);

/// WHAT A CHAIN HOLDS, by its devices' names in chain order ("Filter › OWLTT"), or "empty".
[[nodiscard]] std::string chainSummary(const rack::InnerChain* chain);

class ParallelRackCard final : public FxCard {
public:
	ParallelRackCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	ParallelRackCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawHeadExtras(visage::Canvas& canvas) override;
	std::string headCaption() const override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;

	const device::Descriptor* descriptor() const override { return &rack::parallel::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 6.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// The Mode dropdown's width on its row (stage 2).
	static constexpr float kModeWidth = 96.0f;
	/// The Unused chip beside it.
	static constexpr float kUnusedWidth = 72.0f;

	/// The controls the card seats: Mode, Position, Mix and Gain (the enable is the head's).
	[[nodiscard]] std::vector<rack::parallel::Param> addressedParamsForTest() const;
	/// THE MODE'S DROPDOWN and THE POSITION'S KNOB (stage 2) — the knob shown only where the mode reads it.
	[[nodiscard]] ValueBox& modeBoxForTest() const { return *modeBox_; }
	[[nodiscard]] Knob& positionKnobForTest() const { return *position_; }
	[[nodiscard]] SegmentedControl& unusedChipForTest() const { return *unused_; }
	[[nodiscard]] SegmentedControl& slopeForTest() const { return *slope_; }
	[[nodiscard]] RackSpectrumStrip& spectrumForTest() const { return *spectrum_; }
	[[nodiscard]] RackPositionBar& positionBarForTest() const { return *positionBar_; }
	/// THE PHANTOM STRIPS for roles no chain fills (§2a): an empty chain's face, made real by its first edit.
	[[nodiscard]] size_t placeholderCountForTest() const noexcept { return phantoms_.size(); }
	[[nodiscard]] ChainStrip& phantomForTest(size_t m) const { return *phantoms_[m]; }
	/// THE ROLE A MODE GIVES CHAIN `role` ("Mid", "Band 3"), and what an empty chain in it passes ("empty, passes the side").
	[[nodiscard]] static std::string roleName(rack::parallel::Mode mode, size_t role);
	[[nodiscard]] static std::string placeholderText(rack::parallel::Mode mode, size_t role);
	/// The height a segmented control takes on a row.
	static constexpr float kSegmentHeight = 18.0f;
	/// **THE CARD FITS ONE CARD UNIT, IN EVERY MODE** (the card was too tall in a horizontal chain, whose
	/// slot is `kCardUnitHeight`). The corners are dragged on the spectrum strip and their knobs and the Key Track switch
	/// are the window's; the chain rows stop where the unit does, and a line says how many more the window holds.
	[[nodiscard]] size_t hiddenRowsForTest() const noexcept { return laidOut_.overflow; }
	[[nodiscard]] std::string overflowTextForTest() const;
	[[nodiscard]] ValueBox& bandsBoxForTest() const { return *bandsBox_; }
	[[nodiscard]] const std::vector<std::unique_ptr<ChainStrip>>& stripsForTest() const { return strips_; }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(rack::parallel::Param p) const;
	[[nodiscard]] rack::parallel::Rack* rack() const;
	void layOut();
	/// Rebuild the strips when the chains changed; re-read their summaries. True when anything moved.
	bool follow();
	/// **A CONTROL IS HIDDEN, NOT GREYED, WHERE THE MODE DOES NOT READ IT** (`rack::parallel::controlShown`): the
	/// Position outside Select and Blend, Unused outside the role modes. True when anything changed.
	bool showForMode();
	/// **WHAT THE CARD LAYS OUT IN THIS STATE** — the rows a mode adds, and the placeholders for missing roles.
	struct Plan {
		rack::parallel::Mode mode = rack::parallel::Mode::Parallel;
		bool bands = false;
		bool spectrum = false;
		bool bar = false;
		size_t missing = 0;
		size_t strips = 0;
		/// The chain rows (strips, then phantoms) drawn, and how many the unit has no room for.
		size_t rows = 0;
		size_t overflow = 0;
		friend bool operator==(const Plan&, const Plan&) = default;
	};
	[[nodiscard]] Plan plan() const;
	[[nodiscard]] float bodyHeight() const;
	/// The rows above the chains a plan lays out: the mode row, and Bands', the spectrum's and the bar's where shown.
	[[nodiscard]] static float fixedRowsHeight(const Plan& p);
	/// A PHANTOM'S FIRST EDIT: the chain made (one undo step), its id returned. Never on a mode change (§2a).
	uint16_t materialiseChain(size_t role);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<Knob> mix_;
	std::unique_ptr<Knob> gain_;
	std::unique_ptr<ValueBox> modeBox_;
	std::unique_ptr<Knob> position_;
	std::unique_ptr<SegmentedControl> unused_;
	std::unique_ptr<ValueBox> bandsBox_;
	std::unique_ptr<ValueBox> filteringBox_;
	std::unique_ptr<SegmentedControl> slope_;
	/// "+3 more chains in the window" — drawn in the row after the last one shown, when any are not.
	std::unique_ptr<visage::Frame> overflow_;
	std::unique_ptr<RackSpectrumStrip> spectrum_;
	std::unique_ptr<RackPositionBar> positionBar_;
	Plan laidOut_ {};
	/// **THE EXTENT THE HOST LAST PLACED THIS CARD AT** (recorded in `resized`), so a change of chains — which arrives
	/// through the running body and is no layout of the host's — tells the host once (`FxCard::extentChanged`).
	float placedExtent_ = -1.0f;
	std::vector<std::unique_ptr<ChainStrip>> phantoms_;
	/// The widgets `showForMode` shows and hides, and the control each one is.
	std::vector<std::pair<visage::Frame*, rack::parallel::Param>> gated_;
	std::vector<std::unique_ptr<ChainStrip>> strips_;
	std::vector<uint16_t> ids_;
	EditHistory* history_ = nullptr;
	ModState* mod_ = nullptr;
};

} // namespace feathers::ui
