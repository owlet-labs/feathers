/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/spectrum_plot.h"
#include "plugin/ui/spectrum_source.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <visage/ui.h>

#include "plugin/ui/pill_stack.h" // complete, for the unique_ptr<PillStack> members (macOS CI: incomplete-type delete)
#include "plugin/ui/switch_chip.h" // complete: VocodeAnalysisColumn holds one
#include "plugin/ui/value_box.h"

// THE SPECTRUM EDITOR: the tab on Explore's
// bottom edge with the master switch, and the FLOATING PANEL it opens — the plot (spectrum_plot.h, D2) over the operator
// row. After auditioning the drawer that preceded it, a pop-up window proved better than a drawer constrained to
// Explore. So the editor is an `ExpandedView`, presented through the editor's surfaces as
// every device window is (the in-editor overlay by default, an OS window only if the popout preference asks), and the
// drawer it replaced is gone. On Additive only — on every other engine the Spectral effect device supersedes drawers
// (design R9), so ExploreStage hides the tab and the editor closes the panel.
//
// LAYOUT IS CONSTANTS, deliberately, and all of them are here, so moving the row's height or a panel's columns is an edit
// to one number.

namespace feathers::ui {

class Knob;
class ModState;
class ModeValueCell;
class EditHistory;
class Notifier;
class ParamControl;
class SegmentedControl;
class SwitchChip;
class PillStack; // complete type included below: a unique_ptr<PillStack> member needs it where the panel is destroyed (Apple libc++ refuses an incomplete delete)
class FaderPill;
class ValueBox;

/// **OE: THE ENGINE VOCODER'S ANALYSIS CHOICES IN ONE CELL** (additive design §9.4): Resolution as a ValueBox with its menu (every
/// stop on demand before one is applied), Freeze and Key Track as two-state SwitchChips under it. Engine-wide controls shown in each
/// sound's Vocode section, because the one analysis is what every sound's Vocode reads; their tooltips say so.
class VocodeAnalysisColumn final : public visage::Frame {
public:
	VocodeAnalysisColumn(device::Modulatable& resolution, device::Modulatable& stereo, device::Modulatable& freeze,
	                     device::Modulatable& keyTrack)
	    : resolution_(resolution, "Resolution"), stereo_(stereo, "Stereo"), freeze_(freeze, "Freeze"), keyTrack_(keyTrack, "Key") {
		addChild(&resolution_);
		addChild(&stereo_);
		addChild(&freeze_);
		addChild(&keyTrack_);
		resolution_.setCompact(true); // one row, caption beside the number: a menu this small does not earn two lines
		stereo_.setCompact(true);
	}
	/// The cell's caption, at its bottom like every section's ("Partials", "Tone"): it names the engine-wide analysis the
	/// controls above it belong to. The pill stack beside it is the envelope follower's (its own title says so).
	static constexpr const char* kCaption = "Analysis";
	void resized() override {
		// THE COMPACT BOXES ON TOP (one row each: Resolution, OE2's Stereo), the two chips under them side by side, the caption on
		// the bottom line.
		const float line = SwitchChip::kHeight;
		const float gap = 4.0f;
		resolution_.setBounds(0.0f, 0.0f, width(), ValueBox::kCompactHeight);
		stereo_.setBounds(0.0f, ValueBox::kCompactHeight + gap, width(), ValueBox::kCompactHeight);
		const float chips = 2.0f * (ValueBox::kCompactHeight + gap);
		const float half = (width() - gap) * 0.5f;
		freeze_.setBounds(0.0f, chips, half, line);
		keyTrack_.setBounds(half + gap, chips, width() - half - gap, line);
	}
	void draw(visage::Canvas& canvas) override;
	/// The analysis is live while some sound vocodes; Key Track only while Freeze holds as well (the honesty rule).
	void setLive(bool live, bool keyLive) {
		resolution_.setEnabled(live);
		stereo_.setEnabled(live);
		freeze_.setEnabled(live);
		keyTrack_.setEnabled(keyLive);
	}
	ValueBox& resolution() { return resolution_; }
	ValueBox& stereo() { return stereo_; }
	SwitchChip& freeze() { return freeze_; }
	SwitchChip& keyTrack() { return keyTrack_; }

private:
	ValueBox resolution_;
	ValueBox stereo_;
	SwitchChip freeze_;
	SwitchChip keyTrack_;
};



/// **A TWO-STATE SWITCH OVER STATE THAT IS NOT A PARAMETER** — the spectral master switch and the drawn curve's switch
/// (SpectralState), and the Draw tool (per-viewer). A `device::Modulatable` so a `SwitchChip` can draw it; never a
/// destination (design §3.1: the master switch is not automatable), so it offers no amount and no gesture to a host. THE
/// HOST'S UNDO NEVER SEES STORED STATE, so with a history (`setHistory`) each flip is one step of the editor's
/// (StoredSwitchEdit); without one (the Draw tool, which is not patch state) it records nothing.
class StoredSwitch final : public device::Modulatable {
public:
	StoredSwitch(const char* key, const char* name, const char* tooltip, std::function<bool()> get,
	             std::function<void(bool)> set);
	const device::ParamDesc& desc() const override { return desc_; }
	double value() const override { return get_() ? 1.0 : 0.0; }
	void setValue(double value) override;
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return get_() ? "On" : "Off"; }
	void setHistory(EditHistory* history) { history_ = history; }

private:
	device::ParamDesc desc_;
	std::function<bool()> get_;
	std::function<void(bool)> set_;
	EditHistory* history_ = nullptr;
};

