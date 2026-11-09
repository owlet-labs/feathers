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

#include "plugin/ui/chain_column.h"
#include "plugin/ui/menu_anchor.h"

#include "plugin/device_registry.h"
#include "plugin/ui/card_factory.h"
#include "plugin/ui/device_menu.h"
#include "plugin/ui/drop_gap.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"
// FOR `rack::kMaxDepth` — the SAVE FORMAT's nesting bound, which is the one this column stops at.
#include "rack/rack_device.h"

#include <algorithm>
#include <string>
#include <vector>

namespace feathers::ui {

namespace {

/// **A DEVICE CHANGING PLACES IN A HELD CHAIN** — `DeviceMoveEdit`, over a `rack::RackBridge` rather
/// than Feathers' `ParamBridge`, and BY THE DEVICE'S ID rather than by two slot numbers: the chain is
/// keyed by identity since kill-lanes, so an undo names the device that was moved and puts it back
/// where it stood, whatever has been added or removed around it since.
///
/// A REFERENCE TO THE BRIDGE AND NOTHING OF THE COLUMN — edit_history.h's rule, and the same shape as
/// the value and route records this column's cards already write (`DeviceValueEdit`, over the same
/// bridge). **ALL OF THEM SHARE ONE OPEN QUESTION**: the history is the RAIL's and survives a rebuild
/// of the column onto a new bridge (trap T9), so a record could outlive the chain it names. Nothing
/// walks that history today — no key or button undoes inside a lane — so it is latent, not live.
class ChainMoveEdit final : public EditRecord {
public:
	ChainMoveEdit(rack::RackBridge& bridge, DeviceInstanceId device, size_t from, size_t to)
	    : bridge_(bridge), device_(device), from_(from), to_(to) {}

