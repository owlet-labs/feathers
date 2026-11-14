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

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/fx_lane.h"

#include "plugin/ui/card_factory.h"

#include "plugin/device_registry.h"
#include "fx/owltt/owltt.h"
#include "plugin/ui/device_edits.h"
#include "plugin/ui/device_menu.h"
#include "plugin/ui/automod_card.h"
#include "plugin/ui/owltt_card.h"
#include "plugin/ui/drawn_card.h"
#include "plugin/ui/drawn_expanded.h"
#include "plugin/ui/shaper_card.h"
#include "plugin/ui/waypoint_card.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/harm_card.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"

#include <visage/ui.h>

#include <algorithm>
#include <string>

namespace feathers::ui {

namespace {
/// The card radius the modulator strip uses, so a docked device and a macro card will read as the
/// same furniture when the devices arrive.
constexpr float kCornerRadius = 5.0f;
} // namespace

FxLane::FxLane(ParamBridge& bridge, WidgetRegistry& registry, ModState* mod, EditHistory* history,
               Notifier* notifier, FrameWatchers* watchers)
    : Rail("POST-FX", Rail::Side::Right), bridge_(bridge), registry_(registry), mod_(mod),
      history_(history), notifier_(notifier), watchers_(watchers) {
	// **DELETE AND BACKSPACE, THE SAME AS THE RACK'S.** This is about effects, not about one host —
	// a gesture that works in the rack and not in Feathers is worse than one that works in
	// neither, because the hand learns it and then it fails somewhere.
	//
	// A KEY NOBODY WANTS IS NOT SWALLOWED: `false` hands it on. `ExpandedOverlay`'s Esc handler had to
	// learn that rule and its note is the one this follows.
	setAcceptsKeystrokes(true);
	// **THE SELECTION OVER THE LANE** (backlog OA, D1) — the chain editor's own `LaneSelection`, over this bridge.
	LaneSelection::Hooks selectionHooks;
	selectionHooks.order = [this] { return laneOrder(); };
	selectionHooks.changed = [this] { applySelection(); };
	selectionHooks.cardBounds = [this](DeviceInstanceId id) -> std::optional<visage::Bounds> {
		const size_t slot = bridge_.fxSlotOf(id);
		if (slot == kNoPosition || cardAt(slot) == nullptr || !cardAt(slot)->isVisible()) {
			return std::nullopt;
		}
		return cardAt(slot)->bounds();
	};
	selection_ = std::make_unique<LaneSelection>(bridge_, history_ != nullptr ? *history_ : spareHistory_, notifier_,
	                                             std::move(selectionHooks));
	selectionOverlay_ = std::make_unique<SelectionOverlay>(*selection_);
	onKeyPress() = [this](const visage::KeyEvent& e) {
		// THE SELECTION'S KEYS FIRST (backlog OA): Ctrl/Cmd + A C X V D G, and Delete over two or more.
		if (selection_->key(e)) {
			afterVerb();
			return true;
		}
		const visage::KeyCode code = e.keyCode();
		if (code != visage::KeyCode::Delete && code != visage::KeyCode::Backspace
		    && code != visage::KeyCode::KPBackspace) {
			return false;
		}
		if (focusedSlot_ >= cards_.size() || cards_[focusedSlot_] == nullptr) {
			return false;
		}
		const size_t going = focusedSlot_;
		// **FOCUS GOES FIRST, AND HERE IT MATTERS MORE THAN IN THE RACK.** This chain COMPACTS — every
		// device below the hole slides up one — so an index kept across a removal does not merely name
		// an empty slot, it names a DIFFERENT DEVICE. Holding Delete would walk the chain.
		focusSlot(kNoFocus);
		return removeDeviceAtLane(going);
	};
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		ensureCard(slot);
	}
	// A RELABELLED OUTPUT RE-LAYS THE LANE, so its arm is rebuilt with the new word at the new width.
	// Nothing in `resized` rebuilds a card for a label (a card is keyed by its tag and its device), so
	// this is safe inside the MACROS card's own rename commit.
	labelSubscription_ = bridge_.subscribeOutputLabel([this](DeviceInstanceId, size_t) {
		resized();
		redraw();
	});
	onMouseDown() = [this](const visage::MouseEvent& e) {
		switch (partAt(e.position)) {
		case Part::Collapse:
			toggleCollapsed();
			break;
		case Part::Add:
			showAddMenu();
			break;
		case Part::None:
			// THE BARE LANE: a lasso (backlog OA). A click that does not drag clears the selection.
			if (!collapsed()) {
				selection_->lassoBegin(e.position, e.isShiftDown());
			}
			break;
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) { selection_->lassoMove(e.position); };
	onMouseUp() = [this](const visage::MouseEvent& e) {
		if (selection_->lassoRect().has_value()) {
			selection_->lassoMove(e.position);
			selection_->lassoEnd();
		}
	};
	addChild(selectionOverlay_.get());
}

std::vector<DeviceInstanceId> FxLane::laneOrder() const {
	std::vector<DeviceInstanceId> out;
	const rack::ChainRecord& record = bridge_.chainRecord();
	for (size_t p = 0; p < record.length(kLaneChain); ++p) {
		out.push_back(record.deviceAt(kLaneChain, p));
	}
	return out;
}

void FxLane::applySelection() {
	const CardSelection& s = selection_->selection();
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		if (cards_[slot] != nullptr) {
			cards_[slot]->setFocused(slot < deviceCount() && s.contains(bridge_.fxDeviceAt(slot)));
		}
	}
	// THE KEY'S TARGET: the anchor, as a slot — Delete on one card is the lane's own road.
	const size_t anchor = s.anchor() == kNoInstance ? kNoPosition : bridge_.fxSlotOf(s.anchor());
	focusedSlot_ = anchor == kNoPosition ? kNoFocus : anchor;
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->redraw();
	}
}

void FxLane::afterVerb() {
	resized(); // the cards follow the chain (`ensureCard` per slot)
	selection_->prune();
	applySelection();
	redraw();
}

bool FxLane::slotVerbForTest(size_t slot, FragmentVerb verb) {
	if (slot >= deviceCount()) {
		return false;
	}
	if (!selection_->chooseVerb(kVerbIds + static_cast<int>(verb), kVerbIds, bridge_.fxDeviceAt(slot))) {
		return false;
	}
	afterVerb();
	return true;
}

size_t FxLane::deviceCount() const {
	// THE CHAIN DECIDES HOW MANY CARDS THERE ARE, not the lane. One today; the (+) will make it more
	// without this file changing, which is the point of the change that put this here.
	//
	// THE COUNT, NOT THE CHAIN. This read `bridge_.fxChain().count` -- a ~4 KB copy built from about a
	// thousand relaxed loads, to read one field of it -- and it is asked several times per card per
	// idle frame: the loop conditions below re-evaluated it every iteration and cardAt() asked again.
	// MEASURED before the change at 1.04 us per idle frame with ONE card, almost all of it copies
	// (backlog T; the numbers after are in the commit that made this change). Same shape as the
	// routes() defect fixed at 48607d7: a by-value accessor inside a per-frame loop.
	return std::min(bridge_.fxDeviceCount(), cards_.size());
}

FxCard* FxLane::cardAt(size_t slot) const {
	return slot < deviceCount() ? cards_[slot].get() : nullptr;
}

const device::Descriptor* FxLane::descriptorIn(const FxChain& chain, size_t slot) const {
	if (slot >= chain.count) {
		return nullptr;
	}
	const device::Tag tag = fxDeviceAtPosition(chain, slot).tag();
	return deviceForTag(tag.data());
}

const device::Descriptor* FxLane::descriptorAt(size_t slot) const {
	// **THE CHAIN IS READ BY VALUE AND IT IS ABOUT 4 KB, so this is the convenience form and callers
	// in a LOOP take `descriptorIn` with a chain read once.** Backlog T is this exact shape found in
	// this exact file — a by-value accessor inside a per-slot walk — and `deviceCount()`'s own note
	// records the measurement. This one runs when an arm is BUILT, which happens when the chain
	// changes rather than when it is laid out.
	const FxChain chain = bridge_.fxChain();
	return descriptorIn(chain, slot);
}

backplane::Source FxLane::sourceOfOutput(size_t slot, size_t output) const {
	// **ASKED, NOT REMEMBERED** — `ModPanel::sourceOfOutput`'s contract, and the reason is the same in
	// this lane: a source names a DEVICE, and which device stands at a slot changes whenever the chain
	// is shortened or reordered, so an arm that kept the answer would arm somebody else's output.
	//
	// **A SLOT HOLDING NOTHING ANSWERS `dormantOutput`, NOT `output(kNoInstance, …)`** — that is the
	// host's own device number, so it would be MACRO 1 and a dormant arm would point at the macro row.
	// The constant and its reasoning live once, in `device/endpoint.h`, because this lane and the
	// modulator strip had both spelled it out.
	const DeviceInstanceId device = bridge_.fxDeviceAt(slot);
	return device != kNoInstance ? backplane::Source::output(device, output)
	                             : backplane::Source::dormantOutput(output);
}

