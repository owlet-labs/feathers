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

#pragma once

#include <visage/ui.h>

#include "backplane/device_host.h"
#include "device/modulatable.h"
#include "device/registry.h"
#include "plugin/ui/escape_stack.h"
#include "plugin/ui/frame_watchers.h"
// FOR `DeviceNest` AND `instanceTintFor` — the panel's tint and the key it is filed under are both
// derived from the same pair, and `theme.h` is where that derivation lives.
#include "plugin/ui/theme.h"
#include "plugin/ui/mod_readout_layer.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/window_controls_side.h"

#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

// A DEVICE'S EXPANDED VIEW, AS A LAYER OVER THE EDITOR.
//
// THE DECISION, 2026-08-24: of three options — the centre stage as a third mode, an overlay, or the
// lane widening in place — the choice was **a panel floating above the editor, dimming what is behind
// it.**
//
// WHAT THAT SETTLED IS A QUESTION THAT THEN DID NOT NEED ANSWERING. The dual-view brief named "who
// owns the stage" as its hardest piece. Nobody does, because the expanded view does not take it:
// `Explore`/`Perform` stays a two-way switch, which is exactly what the centre-stage option would
// have cost.
//
// THE OVERLAY IS A STEP AND NOT THE DESTINATION. The eventual goal is a full popup: a real detached
// window. **That does not change what is built here; it
// changes what must not be baked in**, and the four properties below are what make the second
// implementation a re-parent instead of a rewrite:
//
//   1. THE CONTENT TAKES ITS DATA FROM THE BRIDGE, NEVER FROM ITS PARENT. A detached window has no
//      editor above it to ask. This layer therefore hands its content nothing but a place to draw.
//   2. NOTHING IS COMPUTED IN THE EDITOR'S COORDINATES. Every rectangle here comes from this frame's
//      own width and height. A panel positioned relative to the lane it came from would be a panel
//      that cannot leave the window.
//   3. THE CONTENT ASSERTS A MINIMUM AND NOTHING MORE. A window is resizable; an overlay is not.
//      `minimumSize` is the one size fact the content is allowed to state, and it is the same fact a
//      window manager will want.
//   4. DISMISSAL IS ONE CALLBACK. The content never closes itself and never knows what closed it, so
//      the same content is dismissed by a window's title bar later without learning anything new.
//   5. MODULATION STATE IS INJECTED, AND THE CONTENT IS ON THE NOTIFY ROAD IN ITS OWN RIGHT. Modulation
//      must still reach the expanded view, and the design must plan for that in a popup too. It is
//      possible, and what makes it possible is a property to keep
//      rather than a feature to build: ARMING IS STATE, NOT GEOMETRY. A source is armed in
//      `ModState` and a destination glows because it ASKS — nothing requires the armed arrow and the
//      glowing control to share a window, since both are the same process reading the same state.
//      **What would break it is reaching `ModState` through the frame tree**, which is property 1
//      wearing another hat. So the layer takes a `FrameWatchers*` and puts its content on the state
//      channel itself, rather than the content riding the editor's repaint because it happens to be
//      a descendant of it.
//
// AND THE REASON THE SHAPE IS THIS SHAPE: `HarmScope` was built first, before any of this existed,
// precisely because a `visage::Frame` does not care who hosts it. The intended host was the centre
// stage at the time; it became an overlay, and will become a window. **The content has not had to
// change for either.**
//
// DECISION REVERSED, 2026-08-24, AND IT IS WORTH THE PARAGRAPH BECAUSE THE REASONING WAS THE PROBLEM.
//
// The layer's defaults said "click outside closes it. It is a VIEW, not a modal edit — nothing to
// commit or lose, so the cheapest dismissal is the right one." **That crashed the plugin, and it also
// stopped modulation reaching the panel, and those are one bug.**
//
// ROUTING MODULATION IS A GESTURE THAT SPANS SURFACES: arm a source in the mod panel, then click a
// destination on this one. For an outside click to DISMISS, the layer must first SWALLOW it — and
// swallowing is exactly what stops the arm from happening. **The dismissal and the arming want the
// same press, and only one of them can have it.** So the panel is not "a view with nothing to lose";
// it is the destination of an in-flight gesture, and dismissing mid-gesture destroys the target.
//
// THE MECHANISM MAKES IT STRUCTURAL RATHER THAN A POLICY CHOICE. Neither `Frame::containsPoint` nor
// `Frame::frameAtPoint` is virtual, so a frame's RECTANGLE is its hit region. A layer that covers the
// editor in order to dim it takes every press in the editor, full stop. **The dimming and the hit
// region had to stop being the same frame**, which is what `ExpandedPanel` is.
//
// SO: the scrim draws and takes nothing; the panel takes its own presses; a press anywhere else
// reaches the editor beneath and arms a source there. Esc and the ✕ remain, and both are unambiguous.
//
// DO NOT COST THE DETACHED WINDOW FROM THIS. A CLAP plugin opening a top-level window has host
// integration, focus and always-on-top behaviour that differ per host, and this project keeps
// `cyclehost` for the day editor lifetime bit it. Measure it in a host when it is time.

namespace feathers::ui {

class FrameWatchers;
class ModState;
class EditHistory;
class SurfaceLayers;
class Notifier;

/// WHAT A DEVICE'S EXPANDED CONTENT MUST BE ABLE TO ANSWER, and it is deliberately almost nothing —
/// two questions, both about presentation, neither about where it is.
class ExpandedView : public visage::Frame, public TooltipHost {
public:
	/// **THE TOOLTIP LAYER, AS `FxCard` CARRIES ONE** — a panel's knobs and boxes are the same widgets a
	/// card seats, and they find the nearest host; a panel that holds a whole card (SKLITTER's inner
	/// chain) leaves those controls to the card's own layer, which is nearer. See `ui/tooltip.h`.
	ExpandedView() {
		addChild(&tips_);
		tips_.watch(*this);
		addChild(&modReadout_); // the as-modulated readout (backlog PU), as `FxCard` carries one
		modReadout_.watch(*this);
	}

	TooltipLayer& tooltipLayer() override { return tips_; }

	/// The smallest this content can be drawn at, in logical pixels. ONE OF THE TWO SIZE FACTS THE
	/// CONTENT IS ALLOWED TO ASSERT, the other being the ceiling below: a window is resizable and an
	/// overlay is not, so a content that knew its exact size would be a content that could not be put
	/// in a window. A floor and a ceiling are limits on a hand, never a size.
	virtual visage::Point minimumSize() const = 0;

