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

#include "plugin/ui/fx_lane_rail.h"

#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/ui/theme.h"
#include "rack/inner_chain.h"

#include <algorithm>
#include <string>
#include <utility>

namespace feathers::ui {

//--------------------------------------------------------------------------------------------------//
// RAILS 2 AND 3 — ONE FX LANE EACH, GRAIN THEN POST.                                                 //
//--------------------------------------------------------------------------------------------------//

namespace {

rack::InnerChain* sklitterGrain(void* surface) {
	return fx::sklitter::grainChainOf(surface);
}
rack::InnerChain* sklitterPost(void* surface) {
	return fx::sklitter::postChainOf(surface);
}
/// SKLITTER's seven, published inward by `publishCoreSource(n, performOutput(n))` (backlog HF).
std::string sklitterSourceName(size_t index) {
	return index < fx::sklitter::kPerformOutputs ? fx::sklitter::kOutputs[index].name
	                                             : "Source " + std::to_string(index + 1);
}

} // namespace

FxLaneRail::Spec FxLaneRail::specFor(Lane lane) noexcept {
	return lane == Lane::Grain ? Spec { kGrainLabel, kGrainLine, &sklitterGrain, fx::sklitter::kPerformOutputs, &sklitterSourceName }
	                           : Spec { kPostLabel, kPostLine, &sklitterPost, fx::sklitter::kPerformOutputs, &sklitterSourceName };
}

FxLaneRail::FxLaneRail(Lane lane, WidgetRegistry& registry, Notifier* notifier) noexcept
    : FxLaneRail(specFor(lane), registry, notifier) {
	lane_ = lane;
}

FxLaneRail::FxLaneRail(const Spec& spec, WidgetRegistry& registry, Notifier* notifier) noexcept
    : Rail(spec.label, Side::Right), spec_(spec), registry_(registry), notifier_(notifier) {
	// **BOTH START CLOSED.** A chain is not the instrument — the circle is — and a rail that opened
	// itself would take 300px from the grid to show a column that is usually empty. With TWO of them
	// that is 600px, so the argument is twice as strong as it was. One click, and `Rail` remembers it
	// for the life of the editor.
	setCollapsed(true);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (toggleHit(e.position)) {
			toggleCollapsed();
		}
		else if (onPhantomAdd_ && column_ == nullptr && !collapsed() && phantomAddBounds().contains(e.position)) {
			onPhantomAdd_(); // a phantom's (+): its chain made, and its add menu opened by the holder
		}
	};
}

FxLaneRail::~FxLaneRail() {
	dropColumn();
}

const char* FxLaneRail::captionForTest() const noexcept {
	return spec_.caption;
}

rack::RackBridge* FxLaneRail::bridgeIn(void* surface) const {
	// **THE ONE PLACE THE CAST IS MADE.** Spelled here rather than at the call site so there is a single line
	// to get wrong, and so a mutation of it lands on exactly one case.
	rack::InnerChain* chain = spec_.chainOf != nullptr ? spec_.chainOf(surface) : nullptr;
	return chain != nullptr ? chain->bridge() : nullptr;
}

void FxLaneRail::setOnDropOutside(std::function<bool(std::span<const DeviceInstanceId>, visage::Point)> f) {
	onDropOutside_ = std::move(f);
	if (column_ != nullptr) {
		column_->setOnDropOutside(onDropOutside_);
	}
}

void FxLaneRail::bindHistory() {
	history_.forwardTo(bridge_ != nullptr ? outerHistory_ : nullptr,
	                   bridge_ != nullptr ? bridge_->fragmentLife() : std::weak_ptr<const void> {});
}

void FxLaneRail::dropColumn() {
	if (column_ != nullptr) {
		removeChild(column_.get());
		column_.reset();
	}
	bridge_ = nullptr;
	surface_ = nullptr;
}

