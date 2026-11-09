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

#include "plugin/ui/surface_layers.h"

#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>

namespace feathers::ui {

MountedFrame::MountedFrame(std::unique_ptr<visage::Frame> content, std::string title,
                           visage::Point minimum)
    : content_(std::move(content)), title_(std::move(title)), minimum_(minimum) {
	if (content_ != nullptr) {
		addChild(content_.get());
		content_->setVisible(true);
		// **AND THE MOUNTED PANEL'S "MY MINIMUM MOVED" IS PASSED STRAIGHT UP.**
		//
		// `minimumSize()` above defers to the content, so a mount is TRANSPARENT about how big its
		// panel needs to be — and it was OPAQUE about the panel saying that number had changed. The
		// host wires `onMinimumSizeChanged` on what it was handed, which is this mount; the panel sets
		// its own and nothing had connected the two, so a rail that opened inside a mounted SKLITTER
		// panel announced into a null and the panel was re-laid-out inside its old rectangle. FOUND BY
		// THE CASE, not by reading: the first draft of it asserted on `ExpandedOverlay::panelBounds()`,
		// which recomputes from `minimumSize()` live, and was green on a build where nothing moved.
		//
		// A forward rather than a re-derivation: the mount adds no size fact of its own on this road.
		if (auto* view = dynamic_cast<ExpandedView*>(content_.get()); view != nullptr) {
			view->onMinimumSizeChanged([this] {
				// THIS MOUNT'S OWN LAYOUT FIRST — `resized()` sizes the content from its minimum, so a
				// panel that grew taller gets the room here whether or not the host resizes the mount.
				resized();
				minimumSizeChanged();
			});
		}
	}
	onMouseWheel() = [this](const visage::MouseEvent& e) {
		scrollBy(-e.wheel_delta_y * 40.0f);
		return true;
	};
}

MountedFrame::~MountedFrame() {
	// THE CHILD GOES BEFORE THE POINTER THAT OWNS IT, the same order `TakeoverStack`'s destructor
	// keeps and for the same reason: a parent still listing a freed frame is a parent walking it.
	if (content_ != nullptr) {
		removeChild(content_.get());
	}
}

std::string MountedFrame::headerTitle() const {
	// **TRANSPARENT, AS `titleAccessory` IS**: a view whose header follows state says so through here, or not at all.
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->headerTitle();
	}
	return title_;
}

visage::Frame* MountedFrame::titleAccessory() const {
	// **TRANSPARENT, AS `minimumSize` IS** — every panel in either host is presented through one of these,
	// so a mount that answered null here would hide every view's title-row control, silently, in both.
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->titleAccessory();
	}
	return nullptr;
}

DeviceInstanceId MountedFrame::watchedDevice() const {
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->watchedDevice();
	}
	if (const auto* card = dynamic_cast<const FxCard*>(content_.get()); card != nullptr) {
		return card->device();
	}
	return kNoInstance;
}

DeviceNest MountedFrame::nest() const noexcept {
	// THE SAME TWO KINDS, IN THE SAME ORDER, FOR THE SAME REASON — a mounted panel is an `ExpandedView`
	// or a vertical `FxCard` (the generic fallback both chain hosts fall back to), and both carry the
	// nest their presenter stamped on them.
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->nest();
	}
	if (const auto* card = dynamic_cast<const FxCard*>(content_.get()); card != nullptr) {
		return card->nest();
	}
	return {};
}

size_t MountedFrame::deviceEnableControl() const {
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->deviceEnableControl();
	}
	return kNoEnable;
}

bool MountedFrame::deviceEnabled() const {
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->deviceEnabled();
	}
	return true;
}

std::optional<bool> MountedFrame::toggleDeviceEnable() {
	if (auto* view = dynamic_cast<ExpandedView*>(content_.get()); view != nullptr) {
		return view->toggleDeviceEnable();
	}
	return std::nullopt;
}

ExpandedView::EnableMarks MountedFrame::deviceEnableMarks() const {
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->deviceEnableMarks();
	}
	return {};
}

bool MountedFrame::routeDeviceEnable() {
	if (auto* view = dynamic_cast<ExpandedView*>(content_.get()); view != nullptr) {
		return view->routeDeviceEnable();
	}
	return false;
}

