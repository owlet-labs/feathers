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
 */

#include "rack/site_expanded.h"

#include "backplane/container_bodies.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/theme.h"
#include "rack/rack_bridge.h"
#include "rack/site_chains.h"

namespace feathers::rack {

SiteExpanded::SiteExpanded(backplane::DeviceHost& host, DeviceInstanceId siteKey, ui::Notifier* notifier)
    : host_(host), key_(siteKey) {
	(void)notifier; // refusals are said on the inner editor's own tray, as `ChainExpanded` says them
	// THE HIERARCHY (see the header): the centre follows PRE and spells a PRE source as the device's export and a host
	// source as the host spells it; POST follows the centre and reads everything from outside, the default spelling.
	centreMod_.setParent(&mod_);
	centreMod_.setOuterSpelling([this](device::Source s) {
		return s.isOuter() ? s.inner() : host_.siteExport(owner(), s);
	});
	postMod_.setParent(&centreMod_);
	(void)follow();
}

SiteExpanded::~SiteExpanded() {
	// A RELAYED ARM GOES WITH THE PANEL: the host's cards must not stay lit for a PRE nobody can see.
	(void)relay_.release();
	dropEditor();
}

DeviceInstanceId SiteExpanded::owner() const noexcept { return key_ & ~kSiteKeyBit; }

bool SiteExpanded::offersPre() const {
	const device::Descriptor* d = host_.descriptorOf(owner());
	return d != nullptr && siteLanesOf(*d) == device::Traits::SiteLanes::PreAndPost;
}

std::string SiteExpanded::preUnavailableReason() const {
	if (!offersPre()) {
		return "This device has no PRE: nothing comes before it to process";
	}
	if (host_.runsPerVoice(owner())) {
		return "PRE is off while the device runs per voice: no chain on the bus can feed it";
	}
	return {};
}

visage::Point SiteExpanded::minimumSize() const {
	// WIDER THAN A RACK BY THE FURNITURE'S TWO KNOBS, as a chain device's panel is (`ChainExpanded::minimumSize`).
	return { static_cast<float>(RackEditor::kWidth), // OC folded the macro column into kWidth
		     static_cast<float>(RackEditor::kHeight) };
}

void SiteExpanded::dropEditor() {
	if (editor_ != nullptr) {
		removeChild(editor_.get());
		editor_.reset();
	}
	pin_.hold(nullptr); // AND ONLY THEN is the site let go: the editor's teardown reads the lanes' bridges
	bridge_ = nullptr;
	surface_ = nullptr;
}

void SiteExpanded::bindHistory() {
	// ONE HISTORY PER PLUGIN (backlog JB): the lanes' edits record in the host's, tied to the site's bridge and this panel.
	history_.forwardTo(bridge_ != nullptr ? outerHistory_ : nullptr,
	                   bridge_ != nullptr ? bridge_->fragmentLife() : std::weak_ptr<const void> {});
}

void SiteExpanded::attachTail() {
	auto* site = static_cast<SiteChains*>(surface_);
	if (editor_ == nullptr || site == nullptr) {
		return;
	}
	RackEditor::SiteTail tail;
	tail.outer = &host_;
	tail.centre = owner();
	tail.post = site->post().bridge();
	tail.centreMod = &centreMod_;
	tail.postMod = &postMod_;
	tail.centreHistory = outerHistory_ != nullptr ? outerHistory_ : &history_;
	editor_->setSiteTail(tail);
	preShown_ = preAvailable();
	editor_->setPreAvailable(preShown_, preUnavailableReason());
}

bool SiteExpanded::follow() {
	void* now = host_.containerSurfaceOf(key_);
	if (now == surface_ && (now == nullptr) == (editor_ == nullptr)) {
		return false;
	}
	dropEditor();
	surface_ = now;
	auto* site = static_cast<SiteChains*>(now);
	if (site == nullptr || site->pre().bridge() == nullptr || site->post().bridge() == nullptr) {
		return true;
	}
	bridge_ = site->pre().bridge();
	bindHistory();
	pin_.hold(now);
	editor_ = std::make_unique<RackEditor>(*bridge_, mod_, history_);
	editor_->setOuterLayers(presenter_);
	editor_->setNest(innerNest());
	addChild(editor_.get());
	editor_->setBounds(0, 0, width(), height());
	attachTail();
	return true;
}

void SiteExpanded::setPresentedOn(ui::SurfaceLayers* host) {
	presenter_ = host;
	if (editor_ != nullptr) {
		editor_->setOuterLayers(presenter_);
	}
}

void SiteExpanded::onInnerNestChanged() {
	if (editor_ != nullptr) {
		editor_->setNest(innerNest());
	}
}

void SiteExpanded::modulationBound(ui::ModState* mod, ui::EditHistory* history) {
	// THE INNER STATE FOLLOWS THE OUTER (rack roadmap §7), as a chain device's panel's does: an outer source armed is
	// what every control inside reads, spelled as an outer source. AND THE DEVICE'S OWN EDITS RECORD WHERE ITS CARD'S
	// DO, so an undo in the host takes back a turn of the centre's knob.
	mod_.setParent(mod);
	outerHistory_ = history;
	bindHistory();
	outerMod_ = mod;
	relay_.setTarget(mod);
	attachTail();
}

bool SiteExpanded::relayArm() {
	// **RUNG 4, THE GLOW OUT THE BACK** (backlog NZ: a site modulator reaches out the back of the compound chain
	// to the devices after it). PRE's arm is local to this panel; the host's lane cards answer to the host's own state.
	// So while a hand holds a PRE source armed here, the host's state is armed for it too, spelled as the device's
	// export, and every lane card that may take the route volunteers through `mayRoute` as for any source. Borrowed and
	// put back as found — `ui::BorrowedArmRelay`, which a Chain's and a Rack's panel share (NZ (b)).
	device::Source want = device::Source::nobody();
	if (outerMod_ != nullptr && mod_.armedLocally() && preAvailable()) {
		want = host_.siteExport(owner(), mod_.source());
	}
	return relay_.relay(want);
}

void SiteExpanded::resized() {
	if (editor_ != nullptr) {
		editor_->setBounds(0, 0, width(), height());
	}
}

bool SiteExpanded::updateContent() {
	if (follow()) {
		return true;
	}
	if (editor_ == nullptr) {
		return false;
	}
	// PRE VEILED THE MOMENT THE DEVICE GOES INTO THE VOICES, AND BACK WHEN IT LEAVES (D4).
	bool moved = false;
	if (preAvailable() != preShown_) {
		preShown_ = preAvailable();
		editor_->setPreAvailable(preShown_, preUnavailableReason());
		moved = true;
	}
	auto* site = static_cast<SiteChains*>(surface_);
	(void)bridge_->service();
	if (site != nullptr && site->post().bridge() != nullptr) {
		(void)site->post().bridge()->service();
	}
	editor_->syncCards();
	editor_->syncMacros();
	moved = relayArm() || moved;
	return editor_->updateCards() > 0 || moved;
}

void SiteExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(ui::kSurface);
	canvas.fill(0, 0, width(), height());
	if (editor_ == nullptr) {
		canvas.setColor(ui::kMuted);
		canvas.text("These chains are not running here — close and reopen them.", ui::font(ui::kKnobReadoutSize),
		            visage::Font::kCenter, 0.0f, 0.0f, width(), height());
	}
}

void registerSiteExpandedView() {
	ui::registerExpandedView(ui::siteViewDescriptor().traits.tag,
	                         [](backplane::DeviceHost& h, ParamBridge*, ui::WidgetRegistry&, DeviceInstanceId key,
	                            ui::Notifier* n) -> std::unique_ptr<ui::ExpandedView> {
		                         return std::make_unique<SiteExpanded>(h, key, n);
	                         });
}

} // namespace feathers::rack
