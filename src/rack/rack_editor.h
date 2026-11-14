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

#include "device/subscribers.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/card_factory.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/lane_selection.h"
#include "plugin/ui/libra_door.h"
#include "plugin/ui/macro_polarity.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/route_list.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/tooltip.h"
#include "rack/add_device.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include <visage/ui.h>

// THE RACK'S SURFACE — eight macros across the top, a card per device below.
//
// IT IS BUILT ENTIRELY FROM THE CARD FLOOR, which is the claim WP2 exists to make good on: `Knob`,
// `GenericCard`, `ModState`, `EditHistory` and the theme, and NOT ONE LINE of Feathers. If this file
// compiles, the floor is portable; if it needed something from `src/plugin` that the floor does not
// carry, the floor was not finished.
//
// A MACRO KNOB IS A `device::Modulatable` LIKE ANY OTHER. It is not a clap parameter as far as this
// widget is concerned — the plugin binding seats it at one, and the editor never finds out. That is
// the same indifference a device's control already has, arriving from the other end.
//
// WHAT IT DELIBERATELY DOES NOT HAVE: a device browser, expanded views, presets, a rail. Each is
// additive and none is on the path to "the backplane works". The `(+)` that adds a device is the one
// verb this surface is missing to be usable by hand, and it wants the tag tree that
// `2026-08-22-device-browser-and-unpinned-slots.md` describes rather than a menu invented here.

namespace feathers::rack {

class RackBridge;

/// One macro, as a control the floor's widgets understand.
///
/// IT WRITES THROUGH THE ENGINE and not through a parameter, because the engine is what the audio
/// thread reads. The bridge also queues each move for the host as a gesture (`setMacroByHand`, backlog NG),
/// which the plugin sends from `process` and `params.flush`; this is the road a hand takes.
class MacroControl final : public device::Modulatable {
public:
	MacroControl(RackBridge& bridge, size_t index, const char* name);

	const device::ParamDesc& desc() const override { return desc_; }
	double value() const override;
	void setValue(double value) override;
	std::string text() const override;

	/// A MACRO IS NOT ITSELF A DESTINATION. Nothing may be routed AT a macro — it is where routes come
	/// from — so this reports no destination and its knob never glows or draws a ring. `ModDisplay`'s
	/// own note says `isDestination` false makes every other field meaningless, which is exactly right
	/// here.
	device::ModDisplay display() const override;

	/// NOTHING MAY BE ROUTED AT A MACRO, so this is unreachable rather than unimplemented — `Knob`
	/// gates every amount road on `display().isDestination`, which is false above. It is written out
	/// because the contract is pure here, and a no-op that says WHY is better than one that reads as
	/// an oversight.
	void setAmount(float) override {}
	void beginAmountGesture() override {}
	void endAmountGesture() override {}

	/// **THE HAND'S GESTURE GOES TO THE HOST** (backlog NG): Begin and End on the macro's parameter, around the
	/// Values `setValue` sends (`RackBridge::beginMacroGesture`). These were empty, under a comment saying "the
	/// binding" carried them — no binding did, so the host never heard a macro move. A macro's undo still
	/// belongs to the DAW, which owns the parameter and now records the drag as one gesture.
	void beginGesture() override;
	void endGesture() override;

private:
	RackBridge& engine_;
	size_t index_ = 0;
	device::ParamDesc desc_ {};
};

/// THE ARROW THAT ARMS ONE MACRO.
///
/// `mod_panel.h` states the rule this follows: *"the card is the unit of 'a source you can point at',
/// and collapsing them into one card with four knobs would have left the arrow ambiguous about which
/// of them it was arming."* Eight macros are eight sources, so they get eight arrows.
///
/// WITHOUT ONE, NOTHING CAN BE ROUTED BY HAND AT ALL. The rack shipped its first playable build with
/// every part of the modulation surface present — destinations declared, routes stored, rings drawn,
/// amounts resolved — and no way to ARM a source, so none of it could be reached.
/// THE ARM THAT POINTS A MODULATION SOURCE AT A CONTROL — now `ui::SourceArm`, shared.
///
/// **IT WAS DEFINED HERE AND FEATHERS PAINTED ITS OWN.** Two implementations of one verb, which broke
/// the day a card grew its own frame: a panel draws BENEATH its children, so Feathers' painted chips
/// vanished under the card while these widgets did not. This is that api; the alias keeps this file's call sites reading
/// as they did.
using SourceArm = ui::SourceArm;

class ColumnBackdrop;

/// **THE LANE'S HORIZONTAL SCROLL BAR** (backlog NE): visible and draggable
/// while the chain is longer than its view, showing the visible span against the whole chain, and HIDDEN when nothing
/// overflows. It sits in the pad under the lane, so it costs no vertical room either way. A press on the thumb drags it;
/// a press on the track beside it jumps the thumb's centre there and keeps dragging. The wheel, the trackpad and the
/// edge scroll under a held card (`RackEditor::tickDrag`) all move the same scroll, and the thumb follows them.
/// **UNDO AND REDO, VISIBLE** (backlog JB: a visible undo/redo in the rack editor): two glyph buttons beside the
/// settings door, over the editor's history (the rack plugin's one; a nested editor's forwards to the outermost). Dim
/// while there is nothing to walk; they repaint when the history moves (`EditHistory::watch`).
class UndoRedoButtons final : public visage::Frame, public ui::TooltipSource {
public:
	static constexpr float kButton = 22.0f;
	static constexpr float kGap = 4.0f;
	static constexpr float kWidth = 2.0f * kButton + kGap;
	explicit UndoRedoButtons(ui::EditHistory& history);
	void draw(visage::Canvas& canvas) override;
	/// The two halves, in this frame's coordinates.
	[[nodiscard]] visage::Bounds undoBounds() const { return { 0.0f, 0.0f, kButton, height() }; }
	[[nodiscard]] visage::Bounds redoBounds() const { return { kButton + kGap, 0.0f, kButton, height() }; }
	/// WHICH BUTTON THE POINTER IS ON, in words — the icons are two hooked arrows and say nothing on their own.
	const char* tooltipAt(visage::Point local) const override;
	/// Writes no device control (it walks the shared history), so it offers none to the window-superset guard.
	void offeredControls(std::vector<Offered>&) const override {}

private:
	ui::EditHistory& history_;
	ui::Watch watch_;
};

class LaneScrollBar final : public visage::Frame {
public:
	static constexpr float kHeight = 8.0f;
	static constexpr float kMinThumb = 18.0f;
	struct Thumb {
		float x = 0.0f;
		float width = 0.0f;
	};
	/// THE THUMB on a track `track` px long, for a view `view` px wide over `view + maxScroll` px of chain, at `scroll`:
	/// its width the visible fraction (never under `kMinThumb`), its place the scroll's fraction of the travel.
	[[nodiscard]] static Thumb thumbFor(float track, float view, float maxScroll, float scroll) noexcept;
	/// THE SCROLL a thumb whose left edge is at `thumbX` stands for — `thumbFor`'s inverse, clamped.
	[[nodiscard]] static float scrollFor(float track, float view, float maxScroll, float thumbX) noexcept;

