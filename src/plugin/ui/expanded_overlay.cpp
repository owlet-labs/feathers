/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/mod_state.h"
#include "plugin/ui/expanded_overlay.h"

#include "plugin/ui/widget_registry.h"

#include "plugin/ui/device_control.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/ui_state.h"
#include "plugin/ui/window_controls_side.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace feathers::ui {

size_t ExpandedView::deviceEnableControl() const {
	const device::Descriptor* desc = watchedDescriptor();
	if (desc == nullptr || watchedHost() == nullptr || desc->traits.enableControl < 0) {
		return kNoEnable;
	}
	const auto at = static_cast<size_t>(desc->traits.enableControl);
	// A DECLARATION PAST THE END IS REFUSED rather than trusted — `FxCard::enableControl`'s rule.
	return at < desc->params.size() ? at : kNoEnable;
}

bool ExpandedView::deviceEnabled() const {
	const size_t at = deviceEnableControl();
	if (at == kNoEnable) {
		return true;
	}
	// THE MODULATED VALUE (backlog KW): the enable is a destination, so the chip says what the device IS — a gate routed
	// off reads off — by the one reader of an enable (`device::enableOn`).
	return device::enableOn(watchedHost()->modulatedFieldAt(watchedDevice(), at));
}

device::Modulatable* ExpandedView::enableSwitch() const {
	const size_t at = deviceEnableControl();
	if (at == kNoEnable || host_ == nullptr || descriptor_ == nullptr) {
		return nullptr;
	}
	if (enableSwitch_ == nullptr) {
		enableSwitch_ = std::make_unique<DeviceControl>(*host_, device_, *descriptor_, at, mod_, history_);
	}
	return enableSwitch_.get();
}

ExpandedView::EnableMarks ExpandedView::deviceEnableMarks() const {
	EnableMarks out;
	const device::Modulatable* control = enableSwitch();
	if (control == nullptr) {
		return out;
	}
	const device::ModDisplay d = control->display();
	if (!d.isDestination) {
		return out;
	}
	const device::Modulatable::Span span = control->amountSpan();
	if (d.armed) {
		out.glow = d.routable;
		return out;
	}
	out.ring = span.hi > span.lo || d.routedElsewhere;
	return out;
}

bool ExpandedView::routeDeviceEnable() {
	device::Modulatable* control = enableSwitch();
	if (control == nullptr) {
		return false;
	}
	const device::ModDisplay d = control->display();
	if (!d.isDestination || !d.armed || !d.routable) {
		return false;
	}
	// ROUTED BY THE ARMED SOURCE ALREADY: unroute. Otherwise route at full amount, so the source's whole travel crosses
	// the gate's half — a stepped destination rounds, so less would leave the lower half of the source dead. Asked as
	// the armed source's AMOUNT, not its reach: on a switch that is on, a full route reaches nothing above the top.
	const bool routed = mod_ != nullptr && control->amountFor(mod_->source()) != 0.0f;
	control->beginAmountGesture();
	control->setAmount(routed ? 0.0f : 1.0f);
	control->endAmountGesture();
	return true;
}

std::optional<bool> ExpandedView::toggleDeviceEnable() {
	const size_t at = deviceEnableControl();
	// THE WRITE NEEDS THE MUTABLE HOST THE FACTORY STAMPED; a panel whose `watchedHost` is an override
	// with no host of this base's has nothing to write through, and says so rather than guessing.
	if (at == kNoEnable || host_ == nullptr || descriptor_ == nullptr) {
		return std::nullopt;
	}
	(void)enableSwitch();
	// **THE CARD'S BODY, LINE FOR LINE** (`FxCard::toggleEnable`): bracketed, because the bracket is the
	// undo step, and the new state read back from what was written rather than assumed.
	const bool on = enableSwitch_->value() >= 0.5;
	enableSwitch_->beginGesture();
	enableSwitch_->setValue(on ? 0.0 : 1.0);
	enableSwitch_->endGesture();
	// AND IT IS ANNOUNCED, as the card announces its own ("Shaper off"), in the device's name.
	if (notifier_ != nullptr && descriptor_->traits.name != nullptr) {
		notifier_->say(std::string(descriptor_->traits.name) + (on ? " off" : " on"));
	}
	return !on;
}

namespace {

/// **DESTROY A PARK LIST BY SWAPPING IT EMPTY FIRST — backlog HL.** `parked.clear()` is not safe here
/// and the reason is libstdc++'s implementation rather than anything about this layer: `clear()`
/// destroys `[_M_start, _M_finish)` with `_M_finish` STILL AT THE OLD END, so a `push_back` that
/// happens from inside one of those destructors sees a full vector, reallocates, and FREES THE BUFFER
/// THE DESTROY LOOP IS WALKING. The loop then reads its next element out of freed memory.
///
/// That is not hypothetical: it is the crash that took the whole plugin down on 2026-09-20, and
/// the road is a straight line. `updateContent` clears `retired_`; the retired SKLITTER panel's
/// destructor reaches `~ChainColumn`, which dismisses the nested panel its column presented; that
/// dismissal lands back in `ExpandedOverlay::dismissBy` on THIS overlay and pushes twice. Two pushes
/// into a vector of capacity 2 — deterministic, and it needs only two panels open, not the three
/// levels that happened to be open.
///
/// **THE SWAP IS THE FIX AND IT IS STRUCTURAL, NOT A GUARD.** After the swap the member holds the
/// empty buffer and the local holds everything being destroyed, so a re-entrant `push_back` allocates
/// a fresh buffer that has nothing to do with the one being walked. The late arrival simply sits in
/// the member until the next reap, which is exactly what the park list is for. No flag to forget, no
/// ordering to get right, and it cannot regress by someone adding a destructor that re-enters.
template <typename T>
void reapParked(std::vector<T>& parked) {
	std::vector<T> going;
	going.swap(parked);
}

/// **A PANEL'S PLACE IS ROUNDED ON THE DEVICE GRID, NOT THE LOGICAL ONE — backlog PE.** A drag hands this
/// layer the hand's position in logical pixels, one DEVICE pixel at a time; rounding that to a whole
/// logical pixel at 1.75x moves the panel four times in every seven frames, two of
/// them by two device pixels, and leaves it up to one device pixel behind the hand. MEASURED through
/// the real handlers, 40 one-pixel events: 17 frames standing still and 17 catching up two at once.
/// That is the "twitchy" stutter. `Frame::setBounds` rounds `bounds * dpi` to native, so a
/// place on THIS grid is drawn exactly where it was asked for. At a scale of 1 the two grids are one.
float onDeviceGrid(float logical, float dpi) {
	return dpi > 0.0f ? std::round(logical * dpi) / dpi : std::round(logical);
}

} // namespace

