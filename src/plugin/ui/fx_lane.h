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

#include "plugin/ui/widget_registry.h"
#include "plugin/bridge_device_host.h"
#include "plugin/fx_chain.h"
#include "plugin/ui/filter_card.h"
#include "plugin/ui/drop_gap.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/lane_selection.h"
// THE CHIP AT THE FOOT OF A CARD WHOSE DEVICE PUBLISHES — this lane owns them now; see `outputArms_`.
#include "plugin/ui/source_arm.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/rail.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// THE POST-FX LANE — the mock's `#fxStrip`: right of the panel, collapsible.
//
// A SHELL AND NOTHING ELSE, deliberately. There are no effect devices yet and this file does not
// pretend otherwise: it draws the lane, its head and the mock's own empty-chain placeholder, and it
// touches no parameter, no id and no engine. The devices are a later arc — the lean is that all
// FX are post-FX, and the engine work is front-loaded before them — so what this is for now is the
// place they will dock and the column the stage has already yielded for them.
//
// THE MOCK SPECIFIES ALL OF IT, so nothing here was invented: `#fxStrip{width:256px; padding:12px
// 10px; gap:10px; border-left:1px solid var(--line); overflow-y:auto}`, a head reading `Post-FX`
// with its button at the END of the row rather than the start, `#fxStrip.min{width:34px}` with the
// label turned upright, and a `.slot-empty` reading *"empty chain / effects dock here, top → bottom
// / · same cards as modulators · / · survives any center topology ·"*.
//
// THE THIRD RAIL, and the one that proves the base was a mechanism rather than two panels sharing
// code: it lives against the RIGHT edge, so its toggle sits on its inboard (left) side and its
// chevrons point the other way. Everything about that is Rail's, not this class's — see Side.
//
// WHAT IT IS HONEST ABOUT. An empty chain is drawn as an empty chain, in the voice the CV card
// already uses for a thing that is present but not yet real ("2 lanes · waiting on a cable"). The
// house rule that a control is enabled iff it changes sound through a real param is not strained
// here, because there are no controls: a placeholder that answers nothing looks like a placeholder.

namespace feathers::ui {

class FxLane : public Rail {
public:
	/// FREE THE CARDS A REBUILD RETIRED — call this only from a point that is provably NOT inside an
	/// event dispatch or a draw.
	///
	/// `ensureCard` used to free them itself, under the comment "when nothing is walking the tree".
	/// That is true of the state-channel notify it was written for and false of a drag: a card's own
	/// `onMouseUp` reaches `moveDevice`, a reorder touches at least two slots, and the second slot's
	/// free destroyed the card the first had retired — the one whose handler was still on the stack.
	/// It crashed on every reorder. See the note in `ensureCard`.
	///
	/// SO THE CALLER DECIDES, because only the caller knows. `Gui::Impl::tick()` runs before the pump
	/// drains any event, which is the one place in the editor that can promise it.
	/// **AND THE ARMS WITH THEM** — `retiredArms_` is the same deferral for the same reason; see it.
	///
	/// **THE NAME IS NOW NARROWER THAN WHAT IT DOES, AND IT IS KEPT ON PURPOSE.** It frees retired
	/// CARDS and retired ARMS; "retired frames" would be the accurate name. Renaming it means touching
	/// `Gui::Impl::tick()` and `ModPanel`'s twin, and the one caller that matters is the one place in
	/// the editor that can promise no dispatch is in flight — a rename that reached into the tick for
	/// tidiness would be moving the riskiest line in this arrangement to say something the next line
	/// already says. Stated instead.
	void releaseRetiredCards() {
		retiredCards_.clear();
		retiredArms_.clear();
	}

	/// HOW MANY CARDS ARE WAITING TO BE FREED. For cases: the claim "a reorder does not destroy the
	/// card that is dispatching" is a claim about a DEFERRAL, and a case that only checks the editor
	/// survived would pass on a build that freed it and got lucky — a dangling read is undefined, not
	/// reliably fatal, which is how the fx lane's previous use-after-free survived three notifications
	/// before it killed anything.
	[[nodiscard]] size_t retiredCountForTest() const { return retiredCards_.size(); }