	LaneScrollBar();
	/// What it shows: the view's width, how far the chain can scroll, and how far it is. Hidden at `maxScroll` 0.
	void set(float view, float maxScroll, float scroll);
	/// Called with the absolute scroll a drag asks for.
	std::function<void(float)> onScroll;
	[[nodiscard]] Thumb thumb() const noexcept { return thumbFor(static_cast<float>(width()), view_, maxScroll_, scroll_); }
	void draw(visage::Canvas& canvas) override;

private:
	float view_ = 0.0f;
	float maxScroll_ = 0.0f;
	float scroll_ = 0.0f;
	float grab_ = 0.0f; ///< where in the thumb the press landed
	bool dragging_ = false;
};

class RackEditor : public visage::Frame, public ui::TooltipHost {
public:
	RackEditor(RackBridge& bridge, ui::ModState& mod, ui::EditHistory& history);
	~RackEditor() override;

	void resized() override;
	void draw(visage::Canvas& canvas) override;

	/// HOW FAR THE CHAIN IS SCROLLED ALONG ITS FLOW AXIS, in pixels, and how far it CAN be.
	///
	/// THE AXIS IS HORIZONTAL HERE AND VERTICAL IN FEATHERS, which is why this is "flow" rather than
	/// "x" or "y": `FxCard::laneExtent` made the two lanes one piece of code, and a scroll named after
	/// a direction would have been the one thing left claiming there are two.
	///
	/// THE RACK NEEDS THIS AND FEATHERS DOES NOT, which is why it is here and not in the floor: a
	/// synth's lane holds a handful of cards in a window sized for them, and a rack holds whatever a
	/// player drops in it. OWLTT's generic card alone is 842px in a 560px window — measured, and it is
	/// what "the full generic ui spills off the bottom and i cant scroll" was.
	///
	/// AND THAT MEASUREMENT NOW READS AS AN ARGUMENT RATHER THAN A BUG REPORT. Horizontally a card
	/// cannot spill: it gets the lane's full height and no more. The 842 does not go away, it moves —
	/// out of the lane's scroll and into the card's own problem, which is the expanded view.
	float scroll() const noexcept { return scroll_; }
	float maxScroll() const noexcept { return maxScroll_; }
	/// Returns whether the chain moved — false when it is already at the end the wheel is pushing
	/// towards, so the event can BUBBLE to whatever holds this editor (a `MountedFrame` on a canvas
	/// card scrolls the other axis; 2026-09-14: the inner chain's cards clipped off the canvas
	/// and could not be scrolled to because this editor answered every wheel).
	bool scrollBy(float delta);

	/// SAY SOMETHING TO THE PLAYER — one line, and a later one replaces an earlier one.
	///
	/// IT EXISTS BECAUSE A REFUSAL MUST BE SAID. `loadRack` counts the routes it dropped and names the
	/// first, and until now that count was captured and shown to nobody — which is the honesty rule's
	/// failure case in the quietest possible form: the build knew, and did not mention it.
	ui::Notifier& notifier() noexcept { return *notify_; }

	/// WHAT THE TRAY IS SHOWING — for a venue. Reading the frame's own text rather than a copy kept
	/// beside it, so a case cannot pass against a record of what was meant to be said.
	const std::string& notifierTextForTest() const { return notify_->text(); }

	/// THE ARMED SOURCE'S ROUTE LIST (rack roadmap §7f) — the same widget Feathers' Engine rail holds, in
	/// the macro row's right column: the rack's own routes from the armed source and the routes inside
	/// its chains, the amount draggable, the × removing, with undo. For a venue.
	ui::RouteList& routeListForTest() noexcept { return *routes_; }
	/// Where this editor's cards record their undo steps — the history it was built with.
	ui::EditHistory& historyForTest() noexcept { return history_; }
	/// The column the list has, so the macros share the row with it.
	static constexpr float kRouteListWidth = 200.0f;
	/// **THE MACROS ARE A NARROW BLOCK AT THE LEFT, TWO ACROSS AND FOUR TALL** (backlog OC), below the
	/// chain device's Mix and Gain (nested only) and an H-line between them. The lane takes the rest of the width; the
	/// route list stands UNDER the macros in the same column and SCROLLS in what is left (a left-hand Engine rail like
	/// Feathers', the window keeping its height). A macro is a cell `kMacroCellWidth` wide: the knob and its arm beside it.
	static constexpr float kMacroCellWidth = 100.0f;
	/// THE KNOB TAKES THE CELL'S LEFT 56 and its arm sits BESIDE it (not under), which is what leaves the column room for the
	/// route list below it; Mix and Gain stand over the two knobs.
	static constexpr float kMacroKnobWidth = 56.0f;
	static constexpr int kMacroColumns = 2;
	static constexpr float kMacroColumnWidth = kMacroCellWidth * static_cast<float>(kMacroColumns);
	/// WHERE THE LANE STARTS, from the left edge: the pad, the column, the pad.
	static constexpr float kLaneLeft = 12.0f + kMacroColumnWidth + 12.0f;

	//-- THE MACROS COLUMN, COLLAPSIBLE (backlog QN) --//
	// The column at the left — the settings door and presets, Mix and Gain, the macros, the route list — is the MACROS
	// column (it was called the inspector; Feathers' own became the Engine rail, QH). It folds as the Engine rail does: a
	// `ui::Rail` named MACROS whose toggle heads the column, a 34 px strip with the upright label when closed, a notice,
	// and the fold kept IN THE PATCH (`RackBridge::macrosCollapsed`, the canvas fold's posture), so it survives a save into a
	// fresh instance in the Rack plugin and in a Chain device's window alike, which share this editor.