	void undo() const override { (void)bridge_.moveDeviceTo(device_, from_); }
	void redo() const override { (void)bridge_.moveDeviceTo(device_, to_); }
	const char* verb() const override { return "Reorder"; }

private:
	rack::RackBridge& bridge_;
	DeviceInstanceId device_ = kNoInstance;
	size_t from_ = 0;
	size_t to_ = 0;
};

} // namespace

ChainColumn::ChainColumn(rack::RackBridge& bridge, WidgetRegistry& registry, ModState& mod,
                         EditHistory& history, Notifier* notifier)
    : bridge_(bridge), registry_(registry), mod_(mod), history_(history), notifier_(notifier) {
	// **DELETE AND BACKSPACE, THE SAME AS BOTH LANES'.** The gesture belongs to effects, not to
	// one host — a gesture that works in the fx lane and not here is worse than one that works in
	// neither, because the hand learns it and then it fails somewhere.
	//
	// A KEY NOBODY WANTS IS NOT SWALLOWED: `false` hands it on.
	setAcceptsKeystrokes(true);
	// **THE SELECTION OVER THIS COLUMN'S CHAIN** (backlog OA, D1) — the chain editor's own `LaneSelection`.
	LaneSelection::Hooks selectionHooks;
	selectionHooks.order = [this] { return chainOrder(); };
	selectionHooks.changed = [this] { applySelection(); };
	selectionHooks.cardBounds = [this](DeviceInstanceId id) -> std::optional<visage::Bounds> {
		FxCard* card = cardOf(id);
		if (card == nullptr || !card->isVisible()) {
			return std::nullopt;
		}
		return card->bounds();
	};
	selection_ = std::make_unique<LaneSelection>(bridge_, history_, notifier_, std::move(selectionHooks));
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
		if (focused_ == kNoInstance) {
			return false;
		}
		// **FOCUS GOES FIRST.** The id is the device's own, so unlike a slot index it cannot come to name
		// a different device after the removal — but a focus left pointing at a device that has gone is a
		// second press answering nothing, which reads as a stuck key.
		const DeviceInstanceId going = focused_;
		focusDevice(kNoInstance);
		return removeDevice(going);
	};
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (partAt(e.position) == Part::Add) {
			showAddMenu();
		}
		else {
			// THE BARE COLUMN: a lasso (backlog OA). A click that does not drag clears the selection.
			selection_->lassoBegin(e.position, e.isShiftDown());
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
	// **BUILT BEFORE THE FIRST SYNC, BECAUSE A CARD'S `onExpand` REACHES THROUGH IT.** It holds nothing
	// until `setPresentedOn` gives it an outer, and offers nothing either: with no outer and no query
	// installed, no card draws a mark and nothing is ever presented.
	surfaces_ = std::make_unique<SurfaceLayers>();
	surfaces_->setModulationState(&mod_);
	installSourceList();
	// **A RELABELLED OUTPUT RENAMES ITS CHIP AND RE-LAYS THE COLUMN** — the arms here are kept across a
	// relabel (they are rebuilt only when the stack changes), so the word is handed to the one arm, and
	// the layout measures it again. Ended first in the destructor: it captures `this`.
	//
	// **THE LAYOUT RUNS WHETHER OR NOT AN ARM WAS FOUND**, as `RackEditor`'s does. The arm is not the
	// only reader of the word: a card's strip reserves its chip ROWS from the labels
	// (`outputChipRowsFor`), so a longer name can make a card taller before any arm for it exists, and
	// one pass per relabel — a hand's edit or a load — is nothing to pay for not having to prove which
	// relabels move nothing.
	labelSubscription_ = bridge_.subscribeOutputLabel([this](DeviceInstanceId device, size_t output) {
		if (SourceArm* arm = outputArmOf(device, output); arm != nullptr) {
			arm->setLabel(bridge_.outputLabel(device, output));
		}
		resized();
		redraw();
	});
	// NOT A CHILD FRAME. Its two layers stay empty for this column's whole life — every present is
	// forwarded — and a full-window layer parented into a 300px rail would be a frame that can only
	// ever be wrong about its own size. `RackEditor` adds its own because its takeover really does show
	// the device browser there; this one has no local customer.
	syncCards();
}

ChainColumn::~ChainColumn() {
	// NO RELABEL IS HEARD INTO A COLUMN THAT IS GOING — the bridge outlives it.
	labelSubscription_.reset();
	// **A PANEL THIS COLUMN PRESENTED OUTLIVES THE COLUMN UNLESS IT IS TAKEN DOWN HERE**, and it reads
	// a device of a bridge that is going with the frame that owns this one. `RackEditor`'s destructor
	// does the same thing for the same reason. The dismissal runs through this column's OWN layers, so
	// `forgetFrameTree` takes the panel's knobs off `mod_`'s watcher list NOW rather than whenever the
	// outer overlay gets round to freeing the mount — which is after the next `draw`, by which time
	// `mod_` may be gone.
	if (surfaces_ != nullptr) {
		const std::vector<SurfaceId> up = expanded_;
		for (const SurfaceId id : up) {
			surfaces_->dismiss(id);
		}
		expanded_.clear();
	}
	// **OFF THE WATCHER LIST BEFORE THEY DIE.** `Knob::setModulation` puts every knob on `ModState`'s
	// watcher list and that list holds RAW FRAME POINTERS; the next `arm()` walks them. Four places in
	// Feathers do this and the rack's editor is the fifth — a card stack that skipped it is exactly the
	// shape of the three use-after-frees that shipped through a green `ctest`.
	for (CardEntry& entry : cards_) {
		if (entry.card != nullptr) {
			forgetFrameTree(entry.card.get(), nullptr, nullptr, &mod_.watchers());
		}
	}
	// **AND THE ARMS, WHICH ARE ON THAT SAME WATCHER LIST AND ARE NOT UNDER ANY CARD.** They are
	// children of the column (see `OutputArm`), so the card walk above cannot reach them — each one
	// takes itself off the list in `~SourceArm`, through the watch token.
	//
	// **CLEARED IN THIS BODY RATHER THAN LEFT TO THE MEMBER'S OWN DESTRUCTION, which is an ordering
	// fact and not tidiness.** Members are destroyed after this body and before the `visage::Frame`
	// base, so leaving it would tear the arms down at a point where the column is half-destroyed and
	// its child list still names them. Doing it here is the same discipline the card walk above
	// follows, for the same reason.
	outputArms_.clear();
	// **AND THE TWO CLOSURES THIS COLUMN LEFT ON A STATE IT DOES NOT OWN.** `mod_` is a reference and
	// outlives this frame; a lister that captured `this` and stayed installed would be called by the
	// route menu of whatever surface came next, reading a destroyed bridge. `RackEditor`'s destructor
	// clears its own pair for the same reason and says so.
	mod_.setSourceLister(nullptr);
	mod_.setSourceNamer(nullptr);
}

void ChainColumn::installSourceList() {
	// **WHAT SOURCES THIS CHAIN HAS, FOR THE INBOUND-ROUTE MENU — the road this column was extracted
	// without** (backlog HM: right-clicking a param in the fx lane did not show its mapped mod
	// sources' context menu).
	//
	// **IT IS THE SAME OMISSION AS THE MISSING OUTPUT ARMS, ONE DOOR ALONG, and this file already said
	// so**: `ChainColumn` was extracted out of `RackEditor`'s card half and neither passenger came with
	// it. `ui::showRouteMenu` refuses an EMPTY menu so the press falls through to its ordinary road —
	// which is right, and which is exactly why the absence was silent: with no lister,
	// `ModState::sources()` is *"an empty list, which offers no menu rather than a guessed one"*, so
	// `inboundSources` could never find a route however many were live, and the gesture answered
	// nothing without ever looking wrong.
	//
	// **SHARED WITH THE RACK EDITOR BY SHAPE, NOT BY BODY.** `RackEditor::RackEditor` installs the same
	// two closures over ITS engine; the list is "this host's core sources, then every output of every
	// device in the chain, by endpoint", and both hosts answer it through their own seam. Lifting the
	// body would mean naming a type that is neither host's.
	//
	// ASKED LIVE, because the chain changes under it: a device added a moment ago is a source now.
	mod_.setSourceLister([this] {
		std::vector<backplane::Source> out;
		// **THE HOLDER'S OWN, WHICH IS WHAT THIS CHAIN'S CORE SOURCES ARE.** They carry no widget here
		// (see the banner's "what a player cannot do here yet"), but a ROUTE from one is ordinary and
		// the menu is how a hand reaches one that exists — which is the whole gesture.
		for (size_t i = 0; i < bridge_.axes().coreSources; ++i) {
			out.push_back(backplane::Source::core(i));
		}
		for (size_t position = 0; position < deviceCount(); ++position) {
			const DeviceInstanceId device = deviceAt(position);
			if (const device::Descriptor* d = bridge_.descriptorOf(device); d != nullptr) {
				for (size_t output = 0; output < d->outputCount(); ++output) {
					out.push_back(backplane::Source::output(device, output));
				}
			}
		}
		return out;
	});
	// **AND WHAT TO CALL THEM.** A menu row is read before it is picked, so a list of ordinals would
	// make you choose one to find out which one you meant.
	mod_.setSourceNamer([this](backplane::Source source) -> std::string {
		if (!source.isDevice()) {
			// **THE HOLDER NAMES ITS OWN OUTPUTS, OR NOBODY DOES.** This column knows the devices IN the
			// chain and not the device the chain is in; `setCoreSourceNamer` is how the surface that does
			// says. Unset falls back to the ordinal rather than to a guess — the same honest refusal
			// `ModState::sourceName` makes with no namer at all.
			return coreNamer_ ? coreNamer_(source.index) : "Source " + std::to_string(source.index + 1);
		}
		const device::Descriptor* d = bridge_.descriptorOf(source.device);
		if (d == nullptr) {
			return "Source"; // an id nothing holds names nothing
		}
		std::string name = d->traits.name;
		if (const size_t position = bridge_.positionOf(source.device);
		    position != rack::RackBridge::kNoSlot) {
			name += " ";
			name += std::to_string(position + 1);
		}
		if (d->outputCount() > 1 && source.index < d->outputCount()) {
			name += " \xc2\xb7 "; // " · "
			name += bridge_.outputLabel(source.device, source.index); // as this instance calls it
		}
		return name;
	});
}

void ChainColumn::setCoreSourceNamer(std::function<std::string(size_t)> namer) {
	coreNamer_ = std::move(namer);
}

size_t ChainColumn::deviceCount() const {
	return bridge_.chainLength(0);
}

DeviceInstanceId ChainColumn::deviceAt(size_t position) const {
	return bridge_.deviceAtPosition(position, 0);
}

ChainColumn::CardEntry* ChainColumn::entryOf(DeviceInstanceId device) noexcept {
	for (CardEntry& entry : cards_) {
		if (entry.device == device) {
			return &entry;
		}
	}
	return nullptr;
}

const ChainColumn::CardEntry* ChainColumn::entryOf(DeviceInstanceId device) const noexcept {
	for (const CardEntry& entry : cards_) {
		if (entry.device == device) {
			return &entry;
		}
	}
	return nullptr;
}

FxCard* ChainColumn::cardOf(DeviceInstanceId device) const noexcept {
	const CardEntry* entry = entryOf(device);
	return entry != nullptr ? entry->card.get() : nullptr;
}

std::vector<FxCard*> ChainColumn::cardsForTest() const {
	std::vector<FxCard*> out;
	for (size_t position = 0; position < deviceCount(); ++position) {
		if (FxCard* card = cardOf(deviceAt(position)); card != nullptr) {
			out.push_back(card);
		}
	}
	return out;
}

size_t ChainColumn::cardCountForTest() const noexcept {
	size_t n = 0;
	for (const CardEntry& entry : cards_) {
		n += entry.card != nullptr ? 1 : 0;
	}
	return n;
}

void ChainColumn::syncCards() {
	// COMPARE BEFORE BUILDING, `RackEditor::syncCards`' rule: a card is keyed by its DEVICE and not by
	// where the device stands, so a reorder builds nothing and no card is rebuilt under a live press.
	bool changed = false;
	const auto missingTagOf = [this](DeviceInstanceId device, const device::Descriptor* desc) {
		return desc == nullptr ? bridge_.heldTagOf(device) : device::Tag {};
	};
	// FIRST, EVERY CARD WHOSE DEVICE HAS LEFT THE CHAIN OR BECOME ANOTHER, RETIRED.
	for (auto it = cards_.begin(); it != cards_.end();) {
		const device::Descriptor* desc = bridge_.descriptorOf(it->device);
		const bool held = bridge_.positionOf(it->device) != rack::RackBridge::kNoSlot;
		if (held && it->desc == desc && it->missing == missingTagOf(it->device, desc)) {
			++it;
			continue;
		}
		if (it->card != nullptr) {
			// FORGET IT BEFORE DESTROYING IT — see the destructor.
			forgetFrameTree(it->card.get(), nullptr, nullptr, &mod_.watchers());
			removeChild(it->card.get());
		}
		if (it->device == focused_) {
			focused_ = kNoInstance;
		}
		it = cards_.erase(it);
		changed = true;
	}
	// THEN A CARD FOR EVERY DEVICE THE CHAIN HOLDS THAT HAS NONE.
	for (size_t position = 0; position < deviceCount(); ++position) {
		const DeviceInstanceId i = deviceAt(position);
		if (i == kNoInstance || entryOf(i) != nullptr) {
			continue;
		}
		const device::Descriptor* desc = bridge_.descriptorOf(i);
		const device::Tag missing = missingTagOf(i, desc);
		std::unique_ptr<FxCard> card;
		if (desc != nullptr) {
			// THE DEVICE'S OWN CARD IF IT HAS ONE ON THE SEAM, the generic grid otherwise — the SAME
			// selection both other hosts use, so the three cannot disagree about which UI a device gets.
			card = makeCustomCard(bridge_, registry_, i, *desc, notifier_, &mod_, &history_);
			if (card == nullptr) {
				card = std::make_unique<GenericCard>(bridge_, i, *desc, notifier_);
			}
		}
		else if (missing[0] != '\0') {
			card = std::make_unique<MissingDeviceCard>(std::string_view(missing.data(), missing.size()));
		}
		if (card != nullptr) {
			card->setModulation(&mod_, &history_);
			// **FOLDED AS THE PATCH LEFT IT, BEFORE THE HEAD MARKS ARE TAKEN FROM IT.** The fold is the
			// device's and the bridge holds it, so a card rebuilt for a device that was folded comes back
			// folded. Order matters by one line: `setHeadMarks` reads `card->folded()`.
			card->setFolded(bridge_.deviceFolded(i));
			wireCard(*card, i);
			addChild(card.get());
		}
		// RECORDED EVEN WITH NO CARD (a device with neither a descriptor nor a tag), so it is not
		// "arriving" again on every sync.
		cards_.push_back(CardEntry { i, desc, missing, std::move(card) });
		changed = true;
	}
	if (changed) {
		// **THE ARMS BEFORE THE LAYOUT, BECAUSE THE LAYOUT IS WHAT PLACES THEM.** `resized()` walks the
		// arms that exist; building them after it would leave a freshly added device's chips at the
		// origin until the next unrelated layout pass.
		rebuildOutputArms();
		// A CARD BUILT OR GONE: the selection drops what the chain no longer holds, every card draws its flag, and
		// the overlay goes back above the cards (backlog OA). It ignores the mouse, so lifting it drops no press.
		selection_->prune();
		applySelection();
		if (indexOfChild(selectionOverlay_.get()) != static_cast<int>(children().size()) - 1) {
			removeChild(selectionOverlay_.get());
			addChild(selectionOverlay_.get());
		}
		resized();
		redraw();
		return;
	}
	// **A REORDER BUILDS NOTHING, SO NOTHING ABOVE NOTICES ONE** — and a reorder this column did not
	// make (an undo: its record holds the bridge, not this frame) would leave every card where the old
	// order put it. Compared against the order last placed, element by element, so a beat with nothing
	// new allocates nothing.
	const size_t devices = deviceCount();
	bool reordered = laidOrder_.size() != devices;
	for (size_t position = 0; !reordered && position < devices; ++position) {
		reordered = laidOrder_[position] != deviceAt(position);
	}
	if (reordered) {
		resized();
		redraw();
	}
}

SourceArm* ChainColumn::outputArmOf(DeviceInstanceId device, size_t output) const noexcept {
	for (const OutputArm& entry : outputArms_) {
		if (entry.device == device && entry.output == output) {
			return entry.arm.get();
		}
	}
	return nullptr;
}

visage::Bounds ChainColumn::outputArmBoundsForTest(DeviceInstanceId device,
                                                   size_t output) const noexcept {
	const SourceArm* arm = outputArmOf(device, output);
	return arm != nullptr ? arm->bounds() : visage::Bounds {};
}

void ChainColumn::rebuildOutputArms() {
	// **REBUILT WHOLESALE RATHER THAN PATCHED** — `RackEditor::syncCards`' rule, and its reason is
	// this column's too: an arm names its source by ENDPOINT (the device's id and its output number),
	// which no reorder or removal can re-point, so throwing them all away and asking the chain again is
	// both correct and cheap. It runs only when the card stack changed.
	for (OutputArm& entry : outputArms_) {
		if (entry.arm != nullptr) {
			removeChild(entry.arm.get());
		}
	}
	// **CLEARED BEFORE THE NEW ONES ARE BUILT, so the destructors run while `mod_` is certainly alive.**
	// `~SourceArm` releases the watch token, which is what takes it off `mod_`'s watcher list — the list
	// `notify()` walks holding raw frame pointers, and the crash once hit on the arrow.
	outputArms_.clear();
	for (size_t position = 0; position < deviceCount(); ++position) {
		const DeviceInstanceId i = deviceAt(position);
		const device::Descriptor* desc = bridge_.descriptorOf(i);
		if (i == kNoInstance || desc == nullptr) {
			continue;
		}
		for (size_t output = 0; output < desc->outputCount(); ++output) {
			// LABELLED, exactly as both other hosts label their chips. A device publishing two outputs
			// draws two identical glyphs and nothing but the name says which is which.
			// **BY THE LABEL THIS INSTANCE GIVES IT** (`RackBridge::outputLabel`) — the declared name until a
			// hand relabels it; a relabel after this is heard by `labelSubscription_`.
			OutputArm entry;
			entry.device = i;
			entry.output = output;
			// **THE ENDPOINT IS THIS CHAIN'S OWN DEVICE ID, WHICH IS WHAT KEEPS THE OFFER HONEST.** A
			// route written from this arm is written through `bridge_` — the holder's `InnerChain` — so
			// the source it names is resolved inside the same authority that will run it. An outer id
			// here would be an arm offering a route the engine refuses.
			entry.arm = std::make_unique<SourceArm>(mod_, backplane::Source::output(i, output),
			                                        bridge_.outputLabel(i, output));
			// **AND A PRESS ON THE CHIP SELECTS THE CARD IT SITS ON.** The chip is on top of the card, so
			// `FxCard::onCardPressed` never hears this press; `SourceArm::onPressed` exists for exactly
			// this, and the report behind it was a lit card with a DIFFERENT card deleted.
			entry.arm->onPressed = [this, i] { focusDevice(i); };
			// **ABOVE THE CARD IT SHARES A RECTANGLE WITH, STATED RATHER THAN INHERITED FROM CALL
			// ORDER.** The card frame covers the whole slot including this strip, and `syncCards`
			// `addChild`s a card whenever its device changes — which puts the card at the END of the
			// child list and therefore on top of its own arrows.
			entry.arm->setOnTop(true);
			addChild(entry.arm.get());
			outputArms_.push_back(std::move(entry));
		}
	}
}

void ChainColumn::layOutOutputArms() {
	for (size_t position = 0; position < deviceCount(); ++position) {
		const DeviceInstanceId i = deviceAt(position);
		const FxCard* card = cardOf(i);
		const device::Descriptor* desc = bridge_.descriptorOf(i);
		if (card == nullptr || desc == nullptr || desc->outputCount() == 0) {
			continue;
		}
		// **GATHERED INTO OUTPUT ORDER, which is the order the chips come back in and the only order
		// the two can be matched by.** Sized to what is DECLARED and to what is THERE, so an arm the
		// descriptor no longer has an output for is still handed to the walk and hidden by it rather
		// than left visible where it last was.
		size_t count = desc->outputCount();
		for (const OutputArm& entry : outputArms_) {
			if (entry.arm != nullptr && entry.device == i) {
				count = std::max(count, entry.output + 1);
			}
		}
		std::vector<SourceArm*> arms(count, nullptr);
		for (const OutputArm& entry : outputArms_) {
			if (entry.arm != nullptr && entry.device == i) {
				arms[entry.output] = entry.arm.get();
			}
		}
		layOutCardOutputArms(*card, arms);
	}
	// **AN ARM WHOSE DEVICE HAS LEFT THE CHAIN IS HIDDEN, not left where it was.** `syncCards` rebuilds
	// them on the very next beat, and a chip drawn in between would point at a device that is gone —
	// the same one-frame window `resized()` already closes for the cards themselves.
	for (const OutputArm& entry : outputArms_) {
		if (entry.arm != nullptr && bridge_.positionOf(entry.device) == rack::RackBridge::kNoSlot) {
			entry.arm->setVisible(false);
		}
	}
}

void ChainColumn::wireCard(FxCard& card, DeviceInstanceId device) {
	// **THE HEAD MARKS ARE THE FOLD STATE AND NOTHING ELSE.** `CardHeadMarks::menu` is the pair of
	// dots the FRAME draws; this column's slot verbs are on the card's OWN three dots
	// (`FxCard::slotMenuBounds`), which is what `onSlotMenu` below is bound to. The expand is not a
	// head mark at all — it is `FxCard::expandBounds`, which the card derives from the HOST's answer
	// to "can you open a panel for this device". That is why the mark can appear here without one line
	// of geometry: `setPresentedOn` changes what the host says.
	//
	// Order matters by one line: `setHeadMarks` reads `card.folded()`.
	card.setHeadMarks({ false, card.folded() });
	// THE CARD WEARS THE CHAIN'S NEST, so its tint is this chain's and not the outer host's — two
	// devices that are both instance 1 in two different chains are two colours (`DeviceNest`).
	card.setNest(nest_);
	card.onSlotMenu = [this, device] { showSlotMenu(device); };
	// **THE SELECTION'S TWO ROADS** (backlog OA, D1; `LaneSelection`'s banner).
	card.onCardPressed = [this, device] { selection_->focusPress(device); };
	card.onCardSelectPress = [this, device](CardSelection::Press kind) { selection_->cardPress(device, kind); };
	card.onCardClicked = [this, device] { selection_->cardClick(device); };
	// **AND THE DRAG, WHICH IS THE HOOK THIS COLUMN WAS BUILT WITHOUT** (backlog JA, 2026-09-23:
	// effects in the fx lanes could not be reordered). The card's own road — `noteCardMove` past the
	// threshold, `noteCardRelease` at the end — has always run here and reported to nobody: a card in
	// GRAIN FX or POST FX lifted and went back where it was. `FxLane`, `ModPanel` and `RackEditor` all
	// bind it; this was the fourth lane and the only one that did not. Bound by DEVICE, because a card
	// here is its device's and a position would name somebody else after the first reorder.
	card.onCardDrag = [this, device](FxCard::DragPhase phase, visage::Point inCard) {
		cardDragForTest(device, phase, inCard);
	};
	card.onHeadFold = [this, c = &card, device] {
		c->setFolded(!c->folded());
		c->setHeadMarks({ false, c->folded() });
		bridge_.setDeviceFolded(device, c->folded());
		resized();
		redraw();
	};
	// **AND THE FOLD BY ANY OTHER ROAD, WHICH IS A SEPARATE HOOK AND WAS THE ONE NOT HELD** (backlog
	// HM). `FxCard::setFolded` fires `onFoldChanged` and nothing else; with no installer here the card
	// shrank its contents and kept its RECTANGLE, and the cards below it did not move — the bug where a
	// collapsed effect's UI went away but its card kept its size, arriving on a
	// surface the original fix never reached. MEASURED before this line existed: through `setFolded`
	// the top card's height changed by ZERO and the card under it did not move at all.
	//
	// **THE HEAD ROAD WAS ALREADY RIGHT, AND THAT IS WHY THIS WAS INVISIBLE.** `onHeadFold` above calls
	// `resized()` itself, so the gesture a hand actually makes has always re-placed the stack; a sweep
	// that looked for `onFoldChanged` installers found none here and read that as the behaviour being
	// absent. An absent hook is not an absent behaviour — the case drives BOTH roads for that reason.
	//
	// **BOTH ARE KEPT, DELIBERATELY.** With this installed, `onHeadFold`'s own `resized()` is redundant
	// by construction — its first statement is a `setFolded`. It stays because it is what makes the
	// head road independent of this hook still being here tomorrow, and a duplicated layout pass costs
	// one walk over frames whose `setBounds` dedups.
	card.onFoldChanged = [this] {
		resized();
		redraw();
	};
	// **THE WAY IN.** Bound unconditionally, because the guard is not here: with no presenter the host
	// declines to have a panel, so `expandBounds` is empty and the press that would call this cannot be
	// made. Binding it on a condition read at BUILD time would be the stale kind of guard — a column
	// given a presenter after its cards were built would have live marks and dead arms.
	card.onExpand = [this, device] { expandDevice(device); };
	// AND THE SITE CHIP (backlog NR), bound the same way for the same reason.
	card.onSite = [this, device] { expandSite(device); };
	// AND A PRESS ON THE HEAD RAISES THIS DEVICE'S OPEN PANEL — the fx lane's verb, in this chain's
	// nest, through the layers the panel was presented on.
	card.onHeadPressed = [this, device] { (void)surfaces_->focusPanelFor(device, nest_); };
}

void ChainColumn::setPresentedOn(SurfaceLayers* host) {
	if (surfaces_->outerLayers() == host) {
		return;
	}
	// **WHAT IS UP GOES DOWN FIRST.** A panel presented through the old outer is held by that outer;
	// leaving it there while this column starts answering to another one is a panel nothing can
	// dismiss, over a bridge this column may be about to stop drawing.
	const std::vector<SurfaceId> up = expanded_;
	for (const SurfaceId id : up) {
		surfaces_->dismiss(id);
	}
	expanded_.clear();
	surfaces_->setOuterLayers(host);
	// AND THE SELECTION'S ESC ENTRY GOES ON THE OUTERMOST STACK WITH EVERYTHING ELSE (backlog OA, NN).
	selection_->setEscapeStack(host != nullptr ? &surfaces_->escapeStack() : nullptr);
	// **AND THE HOST'S ANSWER CHANGES WITH IT, WHICH IS THE WHOLE MECHANISM.** `RackEngine` declines
	// every descriptor until a query is installed, so a chain with nowhere to present draws no mark —
	// and one that gains a presenter draws the mark on the cards it already has, because
	// `FxCard::expandBounds` asks the host on every paint rather than caching an answer.
	//
	// THE TABLE IS THE SAME ONE `expandDevice` BUILDS FROM (`ui::hasExpandedView` against
	// `ui::makeExpandedView`), which is `RackEditor`'s own rule: the button and the panel cannot
	// disagree about which devices have one.
	if (host != nullptr) {
		bridge_.setExpandedViewQuery(&hasExpandedView);
	}
	else {
		bridge_.setExpandedViewQuery(nullptr);
	}
	redraw();
}

void ChainColumn::setNest(DeviceNest nest) {
	if (nest == nest_) {
		return;
	}
	nest_ = nest;
	for (CardEntry& entry : cards_) {
		if (entry.card != nullptr) {
			entry.card->setNest(nest_);
		}
	}
	redraw();
}

void ChainColumn::pruneExpanded() {
	std::erase_if(expanded_, [this](SurfaceId id) { return !surfaces_->holds(id); });
}

void ChainColumn::expandDevice(DeviceInstanceId device) {
	if (surfaces_->outerLayers() == nullptr) {
		return;
	}
	// A PRESS WHILE THIS DEVICE'S PANEL IS UP CLOSES IT, asked of the outermost
	// layers the panel was escalated to, in this chain's nest — the key it was filed under.
	if (surfaces_->dismissPanelFor(device, nest_)) {
		return;
	}
	// THE DESCRIPTOR IS RE-READ AT PRESS TIME, not captured — `RackEditor`'s note: a lambda holding one
	// would be one `removeDevice` away from opening a panel for a device that has gone.
	const device::Descriptor* desc = bridge_.descriptorOf(device);
	if (desc == nullptr) {
		return;
	}
	// **NULLPTR FOR THE PARAMETER BRIDGE MEANS "NOT ON THIS HOST", NOT "NO PANEL"** — the rack passes
	// none for the same reason and gets the same honest decline, with the generic fallback below.
	std::unique_ptr<visage::Frame> content =
	    makeExpandedView(bridge_, nullptr, registry_, device, *desc, notifier_);
	if (content != nullptr) {
		if (auto* view = dynamic_cast<ExpandedView*>(content.get()); view != nullptr) {
			view->setModulation(&mod_, &history_);
			// **THE PANEL IS STAMPED WITH THIS CHAIN'S NEST, AND THAT IS NOT DECORATION.**
			// `ExpandedOverlay` keys one panel per (title, nest, device); without the nest a device
			// that is instance 1 here and a device that is instance 1 in the host's own chain share a
			// key, and opening the second RETIRES the first. It is also what makes the panel's tint
			// the card's tint, since both read `instanceTintFor(device, nest)`.
			view->setNest(nest_);
		}
	}
	else {
		// THE GENERIC PANEL: the same card turned VERTICAL, which is the uncapped axis, so it shows the
		// device's whole control set rather than the columns a narrow rail allows.
		auto panel = std::make_unique<GenericCard>(bridge_, device, *desc, notifier_);
		panel->setLaneAxis(FxCard::LaneAxis::Vertical);
		panel->setModulation(&mod_, &history_);
		panel->setNest(nest_);
		panel->setBounds(0, 0, 480, static_cast<int>(panel->laneExtent(480.0f)));
		content = std::move(panel);
	}
	// **`SpansGesture`, NEVER A TAKEOVER**, for `RackEditor`'s reason one host over: a hand must be able
	// to arm a source and then click a control on this panel, and a takeover would put whatever it
	// armed from off-screen mid-gesture. The layers forward it to the outer, which is where a popup
	// floating over the whole editor comes from.
	const SurfaceId presented = surfaces_->present(std::move(content), SurfaceNeed::SpansGesture,
	                                               desc->traits.name, [this] { pruneExpanded(); });
	if (presented != kNoSurface) {
		expanded_.push_back(presented);
	}
}

void ChainColumn::expandSite(DeviceInstanceId device) {
	if (surfaces_->outerLayers() == nullptr) {
		return;
	}
	const DeviceInstanceId key = siteKeyOf(device);
	if (surfaces_->dismissPanelFor(key, nest_)) {
		return;
	}
	if (!bridge_.openSite(device)) {
		// A SITE IS A CHAIN BELOW THIS ONE: refused at the depth gate, and said so.
		if (!rack::fitsNesting(bridge_.nestingDepth(), 1) && notifier_ != nullptr) {
			notifier_->say(kNestingFull);
		}
		return;
	}
	std::unique_ptr<visage::Frame> content = makeExpandedView(bridge_, nullptr, registry_, key, siteViewDescriptor(), notifier_);
	if (content == nullptr) {
		return;
	}
	if (auto* view = dynamic_cast<ExpandedView*>(content.get()); view != nullptr) {
		view->setModulation(&mod_, &history_);
		view->setNest(nest_);
	}
	const SurfaceId presented = surfaces_->present(std::move(content), SurfaceNeed::SpansGesture,
	                                               siteViewDescriptor().traits.name, [this] { pruneExpanded(); });
	if (presented != kNoSurface) {
		expanded_.push_back(presented);
	}
}

size_t ChainColumn::updateCards() {
	size_t told = 0;
	for (const CardEntry& entry : cards_) {
		// VISIBLE ONLY, which is free and already load-bearing: `Frame::redraw()` refuses an invisible
		// frame anyway, and `resized()` below hides a card scrolled out of the column.
		if (entry.card != nullptr && entry.card->isVisible() && entry.card->update()) {
			++told;
		}
	}
	return told;
}

bool ChainColumn::full() const {
	// NEVER, since unbounded devices (step 6): the chain's record grows. Kept as a question because the draw and
	// the menus ask it, and the answer they need now is "no".
	return false;
}

std::vector<const device::Descriptor*> ChainColumn::addable() const {
	// **ASKED OF THE CHAIN, NOT LISTED HERE.** The build's table says what devices exist; the chain's own
	// catalogue says which of them it can run, and only the second is the honest menu — a chain adopts
	// its catalogue from whoever holds it and may know fewer devices than this process does.
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& d : kDevices) {
		if (bridge_.descriptorFor(d.traits.tag) != nullptr) {
			out.push_back(&d);
		}
	}
	return out;
}