	/// THE LANE NEEDS THE BRIDGE BECAUSE IT DRAWS THE CHAIN. It used to be handed one card and hold a
	/// pointer to it, which was true while the chain could hold exactly one thing; the chain has been
	/// able to hold eight since the fx channel landed, and a lane that could draw one of them was the
	/// visible half of a half-unpinned thing — the same sentence rack_card.h opens with.
	///
	/// `watchers` may be null and is not null in the editor. A card built here subscribes to the state
	/// road AS IT IS BUILT, because a swallowed device's settings are not parameters and the state
	/// road is the only thing that will repaint it. Registering from the outside means remembering to
	/// do it again every time a slot changes device, and a card that quietly stops repainting is a
	/// failure no headless suite can see.
	FxLane(ParamBridge& bridge, WidgetRegistry& registry, ModState* mod = nullptr,
	       EditHistory* history = nullptr, Notifier* notifier = nullptr,
	       FrameWatchers* watchers = nullptr);

	void draw(visage::Canvas& canvas) override;

	/// The mock's `#fxStrip{width:256px}`.
	float expandedWidth() const override { return kWidth; }
	static constexpr float kWidth = 256.0f;

	/// The mock's `padding:12px 10px` and its `gap:10px`.
	static constexpr float kPadX = 10.0f;
	static constexpr float kHeadingHeight = 16.0f;
	static constexpr float kSlotGap = 10.0f;
	/// The `.slot-empty` box: `min-height:56px`, and taller here because our type sets the four lines
	/// the mock writes into it at a readable size rather than at its 10px.
	static constexpr float kEmptySlotHeight = 92.0f;

	/// Where the empty-chain placeholder sits, so a case can ask rather than restate it.
	visage::Bounds emptySlotBounds() const;

	/// Where the lane's own name is written. Public for one reason: on a rail against the RIGHT edge
	/// the toggle is at the same end as the heading, so "these two do not overlap" is a real question
	/// with a wrong answer available — see the case, and railHeadingLeft in Rail.
	visage::Bounds headingBounds() const;

	/// Whether a press was the lane's own verb — the toggle — rather than something in the chain.
	/// A device in the chain is a child frame and answers for itself; this is the lane's own verb only.
	bool pressCollapse(visage::Point pixels);

	//---------------------//
	// Growing the chain    //
	//---------------------//

	/// The lane's own parts, so a case can drive the real button at the real place rather than at a
	/// rectangle it restated. A card in the chain is a child frame and answers for itself; these two
	/// are the LANE's verbs.
	enum class Part { None, Collapse, Add };
	Part partAt(visage::Point pixels) const;

	/// WHERE THE `(+)` IS: under the last card, which is where the next device will actually go.
	///
	/// THE CHAIN'S ORDER IS ITS SOUND, so a button at the END is a picture of an APPEND — which is the
	/// only mutation `addFxDevice` performs and the only one that cannot move a live routing column.
	/// A `(+)` drawn BETWEEN two cards would be offering an insertion, and there is no insertion
	/// behind it: reorder and remove exist as separate operations with their own remap. The button
	/// must not promise a road that is not there.
	visage::Bounds addBounds() const;
	static constexpr float kAddHeight = 22.0f;

	/// THE DEVICES THIS LANE MAY BE ASKED TO ADD — every registered device that can render audio, and every
	/// device the strip offers (every mod device is in scope in the Feathers fx rack).
	///
	/// DERIVED, NEVER LISTED. `maySitInAudioChain` reads the descriptor's own `audio` pointer and the strip's
	/// pair (`maySitInModRack` and the `modulator` tag) reads its outputs and its tags, so a device earns a
	/// seat here by declaring them rather than by anyone remembering to add it to a menu. A menu written by
	/// hand is the shape that let FilterPoly ship with no control at all.
	std::vector<const device::Descriptor*> addableForTest() const;

	/// Open the picker, as a press on the `(+)` does. Public so a case can drive the gesture without a
	/// window — `visage::PopupMenu::show` needs one, and what a case is about is which devices are
	/// offered and what choosing one does.
	///
	/// WITH `before` NAMING A SLOT, it is the "Add before" road (backlog CR): the same picker, opened
	/// at that card's dots, and the pick inserts before THAT CARD — its lane is captured when the
	/// picker opens, so a card that moves between the open and the pick is still the one meant. With
	/// `after`, the "Add after" road: the same picker at the same card, and the pick goes BEHIND it.
	void showAddMenu(size_t before = kNoPosition, bool after = false);

