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
#include "mod/modulator.h"
#include "mod/samplehold/samplehold_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/trig_pad.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// THE SAMPLE & HOLD CARD (backlog MW) — COMPOSED AROUND THE STAIRCASE IT HOLDS, not a list of its knobs.
//
//     (o) ┌──────────────────────────┐   — the Trig pad at the staircase's left, where a sample is taken from;
//         │  ▔▔▁▁▁▔▔▔▁▁▔▔▔  |  |  | │     the scope: the held value as steps, the slew's glide over them, and a
//         └──────────────────────────┘     tick at the foot for every sample taken (what Trig Out emits)
//     [Source][Clock ][Base  ][Rate  ]   — what is sampled and when
//     [Slew  ][Quant ][Prob  ][Amount]   — what happens to a sample
//     [Input ][Seed  ]                   — the gated two, each drawn only where it changes the sound
//     [ Free ][Bipolar][ Poly ]
//
// **THE SCOPE IS THE ENGINE'S OWN READING, NEVER A SECOND COPY OF THE DEVICE.** Each frame it reads the published
// playhead — how many samples have been taken (`ordinal`) and where the hold stands (`level`, raw 0..1) — and pushes
// it into a ring one pixel a frame — with every step taken since the frame before (`DeviceHost::recentEventsAt`), so a
// clock faster than the frame rate draws each frame's steps as one vertical rather than as fewer steps. The slew's glide is drawn over the steps by the device's own law, the exact
// one-pole `1 - exp(-dt/tau)` over the frame's real elapsed time, so the curve is the sound's to within a frame.
// Nothing published (a host with no engine behind it) draws an empty scope rather than a plausible invented one.
//
// **A BOX WHOSE CONTROL DOES NOTHING NOW IS NOT DRAWN** — Rate and Base under the Note and Trig clocks, Input under a
// noise source, Seed under Free (`device::relevantFor`, the descriptor's gates). The rows reflow.

namespace feathers::ui {

class SampleHoldCard final : public FxCard {
public:
	SampleHoldCard(backplane::DeviceHost& host, DeviceInstanceId id, ModState& mod, EditHistory& history);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	SampleHoldCard(backplane::DeviceHost&, size_t, ModState&, EditHistory&) = delete;
	~SampleHoldCard() override;

	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::samplehold::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kRowGap = 4.0f;
	static constexpr float kTrigColumn = TrigPad::kDiameter + 6.0f;
	static constexpr float kScopeHeight = 72.0f;
	static constexpr size_t kColumns = 4;
	static constexpr size_t kBoxRows = 3;
	static constexpr float bodyHeight() {
		return kGap + kScopeHeight + kGap + static_cast<float>(kBoxRows) * (ValueBox::kHeight + kRowGap) - kRowGap
		       + kGap + SwitchChip::kHeight + kGap;
	}
	/// OPEN, IN A VERTICAL LANE — `MathCard::openHeight`'s shape, for `ModPanel::bodyHeightOf`.
	static float openHeight(size_t outputRows);
	/// THE RING'S LENGTH — one reading a frame, one pixel a reading; wider than any card the strip lays out.
	static constexpr size_t kScopeCapacity = 512;

	/// THE BOXES, IN READING ORDER — the grid flows them, skipping a box whose control is gated off.
	static constexpr std::array<mod::samplehold::Param, 10> kBoxes {
		mod::samplehold::Param::Source, mod::samplehold::Param::Clock,       mod::samplehold::Param::TimeBase,
		mod::samplehold::Param::Rate,   mod::samplehold::Param::Slew,        mod::samplehold::Param::Quantize,
		mod::samplehold::Param::Probability, mod::samplehold::Param::Amount, mod::samplehold::Param::Input,
		mod::samplehold::Param::Seed,
	};
	static constexpr std::array<mod::samplehold::Param, 3> kChips {
		mod::samplehold::Param::SeedMode, mod::samplehold::Param::Bipolar, mod::samplehold::Param::Poly,
	};

	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] ValueBox& box(mod::samplehold::Param p) const;
	[[nodiscard]] SwitchChip& chip(mod::samplehold::Param p) const;
	[[nodiscard]] TrigPad& trigPad() const { return *trigPad_; }
	/// Whether `p`'s box is drawn now — its gate, asked of the descriptor with the instance's value.
	[[nodiscard]] bool boxLive(mod::samplehold::Param p) const;
	/// Whether the Input knob does anything now: only under Source = Input (else it is drawn greyed, with the reason).
	[[nodiscard]] bool inputRead() const;
	/// How many rows the live boxes fill (1..kBoxRows); the rest of the grid's height is the scope's.
	[[nodiscard]] size_t boxRowsUsed() const;
	[[nodiscard]] visage::Bounds scopeBounds() const;
	/// Every device control this card puts a widget on, asked of the widgets' own controls.
	[[nodiscard]] std::vector<mod::samplehold::Param> addressedParamsForTest() const;

	/// **THE SCOPE'S SEAM** — one reading per frame, newest first. A case asserts the VALUE the staircase is drawn
	/// from, not its pixels.
	struct Reading {
		float level = 0.0f;   ///< the hold, raw 0..1 (the published playhead level)
		float glided = 0.0f;  ///< the slew's output over it, raw 0..1
		bool taken = false;   ///< a sample was taken since the reading before (the ordinal moved)
		/// **EVERY STEP SINCE THE READING BEFORE** (`DeviceHost::recentEventsAt`): how many, and the span of their levels —
		/// a clock faster than the frame rate takes several in one pixel, and the column draws all of them as one
		/// vertical rather than only the newest. One step: `lo == hi == level`.
		uint32_t steps = 0;
		float lo = 0.0f;
		float hi = 0.0f;
	};
	[[nodiscard]] size_t scopeCountForTest() const noexcept { return count_; }
	[[nodiscard]] Reading scopeAtForTest(size_t back) const noexcept;
	/// ONE FRAME'S LOOK AT THE PLAYHEAD, `dtSeconds` after the last — `updateContent` with the clock supplied.
	bool pollScope(double dtSeconds);

private:
	device::Modulatable* enableModulatable() override;
	void layoutControls();
	[[nodiscard]] float gateKey() const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	ModState& mod_;
	EditHistory& history_;
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(mod::samplehold::Param::kCount)> controls_;
	std::array<std::unique_ptr<ValueBox>, kBoxes.size()> boxes_;
	std::array<std::unique_ptr<SwitchChip>, kChips.size()> chips_;
	std::unique_ptr<TrigPad> trigPad_;
	std::vector<Watch> watches_;
	/// THE GATES' VALUES THE LAYOUT WAS LAST MADE FOR — Source, Clock and Seed Mode packed — so an undo, a load or host
	/// automation that flips one reflows the rows at the next frame.
	float gateDrawn_ = -1.0f;

	std::array<Reading, kScopeCapacity> ring_ {};
	size_t head_ = 0;
	size_t count_ = 0;
	size_t still_ = 0;
	bool haveOrdinal_ = false;
	uint32_t lastOrdinal_ = 0;
	double glide_ = 0.0;
	bool haveClock_ = false;
	double lastSeconds_ = 0.0;
};

} // namespace feathers::ui