void ExpandedPanel::wire() {
	if (wired_) {
		return;
	}
	wired_ = true;
	// PASSED TO CHILDREN, AND THE SECOND ARGUMENT IS THE WHOLE OF IT. `setIgnoresMouseEvents(false,
	// false)` on the layer is what made every control on this panel INERT: with
	// `pass_mouse_events_to_children_` false, `Frame::frameAtPoint` skips both child loops and
	// returns the frame itself, so no knob, no scope, no harmonic row and no switch ever received a
	// press. Fifteen subcases passed over a panel nobody could touch, because every one of them
	// addressed the widgets directly instead of asking where a press lands.
	setIgnoresMouseEvents(false, true);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE ✕ CLOSES; THE REST OF THE HEAD DRAGS. Anything else arriving here missed every control
		// and landed on the panel's own background, which is not a verb.
		if (closeBounds().contains(e.position)) {
			if (onClose_) {
				onClose_();
			}
			return;
		}
		// THE DEVICE'S ENABLE CHIP — before the fold and the drag, because it sits in the head and a
		// button inside the head is unclickable if the head is tested first (`FxCard::headControlAt`'s
		// rule). A double-click on it toggles twice and folds nothing.
		if (enableSource_ != nullptr && enableBounds().contains(e.position)) {
			// ARMED (backlog KW): the press routes the armed source into the enable, as a pill's power does; Alt
			// presses the switch whatever is armed.
			const bool alt = (e.modifiers & visage::kModifierAlt) != 0;
			if (alt || !enableSource_->routeDeviceEnable()) {
				(void)enableSource_->toggleDeviceEnable();
			}
			syncEnable();
			return;
		}
		// THE MAXIMIZE BUTTON (backlog OV), tested with the other head buttons and before the drag.
		if (maximizeBounds().contains(e.position)) {
			if (onMaximize_) {
				onMaximize_();
			}
			return;
		}
		// THE FOLD: its chevron, or a double-click on the head (backlog EE). The second press of a
		// double-click does not begin a drag — a hand that double-clicked was not dragging.
		if (foldBounds().contains(e.position)
		    || (titleBounds().contains(e.position) && e.repeatClickCount() >= 2)) {
			if (onFold_) {
				onFold_();
			}
			return;
		}
		// THE GRIP FIRST — it sits in the pad below and beside the content, where a title press cannot land.
		if (gripBounds().contains(e.position)) {
			resizing_ = true;
			resizeOrigin_ = e.window_position;
			if (onResizeBegin_) {
				onResizeBegin_();
			}
			return;
		}
		if (titleBounds().contains(e.position)) {
			dragging_ = true;
			// WINDOW PIXELS, because the panel is about to move under the hand and `e.position` moves
			// with it; the window is the one frame that stays still for the whole gesture.
			dragOrigin_ = e.window_position;
			if (onDragBegin_) {
				onDragBegin_();
			}
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (resizing_ && onResizeTo_) {
			onResizeTo_(e.window_position - resizeOrigin_);
			return;
		}
		if (dragging_ && onDragTo_) {
			onDragTo_(e.window_position - dragOrigin_);
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		dragging_ = false;
		resizing_ = false;
		// TO THE FRONT, ON RELEASE — see `onRaise_` for why not on the press.
		if (onRaise_) {
			onRaise_();
		}
	};
}

ExpandedOverlay::ExpandedOverlay() {
	// INVISIBLE ALWAYS DEAF. The scrim DRAWS the dimming and takes nothing: a frame's rectangle is
	// its hit region — `containsPoint` is not virtual — so a layer covering the editor in order to
	// dim it would swallow every press in the editor. Passing to children is what lets the panels
	// below take their own, and lets everything else fall through to the editor and arm a source.
	setVisible(false);
	setIgnoresMouseEvents(true, true);

	// ESC CLOSES THE FRONT PANEL — a default rather than design: a layer with no keyboard exit is a trap. One panel per press, nearest the hand first.
	//
	// WHETHER THE HOST DELIVERS THE KEY IS A SEPARATE QUESTION AND IS NOT ANSWERED HERE. A CLAP
	// editor embedded in someone else's window gets the keystrokes that host chooses to forward, and
	// this editor has never asked for any before now. The wiring is tested by injecting the event;
	// that a real DAW sends it is unverified. CLICK-OUTSIDE USED TO BE THE PRIMARY ROAD AND IS GONE —
	// see the header — so Esc and the ✕ are the two, and the ✕ is the one that certainly works.
	setAcceptsKeystrokes(true);
	onKeyPress() = [this](const visage::KeyEvent& e) {
		if (e.keyCode() != visage::KeyCode::Escape) {
			return false;
		}
		// **WITH THE EDITOR'S STACK, THE MOST RECENTLY OPENED THING WINS** (backlog NN) — a panel of this
		// layer, or an arm, a browser or anything else that opened after it. One thing per press still.
		if (EscapeStack* stack = escapeStack(); stack != nullptr) {
			return stack->closeTop() || (onEscapeWhenClosed != nullptr && onEscapeWhenClosed());
		}
		// NEAREST THE HAND WINS, AND ONLY ONE THING HAPPENS PER PRESS. Open: the front panel closes
		// and whatever else Esc might mean waits for the next press. Closed: the key is offered to
		// whoever else wants it — today that is "leave modulation assign mode" (backlog 20).
		if (Slot* top = front(); top != nullptr) {
			dismissBy(*top, Dismissal::Escape);
			return true;
		}
		// AND A KEY NOBODY WANTS IS NOT SWALLOWED. This layer would otherwise eat Esc for the whole
		// life of the editor, which is the failure its own case has guarded against since it was
		// written — now with a second road able to claim the key first.
		return onEscapeWhenClosed != nullptr && onEscapeWhenClosed();
	};
}

ExpandedOverlay::~ExpandedOverlay() {
	// **THE FOURTH REAP, AND THE ONE NOBODY WROTE — backlog HL.** `show`, `updateContent` and `draw`
	// empty `retired_` explicitly and all three now go through `reapParked`. The fourth emptying is
	// the one this class did not have a destructor for: `~vector`, during member teardown, with the
	// same `_M_finish`-at-the-old-end walk and the same re-entrant `push_back` landing in it.
	//
	// **MEASURED, NOT REASONED.** Park one panel and close the editor before the next tick and ASan
	// calls it a heap-use-after-free at the 16-byte buffer `dismissBy` had just allocated. The case
	// that does it is `backlog HL ... the DESTRUCTOR road`, and the case beside it that does the same
	// teardown WITHOUT dismissing first is green — because an empty vector's destructor deallocates
	// nothing, so the re-entrant push merely leaks a buffer under `detect_leaks=0`. One `dismiss`
	// apart, and the green one says nothing about the red one.
	//
	// **THE CHILDREN GO BEFORE THE VECTORS THAT OWN THEM**, the order `~TakeoverStack` states: this
	// frame still lists the panels as children, and `~Frame` runs after these members, so destroying
	// them here without saying so leaves a parent pointing at freed frames for the rest of teardown.
	// **THE PARK LIST ONLY, AND `slots_` IS DELIBERATELY LEFT TO THE IMPLICIT TEARDOWN THAT FOLLOWS.**
	// `retired_` is declared after `slots_`, so `~vector` was going to destroy it first anyway: this
	// changes HOW it is emptied, not WHEN.
	//
	// **THE RESTRAINT IS MEASURED, AND THE FIRST DRAFT DID NOT HAVE IT.** Giving `slots_` the same
	// swap took this binary from 7 green to 4: cases that had nothing to do with the park list started
	// failing in `forgetFrameTree`. The reason is that the swap makes `dismissBy` a NO-OP — it looks
	// the slot up in `slots_`, finds an empty vector, and returns before `onDismissPanel` — and
	// `onDismissPanel` is `SurfaceLayers::panelWentDown`, the only thing that takes a nested host's
	// `forwarded_`/`presented_` entry off its list. Skip it and a later destructor in the same walk
	// reads a mount the earlier one already freed. So the panel-by-panel road through `~vector` is
	// LOAD-BEARING here, and "empty it the safe way" was an optimisation that introduced three bugs.
	//
	// **WHAT THIS THEREFORE DOES NOT CLOSE, stated rather than left to be discovered.** A push into
	// `retired_` from inside `~vector<Slot>` still lands in a member whose own `~vector` has already
	// run. It is not a use-after-free only because the drain below leaves it at capacity 0, so that
	// `~vector` deallocates nothing and the late `push_back` allocates a buffer nobody frees — a leak,
	// invisible under `detect_leaks=0`. Closing it properly means dismissing each slot through the
	// normal road first, which means calling back into hosts that are themselves being destroyed
	// (`~ExpandedCanvas` nulls its callbacks to avoid exactly that), and that is a design change and
	// not a lifetime fix.
	//
	// DRAINED, RATHER THAN REAPED ONCE. Each pass can park more (the two frames of a panel a
	// destructor in the previous pass took down), and a single reap would leave those to `~vector`,
	// which is the walk this destructor exists to avoid. It terminates because teardown constructs no
	// panels, so every pass strictly shrinks the set.
	while (!retired_.empty()) {
		reapParked(retired_);
	}
}

std::string ExpandedOverlay::keyOf(const ExpandedView& content) {
	// **THE CHAIN AND THEN THE DEVICE, BECAUSE THE DEVICE ALONE IS NOT UNIQUE ACROSS NESTING.** See
	// `DeviceNest`: each chain counts its instances from `kFirstInstance`, so a device inside
	// SKLITTER's post chain and a device in the host's own chain can both be instance 1. With only
	// the id in the key, "one panel per device" retired a panel about a DIFFERENT device — a
	// SKLITTER-inside-SKLITTER, where opening the inner one closed the outer.
	//
	// **THE TOP-LEVEL SPELLING IS UNCHANGED** (`path` is 0 and prints as `0:`) only in the sense that
	// it is still one key per device; the string itself moved, and nothing persists it — the
	// remembered placement, size and fold are all keyed for the layer's lifetime and no longer.
	return content.title() + "#" + std::to_string(content.nest().path) + ":"
	       + std::to_string(content.watchedDevice());
}

ExpandedOverlay::Slot* ExpandedOverlay::slotOf(PanelId id) const {
	for (const auto& slot : slots_) {
		if (slot->id == id) {
			return slot.get();
		}
	}
	return nullptr;
}

ExpandedOverlay::PanelId ExpandedOverlay::show(std::unique_ptr<ExpandedView> content) {
	if (content == nullptr) {
		return kNoPanel;
	}
	// THE PREVIOUSLY RETIRED FRAMES ARE DESTROYED HERE, BEFORE THIS CALL RETIRES ANOTHER. It was the
	// other way round — replace, then reset — on the grounds that *"no dispatch is in flight at this
	// point"*. That is true while the press that opens a panel is on a CARD in the editor, and false
	// the moment panels nest: a card inside a chain device's panel presents through the outer host, so
	// the press is on a frame INSIDE a panel this call may replace. The older retirees were retired by
	// an earlier event and are safe to free; the one this call retires lives until the next `draw`.
	reapParked(retired_); // NOT clear() — backlog HL; see `reapParked`
	// ONE PANEL PER DEVICE. Whatever was open for this device goes first, through the same road as any
	// other dismissal, so a host keying surfaces by panel is told (`onDismissPanel`) — but the opener
	// is not told the layer "closed" (`onDismiss`), because it is mid-`show`.
	const std::string key = keyOf(*content);
	for (const auto& slot : slots_) {
		if (slot->key == key) {
			dismissBy(*slot, Dismissal::Replaced);
			break; // `dismissBy` erased it; the loop's iterator is not to be trusted past this
		}
	}

	auto slot = std::make_unique<Slot>();
	Slot* raw = slot.get();
	slot->id = next_++;
	slot->key = key;
	// THE PANEL'S CHROME, WIRED TO THIS SLOT. Every callback captures the slot pointer, which is stable
	// for the slot's life (a `unique_ptr` in the vector, moved by pointer and never by value).
	slot->panel = std::make_unique<ExpandedPanel>(
	    [this, raw] { dismissBy(*raw, Dismissal::CloseButton); }, [this, raw] { beginPanelDrag(*raw); },
	    [this, raw](visage::Point delta) { dragPanelBy(*raw, delta); },
	    [this, raw] { beginPanelResize(*raw); },
	    [this, raw](visage::Point delta) { resizePanelBy(*raw, delta); }, [this, raw] { raise(*raw); },
	    [this, raw] { setFolded(*raw, !raw->folded); },
	    [this, raw] { setMaximized(*raw, !raw->maximized); });
	slot->panel->wire();
	slot->content = std::move(content);
	// **WHEN THE CONTENT'S MINIMUM MOVES, EVERY PANEL IS FITTED AGAIN.** `panelSizeOf` reads
	// `minimumSize()` live, so this is the ASK rather than the answer — see
	// `ExpandedView::onMinimumSizeChanged` for the defect it closes. `resized()` re-derives every
	// slot's rectangle from its own content, so a content that grew is redrawn at its new size and one
	// that shrank hands the room back.
	slot->content->onMinimumSizeChanged([this] {
		resized();
		redraw();
	});
	slot->panel->setTitle(slot->content->headerTitle());
	// THE SAME DERIVATION THE CARD USES (`FxCard::tintedHeadMarks`), over the same two facts — so the
	// card a hand pressed and the panel that opened cannot disagree about which colour they are.
	slot->panel->setTint(instanceTintFor(slot->content->watchedDevice(), slot->content->nest()));
	// WHERE THIS DEVICE'S PANEL WAS LEFT, if it has been dragged in this editor's lifetime; the dodge
	// below places it otherwise, and moves it off another panel if the remembered place is covered.
	if (const auto it = remembered_.find(key); it != remembered_.end()) {
		slot->placed = it->second;
	}
	if (const auto it = rememberedSize_.find(key); it != rememberedSize_.end()) {
		slot->sized = it->second;
	}
	// AND WHETHER IT WAS LEFT FOLDED — it reopens as the strip it was (backlog EE).
	if (const auto it = rememberedFold_.find(key); it != rememberedFold_.end() && it->second) {
		slot->folded = true;
		slot->panel->setFolded(true);
	}
	// AND WHETHER IT WAS LEFT MAXIMIZED (backlog OV); a fold wins, the two exclude each other.
	if (const auto it = rememberedMaximized_.find(key); it != rememberedMaximized_.end() && it->second && !slot->folded) {
		slot->maximized = true;
		slot->panel->setMaximized(true);
	}
	slot->panel->addChild(slot->content.get());
	// THE TITLE ROW'S CONTROL, IF THE CONTENT HAS ONE — parented here, taken back in `dismissBy` before
	// either frame is retired. It stays up while folded: the strip is still the window's title row.
	slot->panel->setAccessory(slot->content->titleAccessory());
	// AND THE DEVICE'S ENABLE CHIP, answered by the content — taken back in `dismissBy` with the accessory.
	slot->panel->setEnableSource(slot->content.get());
	slot->content->setVisible(!slot->folded);
	slot->panel->setVisible(true);
	// ON THE STATE CHANNEL IN ITS OWN RIGHT — property 5. Not because it is a child of the editor,
	// which it will one day not be.
	if (watchers_ != nullptr) {
		slot->watch = watchers_->watch(slot->content.get());
	}
	addChild(slot->panel.get()); // last child: on top
	// ON THE ESC STACK FROM NOW UNTIL IT CLOSES (backlog NN), by its id: Esc finds it again rather than
	// holding the slot, and finds nothing — so the stack moves on — if it has already gone.
	if (EscapeStack* stack = escapeStack(); stack != nullptr) {
		const PanelId id = raw->id;
		slot->escape = stack->push([this, id] {
			Slot* open = slotOf(id);
			if (open == nullptr) {
				return false;
			}
			dismissBy(*open, Dismissal::Escape);
			return true;
		});
	}
	slots_.push_back(std::move(slot));
	place(*raw);
	setVisible(true);
	requestKeyboardFocus();
	resized();
	redraw();
	return raw->id;
}

void ExpandedOverlay::place(Slot& slot) {
	const visage::Point size = panelSizeOf(slot);
	const float w = size.x;
	const float h = size.y;
	const auto clampedRect = [&](visage::Point at) {
		at.x = std::clamp(at.x, 0.0f, std::max(0.0f, width() - w));
		at.y = std::clamp(at.y, 0.0f, std::max(0.0f, height() - h));
		return visage::Bounds { std::round(at.x), std::round(at.y), w, h };
	};
	const auto covered = [&](const visage::Bounds& rect) {
		for (const auto& other : slots_) {
			if (other.get() != &slot && rect.overlaps(panelBoundsOf(*other))) {
				return true;
			}
		}
		return false;
	};
	// THE PREFERRED SPOT: where this device's panel was left; else the preferred region's top-left — the
	// open canvas's floor, FULL LEFT, so that windows appearing rightward have headroom; else centred
	// over the editor (decided 2026-09-07, and where the mod rails stay reachable beside it). The place a hand leaves a
	// panel is remembered for the device while the editor is open, and wins over the region: the region
	// is where a panel ARRIVES, not where it must stay.
	const bool inRegion = preferredRegion_.has_value() && !slot.placed.has_value();
	const visage::Point preferred = slot.placed.value_or(
	    inRegion ? visage::Point { preferredRegion_->x(), preferredRegion_->y() }
	             : visage::Point { std::round((width() - w) * 0.5f), std::round((height() - h) * 0.5f) });
	const visage::Bounds first = clampedRect(preferred);
	if (!covered(first)) {
		slot.placed = { first.x(), first.y() };
		return;
	}
	// **DODGE**: every place a new panel could sit flush beside an open one — its four sides, and the
	// layer's corners — kept only where the whole panel fits inside the layer and covers nobody, and the
	// nearest of those to the preferred spot wins. No such place is the layer being too full, and then
	// the preferred spot it is: overlap only if there is no alternative.
	constexpr float kGap = 8.0f;
	std::vector<visage::Point> candidates;
	for (const auto& other : slots_) {
		if (other.get() == &slot) {
			continue;
		}
		const visage::Bounds r = panelBoundsOf(*other);
		candidates.push_back({ r.right() + kGap, r.y() });
		candidates.push_back({ r.x() - w - kGap, r.y() });
		candidates.push_back({ r.x(), r.bottom() + kGap });
		candidates.push_back({ r.x(), r.y() - h - kGap });
		candidates.push_back({ r.right() + kGap, preferred.y });
		candidates.push_back({ r.x() - w - kGap, preferred.y });
		candidates.push_back({ preferred.x, r.bottom() + kGap });
		candidates.push_back({ preferred.x, r.y() - h - kGap });
	}
	candidates.push_back({ 0.0f, 0.0f });
	candidates.push_back({ width() - w, 0.0f });
	candidates.push_back({ 0.0f, height() - h });
	candidates.push_back({ width() - w, height() - h });
	if (inRegion) {
		const visage::Bounds& r = *preferredRegion_;
		candidates.push_back({ r.right() - w, r.y() });
		candidates.push_back({ r.x(), r.bottom() - h });
		candidates.push_back({ r.right() - w, r.bottom() - h });
	}
	// THE NEAREST, ROWS FIRST: the candidate on the preferred spot's own row wins over one below or above
	// it, however far along the row — so panels fill RIGHTWARD from the first and wrap only when the row
	// is full (so that windows appearing rightward have headroom). Within a row, the nearer. Asked of an
	// AREA the whole panel must fit in: the preferred region first, then the whole layer.
	const auto nearestFreeIn = [&](const visage::Bounds& area) -> std::optional<visage::Point> {
		std::optional<visage::Point> best;
		float bestRow = 0.0f;
		float bestAlong = 0.0f;
		for (const visage::Point& c : candidates) {
			if (c.x < area.x() || c.y < area.y() || c.x + w > area.right() || c.y + h > area.bottom()) {
				continue; // does not fit whole: a clamped candidate is a different rectangle
			}
			const visage::Bounds rect { std::round(c.x), std::round(c.y), w, h };
			if (covered(rect)) {
				continue;
			}
			const float row = std::abs(rect.y() - preferred.y);
			const float along = std::abs(rect.x() - preferred.x);
			if (!best.has_value() || row < bestRow || (row == bestRow && along < bestAlong)) {
				best = visage::Point { rect.x(), rect.y() };
				bestRow = row;
				bestAlong = along;
			}
		}
		return best;
	};
	std::optional<visage::Point> best;
	if (inRegion) {
		best = nearestFreeIn(*preferredRegion_);
	}
	if (!best.has_value()) {
		best = nearestFreeIn({ 0.0f, 0.0f, width(), height() });
	}
	if (best.has_value()) {
		slot.placed = *best;
		return;
	}
	// TOO FULL: OVER ANOTHER, BUT CASCADED — down one title row and a step right from the panel on top
	// of the preferred spot, so the covered panel's head stays out from under the new one and a hand
	// can still reach it to raise it. When the window is full, the first
	// answer, "on top, at the preferred spot", stacked same-sized panels exactly, and the one below
	// was unreachable until the one above was moved or closed.
	constexpr float kCascadeStep = 24.0f;
	visage::Point cascaded { first.x(), first.y() };
	for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
		if (it->get() == &slot) {
			continue;
		}
		const visage::Bounds r = panelBoundsOf(**it);
		if (r.overlaps(first)) {
			cascaded = { r.x() + kCascadeStep, r.y() + kTitleHeight };
			break; // the front-most panel under the spot is the one whose head must stay clear
		}
	}
	const visage::Bounds rect = clampedRect(cascaded);
	slot.placed = { rect.x(), rect.y() };
}

