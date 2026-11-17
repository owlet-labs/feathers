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

#include "plugin/ui/chain_column.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/tooltip.h"

#include <cstdint>
#include <memory>

// THE FRONT DEVICE'S OWN WINDOW, HELD IN THE RACK WINDOW'S TABS VIEW (the rack's `rack_front` = Window).
//
// **A WINDOW THAT IS A PANE.** The device's expanded view is built by the same factory its own window opens through
// (`ChainColumn::buildWindowContent`: the same modulation state, undo history and nest), and is placed here, under a thin header that
// carries the device's name and its enable (the card's head square is the one control a window's body does not hold, and the first
// card is held back from the column while the pane shows it). It is the same device the card is bound to, so a control turned here and
// the card's, or an open window's, are one parameter.
//
// **IT IS KEPT BY IDENTITY, NEVER BY ADDRESS.** `follow` is handed the chain's stable id and its column; it compares the chain id, the
// bridge's life token (a weak pointer to the thing a fragment's undo records hold, which cannot be reused while this holds it), the first
// device's id and its descriptor with what the pane was built over, and any difference drops the content and builds another. A tab switch,
// a reorder that changes the first device, a replacement, a deleted chain or an undone one therefore rebuild; nothing is ever re-bound in
// place. `validate` is the half that runs BEFORE the rails follow their chains: it drops content whose chain has gone or changed so nothing
// built over a dying bridge is drawn or pressed.
//
// **THE CONTENT DIES BEFORE THE STATE IT IS WATCHED ON.** Its knobs are on the rail's modulation list (raw frame pointers), so `release`
// takes the whole tree off that list first (`forgetFrameTree`) and the window releases the pane before it drops a rail.
//
// **A PANE NEVER NESTS.** Where the first device is itself a rack, its window is shown as it opens with `rack_front` = Cards.
// Where the first device has no window of its own its card is shown, centred.

namespace feathers::ui {

class SurfaceLayers;

class RackWindowPane final : public visage::Frame, public TooltipSource {
public:
	enum class Hosting { Nothing, Window, Card };

	RackWindowPane();
	~RackWindowPane() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	const char* tooltipAt(visage::Point at) const override;
	/// It declares nothing itself: the content's controls are offered by the content, and the enable chip is the window header's.
	void offeredControls(std::vector<Offered>&) const override {}

	/// BEFORE THE RAILS FOLLOW: drop the content unless it is over chain `chainId` on the bridge `life` names (`chainId` 0xFFFF: none).
	/// True when it dropped something.
	bool validate(uint16_t chainId, const std::weak_ptr<const void>& life);
	/// BIND TO THE FRONT CHAIN'S FIRST DEVICE: `column` is the chain's (null: nothing to show), `chainId` its stable id. Builds or drops
	/// as the identity says. True when what is hosted changed.
	bool follow(ChainColumn* column, uint16_t chainId);
	/// Take everything down now (the window does this before it drops a rail).
	void release();
	/// Advance what is hosted; how many frames said the screen is out of date.
	size_t update();
	/// Where the content's panels go, and the host's say over what it is told (a container's window nests its own chain).
	void setPresentedOn(SurfaceLayers* host);

	[[nodiscard]] Hosting hosting() const noexcept { return hosting_; }
	/// The size the pane wants: the content's own natural window size under the header, or the card with its margin. Zero when nothing is hosted.
	[[nodiscard]] visage::Point naturalSize() const;
	/// The device that is hosted (none: `kNoInstance`).
	[[nodiscard]] DeviceInstanceId device() const noexcept { return device_; }
	/// The press on the header's enable chip, as the hand makes it; its place and what it last drew.
	void toggleEnableForTest() { toggleEnable(); }
	[[nodiscard]] visage::Bounds enableBounds() const;
	[[nodiscard]] ExpandedView* contentForTest() noexcept { return content_.get(); }
	[[nodiscard]] FxCard* cardForTest() noexcept { return card_.get(); }
	[[nodiscard]] visage::Bounds contentBounds() const;
	[[nodiscard]] uint16_t chainIdForTest() const noexcept { return chainId_; }
	/// How many times content was built (a rebuild is a build), for the cases that say "not rebuilt".
	[[nodiscard]] size_t buildsForTest() const noexcept { return builds_; }

	static constexpr float kHeaderHeight = 22.0f;
	static constexpr float kEnableSize = 12.0f;
	static constexpr float kCardMargin = 8.0f;

private:
	void toggleEnable();
	void drop();

	std::unique_ptr<ExpandedView> content_;
	std::unique_ptr<FxCard> card_;
	Hosting hosting_ = Hosting::Nothing;
	/// WHAT THE CONTENT WAS BUILT OVER.
	uint16_t chainId_ = 0xFFFF;
	std::weak_ptr<const void> life_;
	DeviceInstanceId device_ = kNoInstance;
	const device::Descriptor* desc_ = nullptr;
	/// The list the content's frames are on, and the layers its panels go to.
	FrameWatchers* modulation_ = nullptr;
	SurfaceLayers* presenter_ = nullptr;
	size_t builds_ = 0;
	bool enableShown_ = true;
};

} // namespace feathers::ui