	/// The row the toggle takes when the column has no door row and no Mix/Gain row to share: the Rail's toggle and its pad.
	static constexpr float kMacrosHeadHeight = 26.0f;
	/// The corner the open column gives its toggle, at the top right: the Rail's toggle and its inset either side.
	static constexpr float kMacrosToggleRoom = ui::Rail::kToggleSize + 2.0f * ui::Rail::kCollapsedPadX;
	/// WHERE THE LANE STARTS NOW: `kLaneLeft` with the column open, the closed strip and half a pad with it closed.
	[[nodiscard]] float laneLeft() const;
	[[nodiscard]] bool macrosCollapsed() const;
	/// Fold or unfold it — the toggle's own road, without its notice. True when that changed anything.
	bool setMacrosCollapsed(bool collapsed);
	/// Told after every fold or unfold (the hand's, a load's or `setMacrosCollapsed`'s), with the new state.
	std::function<void(bool)> onMacrosCollapsedChanged;
	/// The column's rail, for a case: its toggle, its width.
	[[nodiscard]] ui::Rail& macrosRailForTest() const;
	/// HOW TALL THE COLUMN IS AT LEAST, for a window with or without the chain's Mix and Gain and the settings door —
	/// what the default height has to clear. `columnNeeded` is the one arithmetic the layout and the cases both ask. The ROUTES list takes what is left under it.
	[[nodiscard]] static float columnNeeded(bool levels, bool door) noexcept;
	/// The macro column's boxes after layout, for the cases: a macro's knob, the arm under it, Mix/Gain, the H-line.
	[[nodiscard]] visage::Bounds macroBoundsForTest(size_t i) const { return macroSlots_[i]->bounds(); }
	[[nodiscard]] visage::Bounds macroArmBoundsForTest(size_t i) const { return macroArms_[i]->bounds(); }
	[[nodiscard]] visage::Bounds levelBoundsForTest(size_t i) const { return outerLevelKnobs_[i]->bounds(); }
	[[nodiscard]] size_t levelCountForTest() const noexcept { return outerLevelKnobs_.size(); }
	[[nodiscard]] visage::Bounds columnRuleForTest() const noexcept { return columnRule_; }
	[[nodiscard]] visage::Bounds columnBackdropForTest() const noexcept;
	[[nodiscard]] visage::Frame* columnBackdropFrameForTest() const noexcept;

	/// REDRAW ANY MACRO WHOSE VALUE HAS MOVED SINCE IT WAS LAST DRAWN.
	///
	/// THE ENGINE->EDITOR ROAD, and it did not exist: a knob reads `engine_.macro(i)` when it draws,
	/// and nothing was asking it to draw. Automation moved the sound and left the face where it was —
	/// the first hands-on report was "moving macros in daw doesnt seem to move the macros in the
	/// plugin".
	///
	/// A DIFF RATHER THAN AN UNCONDITIONAL REDRAW, because this runs on every beat: marking eight
	/// knobs stale sixty times a second would repaint a still panel for the life of the editor, which
	/// is the cost `NotifyTray::update` is shaped to avoid for the same reason.
	///
	/// **AND RE-SEAT ANY SLOT WHOSE TYPE MOVED** (`MacroSlot::syncType`, no string built): a patch load
	/// sets types through the bridge and no listener reports a type, so a Latch loaded over a Knob is a
	/// pad by the next beat — `ModPanel::syncMacroSlots`'s job, done on this editor's beat. A NAME needs
	/// no beat: the bridge's rename listener relabels its slot at once (`renameSubscription_`).
	void syncMacros();

	/// REBUILD THE CARD STACK FROM THE ENGINE. Called when the chain changes; cheap enough to call on
	/// a tick, because it compares tags before it builds anything.
	/// **THE ONE SLOT VERB THIS ENGINE HAS.** Feathers' menu holds three — Remove, Move earlier, Move
	/// later — and this one holds Remove alone, because reordering here is a DRAG.
	///
	/// Which is also what makes the other question answerable: whether this menu is needed at all if
	/// delete works with an effect focused. Once move is a gesture in both hosts, Remove is all
	/// either menu holds, and a focused delete would replace the whole mark rather than a third of it.
	///
	/// **EVERY DEVICE VERB HERE NAMES THE DEVICE BY ITS ID (kill-lanes step 2)** — never a slot or a
	/// lane. The bridge knows where it keeps a device; this editor knows which device a card is.
	void showSlotMenu(DeviceInstanceId device);

	/// **REMOVE A DEVICE — the menu's verb and the keyboard's, one implementation.** Two roads to a
	/// destructive action is two chances for one of them to skip the panel teardown.
	void removeDevice(DeviceInstanceId device);

	/// WHICH CARD A KEY WOULD ACT ON. `kNoInstance` when none — a fresh editor, or the device that held
	/// focus having just been removed.
	void focusDevice(DeviceInstanceId device);

	/// **REORDER BY DRAG.** The gesture is
	/// `FxCard`'s and is already settled — a second click arms, holding and moving reorders — so this
	/// is the lane's half: where the pointer is, which gap it names, and the commit.
	///
	/// Named `ForTest` on the fx lane's precedent: it is the seam a case drives, and the real caller is
	/// the card's `onCardDrag`.
	void cardDragForTest(DeviceInstanceId device, ui::FxCard::DragPhase phase, visage::Point inCard);
	std::optional<size_t> dropGapForTest() const noexcept { return dropGap_; }
	/// Whether the gap in flight is in a site's POST (backlog NZ) rather than this editor's own chain.
	bool dropIntoPostForTest() const noexcept { return dropIntoPost_; }
	visage::Bounds gapBoundsForTest(size_t gap) const { return gapBounds(gap); }
	/// Where a device's card sits in this editor, for a case that drives a drag in the card's coordinates.
	visage::Bounds cardBoundsForTest(DeviceInstanceId device) const;

	/// **THE LANE SCROLLS UNDER A HELD DRAG** (backlog DB) — the fx lane's rule, on this lane's axis: a
	/// card held inside the editor within `kEdgeScrollBand` of its left or right edge scrolls the chain
	/// `kEdgeScrollStep` that way per tick (`RackGui::tick`), and the drop gap follows. Returns whether it
	/// scrolled.
	bool tickDrag();
	/// THE TOOLTIP DELAY, advanced on the same beat (`RackGui::tick`). Returns whether it asked for a repaint.
	bool tickTooltips() { return tips_.advance(); }
	ui::TooltipLayer& tooltipLayer() override { return tips_; }
	/// The lane's horizontal scroll bar (backlog NE).
	[[nodiscard]] LaneScrollBar& laneScrollBarForTest() const noexcept { return *laneScrollBar_; }
	/// The visible undo/redo, or null while this editor has no door row.
	[[nodiscard]] UndoRedoButtons* undoRedoForTest() const noexcept { return undoRedo_.get(); }
	static constexpr float kEdgeScrollBand = 28.0f;
	static constexpr float kEdgeScrollStep = 6.0f;
	DeviceInstanceId focusedDeviceForTest() const noexcept { return focusedDevice_; }

	/// **THE SELECTION OVER THIS EDITOR'S OWN CHAIN** (backlog OA) — its cards, its verbs, its lasso.
	[[nodiscard]] ui::LaneSelection& selectionForTest() noexcept { return *selection_; }
	/// **AND OVER A SITE'S POST** (backlog OA), null without a tail. One chain's selection at a time: a selection made in
	/// one clears the other, because no verb acts across the centre (Group, a block move and Copy are one chain's).
	[[nodiscard]] ui::LaneSelection* postSelectionForTest() noexcept { return postSelection_.get(); }
	/// ONE OF THE DOTS MENU'S SELECTION VERBS, picked on `device`'s card — the menu's road, without a menu.
	bool slotVerbForTest(DeviceInstanceId device, ui::FragmentVerb verb);
	/// Where the verbs' option ids start in the dots menu (below them: the card's own Remove and Add before).
	static constexpr int kVerbIds = 100;