	/// **THE LARGEST SIZE THIS CONTENT HAS A USE FOR, PER AXIS, in logical pixels** — backlog EI,
	/// 2026-09-24: the window size is capped. Above it the content has nothing to put in the room: a grid of
	/// fixed cells laid out from the top-left leaves the slack as empty surface below and to the right,
	/// which looks like a window that lost its contents.
	///
	/// **THE DEFAULT IS NO CAP, AND A PANEL OPTS IN PER AXIS.** A content that stretches something into
	/// the room — a plane, a scope, a graph, a pad, the perform circle, a chain's editor — leaves that
	/// axis at `kNoCap`; one whose axis is all fixed cells caps it at its natural size plus a margin of
	/// its OWN padding, read from its layout rather than spelled. Knob cells spreading apart does not
	/// count as using the room: it moves nothing into it.
	///
	/// It is a CEILING and not a size, as `minimumSize` is a floor: the host floors it at the minimum,
	/// so a panel cannot cap itself below what it asked for. **A change to it is announced by the same
	/// `minimumSizeChanged()`** — every panel whose cap moves also moves its minimum, or moves a rail
	/// that already announces — and a window above its new cap is drawn at the cap on that pass.
	[[nodiscard]] virtual visage::Point maximumSize() const { return { kNoCap, kNoCap }; }
	/// No ceiling on this axis.
	static constexpr float kNoCap = std::numeric_limits<float>::infinity();

	/// What the layer writes above it. The DEVICE's name, so the panel says which device it is —
	/// the same thing the card's head says, asked of the same authority.
	virtual std::string title() const = 0;
	/// **THE TEXT THE HEADER SHOWS, WHICH MAY FOLLOW STATE** — `title()` by default. `title()` stays the panel's IDENTITY
	/// (the overlay keys a remembered placement by it), so a panel whose header names something that moves — the spectrum
	/// panel names the selected sound — answers here and keeps its title fixed. Re-read every tick.
	[[nodiscard]] virtual std::string headerTitle() const { return title(); }

	/// **A CONTROL THAT BELONGS IN THE TITLE ROW** — the one place a panel has that is the WINDOW's
	/// rather than the content's (backlog FO: SKLITTER's preset browser, in the window header when it
	/// fits). The view OWNS the frame and says what it is; the panel that presents the view
	/// parents it into its title row, right-aligned against the fold chevron, and takes it back out
	/// before either is retired. Null — the default — for a view with nothing to put there.
	///
	/// ASKED ONCE, at `show`: a view that wants a different accessory later is a different panel.
	[[nodiscard]] virtual visage::Frame* titleAccessory() const { return nullptr; }

	/// ADVANCE WHATEVER THIS PANEL DRAWS FROM A LIVE ENGINE READING, and say whether the screen is
	/// out of date — the same contract `FxCard::update` carries, and needed separately because an
	/// open panel owns its OWN widgets. OWLTT's expanded view builds a second `OwlttGraph`, so a road
	/// that only updated the card would leave the panel's meters dead while the card's moved: the
	/// exact half-wired failure this device has already been through three times.
	///
	/// **NOT VIRTUAL, AND DELIBERATELY SO — DO NOT "RESTORE" A VIRTUAL `update()`.** It was virtual,
	/// and a panel that wanted to follow a moving reading had to override it and remember to look at
	/// `modulatedFieldAt`; both panels gated on a settings snapshot that cannot see modulation move.
	/// So the modulation dot moved only when the underlying parameter was edited. The base owns
	/// the walk now and a panel overrides `updateContent()` for what is its own — the same decision, and
	/// the same reason, as `FxCard::update`.
	///
	/// **THE WALK NEEDS A DEVICE, AND A PANEL THAT DOES NOT NAME ONE SIMPLY DOES NOT GET IT.** Unlike
	/// `FxCard`, this base is a bare `visage::Frame` with no device of its own, so it ASKS through the
	/// three accessors below. A panel that answers them is watched by construction; one that does not
	/// keeps exactly the behaviour it had. That is a weaker guarantee than the card's and it is the
	/// honest one — inventing a device for a panel that has none would be worse than not watching it.
	bool update() {
		bool told = modulationMoved();
		told = updateContent() || told;
		told = tips_.advance() || told;
		told = modReadout_.advance() || told;
		return told;
	}

	/// WHAT THIS PANEL ADVANCES BEYOND ITS CONTROLS' MODULATION. Override this, never `update()`.
	virtual bool updateContent() { return false; }

	/// **WHICH DEVICE THIS PANEL IS SHOWING** — answer all three and the base watches its controls'
	/// modulation for you; answer none and it does not. There is no half state: `watchedDescriptor()`
	/// returning non-null with no host is treated as no device at all.
	/// **ALL THREE ARE STAMPED BY THE FACTORY** (`makeExpandedView`, through `setWatched`), so the
	/// walk above runs for every panel it builds. MEASURED 2026-09-14: no panel in the repo had ever
	/// overridden `watchedDescriptor` or `watchedHost` — `git log -S` finds no such line — so the
	/// walk that fixed the modulation dot moving only on an edit of the underlying parameter was
	/// live in its test's own view and in no real panel. A panel's ring followed modulation only
	/// while something ELSE dirtied the window, so the modulation glow did not reach the expanded
	/// panel's controls.
	virtual const device::Descriptor* watchedDescriptor() const { return descriptor_; }
	virtual const backplane::DeviceHost* watchedHost() const { return host_; }
	/// **THE HOST IS TAKEN MUTABLE, AND THE NOTIFIER WITH IT**, because the window header's enable chip
	/// writes through them (`toggleDeviceEnable`). The watch above only reads, and still asks through
	/// the const accessor.
	void setWatched(backplane::DeviceHost* host, const device::Descriptor* descriptor, DeviceInstanceId device,
	                Notifier* notifier = nullptr) {
		host_ = host;
		descriptor_ = descriptor;
		device_ = device;
		notifier_ = notifier;
		enableSwitch_.reset();
		// **THE DEVICE IS HALF OF `innerNest()`, SO CHANGING IT MOVES THAT TOO.** A panel holding a
		// chain is stamped by the factory AFTER it is constructed, so a hook that fired only on
		// `setNest` would never fire at all for a TOP-LEVEL panel — whose nest is the default and whose
		// inner nest very much is not. That was the first version, and it left every top-level
		// SKLITTER's chain wearing the host's own tints.
		onInnerNestChanged();
	}
	/// **THE DEVICE IS STAMPED BY THE FACTORY, NOT ANSWERED BY EACH PANEL.** `makeExpandedView` is
	/// handed the instance and sets it here; a panel may still override. It was a virtual that every
	/// one of the seven real panels left at its default, so every instance of one kind keyed the
	/// overlay alike (`ExpandedOverlay::keyOf` is title#device): a second instance of a device
	/// REPLACED the first, in the first's remembered place, under the same title — so a second instance
	/// could not open in the canvas until the first was closed — and its tint was the no-device tint,
	/// so the canvas windows showed none. The tests answered the
	/// question in their own views and so never met the default.
	virtual DeviceInstanceId watchedDevice() const { return device_; }
	void setWatchedDevice(DeviceInstanceId device) { device_ = device; }