visage::Point MountedFrame::minimumSize() const {
	if (content_ == nullptr) {
		return minimum_;
	}
	// A PANEL KNOWS ITS OWN SIZE. Every `ExpandedView` already answers this — it is how Feathers' own
	// overlay has always sized them — so a mount that ignored it was throwing away the one number that
	// mattered.
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->minimumSize();
	}
	// AND CONTENT THAT IS NOT A PANEL ANSWERS WITH THE BOUNDS IT WAS GIVEN, which is what the guest
	// laid it out to. The floor passed in is only for content that has neither — it is a floor, not a
	// size.
	const float w = static_cast<float>(content_->width());
	const float h = static_cast<float>(content_->height());
	return { w > minimum_.x ? w : minimum_.x, h > minimum_.y ? h : minimum_.y };
}

visage::Point MountedFrame::maximumSize() const {
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		return view->maximumSize();
	}
	return { kNoCap, kNoCap };
}

void MountedFrame::resized() {
	if (content_ == nullptr) {
		return;
	}
	// THE CONTENT KEEPS THE HEIGHT IT ASKS FOR AND THE PANEL SCROLLS IT. Sizing it to the panel would
	// squash a grid that has already decided how many rows it needs, which is how a card ends up
	// drawing on top of itself.
	//
	// **UNLESS IT IS A PANEL, WHICH FILLS WHAT IT IS GIVEN.** An `ExpandedView` lays itself out from
	// its own width and height (property 2), so a mount that held it at the height it happened to
	// have would leave a canvas-mounted panel at its overlay size with the canvas's room unused. It
	// gets the larger of the mount and its own minimum, and scrolls only past the minimum.
	int wanted = content_->height() > 0 ? content_->height() : height();
	if (const auto* view = dynamic_cast<const ExpandedView*>(content_.get()); view != nullptr) {
		wanted = std::max(static_cast<int>(height()), static_cast<int>(view->minimumSize().y));
	}
	// A GUTTER FOR THE BAR, only when there is one: content that fits keeps the whole width.
	const int contentWidth = wanted > height() ? std::max(0, static_cast<int>(width()) - static_cast<int>(kScrollGutter)) : static_cast<int>(width());
	contentHeight_ = static_cast<float>(wanted);
	content_->setBounds(0, -static_cast<int>(scroll_), contentWidth, wanted);
	maxScroll_ = std::max(0.0f, static_cast<float>(wanted - height()));
	if (scroll_ > maxScroll_) {
		scroll_ = maxScroll_;
		content_->setBounds(0, -static_cast<int>(scroll_), contentWidth, wanted);
	}
}

void MountedFrame::draw(visage::Canvas& canvas) {
	drawBodyScrollBar(canvas, { 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()) },
	                  scroll_, contentHeight_);
}

void MountedFrame::scrollBy(float delta) {
	// CLAMPED AGAINST A BOUND THE LAYOUT COMPUTED, not one remembered from before the content changed
	// size — the rack's own scroll records why: a clamp against a stale maximum stops short.
	const float was = scroll_;
	scroll_ = std::clamp(scroll_ + delta, 0.0f, maxScroll_);
	if (scroll_ != was) {
		resized();
		redraw();
	}
}

SurfaceLayers::SurfaceLayers() {
	takeover_ = std::make_unique<TakeoverStack>();
	overlay_ = std::make_unique<ExpandedOverlay>();
	// THE OVERLAY GOES ON TOP OF THE TAKEOVER, which is the only order that can be right: a takeover
	// REPLACES the editor, and an expanded panel floats above whatever is showing. Reversed, a
	// takeover opened from inside a panel would paint over the panel that opened it.
	addChild(takeover_.get());
	addChild(overlay_.get());

	// THIS FRAME ITSELF TAKES NOTHING. It is a container for layers that each have their own rules
	// about presses — the takeover swallows, the overlay's scrim deliberately does not — and a parent
	// with its own hit region would sit in front of both.
	setIgnoresMouseEvents(true, true);

	// TOLD PER PANEL, AND THE ID RETIRED FIRST. `onDismiss` may present again, and a handler that ran
	// while the entry still named the thing being closed would have the next `dismiss(id)` land on a
	// surface that had already gone.
	overlay_->onDismissPanel = [this](ExpandedOverlay::PanelId panel) { panelWentDown(panel); };

	// BOTH LAYERS OPEN ONTO THE EDITOR'S ONE ESC STACK, asked each time rather than kept, because which
	// stack that is follows `setOuterLayers`.
	takeover_->setEscapeStack([this] { return &escapeStack(); });
	overlay_->setEscapeStack([this] { return &escapeStack(); });
}