bool FxLaneRail::follow(void* surface) {
	// **THE HANDLE IS COMPARED, NEVER DEREFERENCED FROM MEMORY** — `surface_` is the answer the host
	// gave LAST tick and `surface` is the one it gave THIS tick. Comparing two pointers read in one
	// pass is fine; what is fatal is dereferencing one that was remembered (CLAUDE.md's ABA rule), and
	// this function never does.
	if (surface == surface_ && (surface == nullptr) == (column_ == nullptr)) {
		return false;
	}
	dropColumn();
	surface_ = surface;
	// **THE RAIL REPAINTS ITSELF ON EVERY ANSWER, NOT ONLY ON THE ONES THAT BUILD SOMETHING.** The
	// column is a CHILD frame and paints itself; the "not running here" line is the RAIL's own paint,
	// so a build that placed a column without marking the rail stale left that sentence on screen
	// underneath a working chain. PHOTOGRAPHED on the private display — every automated check was
	// green, because no case asks a frame what it last painted.
	redraw();
	bridge_ = bridgeIn(surface);
	bindHistory();
	if (bridge_ == nullptr) {
		// REMEMBERED EVEN WHEN NULL, so a handle that stays null does not report a change every tick.
		// A live instance whose `adopt` has not run yet has a chain and no bridge, which is the same
		// answer for this rail's purposes: there is nothing to edit through.
		return true;
	}
	column_ = std::make_unique<ChainColumn>(*bridge_, registry_, mod_, history_, notifier_);
	// **A REBUILT COLUMN IS TOLD WHAT THE OLD ONE KNEW, AND THIS IS WHERE A REBUILD LOSES IT.** The
	// panel hands these down once, when it is mounted; the instance behind the rail is replaced
	// whenever a body arrives (trap T9), and a column built after that moment has never been told
	// where its panels go. It would then draw no expand mark — the same silence that was reported, back
	// again by a different road and only after a patch load.
	column_->setPresentedOn(presenter_);
	column_->setNest(nest_);
	column_->setOnDropOutside(onDropOutside_);
	// **AND WHAT THIS CHAIN'S CORE SOURCES ARE CALLED, WHICH ONLY THE HOLDER'S SURFACE KNOWS** (backlog
	// HF). SKLITTER's are its own seven, published inward by `publishCoreSource(n, performOutput(n))`,
	// so the route menu reads "Perf X" where it would otherwise read "Source 1". Set on every rebuild
	// beside the other two facts a fresh column has never been told — trap T9's own list.
	if (spec_.coreSourceName != nullptr) {
		column_->setCoreSourceNamer(spec_.coreSourceName);
	}
	addChild(column_.get());
	// **HIDDEN IF THE RAIL IS SHUT, BEFORE IT IS EVER DRAWN.** `Rail::collapsedChanged` hides every
	// child, but it only runs on a TOGGLE — a column built while the rail is already closed would never
	// have been told, and would draw across the 34px strip.
	column_->setVisible(!collapsed());
	resized();
	return true;
}

size_t FxLaneRail::updateCards() {
	if (column_ == nullptr) {
		return 0;
	}
	// **THE CHAIN CAN CHANGE WITHOUT THIS COLUMN HAVING ASKED FOR IT**, which is why the sync is on the
	// TICK and not only on the column's own verbs: a patch load fills the inner chain through
	// `ContainerOps::loadState`, and a column that only synced after its own (+) would draw an empty
	// chain over a full one. `ChainExpanded::updateContent` calls the rack editor's `syncCards` every
	// tick for the same reason; it compares before it builds, so a beat with nothing new costs a walk.
	column_->syncCards();
	// **AND A HELD CARD AT THE COLUMN'S EDGE SCROLLS IT, ONE STEP A TICK** — the fx lane's gesture
	// (`FxLane::tickDrag`, driven from the editor's tick), because a chain taller than the rail has a
	// gap off-screen that a drag could otherwise never reach. Counted as a frame told: it repainted.
	const size_t scrolled = column_->tickDrag() ? 1 : 0;
	return column_->updateCards() + scrolled;
}

void FxLaneRail::setOuterModulation(ModState* mod, EditHistory* history,
                                    std::function<DeviceInstanceId()> holder) {
	// **THE INNER STATE FOLLOWS THE OUTER** (rack roadmap §7, and `ChainExpanded::setModulation`): an
	// outer source armed is what every inner control reads, spelled as an outer source. AND THE LANE'S EDITS RECORD
	// IN THE HOLDER'S HISTORY (backlog JB C9: one history per plugin, out to the outermost), each tied to this lane's
	// bridge and to this rail, so a rail rebuilt onto a new instance (trap T9) leaves its old records dead, not dangling.
	outerHistory_ = history;
	bindHistory();
	mod_.setParent(mod);
	// **AND THE HOLDER'S OWN SEVEN ARE SPELLED AS THIS CHAIN'S CORE SOURCES** (backlog HM, 2026-09-21:
	// arming perform's modulation outputs did not glow the fx params in the grain fx lane). See `ModState::setOuterSpelling` for why the default spelling could never have
	// worked, and `setOuterModulation`'s own note for why this fact is stated HERE: SKLITTER declares
	// `kPostChainAxes.coreSources == kPerformOutputs` and publishes `performOutput(n)` into core source
	// `n` of both lanes, and no general rule says a holder does that — a nested rack's core sources are
	// its macros. `tests/gui_widgets/test_gui_widgets_devices.cpp` pins the premise as a separate case,
	// because `sklitter.cpp` is not a file this rail can see.
	//
	// **UNSET WHEN THERE IS NO HOLDER TO NAME**, so a rail with no panel behind it keeps the ordinary
	// outer spelling rather than mapping everything onto core source 0.
	if (mod == nullptr || !holder) {
		mod_.setOuterSpelling(nullptr);
		return;
	}
	mod_.setOuterSpelling([holder = std::move(holder), cores = spec_.coreSources](device::Source source) {
		const DeviceInstanceId self = holder();
		// **THE HOLDER'S OWN OUTPUT, AND NOTHING ELSE.** A Feathers macro or LFO armed outside is a
		// genuine OUTER source in here and reaches an inner control by the outer road, which the gate
		// really does allow (a core source is upstream of everything); re-spelling those would aim them
		// at whatever core source wore that index.
		if (self != kNoInstance && source.isDevice() && source.device == self
		    && source.index < cores) {
			return device::Source::core(source.index);
		}
		return source.asOuter();
	});
}