void ExpandedOverlay::raise(Slot& slot) {
	// BROUGHT TO THE FRONT IS OPENED AGAIN, for Esc's order too (backlog NN) — even when it already was the
	// front panel, because something of another kind may have opened over it since.
	slot.escape.raise();
	if (slots_.empty() || slots_.back().get() == &slot) {
		return;
	}
	for (auto it = slots_.begin(); it != slots_.end(); ++it) {
		if (it->get() == &slot) {
			std::unique_ptr<Slot> taken = std::move(*it);
			slots_.erase(it);
			// LAST CHILD DRAWS LAST: re-added at the end. Only ever from a RELEASE (see `ExpandedPanel::
			// onRaise_`), because `removeChild` clears the frame a drag is being delivered to.
			removeChild(taken->panel.get());
			addChild(taken->panel.get());
			slots_.push_back(std::move(taken));
			redraw();
			return;
		}
	}
}

bool ExpandedOverlay::focusPanelFor(DeviceInstanceId device, DeviceNest nest) {
	if (device == kNoInstance) {
		return false;
	}
	for (const auto& slot : slots_) {
		if (slot->content->watchedDevice() == device && slot->content->nest() == nest) {
			raise(*slot); // `raise` moves the vector's entries; the loop ends here
			return true;
		}
	}
	return false;
}