/// **THE TAB**: Explore's bottom edge, the word SPECTRUM (lit while the master switch is on; a click opens or closes the
/// spectrum panel) over the master switch's chip, usable with the panel shut (design §3.1). Its caret lights while the panel
/// is open — which the editor (Gui) tells it, since the editor, not Explore, presents and owns the panel.
class SpectrumTab : public visage::Frame {
public:
	static constexpr float kWidth = 84.0f;  ///< a flank column (ExploreStage::kCornerWidth, 74) and a little: it sits under one
	static constexpr float kHeight = 40.0f; ///< the label's line and the chip's
	static constexpr float kLabelHeight = 18.0f;

	SpectrumTab(ParamBridge& bridge, std::function<void()> togglePanel);
	~SpectrumTab() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	/// Whether the panel is open, for the caret. Told by the editor through ExploreStage::setSpectrumOpen.
	void setOpen(bool open);
	/// The label lit (the master switch on); compared each frame by `sync`, which repaints only on a change.
	void sync(bool enabled);

	/// The master switch's flips are steps of the editor's undo (StoredSwitch).
	void setHistory(EditHistory* history) { master_.setHistory(history); }

	[[nodiscard]] SwitchChip& masterForTest() { return *chip_; }
	[[nodiscard]] bool litForTest() const { return lit_; }
	/// The caret lit: the panel open.
	[[nodiscard]] bool caretLitForTest() const { return open_; }
	/// A press on the label, as a hand makes it.
	void clickLabelForTest() { toggle_(); }
	[[nodiscard]] visage::Bounds labelBounds() const { return { 0.0f, 0.0f, width(), kLabelHeight }; }

private:
	ParamBridge& bridge_;
	std::function<void()> toggle_;
	StoredSwitch master_;
	std::unique_ptr<SwitchChip> chip_;
	bool open_ = false;
	bool lit_ = true;
};