	/// **AND WHICH CHAIN THAT DEVICE IS IN, WHICH IS THE OTHER HALF OF ITS IDENTITY.** See
	/// `DeviceNest`: a device id is unique within its own chain and not across nesting, so `keyOf`
	/// — the thing that decides "one panel per device" — needs both or it retires a panel that is
	/// about a different device. Stamped by whoever PRESENTS the view, because only the presenter
	/// knows which chain it is reaching into; the default is the host's own chain.
	void setNest(DeviceNest nest) {
		if (nest == nest_) {
			return;
		}
		nest_ = nest;
		onInnerNestChanged();
	}
	/// **VIRTUAL FOR THE SAME REASON `watchedDevice` IS, AND IT WAS NOT AT FIRST.** What the overlay
	/// keys and tints is never the panel a presenter built — it is the `MountedFrame` wrapped round it,
	/// which is an `ExpandedView` in its own right and carries its own default of everything. MEASURED
	/// on the private display before this was virtual: the CARD in SKLITTER's chain wore its nested
	/// hue and the PANEL it opened wore the host's, one line apart in one capture, because the mount
	/// answered for the device and not for the chain. `MountedFrame` overrides both together now.
	[[nodiscard]] virtual DeviceNest nest() const noexcept { return nest_; }
	/// **THE CHAIN A PANEL OF ITS OWN WOULD OPEN INTO** — this view's nest, with its own device mixed
	/// in. A panel that holds a chain (`SklitterExpanded`, `ChainExpanded`) hands this to whatever
	/// draws that chain, so the cards inside it are stamped one level deeper. Asked here rather than
	/// spelled at each call site, so the two hosts that nest cannot arrive at different answers.
	[[nodiscard]] DeviceNest innerNest() const { return DeviceNest::under(nest(), watchedDevice()); }

	/// HAND THE CONTENT THE MODULATION STATE, before it is shown. A control that has not been told
	/// never lights while mapping is armed, and reports itself un-armable forever.
	///
	/// **VIRTUAL, BECAUSE gui.cpp WAS ASKING EVERY PANEL BY NAME AND HAD STOPPED ASKING SOME.** It
	/// cast to `HarmExpanded`, then — after HARM shipped the bug — to `OwlttExpanded`, and its own
	/// comment recorded the rule: *"two is the point at which this stops being an omission and
	/// becomes a pattern, so the next device gets a virtual `setModulation` on the base rather than
	/// a third branch here."* By the time a fourth device arrived there were still two casts and
	/// **`WaypointExpanded` had never been given one** — its panel was built, shown, and never handed
	/// the state, so every control on it reported itself a destination and none was armable. That is
	/// the identical failure HARM shipped, sitting unnoticed behind the note predicting it.
	///
	///
	/// **NOT VIRTUAL ANY MORE, AND A PANEL OVERRIDES `modulationBound` INSTEAD** (2026-09-26). The base
	/// needs the two pointers for itself — the window header's enable chip records its undo step through
	/// `history` — and a virtual that every panel overrides without calling up is a virtual the base
	/// never hears. The same shape as `update()` / `updateContent()`, for the same reason.
	void setModulation(ModState* mod, EditHistory* history) {
		mod_ = mod;
		history_ = history;
		enableSwitch_.reset();
		modulationBound(mod, history);
	}

	/// **THE DEVICE'S ENABLE, AS THE WINDOW HEADER'S CHIP READS AND WRITES IT** (devices had been
	/// adding their own active chips in their windows, inconsistently; the enable belongs in the window
	/// header). ONE CHIP, drawn by the panel chrome
	/// (`ExpandedPanel::enableBounds`) for every device, over the SAME control the card's head square
	/// switches — `Traits::enableControl`, asked of the descriptor — and the same road: a
	/// `DeviceControl` bracketed in a gesture, which is where the undo step is recorded
	/// (`FxCard::toggleEnable`'s note). A panel draws no enable of its own in its body any more.
	///
	/// `kNoEnable` when the view names no device, or its device declares no enable — and then the header
	/// draws no chip, which is the honesty rule: a switch appears exactly when there is one to switch.
	/// VIRTUAL so `MountedFrame` can answer for the view it wraps; no panel overrides these.
	static constexpr size_t kNoEnable = static_cast<size_t>(-1);
	[[nodiscard]] virtual size_t deviceEnableControl() const;
	[[nodiscard]] bool hasDeviceEnable() const { return deviceEnableControl() != kNoEnable; }
	/// READ FROM THE HOST'S FIELD, as `FxCard::headEnabled` reads it — one answer, the number the
	/// engine reads. True when there is no enable: a device without a switch is always in.
	[[nodiscard]] virtual bool deviceEnabled() const;
	/// FLIP IT. Returns the new state, or `nullopt` when there is nothing to flip.
	virtual std::optional<bool> toggleDeviceEnable();
	/// **THE ENABLE AS A MODULATION DESTINATION (backlog KW)** — the chip's marks, as `SwitchChip`'s are drawn: armed and
	/// routable, it glows; routed and not armed, a ring. Nothing for a view whose device has no enable.
	struct EnableMarks {
		bool glow = false;
		bool ring = false;
		friend bool operator==(const EnableMarks&, const EnableMarks&) = default;
	};
	[[nodiscard]] virtual EnableMarks deviceEnableMarks() const;
	/// ARMED AND ROUTABLE: a press routes the armed source into the enable at full amount, or unroutes it if it is
	/// routed already (a gate has no depth worth a drag). Returns whether it did; false leaves the press to the toggle.
	virtual bool routeDeviceEnable();

