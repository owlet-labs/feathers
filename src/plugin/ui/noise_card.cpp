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

#include "plugin/ui/noise_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace nz = fx::noise;
using P = nz::Param;

namespace {

/// THE CONTROLS THAT TAKE A VALUE BOX, with their captions.
struct BoxSpec {
	P param;
	const char* caption;
};
constexpr std::array<BoxSpec, 11> kBoxes { {
	{ P::Level, "Level" },
	{ P::Stereo, "Stereo" },
	{ P::Colour, "Colour" },
	{ P::Density, "Density" },
	{ P::KeyTrack, "Key Track" },
	{ P::Seed, "Seed" },
	{ P::Sync, "Sync" },
	{ P::Jitter, "Jitter" },
	{ P::Cluster, "Cluster" },
	{ P::HitSize, "Hit Size" },
	{ P::Decay, "Decay" },
} };

/// THE THREE IMPULSE ROWS' HEIGHT — the room the plot takes under Noise.
constexpr float impulseRows() {
	return 3.0f * (ValueBox::kHeight + NoiseCard::kRowGap);
}

} // namespace

NoiseCard::NoiseCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(P::kCount));
	boxes_.resize(static_cast<size_t>(P::kCount));

	plot_ = std::make_unique<NoisePlot>(host_, device_);
	addChild(plot_.get());

	source_ = std::make_unique<SegmentedControl>(control(P::Source), registry, nz::kSourceNames, notifier_);
	filterType_ = std::make_unique<SegmentedControl>(control(P::FilterType), registry, nz::kFilterTypeNames, notifier_);
	timing_ = std::make_unique<SegmentedControl>(control(P::Timing), registry, nz::kTimingNames, notifier_);
	seedMode_ = std::make_unique<SegmentedControl>(control(P::SeedMode), registry, nz::kSeedModeNames, notifier_);
	for (SegmentedControl* s : { source_.get(), filterType_.get(), timing_.get(), seedMode_.get() }) {
		addChild(s);
	}
	// TWO STATES EACH, SO CHIPS.
	poly_ = std::make_unique<SwitchChip>(control(P::Poly), "Poly");
	addChild(poly_.get());
	polarity_ = std::make_unique<SwitchChip>(control(P::Polarity), "±");
	addChild(polarity_.get());
	trigPad_ = std::make_unique<TrigPad>(control(P::Trig), [this](uint32_t& fires) { return host_.trigFiresAt(device_, fires); });
	addChild(trigPad_.get());

	for (const BoxSpec& b : kBoxes) {
		auto box = std::make_unique<ValueBox>(control(b.param), b.caption);
		addChild(box.get());
		boxes_[static_cast<size_t>(b.param)] = std::move(box);
	}

	// THE HEAD'S OCCUPANTS — `ClipCard`'s, word for word.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Noise on" : "Noise off");
			}
		}
	};
}

DeviceControl& NoiseCard::control(P p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, nz::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* NoiseCard::enableModulatable() {
	return &control(P::Enabled);
}

bool NoiseCard::headEnabled() const {
	return control(P::Enabled).value() >= 0.5;
}

void NoiseCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	plot_->setModulation(mod, history);
	for (size_t i = 0; i < static_cast<size_t>(P::kCount); ++i) {
		control(static_cast<P>(i)).setModulation(mod, history);
	}
}

float NoiseCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kSwitchHeight + kRowGap + kPlotHeight + kRowGap + 3.0f * (ValueBox::kHeight + kRowGap)
	       + impulseRows() + kPad - kRowGap;
}

float NoiseCard::laneHeightAt(float cardWidth) const {
	// **AND THE OUTPUTS STRIP UNDER IT.** The body's rows were all of it, so the lane gave the card no room for the strip
	// its three output chips hang on, `cardFrameOf` clamped the strip inside the body instead, and the Seed row was drawn
	// under the chips. `SampleHoldCard::laneHeightAt` is the shape: the body, then the strip at this width.
	return folded() ? foldedHeight() : openHeight() + outputStripHeightAt(cardWidth);
}

float NoiseCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

void NoiseCard::resized() {
	layOut();
}

void NoiseCard::foldedChanged() {
	layOut();
}

bool NoiseCard::isLive(P p) const {
	return nz::liveIn(p, [this](size_t i) { return static_cast<double>(host_.fieldAt(device_, i)); });
}

uint32_t NoiseCard::layoutKey() const {
	uint32_t key = 0;
	for (const int16_t gate : { nz::kSourceControl, nz::kSyncControl, nz::kTimingControl, nz::kSeedModeControl }) {
		const auto v = static_cast<uint32_t>(std::lround(host_.fieldAt(device_, static_cast<size_t>(gate))));
		key = key * 32u + (v & 31u);
	}
	return key;
}

