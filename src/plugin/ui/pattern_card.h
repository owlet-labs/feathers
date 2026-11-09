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

#include "backplane/device_host.h"
#include "mod/arp/pattern.h"
#include "mod/arp/pattern_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/paged_controls.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"

#include <visage/ui.h>

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

// THE PATTERN CARD — composed around the focused pattern's lane, not a list of
// knobs:
//
//     [ 1  E S ][ 2  E S ][ 3  E S ][ 4  E S ]   — the tabs: a press on the number FOCUSES that pattern (edit only);
//     [ Keys ▾ ] [ Humanize ▾ ]       [ Latch ]     E and S are its Enable and Solo, two-state chips (design §7.3)
//     ┌───────────────────────────────────────┐
//     │ ▬  ▬▬▬   ▬ ▬▬     ▬▬▬▬  ▬   ▬    ▬▬   │   — the FOCUSED pattern's cycle as the device would play it over a
//     └───────────────────────────────────────┘     held C: a bar a note, stacked for a chord, joined for a tie
//     [ Mode ▾ ]   [ Scale ▾ ]                      — the focused pattern's
//       Pattern    Rate     Length    Amount        — the selector ("1 · C Maj") and Humanize's amount are global
//
// THE FOCUS IS THIS CARD'S UI STATE, and only the focused pattern's own widgets are shown. The window (`GenericExpanded`
// until its own) holds every control of every pattern, so the window ⊇ the card at every focus.

namespace feathers::ui {

class Notifier;
class WidgetRegistry;

/// **TURNING ROTATE KEEPS THE PATTERN UNDER THE PUCK** (Feathers' rule; design §3): watches each pattern's Rotate on the
/// message thread and, when one moved, banks what the turn would have moved (`pat::foldRotate`) into the hand's block,
/// one write. ONE WATCH PER DEVICE, shared by the card and the window, so a turn is folded once whoever sees it first.
/// True when it folded.
bool foldRotateIfTurned(backplane::DeviceHost& host, DeviceInstanceId id);

/// **THE HAND'S EDITS ARE SCOPED TO THE STANCE** (and the pattern returns to what the terrain defines if the puck
/// moves): any pattern whose edits were made at another base place, or under another Mode, than its
/// authored controls hold now has them cleared — one block write, no undo step of its own (the move that cleared them is
/// the step). Called by the card and the window every tick, as the Rotate fold is. True when it cleared any.
bool clearStaleEdits(backplane::DeviceHost& host, DeviceInstanceId id);

class EditHistory;

/// **THE PATTERN TABS' CONTEXT MENU** (as on the Drawn filter's waypoint tabs: copy to 1, 2, 3, 4 and every
/// context option) — Drawn's frame-tab menu, as it applies: "Reset to default", then
/// the shared copy half (`slot_copy_menu.h`). A copy moves the pattern's WHOLE per-pattern set — every control but Enable
/// and Solo (the selector's range is the player's, not the pattern's; Drawn's frames have no such switches), the hand's
/// slice (the banked Rotate residue, the pin switches, the zoom) and the hand-edit overlay. NOT the likes (Drawn has no
/// analogue; likes are per slot — provisional). Each verb is ONE undo step.
inline constexpr int kPatternMenuReset = -2;
inline constexpr int kPatternMenuCopyAll = -3;
/// **EXPORT MIDI…** (design §7.9): offered when the surface can write a file (the window).
inline constexpr int kPatternMenuExportMidi = -4;
[[nodiscard]] std::vector<std::pair<int, std::string>> patternTabMenuOptions(size_t self);
/// Does what option `choice` of tab `from`'s menu says; records it in `history` (null: no record).
void choosePatternTabMenu(backplane::DeviceHost& host, DeviceInstanceId id, size_t from, int choice, EditHistory* history);
/// **SOLO IS A RADIO** (soloing a tab un-solos the previously soloed one; design §7.3): a
/// press of tab `tab`'s S solos it alone; pressed on the only solo, it clears it; `additive` (Ctrl/Cmd) adds this tab to
/// the soloed set or takes it out, so "several soloed" is still reachable. Every Solo field it moves, as ONE undo step.
/// True when the tab is soloed after the press (its surface then focuses it: soloing a pattern focuses its tab).
bool pressPatternSolo(backplane::DeviceHost& host, DeviceInstanceId id, size_t tab, bool additive, EditHistory* history);
/// Opens tab `from`'s menu at `at` on `source`.
void showPatternTabMenu(backplane::DeviceHost& host, DeviceInstanceId id, size_t from, EditHistory* history,
                        visage::Frame& source, visage::Point at, std::function<void()> exportMidi = {});
/// THE ROTATE WATCH TAKES THE ROW AS IT STANDS — after a verb that wrote Rotate together with the residue it belongs with,
/// so the fold does not bank that move a second time.
void resyncRotateWatch(backplane::DeviceHost& host, DeviceInstanceId id);

/// **THE FOCUSED PATTERN'S CYCLE, DRAWN** — decoded on the message thread by the device's own road (`pattern::build`
/// over `PatternDevice::placeOf` and `recipeFor`) against a held C4, so it shows the pattern's shape before a key is
/// pressed. Rebuilt only when the row moved.
class PatternLane final : public visage::Frame, public TooltipSource {
public:
	PatternLane(backplane::DeviceHost& host, DeviceInstanceId id);

