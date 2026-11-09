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
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "plugin/ui/escape_stack.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/surface_host.h"
#include "plugin/ui/takeover_stack.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <visage/ui.h>

// EVERY KIND BEHIND ONE HOST — the thing a guest actually holds.
//
// A `TakeoverStack` can only answer `SelfContained`, and `SurfaceHost::kindFor` correctly turns its
// offer of a takeover for a spanning gesture into a refusal. That was honest and it was also a dead
// end: the rack's expand verb IS a spanning gesture, so with only a takeover the button had nowhere
// to go and `kindFor` said so.
//
// SO THIS OWNS ONE OF EACH AND ROUTES BY NEED. `SelfContained` takes the cheap road — no resize, no
// event loop; `SpansGesture` takes the CANVAS while the host has one showing, and the overlay
// otherwise — both leave the editor **reachable** beneath, which `expanded_overlay.h` records is not
// negotiable: *"the scrim draws and takes nothing; the panel takes its own presses; a press anywhere
// else reaches the editor beneath and arms a source there."*
//
// IT IS IN THE FLOOR RATHER THAN IN THE RACK because the routing is not the rack's policy — it is the
// contract's. A host that owns these layers answers the same way whichever plugin it is in, and since
// 2026-09-13 Feathers holds one too: **one code path** for every expand in either host, and for a rack
// nested in either (see `setOuterLayers`).

namespace feathers::ui {

class ExpandedCanvas;
class FrameWatchers;
class ModState;

/// AN ARBITRARY FRAME, MOUNTED AS AN EXPANDED VIEW.
///
/// `SurfaceHost::present` takes a plain `Frame` on purpose: content should not have to know which kind
/// it got, which is property 1 of the design. `ExpandedOverlay::show` wants an `ExpandedView`, which
/// knows its own title and minimum size. **The adapter is where those two facts meet**, and it is the
/// host's business rather than the guest's — the title came in as a `present` argument and the minimum
/// size is the content's own, asked of the frame it already is.
class MountedFrame final : public ExpandedView {
public:
	MountedFrame(std::unique_ptr<visage::Frame> content, std::string title, visage::Point minimum);
	~MountedFrame() override;

	/// THE CONTENT'S OWN MINIMUM, ASKED — not a constant this mount chose.
	///
	/// **RETURNING A FIXED SIZE WAS A BUG AND IT SHOWED AS ONE.** 2026-08-31: the pop-out cards did not
	/// expand to their full size, leaving an invisible scroll instead; they are meant to be full size.
	/// `ExpandedOverlay` sizes its panel from `minimumSize()`, so a mount
	/// answering 420x300 for every panel made every panel 420x300 — and the real one, which knows how
	/// big it needs to be, was scrolled inside it.
	///
	/// THE SCROLL WAS RIGHT AND ITS DEFAULT WAS WRONG. Deferring to the content makes scrolling the
	/// EXCEPTION — a panel taller than the layer can be — rather than the normal case, which is what
	/// it was written for and not what it was doing.
	visage::Point minimumSize() const override;
	/// **AND ITS CEILING, ASKED THE SAME WAY** (backlog EI). A mount that answered the default would
	/// uncap every panel the moment it was presented through the layers, which is every panel in
	/// either host. Content that is not a panel states none and gets none.
	visage::Point maximumSize() const override;
	std::string title() const override { return title_; }
	/// The mounted view's header text, forwarded (the title above stays the identity) — see `ExpandedView::headerTitle`.
	std::string headerTitle() const override;
	/// The mounted view's title-row control, forwarded — see `ExpandedView::titleAccessory`.
	visage::Frame* titleAccessory() const override;
	/// THE DEVICE THE MOUNTED CONTENT IS ABOUT, so the overlay keys the panel by it: two devices are two
	/// panels and the same device again is a replacement. A view knows; a card knows; anything else is
	/// no device, and every no-device mount shares one key, as before.
	DeviceInstanceId watchedDevice() const override;
	/// **AND WHICH CHAIN THAT DEVICE IS IN, ANSWERED THE SAME WAY AND IN THE SAME BREATH.** The two are
	/// one identity (`DeviceNest`), and the overlay asks the MOUNT for both — so a mount that delegated
	/// one and not the other files a nested panel under a key and a colour belonging to a different
	/// device. That is not hypothetical: it was photographed, with the card and its own panel wearing
	/// two colours.
	DeviceNest nest() const noexcept override;
	/// **AND THE DEVICE'S ENABLE, FOR THE WINDOW HEADER'S CHIP — TRANSPARENT, AS THE TWO ABOVE.** Every
	/// panel in either host is presented through a mount, so a mount answering "no enable" would take
	/// the chip off every window at once. Forwarded to a mounted VIEW only: the generic fallback both
	/// chain hosts keep is a whole CARD, whose own head already carries the square — a second chip
	/// above it would be the duplicate this chip exists to remove.
	size_t deviceEnableControl() const override;
	bool deviceEnabled() const override;
	std::optional<bool> toggleDeviceEnable() override;
	EnableMarks deviceEnableMarks() const override;
	bool routeDeviceEnable() override;
	void resized() override;
	/// THE SCROLL BAR, when there is anything to scroll — the cards' own `drawBodyScrollBar`, in a
	/// gutter the content is kept out of (a chain panel fills its whole width with its own surface,
	/// so a bar drawn under it would be a bar nobody sees). On the Engine rail there was no visible scroll
	/// bar though it did scroll; the mount had the same silence.
	void draw(visage::Canvas& canvas) override;
	static constexpr float kScrollGutter = 8.0f;
	/// The height the content was given — its own minimum or the mount's, whichever is larger.
	float contentHeightForTest() const noexcept { return contentHeight_; }