	/// THE MENU `showAddMenu` WOULD SHOW, built and not shown — its category submenus, their rows and
	/// enables, and its `onSelection`, so a case can read the shape and pick a row without a window.
	visage::PopupMenu addMenuForTest(size_t before = kNoPosition, bool after = false);

	/// Choose the nth offered device, as clicking that row would. Returns the slot it landed in, or
	/// kNoPosition if the lane refused it.
	size_t addChoiceForTest(size_t index);

	/// "Add before" chosen for `slot`, then the nth offered device picked — the whole gesture without
	/// a window. Returns the position the new device landed at, or kNoPosition if refused.
	size_t insertChoiceForTest(size_t slot, size_t index);

	/// Insert a new device of `tag` before the device `anchor`; one undo step. Public
	/// because it is the lane's verb and a case drives it; the picker is the only caller in the editor.
	size_t insertDeviceBefore(DeviceInstanceId anchor, const char* tag);
	/// "ADD AFTER" IS "ADD BEFORE THE NEXT ONE" — or an append behind the last — through the two roads
	/// above, so a full chain is refused in the same words. `anchor` is found at the pick.
	size_t insertDeviceAfter(DeviceInstanceId anchor, const char* tag);

	//------------------------------//
	// Shrinking and reordering it   //
	//------------------------------//

	/// Open the slot menu for `slot`, as pressing the three dots in that card's head does.
	void showSlotMenu(size_t slot);

	/// WHAT THAT MENU OFFERS, in order, for a given slot — so a case reads the card's own answer
	/// rather than restating the rules about which ends can move.
	enum class SlotVerb { AddBefore, AddAfter, MoveUp, MoveDown, Remove };
	struct SlotOption {
		SlotVerb verb;
		bool enabled;
	};
	std::vector<SlotOption> slotOptionsForTest(size_t slot) const;

	/// Choose one, as clicking that row would. False if the verb is not available for that slot.
	bool slotChoiceForTest(size_t slot, SlotVerb verb);

	/// Remove the device at `slot`, and move one. Public because they are the lane's verbs and a case
	/// drives them; the menu above is the only thing in the editor that calls them.
	bool removeDeviceAtLane(size_t slot);

	/// **WHICH CARD A KEY ACTS ON** — Delete or Backspace removes the focused effect. The rack got this
	/// first; the need is about effects generally, and a gesture that works in one host and not the other is worse than one
	/// that works in neither.
	static constexpr size_t kNoFocus = static_cast<size_t>(-1);
	void focusSlot(size_t slot);
	/// **FOCUS THE CARD OF `device`**, wherever it now stands — for an armed source in the lane (a modulator, since
	/// 2026-10-04; `ModPanel::onArmedElsewhere`). Returns whether the lane holds it.
	bool focusDevice(DeviceInstanceId device);
	size_t focusedSlotForTest() const noexcept { return focusedSlot_; }
	bool moveDevice(size_t from, size_t to);

	/// **THE SELECTION OVER THE LANE** (backlog OA): its cards, its verbs (Group into Chain/Rack, copy, paste,
	/// duplicate, remove), its lasso and its Esc entry — the same `LaneSelection` the chain editor owns.
	[[nodiscard]] LaneSelection& selectionForTest() noexcept { return *selection_; }
	/// The editor's Esc stack (backlog NN): a selection of two or more is one entry on it.
	void setEscapeStack(EscapeStack* stack) { selection_->setEscapeStack(stack); }
	/// ONE OF THE DOTS MENU'S SELECTION VERBS, picked on `slot`'s card — the menu's road, without a menu.
	bool slotVerbForTest(size_t slot, FragmentVerb verb);
	/// Where those verbs' option ids start in the dots menu, past the lane's own `SlotVerb`s.
	static constexpr int kVerbIds = 100;

	//------------------------//
	// Dragging a card         //
	//------------------------//

	/// WHICH CARD THE HAND IS HOLDING, and WHICH GAP it would drop into. Both empty unless a drag is
	/// in flight. A gap and not a slot, for the reason drop_gap.h states and the
	/// sound list showed: a highlighted CARD says "it would replace that one", and what a reorder does is put
	/// the card in a GAP — which is also the only way to say "before the first" or "after the last".
	/// THE DRAGGED CARD'S SLOT NOW: found again from its device id, so a reorder or removal under the hand moves it
	/// with the device, and a device that has gone has none.
	std::optional<size_t> dragSlot() const;
	std::optional<size_t> dropGap() const noexcept { return dragging_ ? dropGap_ : std::nullopt; }