SourceArm* FxLane::ensureOutputArm(size_t slot, size_t output) {
	for (OutputArm& entry : outputArms_) {
		if (entry.slot == slot && entry.output == output && entry.arm != nullptr) {
			return entry.arm.get();
		}
	}
	const device::Descriptor* desc = descriptorAt(slot);
	if (desc == nullptr || output >= desc->outputs.size()) {
		return nullptr;
	}
	OutputArm entry;
	entry.slot = slot;
	entry.output = output;
	// WHAT THIS INSTANCE CALLS THE OUTPUT (`ParamBridge::outputLabel`) — the declared name until a hand
	// relabels it, as every route list reads it.
	entry.label = bridge_.outputLabel(bridge_.fxDeviceAt(slot), output);
	entry.arm = std::make_unique<SourceArm>(*mod_, sourceOfOutput(slot, output), entry.label);
	// THE WAY TO ASK AGAIN — the arm's identity is its PLACE in the lane, and the source is derived
	// from that place at every press and every paint.
	entry.arm->setSourceProvider([this, slot, output] { return sourceOfOutput(slot, output); });
	// AND A PRESS ON THE CHIP SELECTS THE CARD IT SITS ON. The chip is on top of the card, so the
	// card's own `onCardPressed` never hears this press — `SourceArm::onPressed` exists for exactly
	// this, and the defect behind it was a lit card with a different card deleted.
	entry.arm->onPressed = [this, slot] { focusSlot(slot); };
	SourceArm* raw = entry.arm.get();
	// **ABOVE THE CARD IT SHARES A RECTANGLE WITH, STATED RATHER THAN INHERITED FROM CALL ORDER.** The
	// card frame covers the whole slot including this strip, and `ensureCard` re-`addChild`s a card
	// whenever its device changes — which puts the card at the END of the child list and therefore on
	// top of its own arrows. `ModPanel` was measured losing two cases to exactly that.
	raw->setOnTop(true);
	addChild(raw);
	outputArms_.push_back(std::move(entry));
	return raw;
}

void FxLane::layOutOutputArms() {
	// **THE ARMS ARE PRUNED BEFORE THEY ARE PLACED**, because a slot whose device changed keeps its
	// number: an arm kept across that swap would name the old device's output beside the new one's
	// arrow. The label is part of the identity for that reason; the SOURCE deliberately is not, since
	// it is asked for at use.
	//
	// **THE CHAIN IS READ ONCE FOR BOTH WALKS BELOW.** It is a ~4 KB copy and this runs on every
	// layout — which is every scroll — so a read per arm and per slot is backlog T's defect wearing
	// this function's name. It cannot change underneath them: this is the message thread and neither
	// walk edits the chain.
	const FxChain chain = bridge_.fxChain();
	for (auto it = outputArms_.begin(); it != outputArms_.end();) {
		const device::Descriptor* desc = descriptorIn(chain, it->slot);
		// THE LABEL, NOT THE DECLARATION — so a relabel is an identity change and the arm is rebuilt with
		// the new word (and the walk below measures it at the new width).
		const bool declared = desc != nullptr && it->output < desc->outputs.size();
		const bool stale = it->arm == nullptr || it->slot >= deviceCount() || !declared
		                   || it->label != bridge_.outputLabel(bridge_.fxDeviceAt(it->slot), it->output);
		if (!stale) {
			++it;
			continue;
		}
		if (it->arm != nullptr) {
			// RETIRED, NOT DESTROYED — see `retiredArms_`. This runs from `resized()`, which a card's
			// own mouse-up reaches through `moveDevice`, so the frame being dropped may be one visage
			// is still walking.
			removeChild(it->arm.get());
			retiredArms_.push_back(std::move(it->arm));
		}
		it = outputArms_.erase(it);
	}
	// **NO ARMS WITHOUT A PANEL STATE TO ARM AGAINST.** `SourceArm` holds a `ModState&` and this lane's
	// constructor takes a POINTER that defaults to null — so a lane built without one draws no chips,
	// which is the honest picture rather than a chip that would arm nothing. (It is also the only
	// thing that could be done: there is no reference to bind.)
	if (mod_ == nullptr) {
		return;
	}
	for (size_t slot = 0; slot < deviceCount(); ++slot) {
		const device::Descriptor* desc = descriptorIn(chain, slot);
		FxCard* card = cards_[slot].get();
		if (desc == nullptr || desc->outputCount() == 0 || card == nullptr) {
			continue;
		}
		// **THE ARMS ARE BUILT HERE AND PLACED BY THE SHARED WALK** — `ui::layOutCardOutputArms`, which
		// carries the strip-in-host-coordinates arithmetic and the three honesty tests. Building is
		// this lane's own because an arm here belongs to a SLOT and is pruned on a label change; where
		// a chip goes and whether it is shown is the same question in every host, and used to be
		// answered in four places, one of which did not answer it at all.
		std::vector<SourceArm*> arms;
		arms.reserve(desc->outputCount());
		for (size_t output = 0; output < desc->outputCount(); ++output) {
			arms.push_back(ensureOutputArm(slot, output));
		}
		layOutCardOutputArms(*card, arms);
	}
}

size_t FxLane::updateCards() {
	size_t told = 0;
	// READ ONCE: the count cannot change inside one update, and this is the per-frame road.
	const size_t devices = deviceCount();
	for (size_t slot = 0; slot < devices; ++slot) {
		FxCard* card = cards_[slot].get();
		if (card != nullptr && card->isVisible() && card->update()) {
			++told;
		}
	}
	return told;
}

size_t FxLane::syncCards() {
	reserveCards(bridge_.fxDeviceCount());
	// `ensureCard`'s own key, asked without building: a seated slot is current when its card was built
	// for this tag AND this device; an empty slot is current when it holds no card.
	const FxChain chain = bridge_.fxChain();
	size_t stale = 0;
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		const bool current = slot < chain.count
		                         ? cards_[slot] != nullptr && cardTag_[slot] == fxDeviceAtPosition(chain, slot).tag()
		                               && cardDevice_[slot] == bridge_.fxDeviceAt(slot)
		                         : cards_[slot] == nullptr;
		stale += current ? 0u : 1u;
	}
	if (stale > 0) {
		// LAID OUT, NOT JUST REDRAWN — `addDevice` says why: resized() is what builds the card, sizes it
		// and shows it.
		resized();
		redraw();
	}
	return stale;
}

FilterCard* FxLane::filterCard() const noexcept {
	// SEARCHED RATHER THAN REMEMBERED. The lane held one pointer while it could hold one card; a
	// stored pointer would now answer with slot 0's card whatever slot 0 turned out to hold.
	const size_t devices = deviceCount();
	for (size_t slot = 0; slot < devices; ++slot) {
		if (auto* filter = dynamic_cast<FilterCard*>(cards_[slot].get()); filter != nullptr) {
			return filter;
		}
	}
	return nullptr;
}

void FxLane::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	// EVERY CARD, AND THE STATE IS KEPT so a card built later gets it too. Handing it only to the
	// cards that exist now is the shape that leaves a device added at runtime unable to be routed to,
	// and an untold card is silently exactly as it was.
	for (std::unique_ptr<FxCard>& card : cards_) {
		if (card != nullptr) {
			card->setModulation(mod, history);
		}
	}
}

