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

#pragma once

#include "backplane/device_host.h"
#include "device/registry.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// THE FOLLOWER'S PICTURE (backlog MY: "card around a plot of the envelope with the threshold line drawn on it").
//
//     ┌──────────────────────────────┐
//     │  ╷        ╷         ╷        │  — a mark at the top for each Trig pulse
//     │──────────────────────────●── │  — THE THRESHOLD, its handle at the right: drag it up and down
//     │ ·  ·  ·  ·  ·  ·  ·  ·  ·  · │  — the re-arm level, half of it, faint
//     │   /\__    /\_____   /\_      │  — the envelope as the ENGINE published it, newest at the right
//     └──────────────────────────────┘
//
// **WHAT IT DRAWS IS WHAT THE ENGINE PUBLISHED**, never a second follower run on the UI thread: each frame it
// reads the device's level (`DeviceHost::playheadLevelAt`, the Env output as published) and its pulse count
// (`trigFiresAt`), and scrolls them in a ring a plot's width long. A host that publishes neither draws no trace.
//
// **THE THRESHOLD IS A HANDLE ON THE PLOT, where it acts.** A press puts the line under the pointer and a drag
// moves it; with a source armed the press is a ROUTE drag on Threshold, `Knob`'s gesture (`NoisePlot`'s too).

namespace feathers::ui {

class ModState;
class EditHistory;

class FollowerPlot final : public visage::Frame, public TooltipSource {
public:
	/// `desc` is the follower's own descriptor; `threshold` the index of its Threshold control.
	FollowerPlot(backplane::DeviceHost& host, DeviceInstanceId id, const device::Descriptor& desc, size_t threshold);

	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override;
	void setModulation(ModState* mod, EditHistory* history);

	/// ONE FRAME: read the published level and pulse count, push them, and report whether the picture moved —
	/// the card's `updateContent` contract. Still once every value the plot shows is the newest and nothing fired.
	bool update();
	/// **BIPOLAR UNDER SIGNED** (backlog OP): zero is the plot's middle, the envelope is filled from it, and the
	/// threshold (0..1) and its re-arm line live in the upper half. The level read is the drawn 0..1 either way
	/// (`Follower::drawnLevel`, 0.5 is zero under Signed).
	void setBipolar(bool bipolar);
	[[nodiscard]] bool bipolar() const noexcept { return bipolar_; }

	/// THE RING'S LENGTH — one reading a frame, one pixel a reading; wider than any card lays out.
	static constexpr size_t kCapacity = 512;
	static constexpr float kInset = 4.0f;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — the plotted VALUES.                                                              //
	//----------------------------------------------------------------------------------------------//
	/// The level `back` frames ago (0 = newest); 0 past what is held.
	[[nodiscard]] float levelAtForTest(size_t back) const noexcept;
	/// Whether a pulse was seen on that frame.
	[[nodiscard]] bool firedAtForTest(size_t back) const noexcept;
	[[nodiscard]] size_t countForTest() const noexcept { return count_; }
	/// The threshold the line is drawn at, as the ENGINE resolved it.
	[[nodiscard]] float thresholdForTest() const noexcept { return threshold_; }
	[[nodiscard]] float levelToYForTest(float v) const noexcept { return levelToY(v); }
	[[nodiscard]] DeviceControl& thresholdControl() const;

private:
	[[nodiscard]] float levelToY(float v) const noexcept;
	[[nodiscard]] float yToLevel(float y) const noexcept;
	/// A VALUE OF THE THRESHOLD'S (0..1) IN DRAWN UNITS — itself, or the upper half when bipolar.
	[[nodiscard]] float drawnOf(float value) const noexcept { return bipolar_ ? 0.5f * (value + 1.0f) : value; }
	void place(const visage::MouseEvent& e);
	void push(float level, bool fired) noexcept;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	const device::Descriptor& desc_;
	size_t thresholdIndex_ = 0;
	mutable std::unique_ptr<DeviceControl> threshold_control_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	std::array<float, kCapacity> level_ {};
	std::array<uint8_t, kCapacity> fired_ {};
	size_t head_ = 0;
	size_t count_ = 0;
	/// HOW MANY OF THE NEWEST FRAMES ARE THE SAME UNFIRED LEVEL — once it covers the plot, nothing repaints.
	size_t still_ = 0;
	bool haveFires_ = false;
	uint32_t lastFires_ = 0;
	float threshold_ = 0.5f;
	bool bipolar_ = false;
	bool drawnArmed_ = false;
	float drawnAmount_ = 0.0f;

	struct Drag {
		bool active = false;
		bool route = false;
		float startY = 0.0f;
		float anchor = 0.0f;
	} drag_ {};
};

} // namespace feathers::ui