bool ChainColumn::wouldNest(const device::Descriptor& desc) const noexcept {
	return desc.container != nullptr;
}

bool ChainColumn::nestingFull() const noexcept {
	// **THE HAND STOPS WHERE THE LOADER STOPS, AND NOT ONE LEVEL LATER.** `rack::kMaxDepth` is the
	// FORMAT's bound: `InnerChain::loadState` and `rack_device.cpp`'s both refuse a body at
	// `depth + 1 >= kMaxDepth`, all-or-nothing. A surface that let a player build deeper would be
	// offering an add whose result cannot be reloaded — which is the honesty rule failing at the one
	// moment a player would find out, days later, with the patch in their hands.
	//
	// **`nest_.depth` IS HOW MANY CHAINS DEEP THIS COLUMN ALREADY IS**, so a container added here holds
	// a chain at `depth + 1`, and the deepest one whose contents survive a save is `kMaxUsefulDepth`.
	// Written against that constant rather than against `kMaxDepth - 1`, because the off-by-one between
	// them is real and is exactly what a case caught me getting wrong from the other side.
	// AND THE MODEL'S OWN DEPTH (2026-10-31, `RackBridge::nestingDepth`): the deeper of the two, so the menu never
	// offers what the record would refuse. One gate, `rack::fitsNesting`, which the loader's test defines.
	return !rack::fitsNesting(std::max(static_cast<size_t>(nest_.depth), bridge_.nestingDepth()), 1);
}