	/// Which gap a point in the LANE's coordinates is asking for, or nullopt outside the lane.
	std::optional<size_t> dropGapAt(visage::Point position) const;

	/// Where the bumper is drawn for a gap — a band in the space between two cards, above the first,
	/// or below the last. Public so a case can ask the geometry rather than photograph it.
	visage::Bounds gapBounds(size_t gap) const;

	/// Drive the whole gesture as a card would, for the cases: the card reports in ITS coordinates and
	/// the lane converts, so a case that made up lane coordinates would be testing its own arithmetic.
	void cardDragForTest(size_t slot, FxCard::DragPhase phase, visage::Point inCard);

	/// **THE LANE SCROLLS UNDER A HELD DRAG** (backlog DB: the column did not scroll while a dragged
	/// effect was held over the bottom of the visible scroll region). While a card is
	/// held and the hand sits inside the lane within `kEdgeScrollBand` of its top or bottom, each editor
	/// tick scrolls the lane `kEdgeScrollStep` that way and the drop gap follows the hand. A steady rate
	/// rather than a jump, because a hand parked at the edge is asking for "further", not "the end".
	/// Driven from the editor's per-frame tick (`Gui::Impl::tickOnce`) — the one clock a held pointer
	/// has, since visage delivers no drag event while the pointer is still. Outside the lane is the
	/// cancel, not a scroll. Returns whether it scrolled, so the road that delivers a repaint reports it.
	bool tickDrag();
	static constexpr float kEdgeScrollBand = 28.0f;
	static constexpr float kEdgeScrollStep = 6.0f;

	/// THE ROW A POPUP OPENED FROM `slot` HANGS OFF — the card's head row, or the (+) for a slot past the
	/// end. `showMenuAt` flips the menu ABOVE it when it would not fit under it (both of this lane's menus
	/// hang off rows that can sit at the bottom of the window). Public so a case can check a menu against it.
	visage::Bounds menuAnchorFor(size_t slot) const;

	//--------//
	// Scroll //
	//--------//

	/// IT SCROLLS, and it had to the moment a SECOND device drew a real card.
	///
	/// MEASURED, NOT ANTICIPATED. At the canvas the layout is written for the lane is 534px tall. The
	/// filter's card is 356 and HARM's is 330, so two cards plus the heading and the gaps are ~706 —
	/// and HARM's card was placed with its bottom 86px past the window, which put its last knob row
	/// and its Poly switch where no hand could reach them. The NoEditorCard that stood there before
	/// only fit because a placeholder is short, so the lane looked fine right up until a device
	/// brought an editor.
	///
	/// THE ANSWER TO CONTENT TALLER THAN ITS FRAME IS NEVER A TALLER WINDOW — the logical canvas has a
	/// floor and everything is written to fit inside it. The modulator strip beside this one reached
	/// the same wall with five cards and answered it this way (mod_panel.h says so in as many words),
	/// and the sound rail before it. This is that answer a third time, deliberately identical.
	///
	/// All four are readable because the contract is a statement about them: never scrolled past the
	/// end, never scrolled off the top, and every card reachable from some scroll position.
	float contentHeight() const;
	float scroll() const noexcept { return scroll_; }
	float maxScroll() const;

	/// SCROLL UNTIL A CARD IS WHOLLY VISIBLE, if it can be.
	///
	/// THE LANE CANNOT SHOW TWO OPEN CARDS AND THAT IS ARITHMETIC, not a bug: it is 534px tall and the
	/// filter card alone is 356, so ANY second card needs scrolling to be used at all. That was
	/// survivable while a card was a list of knobs -- a knob you cannot see is a knob you have not
	/// scrolled to. It stopped being survivable when the card's principal control became a PICTURE,
	/// because a bar sits where its VALUE puts it: the part of the plot below the lane is a RANGE OF
	/// VALUES whose control cannot be touched, and the default threshold lands in it.
	///
	/// Seen twice: the bars still did not drag. MEASURED: plot 488..648, lane ends at 586, default lower
	/// bar at ~588 — two pixels the wrong side of a line nothing draws.
	///
	/// SO ADDING A DEVICE SCROLLS TO IT, which is also just what a hand expects: you added the thing,
	/// you are shown the thing. It does not make the lane bigger and it does not pretend two cards fit
	/// — it makes the card you are working on the one that is whole.
	void scrollCardIntoView(size_t slot);

