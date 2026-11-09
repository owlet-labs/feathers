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

#include "plugin/ui/edit_history.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/lane_selection.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/widget_registry.h"
#include "rack/rack_bridge.h"
// FOR `rack::kMaxUsefulDepth` AND `rack::kNestingFullMessage` — the nesting bound and the sentence it
// is explained by, which live beside each other in the rack format's own header.
#include "rack/rack_device.h"

#include <functional>
#include <span>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// **A CHAIN A DEVICE HOLDS, DRAWN AS A NARROW VERTICAL COLUMN** — the surface backlog **EW**'s master
// lane needed and did not have. Bolt-on-chains gap **G5**, the half that document called a
// registration problem and which is really a SIZE problem.
//
// **WHY A NEW WIDGET AND NOT `RackEditor`.** `RackEditor` is the chain editor that exists, and its
// CARD half is exactly right for this job — keyed by `DeviceInstanceId`, built over a
// `backplane::DeviceHost` through `ui::makeCustomCard`, with folds, a slot menu and a drag. What does
// not fit is everything around it: it is `kWidth = 720` by `kHeight = 560`, its lane runs
// HORIZONTALLY, and it carries a macro row, a route list and the canvas band. A chain inside a
// device's panel has room for none of that, and — for SKLITTER, the first caller — WANTS none of it:
// that chain's eight "macros" are the seven perform outputs the device publishes inward
// (`kPostChainAxes.coreSources`), which no hand may turn, so a macro row here would be eight knobs
// that are not controls. A 720px editor in a 300px rail is not a near miss.
//
// **AND WHY IT IS NOT A REFACTOR OF EITHER LANE, WHICH IS THE SAME DECISION `rack::InnerChain` MADE ONE
// LAYER DOWN AND FOR THE SAME REASON.** `inner_chain.h`: *"`rack_device.cpp` IS DELIBERATELY NOT
// PORTED ONTO THIS YET. Doing so would change live drain semantics in the shipped rack before this
// unit has a second caller in anyone's hands."* The same sentence with the nouns changed: extracting
// this column out of `RackEditor::syncCards` + `resized()` would rewrite the layout of the shipped
// rack editor before this column has a second caller. The two are expected to converge; until they
// do, a change to how a chain's cards are built belongs in BOTH.
//
// **WHAT IT IS BOUND TO, AND WHY THAT IS `RackBridge` RATHER THAN A DEVICE HOST.** The cards need only
// a `backplane::DeviceHost`, which is why one factory serves both hosts. The CHAIN verbs — what is in
// it, in what order, add, remove, move, folded — are not on that interface and cannot be: a host
// knows how to address a device, and only a chain knows what a chain holds. `RackBridge` is that, and
// `rack::InnerChain::bridge()` is where a holding device keeps one.
//
// **THE HONESTY RULE, AND THE ONE PLACE IT BITES HERE.** A chain with no running instance behind it
// has no column at all — the owner draws its placeholder instead — because a (+) over a null bridge
// is a button that cannot add. A chain at capacity draws its (+) DIMMED rather than hiding it, which
// is `FxLane`'s own rule: *"a control that vanished at the bound would leave a player wondering
// where it went."*
//
// **WHAT A PLAYER CANNOT DO HERE YET, stated because a surface that quietly lacks a verb reads as one
// that has it.** There is no arm for the HOLDER's own published sources inside the chain (SKLITTER's
// seven perform outputs are the chain's core sources and are routable by the engine, with no widget
// to arm them — the same shape as the rack's own unreachable mod outputs), and no route
// list. Neither is drawn, so neither is promised.
//
// **THE CHAIN'S OWN DEVICES DO HAVE ARMS, AND THIS FILE ONCE SAID OTHERWISE BY SAYING NOTHING.** That
// paragraph named the holder's sources and stopped, which read as though the devices IN the chain were
// covered. They were not: this column was extracted out of `RackEditor`'s card half and the
// output-arm machinery did not come with it, so a modulator dropped into an inner chain drew no chip,
// and with no chip there was no arm, and with no arm its output could not be routed from the surface
// it was sitting on (the defect: an LFO put in the grain fx had no modulation output on its card).
// `rebuildOutputArms` and `layOutOutputArms` are that road; the placement half
// is `ui::layOutCardOutputArms`, shared with the fx lane and the rack editor.
//
// **AN ARM HERE ROUTES INSIDE THIS CHAIN'S AUTHORITY BY CONSTRUCTION, NOT BY A CHECK — and this is
// the paragraph to read before "simplifying" the per-lane `ModState` into a shared one.** Two facts
// close it with nothing to enforce:
//
//   - the arm arms against `mod_`, and `SklitterExpanded` gives each lane its OWN `ModState`
//     (`masterMod_`, `grainMod_`) precisely so a master-lane control is never lit for a grain-lane
//     modulator the engine will refuse to route — its own header calls that *"the honesty rule failing
//     in the one place it is expensive to notice"*;
//   - the route a knob then writes goes through `ParamControl`'s host, which for a card in this column
//     is this column's own `bridge_` — the holder's `InnerChain`.
//
// So `backplane::Source::output(innerDeviceId, n)` is resolved by the same authority that will run it.
// **Nothing validates that and nothing needs to**, which is the guarantee CLAUDE.md prefers: a shared
// `ModState` across the two lanes would not fail a test here, it would light half the wrong controls
// and quietly offer routes the engine drops. Both halves were already in place when the arms were
// missing — which is exactly why the gap was invisible to reading.
//
// **THE EXPAND MARK IS HERE NOW, AND WHAT MADE IT HONEST WAS GIVING THE COLUMN SOMEWHERE TO PRESENT.**
// A SKLITTER inside a SKLITTER's chain had no expand button. This file used to answer that with a
// refusal — *"a card's expand needs a
// `SurfaceLayers` to present onto, and this column has none of its own"* — which was the right verdict
// on the wrong question. The mark was missing for a narrower reason than the comment gave: a card asks
// the HOST whether a panel exists (`FxCard::offersExpand` → `DeviceHost::hasExpandedViewFor`), and
// `RackEngine` declines until somebody installs `setExpandedViewQuery` — which only `RackEditor`'s
// constructor ever did. So the query is installed exactly when `setPresentedOn` gives this column a
// presenter, and removed when it does not: the mark appears iff a press would open something, which is
// the honesty rule as a mechanism rather than as a check.
//
// **AND THE PANEL ESCALATES TO THE OUTERMOST HOST, WHICH IS WHY THIS COLUMN OWNS A `SurfaceLayers`
// RATHER THAN PRESENTING ON THE ONE IT IS GIVEN.** By design, nesting escalates outward and
// `SurfaceLayers::setOuterLayers` is the road; `RackEditor` takes the same one.
// It is not a stylistic echo — **`SurfaceLayers::forwarded_` is what takes a forwarded panel off THIS
// column's modulation list before the outer frees it**, and that list (`mod_`) can die while the outer
// still holds the mount, because a panel the outer retires *"lives until the next `draw`"*. Presenting
// straight onto the presenter would leave a knob's `ModState*` one deferred destruction behind its
// state — the exact lifetime class the sanitizer gate exists for.