bool ExpandedOverlay::dismissPanelFor(DeviceInstanceId device, DeviceNest nest) {
	if (device == kNoInstance) {
		return false;
	}
	for (const auto& slot : slots_) {
		if (slot->content->watchedDevice() == device && slot->content->nest() == nest) {
			dismissBy(*slot, Dismissal::ExpandToggle); // erases the entry; the loop ends here
			return true;
		}
	}
	return false;
}

void ExpandedOverlay::dismiss() {
	while (!slots_.empty()) {
		dismissBy(*slots_.back(), Dismissal::Programmatic);
	}
}

void ExpandedOverlay::dismiss(PanelId id) {
	if (Slot* slot = slotOf(id); slot != nullptr) {
		dismissBy(*slot, Dismissal::Programmatic);
	}
}

bool ExpandedOverlay::updateContent() {
	// A TICK IS OUTSIDE EVERY DISPATCH, so a retired frame can be freed here as well as in `draw`
	// — and a case can free one without a canvas to draw into.
	reapParked(retired_); // NOT clear() — backlog HL; see `reapParked`
	bool told = false;
	for (const auto& slot : slots_) {
		if (slot->content->update()) {
			told = true;
		}
		// THE HEADER'S TEXT FOLLOWS ITS CONTENT (ExpandedView::headerTitle) — the spectrum panel's names the selected sound.
		if (std::string header = slot->content->headerTitle(); header != slot->panel->title()) {
			slot->panel->setTitle(std::move(header));
			slot->panel->redraw();
			told = true;
		}
		// THE HEADER'S ENABLE CHIP FOLLOWS ITS FIELD — a toggle from the card, an undo, a patch load.
		if (slot->panel->syncEnable()) {
			told = true;
		}
		// AND THE SIDE ITS CONTROLS SIT ON FOLLOWS THE DESK'S SETTING (backlog OD).
		if (slot->panel->syncSide()) {
			told = true;
		}
	}
	return told;
}