	/// ADD A DEVICE THE WAY THE (+) DOES — public because a case that reaches past it to the bridge
	/// is testing a different act. The lane does things the bridge cannot know about: it builds the
	/// card, lays the lane out, records the undo step and scrolls the new card into view. A fixture
	/// that wrote the chain directly and then called `resized()` looked equivalent and skipped three
	/// of those, which is how the state a hand actually meets went untested.
	size_t addDevice(const char* tag);
	void setScroll(float y);

	bool mouseWheel(const visage::MouseEvent& e) override;

	/// One notch of the wheel. A FIXED DISTANCE RATHER THAN A CARD, for the reason the modulator strip
	/// records having learned: a device card is most of this lane's height, so a notch of one card
	/// would be the whole view and the wheel would teleport instead of scroll.
	static constexpr float kScrollNotch = 48.0f;

	//------------------//
	// The chain's cards //
	//------------------//

	/// How many cards the lane is drawing, which is the CHAIN's count and not the lane's idea of it.
	size_t deviceCount() const;

	/// The card in a slot, or null for a slot past the end. Typed as the interface, because what the
	/// lane holds is "a device's card" and the day it holds two kinds is the day this file stops
	/// having to change.
	FxCard* cardAt(size_t slot) const;

	/// **WHERE ONE SLOT'S n-th OUTPUT ARM IS, IN THE LANE'S COORDINATES** — empty when there is no such
	/// arm, which is the answer a case about "every output has one" is asking for. The rack editor's
	/// `outputArmBoundsForTest` is the same accessor for the same claim, keyed by device because that
	/// lane is keyed by device.
	visage::Bounds outputArmBoundsForTest(size_t slot, size_t output) const noexcept {
		for (const OutputArm& entry : outputArms_) {
			if (entry.slot == slot && entry.output == output && entry.arm != nullptr) {
				return entry.arm->bounds();
			}
		}
		return {};
	}
	/// THE ARM ITSELF, or null — for a claim about the WORD it prints, which a rectangle cannot carry.
	[[nodiscard]] const SourceArm* outputArmForTest(size_t slot, size_t output) const noexcept {
		for (const OutputArm& entry : outputArms_) {
			if (entry.slot == slot && entry.output == output) {
				return entry.arm.get();
			}
		}
		return nullptr;
	}
	/// HOW MANY ARMS THE LANE HOLDS — for the pruning claim, which is about arms that should have GONE
	/// and so cannot be read from any one arm.
	[[nodiscard]] size_t outputArmCountForTest() const noexcept { return outputArms_.size(); }

	/// TICK EVERY VISIBLE CARD, AND REPORT HOW MANY SAID THE SCREEN IS OUT OF DATE.
	///
	/// The count exists for the reason `FrameWatchers::notify` and `WidgetRegistry::notify` return
	/// one: a repaint cannot be observed headlessly, so the only honest evidence that the signal was
	/// delivered is a number produced BY the delivery. A counter standing beside the call could not
	/// promise it.
	///
	/// VISIBLE ONLY, which is free and already load-bearing: `Frame::redraw()` refuses an invisible
	/// frame anyway, and a card that is collapsed, on the other surface, or scrolled out of the lane
	/// is not visible. So a card nobody can see costs one comparison of its own meters and nothing
	/// else — and a card scrolled back in is told again on the very next tick.
	size_t updateCards();

	/// **REBUILD AND RE-LAY THE LANE IF THE CHAIN NO LONGER MATCHES ITS CARDS — and report how many slots
	/// did not.** After a preset load, effects did not appear until the fx pane was closed and
	/// reopened. A load replaces the chain under the lane (`ParamBridge::restoreChains`)
	/// and tells the editor only through the state channel, which REDRAWS watchers; the cards are built by
	/// `resized()`, which nothing on that road called — so the lane drew the old chain until the collapse
	/// toggle laid it out. Called from `Gui::Impl::tick()` on that channel, batched, never from inside the
	/// restore; the compare reads the chain once, so a tick with nothing new costs one copy and no layout.
	size_t syncCards();