namespace feathers::ui {

class Notifier;

/// A vertical stack of device cards over one `rack::RackBridge`, with a (+) at its foot.
///
/// **IT IS NOT A `Rail`.** A rail is a column of the EDITOR; this is a frame a rail (or a panel, or a
/// card) puts inside itself, so it draws no chrome, no heading and no hairline and owns no collapse.
/// Its owner decides where it sits and how wide it is; it decides only how tall its content is.
class ChainColumn : public visage::Frame {
public:
	/// **BUILT OVER A LIVE BRIDGE AND REBUILT WHEN THAT BRIDGE CHANGES.** The bridge is a reference
	/// rather than a pointer because a column without one has nothing to draw: the owner destroys this
	/// frame when the handle goes, exactly as `ChainExpanded::follow` drops its editor.
	ChainColumn(rack::RackBridge& bridge, WidgetRegistry& registry, ModState& mod, EditHistory& history,
	            Notifier* notifier = nullptr);
	~ChainColumn() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	bool mouseWheel(const visage::MouseEvent& e) override;

	//---------------------//
	// What is in the chain //
	//---------------------//

	/// How many devices the chain holds — the BRIDGE's count, never this frame's idea of it.
	[[nodiscard]] size_t deviceCount() const;
	/// The device at a position in the chain, or `kNoInstance`.
	[[nodiscard]] DeviceInstanceId deviceAt(size_t position) const;
	/// The card for a device, or null. Keyed by the DEVICE for the reason `RackEditor` records: a card
	/// belongs to a device, so a reorder moves nothing and rebuilds nothing.
	[[nodiscard]] FxCard* cardOf(DeviceInstanceId device) const noexcept;
	/// Every card, in chain order — the order they are drawn in.
	[[nodiscard]] std::vector<FxCard*> cardsForTest() const;
	/// Every card this column holds, whether or not the chain still names its device. The count a case
	/// needs to prove a removed device's card went, which a walk of the chain cannot see.
	[[nodiscard]] size_t cardCountForTest() const noexcept;