EscapeStack* SurfaceLayers::escapeStackFor(const visage::Frame& frame) {
	for (const visage::Frame* at = &frame; at != nullptr; at = at->parent()) {
		if (auto* layers = dynamic_cast<SurfaceLayers*>(const_cast<visage::Frame*>(at)); layers != nullptr) {
			return &layers->escapeStack();
		}
		for (visage::Frame* child : at->children()) {
			if (auto* layers = dynamic_cast<SurfaceLayers*>(child); layers != nullptr) {
				return &layers->escapeStack();
			}
		}
	}
	return nullptr;
}

void SurfaceLayers::panelWentDown(ExpandedOverlay::PanelId panel) {
	for (auto it = presented_.begin(); it != presented_.end(); ++it) {
		if (it->panel == panel) {
			std::function<void()> told = std::move(it->onDismiss);
			presented_.erase(it);
			if (told) {
				told();
			}
			return;
		}
	}
}

bool SurfaceLayers::holds(SurfaceId id) const noexcept {
	if (id == kNoSurface) {
		return false;
	}
	for (const Presented& p : presented_) {
		if (p.id == id) {
			return true;
		}
	}
	// A FORWARDED ID IS THE OUTER'S, and this host still remembers having forwarded it.
	for (const Forwarded& f : forwarded_) {
		if (f.id == id) {
			return true;
		}
	}
	return false;
}

void SurfaceLayers::dismissPresented() {
	// FROM THE BACK, one at a time: each dismissal erases its entry through `panelWentDown`.
	while (!presented_.empty()) {
		overlay_->dismiss(presented_.back().panel);
	}
}

SurfaceId SurfaceLayers::overlayIdForTest() const noexcept {
	return presented_.empty() ? kNoSurface : presented_.back().id;
}

SurfaceLayers::~SurfaceLayers() {
	// THE CANVAS IS THE HOST'S AND MAY OUTLIVE THIS FRAME; its dismissal road points here, so it is
	// unhooked before the pointer it would call through goes.
	setCanvas(nullptr);

	// **A HOST GOING AWAY TAKES THE PANELS IT FORWARDED WITH IT — inside out (2026-10-31, the ASan gate on ecb647e2).**
	// Every mount this host escalated, its own and those of hosts nested in it, stands on the OUTERMOST overlay, and its
	// view holds what the presenter gave it: a `ModState`, a bridge, a history, owned by a host that is dying now or
	// with this one. Left up, it was an orphan with a dangling `ModState`, and the window header's enable chip
	// (`ExpandedPanel::syncEnable`, backlog KW) read it on the next tick. So each is dismissed HERE, while this host and
	// its lists are alive: `dismiss` unlists the mount's knobs from this host's `ModState`, drops the entry, and hands
	// the id outward, where the outermost retires it (freed at its next tick) and takes it out of the panels it syncs.
	while (outer_ != nullptr && !forwarded_.empty()) {
		dismiss(forwarded_.back().id);
	}

	// OFF THE OUTER'S LIST, so an outer that dies later does not walk a host that has already gone.
	if (outer_ != nullptr) {
		outer_->forgetInner(this);
		outer_ = nullptr;
	}

	// AND EVERY HOST NESTED IN THIS ONE IS TOLD, HERE, WHERE `overlay_` IS STILL ALIVE — backlog HQ.
	//
	// **THIS IS THE ONE INSTANT AT WHICH THE TELLING IS POSSIBLE.** A nested host's `forwarded_` entry
	// names a `MountedFrame` that THIS host's overlay owns. That mount is freed by `~ExpandedOverlay`
	// → `~vector<Slot>`, which DESTROYS rather than dismisses, so `onDismissPanel` → `panelWentDown`
	// never runs and nothing unlists the entry. Members are destroyed after this body, so the mounts
	// are alive on this line and freed on the next — and the nested host's knobs are on ITS OWN
	// `ModState`, which no walk from here can reach.
	//
	// SWAPPED OUT FIRST rather than iterated in place: `outerLayersGoingAway` nulls its own `outer_`,
	// and a re-entrant `forgetInner` on the emptied vector is then a no-op rather than an erase inside
	// the walk that is reading it.
	std::vector<SurfaceLayers*> nested;
	nested.swap(inner_);
	for (SurfaceLayers* host : nested) {
		if (host != nullptr) {
			host->outerLayersGoingAway();
		}
	}
}