	/// **THE GESTURES, ON HOVER** (a gesture tooltip on the pattern lane, because how to modify notes was not
	/// discoverable). Only while the lane is EDITABLE (the window's; the card's is a view and says nothing). Each line is
	/// a gesture the press handlers below implement — the tooltips suite's case drives every one of them.
	/// ONE SENTENCE OF GESTURES, then one of when edits clear: a Brief tooltip (`briefTooltip`, the first sentence) still
	/// lists every gesture.
	/// (No modifier-click note-expression editing: the active step's settings show near the sequence view;
	/// a single click selects, a double click adds or deletes.)
	static constexpr const char* kPitchedGestures =
	    "Edit by hand: click a step to select it and set it in the step inspector beside the lane; double-click to add a "
	    "note at the pitch under the pointer, or to delete the note there; drag a note up or down for its pitch, snapped to "
	    "the scale, or its right end for its length. Edits clear when the place (the puck, Zone, Zone Offset, Gamma) or the "
	    "Mode changes; Rotate keeps them.";
	static constexpr const char* kDrumGestures =
	    "Edit by hand: click a cell to select it and set it in the step inspector beside the grid; double-click to turn "
	    "that lane's hit on that step on or off. Edits clear when the place (the puck, Zone, Zone Offset, Gamma) or the "
	    "Mode changes; Rotate keeps them.";
	const char* tooltipAt(visage::Point local) const override;
	/// THE PHASE MARKERS (design §7.8), on the window's lane: its Start, and a fill's End — device controls, so offered here.
	/// The hand's step edits write the hand's block, never a control.
	void offeredControls(std::vector<Offered>& out) const override;
	/// **THE MARKERS** (a start-phase cursor dragged across the sequence, and an end
	/// phase for fills): the step each stands at (the End only while the focused tab is a fill), and where it is drawn.
	[[nodiscard]] int startMarkForTest() const { return startMark_; }
	[[nodiscard]] int endMarkForTest() const { return endMark_; }
	[[nodiscard]] bool fillForTest() const { return fill_; }
	[[nodiscard]] float markX(int step) const;
	[[nodiscard]] int markAtForTest(float x) const { return markAt(x); }
	/// A marker's HANDLE is the lane's top band (a press there drags it; below, the steps take the press), and how near x
	/// must be.
	static constexpr float kMarkHandle = 11.0f;
	static constexpr float kMarkReach = 5.0f;
	/// **THE MARKERS ARE START AND END'S FACES FOR MODULATION** (they glow when assignable and show a route
	/// ring once routed). Armed and routable, a press on a handle routes: VERTICAL travel sets that marker's amount (its own
	/// drag is horizontal); a double-click unroutes; Alt drags the marker as ever. End answers only while it is drawn (a fill).
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override;
	/// The window's modulation state, for the markers' controls (`PatternExpanded::modulationBound`).
	void setModState(ModState* mod);
	[[nodiscard]] DeviceControl* startControlForTest() const { return startCtl_.get(); }
	[[nodiscard]] DeviceControl* endControlForTest() const { return endCtl_.get(); }
	static constexpr float kRouteDragRange = 70.0f;
	static constexpr float kRouteSeedAmount = 0.4f;
	void setFocus(size_t pattern);
	/// Reads the row; true (and a repaint) when the focused pattern's inputs moved.
	bool poll();
	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] const mod::arp::pattern::PatternCycle& cycleForTest() const { return cycle_; }
	/// Whether the focused pattern is in Drum, so the lane is drawn as the drum grid.
	[[nodiscard]] bool drumForTest() const { return drum_; }
	/// The focused pattern is in Modulation (the Drum decode's grid, its rows named Mod 1–8, not the kit's).
	[[nodiscard]] bool modulationForTest() const { return modulation_; }