/// **THE PANEL**: the spectrum editor as a floating window over the editor — the plot at the top, the operator row under it,
/// the master switch on the window header's enable chip.
///
/// PER SOUND: every sound has its own drawer settings, and the row shows and edits the SELECTED sound's —
/// the flank knobs' pattern: eight sets of controls, each bound at construction to its own sound's params, one shown per
/// selection (a rebindable control is a control that can be pointing somewhere its label does not say). The voice plays
/// the sounds' drawers blended by weight (Synth::drawerFor), and the plot draws that blend.
///
/// THE ENGINE'S PLOT IS WANTED EXACTLY WHILE THIS IS SHOWN (ParamBridge::plotWanted): set each frame it is ticked, cleared
/// when it is destroyed — which is every road out of it: ✕, Esc, the tab, leaving Additive (the editor dismisses it), and
/// the editor closing. It is fresh on every open, so it takes no frame published before it (SpectrumPlot).
class SpectrumPanel : public ExpandedView {
public:
	/// THE LAYOUT, in one place (see the file note).
	static constexpr float kPad = 10.0f;
	static constexpr float kPanelGap = 8.0f;
	static constexpr float kPanelPad = 6.0f;
	static constexpr float kCellGap = 4.0f; ///< between neighbouring pill stacks and cells in a section (they were flush)
	static constexpr float kHeadHeight = 16.0f; ///< SwitchChip::kHeight
	static constexpr float kKnobFace = 26.0f;
	static constexpr float kKnobColumn = 52.0f;
	static constexpr float kModeWidth = 180.0f;    ///< Filter Mode's five segments, in the Filter panel's head
	static constexpr float kFilterTabsWidth = 100.0f; ///< F2's "Filter 1 | Filter 2" tabs, first in a filter panel's head
	static constexpr float kLinkWidth = 40.0f;     ///< Formant Shift's Link chip, in its panel's head
	static constexpr float kFlattenWidth = 58.0f;
	static constexpr float kDrawWidth = 44.0f; ///< the Draw tool's chip, after Flatten
	/// THE PANEL'S MINIMUM, logical. WIDTH: 940 held the v1 operators' row on one line (912 and the pads); with the later
	/// five (spectral-ops, 2026-10-12) the row is 1356 at its natural column, past the 1240 canvas, so it is TWO rows at every
	/// width the canvas allows and 940 stays the width. HEIGHT: tall enough that the narrowest width the sweep gives the panel
	/// (374, five rows) still leaves the plot over 80 px (480 left it none once the row grew); under the 1240x700 canvas, so
	/// it always opens whole.
	static constexpr float kMinimumWidth = 940.0f;
	static constexpr float kMinimumHeight = 640.0f; ///< 600, plus A-PH's strip (kStripHeight and a gap) so the plot keeps 80 px
	/// The operator row: a head, a knob column, the panel's pads.
	[[nodiscard]] static float rowHeight();
	/// THE ROW AS THIS PANEL LAYS IT: `rowHeight()`, or the DENSE row (a PillStack's cell under the head) in a dense window.
	[[nodiscard]] float rowHeightNow() const;
	/// THE DENSE WINDOW'S HEADER STRIP above the plot (a full ValueBox's height), and its gap to the plot.
	static constexpr float kStripHeight = 28.0f;
	static constexpr float kPhasesBoxWidth = 112.0f; ///< A-PH's Phases box in the drawer's strip ("Sub priority" fits)
	static constexpr float kStripBoxWidth = 72.0f;   ///< A-PH's Sub and Ceiling boxes
	static constexpr float kVocodeColumnWidth = 96.0f; ///< OE: Resolution's box ("Balanced" fits), Freeze and Key side by side under it
	/// A compact ValueBox in a panel's head (Filter Mode, in a dense window).
	static constexpr float kModeBoxWidth = 96.0f;
	/// The plot overlay's inset from the plot's left edge: clear of the dB axis labels.
	static constexpr float kOverlayInset = 40.0f;
	/// Every operator row (one, or more once the panels wrap) and the gap between them.
	[[nodiscard]] float rowsHeight() const;