	void syncCards();

	//--------------------------------------------------------------------------------------------//
	// THE SITE'S MICROCHAIN (backlog NZ;)                                        //
	//--------------------------------------------------------------------------------------------//
	//
	// THE SPECIFICATION: pressing the chain chip opens ONE ordinary horizontal-scrolling chain view — the PRE devices, then
	// the MAIN effect itself, then the POST devices, separated by partitions, with the main effect outlined as the centre. This
	// editor's own chain is the site's PRE. After it come a partition, the device (the CENTRE: its own card over the
	// host it runs in, so it is the same instance and every edit passes straight through), a partition, and the POST
	// lane's cards with their own (+). Screen order is reach order: each segment's controls answer to a ModState that
	// follows the one before it (`SiteExpanded`), so a modulator glows exactly what lies to its right.

	struct SiteTail {
		/// The host the device runs in, and the device.
		backplane::DeviceHost* outer = nullptr;
		DeviceInstanceId centre = kNoInstance;
		/// The POST lane's bridge.
		RackBridge* post = nullptr;
		/// What the centre card and the POST cards arm and read; the centre's history is the host's.
		ui::ModState* centreMod = nullptr;
		ui::ModState* postMod = nullptr;
		ui::EditHistory* centreHistory = nullptr;
	};
	/// Attach the tail (or, with `tail.outer` null, take it away) and build its cards.
	void setSiteTail(const SiteTail& tail);
	/// **PRE IS UNAVAILABLE WHILE THE DEVICE RUNS IN THE VOICES (D4)**: its segment is veiled, says `why`, refuses the
	/// hand, and offers no (+). Available again restores it.
	void setPreAvailable(bool available, std::string why = {});
	/// The PRE veil's reason as drawn (one string per row) and the veil's bounds; empty / zero when there is no veil.
	[[nodiscard]] std::vector<std::string> preVeilLinesForTest() const;
	[[nodiscard]] visage::Bounds preVeilBoundsForTest() const;
	[[nodiscard]] bool preVeilVisibleForTest() const noexcept { return preVeil_ != nullptr && preVeil_->isVisible(); }
	[[nodiscard]] bool preAvailableForTest() const noexcept { return preAvailable_; }
	[[nodiscard]] ui::FxCard* centreCardForTest() const noexcept { return centreCard_.get(); }
	[[nodiscard]] std::vector<ui::FxCard*> postCardsForTest() const;
	[[nodiscard]] AddDeviceRow* postAddRowForTest() noexcept { return postAddRow_.get(); }
	/// The two partitions' x, in this editor's coordinates: before the centre and after it.
	[[nodiscard]] float partitionForTest(size_t i) const noexcept { return i < 2 ? partitionX_[i] : 0.0f; }
	/// **THE SITE'S FURNITURE IN THE HEAD ROW** — the device's declared Mix and wet Gain (backlog NR's D1/D2 roles), the
	/// same two knobs a Chain container's panel carries. Empty when the device declares neither.
	[[nodiscard]] size_t furnitureKnobCountForTest() const noexcept { return outerLevelKnobs_.size(); }

	/// ADD ONE DEVICE AND SAY SO IF IT WOULD NOT FIT — the road every pick from the add menu takes.
	///
	/// SHARED RATHER THAN COPIED because the refusal is the part that gets forgotten: a second
	/// presentation with its own inline `addDevice` call is a second place for a full rack to fail
	/// silently, which is the exact bug the notifier was added to close.
	/// Returns the new device's id, or `kNoInstance` when refused (and said so). An "Add before" that
	/// set `insertAnchor_` lands here as an insert before that device (backlog CR).
	DeviceInstanceId addDeviceByTag(const char* tag);

	/// **THE ADD MENU — FEATHERS' OWN** (backlog KP, 2026-09-28: the rack should use Feathers' browser,
	/// by category). One submenu per category through `ui::addCategorisedDevices`, placed by `ui::showMenuAt`
	/// against `anchor` (in `source`'s coordinates), which flips and sides it to fit any window at any UI
	/// scale. It replaced a full-window takeover list that had no scroll and was cut off at large scales.
	/// Both the (+) and "Add before" open it; the pick lands through `addDeviceByTag`.
	void showAddMenu(visage::Frame& source, const visage::Bounds& anchor);

	/// "Add before" for the card of `anchor`, then the pick of `tag` — the whole gesture without the
	/// popup and the browser. Returns the new device's id, or kNoInstance.
	DeviceInstanceId insertDeviceBeforeForTest(DeviceInstanceId anchor, const char* tag);

	/// PUT THE LAYERS THAT MUST DRAW OVER EVERYTHING BACK ON TOP.
	///
	/// **VISAGE DRAWS CHILDREN IN THE ORDER THEY WERE ADDED, AND CARDS ARE ADDED LATE.** The notifier
	/// and the surface host are built in the constructor; a card is added by `syncCards` every time a
	/// device arrives — so every card became a LATER child than both, and drew over them..
	///
	/// THERE IS NO Z-ORDER TO SET. `Frame` has no depth and no "always on top"; sibling order IS the
	/// depth, so the only way to be in front is to be last. This is `SurfaceKind`'s "top layer" — the
	/// thing `<dialog>` exists to provide on the web — arriving as the one mechanism visage has.
	///
	/// CALLED AFTER ANYTHING THAT ADDS A CHILD, which today is exactly `syncCards`.
	void raiseTopLayers();

	/// HOW MANY TIMES THE LAYERS WERE ACTUALLY RE-ADDED — a count produced BY the road, so a raise that
	/// starts happening every beat again cannot look like one that had nothing to do. `Gui::notify()`
	/// returns its frame count for the same reason: the alternative is a counter standing beside the
	/// call, which can agree with a road that is not running.
	size_t raisesForTest() const noexcept { return raises_; }

	/// THE LANE KEEPS THE 720 IT HAD; the macro column is added to it (it used to be the row above).
	static constexpr int kWidth = 720 + static_cast<int>(kLaneLeft - 12.0f);
	/// **ONE VERTICAL UNIT: a pad, one card unit (`ui::kCardUnitHeight`) and the pad under it, and the Engine rail column beside it**
	/// — 106 + 12 + 336 + 12 (asserted against the layout's own constants in rack_editor.cpp). It is the design
	/// height the window prefers and floors at, with the canvas's band above it.. It was 560, which left 94 empty
	/// rows under every card, and because `adjust_size` floors at the design times the scale, a host that
	/// opened the window at `get_size`'s answer snapped it to that height on the first drag (a snap to a
	/// too-large default size on the first resize: 1040 rows at 1.75 against the 875 one unit needs).
	static constexpr int kHeight = 466;