	/// **EDITABLE BY HAND** (the window's lane, design §15-bis; the card's stays a view). A click toggles a step (or, in
	/// Drum, a lane's hit); Shift-click its accent; Alt-click its slide; Ctrl/Cmd-click a tie; a vertical drag a note's pitch,
	/// snapped to the scale (Shift-drag its velocity); a drag from a bar's right end its gate. One undo step a gesture.
	void setEditable(bool editable, EditHistory* history);
	/// The geometry the drawing and the hit test share: the step under x, the lane (Drum) or key (pitched) under y.
	[[nodiscard]] size_t stepAt(float x) const;
	[[nodiscard]] size_t laneAt(float y) const;
	[[nodiscard]] int keyAt(float y) const;
	[[nodiscard]] float xOfStep(size_t k) const;
	[[nodiscard]] float yOfKey(int key) const;
	[[nodiscard]] float yOfLane(size_t lane) const;
	/// THE STEP PLAYING NOW on the focused pattern (the device's playhead), or -1: none, or another pattern playing.
	[[nodiscard]] int playStepForTest() const { return playStep_; }
	/// **THE TONIC'S ROWS** (highlighted): every key the view shows at the
	/// cycle's own `rootKey` in any octave, low to high — the rows drawn highlighted. None in Drum, where rows are lanes.
	[[nodiscard]] std::vector<int> tonicKeys() const;
	/// The cycle the terrain decodes here, before the hand's edits.
	[[nodiscard]] const mod::arp::pattern::PatternCycle& terrainForTest() const { return terrain_; }