	/// WHICH SURFACE HOST THIS VIEW IS PRESENTED ON, told by `SurfaceLayers` as it mounts the view —
	/// rack roadmap §5's escalation. A view that HOSTS A CHAIN of its own (`ChainExpanded`) hands
	/// this to its inner editor, whose presents then go to the outermost host rather than into a panel
	/// inside a panel — the clipping-container problem `surface_host.h` was written about. Every
	/// other view ignores it, which is the default.
	virtual void setPresentedOn(SurfaceLayers* host) { (void)host; }

	/// **THE MINIMUM IS READ LIVE, BUT LIVE IS NOT AUTOMATIC: SOMETHING HAS TO ASK.**
	///
	/// `ExpandedOverlay::panelSizeOf` calls `minimumSize()` on every layout pass, so a content whose
	/// minimum has moved is drawn at its new size the next time the layer lays out — and nothing made
	/// that pass happen. It went unnoticed while every content's minimum was a constant. The first one
	/// that changes its own (`SklitterExpanded`, whose two rails take width when they open) was
	/// re-fitted only because `widget-shot` called `overlay->resized()` by hand after setting them; in
	/// the editor the rail opened inside the old rectangle and was clipped.
	///
	/// **TOLD, NOT POLLED — the rail's own rule, one layer up.** The host owns the arithmetic, the
	/// content announces, and there is exactly one place deciding how big a panel is. A poll on the
	/// tick would be the same answer computed for every panel on every frame to catch a change that
	/// happens when a hand presses a chevron.
	void onMinimumSizeChanged(std::function<void()> callback) {
		minimumChanged_ = std::move(callback);
	}

protected:
	/// WHAT A PANEL DOES WITH THE MODULATION STATE — hand it to its knobs and adapters. A default that
	/// does nothing, so a panel with no modulatable controls need not care. See `setModulation`.
	virtual void modulationBound(ModState* mod, EditHistory* history) {
		(void)mod;
		(void)history;
	}

	/// TOLD WHEN `innerNest()` MAY HAVE MOVED — either half of it, the nest or the device — so a panel
	/// that holds a chain can re-stamp what is inside it. Every other panel ignores it, which is the
	/// default. It is named for the thing it is about rather than for the setter that fires it,
	/// because two setters fire it and a name from either one would be wrong half the time.
	virtual void onInnerNestChanged() {}

	/// A content calls this once whatever it just did has changed the answer `minimumSize()` gives.
	/// Safe with no host listening — a panel built in a case has none.
	void minimumSizeChanged() {
		if (minimumChanged_) {
			minimumChanged_();
		}
	}

private:
	/// Who to tell. Set by whichever host mounted this content, null in a case that builds one bare.
	std::function<void()> minimumChanged_;

	/// **DID ANY WATCHED CONTROL MOVE UNDER MODULATION SINCE THE LAST ASK?** `FxCard`'s walk, over the
	/// device this panel names rather than one it owns. See `update()` for why a panel that names none
	/// is left alone.
	///
	/// **`redrawAll()`, NOT `redraw()`**: a panel's controls are separate child frames with no
	/// `update()` of their own, so marking this frame alone leaves them holding the last block's ring.
	/// The difference is INVISIBLE under `FEATHERS_FULL_REDRAW`, which repaints every layer anyway —
	/// so anything asserting it must be built and run WITHOUT that flag, or it measures the flag.
	bool modulationMoved() {
		const device::Descriptor* desc = watchedDescriptor();
		const backplane::DeviceHost* host = watchedHost();
		if (desc == nullptr || host == nullptr) {
			return false;
		}
		const size_t count = desc->modulatableCount();
		bool moved = lastModulated_.size() != count;
		lastModulated_.resize(count, 0.0f);
		for (size_t ordinal = 0; ordinal < count; ++ordinal) {
			const size_t control = desc->controlOfNthModulatable(ordinal);
			const float now = host->modulatedFieldAt(watchedDevice(), control);
			if (now != lastModulated_[ordinal]) {
				lastModulated_[ordinal] = now;
				moved = true;
			}
		}
		if (moved) {
			redrawAll();
		}
		return moved;
	}

	/// Indexed by ORDINAL, as `FxCard`'s is, and for the same reason.
	std::vector<float> lastModulated_;
	/// See the constructor. A plain member: a frame's destructor detaches it from its parent.
	TooltipLayer tips_;
	ModReadoutLayer modReadout_;
	DeviceInstanceId device_ = kNoInstance;
	DeviceNest nest_ {};
	const device::Descriptor* descriptor_ = nullptr;
	backplane::DeviceHost* host_ = nullptr;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	/// THE HEADER CHIP'S CONTROL, built on first press and KEPT: a control holds the gesture state its
	/// history entry is paired with, so a fresh one per press would open a gesture on one object and
	/// close it on another (`EnvCard::enableModulatable`'s note). Dropped when the device or the
	/// history changes, so it is never holding a stale one.
	mutable std::unique_ptr<device::Modulatable> enableSwitch_;
	/// The adapter the chip reads and writes through, made on first use; null when there is nothing to switch.
	[[nodiscard]] device::Modulatable* enableSwitch() const;
};

/// THE PANEL ITSELF — the chrome, and the frame the content is a child of.
///
/// IT IS A SEPARATE FRAME FROM THE LAYER AND THAT IS THE WHOLE FIX. visage resolves a press by
/// walking frames and asking each `containsPoint`, and NEITHER `containsPoint` NOR `frameAtPoint`
/// IS VIRTUAL — so a frame's rectangle IS its hit region, and a layer covering the editor swallows
/// every press in it. That is not a thing that can be arranged around; the rectangle has to be the
/// panel.
class ExpandedPanel : public visage::Frame {
public:
	/// `onDragBegin` fires on a press in the head (not on the ✕); `onDragTo` follows with the
	/// pointer's displacement from that press, in WINDOW pixels — the panel moves under the hand, so
	/// its own coordinates shift with every step and only the window's stay still.
	ExpandedPanel(std::function<void()> onClose, std::function<void()> onDragBegin,
	              std::function<void(visage::Point)> onDragTo, std::function<void()> onResizeBegin = nullptr,
	              std::function<void(visage::Point)> onResizeTo = nullptr,
	              std::function<void()> onRaise = nullptr, std::function<void()> onFold = nullptr,
	              std::function<void()> onMaximize = nullptr)
	    : onClose_(std::move(onClose)), onDragBegin_(std::move(onDragBegin)),
	      onDragTo_(std::move(onDragTo)), onResizeBegin_(std::move(onResizeBegin)),
	      onResizeTo_(std::move(onResizeTo)), onRaise_(std::move(onRaise)), onFold_(std::move(onFold)),
	      onMaximize_(std::move(onMaximize)) {}