void FxLane::ensureCard(size_t slot) {
	reserveCards(slot + 1);
	// WHAT THE SLOT ACTUALLY HOLDS, asked of the chain rather than remembered — a patch load can put
	// a different device in a slot without the lane being told, and the (+) will do it on purpose.
	const FxChain chain = bridge_.fxChain();
	device::Tag tag {};
	if (slot < chain.count) {
		tag = fxDeviceAtPosition(chain, slot).tag();
	}
	// **THE BOUNDARY, CROSSED ONCE, HERE.**
	//
	// `slot` is a POSITION — it indexes `cards_` and `cardTag_`, which are the lane's own drawing
	// order. A CARD is addressed by its DEVICE'S ID (kill-lanes step 2; a routing lane before that),
	// because the id is the device's identity and survives a reorder or a deletion above it.
	//
	// hosting's warning was that two addresses living in one object is the condition that produced
	// `routeLaneOf`. **Neither object holds both: the card knows its device and nothing else, and the
	// lane knows the order because it keys `cards_[position]` itself.** This is the only line where one
	// becomes the other, which is what "state the boundary where it is crossed" means — not a helper
	// each site remembers to call.
	const DeviceInstanceId device = bridge_.fxDeviceAt(slot);
	// **AND THE DEVICE IS PART OF THE KEY.** See `cardDevice_`. The tag answers "what KIND of device is
	// at this position"; the id answers "WHICH ONE", and only the second changes when a removal or a
	// reorder shifts a same-tag neighbour into a position. Found by asking whether the modulator strip's
	// version of this defect was here too — it was, MEASURED at `1 == 2` with three filters and the
	// middle one removed.
	if (cards_[slot] != nullptr && cardTag_[slot] == tag && cardDevice_[slot] == device) {
		return;
	}

	// LAST ROUND'S CARDS ARE **NOT** FREED HERE, AND THE LINE THAT DID IT CRASHED EVERY TIME ONE
	// REORDERED WITH AUDIO PLAYING. It read:
	//
	//     // FREE LAST ROUND'S CARDS FIRST, WHEN NOTHING IS WALKING THE TREE.
	//     retiredCards_.clear();
	//
	// and that premise is true when `ensureCard` runs from a state-channel notify — which is where it
	// was written — and FALSE on the road a drag takes. `FxCard::onMouseUp` calls
	// `onCardDrag(DragPhase::End)`, which reaches `FxLane::moveDevice`, which rebuilds. **A reorder
	// always touches at least two slots**, so slot A retires its card and then slot B's `clear()`
	// frees it — while that very card's `onMouseUp` lambda is still on the stack and visage is still
	// walking the tree behind it.
	//
	// MEASURED FROM A FIELD CRASH REPORT, resolved through `scripts/resolve-crash.sh`:
	//
	//     crashHandler <- signal 11
	//     Frame::on_mouse_exit_ lambda        <- a std::function on freed storage
	//     Frame::propagateMouseEvent
	//     WindowEventHandler::handleMouseUp
	//     WindowX11::processEvent
	//     Gui::processEvents
	//
	// The dispatch that invoked our handler kept walking into the frame our handler had destroyed.
	// "Probably happens every time", as it was reported, is exactly what two slots per reorder predicts.
	//
	// SO THE FREE MOVES TO A POINT THAT IS PROVABLY NOT INSIDE A DISPATCH: `Gui::Impl::tick()`, which
	// runs from `processEvents` BEFORE `processPluginFdEvents` drains any X event. Retirement was
	// always the right idea — this is the same lesson `ExpandedOverlay::retired_` records one layer
	// up, and the same one `WindowedExpanded` learned about `ApplicationWindow::close()`: **a thing
	// must not be destroyed from inside the dispatch that is still using it, and "nothing is walking
	// the tree" is a claim about the CALLER, which a comment here cannot check.**

	if (cards_[slot] != nullptr) {
		// RETIRED, NOT DESTROYED. `ensureCard` runs from a state-channel notify, and the editor may be
		// mid-`drawStaleChildren` — visage holds raw `Frame*` in that list and dereferences a widget's
		// `Modulatable*` while painting it. Destroying a card here freed its `FxControl`s under a draw
		// that was already scheduled, and `Knob::draw`'s `control_->display()` called through a dead
		// vtable. It showed as a crash every time OWLTT was added: adding a device runs
		// `ensureCard` for the slot and the new OWLTT card's paint landed on the previous card's grave.
		//
		// THIS IS THE OVERLAY'S BUG IN A SECOND FILE. `ExpandedOverlay::dismissBy` was fixed the same
		// way and its comment states the rule -- "deleting a frame out from under that walk is how a
		// SIGSEGV lands in an unrelated frame's handler" -- and this file kept doing it. A rule fixed
		// in one place is not a rule followed in the others.
		// OFF BOTH REDRAW LISTS BEFORE IT GOES, AND THE WHOLE SUBTREE WITH IT — backlog 19.
		//
		// This card and every widget inside it are on raw-pointer lists that call `redraw()` on
		// their entries, and NOTHING took them off. The reported stack was
		// `ParamControl::setAmount` -> `mod_->touched()` -> `notify()`, walking a list that still
		// held a card freed some earlier round. Retiring the card fixed the paint-under-destruction
		// half and left this one, because they are different lifetimes: retirement is about who is
		// walking the TREE right now, this is about who kept a pointer AFTER.
		//
		// HERE, NOT AT THE FREE. `retiredCards_.clear()` happens above and the children are read from
		// the frame, so the subtree must be walked while it is still assembled.
		// ALL THREE LISTS IN ONE CALL, which it was not until the signature grew a third parameter.
		//
		// THE MODULATION LIST IS NOT THE STATE ONE, and cleaning one and calling it done is what this
		// lane shipped: `Knob::setModulation` puts every knob on `ModState`'s watchers so the glow
		// follows arming, a card destroyed here left them there, and the next `mod_.focus()` —
		// clicking a modulator's arrow to assign it — walked them.
		//
		// THE REPRODUCTION: add MSEG, delete filter, add Drawn, click assign. SIGSEGV in
		// `FrameWatchers::notify` under `ModState::focus` (96d94be).
		//
		// IT SURVIVED THREE NOTIFICATIONS FIRST, which is why it read as unrelated to the delete: a
		// dangling read is undefined, not reliably fatal, so the crash landed on whichever gesture
		// happened after the memory was reused rather than on the one that broke it.
		forgetFrameTree(cards_[slot].get(), &registry_, watchers_,
		                mod_ != nullptr ? &mod_->watchers() : nullptr);
		removeChild(cards_[slot].get());
		cards_[slot]->retired();
		retiredCards_.push_back(std::move(cards_[slot]));
		// THE SUBSCRIPTION GOES WITH THE CARD. Clearing the token first takes the frame off the state
		// list while it is still alive, rather than leaving a dead pointer for `notify()` to redraw.
		cardWatches_[slot] = {};
		cards_[slot].reset();
	}
	cardTag_[slot] = tag;
	// BOTH HALVES OF THE KEY, WRITTEN TOGETHER AND BEFORE THE EARLY RETURN — an empty slot has to
	// record its device too, or it compares unequal on every layout and rebuilds nothing, forever.
	cardDevice_[slot] = device;
	if (slot >= chain.count) {
		return; // an empty slot has no card, which is not the same as a card with nothing in it
	}

	// A NEW CARD RATHER THAN A RECONFIGURED ONE, for the reason the modulator strip's ensureCard
	// gives: two devices have different controls, different heights and different gestures, and a
	// card that tried to become the other one would be the generic knob-list this design exists to
	// avoid, wearing a device's name.
	//
	// THE GENERIC CARD IS THE FALLBACK for a device this build has heard of but has no specific card
	// for — including a device that declines to draw itself. It is chosen by what the lane can BUILD,
	// not by a list of devices that are missing cards, so it needs no maintenance when one arrives. A
	// device the build has NEVER heard of (desc == nullptr) gets NoEditorCard instead — there is no
	// descriptor to walk.
	// **THE SHARED SELECTION FIRST — `ui::makeCustomCard`.** This was a hand-written chain of
	// `sameTag` branches, one per device, and the rack asks a TABLE. Two lists that must agree about
	// which UI a device gets, and **they had already stopped agreeing**: Waypoint got its plane and
	// four dots here and a generic knob list in the rack, so one device wore two faces. That is the
	// exact failure `card_factory.h`'s own header says the table exists to prevent.
	//
	// **AND FILTER IS IN THE TABLE NOW, so there is no exception left to name.** This branch existed
	// because `FilterCard` took a `ParamBridge&` — the type only Feathers has — which meant it could
	// not be built from a `backplane::DeviceHost` and so could not go in the table the rack asks. The
	// cost was exactly the failure the table exists to prevent: the rack drew a filter as a generic
	// knob list, `FILTER ENABLED` and all, while Feathers drew it with a curve. One device, two faces.
	//
	// It reads its row through `fieldsPresentAt`/`fieldAt` now, over the same `DeviceRow` `fxFilterAt`
	// used to walk, so Feathers' numbers are unchanged by construction rather than by testing.
	if (const device::Descriptor* known = deviceForTag(tag.data());
	         known != nullptr
	         && (cards_[slot] = makeCustomCard(deviceHost_, registry_, device, *known, notifier_, mod_,
	                                           history_))
	                != nullptr) {
		// THE TABLE ANSWERED. Nothing more to do — the assignment IS the branch, so there is no second
		// place holding a card that the condition already built.
	}
	else {
		const device::Descriptor* desc = deviceForTag(tag.data());
		if (desc != nullptr) {
			// A DEVICE THIS BUILD HAS HEARD OF BUT HAS NO SPECIFIC CARD FOR — draw it generically by
			// walking its descriptor. This is the seat device.h's hasCard=false was documented to
			// fill, but selected structurally: the lane cannot build a card for this tag, so it
			// builds the generic one.
			cards_[slot] = std::make_unique<GenericCard>(deviceHost_, device, *desc, notifier_);
		}
		else {
			// A DEVICE THIS BUILD DOES NOT HAVE — kept in the record, and it says so ("device
			// missing"). The one card every host draws for it.
			cards_[slot] = std::make_unique<MissingDeviceCard>(std::string_view(tag.data(), tag.size()));
		}
	}
	cards_[slot]->setModulation(mod_, history_);
	// THE LANE'S VERBS, HANDED OVER AS THE CARD IS BUILT — the same place and the same reason as the
	// state-road subscription below. Only the lane knows which slot a card is the face of, and a card
	// rebuilt after a remove is the face of a DIFFERENT device, so the slot has to be bound here
	// rather than remembered by the card.
	cards_[slot]->onSlotMenu = [this, slot] { showSlotMenu(slot); };
	// **THE SELECTION'S TWO ROADS** (backlog OA, D1; `LaneSelection`'s banner): any press on the card, a knob's
	// included, and the card's own press with the modifier held. By DEVICE, which the selection keeps across a reorder.
	cards_[slot]->onCardPressed = [this, device] { selection_->focusPress(device); };
	cards_[slot]->onCardSelectPress = [this, device](CardSelection::Press kind) { selection_->cardPress(device, kind); };
	cards_[slot]->onCardClicked = [this, device] { selection_->cardClick(device); };
	// **THE FOLD IS THE DEVICE'S, KEPT BY THE BRIDGE** — `ChainColumn`'s and the strip's rule, and now this lane's:
	// it holds modulators since 2026-10-04, whose fold the strip saved with the patch, and a lane card's own
	// `folded_` forgot it on every rebuild. Folded as the patch left it BEFORE the head marks are taken from it.
	cards_[slot]->setFolded(bridge_.deviceFolded(device));
	{
		CardHeadMarks marks = cards_[slot]->headMarks(); // only the fold is this lane's to say
		marks.folded = cards_[slot]->folded();
		cards_[slot]->setHeadMarks(marks);
	}
	cards_[slot]->onHeadFold = [this, c = cards_[slot].get(), device] {
		c->setFolded(!c->folded());
		CardHeadMarks marks = c->headMarks();
		marks.folded = c->folded();
		c->setHeadMarks(marks);
		bridge_.setDeviceFolded(device, c->folded());
		resized();
		redraw();
	};
	// AND THE WAY INTO THE EXPANDED VIEW. Bound here for the reason above and one more: the lane
	// cannot open the layer either — it is a sibling under the editor's root — so this forwards to
	// whoever installed `onExpandDevice`. Three hops, and each one is the shortest thing that knows
	// the next: the card knows a hand pressed it, the lane knows which SLOT, the editor knows where
	// the layer is.
	//
	// WHAT AN UNREACHABLE PANEL COSTS, PER DEVICE — kept because it is the evidence, not the history.
	// This used to be five `dynamic_cast` arms and five paragraphs explaining why each was worth adding.
	// The arms are gone (see below); the stakes are not:
	//
	//   DRAWN     — 96 of its 103 controls live in the editor. A missing arm leaves most of the device
	//               unreachable.
	//   SHAPER    — 7 of 12 controls are on the panel and NOWHERE else: Gamma, Harmonics, five switches.
	//   OWLTT      — gained a panel when its twelve knobs became three.
	//   WAYPOINT  — THE ONE THAT ACTUALLY SHIPPED BROKEN. `WaypointExpanded` was implemented, its entry
	//               registered, `hasExpanded` true — and no hand could open it, because this arm was
	//               missing. The plan that built all three declared itself complete.
	//
	// AND THE GUARD COULD NOT SEE IT: `hasExpandedView` is `traits.hasExpanded && entryFor(desc)`, and
	// there is no third term for "and something CALLS it". Both halves were known-badded when the device
	// was built, both went red as intended, and the panel was still unreachable. `Shaper` was the fourth
	// to be forgotten and the first caught before shipping — by a sweep that presses the button through
	// the real tree, which is the only check that can see a missing caller.
	//
	// EVERY CARD, ON THE BASE — this was five `dynamic_cast`s, one per card type, each with this exact
	// body. `FxCard::onExpand` carries the reasoning; the short version is that the arm you forget to
	// add is the bug, and `WaypointExpanded` was complete, correct and unreachable for exactly that.
	// A card whose device declares no expanded view simply never has this called.
	cards_[slot]->onExpand = [this, slot] {
		if (onExpandDevice) {
			onExpandDevice(slot);
		}
	};
	// AND THE SITE CHIP (backlog NR), by the slot for the same reason — bound only when the editor offers it, so a
	// card in a lane nobody wired draws no chip (`FxCard::offersSite`).
	bindSite(slot);
	// AND A PRESS ON THE HEAD BRINGS THAT DEVICE'S OPEN WINDOW TO THE FRONT. Asked of the card at
	// press time, by its device and chain — the panel's own key — so a card rebuilt for another device
	// raises that device's window and never the one this slot used to hold.
	cards_[slot]->onHeadPressed = [this, card = cards_[slot].get()] {
		if (onFocusDeviceWindow) {
			(void)onFocusDeviceWindow(card->device(), card->nest());
		}
	};

	// AND THE DRAG, bound to the slot the same way and for the same reason: a card rebuilt after a
	// remove is the face of a DIFFERENT device, so the slot is bound here rather than remembered by
	// the card.
	cards_[slot]->onCardDrag = [this, slot](FxCard::DragPhase phase, visage::Point inCard) {
		cardDragForTest(slot, phase, inCard);
	};
	// AND THE FOLD, which changes the card's HEIGHT and so the whole lane's layout — every card below
	// it moves and the scroll extent shrinks. resized() does both; redraw() does neither.
	cards_[slot]->onFoldChanged = [this] {
		resized();
		redraw();
	};
	addChild(cards_[slot].get());
	// SUBSCRIBED AS IT IS BUILT. A swallowed device's settings are not parameters, so the state road
	// is the only thing that will repaint this card; registering from outside means remembering to do
	// it again on every rebuild, and a card that quietly stops repainting is invisible to every suite.
	if (watchers_ != nullptr) {
		cardWatches_[slot] = watchers_->watch(cards_[slot].get());
	}
}