	//-- THE ACTIVE STEP (the window's) -------------------------------------------------------------------------//
	/// The step selected (-1: none) and, in Drum, the lane. UI STATE, as the tab focus is: never saved with the patch, and
	/// dropped when the focus moves or the cycle no longer has the step.
	[[nodiscard]] int selectedStep() const { return selected_; }
	[[nodiscard]] size_t selectedLane() const { return selectedLane_; }
	void select(int step, size_t lane = 0);
	/// Told whenever the selection or the selected step's content may have moved (the inspector re-reads).
	std::function<void()> onSelect;
	/// **THE NOTE WHOSE BAR IS UNDER `at`** — its own step, though the bar runs on through later steps (a long gate, a tie):
	/// a press on a note's BODY is a press on that note, never on the step under it (selecting a note body once
	/// overlaid a new note at the same pitch). -1 when no bar is there. Pitched only.
	[[nodiscard]] int noteAt(visage::Point at) const;
	/// THE STEP SHOWN NOW (edits applied), or null with nothing selected.
	[[nodiscard]] const mod::arp::pattern::PatternStep* selectedStepNow() const;
	/// The key the selected step sounds at (the lane's C4 decode), and the key `raw` snaps to in the focused scale,
	/// stepping a whole scale degree in `direction` (±1) when the snap would land back on `from`.
	[[nodiscard]] int selectedKey() const;
	[[nodiscard]] int snapKey(int raw, int from) const;
	/// **THE INSPECTOR'S ROAD INTO THE EDITS**: `fn` edits the selected step's `StepEdit` (started from the terrain's step
	/// if it has none) and the block is written; between `beginStepEdit` and `endStepEdit` it is ONE undo step.
	void beginStepEdit();
	void editSelected(const std::function<void(mod::arp::pattern::StepEdit&)>& fn);
	/// **THE SELECTED CELL'S LEVEL** (0..1), as this lane decodes it: a hit's velocity in Drum; in Modulation the lane's level
	/// there (held from its last hit). And whether the focused tab's lanes are Bipolar.
	[[nodiscard]] double selectedCellLevel() const;
	[[nodiscard]] bool modBipolar() const;
	void endStepEdit();
	/// The key a step made a note sounds at: the terrain's, or the cycle's root.
	[[nodiscard]] int defaultKeyAt(size_t step) const;

private:
	void drawDrumGrid(visage::Canvas& canvas);
	/// The edits block now, and a write of it (recorded as one undo step against `before` when `record`).
	[[nodiscard]] mod::arp::pat::EditState editsNow() const;
	void writeEdits(const mod::arp::pat::EditState& state);
	void recordEdits(const std::vector<float>& before);
	/// THE EDIT OF STEP k, started from the terrain's step if the step has none (anchored at the place now).
	mod::arp::pattern::StepEdit& editAt(mod::arp::pat::EditState& state, size_t k) const;
	bool drum_ = false;
	bool modulation_ = false;
	/// The focused pattern is in Progression: its chords drawn as blocks, each named by its numeral.
	bool progression_ = false;
	int playStep_ = -1;
	/// Reads the device's playhead; true (and a repaint) when the step drawn moved. Only while playing does it change.
	bool pollPlayhead();
	uint8_t drumOn_ = 0xFF; ///< which lanes play (a bit a lane): a lane switched off is drawn dimmed
	/// WHERE THE STEPS START: past the row labels' gutter in Drum, at the edge otherwise.
	[[nodiscard]] float leftPad() const { return drum_ ? 3.0f + kDrumGutter : 3.0f; }
	static constexpr float kDrumGutter = 18.0f;
	bool editable_ = false;
	EditHistory* history_ = nullptr;
	int selected_ = -1;
	/// THE MARKERS: the focused tab's Start and End, whether it is a fill, the controls they write, and a drag in flight
	/// (0 none, 1 Start, 2 End).
	int startMark_ = 0;
	int endMark_ = 16;
	bool fill_ = false;
	std::unique_ptr<DeviceControl> startCtl_;
	std::unique_ptr<DeviceControl> endCtl_;
	int markDrag_ = 0;
	// A MARKER'S ROUTE GESTURE (`modLookFor`): which marker, where the press began, and the amount it started from.
	int routeMark_ = 0;
	float routeFromY_ = 0.0f;
	float routeAnchor_ = 0.0f;
	ModState* mod_ = nullptr;
	Watch watch_;
	const ModState* watched_ = nullptr;
	void refreshWatch();
	/// The marker within reach of x (0 none, 1 Start, 2 End).
	[[nodiscard]] int markAt(float x) const;
	void bindMarks();
	size_t selectedLane_ = 0;
	std::vector<float> stepBefore_;
	int stepGestures_ = 0;
	mod::arp::pattern::PatternCycle terrain_ {};
	int lo_ = 0;
	float span_ = 12.0f;
	// THE GESTURE IN FLIGHT.
	struct Press {
		bool active = false;
		bool dragged = false;
		bool gate = false;
		bool shift = false;
		size_t step = 0;
		visage::Point at {};
		std::vector<float> before;
	} press_ {};
	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	size_t focus_ = 0;
	std::vector<float> lastRow_;
	size_t lastFocus_ = mod::arp::pat::kPatterns;
	mod::arp::pattern::PatternCycle cycle_ {};
};