	/// THE FIRST FILTER IN THE CHAIN, or null. Kept because three cases and the editor ask for "the
	/// filter card" by name and mean it — the lane has held exactly one since it existed. It is a
	/// SEARCH rather than a stored pointer now, so it keeps answering when the filter is not first.
	FilterCard* filterCard() const noexcept;

	/// Where a card sits: the lane's width less its padding, stacked in chain order, each as tall as
	/// it says it is. WIDTH IS DERIVED FROM THIS LANE — kWidth less kPadX on both sides — rather than
	/// copied from the modulator strip's 216, which is the same arithmetic on a different rail and
	/// would be right by coincidence until one of the two lanes changed.
	///
	/// WALKED RATHER THAN INDEXED, because a card above this one may be folded and a card's place is
	/// the sum of what is actually above it. The modulator strip's envCardBounds says the same.
	visage::Bounds cardBounds(size_t slot) const;

	/// The first slot's rectangle, for the cases and the editor that predate the chain being drawn.
	visage::Bounds cardBounds() const { return cardBounds(0); }

	/// A DEVICE'S CARD ASKED TO BE EXPANDED — the slot, and nothing else, because the lane does not
	/// know what an expanded view is either. Installed by the editor, which owns the layer.
	///
	/// EMPTY IS A LANE OUTSIDE AN EDITOR, and a card whose press reaches nothing. That is the right
	/// behaviour for a lane in a test harness and the reason this is a callback rather than a pointer
	/// to the overlay: the lane never has to know whether there is one.
	std::function<void(size_t)> onExpandDevice;
	/// THE CARD'S SITE CHIP (backlog NR): the device at this slot gets its PRE/POST chains and their panel opens.
	/// **A SETTER, NOT A MEMBER, BECAUSE THE LANE BUILDS ITS CARDS IN ITS CONSTRUCTOR** and the editor installs this
	/// afterwards. A card is told `onSite` when it is built, and a card that was never told draws no chip
	/// (`FxCard::offersSite`), so as a plain member every card already in the lane — the default patch's whole
	/// chain — stayed chipless until something rebuilt it (backlog NZ: auditioning). Setting it binds every
	/// card built so far too.
	void setOnExpandSite(std::function<void(size_t)> verb);

	/// A PRESS ON A CARD'S HEAD — the device (and the chain it is in) whose window, if one is open, the
	/// editor raises to the front (clicking an effect's header focuses its window if that window
	/// is open). Returns whether a window was there. Empty, like `onExpandDevice`,
	/// for a lane outside an editor; with nothing open the head keeps its own behaviour.
	std::function<bool(DeviceInstanceId, DeviceNest)> onFocusDeviceWindow;

	/// Hand every card the modulation panel's state, and keep handing it to cards built later.
	void setModulation(ModState* mod, EditHistory* history);

	void resized() override;

	/// SET THE METER CALLBACK on a OWLTT DSP — the DSP publishes per-band GR and level to the
	/// ParamBridge via this callback. Called by the host after the DSP is constructed. The bridge owns
	/// the atomics and outlives every editor open/close cycle, so there is no use-after-free when the
	/// window closes with sound playing.
	void setOwlttMeterCallback(size_t slot, void* dspStorage);

	/// SET THE METER CONNECTOR — the editor calls this after the lane is created, providing a function
	/// that connects a OWLTT DSP to the meter system. The host calls this function after constructing
	/// a OWLTT DSP. This removes the wiring step that "the host must perform" — the editor owns both
	/// sides and connects them.
	void setOwlttMeterConnector(std::function<void(size_t slot, void* dspStorage)> connector);

	/// CONNECT A OWLTT DSP to the meter system — called by the host after constructing a OWLTT DSP.
	/// No-op if no connector has been set.
	void connectOwlttDsp(size_t slot, void* dspStorage);

private:
	/// WHICH CARD A KEY ACTS ON — see `focusSlot`. `kNoFocus` until a hand presses one.
	size_t focusedSlot_ = kNoFocus;
	/// BACKLOG OA — see `selectionForTest`; the frame that draws its lasso and pill above the cards; and the history
	/// its verbs record into when the lane was built without one (a case).
	EditHistory spareHistory_;
	std::unique_ptr<LaneSelection> selection_;
	std::unique_ptr<SelectionOverlay> selectionOverlay_;
	[[nodiscard]] std::vector<DeviceInstanceId> laneOrder() const;
	/// Every card flagged from the selection; the key's target from its anchor.
	void applySelection();
	/// After a verb: the cards follow the chain, the selection is pruned.
	void afterVerb();