	void draw(visage::Canvas& canvas) override;

	visage::Bounds titleBounds() const;
	visage::Bounds closeBounds() const;
	/// THE FOLD — a chevron beside the ✕ (backlog EE, 2026-09-15: a card that folds to its title strip
	/// so several fit a row). A press here, or a DOUBLE-CLICK anywhere on the head, asks the overlay
	/// to fold this panel to its title strip or open it again; the overlay decides the size and keeps
	/// the answer per device. Folded, the panel has no grip and no content: a strip a hand can still
	/// drag, close, and reopen.
	visage::Bounds foldBounds() const;
	void setFolded(bool folded) { folded_ = folded; }
	[[nodiscard]] bool folded() const noexcept { return folded_; }
	/// **THE MAXIMIZE BUTTON** (backlog OV: 2026-10-29), one gap inboard of the fold chevron: a press asks the
	/// overlay to fill its own rectangle with this panel, or to put it back exactly where it was. Maximized, the
	/// panel has no grip and its head no longer drags: the overlay owns the rectangle until it is restored.
	visage::Bounds maximizeBounds() const;
	void setMaximized(bool maximized) { maximized_ = maximized; }
	[[nodiscard]] bool maximized() const noexcept { return maximized_; }
	visage::Bounds contentBounds() const;
	/// THE RESIZE GRIP — the panel's bottom-right corner (the floating window is resizable). A press
	/// here begins a resize; the overlay clamps the size to the content's
	/// minimum and its own bounds, and remembers it per device as it remembers the placement.
	visage::Bounds gripBounds() const;
	static constexpr float kGripSize = 16.0f;
	[[nodiscard]] bool resizingForTest() const noexcept { return resizing_; }

	void setTitle(std::string title) { title_ = std::move(title); }
	[[nodiscard]] const std::string& title() const noexcept { return title_; }

	/// **THE CONTENT'S TITLE-ROW CONTROL** (`ExpandedView::titleAccessory`), laid out by `resized` and
	/// NOT OWNED: the view owns it. Null to take it out — which the overlay does before the view and
	/// this panel are retired, so neither is ever left listing a frame the other destroyed.
	void setAccessory(visage::Frame* accessory);
	[[nodiscard]] visage::Frame* accessory() const noexcept { return accessory_; }
	/// Where the accessory goes: right-aligned one gap left of the fold chevron, vertically centred in
	/// the title row, at its own height. EMPTY when the row is too narrow to hold it clear of the
	/// title text — a control drawn over the name is worse than one that is not there (and the panel's
	/// minimum width is what keeps SKLITTER out of that case; see `SklitterExpanded::minimumSize`).
	[[nodiscard]] visage::Bounds accessoryBounds() const;
	void resized() override;

	/// **THE DEVICE'S ENABLE CHIP, IN THE TITLE ROW — ONE FOR EVERY WINDOW**. The
	/// panel draws it and takes its press; `source` answers what it shows and does the write
	/// (`ExpandedView::toggleDeviceEnable`). NOT OWNED — the overlay's slot owns the view, and takes the
	/// source back out (`nullptr`) before either frame is retired, as it does the accessory.
	void setEnableSource(ExpandedView* source);
	/// WHERE THE CHIP IS: the card head's square (`FxCard::enableBounds`, 12px, rounded 3), one gap left
	/// of the fold chevron — the card puts its square inboard of the host's marks, and so does this.
	/// EMPTY when the source has no enable, so the press test refuses it without a branch.
	[[nodiscard]] visage::Bounds enableBounds() const;
	static constexpr float kEnableSize = 12.0f;
	/// WHERE THE TITLE TEXT STARTS: the pad on the right-hand setting, past the control cluster on the left
	/// one (backlog OD).
	[[nodiscard]] float titleX() const;
	/// **THE HEAD FOLLOWS THE DESK'S SIDE** (`windowControlsSide`): re-lays the accessory and repaints when
	/// the side moved since this panel last looked. Called by the overlay's tick; returns whether it asked
	/// for a repaint, the count the repaint rule wants produced by the notification itself.
	bool syncSide();
	/// **REPAINT THE CHIP IF THE FIELD MOVED UNDER IT** — the card's head, an undo, a patch, automation.
	/// Called by the overlay's tick; returns whether it asked for a repaint, the count the repaint rule
	/// wants produced by the notification itself.
	bool syncEnable();
	/// What the chip was last PAINTED as, for a case: the value `draw` reads.
	[[nodiscard]] bool enableShownForTest() const noexcept { return enableShown_; }
	[[nodiscard]] ExpandedView::EnableMarks enableMarksForTest() const noexcept { return enableMarks_; }

	/// THE INSTANCE'S TINT, drawn as a bar at the left of the title row — the same colour the card's
	/// head wears (`CardHeadMarks::tint`), so a hand can pair a floating panel with its card. Zero: none.
	void setTint(uint32_t tint) { tint_ = tint; }
	[[nodiscard]] uint32_t tintForTest() const noexcept { return tint_; }
	/// THE COLOUR THE BORDER IS DRAWN IN — the instance's edge (`instanceEdgeColor`), the same one its
	/// card's selected outline wears, or the plain line for an untinted panel. `draw` paints exactly this.
	[[nodiscard]] uint32_t edgeColor() const noexcept;