void SurfaceLayers::setOuterLayers(SurfaceLayers* outer) {
	if (outer_ == outer) {
		return;
	}
	if (outer_ != nullptr) {
		outer_->forgetInner(this);
	}
	outer_ = outer;
	if (outer_ != nullptr) {
		outer_->inner_.push_back(this);
	}
}

void SurfaceLayers::forgetInner(SurfaceLayers* nested) {
	const auto it = std::find(inner_.begin(), inner_.end(), nested);
	if (it != inner_.end()) {
		inner_.erase(it);
	}
}

void SurfaceLayers::outerLayersGoingAway() {
	// THE OUTER POINTER GOES FIRST, and it is half the defect rather than tidiness: `dismiss` follows a
	// forwarded id with `outer_->dismiss(id)`, so a host that kept the pointer would call into the very
	// object whose destructor is running. Nulled here, that call cannot be reached.
	outer_ = nullptr;
	forgetEveryForwarded();
}

void SurfaceLayers::forgetEveryForwarded() {
	// THE MOUNTS ARE UNLISTED WHILE THEY ARE STILL FRAMES. Same three lists as `forgetForwarded`, for
	// the same reason — a nested panel's knobs registered on THIS host's `ModState` and state channel,
	// and the outer's own teardown knows nothing about either.
	for (const Forwarded& f : forwarded_) {
		forgetFrameTree(f.mount, nullptr, watchers_, mod_ != nullptr ? &mod_->watchers() : nullptr);
	}
	forwarded_.clear();
	// **AND THE WHOLE CHAIN BELOW, because escalation goes all the way out.** Every mount a host
	// nested in THIS one forwarded went through this host to the same outermost owner — that is what
	// `presentMounted`'s recursion means — so when that owner dies, every level's entry names a freed
	// frame, not just this one's. Their `outer_` is NOT touched: it is this host, which is alive.
	for (SurfaceLayers* host : inner_) {
		if (host != nullptr) {
			host->forgetEveryForwarded();
		}
	}
}

void SurfaceLayers::setCanvas(ExpandedCanvas* canvas) {
	canvas_ = canvas;
}

std::optional<visage::Bounds> SurfaceLayers::canvasRegion() const {
	if (canvas_ == nullptr || !canvas_->showing()) {
		return std::nullopt;
	}
	// THE BAND'S FLOOR IN THE OVERLAY'S PIXELS. The overlay sits at this frame's origin, and the band is
	// wherever its host laid it out; the two meet through the window, which both are placed in.
	const visage::Bounds floor = canvas_->contentBounds();
	const visage::Point at = canvas_->positionInWindow() - positionInWindow();
	return visage::Bounds { at.x + floor.x(), at.y + floor.y(), floor.width(), floor.height() };
}

void SurfaceLayers::setStateWatchers(FrameWatchers* watchers) {
	watchers_ = watchers;
	overlay_->setStateWatchers(watchers);
}

void SurfaceLayers::setModulationState(ModState* mod) {
	mod_ = mod;
	overlay_->setModulationState(mod);
}

bool SurfaceLayers::updateContent() {
	return overlay_->updateContent();
}

bool SurfaceLayers::empty() const noexcept {
	return takeover_->empty() && !overlay_->open();
}

SurfaceKind SurfaceLayers::kindAvailable(SurfaceNeed need) const {
	if (need == SurfaceNeed::SelfContained) {
		return SurfaceKind::Takeover;
	}
	// NESTED: whatever the OUTER answers — `Canvas` while its band shows, `Overlay` otherwise (backlog
	// EB). Either way the panel lands on the one overlay; `Canvas` says it arrives in the band.
	if (outer_ != nullptr) {
		return outer_->kindFor(need);
	}
	return canvas_ != nullptr && canvas_->showing() ? SurfaceKind::Canvas : SurfaceKind::Overlay;
}