void FxLane::bindSite(size_t slot) {
	if (slot >= cards_.size() || cards_[slot] == nullptr) {
		return;
	}
	if (!onExpandSite_) {
		cards_[slot]->onSite = nullptr;
		return;
	}
	cards_[slot]->onSite = [this, slot] {
		if (onExpandSite_) {
			onExpandSite_(slot);
		}
	};
}

void FxLane::setOnExpandSite(std::function<void(size_t)> verb) {
	onExpandSite_ = std::move(verb);
	// THE CARDS ALREADY BUILT — the constructor's, which is every card of the patch the editor opened on.
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		bindSite(slot);
	}
	redraw();
}

void FxLane::setOwlttMeterConnector(std::function<void(size_t slot, void* dspStorage)> connector) {
	owlttMeterConnector_ = std::move(connector);
}

void FxLane::connectOwlttDsp(size_t slot, void* dspStorage) {
	if (owlttMeterConnector_) {
		owlttMeterConnector_(slot, dspStorage);
	}
}

bool FxLane::pressCollapse(visage::Point pixels) {
	if (!toggleHit(pixels)) {
		return false;
	}
	// Announced the way the mock announces it — `notify('Post-FX hidden' / 'Post-FX shown')` — except
	// that the lane has no readout of its own to speak through yet, so it says nothing rather than
	// reaching for one. The editor's notify tray belongs to the panels that were given it.
	toggleCollapsed();
	return true;
}

FxLane::Part FxLane::partAt(visage::Point pixels) const {
	// THE TOGGLE BEFORE ANYTHING ELSE, and before the collapsed early-out below, because it is the
	// only part a closed rail has: everything under it is a card that is not being drawn.
	if (toggleHit(pixels)) {
		return Part::Collapse;
	}
	if (collapsed()) {
		return Part::None;
	}
	if (addBounds().contains(pixels)) {
		return Part::Add;
	}
	return Part::None;
}

visage::Bounds FxLane::addBounds() const {
	// UNDER THE LAST CARD — or under the empty-chain placeholder, which is the row the first device
	// will take. Either way it is where pressing it puts something, which is what makes the button a
	// picture of what it does rather than a control floating beside the thing it changes.
	//
	// AND IT MOVES WITH THE SCROLL, because it is content: it is the last row of the lane and on a
	// full chain it is the row you have to scroll to reach. cardBounds already carries the offset, so
	// deriving from it rather than from kPadTop is what keeps the button under the hand that sees it.
	const size_t devices = deviceCount();
	const visage::Bounds last = devices > 0 ? cardBounds(devices - 1) : emptySlotBounds();
	return { kPadX, last.bottom() + kSlotGap, width() - 2.0f * kPadX, kAddHeight };
}

std::optional<size_t> FxLane::dropGapAt(visage::Point position) const {
	const size_t rows = deviceCount();
	if (rows == 0) {
		return std::nullopt;
	}
	// OUTSIDE THE LANE IS THE CANCEL. Inside it, above the first card or below the last is the END of
	// the chain rather than nothing — the hand aimed past the list because that is where it wanted to
	// go, which a gap can say and a card index could not.
	if (position.x < 0.0f || position.x >= width() || position.y < 0.0f || position.y >= height()) {
		return std::nullopt;
	}
	std::vector<RowSpan> spans;
	spans.reserve(rows);
	for (size_t slot = 0; slot < rows; ++slot) {
		const visage::Bounds box = cardBounds(slot);
		spans.push_back({ box.y(), box.bottom() });
	}
	return gapAlong(position.y, spans);
}