	SpectrumPanel(ParamBridge& bridge, WidgetRegistry& registry, const SoundSelection& selection, Notifier* notifier);
	~SpectrumPanel() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override { return { kMinimumWidth, kMinimumHeight }; }
	/// "Spectrum": the panel's identity (its remembered place). The header says whose settings it edits (headerTitle).
	std::string title() const override { return "Spectrum"; }
	/// "Spectrum — Sound A": the selected sound's, followed every tick.
	std::string headerTitle() const override;
	/// THE TICK: follow the selection and the patch (sync), and keep the engine's plot wanted while shown.
	bool updateContent() override;
	/// THE MASTER SWITCH IS THE HEADER'S ENABLE CHIP — a device window's own enable, here the spectral one. Its flip is one
	/// step of the editor's undo (StoredSwitch), as the tab's chip is.
	[[nodiscard]] size_t deviceEnableControl() const override { return 0; }
	[[nodiscard]] bool deviceEnabled() const override;
	std::optional<bool> toggleDeviceEnable() override;
	/// The tab's master switch is not a device control, so it is no modulation destination (backlog KW).
	EnableMarks deviceEnableMarks() const override { return {}; }
	bool routeDeviceEnable() override { return false; }
	/// Hand every knob of every sound the editor's modulation state (the continuous controls are destinations), and hand
	/// the stored-state edits — the master and curve switches, Flatten, a stroke on the plot — the editor's undo history.
	void modulationBound(ModState* mod, EditHistory* history) override;
	/// Show `selected`'s set and bring its greyed state up to the patch. `live` is whether the drawer acts at all for that
	/// sound — Additive, and the sound in the constellation (a parked sound's controls are dead, as its flank knobs are);
	/// off it the shown set is disabled. The master switch greys everything; an operator that is off greys its own
	/// controls; and the controls an operator setting makes inert grey with it — Resonance under Notch, the high shift and
	/// the crossover while Linked, the Vowel at Depth 0 (the honesty sweep in test_additive_engine pins the first three, the
	/// inert-vowel morph case the last). Hidden sets keep their
	/// enablement, as the flank knobs' do. A CHANGE OF SOUND first closes a gesture open on the plot, which was the other
	/// sound's. Answers whether anything was repainted.
	bool sync(size_t selected, bool live);
	[[nodiscard]] size_t selected() const noexcept { return selected_; }
	/// CLOSE A GESTURE OPEN ON THE PLOT (its stroke recorded, a handle's host gestures ended) — the editor calls it on every
	/// road that closes the panel, so the host is never left holding a gesture with no editor to end it.
	void endGesture() { plot_->endGesture(); }

	/// Where the plot sits, in the panel's coordinates.
	[[nodiscard]] visage::Bounds plotBounds() const;