	//---------------------//
	// The devices' outputs //
	//---------------------//

	/// **THE ARM FOR ONE DEVICE'S n-th OUTPUT, OR NULL.** `RackEditor`'s accessor of the same name, keyed
	/// the same way, because this column is keyed by device for the same reason that one is.
	[[nodiscard]] SourceArm* outputArmOf(DeviceInstanceId device, size_t output) const noexcept;
	/// **WHERE THAT ARM'S CHIP IS, IN THIS COLUMN'S COORDINATES** — empty when there is no such arm,
	/// which is the answer a case about "every output has one" is asking for.
	[[nodiscard]] visage::Bounds outputArmBoundsForTest(DeviceInstanceId device,
	                                                    size_t output) const noexcept;
	/// HOW MANY ARMS THIS COLUMN HOLDS — for the pruning claim, which is about arms that should have
	/// GONE and so cannot be read from any one arm.
	[[nodiscard]] size_t outputArmCountForTest() const noexcept { return outputArms_.size(); }

	/// **WHAT TO CALL THIS CHAIN'S OWN CORE SOURCES — the HOLDER's outputs, which only the holder's
	/// surface can name.** This column knows the devices IN the chain; it does not know the device the
	/// chain is in, and a menu row reading "Source 3" where SKLITTER means "Weight 2" is a row you have
	/// to pick to find out what it was. `FxLaneRail` installs one from `sklitter::kOutputs[n].name`.
	///
	/// UNSET IS AN ORDINAL, not a guess — the same refusal `ModState::sourceName` makes with no namer.
	void setCoreSourceNamer(std::function<std::string(size_t)> namer);

	/// REBUILD THE CARD STACK FROM THE BRIDGE. Compares before it builds, so a reorder builds nothing.
	void syncCards();
	/// TICK EVERY VISIBLE CARD, AND REPORT HOW MANY SAID THE SCREEN IS OUT OF DATE — the count is
	/// produced BY the delivery, per the repaint rule: a counter standing beside the call cannot
	/// promise the call happened.
	size_t updateCards();

	//---------------------//
	// Growing and shrinking //
	//---------------------//

	/// The column's own verbs, so a case drives the real place rather than a rectangle it restated. A
	/// card is a child frame and answers for itself.
	enum class Part : uint8_t { None, Add };
	[[nodiscard]] Part partAt(visage::Point pixels) const;
	/// Where the `(+)` is: under the last card, which is where the next device will actually go.
	[[nodiscard]] visage::Bounds addBounds() const;
	static constexpr float kAddHeight = 22.0f;

	/// THE DEVICES THIS CHAIN MAY BE ASKED TO ADD — **derived, never listed**.
	///
	/// Every device in the build that this chain's own catalogue can resolve. **Not filtered to audio**,
	/// and that is EW's whole "except": `rack_bridge.cpp` builds its converger with
	/// `Converger::fitsAnywhere`, so a rack's single chain already takes audio devices and modulators
	/// together and the engine sorts them by CAPABILITY rather than by chain. The post chain is
	/// specified as a duplicate of Feathers' own, except that modulation tools are allowed in it; the
	/// except is free, and filtering here would be the one line that threw it away.
	/// **UNFILTERED, AND THE NESTING BOUND DOES NOT CHANGE THAT** — a device this chain cannot take at
	/// this depth is still OFFERED and drawn disabled (`showAddMenu`), which is the rule `full()`
	/// already follows one axis over: *"a control that vanished at the bound would leave a player
	/// wondering where it went."*
	[[nodiscard]] std::vector<const device::Descriptor*> addable() const;