	/// ADVANCE EVERYTHING THAT MOVES WITHOUT A GESTURE, AND SAY HOW MANY FRAMES WERE TOLD.
	///
	/// **THE RACK HAD NO SUCH ROAD AT ALL**, which is why an OWLTT analyser that moved only
	/// while the mouse was over it and an Automod whose curves never moved. Feathers drives four of
	/// these out of `Gui::Impl::tick()` (`fxLane`, `modPanel`, and both expanded layers); the rack
	/// drove none, so a card repainted only when an input event happened to reach it.
	///
	/// The count is produced BY the work, per the notify-count rule: a card that answers true without
	/// having asked for a frame is lying to a measurement.
	size_t updateCards();

	/// HOW MANY MOD OUTPUTS A DEVICE PUBLISHES — zero for an effect, which is what makes its card's
	/// outputs strip empty and costs it nothing.
	size_t outputsOf(DeviceInstanceId device) const;


	/// WHETHER THE OPEN PANEL IS ON THE TICK ROAD. The defect this exists for was the ABSENCE of the
	/// road, and a road's absence is not visible in any rendered frame — a panel that never advances
	/// draws a perfectly plausible still picture. Per the value-assertion rule: read the wiring, do
	/// not infer it from pixels.
	/// **THE TICK IS THE MOUNT'S NOW** (`MountedFrame::updateContent`), installed by `SurfaceLayers`
	/// where the type is known and freed with the mount — so "on the tick road" and "presented" are one
	/// fact, and this reads the id that says so.
	bool panelTickWiredForTest() const noexcept { return !expanded_.empty(); }
	/// THE MOST RECENT PANEL THIS EDITOR PRESENTED that is still up, or none.
	ui::SurfaceId expandedIdForTest() const noexcept { return expanded_.empty() ? ui::kNoSurface : expanded_.back(); }
	size_t expandedCountForTest() const noexcept { return expanded_.size(); }

	/// THE OUTPUT ARMS, FOR ASSERTING THE SOURCE THEY POINT AT. "An arm exists" is the weak claim —
	/// an arm wired to the WRONG source arms somebody else's output and looks identical on screen, so
	/// the case reads the device and the output rather than counting widgets.
	size_t outputArmCountForTest() const noexcept { return outputArms_.size(); }
	DeviceInstanceId outputArmDeviceForTest(size_t i) const noexcept { return outputArms_[i].device; }
	size_t outputArmOutputForTest(size_t i) const noexcept { return outputArms_[i].output; }
	visage::Frame* outputArmFrameForTest(size_t i) const noexcept { return outputArms_[i].arm.get(); }

	/// **WHERE ONE DEVICE'S n-th OUTPUT ARM IS, IN THE EDITOR'S COORDINATES** — or an empty rectangle
	/// if this editor has no such arm, which is itself the answer a case about "every output has one"
	/// wants to see.
	///
	/// BY ENDPOINT RATHER THAN BY INDEX INTO `outputArms_`, because the index is a position in a
	/// vector rebuilt whenever the chain changes and the endpoint is the device's own identity. A case
	/// walking the registry knows which output it is asking about; it does not know where the arm for
	/// it landed in a list.
	visage::Bounds outputArmBoundsForTest(DeviceInstanceId device, size_t output) const noexcept {
		for (const OutputArm& entry : outputArms_) {
			if (entry.device == device && entry.output == output && entry.arm != nullptr) {
				return entry.arm->bounds();
			}
		}
		return {};
	}
	/// Whether that arm is SHOWN — a folded card's arms are placed and hidden, so bounds alone cannot
	/// tell the two apart.
	bool outputArmVisibleForTest(DeviceInstanceId device, size_t output) const noexcept {
		for (const OutputArm& entry : outputArms_) {
			if (entry.device == device && entry.output == output && entry.arm != nullptr) {
				return entry.arm->isVisible();
			}
		}
		return false;
	}

	/// **THE CARD OF ONE DEVICE, BY ITS ID** — or null for a device this editor draws no card for.
	///
	/// A card belongs to a DEVICE, so it neither moves nor is rebuilt when the rack is rearranged; the
	/// order on screen comes from the graph (`RackBridge::deviceAtPosition`), and every walk in this
	/// file that means "as drawn" goes through it — the layout, the drop gaps, the bumper. The cards
	/// were indexed by LANE until kill-lanes step 2, and four walks in this file and its tests read that
	/// array as order, all correct until the first reorder: an id has no order to be mistaken for.
	ui::FxCard* cardOfDeviceForTest(DeviceInstanceId device) const noexcept;
	/// **EVERY CARD, IN CHAIN ORDER** — the order they are drawn in.
	std::vector<ui::FxCard*> cardsForTest() const;
	/// **EVERY CARD THE EDITOR HOLDS**, whether or not the chain still names its device — the count a case
	/// needs to prove a removed device's card went, which a walk of the chain cannot see.
	size_t cardCountForTest() const noexcept {
		size_t n = 0;
		for (const CardEntry& entry : cards_) {
			n += entry.card != nullptr ? 1 : 0;
		}
		return n;
	}
	const std::vector<std::unique_ptr<ui::MacroSlot>>& macrosForTest() const { return macroSlots_; }
	AddDeviceRow* addRowForTest() noexcept { return addRow_.get(); }

	/// WHERE A GUEST ASKS FOR A SURFACE.
	///
	/// BOTH KINDS NOW. `SelfContained` takes the takeover — the browser, which wants the whole window
	/// and gives a value back. `SpansGesture` takes the overlay, which is the only kind that leaves
	/// the editor REACHABLE beneath it, and the card's expand verb is exactly that: a panel whose
	/// controls a hand must be able to route to while the mod panel is still on screen.
	///
	/// EXPOSED RATHER THAN INHERITED, and there is NO LOGIC HERE, for the same reason
	/// `bridge_device_host.h` has none: a forwarding layer that does anything is a second place the
	/// contract can be got wrong.
	ui::SurfaceHost& surfaces() noexcept { return *surfaces_; }
	ui::SurfaceLayers& surfaceLayersForTest() noexcept { return *surfaces_; }
	/// THE EDITOR'S ESC STACK (backlog NN) — its layers', which is the outermost host's when nested.
	ui::EscapeStack& escapeStack() noexcept { return surfaces_->escapeStack(); }
	ui::NotifyTray& notifyTrayForTest() noexcept { return *notify_; }
	ui::TakeoverStack& takeoverForTest() noexcept { return surfaces_->takeover(); }
	ui::ExpandedOverlay& overlayForTest() noexcept { return surfaces_->overlay(); }