	/// THE MOUNTED CONTENT'S OWN ADVANCE. `SurfaceLayers` decides at mount time what the content is —
	/// an `ExpandedView`, a card, or neither — and installs the thunk; this mount only runs it. That
	/// is what puts a mounted panel on the tick road THROUGH the layers rather than beside them: the
	/// rack used to keep its own thunk next to the id, and Feathers had no mount at all, so the two
	/// hosts advanced a panel by two different roads.
	bool updateContent() override { return tick_ != nullptr && tick_(); }
	void setTick(std::function<bool()> tick) { tick_ = std::move(tick); }
	/// PUT THE MOUNTED FRAME ITSELF ON THE STATE CHANNEL. The layer that shows this mount watches the
	/// MOUNT, and `Frame::redraw()` marks one frame — so a panel inside it would repaint on a patch
	/// load only if it were watched in its own right, which is property 5 of `expanded_overlay.h` and
	/// what every panel had before the mount stood between it and the layer.
	void watchOn(FrameWatchers* watchers) {
		innerWatch_ = watchers != nullptr && content_ != nullptr ? watchers->watch(content_.get()) : Watch {};
	}
	/// The frame that was mounted, for a case or a host that has to hand it something.
	visage::Frame* mounted() const noexcept { return content_.get(); }
	visage::Frame* contentForTest() const { return content_.get(); }

	/// HOW FAR THE MOUNTED CONTENT IS SCROLLED, and how far it CAN be.
	///
	/// A PANEL IS NOT A GUARANTEE OF ROOM, which is the whole reason this exists. The rack's expand verb
	/// shows a device's FULL control set — 127 of them on the largest device, which is about 2280px of
	/// grid — and an overlay sized to the editor has a few hundred. Without a scroll the panel would
	/// repeat the bug it was opened to fix: showing part of a device while looking complete.
	///
	/// IT LIVES HERE RATHER THAN IN THE CONTENT because content does not know which kind it got, and a
	/// frame that scrolled itself would be wrong in a takeover, which is already full-window.
	float scroll() const noexcept { return scroll_; }
	float maxScroll() const noexcept { return maxScroll_; }
	void scrollBy(float delta);

private:
	std::unique_ptr<visage::Frame> content_;
	std::string title_;
	visage::Point minimum_ {};
	std::function<bool()> tick_;
	Watch innerWatch_;
	float scroll_ = 0.0f;
	float maxScroll_ = 0.0f;
	float contentHeight_ = 0.0f;
};

class SurfaceLayers final : public visage::Frame, public SurfaceHost {
public:
	SurfaceLayers();
	~SurfaceLayers() override;

	void dismiss(SurfaceId id) override;
	void resized() override;

	TakeoverStack& takeover() noexcept { return *takeover_; }
	ExpandedOverlay& overlay() noexcept { return *overlay_; }

	/// THE CANVAS, IF THE HOST HAS ONE — NOT OWNED AND NOT PLACED HERE. This frame is the full-window
	/// top layer for the takeover and the overlay; the canvas is a REGION of the editor's layout, a
	/// column or a band the host lays out like a rail, so the host owns it and places it. **IT HOLDS NO
	/// PANELS**: every spanning present lands on this frame's one
	/// overlay, which covers the editor and the band together, and the band's floor is only where a new
	/// panel PREFERS to land while the band is showing. Null is a host without a canvas: no preference,
	/// and a new panel is centred.
	void setCanvas(ExpandedCanvas* canvas);
	ExpandedCanvas* canvas() const noexcept { return canvas_; }
	/// THE SHOWING BAND'S FLOOR IN THE OVERLAY'S PIXELS, or none when there is no band or it is folded.
	std::optional<visage::Bounds> canvasRegion() const;