visage::Bounds FxLane::gapBounds(size_t gap) const {
	const size_t rows = deviceCount();
	if (gap > rows) {
		return {};
	}
	const float thickness = 2.0f;
	float y = 0.0f;
	if (gap < rows) {
		y = cardBounds(gap).y() - kSlotGap * 0.5f - thickness * 0.5f;
	}
	else {
		y = cardBounds(rows - 1).bottom() + kSlotGap * 0.5f - thickness * 0.5f;
	}
	return { kPadX, y, width() - 2.0f * kPadX, thickness };
}

std::optional<size_t> FxLane::dragSlot() const {
	if (!dragging_ || !dragDevice_.has_value()) {
		return std::nullopt;
	}
	for (size_t slot = 0; slot < deviceCount(); ++slot) {
		if (bridge_.fxDeviceAt(slot) == *dragDevice_) {
			return slot;
		}
	}
	return std::nullopt;
}

void FxLane::cardDragForTest(size_t slot, FxCard::DragPhase phase, visage::Point inCard) {
	// THE CONVERSION IS THE LANE'S, and it is the whole reason the card reports in its own
	// coordinates: only the lane knows where a card sits in it, and the card's own y moves with the
	// scroll. A case that made up lane coordinates would be testing its own arithmetic.
	const visage::Bounds box = cardBounds(slot);
	const visage::Point inLane { box.x() + inCard.x, box.y() + inCard.y };

	switch (phase) {
	case FxCard::DragPhase::Begin:
		dragDevice_ = slot < deviceCount() ? std::optional<DeviceInstanceId>(bridge_.fxDeviceAt(slot)) : std::nullopt;
		dragging_ = dragDevice_.has_value();
		dragPoint_ = inLane;
		break;
	case FxCard::DragPhase::Cancel:
		dragDevice_.reset();
		dropGap_.reset();
		dragging_ = false;
		redraw();
		break;
	case FxCard::DragPhase::Move:
		if (!dragging_) {
			return;
		}
		dragPoint_ = inLane;
		dropGap_ = dropGapAt(inLane);
		redraw();
		break;
	case FxCard::DragPhase::End: {
		// THE SLOT IS RESOLVED NOW, from the device the gesture began on: the chain may have been reordered or shortened
		// under the hand, and a device that is gone moves nothing (nor does a neighbour that took its place).
		const std::optional<size_t> from = dragSlot();
		const std::optional<size_t> gap = dropGap_;
		dragDevice_.reset();
		dropGap_.reset();
		dragging_ = false;
		const DeviceInstanceId dragged = from.has_value() ? bridge_.fxDeviceAt(*from) : kNoInstance;
		if (gap.has_value() && selection_->movesBlock(dragged)) {
			// **A SELECTED CARD CARRIES THE SELECTION** (backlog OA, D1): the block lands at the gap, in order, one
			// undo step.
			if (selection_->verbs().moveBlock(selection_->selection().inOrder(laneOrder()), *gap)) {
				afterVerb();
			}
		}
		else if (from.has_value() && gap.has_value()) {
			// THE TWO GAPS EITHER SIDE OF THE DRAGGED CARD ANSWER NOTHING — a drop where the card
			// already is moves nothing rather than moving by zero, which would still spend an undo
			// step. See drop_gap.h.
			const std::optional<size_t> to = rowForGap(*from, *gap, deviceCount());
			if (to.has_value()) {
				moveDevice(*from, *to);
			}
		}
		redraw();
		break;
	}
	}
}

bool FxLane::tickDrag() {
	if (!dragging_) {
		return false;
	}
	if (!dragSlot().has_value()) {
		// THE DEVICE UNDER THE HAND IS GONE (removed, or the chain was replaced): nothing is being dragged.
		dragDevice_.reset();
		dropGap_.reset();
		dragging_ = false;
		redraw();
		return false;
	}
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	if (dragPoint_.x < 0.0f || dragPoint_.x >= w || dragPoint_.y < 0.0f || dragPoint_.y >= h) {
		return false; // outside the lane is the cancel (`dropGapAt` answers nothing there), not a scroll
	}
	float to = scroll_;
	if (dragPoint_.y < kEdgeScrollBand) {
		to -= kEdgeScrollStep;
	}
	else if (dragPoint_.y >= h - kEdgeScrollBand) {
		to += kEdgeScrollStep;
	}
	else {
		return false;
	}
	const float before = scroll_;
	setScroll(to);
	if (scroll_ == before) {
		return false; // already at that end
	}
	// THE DROP TARGET FOLLOWS THE HAND: the cards moved under a still pointer, so the gap under it is a
	// different one now. `setScroll` laid the cards out again; the gap is read off the new layout.
	dropGap_ = dropGapAt(dragPoint_);
	redraw();
	return true;
}

std::vector<const device::Descriptor*> FxLane::addableForTest() const {
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& d : kDevices) {
		// AN AUDIO DEVICE, OR WHAT THE STRIP OFFERS — `ModPanel::addableForTest`'s own pair of questions, so the two
		// menus cannot disagree about which modulators a hand is offered.
		if (maySitInLane(d) && (maySitInAudioChain(d) || (maySitInModRack(d) && belongsInModulationRack(d)))) {
			out.push_back(&d);
		}
	}
	return out;
}

size_t FxLane::addChoiceForTest(size_t index) {
	const std::vector<const device::Descriptor*> offered = addableForTest();
	if (index >= offered.size()) {
		return kNoPosition;
	}
	return addDevice(offered[index]->traits.tag);
}

size_t FxLane::addDevice(const char* tag) {
	const size_t slot = bridge_.addFxDevice(tag);
	reserveCards(bridge_.fxDeviceCount()); // a card for every device, at once (unbounded devices)
	if (slot == kNoPosition) {
		// REFUSED, AND SAID SO. A full lane is the case a hand will actually meet, and a button that
		// answered a press with nothing at all is the one thing worse than a disabled one.
		if (notifier_ != nullptr) {
			notifier_->say("Chain full");
		}
		return slot;
	}
	if (notifier_ != nullptr) {
		const device::Descriptor* desc = deviceForTag(tag);
		notifier_->say(std::string(desc != nullptr ? desc->traits.name : "Device") + " added");
	}
	// RECORDED AS THE DEVICE THAT WAS ACTUALLY MADE, serial and all, rather than as "an add of tag T".
	// A redo that re-added would mint a FRESH serial and hand the player a device with a new identity;
	// re-INSERTING the recorded one survives a round trip. See fx_edits.h.
	if (history_ != nullptr && slot != kNoPosition) {
		const FxChain chain = bridge_.fxChain();
		history_->record(std::make_unique<DeviceSlotEdit>(bridge_, DeviceLane::Fx, slot,
		                                                 fxDeviceAtPosition(chain, slot).row,
		                                                 std::vector<backplane::Route> {}, true));
	}
	// LAID OUT, NOT JUST REDRAWN, and the difference is the whole of the defect where a new card did not
	// appear until something else (a scroll, say) disturbed the UI. resized() is what BUILDS the new slot's card
	// (ensureCard runs there), gives it a size and a place, and makes it visible; redraw() alone marks
	// this frame stale and the lane duly repaints around a card that does not exist yet.
	resized();
	// AND SCROLLED TO, BEFORE THE REDRAW. `resized()` is what builds the card and gives it a place, so
	// its bounds are only knowable after it — and the scroll changes those bounds again, which is why
	// `redraw()` comes last rather than in the middle.
	//
	// A CARD YOU CANNOT SEE ALL OF IS A CONTROL YOU CANNOT USE, now that the card's principal control
	// is a picture: a bar sits where its VALUE puts it, so the part of a plot below the lane is a
	// range of values no hand can reach. See `scrollCardIntoView`.
	scrollCardIntoView(slot);
	redraw();
	return slot;
}

std::vector<FxLane::SlotOption> FxLane::slotOptionsForTest(size_t slot) const {
	// THE ENDS CANNOT MOVE FURTHER OUT, and the rows are DISABLED rather than absent — a menu whose
	// entries shuffle depending on which card you pressed teaches nothing and puts Remove under a
	// different finger every time. The filter card's slope menu makes the same choice for the same
	// reason.
	const size_t devices = deviceCount();
	// "ADD BEFORE" FIRST AND REMOVE LAST — the constructive verb at the top, the destructive one under
	// the same finger it has always been under. Greyed when the chain is full, like the (+).
	const bool room = slot < devices; // the lane grows (unbounded devices): there is always room
	return { { SlotVerb::AddBefore, room },
		     { SlotVerb::AddAfter, room },
		     { SlotVerb::MoveUp, slot > 0 && slot < devices },
		     { SlotVerb::MoveDown, devices > 0 && slot + 1 < devices },
		     { SlotVerb::Remove, slot < devices } };
}