void FxLaneRail::setPresentedOn(SurfaceLayers* host) {
	presenter_ = host;
	if (column_ != nullptr) {
		column_->setPresentedOn(host);
	}
}

void FxLaneRail::setNest(DeviceNest nest) {
	nest_ = nest;
	if (column_ != nullptr) {
		column_->setNest(nest);
	}
}

visage::Bounds FxLaneRail::headingBounds() const {
	const float left = railHeadingLeft(kPadX);
	return { left, kPadTop, std::max(0.0f, static_cast<float>(width()) - left - kPadX), kHeadingHeight };
}

visage::Bounds FxLaneRail::captionBounds() const {
	return { kPadX, kPadTop + kHeadingHeight + kGap,
		     std::max(0.0f, static_cast<float>(width()) - 2.0f * kPadX), kLineHeight };
}

visage::Bounds FxLaneRail::columnBounds() const {
	// **THE REST OF THE RAIL, WHICH IS THE WHOLE POINT OF THE SPLIT.** The stacked version gave each
	// lane HALF of what was left after two captions and a foot line; one lane per rail gives it
	// everything below its own caption, down to the bottom padding. Same arithmetic
	// `kOpenMinimumHeight` does in reverse, so the two cannot disagree about what the chrome costs.
	const float top = (accessoryShown() ? accessoryBounds().bottom() : belowHeading()) + kGap;
	const float bottom = std::max(0.0f, static_cast<float>(height()) - kPadTop);
	return { 0.0f, top, static_cast<float>(width()), std::max(0.0f, bottom - top) };
}

bool FxLaneRail::accessoryShown() const {
	// A SHUT RAIL SHOWS NO ACCESSORY and has no column to push down; a GATED one takes no room.
	return accessory_ != nullptr && accessoryGate_ && (!collapsed() || accessoryWhenCollapsed_);
}

void FxLaneRail::setAccessory(visage::Frame* frame, float height) {
	if (accessory_ != nullptr) {
		removeChild(accessory_);
	}
	accessory_ = frame;
	accessoryHeight_ = frame != nullptr ? height : 0.0f;
	if (accessory_ != nullptr) {
		addChild(accessory_);
		accessory_->setVisible(accessoryShown()); // a rail shut before this was set never told it
	}
	resized();
}

bool FxLaneRail::setAccessoryGate(bool shown) {
	if (shown == accessoryGate_) {
		return false;
	}
	accessoryGate_ = shown;
	if (accessory_ != nullptr) {
		accessory_->setVisible(accessoryShown());
	}
	resized();
	redraw();
	return true;
}

void FxLaneRail::collapsedChanged() {
	Rail::collapsedChanged(); // every child to the rail's state, the accessory included —
	if (accessory_ != nullptr) {
		accessory_->setVisible(accessoryShown()); // — and then the accessory to its own gate as well
	}
}

float FxLaneRail::belowHeading() const {
	// THE CAPTION'S ROW IS GIVEN BACK WHEN THERE IS NO CAPTION (the Rack's chains say it in a tooltip, backlog OC).
	return spec_.caption != nullptr && spec_.caption[0] != '\0' ? captionBounds().bottom() : headingBounds().bottom();
}

void FxLaneRail::setAccessoryWhenCollapsed(bool shown) {
	accessoryWhenCollapsed_ = shown;
	// A rail that says its name through the accessory (the chain strip's label) leaves the upright one out.
	setUprightLabel(!shown);
	if (accessory_ != nullptr) {
		accessory_->setVisible(accessoryShown());
	}
	resized();
}