	/// Whether adding this device would put a CHAIN inside this one — i.e. whether the nesting bound
	/// has anything to say about it. A filter is not a container and is never refused for depth.
	[[nodiscard]] bool wouldNest(const device::Descriptor& desc) const noexcept;

	/// **WHETHER A DEVICE ADDED HERE COULD STILL HOLD A CHAIN OF ITS OWN** — false once this column is
	/// `rack::kMaxDepth - 1` chains deep, which is exactly where `InnerChain::loadState` refuses a body.
	/// Public because a case asserting the bound must read the column's own answer rather than restate
	/// the arithmetic it is checking.
	[[nodiscard]] bool nestingFull() const noexcept;

	/// **THE WALL'S OWN WORDS, VERBATIM AND LOWER CASE**. Not an error string: by design it is a
	/// congratulation, and a player only ever sees it after nesting a device inside a device
	/// `rack::kMaxDepth - 1` times. Drawn as a disabled row at the top of the add menu, and said on the
	/// tray by `addDevice` for any other road to the same refusal.
	///
	/// A CONSTANT SO THE CASE ASSERTS THE WORDS rather than a screenshot of them — `FxLanesRail`'s
	/// three lines are pinned the same way, for the same reason.
	///
	/// **IT IS THE RACK'S, NOT THIS COLUMN'S** (`rack::kNestingFullMessage`, beside the bound it is
	/// about). The rack editor refuses the same add at the same depth and must say the same thing; one
	/// constant is what makes that a fact rather than a promise about two string literals.
	static constexpr const char* kNestingFull = rack::kNestingFullMessage;

	/// Open the picker, as a press on the `(+)` does. Public so a case can drive the gesture without a
	/// window — `visage::PopupMenu::show` needs one, and what a case is about is which devices are
	/// offered and what choosing one does. `before` names the device to insert in front of, or
	/// `kNoInstance` to append; with `after`, the pick goes directly BEHIND that device instead
	/// ("Add after…", `addDeviceAfter`).
	void showAddMenu(DeviceInstanceId before = kNoInstance, bool after = false);
	/// THE MENU `showAddMenu` WOULD SHOW, built and not shown — so a case reads its category submenus
	/// and picks a row without a window.
	visage::PopupMenu addMenuForTest(DeviceInstanceId before = kNoInstance, bool after = false);
	/// Choose the nth offered device, as clicking that row would. Returns the new device's id, or
	/// `kNoInstance` if the chain refused it.
	DeviceInstanceId addChoiceForTest(size_t index);
	/// "Add before" chosen for `anchor`, then the nth device picked — the whole gesture without a popup.
	DeviceInstanceId insertChoiceForTest(DeviceInstanceId anchor, size_t index);

	/// ADD ONE DEVICE AND LAY THE COLUMN OUT. The one road both the (+) and the menu take, so there is
	/// one place a refusal is worded. `before == kNoInstance` appends.
	DeviceInstanceId addDevice(const char* tag, DeviceInstanceId before = kNoInstance);
	/// "ADD AFTER" IS "ADD BEFORE THE NEXT ONE" — or an append behind the last — through `addDevice`, so
	/// the chain-full and nesting refusals are the same ones. `anchor` is resolved at the pick, not when
	/// the menu opened (backlog CR: the card the hand meant, wherever it has moved).
	DeviceInstanceId addDeviceAfter(DeviceInstanceId anchor, const char* tag);
	/// Whether the chain has room for another. What the dimmed (+) is drawn from.
	[[nodiscard]] bool full() const;

	/// Open the slot menu for a device, as pressing the three dots in its card's head does.
	void showSlotMenu(DeviceInstanceId device);
	/// WHAT THAT MENU OFFERS, in order — so a case reads the column's own answer rather than restating
	/// the rules about which ends can move.
	enum class SlotVerb : uint8_t { AddBefore, AddAfter, MoveUp, MoveDown, Remove };
	struct SlotOption {
		SlotVerb verb;
		bool enabled;
	};
	[[nodiscard]] std::vector<SlotOption> slotOptionsFor(DeviceInstanceId device) const;
	/// Drive one row of that menu. Returns whether it did anything.
	bool slotChoiceForTest(DeviceInstanceId device, SlotVerb verb);