void NoiseCard::layOut() {
	laidOutKey_ = layoutKey();
	const bool open = !folded();
	const bool impulses = isLive(P::Timing);
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float cell = width / static_cast<float>(kCells);
	const float boxH = ValueBox::kHeight;
	const float switchInset = 0.5f * (boxH - kSwitchHeight);

	const auto box = [&](P p, float x, float y, float w) {
		ValueBox& b = *boxes_[static_cast<size_t>(p)];
		b.setBounds(x, y, w, boxH);
		b.setVisible(open && isLive(p));
	};
	const auto hideBox = [&](P p) { boxes_[static_cast<size_t>(p)]->setVisible(false); };

	// ROW 1 — the source, both on the face, and Poly at the row's right end. `setBounds` unconditionally
	// (CLAUDE.md's DPI rule).
	float y = kSlotHeadHeight + 4.0f;
	source_->setBounds(left, y, std::max(0.0f, width - kChipWidth - kRowGap), kSwitchHeight);
	source_->setVisible(open);
	poly_->setBounds(left + width - kChipWidth, y + 0.5f * (kSwitchHeight - SwitchChip::kHeight), kChipWidth,
	                 SwitchChip::kHeight);
	poly_->setVisible(open);
	y += kSwitchHeight + kRowGap;

	// THE PLOT — taller under Noise, by the impulse rows it does not need.
	const float plotH = kPlotHeight + (impulses ? 0.0f : impulseRows());
	const float plotY = y;
	plot_->setBounds(left, plotY, width, plotH);
	plot_->setVisible(open);
	y += plotH + kRowGap;

	// THE FILTER'S RESPONSE AND KEY TRACK.
	filterType_->setBounds(left, y + switchInset, 2.0f * cell - kRowGap, kSwitchHeight);
	filterType_->setVisible(open);
	box(P::KeyTrack, left + 2.0f * cell, y, cell);
	y += boxH + kRowGap;

	// LEVEL, STEREO, AND THE SOURCE'S OWN AMOUNT in the third cell: Colour, or Density while Sync is Free.
	box(P::Level, left, y, cell - kRowGap);
	box(P::Stereo, left + cell, y, cell - kRowGap);
	box(P::Colour, left + 2.0f * cell, y, cell);
	box(P::Density, left + 2.0f * cell, y, cell);
	y += boxH + kRowGap;

	// THE IMPULSE ROWS: when the hits fall (all three on the face, the full width — "Clustered" does not fit two
	// cells' worth of a third), then the rate's sync, the timing's own amount, and the hits' size. POLARITY sits ON
	// THE PLOT, at the top right, above the strip whose sticks it turns up and down.
	if (impulses) {
		timing_->setBounds(left, y + switchInset, width, kSwitchHeight);
		timing_->setVisible(open);
		y += boxH + kRowGap;
		box(P::Sync, left, y, cell - kRowGap);
		box(P::Jitter, left + cell, y, cell - kRowGap);
		box(P::Cluster, left + cell, y, cell - kRowGap);
		box(P::HitSize, left + 2.0f * cell, y, cell);
		polarity_->setBounds(left + width - kChipWidth - 3.0f, plotY + 3.0f, kChipWidth, SwitchChip::kHeight);
		polarity_->setVisible(open);
		trigPad_->setBounds(left + 3.0f, plotY + 3.0f, TrigPad::kDiameter, TrigPad::kDiameter);
		trigPad_->setVisible(open);
		y += boxH + kRowGap;
		// THE IMPULSE DECAY OUTPUT'S TIME, under Hit Size: it shapes what each hit's value does after the hit.
		box(P::Decay, left + 2.0f * cell, y, cell);
		y += boxH + kRowGap;
	}
	else {
		timing_->setVisible(false);
		polarity_->setVisible(false);
		trigPad_->setVisible(false);
		for (const P p : { P::Jitter, P::Cluster, P::Sync, P::HitSize, P::Decay }) {
			hideBox(p);
		}
	}

	// THE SEED.
	seedMode_->setBounds(left, y + switchInset, 2.0f * cell - kRowGap, kSwitchHeight);
	seedMode_->setVisible(open);
	box(P::Seed, left + 2.0f * cell, y, cell);
	plot_->update();
}

bool NoiseCard::updateContent() {
	bool changed = trigPad_->poll();
	if (layoutKey() != laidOutKey_) {
		layOut();
		changed = true;
	}
	changed = plot_->update() || changed;
	if (changed) {
		redraw();
	}
	return changed;
}

void NoiseCard::drawHeadExtras(visage::Canvas& canvas) {
	// THE SLOT MENU, outside the body's clamp — `ClipCard::drawHeadExtras`.
	drawSlotMenu(canvas, kMuted);
}

void NoiseCard::drawContent(visage::Canvas&) {}

std::vector<P> NoiseCard::addressedParamsForTest() const {
	// EVERY CONTROL THE SURFACE REACHES, HIDDEN OR NOT — the Source and the other gates reveal the rest.
	std::vector<P> out;
	for (const BoxSpec& b : kBoxes) {
		out.push_back(b.param);
	}
	for (const P p : { P::Source, P::FilterType, P::Timing, P::SeedMode, P::Poly, P::Polarity, P::Cutoff,
	                   P::Resonance, P::Enabled, P::Trig }) {
		out.push_back(p);
	}
	return out;
}

visage::Frame* NoiseCard::widgetForTest(P p) const {
	if (boxes_[static_cast<size_t>(p)] != nullptr) {
		return boxes_[static_cast<size_t>(p)].get();
	}
	switch (p) {
	case P::Source: return source_.get();
	case P::FilterType: return filterType_.get();
	case P::Timing: return timing_.get();
	case P::SeedMode: return seedMode_.get();
	case P::Poly: return poly_.get();
	case P::Polarity: return polarity_.get();
	case P::Trig: return trigPad_.get();
	case P::Cutoff:
	case P::Resonance: return plot_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
