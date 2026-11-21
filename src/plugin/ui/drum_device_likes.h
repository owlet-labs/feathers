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
#include "plugin/ui/drum_device_pad.h"
#include "plugin/ui/drum_device_ui.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/tooltip.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>
#include <visage/ui.h>

// **THE DRUM'S LIKES, ON ITS WINDOW**: the heart and MUTATE under the terrain pad, as the synth's chart has them. The heart likes the drum
// where it stands (or, standing on a like, removes it); a right press opens its menu (clear, and the likes' strength). The likes are the
// device's own block (`drum.likes.v1`), written whole through `DeviceHost::setBlock`, so a like is one undo step and travels with the device.
// What they make of the pad is derived (`DrumLikeState`): the warp the picture is drawn through, which MUTATE's roll is drawn on too.

namespace feathers::ui {

class DrumLikeBar final : public visage::Frame, public TooltipSource {
public:
	DrumLikeBar(DrumDeviceModel& model, DrumDevicePad& pad, Notifier* notifier);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point local) const override;
	/// MUTATE drives Travel X and Y, which the pad already offers as a gesture; it declares them here too, being a second road to them.
	void offeredControls(std::vector<Offered>& out) const override;

	static constexpr float kHeight = 22.0f;
	static constexpr float kHeartWidth = 30.0f;
	static constexpr float kMutateWidth = 66.0f;
	static constexpr float kGap = 6.0f;
	[[nodiscard]] visage::Bounds heartBounds() const { return { 0.0f, 0.0f, kHeartWidth, height() }; }
	[[nodiscard]] visage::Bounds mutateBounds() const { return { kHeartWidth + kGap, 0.0f, kMutateWidth, height() }; }

	/// Every like in the device's block (any plane, any Kind), and the ones that belong to the terrain being drawn.
	[[nodiscard]] std::vector<dd::DrumLike> all() const;
	[[nodiscard]] const std::vector<dd::DrumLike>& applied() const noexcept { return applied_; }
	/// What the likes make of this plane now: null with none. Re-derived only when the likes, the plane, the Kind, the scalers or the strength move.
	[[nodiscard]] std::shared_ptr<const DrumLikeState> state() const noexcept { return state_; }
	/// Called every frame by the window: reads the block and the row, re-derives if one moved. True if the state changed.
	bool refresh();

	/// THE HEART: likes the drum where it stands, or removes the like it stands on. One undo step. False if nothing changed (the list is full).
	bool toggleLike();
	/// Every like of this Kind (any plane) gone, in one undo step.
	void clearLikes();
	/// MUTATE: one roll of the dice, drawn on the warped view. One undo step (the travel's group).
	void mutate();
	/// **THE HEART'S MENU**: "Clear all likes on <Kind>" (greyed with none), then "Like strength x1/x2/x4".
	[[nodiscard]] visage::PopupMenu menu();
	[[nodiscard]] int strength() const noexcept { return strength_; }
	static constexpr int kClearLikes = 1;
	static constexpr int kLikeStrength = 100;
	/// The dice, for the cases: the next two rolls in [-1, 1].
	void seedDiceForTest(uint32_t seed) noexcept { dice_ = seed != 0 ? seed : 1u; }
	/// How far one roll reaches, in travel: a half view at the pad's zoom.
	[[nodiscard]] double reach() const;

private:
	[[nodiscard]] bool writeLikes(const std::vector<dd::DrumLike>& likes);
	[[nodiscard]] double rollUnit() noexcept;

	DrumDeviceModel& model_;
	DrumDevicePad& pad_;
	Notifier* notifier_ = nullptr;
	std::vector<dd::DrumLike> applied_;
	std::vector<dd::DrumLike> lastBlock_;
	std::shared_ptr<const DrumLikeState> state_;
	std::array<float, 13> stateKey_ {};
	int stateStrength_ = 0;
	int strength_ = 1;
	bool shownLiked_ = false;
	uint32_t dice_ = 0x9e3779b9u;
};

} // namespace feathers::ui