	/// REMOVE A DEVICE — the menu's verb and the keyboard's, one implementation. Two roads to a
	/// destructive action is two chances for one of them to skip the card teardown.
	bool removeDevice(DeviceInstanceId device);
	/// Move a device to a position in the chain. Returns whether the chain moved. ONE STEP IN THE
	/// HISTORY, by the device's id — the menu's two rows and the drag all end here, so all three undo.
	bool moveDeviceTo(DeviceInstanceId device, size_t to);

	//---------------------//
	// Dragging a card      //
	//---------------------//

	/// **THE CARD'S DRAG, CONVERTED AND ACTED ON** — `FxLane::cardDragForTest` and
	/// `RackEditor::cardDragForTest`, one surface over. Each card's `onCardDrag` lands here (backlog
	/// JA: this column was the one lane that never bound it, so a card in SKLITTER's GRAIN FX or POST
	/// FX could be picked up and went nowhere). The card reports in its OWN coordinates; only the
	/// column knows where a card sits in it, and a card's y moves with the scroll.
	void cardDragForTest(DeviceInstanceId device, FxCard::DragPhase phase, visage::Point inCard);
	/// The device being dragged, if one is.
	[[nodiscard]] std::optional<DeviceInstanceId> dragDevice() const noexcept { return dragDevice_; }
	/// The gap the drag would drop into — 0 above the first card, `deviceCount()` below the last —
	/// or none while the hand is outside the column, which is the cancel.
	[[nodiscard]] std::optional<size_t> dropGap() const noexcept { return dropGap_; }
	/// The gap under a point in this column's coordinates. `ui::gapAlong` over the cards' spans, the
	/// fx lane's arithmetic.
	[[nodiscard]] std::optional<size_t> dropGapAt(visage::Point position) const;
	/// Where the bumper for a gap is drawn — a bar in the space between two cards, or empty for a gap
	/// the chain does not have.
	[[nodiscard]] visage::Bounds gapBounds(size_t gap) const;
	/// **SCROLL WHILE A DRAGGED CARD IS HELD AT AN EDGE** — one step per tick, the drop gap following
	/// the hand. `FxLane::tickDrag`'s rule. Returns whether anything moved.
	bool tickDrag();
	/// **A DROP OUTSIDE THIS COLUMN, OFFERED TO THE OWNER** (backlog NQ): `ids` (the dragged card, or the selection it
	/// carries, in chain order) released at `inWindow`. The owner answers whether it took them (a Rack's window moves them
	/// into the chain under the hand); unanswered, a drop outside stays the cancel it always was.
	void setOnDropOutside(std::function<bool(std::span<const DeviceInstanceId> ids, visage::Point inWindow)> f) {
		onDropOutside_ = std::move(f);
	}
	/// **WHERE A DROP AT `inWindow` LANDS IN THIS COLUMN**, as a chain position: the gap under the point, the end below
	/// the last card or in an empty chain; none outside the column.
	[[nodiscard]] std::optional<size_t> dropPositionAtWindow(visage::Point inWindow) const;
	/// This column's chain, and the selection whose verbs edit it.
	[[nodiscard]] rack::RackBridge& bridge() noexcept { return bridge_; }
	[[nodiscard]] LaneSelection& selection() noexcept { return *selection_; }
	/// A card's place in this column's coordinates (`cardBounds`), by device.
	[[nodiscard]] visage::Bounds cardBoundsForTest(DeviceInstanceId device) const;
	/// The history this column records into — the owner's, so a case can walk the step a drag made.
	[[nodiscard]] EditHistory& historyForTest() noexcept { return history_; }
	static constexpr float kEdgeScrollBand = 24.0f;
	static constexpr float kEdgeScrollStep = 12.0f;

	/// WHICH CARD A KEY WOULD ACT ON. `kNoInstance` when none.
	void focusDevice(DeviceInstanceId device);
	[[nodiscard]] DeviceInstanceId focusedDeviceForTest() const noexcept { return focused_; }