	/// **THE SETTINGS SCREEN'S DOOR** (backlog LG): the Libra mark at the head of the macro column (backlog OC), which the
	/// macros stand under. Feathers' door is its header's mark; the rack has no header, so the mark sits at
	/// the head of the surface it does have. Only the TOP-LEVEL rack asks for one (`RackGui`); a chain's
	/// window, which is this editor nested, has none, so its macros keep their full width.
	void setSettingsDoor(std::function<void()> open);
	ui::LibraDoor* settingsDoorForTest() noexcept { return settingsDoor_.get(); }
	/// **THE PLUGIN'S PRESETS** (backlog OU) — a browser over `Chains/`, beside the door on its row: the rack plugin IS a
	/// chain and eight macros, so a chain preset is its whole state. Borrowed (`RackGui` owns it); only the top-level
	/// rack asks for one, as for the door. Null removes it.
	void setPresetBrowser(visage::Frame* browser);
	[[nodiscard]] visage::Frame* presetBrowserForTest() const noexcept { return presets_; }	/// The door's column, before the first macro.
	static constexpr float kSettingsDoorWidth = 22.0f;

	/// THE CANVAS — the band ABOVE the macro row where an expanded view docks (`expanded_canvas.h`).
	/// Owned and
	/// placed HERE, like a rail, because it is a region of this layout; `SurfaceLayers` only routes to it.
	ui::ExpandedCanvas& canvas() noexcept { return *canvas_; }
	/// HOW MUCH OF THE EDITOR'S HEIGHT THE CANVAS TAKES — zero when nested (see `setOuterLayers`).
	/// Everything else starts below it, and `RackGui` adds this to the logical height it reports.
	float canvasExtent() const noexcept { return canvas_ != nullptr ? canvas_->extent() : 0.0f; }
	/// TOLD WHEN THE EXTENT CHANGES, so the window can ask its host for the size this editor now
	/// prefers. The layout has already been redone by the time this fires.
	std::function<void()> onCanvasExtentChanged;
	/// **WHAT THIS EDITOR'S ADD MENU OFFERS, NARROWED** — empty offers everything the rack can run (the Chain plugin). The
	/// Note Rack plugin sets "a modulator or a note effect" (`belongsInModulationRack`): no audio effect in a rack whose
	/// only job is notes. An OFFER, never a capability, as Feathers' strip states for the same rule.
	std::function<bool(const device::Descriptor&)> offerFilter;
	/// What the add menu offers now: every device the rack can run, narrowed by `offerFilter`.
	[[nodiscard]] std::vector<const device::Descriptor*> offer() const;
	/// A PARTITION DRAG STEP, with the pointer's window position, BEFORE this editor lays out — so the
	/// host that owns the window can put the partition under the pointer physically and re-derive the
	/// scale (`ExpandedCanvas::partitionExtent`), keeping the lane at its design height.
	std::function<void(visage::Point)> onCanvasDrag;
	/// HOW MANY LOGICAL ROWS THE WINDOW HOLDS, asked as the canvas opens so the band can take the room
	/// that exists rather than ask the host for more (`ExpandedCanvas::fitOpenExtentTo`). Unset, or
	/// answering zero, leaves the saved extent as it is.
	std::function<double()> windowRoomHeight;
	/// NESTED IN ANOTHER EDITOR (a chain device's panel): no canvas of its own, and every spanning
	/// present escalated to `outer` — rack roadmap §5. Null puts it back on its own.
	void setOuterLayers(ui::SurfaceLayers* outer);

	/// **WHICH CHAIN THIS EDITOR'S DEVICES ARE IN** — `ui::DeviceNest`, and the default is this rack's
	/// own, which is what a top-level editor wants and therefore never sets. A chain device's panel
	/// (`ChainExpanded`) sets it one level deeper, because a `RackEngine` counts its instances from
	/// `kFirstInstance` whether or not it is nested: without this, a device in a nested chain and a
	/// device in the outer one share an id, a tint, and — worse — an `ExpandedOverlay` key, so the
	/// second panel opened retires the first.
	void setNest(ui::DeviceNest nest);
	[[nodiscard]] ui::DeviceNest nest() const noexcept { return nest_; }

	/// **WHETHER A DEVICE ADDED HERE COULD STILL HOLD A CHAIN OF ITS OWN** — false once this editor is
	/// `rack::kMaxUsefulDepth` chains deep, which is exactly where `loadState` refuses a body.
	/// `ChainColumn::nestingFull` is the same test on the other surface, against the same constant, so
	/// the two cannot bound a hand differently. Public because a case must read the editor's own answer
	/// rather than restate the arithmetic it is checking.
	[[nodiscard]] bool nestingFull() const noexcept;

	/// **NESTED, THE MACRO ROW IS THE CHAIN DEVICE'S OWN EIGHT PARAMS, AND SO A DESTINATION OF THE HOST
	/// THIS CHAIN SITS IN.** Standalone, a macro
	/// is a source only (`MacroControl::display` refuses to be one). Inside a chain the same eight are
	/// the composite device's params on the OUTER host — the card in the outer lane already offers
	/// them — so the row's knobs are rebuilt over `DeviceControl`s addressed to that device, on the
	/// outer modulation state: an outer source armed lights them, a drag writes the outer route, the
	/// ring shows the outer modulation, and the value still lands in this rack's engine through the
	/// chain's `setFields`. The arms stay this rack's: a macro is still a source INSIDE.
	/// `host == nullptr` puts the row back to standalone.
	void setOuterDevice(backplane::DeviceHost* host, DeviceInstanceId device, ui::ModState* mod,
	                    ui::EditHistory* history);
	[[nodiscard]] bool macrosAreOuterForTest() const noexcept { return !outerMacros_.empty(); }
	/// THE CANVAS FOLLOWS THE PATCH: the bridge's `CanvasView` applied to the frame when they differ,
	/// on the beat like the macros, so a load moves the partition and folds the band as saved. Returns
	/// whether anything moved.
	bool syncCanvas();