DeviceInstanceId ChainColumn::addDevice(const char* tag, DeviceInstanceId before) {
	// **THE NESTING BOUND IS CHECKED HERE BECAUSE THIS IS WHERE REFUSALS ARE WORDED** — the one road
	// both the (+) and the card menu take. A guard only in `showAddMenu` would be a guard on one of the
	// two roads, which is how a bound becomes a suggestion.
	const device::Descriptor* wanted = bridge_.descriptorFor(tag);
	if (wanted != nullptr && wouldNest(*wanted) && nestingFull()) {
		if (notifier_ != nullptr) {
			notifier_->say(kNestingFull);
		}
		return kNoInstance;
	}
	const DeviceInstanceId added =
	    before != kNoInstance ? bridge_.addDeviceBefore(before, tag) : bridge_.addDevice(tag);
	if (added == kNoInstance) {
		// REFUSED, AND SAID SO WHERE THERE IS SOMEWHERE TO SAY IT. Silently doing nothing is the honesty
		// rule's failure case: the click looked like it worked.
		if (notifier_ != nullptr) {
			notifier_->say(before != kNoInstance && !full() ? "That device is gone; nothing added"
			                                                : "The chain is full");
		}
		return kNoInstance;
	}
	// LAID OUT, NOT JUST REDRAWN: `syncCards` is what BUILDS the new card, gives it a size and a place.
	// `redraw()` alone marks this frame stale and it duly repaints around a card that does not exist.
	syncCards();
	// AND SCROLLED TO, AFTER the layout that gave it a place — a card you cannot see all of is a control
	// you cannot use.
	scrollCardIntoView(bridge_.positionOf(added));
	redraw();
	return added;
}