	/// **THE SELECTION OVER THIS COLUMN'S CHAIN** (backlog OA) — the same `LaneSelection` the chain editor and
	/// Feathers' lane own: its cards, its verbs, its lasso and its Esc entry.
	[[nodiscard]] LaneSelection& selectionForTest() noexcept { return *selection_; }
	void setEscapeStack(EscapeStack* stack) { selection_->setEscapeStack(stack); }
	/// ONE OF THE DOTS MENU'S SELECTION VERBS, picked on `device`'s card — the menu's road, without a menu.
	bool slotVerbForTest(DeviceInstanceId device, FragmentVerb verb);
	static constexpr int kVerbIds = 100;

	//---------------------//
	// The way into a panel //
	//---------------------//

	/// **WHERE THIS COLUMN'S PANELS GO — and, by the same call, whether it offers any.**
	///
	/// Null is the honest default and the state every case that only drives the chain leaves it in:
	/// with no presenter the bridge's expanded-view query is cleared, so `hasExpandedViewFor` declines,
	/// so `FxCard::expandBounds` is empty and no mark is drawn. **A press cannot reach a panel that
	/// cannot be opened, because there is no mark to press.**
	///
	/// The presenter is the layers this column's OWNER was mounted on. It is taken as the OUTER of this
	/// column's own layers rather than presented onto directly — see the banner for the lifetime reason.
	void setPresentedOn(SurfaceLayers* host);
	[[nodiscard]] SurfaceLayers* presentedOnForTest() const noexcept { return surfaces_->outerLayers(); }

	/// **WHICH CHAIN THESE CARDS ARE IN** — `DeviceNest`, stamped on every card and on every panel this
	/// column opens, so the two wear one colour and key one key. The owner computes it from its own
	/// (`ExpandedView::innerNest`); this column only carries it down.
	void setNest(DeviceNest nest);
	[[nodiscard]] DeviceNest nest() const noexcept { return nest_; }

	/// OPEN A DEVICE'S PANEL, as pressing the card's expand mark does. Public so a case can drive the
	/// verb without hit-testing a rectangle; it is a no-op with no presenter or no panel to build.
	void expandDevice(DeviceInstanceId device);
	/// THE SITE CHIP'S VERB (backlog NR): give `device` its PRE/POST chains and open their panel — or close it.
	void expandSite(DeviceInstanceId device);
	[[nodiscard]] size_t expandedCountForTest() const noexcept { return expanded_.size(); }
	[[nodiscard]] SurfaceId expandedIdForTest() const noexcept {
		return expanded_.empty() ? kNoSurface : expanded_.back();
	}

	//---------------------//
	// Geometry             //
	//---------------------//

	/// Where a card sits, by chain POSITION — walked rather than indexed, because a card above this one
	/// may be folded and a card's place is the sum of what is actually above it.
	[[nodiscard]] visage::Bounds cardBounds(size_t position) const;
	/// How tall the whole column's content is: the cards, the gaps and the (+).
	[[nodiscard]] float contentHeight() const;
	[[nodiscard]] float maxScroll() const;
	void setScroll(float y);
	[[nodiscard]] float scrollForTest() const noexcept { return scroll_; }
	void scrollCardIntoView(size_t position);

	/// `FxLane`'s padding and gap, because this is the same object one surface over and two chain
	/// columns that disagreed about their padding would be visible side by side in the same plugin.
	static constexpr float kPadX = 10.0f;
	static constexpr float kPadY = 10.0f;
	static constexpr float kSlotGap = 10.0f;
	/// One notch of the wheel — a FIXED distance and not a card, for the reason the modulator strip
	/// records: a device card is most of this column's height, so a notch of one card would teleport.
	static constexpr float kScrollNotch = 48.0f;