void ExpandedOverlay::dismissBy(Slot& slot, Dismissal how) {
	// TAKEN OUT OF THE LIST FIRST, so nothing below can find a half-closed panel, and so the reference
	// this was called with stays valid while the slot is torn down.
	std::unique_ptr<Slot> taken;
	for (auto it = slots_.begin(); it != slots_.end(); ++it) {
		if (it->get() == &slot) {
			taken = std::move(*it);
			slots_.erase(it);
			break;
		}
	}
	if (taken == nullptr) {
		return;
	}
	// OFF EVERY REDRAW LIST FIRST, AND THE ORDER IS THE WHOLE OF IT. `notify()` calls `redraw()` on
	// every entry, so a list still holding this frame after the reset is a use-after-free waiting for
	// the next thing that moves.
	//
	// AND EVERY FRAME UNDER IT, not only the one this function happens to name — backlog 19. The
	// fix before that one unwatched the content alone, which is correct for the frame it names and
	// misses every knob inside it.
	//
	// ALL THREE LISTS, NAMED. This was two calls until the signature grew a third parameter, and the
	// second of them existed because THIS LAYER SHIPPED THE MISSING ONE: the comment that stood here
	// said `Knob::setModulation` puts the knobs on "the SAME list", and it does not — they go on
	// `ModState`'s, which this layer had no pointer to. The `nullptr` is a declaration rather than an
	// omission: an `ExpandedView` builds its knobs through the `device::Modulatable` constructor,
	// which deliberately does not register, so there is nothing of ours on the parameter list.
	forgetFrameTree(taken->content.get(), nullptr, watchers_, mod_ != nullptr ? &mod_->watchers() : nullptr);
	// **AND THE ACCESSORY, WHICH IS NOT UNDER THE CONTENT** — the content owns it but the PANEL parents
	// it, so the walk above cannot reach it. Off the lists and out of the panel before either is retired,
	// so the panel never lists a frame the content's destructor frees.
	if (visage::Frame* accessory = taken->panel->accessory(); accessory != nullptr) {
		forgetFrameTree(accessory, nullptr, watchers_, mod_ != nullptr ? &mod_->watchers() : nullptr);
		taken->panel->setAccessory(nullptr);
	}
	// THE ENABLE CHIP'S SOURCE IS THE CONTENT, which is retired beside the panel — so the panel stops
	// asking it now, and a parked panel can never read a view the reaper freed first.
	taken->panel->setEnableSource(nullptr);
	taken->watch = Watch {};
	if (taken->placed.has_value()) {
		remembered_[taken->key] = *taken->placed;
	}
	rememberedFold_[taken->key] = taken->folded;
	rememberedMaximized_[taken->key] = taken->maximized;
	taken->panel->removeChild(taken->content.get());
	taken->panel->setVisible(false);
	removeChild(taken->panel.get());
	// RETIRED, NOT DESTROYED — the panel as well as the content. This runs from a mouse handler — the ✕
	// — and visage is still walking the tree when it returns. Deleting a frame out from under that walk
	// is how a SIGSEGV lands in an unrelated frame's handler, which is what happened. The next `show`,
	// `draw` or tick frees them.
	const PanelId id = taken->id;
	retired_.push_back(std::move(taken->content));
	retired_.push_back(std::move(taken->panel));
	taken.reset();
	lastDismissal_ = how;
	setVisible(!slots_.empty());
	redraw();
	// LAST, AND AFTER THE STATE IS ALREADY CONSISTENT. `onDismiss` reaches the lane, which will look
	// at this layer to decide how to draw its expand button — so it must not be able to observe a
	// half-closed one.
	//
	// AND NOT ON A REPLACEMENT, because the caller is mid-`show`: telling it the layer closed while
	// it is opening the next thing would be a lie that lasts one line. The per-panel road IS told, so
	// a host keying surfaces by panel retires the replaced one's id.
	if (onDismissPanel) {
		onDismissPanel(id);
	}
	if (how != Dismissal::Replaced && onDismiss) {
		onDismiss();
	}
}

visage::Point ExpandedOverlay::ceilingOf(const ExpandedView& content) {
	const visage::Point minimum = content.minimumSize();
	const visage::Point maximum = content.maximumSize();
	return { std::max(minimum.x, maximum.x), std::max(minimum.y, maximum.y) };
}

visage::Point ExpandedOverlay::contentSizeOf(const Slot& slot) {
	// THE SIZE A HAND CHOSE, never below the content's minimum (the grip cannot shrink it past that)
	// and never above its ceiling (backlog EI). BOTH READ LIVE, so a content whose limits moved — a
	// mode that thinned the grid, a rail that closed — is fitted on the next layout pass, and a window
	// above its new ceiling is drawn at it. `sized` itself is left as the hand chose it.
	const visage::Point minimum = slot.content->minimumSize();
	if (!slot.sized.has_value()) {
		return minimum;
	}
	const visage::Point ceiling = ceilingOf(*slot.content);
	return { std::clamp(slot.sized->x, minimum.x, ceiling.x), std::clamp(slot.sized->y, minimum.y, ceiling.y) };
}

visage::Point ExpandedOverlay::panelSizeOf(const Slot& slot) const {
	// THE CONTENT'S SIZE PLUS THE LAYER'S CHROME, and nothing else decides it. A content whose
	// minimum does not fit the layer is clipped rather than squashed: a squashed control is a control
	// that lies about where it is, and the honest failure is the visible one.
	const visage::Point size = contentSizeOf(slot);
	// CLAMPED TO THE LAYER AND NOTHING ELSE. A first draft had a `kMaxFraction` ceiling here and it
	// was DEAD — it only bound in the band where the clamp already un-bound it, so the panel was never
	// once smaller than its content asked for.
	if (slot.maximized) {
		return { width(), height() }; // the whole overlay, whatever the hand left behind
	}
	if (slot.folded) {
		return { std::min(size.x + 2.0f * kPanelPad, width()), kTitleHeight }; // the strip keeps its width
	}
	return { std::min(size.x + 2.0f * kPanelPad, width()), std::min(size.y + kTitleHeight + 2.0f * kPanelPad, height()) };
}

bool ExpandedOverlay::folded(PanelId id) const {
	const Slot* slot = slotOf(id);
	return slot != nullptr && slot->folded;
}

void ExpandedOverlay::setFolded(PanelId id, bool folded) {
	if (Slot* slot = slotOf(id); slot != nullptr) {
		setFolded(*slot, folded);
	}
}

void ExpandedOverlay::setFolded(Slot& slot, bool folded) {
	if (slot.folded == folded) {
		return;
	}
	if (folded && slot.maximized) {
		setMaximized(slot, false); // a maximized panel is restored before it folds
	}
	slot.folded = folded;
	slot.panel->setFolded(folded);
	// THE CONTENT IS HIDDEN, NOT REMOVED: its watchers, its state and its place in the tree stay, so
	// unfolding costs nothing and a modulation arm still reaches it the moment it shows again.
	slot.content->setVisible(!folded);
	rememberedFold_[slot.key] = folded;
	if (!folded) {
		raise(slot); // opened again, on top, where it was
	}
	resized();
	redraw();
}

bool ExpandedOverlay::maximized(PanelId id) const {
	const Slot* slot = slotOf(id);
	return slot != nullptr && slot->maximized;
}

void ExpandedOverlay::setMaximized(PanelId id, bool maximized) {
	if (Slot* slot = slotOf(id); slot != nullptr) {
		setMaximized(*slot, maximized);
	}
}