DeviceInstanceId ChainColumn::addChoiceForTest(size_t index) {
	const std::vector<const device::Descriptor*> offered = addable();
	if (index >= offered.size()) {
		return kNoInstance;
	}
	return addDevice(offered[index]->traits.tag);
}

DeviceInstanceId ChainColumn::insertChoiceForTest(DeviceInstanceId anchor, size_t index) {
	const std::vector<const device::Descriptor*> offered = addable();
	if (index >= offered.size() || bridge_.positionOf(anchor) == rack::RackBridge::kNoSlot) {
		return kNoInstance;
	}
	return addDevice(offered[index]->traits.tag, anchor);
}

void ChainColumn::showAddMenu(DeviceInstanceId before, bool after) {
	if (addable().empty()) {
		return;
	}
	visage::PopupMenu menu = addMenuForTest(before, after);
	const size_t at = before != kNoInstance ? bridge_.positionOf(before) : rack::RackBridge::kNoSlot;
	showMenuAt(std::move(menu), *this, menuAnchorFor(at));
}

visage::PopupMenu ChainColumn::addMenuForTest(DeviceInstanceId before, bool after) {
	const std::vector<const device::Descriptor*> offered = addable();
	visage::PopupMenu menu;
	const bool atCapacity = full();
	// **THE CELEBRATION IS A ROW OF THE MENU, WHICH IS WHERE THE WALL IS.**
	//
	// `NotifyTray`'s own header rules it out as the home: *"It is transient by design. Anything that
	// needs to stay on screen belongs to a control."* Four sentences in a 1400 ms one-line tray in the
	// lower left is a joke a player would half-read and a refusal they would never connect to the
	// greyed row in front of them. A disabled row costs no layout, holds as long as the menu is open,
	// sits directly above the thing it is explaining, and is found by the exact gesture that hit the
	// wall. The tray still says it from `addDevice` for any OTHER road to a refusal.
	//
	// IT READS AS A REWARD RATHER THAN AN ERROR, deliberately, and the lower case is intended.
	// It is an easter egg AND an honest refusal, and it is only reachable by a player who
	// nested a device inside a device twenty times.
	if (nestingFull()) {
		menu.addOption(-1, kNestingFull).enable(false);
		menu.addBreak();
	}
	// FILED BY CATEGORY, one submenu per heading (`device_menu.h` has the rules every add menu shares) —
	// effects and modulators together, since a chain takes both.
	//
	// GREYED WHEN THE CHAIN IS FULL rather than the menu being withheld: a player who presses (+) on a
	// full chain should be told what they could have had and why they cannot.
	//
	// **AND GREYED PER DEVICE AT THE NESTING BOUND**, which is the same rule one axis over: at the
	// limit a SKLITTER is refused and a filter is not, so withholding the whole menu — or dropping the
	// container rows out of it, which an earlier draft of this did — would hide a wall rather than show
	// it. Dropping them also silently renumbered `addChoiceForTest`, which is the quieter half of why
	// listing and disabling is the right shape.
	const bool deepest = nestingFull();
	addCategorisedDevices(menu, offered, [&](size_t i) {
		return !atCapacity && !(wouldNest(*offered[i]) && deepest);
	});
	// **THE ANCHOR IS THE CARD'S DEVICE, CAPTURED NOW** (backlog CR) — its identity, not the position the
	// menu was drawn at. A card that moves between this open and the pick is still the card the hand meant.
	menu.onSelection() = [this, offered, before, after](int id) {
		if (id >= 0 && static_cast<size_t>(id) < offered.size()) {
			const char* tag = offered[static_cast<size_t>(id)]->traits.tag;
			if (after && before != kNoInstance) {
				addDeviceAfter(before, tag);
			}
			else {
				addDevice(tag, before);
			}
		}
	};
	return menu;
}