	/// **THE SHORTEST THIS COLUMN IS WORTH DRAWING AT — DERIVED FROM WHAT IT MUST SHOW, NOT CHOSEN.**
	///
	/// A column shorter than this is one a hand cannot work in: it cannot show even a card's head and
	/// one row of its controls together, so the shortest device in the registry is already cut. The sum
	/// is that minimum — **one card's HEAD, one ROW of controls, and the (+)** — plus this column's own
	/// padding and the gap between them.
	///
	/// **IT IS A FLOOR ON THE ROOM, NOT A PROMISE ABOUT ANY PARTICULAR CARD, and the difference is
	/// worth stating because the first draft of this comment got it wrong.** A card TALLER than a head
	/// and a row — which is most of them — still pushes the (+) below the fold, and the scroll is what
	/// answers that. What this guarantees is that the column is never shorter than the smallest
	/// arrangement in which the chain is operable at all; it does not guarantee that a given card
	/// leaves the (+) on screen.
	///
	/// A knob column (`Knob::columnHeight(Knob::kDiameter)`) is the row, because that is what
	/// `GenericExpanded::kCellHeight` is derived from and a generic card's first row is exactly one of
	/// them. Taking it from the same derivation rather than restating 81 is what keeps the two from
	/// drifting the day a knob's readout grows.
	///
	/// **IT IS A FLOOR ON THE PANEL AND NOT A SIZE FOR THIS FRAME.** A rail is as tall as the panel it
	/// is in, so nothing here can make itself taller; `SklitterExpanded::minimumSize` maxes against
	/// `FxLanesRail::kOpenMinimumHeight`, which is this plus the rail's own chrome. PHOTOGRAPHED as the
	/// defect it exists for: body collapsed and circle closed, the panel came out short enough to cut a
	/// filter card off mid-plot with the grain line immediately under it.
	static constexpr float kMinimumUsefulHeight = kPadY + FxCard::kSlotHeadHeight
	                                              + Knob::columnHeight(Knob::kDiameter) + kSlotGap
	                                              + kAddHeight + kPadY;
	/// The mock's card radius, so a device here reads as the same furniture as one in the fx lane.
	static constexpr float kCornerRadius = 5.0f;

private:
	/// WHAT A CARD WAS BUILT FOR — `RackEditor::CardEntry`'s three fields, and they are the three a
	/// sync has to compare: a device whose descriptor changed under its id needs a NEW card, and a tag
	/// this build cannot name needs the missing-device card rather than none at all.
	struct CardEntry {
		DeviceInstanceId device = kNoInstance;
		const void* desc = nullptr;
		device::Tag missing {};
		std::unique_ptr<FxCard> card;
	};
	[[nodiscard]] CardEntry* entryOf(DeviceInstanceId device) noexcept;
	[[nodiscard]] const CardEntry* entryOf(DeviceInstanceId device) const noexcept;
	/// The row a popup opened from `position` hangs off — `FxLane::menuAnchorFor`, for the same reason: both
	/// of this column's menus sit on rows that can be at the bottom of the window.
	[[nodiscard]] visage::Bounds menuAnchorFor(size_t position) const;

	/// Arm a freshly built card's head verbs — the fold, the slot menu and the expand.
	void wireCard(FxCard& card, DeviceInstanceId device);
	/// **ONE ARM PER DEVICE OUTPUT, REBUILT WITH THE CARDS THEY SIT ON** — `RackEditor::syncCards`'
	/// half of the same job, and it is called from the same place for the same reason: an arm names a
	/// device's output by ENDPOINT, so rebuilding wholesale when the chain changes cannot leave one
	/// pointing at somebody else's output, and it is cheap because it only happens when it changes.
	void rebuildOutputArms();
	/// Place them, through the walk every host shares (`ui::layOutCardOutputArms`).
	void layOutOutputArms();
	/// WHAT THE LAYERS NO LONGER HOLD IS NO LONGER OURS — asked rather than told which, because a
	/// dismissal's callback is installed before the id it would name exists (`RackEditor::pruneExpanded`).
	void pruneExpanded();
	/// **PUT THE INBOUND-ROUTE MENU'S SOURCE LIST AND NAMER ON `mod_`** — see the body. Called once from
	/// the constructor and taken off again in the destructor, because `mod_` outlives this frame.
	void installSourceList();