visage::Bounds FxLaneRail::accessoryBounds() const {
	if (collapsed()) {
		// THE WHOLE STRIP UNDER THE TOGGLE: a closed rail is a column, and the accessory is its channel strip.
		const float top = toggleBounds().bottom() + kCollapsedStripGap;
		return { kCollapsedStripPadX, top, std::max(0.0f, static_cast<float>(width()) - 2.0f * kCollapsedStripPadX),
			     std::max(0.0f, static_cast<float>(height()) - top - kCollapsedStripGap) };
	}
	return { kPadX, belowHeading() + kGap, std::max(0.0f, static_cast<float>(width()) - 2.0f * kPadX),
		     accessoryHeight_ };
}

float FxLaneRail::openMinimumHeight() const {
	const float captionRow = spec_.caption != nullptr && spec_.caption[0] != '\0' ? 0.0f : -(kLineHeight + kGap);
	return kOpenMinimumHeight + captionRow + (accessory_ != nullptr && accessoryGate_ ? accessoryHeight_ + kGap : 0.0f);
}

void FxLaneRail::resized() {
	if (accessory_ != nullptr) {
		const visage::Bounds box = accessoryBounds();
		accessory_->setBounds(box.x(), box.y(), box.width(), box.height());
	}
	if (column_ != nullptr) {
		const visage::Bounds box = columnBounds();
		column_->setBounds(box.x(), box.y(), box.width(), box.height());
	}
}

void FxLaneRail::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.rectangle(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	canvas.setColor(kLine);
	canvas.fill(0.0f, 0.0f, 1.0f, static_cast<float>(height()));

	if (collapsed()) {
		drawRailChrome(canvas);
		return;
	}
	const visage::Bounds heading = headingBounds();
	canvas.setColor(kMuted);
	drawTracked(canvas, railLabel(), font(kControlSize), kControlTracking, heading.x(), heading.y(),
	            heading.height());
	drawRailChrome(canvas);

	const visage::Font& face = font(kLineSize);
	const visage::Bounds caption = captionBounds();
	canvas.setColor(kMuted);
	if (captionForTest()[0] != '\0') {
		canvas.text(captionForTest(), face, visage::Font::kLeft, caption.x(), caption.y(), caption.width(),
		            caption.height());
	}
	if (column_ == nullptr && onPhantomAdd_) {
		// **A PHANTOM: AN EMPTY CHAIN'S COLUMN, DRAWN AS `ChainColumn` DRAWS AN EMPTY ONE** — the (+) outline with
		// "add effect", and under it what passes — so a role no chain fills looks like the empty chain it stands for.
		const visage::Bounds add = phantomAddBounds();
		canvas.setColor(withAlpha(kLine, 0.75f));
		canvas.roundedRectangleBorder(add.x(), add.y(), add.width(), add.height(), 4.0f, 1.0f);
		canvas.setColor(kValue);
		const float plus = 7.0f;
		const float cx = add.x() + kPadX + plus * 0.5f;
		const float cy = add.y() + add.height() * 0.5f;
		canvas.segment(cx - plus * 0.5f, cy, cx + plus * 0.5f, cy, 1.4f, true);
		canvas.segment(cx, cy - plus * 0.5f, cx, cy + plus * 0.5f, 1.4f, true);
		canvas.text("add effect", font(kKnobLabelSize), visage::Font::kLeft, cx + plus, add.y(),
		            add.width() - kPadX - plus * 2.0f, add.height());
		canvas.setColor(kMuted);
		canvas.text(phantomPasses_, font(kKnobLabelSize), visage::Font::kLeft, add.x(), add.bottom() + 4.0f, add.width(),
		            kLineHeight);
	}
	else if (column_ == nullptr) {
		// **NO RUNNING INSTANCE BEHIND THE CHAIN, SO NO COLUMN AND NO (+).** `ChainExpanded` says the
		// same sentence for the same state: a (+) over a null bridge is a button that cannot add, which
		// is exactly what the honesty rule forbids.
		const visage::Bounds box = columnBounds();
		canvas.setColor(kMuted);
		canvas.text(kNotRunning, face, visage::Font::kLeft, box.x() + kPadX, box.y(),
		            std::max(0.0f, box.width() - 2.0f * kPadX), kLineHeight);
	}
}

void FxLaneRail::setPhantom(std::string passes, std::function<void()> onAdd) {
	phantomPasses_ = std::move(passes);
	onPhantomAdd_ = std::move(onAdd);
	redraw();
}

visage::Bounds FxLaneRail::phantomAddBounds() const {
	const visage::Bounds box = columnBounds();
	return { box.x() + kPadX * 0.5f, box.y() + 4.0f, std::max(0.0f, box.width() - kPadX), 28.0f };
}

} // namespace feathers::ui
