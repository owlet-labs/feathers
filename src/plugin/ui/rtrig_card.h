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
#include "mod/rtrig/rtrig_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// THE RANDOM TRIGGER'S CARD (backlog NA (a)) — DRAWN AROUND EVENTS, not NOISE's face: a running timeline of hits
// with their sizes, and the distribution's shape.
//
//     ┌───────────────────────────────┐
//     │ |   ||      |  ╲__   |        │   — THE TIMELINE: each hit a stem as tall as its size (down when negative),
//     └───────────────────────────────┘     the Decay output a falling line behind them, newest at the right edge
//     ┌──── when ────┐ ┌─── size ────┐
//     │  gap density │ │ size density │  — THE DISTRIBUTIONS, from `rtrig::gapDensity` / `sizeDensity`, the
//     └──────────────┘ └──────────────┘    generator's own law (checked against its histogram in test_mod_rtrig)
//     [ Regular | Poisson | Clustered ]   [ Fixed | Uniform | Exp ]  ±
//     Density   Sync   Jitter/Cluster
//     [ Stable | Free ]  Seed      Decay   Poly
//
// **THE TIMELINE IS THE ENGINE'S, READ AT THE FRAME RATE.** The device publishes its hit count and the newest hit's
// size on the playhead road (one word) and the Decay output's magnitude beside it; each frame the card pushes one
// column: how many hits arrived since the last frame and EVERY ONE'S SIZE (`DeviceHost::recentEventsAt`, the device's
// newest hits by count) — at 200 hits a second a frame holds three, and each is drawn as its own stem.
//
// **A CONTROL IS SHOWN EXACTLY WHERE THE DEVICE READS IT** (the honesty rule): Density only while Sync is Free,
// Jitter under Regular, Cluster under Clustered, Seed under Stable — `rtrig::liveIn`.

namespace feathers::ui {

class Notifier;

class RandomTriggerCard final : public FxCard {
public:
	RandomTriggerCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                  Notifier* notifier = nullptr, ModState* mod = nullptr, EditHistory* history = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	RandomTriggerCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr, ModState* = nullptr,
	                  EditHistory* = nullptr) = delete;
	~RandomTriggerCard() override;

	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::rtrig::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kTimelineHeight = 60.0f;
	static constexpr float kDistHeight = 40.0f;
	static constexpr float kSwitchHeight = 16.0f;
	static constexpr float kChipWidth = 34.0f;
	/// THE TRIG PAD'S COLUMN, left of the timeline it fires into (the LFO's seat for it).
	static constexpr float kTrigColumn = TrigPad::kDiameter + 6.0f;
	/// THE TIMELINE'S LENGTH — one column a frame, one pixel a column; wider than any card the strip lays out.
	static constexpr size_t kTimelineCapacity = 512;

	static constexpr float bodyHeight() {
		return kGap + kTimelineHeight + kGap + kDistHeight + kGap + 2.0f * (kSwitchHeight + kGap)
		       + 2.0f * (ValueBox::kHeight + kGap);
	}
	/// OPEN, IN A VERTICAL LANE — `StereoToolCard::openHeight`'s shape, for `ModPanel::bodyHeightOf`.
	static float openHeight(size_t outputRows);

	[[nodiscard]] DeviceControl& control(mod::rtrig::Param p) const;
	[[nodiscard]] bool isLive(mod::rtrig::Param p) const;
	[[nodiscard]] std::vector<mod::rtrig::Param> addressedParamsForTest() const;
	/// THE WIDGET ON `p`, or null.
	[[nodiscard]] visage::Frame* widgetForTest(mod::rtrig::Param p) const;

	/// **THE TIMELINE'S SEAM** — one column a frame; a case asserts the values the stems are drawn from.
	struct Column {
		uint32_t hits = 0;
		float size = 0.0f;
		float decay = 0.0f;
		/// **EVERY HIT THE FRAME HELD, NEWEST FIRST** (`DeviceHost::recentEventsAt`): `sized` of them, each drawn as its
		/// own stem. Where the host keeps none, the newest size alone.
		std::array<float, device::kRecentEvents> sizes {};
		uint32_t sized = 0;
	};
	[[nodiscard]] size_t timelineCountForTest() const noexcept { return count_; }
	/// The column `back` frames ago (0 = newest); `{}` past what is held.
	[[nodiscard]] Column timelineAtForTest(size_t back) const noexcept;
	/// PUSH ONE COLUMN FROM THE ENGINE'S READING, as a frame does (what `updateContent` calls).
	void sampleForTest() { sample(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] uint32_t layoutKey() const;
	void layOut();
	/// ONE COLUMN FROM THE PUBLISHED READING; false when the device publishes none (a host without the road).
	bool sample();
	[[nodiscard]] visage::Bounds timelineBounds() const;
	[[nodiscard]] visage::Bounds gapBounds() const;
	[[nodiscard]] visage::Bounds sizeBounds() const;
	void drawTimeline(visage::Canvas& canvas, visage::Bounds box) const;
	void drawGap(visage::Canvas& canvas, visage::Bounds box) const;
	void drawSize(visage::Canvas& canvas, visage::Bounds box) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	uint32_t laidOutKey_ = ~0u;
	/// WHAT THE DISTRIBUTIONS WERE LAST DRAWN FROM — they repaint only when it moves.
	uint64_t drawnShapeKey_ = ~0ull;

	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(mod::rtrig::Param::kCount)> controls_;
	std::unique_ptr<SegmentedControl> timing_, hitSize_, seedMode_;
	std::unique_ptr<SwitchChip> polarity_, poly_;
	/// THE TRIG (backlog MV): a click fires one hit; it lights on each fire, from the device's count.
	std::unique_ptr<TrigPad> trigPad_;
	std::vector<Watch> watches_;
	std::array<std::unique_ptr<ValueBox>, static_cast<size_t>(mod::rtrig::Param::kCount)> boxes_;

	std::array<Column, kTimelineCapacity> ring_ {};
	size_t head_ = 0;
	size_t count_ = 0;
	bool haveCount_ = false;
	uint32_t lastHits_ = 0;
	/// HOW MANY OF THE NEWEST COLUMNS ARE EMPTY — once they cover the plot, nothing moves and nothing repaints.
	size_t quiet_ = 0;
};

} // namespace feathers::ui