bool FxLane::slotChoiceForTest(size_t slot, SlotVerb verb) {
	for (const SlotOption& option : slotOptionsForTest(slot)) {
		if (option.verb != verb) {
			continue;
		}
		if (!option.enabled) {
			return false;
		}
		switch (verb) {
		case SlotVerb::AddBefore:
			// THE PICKER, NOT AN INSERT: choosing the verb opens the catalogue at this card; the
			// device is chosen there. `insertChoiceForTest` is the whole gesture for a case.
			showAddMenu(slot);
			return true;
		case SlotVerb::AddAfter:
			// THE SAME PICKER AT THE SAME CARD; the pick goes behind it (`insertDeviceAfter`).
			showAddMenu(slot, true);
			return true;
		case SlotVerb::MoveUp:
			return moveDevice(slot, slot - 1);
		case SlotVerb::MoveDown:
			return moveDevice(slot, slot + 1);
		case SlotVerb::Remove:
			return removeDeviceAtLane(slot);
		}
	}
	return false;
}

void FxLane::focusSlot(size_t slot) {
	// THE FOCUS IS A SELECTION OF ONE (backlog OA, D1): the flag every card draws is the selection's.
	if (slot < deviceCount()) {
		const std::array<DeviceInstanceId, 1> one { bridge_.fxDeviceAt(slot) };
		selection_->select(one);
	}
	else {
		selection_->clear();
	}
	focusedSlot_ = slot;
}

bool FxLane::focusDevice(DeviceInstanceId device) {
	const size_t slot = bridge_.fxSlotOf(device);
	if (slot == kNoPosition) {
		return false;
	}
	focusSlot(slot);
	if (slot < cards_.size() && cards_[slot] != nullptr) {
		cards_[slot]->requestKeyboardFocus();
	}
	return true;
}

bool FxLane::removeDeviceAtLane(size_t slot) {
	// THROUGH THE BRIDGE'S ROAD, which removes the device from the record and every route naming it
	// with it. Shortening a copy of the chain here and restoring it would be a second road, and a second
	// road is a second chance to leave a departed device's routes behind.
	const device::Descriptor* desc = nullptr;
	const FxChain chain = bridge_.fxChain();
	if (slot < chain.device.size()) {
		desc = deviceForTag(fxDeviceAtPosition(chain, slot).tag().data());
	}
	// LIFTED BEFORE THE REMOVAL, because afterwards they do not exist. Removing a device takes every
	// route naming it — the honest answer, and the reason an undo has to have taken a copy.
	const FxDevice removed = fxDeviceAtPosition(chain, slot);
	std::vector<backplane::Route> routes = bridge_.routesOf(bridge_.fxDeviceAt(slot));
	if (!bridge_.removeFxDevice(slot)) {
		return false;
	}
	if (history_ != nullptr) {
		history_->record(std::make_unique<DeviceSlotEdit>(bridge_, DeviceLane::Fx, slot, removed.row,
		                                                 std::move(routes), false));
	}
	if (notifier_ != nullptr) {
		notifier_->say(std::string(desc != nullptr ? desc->traits.name : "Device") + " removed");
	}
	// REBUILT, NOT JUST REDRAWN, and here it matters more than on the add: the devices BELOW the hole
	// have all moved down a slot, so every card from `slot` on is now the face of a different device.
	// ensureCard compares each slot's tag against what its card was built for and rebuilds the ones
	// that changed, which is why the lane can be told "something moved" rather than what.
	resized();
	redraw();
	return true;
}

bool FxLane::moveDevice(size_t from, size_t to) {
	if (!bridge_.moveFxDevice(from, to)) {
		return false;
	}
	// UNDOABLE FOR THE SAME REASON THE OTHER TWO ARE: a reorder is the sound, and can delete routes it
	// makes backwards, so it is a structural mutation and not a setting. The cheapest to record — a move
	// is its own inverse, so the record is two slot numbers.
	if (history_ != nullptr) {
		history_->record(std::make_unique<DeviceMoveEdit>(bridge_, DeviceLane::Fx, from, to));
	}
	if (notifier_ != nullptr) {
		notifier_->say(from < to ? "Moved later" : "Moved earlier");
	}
	resized();
	redraw();
	return true;
}

void FxLane::showSlotMenu(size_t slot) {
	if (slot >= deviceCount()) {
		return;
	}

	// A MENU RATHER THAN A BARE REMOVE BUTTON, AND THE MENU IS THE CONFIRMATION. Decided here rather
	// than left implicit, because a remove is destructive and there is no undo behind it: undoing one
	// means putting a device back AND unremapping every routing column above the hole, and half of
	// that inside an undo record is worse than none.
	//
	// SO THE PROTECTION IS THE GESTURE'S SHAPE. Two deliberate presses at two different places — the
	// dots, then the row — cannot happen by accident, which is what a confirmation dialog buys and
	// this costs nothing extra: the menu has to exist anyway to hold the two reorder verbs. A modal
	// "are you sure" on top of that would be a third press for the same guarantee, and this editor
	// has no modal vocabulary to spend on it.
	//
	// THIS IS A DESIGN CHOICE, NOT A CONSTRAINT. If a remove wants a confirm, the place to add it is here.
	const std::vector<SlotOption> options = slotOptionsForTest(slot);
	visage::PopupMenu menu;
	for (size_t i = 0; i < options.size(); ++i) {
		const char* label = "Remove";
		if (options[i].verb == SlotVerb::AddBefore) {
			label = "Add before…";
		}
		else if (options[i].verb == SlotVerb::AddAfter) {
			label = "Add after…";
		}
		else if (options[i].verb == SlotVerb::MoveUp) {
			label = "Move earlier";
		}
		else if (options[i].verb == SlotVerb::MoveDown) {
			label = "Move later";
		}
		auto& option = menu.addOption(static_cast<int>(i), label);
		option.enable(options[i].enabled);
	}
	// AND THE SELECTION'S VERBS (backlog OA): over the selection when it holds this card, else this card alone.
	if (slot < deviceCount()) {
		selection_->addVerbs(menu, kVerbIds, bridge_.fxDeviceAt(slot));
	}
	menu.onSelection() = [this, slot, options](int id) {
		if (id >= kVerbIds) {
			(void)slotVerbForTest(slot, static_cast<FragmentVerb>(id - kVerbIds));
		}
		else if (id >= 0 && static_cast<size_t>(id) < options.size()) {
			slotChoiceForTest(slot, options[static_cast<size_t>(id)].verb);
		}
	};
	// OPENED AT THE DOTS AND NOT OFF THE BOTTOM OF THE WINDOW — see menuAnchorFor.
	showMenuAt(std::move(menu), *this, menuAnchorFor(slot));
}

size_t FxLane::insertChoiceForTest(size_t slot, size_t index) {
	const std::vector<const device::Descriptor*> offered = addableForTest();
	if (index >= offered.size() || slot >= deviceCount()) {
		return kNoPosition;
	}
	// THE SAME CAPTURE THE PICKER MAKES: the card's DEVICE at the moment the gesture starts.
	return insertDeviceBefore(bridge_.fxDeviceAt(slot), offered[index]->traits.tag);
}

size_t FxLane::insertDeviceBefore(DeviceInstanceId anchor, const char* tag) {
	const size_t slot = bridge_.insertFxDeviceBefore(anchor, tag);
	reserveCards(bridge_.fxDeviceCount());
	if (slot == kNoPosition) {
		if (notifier_ != nullptr) {
			notifier_->say("Chain full");
		}
		return slot;
	}
	if (notifier_ != nullptr) {
		const device::Descriptor* desc = deviceForTag(tag);
		notifier_->say(std::string(desc != nullptr ? desc->traits.name : "Device") + " added");
	}
	// ONE UNDO STEP, AND THE SAME RECORD AN APPEND MAKES: the device that was actually made, at the
	// position it landed. Undo takes it out of that position; redo re-inserts it there — `insertFxDevice`
	// takes a position up to the count, so an insert and an append are one record shape.
	if (history_ != nullptr) {
		const FxChain chain = bridge_.fxChain();
		history_->record(std::make_unique<DeviceSlotEdit>(bridge_, DeviceLane::Fx, slot,
		                                                 fxDeviceAtPosition(chain, slot).row,
		                                                 std::vector<backplane::Route> {}, true));
	}
	// LAID OUT, SCROLLED TO, THEN REDRAWN — the same three, in the same order, as `addDevice`.
	resized();
	scrollCardIntoView(slot);
	redraw();
	return slot;
}

size_t FxLane::insertDeviceAfter(DeviceInstanceId anchor, const char* tag) {
	const size_t devices = deviceCount();
	for (size_t at = 0; at < devices; ++at) {
		if (bridge_.fxDeviceAt(at) == anchor) {
			return at + 1 < devices ? insertDeviceBefore(bridge_.fxDeviceAt(at + 1), tag) : addDevice(tag);
		}
	}
	return kNoPosition; // the card the hand meant is gone
}