	/// INSTALL THE HIT BEHAVIOUR. Done on first `show` rather than in the constructor because the
	/// flags it sets are the ones that were wrong, and a single place to set them is a single place
	/// to read them.
	void wire();

private:
	std::function<void()> onClose_;
	std::function<void()> onDragBegin_;
	std::function<void(visage::Point)> onDragTo_;
	std::function<void()> onResizeBegin_;
	std::function<void(visage::Point)> onResizeTo_;
	/// TOLD ON RELEASE, so the layer can bring this panel to the front. On release and not on the
	/// press, because raising re-orders the layer's children and `removeChild` clears the frame a drag
	/// is being delivered to — the `raiseTopLayers` lesson, one door over.
	std::function<void()> onRaise_;
	std::function<void()> onFold_;
	std::function<void()> onMaximize_;
	std::string title_;
	visage::Frame* accessory_ = nullptr;
	ExpandedView* enableSource_ = nullptr;
	bool enableShown_ = true;
	ExpandedView::EnableMarks enableMarks_ {};
	WindowControlsSide shownSide_ = windowControlsSide();
	uint32_t tint_ = 0;
	bool folded_ = false;
	bool maximized_ = false;
	bool wired_ = false;
	/// THE HEAD IS BEING HELD, and where in the window the hand took it.
	bool dragging_ = false;
	visage::Point dragOrigin_ { 0.0f, 0.0f };
	bool resizing_ = false;
	visage::Point resizeOrigin_ { 0.0f, 0.0f };
};

class ExpandedOverlay : public visage::Frame {
public:
	ExpandedOverlay();
	/// **IT EXISTS FOR ONE REASON AND THE REASON IS `retired_` — backlog HL.** There was no destructor
	/// here, so the fourth and last emptying of the park list was `~vector`'s own during member
	/// teardown, which has the identical property `clear()` does: it destroys `[_M_start, _M_finish)`
	/// with `_M_finish` at the old end, and a `push_back` reached from inside one of those destructors
	/// frees the buffer the loop is walking. MEASURED as a heap-use-after-free, not reasoned: see the
	/// definition, and the case named `backlog HL ... the DESTRUCTOR road`.
	~ExpandedOverlay() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// A PANEL THIS LAYER HOLDS. Monotonic and never reused — the `SurfaceId` rule one layer up: a
	/// stale handle must land on nothing rather than on whoever holds the slot now.
	using PanelId = uint64_t;
	static constexpr PanelId kNoPanel = 0;

	/// OPEN `content` AS A PANEL BESIDE THE OTHERS. **SEVERAL AT ONCE, since 2026-09-14** — multiple
	/// windows may be open, in the canvas or floating, dodged so they do not overlap where possible and
	/// overlapping only where they must. "A second overlay is a window manager, and nobody asked for
	/// one" stood here; that changed. ONE PANEL PER DEVICE still: content
	/// for a device already open replaces that device's panel rather than opening a twin, because two
	/// panels writing one device is a state nothing can address. Ownership transfers: the layer
	/// destroys the content when it is dismissed, so a re-open is always a fresh view over the same
	/// bridge rather than a stale one. Returns the panel's id, or `kNoPanel` for null content.
	PanelId show(std::unique_ptr<ExpandedView> content);

	/// CLOSE EVERYTHING. Safe when nothing is open, because every road to it can arrive when the
	/// layer is already closed.
	void dismiss();
	/// CLOSE ONE. A stale or unknown id is silence.
	void dismiss(PanelId id);

	bool open() const noexcept { return !slots_.empty(); }
	size_t panelCount() const noexcept { return slots_.size(); }

	/// **BRING THIS DEVICE'S PANEL TO THE FRONT, IF ONE IS OPEN** — the overlay's focus is its front
	/// panel (the one Esc closes, the one drawn last). A press on the device's card
	/// head focuses its window. Addressed by device AND chain, the pair `keyOf` and the tint are both
	/// filed under, so a nested device cannot raise its outer namesake. Returns false, touching
	/// nothing, when no panel is open for it. It neither unfolds the panel nor takes the keyboard:
	/// the press was on the card, and the lane's own keys (Delete on the selected device) stay with it.
	bool focusPanelFor(DeviceInstanceId device, DeviceNest nest);
	/// **CLOSE THIS DEVICE'S PANEL, IF ONE IS OPEN** — pressing a card's window-expand while its
	/// window is already open closes it. Matched exactly as `focusPanelFor`
	/// matches, on device AND chain, and closed through the one dismissal road Esc and the ✕ take, so
	/// the placement, size and fold are remembered and the panel reopens where it was left. Returns
	/// false, touching nothing, when no panel is open for it; every other device's panel is left up.
	bool dismissPanelFor(DeviceInstanceId device, DeviceNest nest);

	/// TICK EVERY OPEN PANEL, and say whether any wants the screen. Both hosts carry this because an
	/// `ExpandedView` lives in one or the other and never in both, and a road installed on only one
	/// of them is a panel whose live readings die depending on a preference.
	bool updateContent();

	/// THE STATE CHANNEL, so the content repaints when the device's row moves — from the card, from a
	/// patch load, from anywhere. A swallowed device's controls are not parameters, so this is the
	/// ONLY road that reaches them, and a panel not on it shows a value that has stopped being true.
	///
	/// THE LAYER SUBSCRIBES AND UNSUBSCRIBES AROUND THE CONTENT'S LIFETIME, which it must: the
	/// content is destroyed on dismissal, and a watcher list holding a freed frame is a
	/// use-after-free the next time anything moves.
	void setStateWatchers(FrameWatchers* watchers) { watchers_ = watchers; }

	/// THE MODULATION LIST IS A SECOND LIST AND THE OVERLAY HAD NO POINTER TO IT.
	///
	/// `setStateWatchers` above is the state road. `Knob::setModulation` separately puts every
	/// modulatable knob on `ModState`'s own `FrameWatchers` so the glow follows arming — a different
	/// list, joined by a child, and nothing here could clean it. MEASURED: opening and dismissing one
	/// expanded view left NINE stale entries on it, and the count never came down across repeated
	/// open/dismiss cycles (124 -> 133 -> 133 -> 148 -> 148).
	///
	/// THAT IS THE CRASH `96d94be` FIXED FOR `FxLane` CARDS, one door over, and it is why this takes
	/// the state rather than a bare list: the overlay must be able to clean BOTH at dismissal.
	void setModulationState(ModState* mod) { mod_ = mod; }

	/// TOLD, NOT ASKED. Installed by whoever put this layer in the tree, called after a panel's content
	/// is gone. It exists so the thing that OPENED the overlay can forget about it — the card's expand
	/// button has to stop looking pressed, and only the lane knows that. NOT told on a replacement
	/// (the caller is mid-`show`); `onDismissPanel` is, because a host keying surfaces by panel must
	/// retire the replaced one's id whatever closed it.
	std::function<void()> onDismiss;
	std::function<void(PanelId)> onDismissPanel;

	/// WHERE A PANEL PREFERS TO LAND when this device's has never been placed, in this layer's pixels.
	/// None is CENTRED over the layer (a panel in a corner would cover the rails a hand arms sources
	/// from). A region — the open canvas's floor, set by `SurfaceLayers` before each present — puts a
	/// new panel at its top-left, so panels fill rightward and have room to, and the dodge looks for a
	/// free place INSIDE it before anywhere else on the layer. A PREFERENCE, NEVER A FENCE (2026-10-03:
	/// the canvas is an extension of the main surface, not a fenced-in region of its own): every panel is
	/// clamped to the whole layer, and a hand may drag it anywhere on it.
	void setPreferredRegion(std::optional<visage::Bounds> region) { preferredRegion_ = region; }
	std::optional<visage::Bounds> preferredRegion() const noexcept { return preferredRegion_; }

