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

#pragma once

#include "backplane/device_host.h"
#include "plugin/ui/arm_relay.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/mod_state.h"
#include "rack/rack_editor.h"

#include <cstdint>
#include <memory>
#include <string>

// **A DEVICE'S PRE AND POST CHAINS, AS ONE CHAIN WITH THE DEVICE IN IT** (backlog NR, then NZ) — what the card's site chip
// opens: the SAME
// chain editor with all its normal features, macros included. So this is the rack's own editor (`RackEditor`, the one
// a chain device's panel is) over PRE's bridge, with the device and POST as its tail (`RackEditor::SiteTail`). The
// macros are PRE's, which are the site's one set (decision O7), and reach the device and POST by the export road.
//
// **THE MODULATION HIERARCHY IS THE SCREEN'S ORDER.** Three states, each following the one before it: PRE's follows the
// host's; the centre's follows PRE's and spells a PRE source as the device's export (`DeviceHost::siteExport`) and the
// host's own sources as the host spells them; POST's follows the centre's and spells everything as POST reads it from
// outside (`asOuter()`). So a modulator glows exactly what lies to its right — the rest of PRE, the device, POST — and a
// POST modulator glows only POST (decision O3).
//
// **WHAT IT IS OPENED ON** is the site's key, `siteKeyOf(device)`: the host files the running site there as a container
// surface, and this view re-asks for it every tick and pins it while an editor is built over it, as `ChainExpanded` does
// for a chain device (CLAUDE.md's ABA rule: the pointer is a contemporaneous handle). A device that offers only POST (a
// generator, D5) shows PRE veiled, as it does while the device runs in the voices (D4).

namespace feathers::rack {

class RackBridge;
class SiteChains;

class SiteExpanded final : public ui::ExpandedView {
public:
	SiteExpanded(backplane::DeviceHost& host, DeviceInstanceId siteKey, ui::Notifier* notifier);
	~SiteExpanded() override;

	visage::Point minimumSize() const override;
	std::string title() const override { return "Pre / Post"; }
	void resized() override;
	void draw(visage::Canvas& canvas) override;
	bool updateContent() override;
	void setPresentedOn(ui::SurfaceLayers* host) override;
	void modulationBound(ui::ModState* mod, ui::EditHistory* history) override;

	[[nodiscard]] bool offersPreForTest() const noexcept { return offersPre(); }
	[[nodiscard]] bool preAvailableForTest() const { return preAvailable(); }
	[[nodiscard]] RackEditor* editorForTest() noexcept { return editor_.get(); }
	[[nodiscard]] ui::EditHistory& historyForTest() noexcept { return history_; }
	/// PRE's bridge — the editor's own chain.
	[[nodiscard]] RackBridge* bridgeForTest() noexcept { return bridge_; }
	[[nodiscard]] bool liveForTest() const noexcept { return editor_ != nullptr; }
	[[nodiscard]] DeviceInstanceId ownerForTest() const noexcept { return owner(); }
	[[nodiscard]] ui::ModState& preModForTest() noexcept { return mod_; }
	[[nodiscard]] ui::ModState& centreModForTest() noexcept { return centreMod_; }
	[[nodiscard]] ui::ModState& postModForTest() noexcept { return postMod_; }
	/// Runs the relay now, as the tick does (`updateContent`).
	bool relayArmForTest() { return relayArm(); }

protected:
	void onInnerNestChanged() override;

private:
	/// The device the site belongs to, its descriptor, and whether its site offers PRE.
	[[nodiscard]] DeviceInstanceId owner() const noexcept;
	[[nodiscard]] bool offersPre() const;
	/// **PRE IS OFFERED AND DOES SOMETHING NOW** — not while the device runs inside the voices, where no PRE can feed it
	/// (D4): its segment is then veiled and refuses the hand (the honesty rule).
	[[nodiscard]] bool preAvailable() const { return offersPre() && !host_.runsPerVoice(owner()); }
	/// Why PRE is veiled, in words, or empty while it is available.
	[[nodiscard]] std::string preUnavailableReason() const;
	bool follow();
	void dropEditor();
	/// The editor's tail — the device and POST — told again (after the states or the histories moved).
	void attachTail();

	backplane::DeviceHost& host_;
	DeviceInstanceId key_ = kNoInstance;
	void* surface_ = nullptr;
	/// THE SITE HELD AGAINST THE HOST'S FREE while an editor is built over it.
	backplane::DeviceHost::SurfacePin pin_ { host_ };
	RackBridge* bridge_ = nullptr;
	/// BEFORE THE EDITOR, so they outlive the cards and arms on their watcher lists. PRE's, the centre's, POST's.
	ui::ModState mod_;
	ui::ModState centreMod_;
	ui::ModState postMod_;
	/// The lanes' undo; the device's own edits record in the host's (`outerHistory_`) as they do on its card.
	ui::EditHistory history_;
	ui::EditHistory* outerHistory_ = nullptr;
	/// The lanes' history forwarded to the host's (backlog JB), tied to the site's bridge.
	void bindHistory();
	/// **RUNG 4: THE HOST'S OWN STATE, ARMED FOR A PRE SOURCE** (backlog NZ) — so the host's lane cards after the device glow
	/// for it. A borrowed arm, put back as it was found; see `relayArm`.
	ui::ModState* outerMod_ = nullptr;
	ui::BorrowedArmRelay relay_;
	/// PRE's own arm, mirrored into the host's state as the export — or taken back off it.
	bool relayArm();
	bool preShown_ = true;
	std::unique_ptr<RackEditor> editor_;
	ui::SurfaceLayers* presenter_ = nullptr;
};

void registerSiteExpandedView();

} // namespace feathers::rack