/// **THE FOUR TABS** (drawn to read as tabs) — one frame drawing a tab strip: the focused
/// tab raised and open at its foot, joining the panel its owner draws below it (`drawPanel`); the others recessed behind a
/// rule. A press anywhere on a tab focuses it; the E and S chips sit on it as children of the owner and take their own.
class PatternTabs final : public visage::Frame, public TooltipSource {
public:
	PatternTabs();
	const char* tooltipAt(visage::Point) const override {
		return "A pattern's tab: press its number to edit it here. E puts it in range; S plays it alone (and edits it) — Ctrl/Cmd-S "
		       "adds it to the soloed ones, S again on the only solo clears it";
	}
	/// THE FOCUS IS THE CARD'S UI STATE, NOT A DEVICE CONTROL: the numbers offer none (the E and S chips offer theirs).
	void offeredControls(std::vector<Offered>&) const override {}
	std::function<void(size_t)> onFocus;
	/// A popup trigger on tab `i` at `at` (the tab menu).
	std::function<void(size_t, visage::Point)> onMenu;
	void setFocus(size_t focus) {
		focus_ = focus;
		redraw();
	}
	void setPlaying(size_t playing) {
		if (playing != playing_) {
			playing_ = playing;
			redraw();
		}
	}
	void draw(visage::Canvas& canvas) override;
	/// The number's square in tab `i`, in the strip's first row.
	[[nodiscard]] visage::Bounds numberBounds(size_t i) const;
	static constexpr float kNumberRow = 24.0f;
	/// The whole of tab `i` — what a press focuses.
	[[nodiscard]] visage::Bounds tabBounds(size_t i) const;
	[[nodiscard]] size_t focusForTest() const { return focus_; }
	/// THE PANEL'S FILL, which the focused tab shares so the two read as one surface.
	static uint32_t tabFill();
	/// Draws the panel a tab strip opens onto: filled as the focused tab, ruled at its sides and foot (its head is the
	/// strip's rule, open under the focused tab).
	static void drawPanel(visage::Canvas& canvas, float x, float y, float w, float h);

private:
	size_t focus_ = 0;
	size_t playing_ = mod::arp::pat::kPatterns;
};

class PatternCard final : public FxCard, public PagedControls {
public:
	PatternCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr,
	            ModState* mod = nullptr, EditHistory* history = nullptr);
	PatternCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr, ModState* = nullptr,
	            EditHistory* = nullptr) = delete;
	~PatternCard() override;

	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::arp::pat::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kRowGap = 2.0f;
	static constexpr float kTabsHeight = 24.0f;
	static constexpr float kLaneHeight = 64.0f;
	/// THE LANE IN A HORIZONTAL CHAIN, where the card has one unit's height (`compactBodyHeight`).
	static constexpr float kLaneHeightCompact = 32.0f;
	/// THE BODY IN A HORIZONTAL CHAIN: the selector's row (Keys over Latch beside it; Humanize is the window's), the tabs,
	/// and the panel with the shorter lane, ONE knob row (Rate, Length, and Mode as a short box), and Scale on a short row of its
	/// own under it (the longest scale names are ~115 px, which only the panel's width holds).
	static constexpr float compactBodyHeight() {
		return kGap + controlsHeight() + kGap + kTabsHeight + kGap + kLaneHeightCompact + kGap + controlsHeight() + kGap
		       + ValueBox::kCompactHeight + kGap
		       + kGap;
	}
	static constexpr float kFaceDiameter = Knob::kEngineRailDiameter;
	static constexpr float controlsHeight() { return Knob::columnHeight(kFaceDiameter); }
	/// THE BODY, top to bottom: the device-wide controls (Keys, Humanize, Latch; the Pattern selector and Humanize's
	/// Amount), then the tab strip and the focused pattern's panel (its lane, Mode and Scale, Rate and Length).
	static constexpr float bodyHeight() {
		return kGap + ValueBox::kHeight + kGap + controlsHeight() + kGap + kTabsHeight + kGap + kLaneHeight + kGap
		       + ValueBox::kHeight + kGap + controlsHeight() + kGap + kGap;
	}
	//-- PagedControls: the tabs ------------------------------------------------------------------------------------//
	[[nodiscard]] size_t pageCount() const override { return mod::arp::pat::kPatterns; }
	void showPage(size_t page) override { setFocus(page); }
	static float openHeight(size_t outputRows);

	/// THE PATTERN BEING EDITED (design §15: a tab press edits; it does not select for playback).
	void setFocus(size_t pattern);
	/// Tells the host which of this device's outputs the card shows (the open tab's: design §7.10).
	void showTabOutputs();
	[[nodiscard]] size_t focus() const { return focus_; }
	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// Every device control this card puts a widget on, every pattern's (row indices).
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;
	/// The card's Wobble box and the Humanize cell beside it (by design).
	[[nodiscard]] ValueBox& wobbleForTest() const { return *wobble_; }
	[[nodiscard]] ModeValueCell& humanizeForTest() const { return *humanize_; }
	[[nodiscard]] const PatternLane& lane() const { return *lane_; }
	[[nodiscard]] const PatternTabs& tabs() const { return *tabs_; }
	[[nodiscard]] const Knob& selectorKnob() const { return *selector_; }
	/// CYCLE'S WIDGETS: the Play Mode chip, and pattern `p`'s Next and Repeats boxes.
	[[nodiscard]] const SwitchChip& cycleChip() const { return *cycle_; }
	[[nodiscard]] const ValueBox& nextBoxForTest(size_t p) const { return *patterns_[p].next; }
	[[nodiscard]] const ValueBox& repeatsBoxForTest(size_t p) const { return *patterns_[p].repeats; }
	/// ONE PATTERN'S SCALE BOX — the long Choice list (46 scales) the choice-menu cases open.
	[[nodiscard]] ValueBox* scaleBoxForTest(size_t pattern) const { return pattern < patterns_.size() ? patterns_[pattern].scale.get() : nullptr; }
	/// AND ITS MODE BOX — a short Choice list (seven modes) beside it.
	[[nodiscard]] ValueBox* modeBoxForTest(size_t pattern) const { return pattern < patterns_.size() ? patterns_[pattern].mode.get() : nullptr; }