	/// HOW MANY TIMES A MACRO HAS BEEN MARKED STALE — a count produced BY the redrawing, so a road
	/// that quietly stops redrawing cannot look like one that had nothing to do. `Gui::notify()`
	/// returns its frame count for exactly this reason.
	size_t macroRedrawsForTest() const noexcept { return macroRedraws_; }
	/// HOW MANY TIMES `syncMacros` RE-SEATED A SLOT FOR A TYPE THAT MOVED — counted from `syncType`'s own
	/// answer, for `macroRedraws_`'s reason. The rack's tick has no report to carry it, so a case reads it.
	size_t macroResyncsForTest() const noexcept { return macroResyncs_; }

private:
	RackBridge& engine_;
	ui::ModState& mod_;
	ui::EditHistory& history_;
	ui::WidgetRegistry registry_;
	/// WHICH CARD A KEY ACTS ON — see `focusDevice`. `kNoInstance` until a hand presses one.
	DeviceInstanceId focusedDevice_ = kNoInstance;
	/// BACKLOG OA — see `selectionForTest`; and the frame that draws its lasso and pill above the cards.
	std::unique_ptr<ui::LaneSelection> selection_;
	std::unique_ptr<ui::SelectionOverlay> selectionOverlay_;
	[[nodiscard]] std::vector<DeviceInstanceId> chainOrder() const;
	/// POST's devices in order (empty without a tail), and every device this editor draws: PRE's, then POST's.
	[[nodiscard]] std::vector<DeviceInstanceId> postOrder() const;
	[[nodiscard]] std::vector<DeviceInstanceId> editOrder() const;
	/// The selection a key, a menu or a drag of `device` acts on: POST's for a POST card, else this editor's own.
	[[nodiscard]] ui::LaneSelection& selectionOf(DeviceInstanceId device);
	/// The selection the keys act on: POST's while it holds anything, else this editor's own.
	[[nodiscard]] ui::LaneSelection& keySelection();
	/// The selection (and so the verbs and their history) of `lane`: POST's for POST, this editor's otherwise.
	[[nodiscard]] ui::LaneSelection& selectionOfLane(RackBridge& lane);
	/// Every card flagged from the selection, and the key's target from its anchor.
	void applySelection();
	/// ONE CHAIN'S SELECTION AT A TIME: `changed` has something selected, so the other chain's is cleared. A lasso keeps
	/// to the chain it first touched (`lassoChain_`).
	void exclusiveSelection(ui::LaneSelection& changed);
	enum class LassoChain { None, Pre, Post };
	LassoChain lassoChain_ = LassoChain::None;
	bool lassoing_ = false;
	/// After a verb: a panel on a device that went is dismissed, the cards follow, the selection is pruned.
	void afterVerb(const std::vector<DeviceInstanceId>& before);
	/// THE DRAG IN FLIGHT — which card's device the hand took, and which gap it is currently over.
	std::optional<DeviceInstanceId> dragDevice_;
	std::optional<size_t> dropGap_;
	/// WHICH CHAIN `dropGap_` COUNTS IN (backlog NZ): a site's POST, or this editor's own (PRE, in a site).
	bool dropIntoPost_ = false;
	/// The gap a pointer names in a site's view, and in which chain: left of the first partition is PRE, right of the
	/// second is POST, and over the centre is nothing (the device is where the site sits, not a place to drop).
	struct DropTarget {
		bool post = false;
		size_t gap = 0;
	};
	std::optional<DropTarget> dropTargetAt(visage::Point position) const;
	void setDropTarget(std::optional<DropTarget> target) noexcept {
		dropGap_ = target.has_value() ? std::optional<size_t> { target->gap } : std::nullopt;
		dropIntoPost_ = target.has_value() && target->post;
	}
	/// The live cards of one chain, in chain order — what a gap counts over and the bumper is drawn between.
	std::vector<const ui::FxCard*> liveCards(bool post) const;
	/// A DROP THAT TOUCHES A SITE'S POST — within it, or across the centre either way — committed as one undo step.
	void commitSiteDrop(DeviceInstanceId from, bool fromPost, bool intoPost, size_t gap);
	/// Where the hand last was, in this editor's coordinates — what `tickDrag` scrolls against.
	visage::Point dragPoint_ { 0.0f, 0.0f };
	/// WHERE THE BUMPER IS DRAWN for a gap — a vertical bar BETWEEN two cards, which is the fx lane's
	/// horizontal one turned ninety degrees along with everything else in this lane.
	visage::Bounds gapBounds(size_t gap) const;
	/// THE MACRO MENU'S HOOKS FOR MACRO `index` — the bridge's reads and verbs,
	/// one spelling for the standalone row and both of `setOuterDevice`'s.
	ui::MacroMenuHooks macroMenuHooks(size_t index);
	/// **WHAT MACRO `index` IS CALLED, AND THE RENAME** — the bridge's `macroName`/`macroRenamed`/
	/// `setMacroName`, each asking `alive_` first like the hooks: a copy of these that outlives the editor
	/// answers the default name and writes nothing.
	ui::MacroSlot::Naming macroNaming(size_t index);
	/// ONE SLOT OVER `control` for macro `index`, captioned and wired the one way all three rows share.
	std::unique_ptr<ui::MacroSlot> makeMacroSlot(device::Modulatable& control, size_t index, ui::ModState* mod,
	                                             ui::EditHistory* history);
	/// Redraws macro `index`'s slot (knob and pad) if the row still has one — looked up at the call.
	void redrawMacroKnob(size_t index);
	/// **THIS EDITOR IS ALIVE WHILE THIS IS** — what the macro menu's hooks hold weakly. visage parents
	/// a popup to the ROOT frame, so an open menu outlives everything below it; nested, the root is the
	/// Feathers editor and `ChainExpanded::follow()` drops and rebuilds this editor on a tick whenever
	/// the host files a new handle. A pick made after that must find nothing to write into rather than
	/// `engine_` on a freed editor. Released first thing in the destructor.
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
	std::vector<std::unique_ptr<MacroControl>> macros_;
	/// THE POLARITY VIEWS THE MACRO KNOBS ARE BUILT OVER (backlog EG), one per macro; after `macros_`
	/// because each refers to its macro. Empty while the row is the outer chain device's.
	std::vector<std::unique_ptr<ui::BipolarMacro>> macroViews_;
	/// Nested only — see `setOuterDevice`. Empty standalone.
	std::vector<std::unique_ptr<ui::DeviceControl>> outerMacros_;
	/// **THE CHAIN DEVICE'S MIX AND GAIN, NESTED ONLY** — its card has them, and a device's window is strictly a
	/// superset of its card, so the window's head row carries them after the macros. Empty
	/// standalone, where there is no outer device. Before `outerLevelKnobs_`, which reads them and goes first.
	std::vector<std::unique_ptr<ui::DeviceControl>> outerLevels_;
	std::vector<std::unique_ptr<ui::Knob>> outerLevelKnobs_;
	/// THE ROW: one `MacroSlot` per macro — the knob, the pad a Latch or Momentary becomes, and the field
	/// Rename opens — over `macroViews_`. Rebuilt by `setOuterDevice`.
	std::vector<std::unique_ptr<ui::MacroSlot>> macroSlots_;
	std::vector<std::unique_ptr<SourceArm>> macroArms_;
	/// **A NAME HEARD FROM THE BRIDGE RELABELS ITS SLOT** — a rename from the slot's own field, and every
	/// name a patch load moves. After `macroSlots_`, so it is destroyed first; the destructor also ends
	/// it explicitly, first thing, beside `alive_`. The bridge outlives this editor.
	device::Subscription renameSubscription_;
	/// ONE ARM PER DEVICE OUTPUT, parallel to `cards_` and rebuilt with them.
	///
	/// A modulator in the rack was drawable and ROUTABLE — the engine ticks it (`mod->tick`) and can
	/// read it (`outputAt`) — with no way to ARM it. The capability existed at every layer except the
	/// one a hand touches. Each arm names its source by endpoint: the device's id and its output.
	struct OutputArm {
		std::unique_ptr<SourceArm> arm;
		DeviceInstanceId device = kNoInstance;
		size_t output = 0;
	};
	std::vector<OutputArm> outputArms_;
	/// **A RELABELLED OUTPUT RENAMES ITS ARM** (`RackBridge::subscribeOutputLabel`) and re-lays the lane,
	/// because the chip is as wide as its word. After `outputArms_`, so destroyed first; the destructor
	/// ends it explicitly beside `renameSubscription_`.
	device::Subscription labelSubscription_;
	/// ONE CARD PER DEVICE THE CHAIN HOLDS, KEYED BY THE DEVICE — in no particular order, because the
	/// order is the graph's (see `cardOfDeviceForTest`). A device gets its CUSTOM card when one exists on
	/// the seam and a `GenericCard` otherwise — see `ui::makeCustomCard`; holding `GenericCard` here was
	/// what made the rack incapable of showing a custom UI at all.
	struct CardEntry {
		DeviceInstanceId device = kNoInstance;
		/// WHAT THE CARD WAS BUILT FOR — compared on every sync, so a device whose descriptor changed
		/// under its id gets a new card rather than the old one showing the wrong controls.
		const void* desc = nullptr;
		/// AND FOR A DEVICE THIS BUILD DOES NOT HAVE (a null descriptor), THE TAG ITS CARD SAYS.
		device::Tag missing {};
		std::unique_ptr<ui::FxCard> card;
	};
	std::vector<CardEntry> cards_;
	/// The entry for `device`, or null — this editor's own chain first, then a site's POST.
	CardEntry* entryOf(DeviceInstanceId device) noexcept;
	const CardEntry* entryOf(DeviceInstanceId device) const noexcept;
	/// **THE BRIDGE A DEVICE'S CARD BELONGS TO** — a site's POST lane for one of its cards, this editor's own otherwise.
	/// A site's two lanes share one id counter, so an id names one lane.
	RackBridge& laneOf(DeviceInstanceId device) noexcept;
	/// ONE LANE'S CARDS AND ARMS BROUGHT INTO LINE WITH ITS BRIDGE — this editor's own chain, or a site's POST. Returns
	/// whether anything was built or retired. Both are reordered by drag (`commitSiteDrop` for POST).
	bool syncLane(RackBridge& lane, std::vector<CardEntry>& cards, std::vector<OutputArm>& arms, ui::ModState& mod,
	              ui::EditHistory& history, bool own);
	/// One lane's cards placed from `x` along the lane, their arms on their feet; returns where the lane ended.
	float placeLane(RackBridge& lane, std::vector<OutputArm>& laneArms, float x, float laneTop, float laneHeight);
	/// The centre's card and the POST lane, brought into line (see `syncLane`).
	bool syncTail();
	/// THE SITE'S FURNITURE: the centre device's declared Mix and wet Gain as knobs after the macros.
	void syncFurniture();