void ExpandedOverlay::setMaximized(Slot& slot, bool maximized) {
	if (slot.maximized == maximized) {
		return;
	}
	// MAXIMIZING A FOLDED PANEL UNFOLDS IT: the two exclude each other (backlog OV).
	if (maximized && slot.folded) {
		setFolded(slot, false);
	}
	// `placed` and `sized` are NOT touched in either direction: `panelBoundsOf` simply answers the overlay's
	// rectangle while this is set, so restoring is the earlier bounds by construction and not by a copy.
	slot.maximized = maximized;
	slot.panel->setMaximized(maximized);
	rememberedMaximized_[slot.key] = maximized;
	raise(slot);
	resized();
	redraw();
}

visage::Bounds ExpandedOverlay::maximizeBounds() const {
	const Slot* top = front();
	const visage::Bounds panel = panelBounds();
	if (panel.width() <= 0.0f || top == nullptr) {
		return {};
	}
	const visage::Bounds button = top->panel->maximizeBounds();
	return { panel.x() + button.x(), panel.y() + button.y(), button.width(), button.height() };
}

visage::Bounds ExpandedOverlay::foldBounds() const {
	const Slot* top = front();
	const visage::Bounds panel = panelBounds();
	if (panel.width() <= 0.0f || top == nullptr) {
		return {};
	}
	const visage::Bounds fold = top->panel->foldBounds();
	return { panel.x() + fold.x(), panel.y() + fold.y(), fold.width(), fold.height() };
}

visage::Bounds ExpandedOverlay::panelBoundsOf(const Slot& slot) const {
	const visage::Point size = panelSizeOf(slot);
	if (slot.maximized) {
		return { 0.0f, 0.0f, size.x, size.y };
	}
	visage::Point at = slot.placed.value_or(visage::Point { std::round((width() - size.x) * 0.5f),
	                                                        std::round((height() - size.y) * 0.5f) });
	// **AND NEVER OUTSIDE THIS LAYER.** "Nearly impossible to find" was the Mac's separate window
	// opening under the DAW; a panel that could be dragged off the editor, or left off it by a resize,
	// would be the same complaint by another road. Clamped every time it is placed, so a resize that
	// shrinks the editor pulls the panel back in.
	at.x = std::clamp(at.x, 0.0f, std::max(0.0f, width() - size.x));
	at.y = std::clamp(at.y, 0.0f, std::max(0.0f, height() - size.y));
	// ON THE DEVICE GRID — see `onDeviceGrid`: a whole logical pixel is a stutter at 1.75x.
	return { onDeviceGrid(at.x, dpiScale()), onDeviceGrid(at.y, dpiScale()), size.x, size.y };
}

visage::Bounds ExpandedOverlay::panelBounds() const {
	const Slot* top = front();
	return top != nullptr ? panelBoundsOf(*top) : visage::Bounds {};
}

visage::Bounds ExpandedOverlay::panelBounds(PanelId id) const {
	const Slot* slot = slotOf(id);
	return slot != nullptr ? panelBoundsOf(*slot) : visage::Bounds {};
}

const ExpandedView* ExpandedOverlay::contentForTest(PanelId id) const {
	const Slot* slot = slotOf(id);
	return slot != nullptr ? slot->content.get() : nullptr;
}

ExpandedPanel* ExpandedOverlay::panelForTest(PanelId id) const {
	const Slot* slot = slotOf(id);
	return slot != nullptr ? slot->panel.get() : nullptr;
}

void ExpandedOverlay::beginPanelDrag() {
	if (Slot* top = front(); top != nullptr) {
		beginPanelDrag(*top);
	}
}

void ExpandedOverlay::dragPanelBy(visage::Point delta) {
	if (Slot* top = front(); top != nullptr) {
		dragPanelBy(*top, delta);
	}
}

void ExpandedOverlay::beginPanelResize() {
	if (Slot* top = front(); top != nullptr) {
		beginPanelResize(*top);
	}
}

void ExpandedOverlay::resizePanelBy(visage::Point delta) {
	if (Slot* top = front(); top != nullptr) {
		resizePanelBy(*top, delta);
	}
}

void ExpandedOverlay::beginPanelDrag(Slot& slot) {
	if (slot.maximized) {
		return; // the overlay owns a maximized panel's rectangle
	}
	const visage::Bounds panel = panelBoundsOf(slot);
	slot.dragAnchor = { panel.x(), panel.y() };
}

void ExpandedOverlay::dragPanelBy(Slot& slot, visage::Point delta) {
	if (slot.maximized) {
		return;
	}
	slot.placed = slot.dragAnchor + delta;
	// PLACED THROUGH `panelBoundsOf`, which clamps — so what is remembered is where the panel actually
	// is, never where the hand went. A dragged panel may cover another: the dodge is for arrivals, and
	// a hand that puts one panel over another meant to.
	resized();
	const visage::Bounds panel = panelBoundsOf(slot);
	slot.placed = { panel.x(), panel.y() };
	remembered_[slot.key] = *slot.placed;
	redraw();
}

void ExpandedOverlay::beginPanelResize(Slot& slot) {
	if (slot.maximized) {
		return;
	}
	// FROM THE SIZE ON SCREEN, not the size remembered: a hand choice above a ceiling that has since
	// dropped would otherwise be a dead zone the grip had to travel back through before anything moved.
	slot.resizeAnchor = contentSizeOf(slot);
	// PIN THE NEAR CORNER. A resize is a gesture on the far corner and the near one stays; every shown
	// panel is placed, but a layer resize may have clamped it, so the place is re-read here.
	const visage::Bounds panel = panelBoundsOf(slot);
	slot.placed = { panel.x(), panel.y() };
	remembered_[slot.key] = *slot.placed;
}

void ExpandedOverlay::resizePanelBy(Slot& slot, visage::Point delta) {
	if (slot.maximized) {
		return;
	}
	// THE GRIP MOVES THE FAR CORNER; the near corner stays where the panel was placed. Clamped three
	// ways: not below the content's minimum (its own floor), not above its ceiling (backlog EI — the
	// grip simply stops, per axis, where the content has nothing more to put), and not past this
	// overlay's edge from where the panel sits.
	const visage::Point minimum = slot.content->minimumSize();
	const visage::Point ceiling = ceilingOf(*slot.content);
	const visage::Bounds panel = panelBoundsOf(slot);
	const float roomX = std::max(minimum.x, std::min(ceiling.x, width() - panel.x() - 2.0f * kPanelPad));
	const float roomY =
	    std::max(minimum.y, std::min(ceiling.y, height() - panel.y() - kTitleHeight - 2.0f * kPanelPad));
	// The far corner on the DEVICE grid, as the place is (`onDeviceGrid`): the grip follows the hand too.
	slot.sized = { std::clamp(onDeviceGrid(slot.resizeAnchor.x + delta.x, dpiScale()), minimum.x, roomX),
		           std::clamp(onDeviceGrid(slot.resizeAnchor.y + delta.y, dpiScale()), minimum.y, roomY) };
	rememberedSize_[slot.key] = *slot.sized;
	resized();
	redraw();
}

visage::Bounds ExpandedPanel::titleBounds() const {
	return { 0.0f, 0.0f, width(), ExpandedOverlay::kTitleHeight };
}

visage::Bounds ExpandedPanel::closeBounds() const {
	const visage::Bounds title = titleBounds();
	// THE CONTROL CLUSTER SITS AT THE END OF THE HEAD THE DESK CHOSE (`windowControlsSide`, backlog OD):
	// right to left from the right edge, or left to right from the left edge — the ✕ is always the
	// outermost of them, as the OS puts its own close.
	const float x = windowControlsSide() == WindowControlsSide::Left
	                    ? title.x() + ExpandedOverlay::kPanelPad
	                    : title.right() - ExpandedOverlay::kPanelPad - ExpandedOverlay::kCloseSize;
	return { x, title.y() + (title.height() - ExpandedOverlay::kCloseSize) * 0.5f, ExpandedOverlay::kCloseSize,
		     ExpandedOverlay::kCloseSize };
}
visage::Bounds ExpandedPanel::gripBounds() const {
	if (folded_ || maximized_) {
		return {}; // a strip has no size to change
	}
	return { std::max(0.0f, width() - kGripSize), std::max(0.0f, height() - kGripSize), kGripSize, kGripSize };
}