std::unique_ptr<MountedFrame> SurfaceLayers::mount(std::unique_ptr<visage::Frame> content, std::string title) {
	// HOW THE PANEL WILL BE ADVANCED, DECIDED HERE WHERE THE TYPE IS STILL KNOWN. The seam takes a bare
	// Frame, so this is the last point at which anything knows what was built: an `ExpandedView`
	// advances through its own `update`, a card through `FxCard::update`, and anything else stands
	// still. **AND THE VIEW IS TOLD WHICH LAYERS IT IS ON** — the road a chain device's panel uses to
	// escalate its inner editor's presents to the outermost host.
	std::function<bool()> tick;
	if (auto* view = dynamic_cast<ExpandedView*>(content.get()); view != nullptr) {
		view->setPresentedOn(this);
		tick = [view] { return view->update(); };
	}
	else if (auto* card = dynamic_cast<FxCard*>(content.get()); card != nullptr) {
		tick = [card] { return card->update(); };
	}
	auto mounted = std::make_unique<MountedFrame>(std::move(content), std::move(title),
	                                              visage::Point { kMinOverlayWidth, kMinOverlayHeight });
	mounted->setTick(std::move(tick));
	mounted->watchOn(watchers_);
	return mounted;
}

SurfaceId SurfaceLayers::presentImpl(std::unique_ptr<visage::Frame> content, SurfaceNeed need,
                                     std::string title, std::function<void()> onDismiss) {
	if (need == SurfaceNeed::SelfContained) {
		return takeover_->present(std::move(content), need, std::move(title), std::move(onDismiss));
	}
	return presentMounted(mount(std::move(content), std::move(title)), std::move(onDismiss));
}

bool SurfaceLayers::focusPanelFor(DeviceInstanceId device, DeviceNest nest) {
	if (outer_ != nullptr) {
		return outer_->focusPanelFor(device, nest);
	}
	return overlay_->focusPanelFor(device, nest);
}

bool SurfaceLayers::dismissPanelFor(DeviceInstanceId device, DeviceNest nest) {
	if (outer_ != nullptr) {
		return outer_->dismissPanelFor(device, nest);
	}
	return overlay_->dismissPanelFor(device, nest);
}

SurfaceId SurfaceLayers::presentMounted(std::unique_ptr<MountedFrame> mounted, std::function<void()> onDismiss) {
	// **THE FORWARD IS HERE RATHER THAN IN `presentImpl`, AND THAT IS BACKLOG HQ'S SECOND DEFECT.**
	//
	// THE BUG: for a SKLITTER inside a rack inside a SKLITTER, the windowed editor would not open.
	// The press was not lost and the panel was not refused: it was BUILT, mounted, keyed and ticked on
	// a `SurfaceLayers` that is deliberately not a frame, so no hand could ever reach it.
	//
	// `setOuterLayers` says a nested host *"presents its panels on the OUTERMOST layers"* and
	// `kindAvailable` really does recurse (`outer_->kindFor(need)`). The present did not: `presentImpl`
	// called `outer_->presentMounted(...)`, which mounted on the IMMEDIATE outer and never asked
	// whether THAT host was itself nested. At one level the immediate outer IS the outermost, so it
	// was right everywhere it had been exercised; at two the panel landed on a `ChainColumn`'s own
	// layers, whose constructor says in as many words *"NOT A CHILD FRAME. Its two layers stay empty
	// for this column's whole life — every present is forwarded"*.
	//
	// **SO THE FORWARD MOVES TO THE ROAD THAT IS TAKEN AT EVERY HOP**, and escalation becomes the same
	// recursion `kindAvailable` already was. EVERY host in the chain keeps its own `forwarded_` entry
	// for the mount — not only the one that built it — because `dismiss` walks back out the same way:
	// each level erases its entry and hands the id to its outer, and each level's `holds()` can then
	// answer for an id that is really the outermost's.
	if (outer_ != nullptr) {
		// FORWARDED, AND THE OUTER'S ID AND THE MOUNT REMEMBERED so this host's `dismiss` can follow it
		// up — and so a dismissal the OUTER causes (its ✕, its Escape, a replacement) drops the entry
		// here before the presenter is told, since the mount is retired the moment that fires.
		visage::Frame* raw = mounted.get();
		auto told = [this, raw, onDismiss = std::move(onDismiss)] {
			forgetForwarded(raw);
			if (onDismiss) {
				onDismiss();
			}
		};
		const SurfaceId id = outer_->presentMounted(std::move(mounted), std::move(told));
		if (id != kNoSurface) {
			forwarded_.push_back({ id, raw });
		}
		return id;
	}
	// **WHERE THE OUTERMOST PUTS ITS OWN: ON ITS ONE OVERLAY, PREFERRING THE CANVAS WHILE IT SHOWS.**
	// The canvas is an extension of the main surface, not a fenced-in region of its own — so there is
	// one stack of panels over the editor and the band together, and the band's floor is only where a
	// new panel arrives (canvas doc §3f). Asked afresh at every present, because the band folds and its
	// partition moves between them.
	overlay_->setPreferredRegion(canvasRegion());
	return presentOverlay(std::move(mounted), std::move(onDismiss));
}

