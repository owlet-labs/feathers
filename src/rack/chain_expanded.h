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

#include "backplane/device_host.h"
#include "plugin/ui/arm_relay.h"
#include "plugin/ui/container_presets.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/mod_state.h"
#include "rack/rack_editor.h"

#include <memory>
#include <string>

// THE CHAIN DEVICE'S EXPANDED VIEW — THE RACK'S OWN EDITOR, OVER THE BRIDGE INSIDE THE DEVICE (rack
// roadmap §6, Stage B; `2026-09-10-chain-as-a-device.md` §4 rows 6j and 6h).
//
// A chain in a lane is a rack device: the same `RackEngine` the standalone plugin runs, and — since this
// step — the same `RackBridge` over it, built inside the device when it adopts its catalogue and drained by
// the device's own process. So the editor for what a chain HOLDS is not a new surface: it is `RackEditor`,
// the standalone's editor, over that bridge. Nesting recurses for free — a chain inside a chain gets the
// same card and the same expand, because the editor hosts cards through the same factory.
//
// **HOW THE VIEW REACHES THE BRIDGE, AND WHY IT RE-ASKS EVERY TICK.** The host's carrier files the running
// instance's storage under the device's id at provisioning (`ContainerBodies::attach`); the host hands it
// on as an opaque pointer (`DeviceHost::containerSurfaceOf`); this module, which built the device, reads
// the bridge out of it (`rack::bridgeOf`). That pointer is a contemporaneous handle and not an identity —
// CLAUDE.md's ABA rule — so `updateContent` re-asks by id every tick and, on a different answer, drops the
// editor rather than let it draw a bridge that no longer exists: a rebuilt device (a reloaded body, a load)
// is a different instance, and the view says so and asks to be reopened. **A RE-ASK ONCE A BEAT IS NOT
// ENOUGH ON ITS OWN** (backlog JX): the host's reap can free the instance between two beats, so the view
// also PINS what it built over (`DeviceHost::SurfacePin`) and the host frees it only once the view lets go.
//
// THE EDITOR'S OWN `ModState` AND `EditHistory` ARE THE VIEW'S: a chain's modulation assignments and undo
// are the inner rack's, not Feathers'. `setModulation` from the outer host is FOLLOWED (rack roadmap §7): the
// inner state answers the outer arm, and the macro row is the outer host's.
//
// REGISTERED, NOT INCLUDED: `expanded_views.cpp` cannot name this file (feathers-rack-editor links
// feathers-ui), so each GUI that hosts chains calls `registerChainExpandedView()` at creation.

namespace feathers::rack {

class RackBridge;
struct LoadReport;

class ChainExpanded final : public ui::ExpandedView {
public:
	ChainExpanded(backplane::DeviceHost& host, DeviceInstanceId device, ui::Notifier* notifier);
	ChainExpanded(backplane::DeviceHost&, size_t, ui::Notifier*) = delete;
	~ChainExpanded() override;

	visage::Point minimumSize() const override;
	std::string title() const override { return "Chain"; }
	/// **THE CHAIN'S PRESETS** (backlog OU): `Chains/` in the title row, as a device panel's are. A load replaces what the
	/// chain holds and its row; the editor follows the record it then holds.
	[[nodiscard]] visage::Frame* titleAccessory() const override { return presets_.browser.get(); }
	[[nodiscard]] ui::PresetBrowser& presetBrowserForTest() const { return *presets_.browser; }
	void resized() override;
	void draw(visage::Canvas& canvas) override;
	bool updateContent() override;
	/// THE HOST THIS PANEL IS ON, HANDED TO THE INNER EDITOR — rack roadmap §5's escalation. The
	/// inner rack's own expand verb then lands on the OUTERMOST layers rather than in a panel inside
	/// this panel; and it is re-applied every time `follow` rebuilds the editor, because the handle can
	/// change under an open panel.
	void setPresentedOn(ui::SurfaceLayers* host) override;
	/// THE OUTER HOST'S MODULATION STATE, kept for ONE thing: the macro row. The inner cards and their
	/// panels answer this view's own `mod_`; the eight macros are the chain device's params on the
	/// outer host and light for an outer source (`RackEditor::setOuterDevice`).
	void modulationBound(ui::ModState* mod, ui::EditHistory* history) override;

	/// Whether the view currently holds an editor over a running instance.
	[[nodiscard]] bool liveForTest() const noexcept { return editor_ != nullptr; }
	[[nodiscard]] RackEditor* editorForTest() noexcept { return editor_.get(); }
	[[nodiscard]] RackBridge* bridgeForTest() noexcept { return bridge_; }
	[[nodiscard]] const ui::ModState& modForTest() const noexcept { return mod_; }
	[[nodiscard]] ui::ModState& modForTest() noexcept { return mod_; }
	/// Runs the relay now, as the tick does (`updateContent`).
	bool relayArmForTest() { return relayArm(); }

protected:
	/// ONE LEVEL DEEPER FOR THE INNER EDITOR — see `RackEditor::setNest`.
	void onInnerNestChanged() override;

private:
	/// Read the host's handle for this device and (re)build the editor over it, or drop the editor when
	/// there is none. Returns whether anything changed.
	bool follow();
	void dropEditor();
	/// This panel's history forwarded to the host's (backlog JB), tied to the current bridge.
	void bindHistory();
	/// Say what the inner rack refused or dropped, on this panel's tray.
	void sayRefusals(const LoadReport& report);

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	void* surface_ = nullptr;
	/// THE INSTANCE HELD AGAINST THE HOST'S REAP while `editor_` is built over it — see `follow`.
	backplane::DeviceHost::SurfacePin pin_ { host_ };
	RackBridge* bridge_ = nullptr;
	ui::ModState mod_;
	ui::EditHistory history_;
	std::unique_ptr<RackEditor> editor_;
	ui::SurfaceLayers* presenter_ = nullptr;
	ui::ModState* outerMod_ = nullptr;
	ui::EditHistory* outerHistory_ = nullptr;
	/// **AN INNER MODULATOR ARMED HERE LIGHTS WHAT FOLLOWS THE CHAIN** (backlog NZ (b)): mirrored into the host's state as
	/// the chain's export, borrowed and put back as found — the site panel's rung-4 relay, at a Chain.
	ui::BorrowedArmRelay relay_;
	bool relayArm();
	ui::ContainerPresets presets_;
};

/// FILE THIS VIEW FOR THE CHAIN DEVICE'S TAG in the expanded-view table. Called by each GUI that hosts
/// chains, at creation; idempotent.
void registerChainExpandedView();

} // namespace feathers::rack