visage::Bounds ExpandedPanel::foldBounds() const {
	// BESIDE THE ✕, one button's gap to its left — the same square, so the two read as a pair.
	const visage::Bounds close = closeBounds();
	constexpr float kGap = 6.0f;
	const float x = windowControlsSide() == WindowControlsSide::Left ? close.right() + kGap
	                                                                 : close.x() - kGap - ExpandedOverlay::kCloseSize;
	return { x, close.y(), ExpandedOverlay::kCloseSize, ExpandedOverlay::kCloseSize };
}


visage::Bounds ExpandedPanel::maximizeBounds() const {
	// ONE GAP INBOARD OF THE FOLD, the same square: close, fold, maximize read outward-in from the end the desk chose.
	const visage::Bounds fold = foldBounds();
	constexpr float kGap = 6.0f;
	const float x = windowControlsSide() == WindowControlsSide::Left ? fold.right() + kGap
	                                                                 : fold.x() - kGap - ExpandedOverlay::kCloseSize;
	return { x, fold.y(), ExpandedOverlay::kCloseSize, ExpandedOverlay::kCloseSize };
}

void ExpandedPanel::setAccessory(visage::Frame* accessory) {
	if (accessory == accessory_) {
		return;
	}
	if (accessory_ != nullptr) {
		removeChild(accessory_);
	}
	accessory_ = accessory;
	if (accessory_ != nullptr) {
		addChild(accessory_);
		resized();
	}
}

void ExpandedPanel::setEnableSource(ExpandedView* source) {
	if (source == enableSource_) {
		return;
	}
	enableSource_ = source;
	enableShown_ = enableSource_ == nullptr || enableSource_->deviceEnabled();
	resized();
	redraw();
}

visage::Bounds ExpandedPanel::enableBounds() const {
	if (enableSource_ == nullptr || !enableSource_->hasDeviceEnable()) {
		return {};
	}
	const visage::Bounds fold = maximizeBounds();
	constexpr float kGap = 10.0f;
	const float x = windowControlsSide() == WindowControlsSide::Left ? fold.right() + kGap : fold.x() - kGap - kEnableSize;
	return { x, std::round(fold.y() + (fold.height() - kEnableSize) * 0.5f), kEnableSize, kEnableSize };
}

bool ExpandedPanel::syncSide() {
	const WindowControlsSide now = windowControlsSide();
	if (now == shownSide_) {
		return false;
	}
	shownSide_ = now;
	resized();
	redraw();
	return true;
}

bool ExpandedPanel::syncEnable() {
	const bool now = enableSource_ == nullptr || enableSource_->deviceEnabled();
	const ExpandedView::EnableMarks marks = enableSource_ == nullptr ? ExpandedView::EnableMarks {}
	                                                                 : enableSource_->deviceEnableMarks();
	if (now == enableShown_ && marks == enableMarks_) {
		return false;
	}
	enableShown_ = now;
	enableMarks_ = marks;
	redraw();
	return true;
}

float ExpandedPanel::titleX() const {
	if (windowControlsSide() == WindowControlsSide::Right) {
		return ExpandedOverlay::kPanelPad;
	}
	// LEFT: the title starts one gap past the last of the cluster — the chip when there is one, else the fold.
	const visage::Bounds sw = enableBounds();
	const visage::Bounds last = sw.width() > 0.0f ? sw : maximizeBounds();
	return last.right() + 12.0f;
}

visage::Bounds ExpandedPanel::accessoryBounds() const {
	if (accessory_ == nullptr) {
		return {};
	}
	const visage::Bounds title = titleBounds();
	if (windowControlsSide() == WindowControlsSide::Left) {
		// MIRRORED: the cluster owns the left end and the title follows it, so the accessory takes the far
		// right end of the row — and, as ever, is CLEAR OF THE TITLE TEXT OR NOT AT ALL.
		const float w = accessory_->width();
		const float h = std::min(accessory_->height(), title.height());
		const float x = title.right() - ExpandedOverlay::kPanelPad - w;
		constexpr float kGap = 10.0f;
		if (x < titleX() + trackedWidth(font(kControlSize), title_, kControlTracking) + kGap) {
			return {};
		}
		return { std::round(x), std::round(title.y() + (title.height() - h) * 0.5f), w, h };
	}
	// INBOARD OF THE ENABLE CHIP WHEN THERE IS ONE, else of the fold — the row reads, right to left,
	// ✕, fold, enable, accessory, and then the title from the left.
	const visage::Bounds sw = enableBounds();
	const visage::Bounds anchor = sw.width() > 0.0f ? sw : maximizeBounds();
	constexpr float kGap = 10.0f;
	const float w = accessory_->width();
	const float h = std::min(accessory_->height(), title.height());
	const float x = anchor.x() - kGap - w;
	// CLEAR OF THE TITLE TEXT OR NOT AT ALL — measured with the font the title is drawn in, so the
	// question is the one a hand would ask looking at the row.
	const float titleRight =
	    ExpandedOverlay::kPanelPad + trackedWidth(font(kControlSize), title_, kControlTracking);
	if (x < titleRight + kGap) {
		return {};
	}
	return { std::round(x), std::round(title.y() + (title.height() - h) * 0.5f), w, h };
}

void ExpandedPanel::resized() {
	if (accessory_ == nullptr) {
		return;
	}
	const visage::Bounds at = accessoryBounds();
	accessory_->setVisible(at.width() > 0.0f);
	if (at.width() > 0.0f) {
		accessory_->setBounds(at.x(), at.y(), at.width(), at.height());
	}
}

visage::Bounds ExpandedPanel::contentBounds() const {
	return { ExpandedOverlay::kPanelPad, ExpandedOverlay::kTitleHeight,
		     std::max(0.0f, width() - 2.0f * ExpandedOverlay::kPanelPad),
		     std::max(0.0f, height() - ExpandedOverlay::kTitleHeight - ExpandedOverlay::kPanelPad) };
}

uint32_t ExpandedPanel::edgeColor() const noexcept {
	return tint_ != 0 ? instanceEdgeColor(tint_) : withAlpha(kLine, 0.9f);
}