	/// THE LAYERS OF THE HOST THIS ONE IS NESTED IN — rack roadmap §5's escalation. A rack inside a
	/// chain device inside a rack presents its panels on the OUTERMOST layers, because a panel inside a
	/// panel is the clipping container `surface_host.h` was written about. With an outer set, a
	/// spanning gesture is answered as the OUTERMOST would answer its own, and presented on the
	/// outermost's overlay, preferring its band while that shows. `SelfContained` stays local: a browser inside the inner editor is inside the inner editor.
	///
	/// **AND THE RELATIONSHIP IS REGISTERED IN BOTH DIRECTIONS, which is the whole of backlog HQ.** An
	/// outer host that dies holding a nested host's forwarded mount frees that mount through its
	/// `ExpandedOverlay`'s member teardown — `~vector<Slot>`, which destroys and does not DISMISS — so
	/// `onDismissPanel` → `panelWentDown` never fires and the inner's `forwarded_` entry outlives the
	/// thing it names. The inner host then walks a freed frame in its own destructor. Knowing its
	/// nested hosts is what lets an outer say "I am going" while its members are still intact.
	void setOuterLayers(SurfaceLayers* outer);
	SurfaceLayers* outerLayers() const noexcept { return outer_; }
	/// How many forwarded presents this host still believes are up — for a case asserting the entry
	/// was dropped rather than merely that nothing crashed.
	size_t forwardedCountForTest() const noexcept { return forwarded_.size(); }
	/// How many hosts are nested in this one, for the same reason.
	size_t innerCountForTest() const noexcept { return inner_.size(); }

	/// **THE EDITOR'S ONE ESC STACK** (backlog NN, `escape_stack.h`) — the OUTERMOST host's, so a browser in
	/// a nested chain, a panel that chain opened and the outer editor's arm are all one order. Both layers
	/// open onto it; the editor's root Esc handler closes its top.
	EscapeStack& escapeStack() noexcept { return outer_ != nullptr ? outer_->escapeStack() : escape_; }

	/// THE STACK OF THE EDITOR `frame` IS IN — the first host met on the way up from it, sitting either on
	/// the road itself or beside it as a child of an ancestor (an editor root holds its layers as a child,
	/// beside the frames they cover). Null for a frame in no editor, which then closes itself as it did.
	static EscapeStack* escapeStackFor(const visage::Frame& frame);

	/// THE STATE AND MODULATION ROADS, handed on to the layer that mounts content.
	void setStateWatchers(FrameWatchers* watchers);
	void setModulationState(ModState* mod);

	/// ADVANCE WHATEVER IS MOUNTED AND SAY WHETHER A FRAME WAS ASKED FOR — the overlay's content. The
	/// one tick road for a presented panel in either host.
	bool updateContent();

	/// IS ANYTHING UP, in any layer this frame owns. The canvas is the host's and is not counted.
	bool empty() const noexcept;

	/// THE MINIMUM AN OVERLAY-MOUNTED FRAME ASKS FOR. Not a design choice so much as a floor: an
	/// `ExpandedView` that reports nothing is one the overlay may draw at zero size.
	static constexpr float kMinOverlayWidth = 420.0f;
	static constexpr float kMinOverlayHeight = 300.0f;

	/// IS THIS ID STILL UP. A presenter holding several ids prunes against this when told of a
	/// dismissal, rather than being told which one.
	bool holds(SurfaceId id) const noexcept;
	/// BRING THIS DEVICE'S OPEN PANEL TO THE FRONT (`ExpandedOverlay::focusPanelFor`), asked of the
	/// layer a panel is actually presented on — the outermost, the same road `presentMounted` takes —
	/// so a card inside a nested chain raises the panel it opened. False when none is open.
	bool focusPanelFor(DeviceInstanceId device, DeviceNest nest);
	/// AND CLOSE IT (`ExpandedOverlay::dismissPanelFor`), asked of the same outermost layer — what a
	/// card's expand mark does when its device's panel is already up. False, touching nothing, when
	/// none is open, so the caller goes on to present one.
	bool dismissPanelFor(DeviceInstanceId device, DeviceNest nest);
	/// TAKE DOWN EVERY SPANNING SURFACE THIS HOST PRESENTED, each through the overlay's dismissal road.
	/// The takeover is not touched.
	void dismissPresented();