	rack::RackBridge& bridge_;
	WidgetRegistry& registry_;
	ModState& mod_;
	EditHistory& history_;
	Notifier* notifier_ = nullptr;
	/// THE EAR ON OUTPUT RELABELS (`RackBridge::subscribeOutputLabel`) — see the constructor.
	device::Subscription labelSubscription_;
	/// See `setCoreSourceNamer`.
	std::function<std::string(size_t)> coreNamer_;
	std::vector<CardEntry> cards_;
	/// **WHAT A CHIP AT THE FOOT OF A CARD IS FOR** — `FxLane::OutputArm` and `RackEditor::OutputArm`'s
	/// three fields, and they are kept for the same reasons: the DEVICE rather than a position, because
	/// a reorder must move nothing; the OUTPUT, because a device may publish several and only the name
	/// says which is which; and the arm itself.
	///
	/// **CHILDREN OF THE COLUMN AND NOT OF THE CARD, DELIBERATELY.** A card is rebuilt whenever its
	/// device changes and an arm on `mod_`'s watcher list must not be rebuilt with it — the crash
	/// `forgetFrameTree` exists for. It is also what puts the chip ABOVE the card it shares a rectangle
	/// with rather than under it, which `SourceArm`'s own header records as the defect that made it a
	/// widget in the first place.
	struct OutputArm {
		DeviceInstanceId device = kNoInstance;
		size_t output = 0;
		std::unique_ptr<SourceArm> arm;
	};
	std::vector<OutputArm> outputArms_;
	DeviceInstanceId focused_ = kNoInstance;
	/// BACKLOG OA — see `selectionForTest`; and the frame that draws its lasso and pill above the cards.
	std::unique_ptr<LaneSelection> selection_;
	std::function<bool(std::span<const DeviceInstanceId>, visage::Point)> onDropOutside_;
	std::unique_ptr<SelectionOverlay> selectionOverlay_;
	[[nodiscard]] std::vector<DeviceInstanceId> chainOrder() const;
	void applySelection();
	void afterVerb();
	float scroll_ = 0.0f;
	/// **THE DRAG, BY DEVICE AND NOT BY POSITION** — a card belongs to its device here, so the thing
	/// picked up is named the way the card is.
	std::optional<DeviceInstanceId> dragDevice_;
	std::optional<size_t> dropGap_;
	/// Where the hand last was, in this column's coordinates — what `tickDrag` scrolls against.
	visage::Point dragPoint_ {};
	/// **THE ORDER THE CARDS WERE LAST PLACED IN.** A move that did not come through this column — an
	/// undo, whose record may not reach a frame — changes the chain and builds no card, so `syncCards`
	/// compares against this to know the stack needs placing again.
	std::vector<DeviceInstanceId> laidOrder_;
	DeviceNest nest_ {};
	/// **THIS COLUMN'S OWN LAYERS, WHICH SHOW NOTHING OF THEIR OWN.** Every present here is
	/// `SpansGesture` and is forwarded to the outer (see the banner); the local overlay and takeover
	/// stay empty for the frame's whole life.
	///
	/// **IT IS CARRIED FOR A LIFETIME REASON AND NOT AS AN ECHO OF `RackEditor`, AND THE REASON IS A
	/// MATTER OF TIMING RATHER THAN OF TIDINESS.** Presenting straight onto the presenter this column
	/// was given is a documented use-after-free:
	///
	///   - a panel this column opens registers its knobs on **this column's** `ModState` (`mod_`), which
	///     belongs to the rail and dies with it;
	///   - the OUTER overlay does not free a retired panel when it is dismissed — `ExpandedOverlay`
	///     holds it in `retired_`, and it *"lives until the next `draw`"*;
	///   - so the mount can outlive `mod_` by a frame, and the knobs' destructors then unwatch through
	///     a dead `ModState`.
	///
	/// `SurfaceLayers::forwarded_` is exactly the machinery that closes that window: a forwarded
	/// dismissal calls `forgetFrameTree` on the mount **synchronously**, taking the panel's knobs off
	/// this column's lists BEFORE the outer gets round to freeing anything. That is the class's own
	/// stated purpose — *"the mount is remembered because its knobs are on THIS host's modulation list
	/// … and that list can die before the outer frees the retired mount"* — and it is the assert the
	/// sanitizer gate found in `~FrameWatchers` on 2026-09-13 when it was missing.
	///
	/// **DECLARED AFTER THE CARDS SO IT OUTLIVES THEM** — a card's `onExpand` reaches through it.
	std::unique_ptr<SurfaceLayers> surfaces_;
	std::vector<SurfaceId> expanded_;
};

} // namespace feathers::ui