	/// Append one device by tag, announce it, and LAY THE LANE OUT. The one place the (+) and the menu
	/// meet the bridge, so there is a single road to test and a single place a refusal is worded.

	/// Build or rebuild the card for a slot, if what the slot holds is not what the card draws.
	void ensureCard(size_t slot);

	/// METER CONNECTION — set by the editor after the lane is created. The editor calls this with a
	/// function that connects a OWLTT DSP to the meter system. The host calls this function after
	/// constructing a OWLTT DSP.
	std::function<void(size_t slot, void* dspStorage)> owlttMeterConnector_;

	ParamBridge& bridge_;
	/// FEATHERS AS A `backplane::DeviceHost`, for the cards that address a device through one.
	/// Owned here because the lane is the one place in this plugin that hosts a device card, and a
	/// second adapter elsewhere would be a second thing to keep in step.
	BridgeDeviceHost deviceHost_ { bridge_ };
	WidgetRegistry& registry_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	Notifier* notifier_ = nullptr;
	FrameWatchers* watchers_ = nullptr;
	/// The editor's site verb (`setOnExpandSite`); empty in a lane nobody wired.
	std::function<void(size_t)> onExpandSite_;
	/// Tell the card at `slot` the site verb, or nothing when the lane has none.
	void bindSite(size_t slot);

	/// BY POSITION, AS LONG AS THE LANE — grown by `reserveCards` (unbounded devices step 6); a card never moves
	/// when they grow, only the pointer to it does.
	std::vector<std::unique_ptr<FxCard>> cards_ = std::vector<std::unique_ptr<FxCard>>(kMaxFxSlots);
	/// Grow every per-position array to at least `count`. UI thread.
	void reserveCards(size_t count);

	/// **ONE TOKEN PER SLOT, BESIDE THE CARD IT WATCHES — not a flat list.**
	///
	/// It was a `std::vector<Watch>` that only ever grew. `ensureCard` destroys and rebuilds a card in
	/// place, so a rebuilt slot left its OLD card's token held: the dead card stayed on the state
	/// watchers' list and `notify()` would `redrawTree()` it. **That is a use-after-free and it
	/// predates the token** — before, nothing unwatched a card at all — but a flat vector reproduced it
	/// faithfully instead of fixing it.
	///
	/// Indexed by slot, assignment does the whole job: handing a slot a new token releases the old
	/// one, which is exactly the moment the old card stopped existing.
	std::vector<Watch> cardWatches_ = std::vector<Watch>(kMaxFxSlots);

	/// CARDS THAT HAVE BEEN REPLACED BUT NOT YET FREED. See `ensureCard` for why they are not
	/// destroyed where they are removed; freed on the next `ensureCard`, when no draw is in flight.
	std::vector<std::unique_ptr<FxCard>> retiredCards_;

	/// **ONE ARM PER OUTPUT OF AN FX DEVICE THAT PUBLISHES ONE** — and until SKLITTER there were none,
	/// which is why this lane drew no chips at all while both of the other two hosts did. Since
	/// 2026-09-17 all of a device's outputs are on its card.
	///
	/// **THE SHAPE IS `ModPanel::outputArms_`'s**, for the same reasons it gives: siblings of the card
	/// rather than children of it, so an arm survives the card being rebuilt under it and is above the
	/// card by construction rather than by draw order; keyed by {slot, output, label}, with the SOURCE
	/// asked for at use rather than remembered, because which device stands at a slot changes whenever
	/// the chain is shortened or reordered.
	struct OutputArm {
		std::unique_ptr<SourceArm> arm;
		size_t slot = 0;
		size_t output = 0;
		/// PART OF THE IDENTITY: a slot whose device changed keeps its number, and an arm kept across
		/// that swap would name the old device's output beside the new one's arrow.
		std::string label;
	};
	std::vector<OutputArm> outputArms_;