void FxLane::showAddMenu(size_t before, bool after) {
	// THE DEVICES THE LANE WILL TAKE, ASKED OF THE DEVICES. Not a list kept here: the descriptor's own ops
	// pointers and tags decide, so a device joins this menu by being able to render audio or by calling itself
	// a modulator — the strip's offer, since the lane runs modulators too.
	if (addableForTest().empty()) {
		return;
	}
	visage::PopupMenu menu = addMenuForTest(before, after);
	// AT THE (+) FOR AN APPEND; AT THE CARD'S DOTS FOR AN INSERT, where the hand already is.
	// (The category headings open first; their submenus hang off the row a hand opens.)
	const bool inserting = before < deviceCount();
	// Placed by `showMenuAt` against the row it hangs off, at the size visage measured for it.
	showMenuAt(std::move(menu), *this, menuAnchorFor(inserting ? before : kNoPosition));
}

visage::PopupMenu FxLane::addMenuForTest(size_t before, bool after) {
	const std::vector<const device::Descriptor*> offered = addableForTest();
	visage::PopupMenu menu;
	// FILED BY CATEGORY, one submenu per heading (`device_menu.h` has the rules every add menu shares).
	// Every row is live: the lane grows (unbounded devices step 6), so there is no "full" to grey for.
	addCategorisedDevices(menu, offered, [](size_t) { return true; });
	// THE ANCHOR IS CAPTURED NOW, AS THE CARD'S DEVICE — its identity, not the position the menu was
	// drawn at (backlog CR, by design). A card that moves between this open and the pick
	// is still the card the hand meant.
	const bool inserting = before < deviceCount();
	const DeviceInstanceId anchor = inserting ? bridge_.fxDeviceAt(before) : kNoInstance;
	menu.onSelection() = [this, offered, inserting, anchor, after](int id) {
		if (id >= 0 && static_cast<size_t>(id) < offered.size()) {
			const char* tag = offered[static_cast<size_t>(id)]->traits.tag;
			if (inserting && after) {
				insertDeviceAfter(anchor, tag);
			}
			else if (inserting) {
				insertDeviceBefore(anchor, tag);
			}
			else {
				addDevice(tag);
			}
		}
	};
	return menu;
}

visage::Bounds FxLane::menuAnchorFor(size_t slot) const {
	// A MENU THAT OPENS DOWNWARD OFF THE LAST ROW IS UNREACHABLE WHEN THE LANE IS SCROLLED TO ITS END,
	// and this lane's two menus are BOTH on rows that can be at the bottom of the window — the (+) is
	// the last row by construction, and a slot menu belongs to whichever card the hand pressed.
	//
	// ASKED AS GEOMETRY RATHER THAN LEFT TO THE POPUP, because it is the same overflow this lane has
	// already been caught by once: a rectangle placed past the window's edge is hit-testable by
	// arithmetic and unreachable by a hand. The popup is a window-level thing, so what it must fit
	// inside is the WINDOW, not the lane.
	//
	// FLIPPED ABOVE THE ROW rather than clamped to the bottom edge. Clamping would put the menu over
	// the control that opened it, so the row a hand is aiming at is covered by the row it lands on.
	//
	// **THE FLIP IS `showMenuAt`'s NOW (`menu_anchor.h`)**, done with the size visage measured rather
	// than one estimated here — this lane was one of two that had worked around visage's own flip,
	// which lands a menu near the window's top. What stays here is the ROW the menu hangs off:
	// the card's head for a slot, the (+) for an append.
	if (slot >= deviceCount()) {
		return addBounds();
	}
	const visage::Bounds card = cardBounds(slot);
	return { card.x(), card.y(), card.width(), FxCard::kSlotHeadHeight };
}

visage::Bounds FxLane::headingBounds() const {
	// PAST THE TOGGLE, which on this side shares the heading's end of the head row. Rail works out
	// where that is, because which end the toggle sits at is Rail's business and not this lane's.
	//
	// AND IT SCROLLS WITH THE CHAIN, as the modulator strip's does. The alternative — a pinned
	// heading — would have the first card slide underneath it and draw over the words.
	const float left = railHeadingLeft(kPadX);
	return { left, kPadTop - scroll_, width() - left - kPadX, kHeadingHeight };
}

visage::Bounds FxLane::cardBounds(size_t slot) const {
	// THE GENERAL ROAD, ASKED VERTICALLY. `FxCard::laneExtent(cross)` is what a lane asks whichever way
	// it runs; this lane is vertical, so the cross extent is the card's width and the answer is its
	// height. `laneExtent`'s default IS `laneHeight()`, and `GenericCard`'s vertical branch computes
	// exactly what `openHeight()` did — so this is the same number by construction, taken from the road
	// that also serves the rack's horizontal lane rather than from the one that only serves this.
	const float cross = width() - 2.0f * kPadX;
	float top = kPadTop + kHeadingHeight + kSlotGap;
	for (size_t above = 0; above < slot && above < cards_.size(); ++above) {
		if (cards_[above] != nullptr) {
			top += cards_[above]->laneExtent(cross) + kSlotGap;
		}
	}
	const float height =
	    slot < cards_.size() && cards_[slot] != nullptr ? cards_[slot]->laneExtent(cross) : 0.0f;
	// THE SCROLL IS IN THE GEOMETRY, so the hit test, the card's placement and the drawing all follow
	// it from one place rather than three that must agree.
	return { kPadX, top - scroll_, width() - 2.0f * kPadX, height };
}

float FxLane::contentHeight() const {
	// SUMMED RATHER THAN MULTIPLIED, because two devices' cards have different heights and either can
	// fold. This is the number the scroll is clamped against, so a fold that did not reach it would
	// leave the lane able to scroll past its own end.
	float total = kPadTop + kHeadingHeight + kSlotGap;
	if (deviceCount() == 0) {
		total += kEmptySlotHeight + kSlotGap;
	}
	const float cross = width() - 2.0f * kPadX;
	for (const auto& card : cards_) {
		if (card != nullptr) {
			total += card->laneExtent(cross) + kSlotGap;
		}
	}
	// AND THE (+) IS IN THE SUM. A row the scroll extent does not know about is a row a full lane
	// cannot be scrolled far enough to reach — the modulator strip's own recorded bug, one lane over,
	// and it is worth restating rather than rediscovering: the (+) is the LAST row, so it is the one
	// with nothing below it to hide a shortfall. The heading's own padding is repeated at the bottom,
	// so the lane ends the way it begins rather than with a border against the frame.
	return total + kAddHeight + kSlotGap + kPadTop;
}

float FxLane::maxScroll() const {
	return std::max(0.0f, contentHeight() - height());
}

void FxLane::scrollCardIntoView(size_t slot) {
	if (slot >= deviceCount()) {
		return;
	}
	// `cardBounds` ALREADY CARRIES THE SCROLL OFFSET, so these are positions in the lane's own view
	// and the deltas below are what to add to `scroll_` rather than absolute targets.
	const visage::Bounds box = cardBounds(slot);
	if (box.bottom() > height()) {
		// BOTTOM FIRST, then top — so a card TALLER than the lane ends up showing its top rather than
		// its bottom. The head is where the fold and the expand live, and a card whose head is off
		// screen looks like no card at all.
		setScroll(scroll_ + (box.bottom() - height()));
	}
	const visage::Bounds after = cardBounds(slot);
	if (after.y() < 0.0f) {
		setScroll(scroll_ + after.y());
	}
}

void FxLane::setScroll(float y) {
	const float clamped = std::clamp(y, 0.0f, maxScroll());
	if (clamped == scroll_) {
		return;
	}
	scroll_ = clamped;
	// THE CARDS ARE CHILD FRAMES, which is what makes this more than a drawing offset: they are
	// PLACED, so the scroll has to move them itself. A lane that slid its geometry while leaving the
	// frames where they were would scroll the borders and not the knobs.
	resized();
	redraw();
}

bool FxLane::mouseWheel(const visage::MouseEvent& e) {
	if (maxScroll() <= 0.0f) {
		// NOT CONSUMED when there is nothing to scroll, so the event goes on to whatever else wants
		// it rather than being swallowed by a lane that did nothing with it.
		return false;
	}
	setScroll(scroll_ - e.precise_wheel_delta_y * kScrollNotch);
	return true;
}