DeviceInstanceId ChainColumn::addDeviceAfter(DeviceInstanceId anchor, const char* tag) {
	const size_t at = bridge_.positionOf(anchor);
	if (at == rack::RackBridge::kNoSlot) {
		if (notifier_ != nullptr) {
			notifier_->say("That device is gone; nothing added");
		}
		return kNoInstance;
	}
	return addDevice(tag, at + 1 < deviceCount() ? deviceAt(at + 1) : kNoInstance);
}

std::vector<ChainColumn::SlotOption> ChainColumn::slotOptionsFor(DeviceInstanceId device) const {
	// THE ENDS CANNOT MOVE FURTHER OUT, and the rows are DISABLED rather than absent — a menu whose
	// entries shuffle depending on which card you pressed teaches nothing and puts Remove under a
	// different finger every time. `FxLane` makes the same choice for the same reason.
	const size_t at = bridge_.positionOf(device);
	const bool held = at != rack::RackBridge::kNoSlot;
	const size_t devices = deviceCount();
	const bool room = held; // the chain grows (unbounded devices step 6)
	return { { SlotVerb::AddBefore, room },
		     { SlotVerb::AddAfter, room },
		     { SlotVerb::MoveUp, held && at > 0 },
		     { SlotVerb::MoveDown, held && at + 1 < devices },
		     { SlotVerb::Remove, held } };
}

bool ChainColumn::slotChoiceForTest(DeviceInstanceId device, SlotVerb verb) {
	for (const SlotOption& option : slotOptionsFor(device)) {
		if (option.verb != verb) {
			continue;
		}
		if (!option.enabled) {
			return false;
		}
		const size_t at = bridge_.positionOf(device);
		switch (verb) {
		case SlotVerb::AddBefore:
			// THE PICKER, NOT AN INSERT: choosing the verb opens the catalogue at this card; the device
			// is chosen there. `insertChoiceForTest` is the whole gesture for a case.
			showAddMenu(device);
			return true;
		case SlotVerb::AddAfter:
			// THE SAME PICKER AT THE SAME CARD; the pick goes behind it (`addDeviceAfter`).
			showAddMenu(device, true);
			return true;
		case SlotVerb::MoveUp:
			return moveDeviceTo(device, at - 1);
		case SlotVerb::MoveDown:
			return moveDeviceTo(device, at + 1);
		case SlotVerb::Remove:
			return removeDevice(device);
		}
	}
	return false;
}

void ChainColumn::showSlotMenu(DeviceInstanceId device) {
	if (bridge_.positionOf(device) == rack::RackBridge::kNoSlot) {
		return;
	}
	// A MENU RATHER THAN A BARE REMOVE BUTTON, AND THE MENU IS THE CONFIRMATION — `FxLane`'s rule:
	// two deliberate presses at two different places cannot happen by accident, and the menu has to
	// exist anyway to hold the two reorder verbs.
	const std::vector<SlotOption> options = slotOptionsFor(device);
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
	selection_->addVerbs(menu, kVerbIds, device);
	menu.onSelection() = [this, device, options](int id) {
		if (id >= kVerbIds) {
			(void)slotVerbForTest(device, static_cast<FragmentVerb>(id - kVerbIds));
		}
		else if (id >= 0 && static_cast<size_t>(id) < options.size()) {
			slotChoiceForTest(device, options[static_cast<size_t>(id)].verb);
		}
	};
	showMenuAt(std::move(menu), *this, menuAnchorFor(bridge_.positionOf(device)));
}

bool ChainColumn::removeDevice(DeviceInstanceId device) {
	if (bridge_.positionOf(device) == rack::RackBridge::kNoSlot) {
		return false;
	}
	if (dragDevice_ == device) {
		dragDevice_.reset();
		dropGap_.reset();
	}
	bridge_.removeDevice(device);
	// **REAPED ON THE MESSAGE THREAD, WHICH IS WHERE WE ARE.** `removeDevice` parks the instance for the
	// drain; nothing else on this surface calls `reapRetired`, so a column that never reaped would leave
	// every removed device's storage alive for the life of the editor.
	bridge_.reapRetired();
	syncCards();
	redraw();
	return true;
}

bool ChainColumn::moveDeviceTo(DeviceInstanceId device, size_t to) {
	const size_t from = bridge_.positionOf(device);
	if (!bridge_.moveDeviceTo(device, to)) {
		return false;
	}
	// UNDOABLE FOR `FxLane::moveDevice`'s REASON: the order of a chain IS the sound, so a reorder is a
	// structural edit and not a setting. By the device's id — see `ChainMoveEdit`.
	history_.record(std::make_unique<ChainMoveEdit>(bridge_, device, from, to));
	if (notifier_ != nullptr) {
		notifier_->say(from < to ? "Moved later" : "Moved earlier");
	}
	// NO CARD IS REBUILT BY THIS — a card is keyed by its device — so this is a re-LAYOUT and nothing
	// more, which is exactly what makes a reorder safe under a live press.
	resized();
	redraw();
	return true;
}

std::optional<size_t> ChainColumn::dropGapAt(visage::Point position) const {
	const size_t devices = deviceCount();
	if (devices == 0) {
		return std::nullopt;
	}
	// OUTSIDE THE COLUMN IS THE CANCEL; above the first card or below the last is the END of the chain
	// rather than nothing. `FxLane::dropGapAt`'s rule, and its arithmetic.
	if (position.x < 0.0f || position.x >= static_cast<float>(width()) || position.y < 0.0f
	    || position.y >= static_cast<float>(height())) {
		return std::nullopt;
	}
	// **SPANS IN CHAIN ORDER** — `cardBounds` walks by position, which is draw order, so a gap index
	// and a chain position are the same number and `rowForGap` needs no translation.
	std::vector<RowSpan> spans;
	spans.reserve(devices);
	for (size_t p = 0; p < devices; ++p) {
		const visage::Bounds box = cardBounds(p);
		spans.push_back({ box.y(), box.bottom() });
	}
	return gapAlong(position.y, spans);
}