	/// THE MOST RECENT SPANNING ID STILL UP, for a case reading where a present landed.
	SurfaceId overlayIdForTest() const noexcept;
	size_t presentedCountForTest() const noexcept { return presented_.size(); }

protected:
	SurfaceKind kindAvailable(SurfaceNeed need) const override;
	SurfaceId presentImpl(std::unique_ptr<visage::Frame> content, SurfaceNeed need, std::string title,
	                      std::function<void()> onDismiss) override;

private:
	std::unique_ptr<MountedFrame> mount(std::unique_ptr<visage::Frame> content, std::string title);
	/// A MOUNT, WHERE THIS HOST PUTS ITS OWN: the overlay, preferring the band while it shows. The road a
	/// NESTED host's forwarded presents take too (backlog EB: a device inside a chain opens beside the
	/// chain's panel in the band, or floats over the editor when the band is folded).
	SurfaceId presentMounted(std::unique_ptr<MountedFrame> mounted, std::function<void()> onDismiss);
	SurfaceId presentOverlay(std::unique_ptr<MountedFrame> mounted, std::function<void()> onDismiss);
	void panelWentDown(ExpandedOverlay::PanelId panel);

	/// EVERY SPANNING SURFACE THIS HOST HAS UP: its id, its panel, and who to tell. Ids stay monotonic
	/// across EVERY layer — see `SurfaceId`: an id that could mean "the overlay's current thing" would be
	/// the ABA bug the takeover stack already refuses to have, one layer up — and panel ids are the
	/// overlay's own, monotonic likewise.
	struct Presented {
		SurfaceId id = kNoSurface;
		ExpandedOverlay::PanelId panel = ExpandedOverlay::kNoPanel;
		std::function<void()> onDismiss;
	};
	std::vector<Presented> presented_;
	/// WHAT THIS HOST FORWARDED AND STILL BELIEVES IS UP — the outer's id and the mount it holds.
	/// The mount is remembered because its knobs are on THIS host's modulation list (the inner rack's
	/// `ModState`), and that list can die before the outer frees the retired mount: a chain view
	/// dropping its editor. So a forwarded dismiss takes the mount off this host's lists FIRST — the
	/// sanitizer gate found the assert in `~FrameWatchers` when it did not (2026-09-13).
	struct Forwarded {
		SurfaceId id = kNoSurface;
		visage::Frame* mount = nullptr;
	};
	std::vector<Forwarded> forwarded_;
	void forgetForwarded(visage::Frame* mount);
	/// EVERY HOST NESTED IN THIS ONE — the inverse of `outer_`, kept so this host can reach them from
	/// its own destructor. **A SECOND ROAD, NOT A REPLACEMENT FOR THE FIRST.** `panelWentDown` remains
	/// the road every ORDINARY dismissal takes, and it is the only thing that unlists a `forwarded_`
	/// entry when a panel is taken down; this one exists for the case where that road structurally
	/// cannot run, because the outer host is being destroyed rather than dismissing anything.
	std::vector<SurfaceLayers*> inner_;
	void forgetInner(SurfaceLayers* nested);
	/// THE OUTER IS GOING, AND ITS MEMBERS ARE STILL ALIVE. Called only from `~SurfaceLayers`, before
	/// `overlay_` and `takeover_` are destroyed — so every mount named in `forwarded_` is still a live
	/// frame and `forgetFrameTree` may walk it. It is the last instant at which that is true.
	void outerLayersGoingAway();
	/// THIS HOST'S FORWARDED ENTRIES AND EVERY NESTED HOST'S, unlisted. Recursive because escalation is:
	/// a present from three levels down passes through every host on the way to the outermost, so each
	/// of them holds an entry naming the SAME mount, and the outermost's death invalidates all of them.
	void forgetEveryForwarded();
	/// THE EDITOR'S ESC STACK when this host is the outermost — see `escapeStack`. Declared before the layers,
	/// so it outlives every ticket they hold (a ticket would survive it anyway: it holds the stack weakly).
	EscapeStack escape_;
	std::unique_ptr<TakeoverStack> takeover_;
	std::unique_ptr<ExpandedOverlay> overlay_;
	ExpandedCanvas* canvas_ = nullptr;
	SurfaceLayers* outer_ = nullptr;
	FrameWatchers* watchers_ = nullptr;
	ModState* mod_ = nullptr;
};

} // namespace feathers::ui
