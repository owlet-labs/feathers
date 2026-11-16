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
#include "plugin/ui/device_control.h"
#include "plugin/ui/rack_view_model.h"
#include "rack/parallel_rack.h"

#include <string>
#include <vector>

// THE RACK VIEW MODEL OVER A REAL RACK (the widgets of rack_views.h read the rack through this).
//
// What it reads and where it writes:
//  - the VIEW, the viewed chain and the pads' page are the rack's patch state (`Param::View`, `ViewedChain`, `PadsPage`), written straight to
//    the host (no undo step: a window's layout is not an edit);
//  - the PLAYING chain is the rack's real parameter (`Param::Playing`, a `DeviceControl` the owner holds), so a change is undoable and
//    reaches the host as a gesture; the chain the Position names answers while it is 0;
//  - a chain's open flag is the rack's own (`Rack::open`), and the macro row's is editor-only (held here);
//  - the strike mask is the engine's (`Rack::takeStrikeMask`): ONE consumer takes it, so each surface holding one of these takes its own
//    look; a note posted from the editor goes through the host's note road (`DeviceHost::postNote`), and `canTrigger` says whether there is one;
//  - the kit level is the rack's Gain.
// The rack is asked through the host afresh at every call (its handle is contemporaneous, never kept).

namespace feathers::ui {

/// **THE VIEW CHOICE WITHOUT TABS** (a rack of more than eight chains cannot be tabbed, so the choice that is offered is Strips | Pads):
/// the rack's `View` control seen through two segments, 0 = Strips and 1 = Pads (the control's 0 and 2). It is the same control (the same
/// descriptor entry, gestures and undo), only narrower, so a surface shows it where Tabs is not on offer and the three-way where it is.
class RackViewPairControl final : public device::Modulatable {
public:
	static constexpr const char* kNames[] = { "Strips", "Pads" };
	explicit RackViewPairControl(device::Modulatable& view) : view_(view) {}
	const device::ParamDesc& desc() const override { return view_.desc(); }
	double value() const override { return view_.value() >= 1.5 ? 1.0 : 0.0; }
	void setValue(double v) override { view_.setValue(v >= 0.5 ? 2.0 : 0.0); }
	void releaseValue(double v) override { view_.releaseValue(v >= 0.5 ? 2.0 : 0.0); }
	device::ModDisplay display() const override { return view_.display(); }
	void setAmount(float amount) override { view_.setAmount(amount); }
	void beginGesture() override { view_.beginGesture(); }
	void endGesture() override { view_.endGesture(); }
	std::string text() const override { return kNames[value() >= 0.5 ? 1 : 0]; }

private:
	device::Modulatable& view_;
};

class RackViewAdapter final : public RackViewModel {
public:
	/// The controls are the owner's and outlive the adapter; `gain` is the kit level, `playing` and `mode` the rack's own.
	struct Controls {
		DeviceControl* playing = nullptr;
		DeviceControl* mode = nullptr;
		DeviceControl* position = nullptr;
		DeviceControl* gain = nullptr;
		/// Follow Played Note (the window's chip); null where the surface has none.
		DeviceControl* follow = nullptr;
	};
	RackViewAdapter(backplane::DeviceHost& host, DeviceInstanceId rack, Controls controls);

	RackViewKind view() const override;
	RackRoutingKind routing() const override;
	size_t chainSlots() const override;
	RackChainView chain(size_t slot) const override;
	size_t viewedChain() const override;
	void viewChain(size_t slot) override;
	size_t playingChain() const override;
	void setPlayingChain(size_t slot) override;
	device::Modulatable* playingControl() override { return controls_.playing; }
	int page() const override;
	void setPage(int page) override;
	bool expanded(size_t slot) const override;
	void setExpanded(size_t slot, bool expanded) override;
	/// THE PADS' CHAIN IN FRONT in three steps: 0 closed (the pads alone), 1 first (the first card), 2 whole (the chain). Editor-only (it
	/// folds a view and writes nothing the sound or a patch holds, so it carries no key; the rack's own open flag is Tabs' and keeps its
	/// meaning), per rack, and First by default. Tabs never reads it.
	[[nodiscard]] int padsFront() const noexcept { return padsFront_; }
	void setPadsFront(int state) noexcept { padsFront_ = state < 0 ? 0 : (state > 2 ? 2 : state); }
	bool macrosOpen() const override { return macrosOpen_; }
	void setMacrosOpen(bool open) override { macrosOpen_ = open; }
	RackKeyMask takeStruck() override;
	RackStrikeKey strikeKey() const override;
	bool followPlayed() const override;
	device::Modulatable* followControl() override { return controls_.follow; }
	bool canTrigger() const override { return host_.canPostNote(); }
	bool trigger(int key, bool on, float velocity) override { return host_.postNote(key, on, velocity); }
	device::Modulatable* kitLevel() override { return controls_.gain; }

	/// How many chains the rack holds (the Tabs choice is offered only up to `kMaxRackTabs`).
	[[nodiscard]] size_t chainCount() const;
	/// ADD A CHAIN ANSWERING `key` (one undo step through `history`, which may be null): an empty keyed chain with a Drum device in it set to
	/// Any Key where the chain's bridge takes one. Returns its index, or `kNoChain`.
	size_t addKeyedChain(int key, EditHistory* history);
	/// The name a chain shows: its first device's, or empty.
	[[nodiscard]] std::string chainTitle(size_t slot) const;
	/// The names of the devices a chain holds, in chain order.
	[[nodiscard]] std::vector<std::string> deviceNames(size_t slot) const;

private:
	[[nodiscard]] rack::parallel::Rack* rack() const;
	[[nodiscard]] int fieldInt(rack::parallel::Param p) const;
	void writeField(rack::parallel::Param p, int value);

	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	Controls controls_;
	bool macrosOpen_ = false;
	int padsFront_ = 1;
};

} // namespace feathers::ui