	// ---- test seams: every control by its sound and the control it binds ----------------------------------------
	[[nodiscard]] Knob* knobForTest(size_t slot, SpectralControl control) const;
	[[nodiscard]] SwitchChip* switchForTest(size_t slot, SpectralControl control) const;
	/// The pill (slot, control) is drawn by in a dense window: an amount in a stack, or Tone's pill for its switch too.
	[[nodiscard]] FaderPill* pillForTest(size_t slot, SpectralControl control) const;
	[[nodiscard]] SegmentedControl& modeForTest(size_t slot) const { return *sets_[slotOf(slot)].mode; }
	/// F2: filter `filter`'s Mode (the drawer's segments; null in a dense window, whose Mode is a box: modeBoxForTest), its tabs
	/// and its routing chip (each filter panel carries its own, over the one routing control).
	[[nodiscard]] SegmentedControl* filterModeForTest(size_t slot, int filter) const {
		return filter == 0 ? sets_[slotOf(slot)].mode.get() : sets_[slotOf(slot)].mode2.get();
	}
	[[nodiscard]] SegmentedControl& filterTabsForTest(size_t slot, int filter) const { return *sets_[slotOf(slot)].tabs[filter == 1 ? 1 : 0]; }
	[[nodiscard]] SwitchChip& routingForTest(size_t slot, int filter) const { return *sets_[slotOf(slot)].routing[filter == 1 ? 1 : 0]; }
	/// The filter tab the row shows (0 or 1): the source's (SpectrumSource::selectedFilter), followed at each sync.
	[[nodiscard]] int filterTabForTest() const { return filterTab_; }
	/// OH: the Partials mode cell of `slot`'s set (null without the scheme).
	[[nodiscard]] ModeValueCell* partialsCellForTest(size_t slot) const { return sets_[slotOf(slot)].partialsCell; }
	/// TEST ONLY: sound `slot`'s Vocode analysis cells (OE), null without them.
	[[nodiscard]] VocodeAnalysisColumn* vocodeColumnForTest(size_t slot) const { return sets_[slotOf(slot)].vocodeColumn; }
	[[nodiscard]] PillStack* vocodeTimesForTest(size_t slot) const { return sets_[slotOf(slot)].vocodeTimes; }
	[[nodiscard]] Knob* vocodeSibilanceForTest(size_t slot) const { return sets_[slotOf(slot)].vocodeSibilance; }
	[[nodiscard]] ModeValueCell* vocodeSpacingForTest(size_t slot) const { return sets_[slotOf(slot)].vocodeSpacing; }
	[[nodiscard]] ModeValueCell* vocodeWidthsForTest(size_t slot) const { return sets_[slotOf(slot)].vocodeWidths; }
	[[nodiscard]] SwitchChip& curveSwitchForTest(size_t slot) const { return *sets_[slotOf(slot)].curveChip; }
	[[nodiscard]] bool flattenEnabledForTest() const { return flattenEnabled_; }
	/// THE ROW'S LAYOUT: how many rows the panels take, the one-row width at a knob column, whether one row fits at the
	/// floor column, and whether the rows fit at all (every row at the floor inside the panel).
	[[nodiscard]] size_t rowsForTest() const { return rows_; }
	[[nodiscard]] float oneRowWidthForTest(float column) const { return rowWidth(column, 0, shownPanels().size()); }
	/// Each shown panel's width at knob column `column`, in row order (the MEASUREMENT case prints them).
	[[nodiscard]] std::vector<float> panelWidthsForTest(float column) const { return panelWidths(column); }
	[[nodiscard]] bool oneRowFitsForTest() const { return oneRowFits_; }
	[[nodiscard]] bool fitsForTest() const { return fits_; }
	/// THE KNOB COLUMN'S FLOOR: the widest caption as drawn (Knob::captionWidth) and its margins each side, so no caption
	/// is ever cut (decided 2026-10-11); and the natural column, kKnobColumn unless a caption needs more.
	[[nodiscard]] float minColumn() const { return minColumn_; }
	[[nodiscard]] float naturalColumn() const { return naturalColumn_; }
	/// TEST ONLY: each section in the row (the shown ones), its bounds, its own chip (null for a chipless one) and every frame that belongs to it.
	struct SectionForTest {
		std::string name;
		visage::Bounds bounds {};
		visage::Frame* chip = nullptr;
		std::vector<visage::Frame*> frames;
	};
	[[nodiscard]] std::vector<SectionForTest> sectionsForTest(size_t slot) const;
	[[nodiscard]] visage::Bounds flattenBoundsForTest() const { return flattenBounds_; }
	[[nodiscard]] SpectrumPlot& plotForTest() const { return *plot_; }
	[[nodiscard]] SwitchChip& drawToolForTest() const { return *drawChip_; }
	/// TEST ONLY: A-PH's Phases, Sub and Ceiling boxes (null in a device's window).
	[[nodiscard]] ValueBox* phasesBoxForTest() const { return phasesBox_; }
	[[nodiscard]] ValueBox* subBoxForTest() const { return subBox_; }
	[[nodiscard]] ValueBox* ceilingBoxForTest() const { return ceilingBox_; }
	[[nodiscard]] bool drawToolOnForTest() const { return drawTool_; }
	/// Flatten, as a press on it does: the SELECTED sound's curve.
	void flattenForTest() { flatten(); }
	/// Whether the control bound to (slot, control) is drawn live — a knob's `active()`, a chip's `enabled()`, the mode's.
	[[nodiscard]] bool liveForTest(size_t slot, SpectralControl control) const;
	/// The control (slot, control) is drawn by, whichever kind it is.
	[[nodiscard]] visage::Frame* frameForTest(size_t slot, SpectralControl control) const;
	/// Every frame one sound's set holds, for the tree walks.
	[[nodiscard]] std::vector<visage::Frame*> controlsForTest(size_t slot) const;
	/// A DENSE WINDOW'S PLOT OVERLAY (the Curve switch, Flatten, Draw): ON the plot by design, so a geometry sweep keeps them
	/// inside it rather than clear of it. Empty in the drawer.
	[[nodiscard]] std::vector<visage::Frame*> overlayForTest(size_t slot) const;
	/// The Filter Mode's menu box in a dense window (null in the drawer, whose Mode is segmented).
	[[nodiscard]] ValueBox* modeBoxForTest(size_t slot, int filter = 0) const {
		return filter == 0 ? sets_[slotOf(slot)].modeBox.get() : sets_[slotOf(slot)].modeBox2.get();
	}

protected:
	/// **FOR A WINDOW OVER ANOTHER SOURCE** (the Spectral device's, spectral_expanded.h): the same plot, the same operator panels
	/// for the controls `source` has, the same curve panel, one set per `source.slots()`. No bridge and no selection: such a
	/// window overrides the tick, the title and the enable. `source` outlives the panel.
	/// **DENSE** (the Spectral window gives the plot back a row): each operator's amounts are a PillStack of
	/// amount-only pills, Filter Mode a ValueBox with its menu, Tilt and Odd/Even one "Tone" cell of two FaderPills (each its
	/// own power), and the Curve's switch, Flatten and Draw an overlay on the plot's corner instead of a panel. The drawer is
	/// never dense.
	SpectrumPanel(SpectrumSource& source, WidgetRegistry& registry, Notifier* notifier, bool dense = false);
	/// A HEAD ITEM OF A LEADING PANEL: a frame and the width it takes in the head.
	struct LeadItem {
		std::unique_ptr<visage::Frame> frame;
		float width = 0.0f;
		float height = 0.0f; ///< 0: the line's own (a head's kHeadHeight, the strip's kStripHeight); otherwise centred in it
	};
	/// **A PANEL OF THE WINDOW'S OWN, FIRST IN THE ROW** (the device's Key box), laid out and drawn as the operators' are; its
	/// widgets' greying is the window's. Call before the first layout.
	void addLeadingPanel(const char* name, std::vector<LeadItem> head, std::vector<std::unique_ptr<Knob>> knobs,
	                     std::vector<std::unique_ptr<PillStack>> stacks = {}, std::vector<LeadItem> cells = {});
	/// **THE HEADER STRIP** (a dense window): its items in one line above the plot, `kStripHeight` tall. Call before the first
	/// layout.
	void addHeaderStrip(std::vector<LeadItem> items);
	/// HIDE OR SHOW the `lead`th leading panel (0 the first added): out of the row and out of sight, the rest closing over it.
	void setLeadingPanelHidden(size_t lead, bool hidden);
	[[nodiscard]] SpectrumSource& source() const { return source_; }
	[[nodiscard]] float columnForTest() const { return naturalColumn_; }

private:
	SpectrumPanel(std::unique_ptr<SpectrumSource> owned, ParamBridge* bridge, const SoundSelection* selection,
	              WidgetRegistry& registry, Notifier* notifier);
	/// The body both constructors share: every set's panels, the plot, the Draw tool, Flatten.
	void build();
	/// The knob column's floor and natural width, from the first set's captions.
	void measureColumn();
	[[nodiscard]] size_t slotOf(size_t slot) const { return std::min(slot, sets_.size() - 1); }
	/// The first set's panels the row lays out, in order: every one but a hidden leading panel.
	[[nodiscard]] std::vector<size_t> shownPanels() const;
	/// One operator: its name, its switch, and its controls in the row. THE CURVE'S panel has no SpectralControl switch (its
	/// switch is the source's curveSwitch); a LEADING panel is the window's own.
	struct Panel {
		/// Cells (OE2): a chipless panel of cells its owner greys — the Vocode section's engine-wide analysis, beside it.
		enum class Kind : uint8_t { Operator, Curve, Lead, Tone, Cells };
		Kind kind = Kind::Operator;
		const char* name = "";
		SpectralControl sw = SpectralControl::kCount; ///< kCount: the curve or a leading panel
		std::unique_ptr<device::Modulatable> swControl;
		std::vector<LeadItem> lead; ///< a leading panel's head
		bool hidden = false; ///< a leading panel the window has hidden (setLeadingPanelHidden): out of the row entirely
		std::unique_ptr<SwitchChip> chip;
		std::vector<std::pair<SpectralControl, std::unique_ptr<Knob>>> knobs;
		/// DENSE: the amounts as PillStacks (after the knobs, a cell each), the controls each pill binds, and the controls they
		/// own. A leading panel's stacks bind no SpectralControl (kCount): the window greys them.
		std::vector<std::pair<std::vector<SpectralControl>, std::unique_ptr<PillStack>>> stacks;
		std::vector<std::unique_ptr<device::Modulatable>> owned;
		/// A LEADING PANEL'S OWN CELLS after its knobs, each its LeadItem width and the cell row's height (a stack's).
		std::vector<LeadItem> cells;
		float headExtra = 0.0f; ///< the head's room after the chip (Mode, Link, Flatten)
		/// F2: A FILTER'S PANEL (0 or 1), -1 for any other. Only the selected filter's is in the row (the other is `hidden`), and its
		/// head is the tabs, its switch (captioned "On"), its Mode and the routing chip.
		int filter = -1;
		visage::Bounds bounds {};
	};
	/// One sound's controls.
	struct SoundSet {
		std::vector<Panel> panels;
		std::unique_ptr<SegmentedControl> mode;
		std::unique_ptr<device::Modulatable> modeControl; ///< dense: Filter Mode's, for its ValueBox
		std::unique_ptr<ValueBox> modeBox;
		std::unique_ptr<device::Modulatable> linkControl;
		std::unique_ptr<SwitchChip> linkChip;
		std::unique_ptr<device::Modulatable> curveSwitch;
		std::unique_ptr<SwitchChip> curveChip;
		/// F2: filter 2's Mode (as filter 1's above), each filter panel's tabs, and the routing control with a chip in each filter
		/// panel's head.
		std::unique_ptr<SegmentedControl> mode2;
		std::unique_ptr<device::Modulatable> mode2Control;
		std::unique_ptr<ValueBox> modeBox2;
		std::array<std::unique_ptr<SegmentedControl>, 2> tabs;
		std::unique_ptr<device::Modulatable> routingControl;
		std::array<std::unique_ptr<SwitchChip>, 2> routing;
		/// OH: the Partials mode cell (the scheme and the count), a cell of the Partials panel; null without the scheme.
		ModeValueCell* partialsCell = nullptr;
		/// OE2: the Vocode section's Spacing and Widths mode cells (null without them).
		ModeValueCell* vocodeSpacing = nullptr;
		ModeValueCell* vocodeWidths = nullptr;
		/// OE: the engine vocoder's analysis in the Vocode section — Resolution, Freeze and Key Track; Bands, Attack and Release — null
		/// without it (a device's window). Engine-wide controls, live while any sound vocodes.
		VocodeAnalysisColumn* vocodeColumn = nullptr;
		PillStack* vocodeTimes = nullptr;
		Knob* vocodeSibilance = nullptr; ///< OE2: the engine's Sibilance, a knob cell of the Vocode panel
	};
	/// F2: SHOW THE FILTER TAB `filter` — its panel in the row, the other's out of it (`hidden`, in every set) — and re-lay the row.
	void showFilterTab(int filter);
	/// OE: the engine-wide analysis cells into `p`, the Vocode section of `set` (the drawer's road only: they are the bridge's params).
	void addVocodeAnalysis(size_t slot, SoundSet& set);
	/// Whether a filter panel's head frame shows: its set shown and its panel in the row.
	void showFilterHead(size_t slot, bool setShown);
	void layoutRow();
	/// Each panel's width at knob column `column`, in row order (every set's are the same).
	[[nodiscard]] std::vector<float> panelWidths(float column) const;
	/// Panels [from, to) side by side at `column`, with their gaps.
	[[nodiscard]] float rowWidth(float column, size_t from, size_t to) const;
	/// The same over widths already worked out (panelWidths), for the layout's cut search.
	[[nodiscard]] static float rowWidth(const std::vector<float>& widths, size_t from, size_t to);
	void flatten();
	[[nodiscard]] bool on(size_t slot, SpectralControl control) const;
	void showSet(size_t slot, bool shown);