	SiteTail tail_ {};
	std::unique_ptr<ui::FxCard> centreCard_;
	const void* centreDesc_ = nullptr;
	std::vector<CardEntry> postCards_;
	std::vector<OutputArm> postArms_;
	std::unique_ptr<AddDeviceRow> postAddRow_;
	/// THE SELECTION AND VERBS OVER POST'S BRIDGE (backlog NZ, OA) — a drag within POST is its block move, and Group, Copy
	/// and Duplicate act on POST's selection; each one undo step in this history.
	std::unique_ptr<ui::LaneSelection> postSelection_;
	/// Whether the next pick from the add menu appends to the site's POST (its (+) was pressed) rather than here.
	bool addToPost_ = false;
	std::array<float, 2> partitionX_ {};
	bool preAvailable_ = true;
	std::unique_ptr<visage::Frame> preVeil_;
	void writeCanvasView();
	std::unique_ptr<ui::NotifyTray> notify_;
	std::unique_ptr<ui::LibraDoor> settingsDoor_;
	visage::Frame* presets_ = nullptr;
	/// THE COLUMN'S OWN GROUND, solid and deaf, UNDER its controls and OVER the cards: the lane scrolls sideways beneath
	/// the column, and a card passing it is hidden here rather than drawn through the macros. The column items are raised
	/// above the cards whenever one was added (`columnDirty_`), never on every beat (a re-added frame drops its drag).
	std::unique_ptr<ColumnBackdrop> columnBackdrop_;
	/// THE VISIBLE UNDO/REDO (backlog JB), on the settings door's row while there is one (the rack plugin's editor).
	std::unique_ptr<UndoRedoButtons> undoRedo_;
	/// THE EDITOR'S OWN TOOLTIP LAYER, for what sits on the editor rather than on a card (the undo and redo buttons). Last
	/// among the raised layers (`raiseTopLayers`), advanced on the tick (`tickTooltips`).
	ui::TooltipLayer tips_;
	/// THE LANE'S HORIZONTAL SCROLL BAR (backlog NE), in the pad under the lane, raised with the column over the cards.
	std::unique_ptr<LaneScrollBar> laneScrollBar_;
	visage::Bounds columnRule_ {};
	bool columnDirty_ = false;
	/// Place the macro column (and the door, Mix and Gain) from `top`; returns the y its last row ends at.
	float layOutColumn(float top);
	std::unique_ptr<ui::RouteList> routes_;
	/// THE MACROS COLUMN'S RAIL (backlog QN): its head row's toggle and heading when open, the whole strip when closed.
	std::unique_ptr<ui::Rail> macrosRail_;
	/// Show or hide everything the column holds, for its fold.
	void showColumn(bool shown);
	std::unique_ptr<AddDeviceRow> addRow_;
	/// THE DEVICE AN "Add before" WAS CHOSEN FOR, until its pick lands or its add menu closes. See
	/// `addDeviceByTag` for why this is state rather than a captured argument (backlog CR).
	std::optional<DeviceInstanceId> insertAnchor_;
	/// THE CANVAS, DECLARED BEFORE THE LAYERS so it is destroyed after them: the layers unhook their
	/// dismissal road from it as they go.
	std::unique_ptr<ui::ExpandedCanvas> canvas_;
	std::unique_ptr<ui::SurfaceLayers> surfaces_;
	/// EVERY PANEL THIS EDITOR PRESENTED AND STILL BELIEVES IS UP — several since 2026-09-14. Pruned
	/// against `SurfaceLayers::holds` when the layers say a panel went, since a dismissal names no id
	/// to the presenter.
	std::vector<ui::SurfaceId> expanded_;
	ui::DeviceNest nest_ {};
	void pruneExpanded();
	/// THE THUNK THAT ADVANCED THE OPEN PANEL USED TO STAND HERE, built at the present site because
	/// the seam carries a bare Frame. It is in the mount now (`SurfaceLayers::mount`), built at the
	/// same moment for the same reason, and Feathers' panels ride the identical road — one code path.
	size_t raises_ = 0;
	std::vector<float> macroDrawn_;
	size_t macroRedraws_ = 0;
	size_t macroResyncs_ = 0;
	float scroll_ = 0.0f;
	float maxScroll_ = 0.0f;
};

} // namespace feathers::rack