	/// ESC WHEN THIS LAYER IS CLOSED — the second customer for the only keyboard road this editor
	/// has. Returns whether it consumed the key.
	///
	/// IT LIVES HERE BECAUSE THE PRECEDENCE HAS TO BE DECIDED IN ONE PLACE. Esc leaves modulation
	/// assign mode (backlog 20), specified together with the Alt bypass — one feature
	/// with two exits, and a mode with neither is a mode you are stuck in. But this layer already
	/// takes Esc, so with two independent handlers the answer to "overlay open, source armed, Esc"
	/// would be whatever visage's dispatch order happened to be. **The decision is that the thing
	/// NEAREST THE HAND wins: the overlay closes, the arming survives, and a second press disarms.**
	/// Expressed as one branch below rather than as two handlers that must not both fire.
	///
	/// AND IT IS A CALLBACK RATHER THAN A `ModState*` because this layer has no business knowing what
	/// modulation is — the same reason a device never names the editor. `Gui` owns the state and
	/// supplies the policy.
	///
	/// **SUPERSEDED IN BOTH EDITORS BY THE ESC STACK** (backlog NN: Esc closes the most recently
	/// opened): "nearest the hand" closed the window even when the arm came after it. With a stack wired
	/// (`setEscapeStack`) each panel is an entry, Esc here closes the stack's top whatever kind it is, and
	/// this is asked only when the stack had nothing. A layer with no stack (a case's bare one) keeps the
	/// old precedence.
	std::function<bool()> onEscapeWhenClosed;

	/// **THE EDITOR'S ESC STACK, asked for on each use** (`SurfaceLayers::escapeStack`, which follows the
	/// nesting). Each panel shown is an entry from then until it closes by any road; raising a panel raises
	/// its entry. Null, or a resolver that answers null, is the stackless layer described above.
	void setEscapeStack(std::function<EscapeStack*()> resolve) { escapeStack_ = std::move(resolve); }

	//----------//
	// Geometry //
	//----------//

	/// The panel itself: WHERE IT WAS LEFT, or centred in this frame until it has been dragged; and
	/// always CLAMPED INSIDE this frame, so it cannot be lost. FROM THIS FRAME'S OWN SIZE, never from
	/// the editor's: see property 2 above.
	///
	/// **FLOATING, NOT FIXED — DECIDED 2026-09-07, on the first Mac build:** back to the window inside
	/// the plugin window, but floating rather than at a fixed location, and without dimming the synth
	/// behind it. The separate OS window (DD) opened
	/// under the DAW on macOS and no ordering call fixed it, so this layer is the one road again, on
	/// every platform — and the head of the panel drags it. The synth behind stays at full
	/// brightness and takes every press this panel does not: that was already true of the hit
	/// region (see the file's top), and is now true of the pixels too.
	/// THE FRONT PANEL'S — the last opened or last raised — and a named panel's. **PLACED TO DODGE**:
	/// a new panel goes where it does not cover another if any such place exists inside this layer
	/// (nearest its preferred spot — the centre, or where this device's panel was left), and only
	/// over another when the layer is too full to avoid it.
	visage::Bounds panelBounds() const;
	visage::Bounds panelBounds(PanelId id) const;

	/// THE HEAD IS TAKEN: remember where the panel is, so the drag is an offset from there and not
	/// from wherever the last event happened to land. The no-argument forms act on the FRONT panel,
	/// which is what a case drives and what a hand has just pressed.
	void beginPanelDrag();
	/// THE HAND MOVED `delta` window pixels since it took the head: place the panel there, clamped,
	/// and remember the place for this device.
	void dragPanelBy(visage::Point delta);
	/// THE GRIP'S HALF: the size the content is given, clamped to its minimum and to this overlay.
	void beginPanelResize();
	void resizePanelBy(visage::Point delta);

	/// **FOLDED TO ITS TITLE STRIP** (backlog EE): the panel is `kTitleHeight` tall, its content hidden and
	/// its grip gone; the dodge places new panels around the strip, so several fit a row. Kept per
	/// device with the placement and the size, so a device reopens as it was left. Unfolding raises the
	/// panel and leaves it where it is — a panel that jumped on unfold would be the "where did it go" the
	/// clamp exists to prevent.
	[[nodiscard]] bool folded(PanelId id) const;
	void setFolded(PanelId id, bool folded);
	/// The front panel's fold chevron, in this overlay's coordinates — for a venue.
	visage::Bounds foldBounds() const;
	/// **MAXIMIZED** (backlog OV): the panel fills this overlay exactly (the editor, or the canvas panel it is docked
	/// in), and follows it when it resizes. Restoring puts back the placement and size the hand left, which are
	/// never touched while maximized, so the earlier bounds return to the pixel. Maximizing a folded panel
	/// unfolds it and folding a maximized one restores it first: the two exclude each other. Kept per device
	/// with the fold, for this layer's lifetime: VIEW state, never the patch's.
	[[nodiscard]] bool maximized(PanelId id) const;
	void setMaximized(PanelId id, bool maximized);
	/// The front panel's maximize button, in this overlay's coordinates — for a venue.
	visage::Bounds maximizeBounds() const;
	[[nodiscard]] bool sizedForTest() const noexcept { return !slots_.empty() && slots_.back()->sized.has_value(); }

	/// The title row, and the ✕ within it.
	visage::Bounds titleBounds() const;
	visage::Bounds closeBounds() const;

	/// Where the content is placed — the panel less its chrome.
	visage::Bounds contentBounds() const;

	/// A point is OUTSIDE when it is in this layer but not in any panel.
	///
	/// IT NO LONGER DISMISSES ANYTHING, and that is a DECISION REVERSED rather than a simplification —
	/// see the note at the top of the file. It is kept because "is this press the panel's" is still
	/// a real question and the cases ask it.
	bool outside(visage::Point point) const;

	static constexpr float kTitleHeight = 28.0f;
	static constexpr float kPanelPad = 12.0f;
	static constexpr float kCloseSize = 14.0f;
	static constexpr float kCornerRadius = 8.0f;
	// THERE IS NO SCRIM ANY MORE. `kScrimAlpha = 0.62f` dimmed the editor behind the panel, as first
	// specified; on 2026-09-07 that changed to not dimming the synth behind the window, so
	// the layer draws nothing of its own and the synth stays at full brightness under a floating panel.

	//-----------//
	// For tests //
	//-----------//