	/// THE SOURCE: owned over the bridge (the drawer's constructor), borrowed otherwise. The bridge and the selection are the
	/// drawer's, null for another window.
	std::unique_ptr<SpectrumSource> ownedSource_;
	SpectrumSource& source_;
	ParamBridge* bridge_ = nullptr;
	const SoundSelection* selection_ = nullptr;
	WidgetRegistry& registry_; ///< the filter tabs' SegmentedControls take it
	Notifier* notifier_ = nullptr;
	std::vector<SoundSet> sets_;
	size_t selected_ = 0;
	/// F2: the filter tab the row shows, and the control every set's tabs draw (view state through the source; never saved).
	int filterTab_ = 0;
	std::unique_ptr<device::Modulatable> filterTabControl_;
	visage::Bounds flattenBounds_ {};
	bool flattenEnabled_ = true;
	bool masterOn_ = true;
	/// THE ROW'S LAYOUT (layoutRow): rows taken, whether one row fit at the floor column, whether the rows fit. `wrap_` is the
	/// wrap itself, a constant true (a known-bad turns it off).
	size_t rows_ = 1;
	bool oneRowFits_ = true;
	bool fits_ = true;
	/// The knob column's floor and natural width (minColumn, naturalColumn), measured from the captions at construction.
	float minColumn_ = kKnobColumn;
	float naturalColumn_ = kKnobColumn;
	static constexpr bool wrap_ = true;
	EditHistory* history_ = nullptr;
	/// THE MASTER SWITCH, for the header's enable chip (toggleDeviceEnable): the tab's own, over the same stored state.
	StoredSwitch master_;
	/// THE PLOT (D2): the engine's frame, the selected sound's handles and curve.
	std::unique_ptr<SpectrumPlot> plot_;
	/// THE DRAW TOOL: per-viewer, never saved, never undone — whether a press on the plot draws the curve or grabs a
	/// handle. Live while the sound and the master switch are; a stroke on an off curve switches it on.
	bool drawTool_ = false;
	StoredSwitch drawSwitch_;
	std::unique_ptr<SwitchChip> drawChip_;
	/// DENSE: Flatten as a button over the plot (the drawer draws it in the curve panel's head), the header strip, and whether
	/// this panel is dense at all.
	std::unique_ptr<visage::Frame> flattenButton_;
	std::vector<LeadItem> strip_;
	/// A-PH (additive design §8): THE DRAWER'S ENGINE-WIDE PHASES, in the strip above the plot — Phases (Natural / Headroom /
	/// Sub priority, a ValueBox with its menu), and Sub and Ceiling, live only under Sub priority (the honesty rule). Their
	/// controls, owned here because a ValueBox holds a reference. Null in a device's window (the strip there is the device's).
	std::unique_ptr<device::Modulatable> phasesControl_, subControl_, ceilingControl_;
	ValueBox* phasesBox_ = nullptr;
	ValueBox* subBox_ = nullptr;
	ValueBox* ceilingBox_ = nullptr;
	bool dense_ = false;
	/// The drawer's FILTER sections keep full knobs where a dense window's are pill stacks (backlog OG, by preference).
	bool filterKnobs_ = false;
	/// OE2: whether the Vocode section's engine analysis panel is in the row (only while some sound vocodes: its controls act then).
	bool vocodeAnalysisShown_ = false;
	/// **THE SUBSCRIPTIONS, HELD** (`ui::Watch`): the chips over params register here, as a Knob registers itself.
	/// Declared last so they are released first.
	std::vector<Watch> watches_;
};

} // namespace feathers::ui
