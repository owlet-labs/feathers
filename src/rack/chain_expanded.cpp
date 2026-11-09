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

#include "rack/chain_expanded.h"

#include "composite/composite_types.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/theme.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/site_expanded.h"

namespace feathers::rack {

ChainExpanded::ChainExpanded(backplane::DeviceHost& host, DeviceInstanceId device, ui::Notifier* notifier)
    : host_(host), device_(device) {
	// THE OUTER NOTIFIER IS NOT KEPT. It was stored and never read — clang's -Wunused-private-field stopped
	// macOS CI run 35156338285 on it — because refusals are said on the inner editor's own tray
	// (`sayRefusals`), which is where this panel's messages belong. The preset browser says its own on it.
	presets_ = ui::makeChainDevicePresets(host, device, notifier);
	(void)follow();
}

ChainExpanded::~ChainExpanded() {
	// A RELAYED ARM GOES WITH THE PANEL: the host's cards must not stay lit for a modulator nobody can see.
	(void)relay_.release();
	dropEditor();
}

visage::Point ChainExpanded::minimumSize() const {
	// THE SAME WIDTH AS A RACK: the chain's Mix and Gain stand in the macro column (backlog OC), not in the row.
	return { static_cast<float>(RackEditor::kWidth), static_cast<float>(RackEditor::kHeight) };
}

void ChainExpanded::dropEditor() {
	if (editor_ != nullptr) {
		removeChild(editor_.get());
		editor_.reset();
	}
	// **AND ONLY THEN IS THE INSTANCE LET GO** — the editor's own teardown reads the bridge (its
	// subscriptions end there). See `follow` for why it was held.
	pin_.hold(nullptr);
	bridge_ = nullptr;
	surface_ = nullptr;
}

bool ChainExpanded::follow() {
	void* now = host_.containerSurfaceOf(device_);
	if (now == surface_ && (now == nullptr) == (editor_ == nullptr)) {
		return false;
	}
	dropEditor();
	surface_ = now;
	bridge_ = bridgeOf(now);
	bindHistory();
	if (bridge_ == nullptr) {
		surface_ = now; // remembered, so a handle that stays null does not report a change every tick
		return true;
	}
	// **THE INSTANCE IS PINNED FOR AS LONG AS THE EDITOR IS BUILT OVER IT** (backlog JX). This follow runs
	// once a beat, and the reap that frees a retired instance does not wait for it: Feathers reaps AFTER
	// the beat's follow (`guiTick`), and a rack reaps on every commit, from whatever handler made it. So
	// between a reap and the next follow the editor could be drawn, or turned, over freed memory. Pinned,
	// the reap leaves it parked and clears only the handle; the next follow sees that, drops the editor
	// and unpins, and the reap after frees it. It also means nothing new can be built at this address
	// while it is remembered, so the comparison above cannot be fooled by reuse.
	pin_.hold(now);
	// THE GATE INSIDE WAS HANDED THE HOST OUTSIDE AT PROVISION (rack roadmap §7, `Converger`), not here:
	// a loaded patch's outer routes are gated with no panel open.
	editor_ = std::make_unique<RackEditor>(*bridge_, mod_, history_);
	editor_->setOuterLayers(presenter_);
	// **ONE LEVEL DEEPER, AND RE-TOLD ON EVERY REBUILD.** The editor is dropped and rebuilt whenever the
	// host files a new handle for this device (trap T9), so an editor built after the panel was stamped
	// would be back at the outer rack's nest — sharing ids, tints and overlay keys with it.
	editor_->setNest(innerNest());
	// THE MACRO ROW IS THE OUTER HOST'S: this device's eight params, on the outer state.
	editor_->setOuterDevice(&host_, device_, outerMod_, outerHistory_);
	addChild(editor_.get());
	editor_->setBounds(0, 0, width(), height());
	// WHAT THE LOAD REFUSED, said now that there is a surface to say it on: an inner route whose outer
	// source the host outside no longer has was dropped at load, with nobody to tell.
	sayRefusals(bridge_->takeLoadReport());
	return true;
}

void ChainExpanded::setPresentedOn(ui::SurfaceLayers* host) {
	presenter_ = host;
	if (editor_ != nullptr) {
		editor_->setOuterLayers(presenter_);
	}
}

void ChainExpanded::onInnerNestChanged() {
	if (editor_ != nullptr) {
		editor_->setNest(innerNest());
	}
}

void ChainExpanded::bindHistory() {
	// ONE HISTORY PER PLUGIN (backlog JB): this window's edits record in the host's, out to the outermost, each tied to
	// this chain's bridge and to this panel (`EditHistory::forwardTo`). With no bridge there is nothing to tie to.
	history_.forwardTo(bridge_ != nullptr ? outerHistory_ : nullptr,
	                   bridge_ != nullptr ? bridge_->fragmentLife() : std::weak_ptr<const void> {});
}

void ChainExpanded::modulationBound(ui::ModState* mod, ui::EditHistory* history) {
	outerMod_ = mod;
	outerHistory_ = history;
	bindHistory();
	relay_.setTarget(mod);
	// THE INNER STATE FOLLOWS THE OUTER (rack roadmap §7): an outer source armed is what every inner
	// control reads, spelled as an outer source, and the inner gate admits it from upstream.
	mod_.setParent(mod);
	if (editor_ != nullptr) {
		editor_->setOuterDevice(&host_, device_, outerMod_, outerHistory_);
	}
}

void ChainExpanded::resized() {
	if (editor_ != nullptr) {
		editor_->setBounds(0, 0, width(), height());
	}
}

bool ChainExpanded::updateContent() {
	if (follow()) {
		(void)relayArm(); // an instance gone takes its relay with it
		return true;
	}
	if (editor_ == nullptr) {
		return relayArm();
	}
	// THE RACK GUI'S TICK, VERBATIM: reap what the drain retired, then the cards and the macros — and the
	// hygiene across the boundary (rack roadmap §7): routes whose outer source is gone or downstream are
	// dropped, and this panel's tray says so, naming the exact pair.
	(void)bridge_->service();
	sayRefusals(bridge_->takeLoadReport());
	editor_->syncCards();
	editor_->syncMacros();
	const bool relayed = relayArm();
	return editor_->updateCards() > 0 || relayed;
}

bool ChainExpanded::relayArm() {
	// **THE GLOW OUT THE BACK OF A CHAIN** (backlog NZ (b)). The road is built — a modulator inside reaches the devices
	// after the chain as `Source::exported(chain, row)` — but a hand arming one in this panel lit only this panel. So
	// while an inner source is armed HERE (locally: an outer source armed outside is the host's already), the host's
	// state is armed for its export. The host answers nobody for what does not export (an inner device that is not a
	// modulator, a full table), and the relay then lets go.
	device::Source want = device::Source::nobody();
	if (outerMod_ != nullptr && editor_ != nullptr && mod_.armedLocally()) {
		want = host_.containerExport(device_, mod_.source());
	}
	return relay_.relay(want);
}

void ChainExpanded::sayRefusals(const LoadReport& report) {
	if (report.empty() || editor_ == nullptr) {
		return;
	}
	// THE FIRST IN FULL AND THE REST COUNTED — `RackGui::sayLoadReport`'s shape (`loadReportLine`). The
	// source is named by the inner state, which names an outer source through its parent, marked "↑".
	const RouteRefusals& refused = report.routes;
	std::string phrase;
	if (refused.count > 0) {
		std::string where = "a device this chain does not hold";
		if (refused.firstDestination.isDevice()) {
			const size_t at = bridge_->positionOf(refused.firstDestination.device);
			if (at != RackBridge::kNoSlot) {
				where = "device " + std::to_string(at + 1) + ", control "
				        + std::to_string(refused.firstDestination.index + 1);
			}
		}
		phrase = mod_.sourceName(refused.firstSource) + " no longer reaches " + where;
	}
	editor_->notifier().say(loadReportLine(report, phrase));
}

void ChainExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(ui::kSurface);
	canvas.fill(0, 0, width(), height());
	if (editor_ == nullptr) {
		// THE INSTANCE THIS VIEW OPENED OVER IS GONE — rebuilt under a new body, or not built yet. Saying
		// so beats drawing an editor over a bridge that no longer exists.
		canvas.setColor(ui::kMuted);
		canvas.text("This chain is not running here — close and reopen it.", ui::font(ui::kKnobReadoutSize),
		            visage::Font::kCenter, 0.0f, 0.0f, width(), height());
	}
}

void registerChainExpandedView() {
	// AND A DEVICE'S PRE AND POST CHAINS (backlog NR), from the same module and by the same hook, so every GUI that
	// hosts chains can open a site's panel too.
	registerSiteExpandedView();
	ui::registerExpandedView(composite::kTraits.tag,
	                         [](backplane::DeviceHost& h, ParamBridge*, ui::WidgetRegistry&, DeviceInstanceId s,
	                            ui::Notifier* n) -> std::unique_ptr<ui::ExpandedView> {
		                         return std::make_unique<ChainExpanded>(h, s, n);
	                         });
}

} // namespace feathers::rack