	/// ARMS THAT HAVE BEEN REPLACED BUT NOT YET FREED — **`retiredCards_`'s rule applied to the arms**,
	/// and it is the same hazard rather than a cautious copy: `layOutOutputArms` prunes, it runs from
	/// `resized()`, and `resized()` is reached from a card's own `onMouseUp` through `moveDevice`. An
	/// arm destroyed there is a frame freed inside the dispatch that is still walking the tree.
	/// `ModPanel` erases its arms in place; this lane already owns the deferral, so it uses it.
	std::vector<std::unique_ptr<SourceArm>> retiredArms_;

	/// THE ENDPOINT A SLOT'S n-th OUTPUT PUBLISHES ON, read at the moment it is asked — the device
	/// standing at the slot now, not the one that was there when the arm was built.
	backplane::Source sourceOfOutput(size_t slot, size_t output) const;
	/// The arm for one output, made if it does not exist yet. Null when the slot publishes no such
	/// output.
	SourceArm* ensureOutputArm(size_t slot, size_t output);
	/// Drop arms whose output is gone or whose device changed under them, and place the rest.
	void layOutOutputArms();
	/// WHAT THE DEVICE IN A SLOT IS, or null for a slot past the chain or a tag this build cannot
	/// name — asked of the chain rather than remembered, like everything else about a slot here.
	///
	/// **TWO FORMS BECAUSE THE CHAIN IS A ~4 KB COPY.** A caller in a loop reads the chain once and
	/// takes `descriptorIn`; `descriptorAt` is the convenience for a caller that needs one answer.
	/// Backlog T is this exact shape found in this exact file.
	const device::Descriptor* descriptorIn(const FxChain& chain, size_t slot) const;
	const device::Descriptor* descriptorAt(size_t slot) const;

	/// WHAT EACH CARD WAS BUILT FOR. Compared against the row's tag on every ensureCard, because a
	/// patch load can put a different device in a slot without the lane being told.
	std::vector<device::Tag> cardTag_ = std::vector<device::Tag>(kMaxFxSlots);

	/// **AND WHICH DEVICE THE CARD WAS BUILT FOR — the other half of the address, which the tag cannot
	/// stand in for.** `ModPanel::cardDevice_` carries the long version of this note; the defect is the
	/// same one in both files and was found in this one by asking whether it was.
	///
	/// MEASURED when a card held a lane: three FILTERs in the chain; remove position 1, and the card
	/// still at position 1 went on addressing the device that had been there. `cards_` is keyed by
	/// POSITION and a card holds its DEVICE (an id since kill-lanes step 2, a lane before), and a tag key
	/// cannot tell two devices of the same kind apart — which is exactly the arrangement in which the
	/// position and the device part company. A DIFFERENT tag masks it, which is why this survived: the
	/// case that puts two filters in one chain never removes one.
	std::vector<DeviceInstanceId> cardDevice_ = std::vector<DeviceInstanceId>(kMaxFxSlots);

	float scroll_ = 0.0f;

	/// THE DRAG, in three pieces because a press is not yet a drag — the sound rail's shape, and its
	/// reasons. `dragSlot_` is the card the gesture began on, `dragging_` is whether the threshold has
	/// been crossed, and `dropGap_` is where it would land. The last is reset when the pointer leaves
	/// the lane, which is the cancel.
	/// WHICH DEVICE THE GESTURE BEGAN ON, by instance id: a slot is a position and the chain can be reordered or shortened
	/// under a live press, so the slot is resolved from this when the drag ends.
	std::optional<DeviceInstanceId> dragDevice_;
	std::optional<size_t> dropGap_;
	bool dragging_ = false;
	/// WHERE THE HAND LAST WAS, in the lane's coordinates — what `tickDrag` scrolls against while the
	/// pointer is still and no drag event arrives.
	visage::Point dragPoint_ { 0.0f, 0.0f };

	/// **THE EAR ON OUTPUT RELABELS** — an arm prints its output's label and is as wide as it, so a
	/// relabel lays the lane out again (`layOutOutputArms` rebuilds that arm). Declared last so it ends
	/// first: it captures `this`, and the bridge outlives the lane.
	device::Subscription labelSubscription_;
};

// (`NoEditorCard` STOOD HERE — "Unknown device — no editor yet". Its last consumer was a device this build
// does not have, and that is `MissingDeviceCard` now, in `generic_card.h`, one card for every host.)

} // namespace feathers::ui