	/// The FRONT panel's content, so a case can ask it what it offers rather than taking the layer's
	/// word; and a named panel's.
	const ExpandedView* contentForTest() const { return slots_.empty() ? nullptr : slots_.back()->content.get(); }
	const ExpandedView* contentForTest(PanelId id) const;
	/// Every open panel's id, back to front.
	std::vector<PanelId> panelIdsForTest() const {
		std::vector<PanelId> out;
		for (const auto& slot : slots_) {
			out.push_back(slot->id);
		}
		return out;
	}
	PanelId frontPanelIdForTest() const noexcept { return slots_.empty() ? kNoPanel : slots_.back()->id; }

	/// WHICH ROAD CLOSED IT, so a case can prove Esc worked rather than that something did.
	/// `Outside` is retained as a value nothing produces any more, so a case asserting it never
	/// happens has a name to assert against.
	enum class Dismissal { None, Escape, Outside, CloseButton, Replaced, Programmatic, ExpandToggle };
	Dismissal lastDismissalForTest() const noexcept { return lastDismissal_; }

	/// THE PANEL FRAME, for the cases: `frameAtPoint` on the layer must resolve to a CONTROL, and a
	/// case needs the panel to ask it in the right coordinates.
	ExpandedPanel* panelForTest() const { return slots_.empty() ? nullptr : slots_.back()->panel.get(); }
	ExpandedPanel* panelForTest(PanelId id) const;

	/// Whether the front panel sits where it was put (true) or is still to be placed (false). Every
	/// shown panel is placed by `show` — dodged or remembered — so this is true once anything is open.
	bool placedForTest() const noexcept { return !slots_.empty() && slots_.back()->placed.has_value(); }

private:
	/// ONE OPEN PANEL: its chrome, its content, where and how big a hand left it, and its
	/// subscription. **THE SUBSCRIPTION BESIDE THE CONTENT** — a flat list of tokens would hold a
	/// dismissed panel on the watcher list after it was destroyed (see `FxLane::cardWatches_` for the
	/// same shape and the bug it hid); the token goes when the slot goes.
	struct Slot {
		PanelId id = kNoPanel;
		std::string key;
		std::unique_ptr<ExpandedPanel> panel;
		std::unique_ptr<ExpandedView> content;
		/// WHERE THE PANEL'S TOP-LEFT IS, in this layer's pixels — set by the dodge on show, moved by a
		/// drag, clamped inside the layer whenever it is read.
		std::optional<visage::Point> placed;
		/// THE CONTENT SIZE A HAND CHOSE, or none for the content's minimum. **KEPT AS CHOSEN, CLAMPED
		/// WHEN READ** (`contentSizeOf`): a panel whose cap drops (SKLITTER's circle closing) is drawn at
		/// the cap, and gets the hand's room back when the cap rises again.
		std::optional<visage::Point> sized;
		bool folded = false;
		bool maximized = false;
		visage::Point resizeAnchor { 0.0f, 0.0f };
		/// The panel's top-left when the head was taken; a drag is an offset from here.
		visage::Point dragAnchor { 0.0f, 0.0f };
		Watch watch;
		/// ITS ENTRY ON THE ESC STACK — off it when the slot goes, whatever took it.
		EscapeTicket escape;
	};

	void dismissBy(Slot& slot, Dismissal how);
	Slot* slotOf(PanelId id) const;
	Slot* front() const { return slots_.empty() ? nullptr : slots_.back().get(); }
	visage::Bounds panelBoundsOf(const Slot& slot) const;
	visage::Point panelSizeOf(const Slot& slot) const;
	/// THE CONTENT'S SIZE BEFORE THE LAYER'S CHROME AND EDGE: the hand's choice (or the minimum),
	/// held between the content's floor and its ceiling. The one place the two limits are applied.
	static visage::Point contentSizeOf(const Slot& slot);
	/// The content's ceiling, floored at its minimum so the two cannot cross.
	static visage::Point ceilingOf(const ExpandedView& content);
	/// THE DODGE: put `slot` where it covers no other panel if such a place exists inside this layer,
	/// nearest its preferred spot; over another only when the layer is too full to avoid it.
	void place(Slot& slot);
	void raise(Slot& slot);
	void beginPanelDrag(Slot& slot);
	void dragPanelBy(Slot& slot, visage::Point delta);
	void beginPanelResize(Slot& slot);
	void resizePanelBy(Slot& slot, visage::Point delta);
	/// The name a device's placement is remembered under: its title and its device, so two filters
	/// are two places. FOR THIS LAYER'S LIFETIME ONLY — the editor's prefs store flags, not
	/// positions, so a placement lasts as long as the editor is open and no longer; said here so
	/// nobody looks for it on disk.
	static std::string keyOf(const ExpandedView& content);

	FrameWatchers* watchers_ = nullptr;
	ModState* mod_ = nullptr;
	std::function<EscapeStack*()> escapeStack_;
	[[nodiscard]] EscapeStack* escapeStack() const { return escapeStack_ ? escapeStack_() : nullptr; }
	/// THE OPEN PANELS, BACK IS FRONT: child order is draw order, and this vector is kept in the same
	/// order as the children, so the last slot is the panel on top.
	std::vector<std::unique_ptr<Slot>> slots_;
	PanelId next_ = 1;
	std::optional<visage::Bounds> preferredRegion_;

	/// Placements and sizes by device, for this layer's lifetime. See `keyOf`.
	std::map<std::string, visage::Point> remembered_;
	std::map<std::string, visage::Point> rememberedSize_;
	std::map<std::string, bool> rememberedFold_;
	void setFolded(Slot& slot, bool folded);
	std::map<std::string, bool> rememberedMaximized_;
	void setMaximized(Slot& slot, bool maximized);
	/// THE CONTENT THAT HAS BEEN CLOSED AND NOT YET DESTROYED.
	///
	/// A FRAME MUST NOT BE DESTROYED FROM INSIDE ITS OWN TREE'S EVENT DISPATCH. `dismissBy` runs from
	/// a mouse handler — the ✕ — and visage is still walking the tree when it returns; deleting a
	/// child and re-routing events underneath that walk is how a SIGSEGV in an unrelated frame's
	/// handler happens, which is what was hit in practice. So a dismissal RETIRES the content and the next
	/// `show` or `draw` destroys it, by which time no dispatch is in flight.
	std::vector<std::unique_ptr<visage::Frame>> retired_;
	Dismissal lastDismissal_ = Dismissal::None;
};

} // namespace feathers::ui