visage::Bounds ChainColumn::gapBounds(size_t gap) const {
	const size_t devices = deviceCount();
	if (devices == 0 || gap > devices) {
		return {};
	}
	constexpr float thickness = 2.0f;
	// CENTRED IN THE SPACE BETWEEN TWO CARDS rather than butted against one, which would read as
	// belonging to that card.
	const float y = gap < devices ? cardBounds(gap).y() - kSlotGap * 0.5f - thickness * 0.5f
	                              : cardBounds(devices - 1).bottom() + kSlotGap * 0.5f - thickness * 0.5f;
	return { kPadX, y, std::max(0.0f, width() - 2.0f * kPadX), thickness };
}

std::optional<size_t> ChainColumn::dropPositionAtWindow(visage::Point inWindow) const {
	const visage::Point p = inWindow - positionInWindow();
	if (p.x < 0.0f || p.x >= static_cast<float>(width()) || p.y < 0.0f || p.y >= static_cast<float>(height())) {
		return std::nullopt;
	}
	if (const std::optional<size_t> gap = dropGapAt(p); gap.has_value()) {
		return gap;
	}
	return deviceCount(); // an empty chain, or the column's foot: the end
}

visage::Bounds ChainColumn::cardBoundsForTest(DeviceInstanceId device) const {
	const size_t at = bridge_.positionOf(device);
	return at != rack::RackBridge::kNoSlot ? cardBounds(at) : visage::Bounds {};
}

void ChainColumn::cardDragForTest(DeviceInstanceId device, FxCard::DragPhase phase, visage::Point inCard) {
	const size_t at = bridge_.positionOf(device);
	if (at == rack::RackBridge::kNoSlot) {
		dragDevice_.reset();
		dropGap_.reset();
		return;
	}
	const visage::Bounds box = cardBounds(at);
	const visage::Point inColumn { box.x() + inCard.x, box.y() + inCard.y };

	switch (phase) {
	case FxCard::DragPhase::Begin:
		dragDevice_ = device;
		dragPoint_ = inColumn;
		break;
	case FxCard::DragPhase::Move:
		if (dragDevice_ != device) {
			return;
		}
		dragPoint_ = inColumn;
		dropGap_ = dropGapAt(inColumn);
		redraw();
		break;
	case FxCard::DragPhase::End: {
		const std::optional<DeviceInstanceId> from = dragDevice_;
		const std::optional<size_t> gap = dropGap_;
		dragDevice_.reset();
		dropGap_.reset();
		if (from == device && gap.has_value() && selection_->movesBlock(device)) {
			// **A SELECTED CARD CARRIES THE SELECTION** (backlog OA, D1): the block lands at the gap, in order.
			if (selection_->verbs().moveBlock(selection_->selection().inOrder(chainOrder()), *gap)) {
				afterVerb();
			}
		}
		else if (from == device && !gap.has_value() && onDropOutside_) {
			// OUTSIDE THE COLUMN: the owner may take it into another chain (backlog NQ). The selection travels when the card
			// carries it, as within the column.
			const std::vector<DeviceInstanceId> ids = selection_->movesBlock(device)
			                                              ? selection_->selection().inOrder(chainOrder())
			                                              : std::vector<DeviceInstanceId> { device };
			if (onDropOutside_(ids, positionInWindow() + inColumn)) {
				afterVerb();
			}
		}
		else if (from == device && gap.has_value()) {
			// THE TWO GAPS EITHER SIDE OF THE DRAGGED CARD ANSWER NOTHING — a drop where the card already
			// is moves nothing and spends no undo step. `rowForGap` is shared with the other lanes
			// rather than restated.
			if (const std::optional<size_t> to = rowForGap(at, *gap, deviceCount()); to.has_value()) {
				moveDeviceTo(device, *to);
			}
		}
		redraw();
		break;
	}
	}
}

bool ChainColumn::tickDrag() {
	if (!dragDevice_.has_value()) {
		return false;
	}
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	if (dragPoint_.x < 0.0f || dragPoint_.x >= w || dragPoint_.y < 0.0f || dragPoint_.y >= h) {
		return false; // outside the column is the cancel, not a scroll
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
	// THE DROP TARGET FOLLOWS THE HAND: the cards moved under a still pointer.
	dropGap_ = dropGapAt(dragPoint_);
	redraw();
	return true;
}

void ChainColumn::focusDevice(DeviceInstanceId device) {
	// THE FOCUS IS A SELECTION OF ONE (backlog OA, D1): the flag every card draws is the selection's.
	if (device == kNoInstance) {
		selection_->clear();
	}
	else {
		const std::array<DeviceInstanceId, 1> one { device };
		selection_->select(one);
	}
	focused_ = device;
}

std::vector<DeviceInstanceId> ChainColumn::chainOrder() const {
	std::vector<DeviceInstanceId> out;
	for (size_t p = 0; p < bridge_.chainLength(); ++p) {
		out.push_back(bridge_.deviceAtPosition(p));
	}
	return out;
}

void ChainColumn::applySelection() {
	const CardSelection& s = selection_->selection();
	for (const CardEntry& entry : cards_) {
		if (entry.card != nullptr) {
			entry.card->setFocused(s.contains(entry.device));
		}
	}
	focused_ = s.anchor();
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->redraw();
	}
}

void ChainColumn::afterVerb() {
	bridge_.reapRetired(); // `removeDevice`'s reason: nothing else on this surface reaps
	syncCards();
	selection_->prune();
	applySelection();
	redraw();
}

bool ChainColumn::slotVerbForTest(DeviceInstanceId device, FragmentVerb verb) {
	if (!selection_->chooseVerb(kVerbIds + static_cast<int>(verb), kVerbIds, device)) {
		return false;
	}
	afterVerb();
	return true;
}

visage::Bounds ChainColumn::cardBounds(size_t position) const {
	// THE GENERAL ROAD, ASKED VERTICALLY. `FxCard::laneExtent(cross)` is what a lane asks whichever way
	// it runs; this column is vertical, so the cross extent is the card's width and the answer is its
	// height. WALKED rather than indexed, because a card above this one may be folded.
	const float cross = std::max(0.0f, width() - 2.0f * kPadX);
	float top = kPadY;
	const size_t devices = deviceCount();
	for (size_t above = 0; above < position && above < devices; ++above) {
		if (const FxCard* card = cardOf(deviceAt(above)); card != nullptr) {
			top += card->laneExtent(cross) + kSlotGap;
		}
	}
	const FxCard* here = position < devices ? cardOf(deviceAt(position)) : nullptr;
	const float tall = here != nullptr ? here->laneExtent(cross) : 0.0f;
	// THE SCROLL IS IN THE GEOMETRY, so the hit test, the placement and the drawing all follow it from
	// one place rather than three that must agree.
	return { kPadX, top - scroll_, cross, tall };
}

float ChainColumn::contentHeight() const {
	// SUMMED RATHER THAN MULTIPLIED, because two devices' cards have different heights and either can
	// fold. This is the number the scroll is clamped against, so a fold that did not reach it would
	// leave the column able to scroll past its own end.
	float total = kPadY;
	const float cross = std::max(0.0f, width() - 2.0f * kPadX);
	for (size_t position = 0; position < deviceCount(); ++position) {
		if (const FxCard* card = cardOf(deviceAt(position)); card != nullptr) {
			total += card->laneExtent(cross) + kSlotGap;
		}
	}
	// AND THE (+) IS IN THE SUM — the modulator strip's own recorded bug: a row the scroll extent does
	// not know about is a row a full column cannot be scrolled far enough to reach.
	return total + kAddHeight + kPadY;
}

float ChainColumn::maxScroll() const {
	return std::max(0.0f, contentHeight() - static_cast<float>(height()));
}

void ChainColumn::setScroll(float y) {
	const float clamped = std::clamp(y, 0.0f, maxScroll());
	if (clamped == scroll_) {
		return;
	}
	scroll_ = clamped;
	// THE CARDS ARE CHILD FRAMES, which is what makes this more than a drawing offset: they are PLACED,
	// so the scroll has to move them itself.
	resized();
	redraw();
}