SurfaceId SurfaceLayers::presentOverlay(std::unique_ptr<MountedFrame> mounted, std::function<void()> onDismiss) {
	// SEVERAL PANELS AT ONCE, ONE PER DEVICE — `ExpandedOverlay`'s own rule: `show` dismisses a panel it
	// replaces THROUGH the dismissal road (`onDismissPanel`), so that entry is erased here before this
	// one is added, and its presenter is told rather than having its content vanish.
	const ExpandedOverlay::PanelId panel = overlay_->show(std::move(mounted));
	if (panel == ExpandedOverlay::kNoPanel) {
		return kNoSurface;
	}
	// THE ID IS TAKEN FROM THE TAKEOVER'S COUNTER so that ids are unique across EVERY layer. Two
	// independent counters would hand out the same number twice, and `dismiss` would then have to
	// guess which layer a caller meant — which is exactly the identity confusion `SurfaceId` exists
	// to prevent.
	const SurfaceId id = takeover_->reserveId();
	presented_.push_back({ id, panel, std::move(onDismiss) });
	return id;
}

void SurfaceLayers::dismiss(SurfaceId id) {
	if (id == kNoSurface) {
		return;
	}
	for (const Presented& p : presented_) {
		if (p.id != id) {
			continue;
		}
		// THE PANEL IT NAMES, and nothing else — the layer's dismissal road erases the entry.
		overlay_->dismiss(p.panel);
		return;
	}
	if (outer_ != nullptr) {
		const auto it = std::find_if(forwarded_.begin(), forwarded_.end(),
		                             [id](const Forwarded& f) { return f.id == id; });
		if (it != forwarded_.end()) {
			// OFF THIS HOST'S LISTS BEFORE THE OUTER RETIRES IT. The outer's dismissal forgets the mount
			// on the OUTER's lists, which is not where a nested panel's knobs are: they registered on
			// this host's `ModState` when the inner editor built the panel, and this call can be coming
			// from that editor's destructor — one line before that `ModState` goes.
			forgetFrameTree(it->mount, nullptr, watchers_, mod_ != nullptr ? &mod_->watchers() : nullptr);
			forwarded_.erase(it);
			outer_->dismiss(id);
			return;
		}
	}
	// NOT A SPANNING LAYER'S, SO IT IS THE STACK'S OR NOBODY'S — and the stack already ignores an id it
	// does not hold, which is what makes a stale id safe rather than lucky.
	takeover_->dismiss(id);
}

void SurfaceLayers::forgetForwarded(visage::Frame* mount) {
	const auto it = std::find_if(forwarded_.begin(), forwarded_.end(),
	                             [mount](const Forwarded& f) { return f.mount == mount; });
	if (it == forwarded_.end()) {
		return;
	}
	forgetFrameTree(it->mount, nullptr, watchers_, mod_ != nullptr ? &mod_->watchers() : nullptr);
	forwarded_.erase(it);
}

void SurfaceLayers::resized() {
	takeover_->setBounds(0, 0, width(), height());
	overlay_->setBounds(0, 0, width(), height());
}

} // namespace feathers::ui