void ExpandedPanel::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.roundedRectangle(0.0f, 0.0f, width(), height(), ExpandedOverlay::kCornerRadius);
	// **THE EDGE CARRIES THE TINT, AND THAT IS WHAT MAKES IT READ AT A GLANCE.** The bar in the title
	// row was the whole flare, and a 3px mark at one corner of a 700px panel is a thing you find rather
	// than a thing you see — which is no use at all in the state it exists for: two panels overlapping,
	// one opened from inside the other, where most of what is on screen is body and border.
	//
	// **AN EDGE AND NOT A GROUND.** A colour flare, not a coloured panel: the surface
	// stays `kSurfaceRaised` in every panel, so nothing on it changes contrast and no device's controls
	// are read against a different background from the same device's card. `kPanelEdgeTintAlpha` is
	// deliberately short of full: the hue must be nameable at a glance and must not out-shout the
	// accent, which is the only saturated colour that MEANS anything on this chrome.
	canvas.setColor(edgeColor());
	canvas.roundedRectangleBorder(0.0f, 0.0f, width(), height(), ExpandedOverlay::kCornerRadius,
	                              1.0f);

	const visage::Bounds title = titleBounds();
	// AND THE BAR STAYS, at full strength, beside the title — the card's head wears the same one in the
	// same place, and that pairing is the question a hand actually asks ("which card opened this?").
	// The edge says *which panel*; the bar says *which card*. Two readings of one colour, and the bar
	// is the one that survives a panel being folded to its title strip.
	if (tint_ != 0) {
		canvas.setColor(tint_);
		canvas.fill(kTintBarInset, title.y() + kTintBarInset, kTintBarWidth, title.height() - 2.0f * kTintBarInset);
	}
	canvas.setColor(kInk);
	drawTracked(canvas, title_, font(kControlSize), kControlTracking,
	            titleX(), title.y(), title.height());
	if (!folded_) {
		canvas.setColor(withAlpha(kLine, 0.7f));
		canvas.fill(ExpandedOverlay::kPanelPad, title.bottom() - 1.0f,
		            std::max(0.0f, title.width() - 2.0f * ExpandedOverlay::kPanelPad), 1.0f);
	}

	// THE DEVICE'S ENABLE CHIP — `FxCard::drawEnableSwitch`'s two shapes in its two colours, because it
	// is the same control and a player should not have to learn it twice: filled accent when on,
	// outlined muted when off. DRAWN FOLDED TOO — the strip is still the window's head, and the card's
	// folded head keeps its square for the same reason. Never dimmed: it is the way back in.
	if (const visage::Bounds sw = enableBounds(); sw.width() > 0.0f) {
		canvas.setColor(enableShown_ ? kAccent : kMuted);
		if (enableShown_) {
			canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
		}
		else {
			canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
		}
		// AS A DESTINATION (backlog KW), `SwitchChip`'s marks: armed, a glow in the modulation colour; routed, a ring.
		if (enableMarks_.glow) {
			canvas.setColor(withAlpha(kModulation, 0.35f));
			canvas.roundedRectangle(sw.x() - 2.0f, sw.y() - 2.0f, sw.width() + 4.0f, sw.height() + 4.0f, 4.0f);
			canvas.setColor(kModulation);
			canvas.roundedRectangleBorder(sw.x() - 2.0f, sw.y() - 2.0f, sw.width() + 4.0f, sw.height() + 4.0f, 4.0f, 1.0f);
		}
		else if (enableMarks_.ring) {
			canvas.setColor(withAlpha(kModulation, 0.9f));
			canvas.ring(sw.right() + 2.0f, sw.y() + sw.height() * 0.5f - 2.5f, 5.0f, 1.0f);
		}
	}

	// THE ✕, AS GEOMETRY AND NOT A GLYPH — this font has no guaranteed multiplication sign that
	// centres, and a missing glyph is an invisible control.
	const visage::Bounds close = closeBounds();
	const float inset = 3.0f;
	canvas.setColor(kMuted);
	canvas.segment(close.x() + inset, close.y() + inset, close.right() - inset,
	               close.bottom() - inset, 1.4f, true);
	canvas.segment(close.right() - inset, close.y() + inset, close.x() + inset,
	               close.bottom() - inset, 1.4f, true);
	// THE MAXIMIZE SQUARE, AS GEOMETRY: one outline square to grow, two offset ones (the restore mark) while maximized.
	{
		const visage::Bounds mx = maximizeBounds();
		canvas.setColor(kMuted);
		if (maximized_) {
			canvas.roundedRectangleBorder(mx.x() + 3.0f, mx.y() + 5.0f, 6.0f, 6.0f, 1.0f, 1.2f);
			canvas.roundedRectangleBorder(mx.x() + 5.0f, mx.y() + 3.0f, 6.0f, 6.0f, 1.0f, 1.2f);
		}
		else {
			canvas.roundedRectangleBorder(mx.x() + 3.0f, mx.y() + 3.0f, mx.width() - 6.0f, mx.height() - 6.0f, 1.0f, 1.2f);
		}
	}
	// THE FOLD CHEVRON, THE SAME WAY: pointing down while open (fold me), right while folded (open me).
	const visage::Bounds fold = foldBounds();
	const float fx = fold.x() + fold.width() * 0.5f;
	const float fy = fold.y() + fold.height() * 0.5f;
	const float arm = 3.5f;
	if (folded_) {
		canvas.segment(fx - arm * 0.5f, fy - arm, fx + arm * 0.5f, fy, 1.4f, true);
		canvas.segment(fx + arm * 0.5f, fy, fx - arm * 0.5f, fy + arm, 1.4f, true);
		return; // a strip has no grip
	}
	canvas.segment(fx - arm, fy - arm * 0.5f, fx, fy + arm * 0.5f, 1.4f, true);
	canvas.segment(fx, fy + arm * 0.5f, fx + arm, fy - arm * 0.5f, 1.4f, true);
	if (maximized_) {
		return; // the overlay owns the rectangle, so there is nothing to drag
	}
	// THE GRIP: three short diagonals in the corner, the mark every desktop reads as "drag me to resize".
	const visage::Bounds grip = gripBounds();
	canvas.setColor(withAlpha(kMuted, 0.8f));
	for (int i = 0; i < 3; ++i) {
		const float step = 4.0f * static_cast<float>(i + 1);
		canvas.segment(grip.right() - 3.0f - step, grip.bottom() - 3.0f, grip.right() - 3.0f,
		               grip.bottom() - 3.0f - step, 1.2f, true);
	}
}

visage::Bounds ExpandedOverlay::titleBounds() const {
	const visage::Bounds panel = panelBounds();
	if (panel.width() <= 0.0f) {
		return {};
	}
	return { panel.x(), panel.y(), panel.width(), kTitleHeight };
}

visage::Bounds ExpandedOverlay::closeBounds() const {
	const Slot* top = front();
	const visage::Bounds panel = panelBounds();
	if (panel.width() <= 0.0f || top == nullptr) {
		return {};
	}
	const visage::Bounds close = top->panel->closeBounds();
	return { panel.x() + close.x(), panel.y() + close.y(), close.width(), close.height() };
}

visage::Bounds ExpandedOverlay::contentBounds() const {
	const Slot* top = front();
	const visage::Bounds panel = panelBounds();
	if (panel.width() <= 0.0f || top == nullptr) {
		return {};
	}
	const visage::Bounds box = top->panel->contentBounds();
	return { panel.x() + box.x(), panel.y() + box.y(), box.width(), box.height() };
}

bool ExpandedOverlay::outside(visage::Point point) const {
	if (slots_.empty()) {
		return false; // nothing open: nothing is outside it either
	}
	for (const auto& slot : slots_) {
		if (panelBoundsOf(*slot).contains(point)) {
			return false;
		}
	}
	return true;
}

void ExpandedOverlay::resized() {
	for (const auto& slot : slots_) {
		const visage::Bounds panel = panelBoundsOf(*slot);
		slot->panel->setBounds(panel.x(), panel.y(), panel.width(), panel.height());
		visage::Bounds box = slot->panel->contentBounds();
		if (slot->maximized) {
			// A CONTENT WITH A CEILING (backlog EI) is not stretched past it: the panel fills the overlay and
			// the content keeps the most it can use, at the top left.
			const visage::Point ceiling = ceilingOf(*slot->content);
			box = { box.x(), box.y(), std::min(box.width(), ceiling.x), std::min(box.height(), ceiling.y) };
		}
		slot->content->setBounds(box.x(), box.y(), box.width(), box.height());
	}
}

void ExpandedOverlay::draw(visage::Canvas& canvas) {
	// FRAMES RETIRED BY A DISMISSAL ARE FREED HERE, where no event dispatch is in flight. See
	// `retired_`: the ✕ closes from inside a mouse handler and visage is still walking the tree.
	reapParked(retired_); // NOT clear() — backlog HL; see `reapParked`
	// AND NOTHING IS DRAWN. This layer used to paint a scrim here, "dimming what is behind it" — and on
	// 2026-09-07 the design became not dimming the synth behind the window: floating panels over a synth
	// that is still being played. Each panel is a child that draws
	// itself and owns its own hit region; everywhere else is the editor, in its own colours, taking
	// its own presses.
	(void)canvas;
}

} // namespace feathers::ui