void ChainColumn::scrollCardIntoView(size_t position) {
	if (position >= deviceCount()) {
		return;
	}
	const visage::Bounds box = cardBounds(position);
	if (box.bottom() > static_cast<float>(height())) {
		// BOTTOM FIRST, then top — so a card TALLER than the column ends up showing its top rather than
		// its bottom. The head is where the fold and the slot menu live.
		setScroll(scroll_ + (box.bottom() - static_cast<float>(height())));
	}
	const visage::Bounds after = cardBounds(position);
	if (after.y() < 0.0f) {
		setScroll(scroll_ + after.y());
	}
}

bool ChainColumn::mouseWheel(const visage::MouseEvent& e) {
	if (maxScroll() <= 0.0f) {
		// NOT CONSUMED when there is nothing to scroll, so the event goes on to whatever else wants it.
		return false;
	}
	setScroll(scroll_ - e.precise_wheel_delta_y * kScrollNotch);
	return true;
}

visage::Bounds ChainColumn::addBounds() const {
	const size_t devices = deviceCount();
	const visage::Bounds last = devices > 0 ? cardBounds(devices - 1) : visage::Bounds {};
	const float top = devices > 0 ? last.bottom() + kSlotGap : kPadY - scroll_;
	return { kPadX, top, std::max(0.0f, width() - 2.0f * kPadX), kAddHeight };
}

ChainColumn::Part ChainColumn::partAt(visage::Point pixels) const {
	return addBounds().contains(pixels) ? Part::Add : Part::None;
}

visage::Bounds ChainColumn::menuAnchorFor(size_t position) const {
	// A MENU THAT OPENS DOWNWARD OFF THE LAST ROW IS UNREACHABLE WHEN THE COLUMN IS SCROLLED TO ITS END,
	// and both of this column's menus are on rows that can be at the bottom of the window. `FxLane`'s
	// arithmetic, and its reason: a rectangle placed past the window's edge is hit-testable by
	// arithmetic and unreachable by a hand. **The flip itself is `showMenuAt`'s now** (see
	// `FxLane::menuAnchorFor`); this names the row the menu hangs off — a card's head, or the (+).
	if (position >= deviceCount()) {
		return addBounds();
	}
	const visage::Bounds card = cardBounds(position);
	return { card.x(), card.y(), card.width(), FxCard::kSlotHeadHeight };
}

void ChainColumn::resized() {
	// THE SCROLL RE-CLAMPED BEFORE ANYTHING IS PLACED, because what just changed may have been the
	// content's height: a card folding, a device removed, or the column itself resized. Assigned rather
	// than pushed through `setScroll`, which calls this.
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->setBounds(0, 0, width(), height()); // backlog OA: the lasso and the pill
	}
	const size_t devices = deviceCount();
	laidOrder_.resize(devices);
	for (size_t position = 0; position < devices; ++position) {
		laidOrder_[position] = deviceAt(position);
		FxCard* card = cardOf(deviceAt(position));
		if (card == nullptr) {
			continue;
		}
		const visage::Bounds box = cardBounds(position);
		card->setBounds(box.x(), box.y(), box.width(), box.height());
		// **SIZED WHETHER SHOWN OR NOT, AND SCROLLED OUT IS NOT VISIBLE** — the fx lane's measured rule:
		// `FrameWatchers::notify()` redraws every VISIBLE frame under every watcher, so cards left
		// visible off the top of the lane cost a repaint each on every state notify (measured there at
		// 56% of a block-rate workload with seven devices seated).
		const bool onScreen = box.bottom() > 0.0f && box.y() < static_cast<float>(height());
		card->setVisible(onScreen);
	}
	// A CARD WHOSE DEVICE HAS LEFT is hidden rather than left where it was — `syncCards` retires it on
	// the very next beat, and a frame drawn in between would be a card for a device that is gone.
	for (const CardEntry& entry : cards_) {
		if (entry.card != nullptr && bridge_.positionOf(entry.device) == rack::RackBridge::kNoSlot) {
			entry.card->setVisible(false);
		}
	}
	// **AND THE OUTPUT CHIPS AFTER THE CARDS, because every one of them is placed FROM its card's
	// rectangle** — `layOutCardOutputArms` reads `card->bounds()`, so an arm laid out first would be
	// placed against the card's previous position and would sit a scroll notch away from it.
	layOutOutputArms();
}

void ChainColumn::draw(visage::Canvas& canvas) {
	// **NO SURFACE FILL AND NO HAIRLINE.** This frame is inside a rail that has already drawn both; a
	// second fill here would be a second opinion about the column's background, visible the day one of
	// them changes.
	const size_t devices = deviceCount();

	// THE SCROLL INDICATOR, only while there is something to scroll: a track that is always drawn is
	// furniture rather than information.
	if (maxScroll() > 0.0f) {
		const float view = static_cast<float>(height());
		const float thumbHeight = std::max(24.0f, view * (view / contentHeight()));
		const float thumbY = (view - thumbHeight) * (scroll_ / maxScroll());
		canvas.setColor(withAlpha(kLine, 0.9f));
		canvas.roundedRectangle(width() - 4.0f, thumbY, 2.0f, thumbHeight, 1.0f);
	}

	// THE BUMPER, while a card is being dragged — drawn by the COLUMN, because a gap belongs to the list
	// rather than to either card beside it, and it sits in the space between cards so no card covers it.
	if (dragDevice_.has_value() && dropGap_.has_value()) {
		const visage::Bounds bumper = gapBounds(*dropGap_);
		if (bumper.height() > 0.0f) {
			canvas.setColor(kValue);
			canvas.roundedRectangle(bumper.x(), bumper.y(), bumper.width(), bumper.height(), 1.0f);
		}
	}

	// THE (+), DRAWN FOR AN EMPTY CHAIN AND A FULL ONE ALIKE. AN OUTLINE RATHER THAN A FILLED CARD,
	// because it is a VERB and not a thing in the chain; DIMMED rather than hidden at capacity, because
	// a control that vanished at the bound would leave a player wondering where it went.
	const visage::Bounds add = addBounds();
	const bool atCapacity = full();
	canvas.setColor(withAlpha(kLine, atCapacity ? 0.35f : 0.75f));
	canvas.roundedRectangleBorder(add.x(), add.y(), add.width(), add.height(), kCornerRadius, 1.0f);
	canvas.setColor(atCapacity ? kDisabled : kValue);
	// GEOMETRY RATHER THAN A GLYPH, the rule the harmonic arrows are drawn by: this font has no
	// guarantee of a "+" that centres, and a missing glyph is an invisible control.
	const float plus = 7.0f;
	const float cx = add.x() + kPadX + plus * 0.5f;
	const float cy = add.y() + add.height() * 0.5f;
	canvas.segment(cx - plus * 0.5f, cy, cx + plus * 0.5f, cy, 1.4f, true);
	canvas.segment(cx, cy - plus * 0.5f, cx, cy + plus * 0.5f, 1.4f, true);
	canvas.text(atCapacity ? "chain full" : "add effect", font(kKnobLabelSize), visage::Font::kLeft,
	            cx + plus, add.y(), add.width() - kPadX - plus * 2.0f, add.height());

	// **AND WHAT AN EMPTY CHAIN DOES, SAID UNDER THE (+) RATHER THAN OVER IT.** `fx_lane.h`'s rule is
	// that an empty chain is drawn as an empty chain; here the (+) is the whole of the drawing, because
	// on an empty chain it sits at the TOP with nothing above it — a first draft put these words in the
	// space above the button, which on an empty chain is zero pixels tall, and PHOTOGRAPHED as nothing
	// at all. So they go below, and they say the one fact a player cannot see: this chain runs on
	// SKLITTER's summed output and an empty one is a true pass, bit for bit.
	if (devices == 0) {
		canvas.setColor(kMuted);
		canvas.text("nothing here — the output passes through", font(kKnobLabelSize),
		            visage::Font::kLeft, add.x(), add.bottom() + kSlotGap * 0.5f, add.width(),
		            kAddHeight);
	}
}

} // namespace feathers::ui