private:
	/// ONE PATTERN'S OWN WIDGETS, shown while it has the focus (its E and S chips always).
	struct PatternWidgets {
		std::unique_ptr<SwitchChip> enable;
		std::unique_ptr<SwitchChip> solo;
		std::unique_ptr<ValueBox> mode;
		std::unique_ptr<ValueBox> scale;
		std::unique_ptr<Knob> rate;
		std::unique_ptr<Knob> length;
		/// CYCLE'S CELL (design §7.6), in Cycle alone: in the selector's place, which Cycle does not read.
		std::unique_ptr<ValueBox> next;
		std::unique_ptr<ValueBox> repeats;
	};

	device::Modulatable* enableModulatable() override;
	void layoutControls();
	[[nodiscard]] std::vector<Knob*> knobs() const;
	/// WHETHER A CONTROL IS LIVE NOW: its gate (the honesty rule) read from the row, as the window reads it.
	[[nodiscard]] bool live(size_t index) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	size_t focus_ = 0;
	std::unique_ptr<PatternTabs> tabs_;
	std::array<PatternWidgets, mod::arp::pat::kPatterns> patterns_ {};
	std::unique_ptr<ValueBox> keys_;
	/// HUMANIZE AS SKLITTER'S GROOVE IS DRAWN (swing/humanize is a zone knob): the feel's
	/// names on a ZoneSelect, its Amount the box above them — two params, one zone-knob look (`ModeValueCell`).
	std::unique_ptr<ModeValueCell> humanize_;
	/// **WOBBLE ON THE CARD** (by design): a small box at Humanize's right, the window's Wobble too (the window is a
	/// superset). Like Humanize it is the window's alone in a horizontal chain's unit.
	std::unique_ptr<ValueBox> wobble_;
	static constexpr float kWobbleWidth = 50.0f;
	std::unique_ptr<SwitchChip> latch_;
	/// PLAY MODE AS A CHIP (two states, so next and which are the same thing): lit, Cycle.
	std::unique_ptr<SwitchChip> cycle_;
	float laidPlayMode_ = -1.0f;
	/// The selector's own control, with the card's compact readout ("1 · C Maj").
	std::unique_ptr<DeviceControl> compactSelector_;
	std::unique_ptr<Knob> selector_;
	std::unique_ptr<PatternLane> lane_;
	/// The focused pattern's panel, as laid out (drawn in `drawContent`, under the controls).
	visage::Bounds panel_ {};
	float laidMode_ = -1.0f;
};

} // namespace feathers::ui