void FxLane::resized() {
	// REBUILT BEFORE PLACED, because this is the frame the lane gets whenever anything about it has
	// changed — and what the chain holds is one of the things that can have changed.
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		ensureCard(slot);
	}
	// THE SELECTION'S OVERLAY OVER EVERY CARD (backlog OA) — a card built above was added after it. It ignores the
	// mouse, so lifting it cannot drop a press being delivered; re-added only when it is not already last.
	if (selectionOverlay_ != nullptr) {
		if (indexOfChild(selectionOverlay_.get()) != static_cast<int>(children().size()) - 1) {
			removeChild(selectionOverlay_.get());
			addChild(selectionOverlay_.get());
		}
		selectionOverlay_->setBounds(0, 0, width(), height());
	}
	// A CARD REBUILT FOR A DEVICE draws the selection's flag like the rest.
	if (selection_ != nullptr) {
		selection_->prune();
		applySelection();
	}
	// AND THE SCROLL RE-CLAMPED BEFORE ANYTHING IS PLACED, because what just changed may have been the
	// content's height: a card folding, a device being removed, or the lane itself being resized. A
	// scroll left past the new end would place every card above the top of the lane. Assigned rather
	// than pushed through setScroll, which calls this.
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		if (cards_[slot] == nullptr) {
			continue;
		}
		const visage::Bounds box = cardBounds(slot);
		cards_[slot]->setBounds(box.x(), box.y(), box.width(), box.height());
		// SIZED WHETHER SHOWN OR NOT, the discipline the modulator strip already follows: a zero-size
		// child frame is one setVisible away from painting at the window origin.
		//
		// AND SCROLLED OUT IS NOT VISIBLE. This rule read `!collapsed() && slot < deviceCount()` and
		// the SCROLL was not in it, so a card that had left the top of the lane still answered
		// `isVisible()` true — and `FrameWatchers::notify()` redraws every visible frame under every
		// watcher, so each state notify repainted every card in the chain including the ones nobody
		// could see. MEASURED with seven seated devices, five of them at y = -1574 .. -332 and all
		// reporting visible: the same block-rate workload took 56% longer in wall clock than with one
		// device, and hiding the cards (by collapsing the lane, which is the one road that reached
		// this rule) made it 4.4x faster while telling the SAME 138 frames.
		//
		// `cardBounds` already carries the scroll offset, so this is the lane's own view and a card
		// straddling either edge is still shown.
		//
		// NOTHING ELSE HAS TO REMEMBER THIS. `setScroll` calls `resized()`, so the predicate is
		// recomputed on every scroll; `Frame::setVisible(true)` calls `redraw()` and `setDrawing`
		// walks the subtree marking each child, so a card scrolled back into view repaints itself and
		// its knobs without anybody asking. The card stays SIZED throughout, so its layout is not
		// rebuilt by coming back.
		const bool seated = !collapsed() && slot < deviceCount();
		const bool onScreen = box.bottom() > 0.0f && box.y() < height();
		cards_[slot]->setVisible(seated && onScreen);
	}
	// **AFTER THE CARDS, BECAUSE AN ARM FOLLOWS ITS CARD** — both its rectangle, which is the strip the
	// card just reserved at its new size, and its visibility, which is the card's own. Placing the
	// arms first would put them where the cards were last layout.
	layOutOutputArms();
}

visage::Bounds FxLane::emptySlotBounds() const {
	const float top = kPadTop + kHeadingHeight + kSlotGap;
	return { kPadX, top - scroll_, width() - 2.0f * kPadX, kEmptySlotHeight };
}

void FxLane::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.rectangle(0.0f, 0.0f, width(), height());
	// The mock's `border-left:1px solid var(--line)` — the hairline is on the INBOARD edge of this
	// lane, where the left rails put theirs on their outboard one. Both rules separate the lane from
	// the stage rather than from the window.
	canvas.setColor(kLine);
	canvas.fill(0.0f, 0.0f, 1.0f, height());

	// CLOSED IS A WHOLE DRAWING, the same rule the other two rails follow: the label goes upright,
	// the chain is not drawn at all, and what is left is the strip and the way back out of it.
	if (collapsed()) {
		drawRailChrome(canvas);
		return;
	}

	const visage::Bounds heading = headingBounds();
	canvas.setColor(kMuted);
	drawTracked(canvas, "POST-FX", font(kControlSize), kControlTracking, heading.x(), heading.y(),
	            heading.height());
	drawRailChrome(canvas);

	// THE SCROLL INDICATOR, on the sound rail's terms: only while there is something to scroll,
	// because a track that is always drawn is furniture rather than information.
	if (maxScroll() > 0.0f) {
		const float thumbHeight = std::max(24.0f, height() * (height() / contentHeight()));
		const float thumbY = (height() - thumbHeight) * (scroll_ / maxScroll());
		canvas.setColor(withAlpha(kLine, 0.9f));
		canvas.roundedRectangle(width() - 4.0f, thumbY, 2.0f, thumbHeight, 1.0f);
	}

	// THE BUMPER, if a card is being dragged — drawn before the (+) so it sits under nothing, and
	// drawn by the LANE because a gap belongs to the list rather than to either card beside it.
	if (dragging_ && dropGap_.has_value()) {
		const visage::Bounds bumper = gapBounds(*dropGap_);
		if (bumper.height() > 0.0f) {
			canvas.setColor(kValue);
			canvas.roundedRectangle(bumper.x(), bumper.y(), bumper.width(), bumper.height(), 1.0f);
		}
	}

	// THE (+), DRAWN IN BOTH CASES — a chain with devices and a chain with none. An empty lane is
	// exactly where a player most needs it, and the early return below used to be the last thing this
	// function did.
	//
	// AN OUTLINE RATHER THAN A FILLED CARD, because it is a VERB and not a thing in the chain. Dimmed
	// and still drawn when the lane is full: a control that vanished at the bound would leave a
	// player wondering where it went, and the menu behind it still says what the devices are.
	const visage::Bounds add = addBounds();
	const bool laneFull = false; // the lane grows (unbounded devices step 6)
	canvas.setColor(withAlpha(kLine, laneFull ? 0.35f : 0.75f));
	canvas.roundedRectangleBorder(add.x(), add.y(), add.width(), add.height(), kCornerRadius, 1.0f);
	canvas.setColor(laneFull ? kDisabled : kValue);
	// GEOMETRY RATHER THAN A GLYPH, the rule the harmonic arrows are drawn by: this font has no
	// guarantee of a "+" that centres, and a missing glyph is an invisible control.
	const float plus = 7.0f;
	const float cx = add.x() + kPadX + plus * 0.5f;
	const float cy = add.y() + add.height() * 0.5f;
	canvas.segment(cx - plus * 0.5f, cy, cx + plus * 0.5f, cy, 1.4f, true);
	canvas.segment(cx, cy - plus * 0.5f, cx, cy + plus * 0.5f, 1.4f, true);
	canvas.text(laneFull ? "Chain full" : "Add effect", font(kKnobReadoutSize), visage::Font::kLeft,
	            cx + plus, add.y(), add.width(), add.height());

	// A CHAIN WITH A DEVICE IN IT DRAWS NO PLACEHOLDER. The card is a child frame and draws itself;
	// what the lane owes it is a place to stand, which resized() gave it.
	if (deviceCount() > 0) {
		return;
	}

	// THE EMPTY CHAIN, in the mock's own words and in the voice this editor already uses for a thing
	// that is present but not yet real — the CV card's "2 lanes · waiting on a cable". Drawn dimmed
	// and answering nothing, which is the honest state of a chain with no devices: the alternative is
	// a live-looking control that does nothing, which is the one thing the honesty rule refuses.
	const visage::Bounds slot = emptySlotBounds();
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.roundedRectangleBorder(slot.x(), slot.y(), slot.width(), slot.height(), kCornerRadius,
	                              1.0f);

	canvas.setColor(kDisabled);
	const float line = 18.0f;
	const float first = slot.y() + (slot.height() - 3.0f * line) * 0.5f;
	canvas.text("empty chain", font(kControlSize), visage::Font::kCenter, slot.x(), first,
	            slot.width(), line);
	// ASCII, DELIBERATELY. This line held an arrow (U+2192) and visage falls back to its EMOJI
	// rasterizer for any character the main font lacks — where a missing glyph rendered as .notdef in
	// 8-bit grey and was copied as 32-bit BGRA, reading off the end of an 18-byte FreeType bitmap.
	// AddressSanitizer, 2026-08-31.
	//
	// THE FORK IS PATCHED AND THIS IS STILL WORTH DOING. The patch makes an unrenderable character
	// draw NOTHING rather than garbage, so the arrow would have quietly disappeared — and a label
	// that says "top bottom" is worse than one that says "top to bottom". Fixing the crash and
	// keeping the sentence are different jobs.
	canvas.text("effects dock here, top to bottom", font(kKnobReadoutSize), visage::Font::kCenter,
	            slot.x(), first + line, slot.width(), line);
	canvas.text("· survives any centre topology ·", font(kKnobReadoutSize), visage::Font::kCenter,
	            slot.x(), first + 2.0f * line, slot.width(), line);
}

} // namespace feathers::ui

namespace feathers::ui {

void FxLane::reserveCards(size_t count) {
	if (count <= cards_.size()) {
		return;
	}
	const size_t want = std::max(count, 2 * cards_.size());
	cards_.resize(want);
	cardWatches_.resize(want);
	cardTag_.resize(want);
	cardDevice_.resize(want);
}

} // namespace feathers::ui
