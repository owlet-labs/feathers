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

#include "plugin/ui/balance_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace bal = fx::balance;

namespace {

/// THE KNOBS IN FLOW ORDER: where the teeth are, how much is taken out, how the measurement follows, where the whole sits.
constexpr std::array<bal::Param, 6> kKnobs { bal::Param::Spacing, bal::Param::Spacing2, bal::Param::Strength,
	                                        bal::Param::Attack,  bal::Param::Release,  bal::Param::Pan };
constexpr std::array<const char*, 6> kCaptions { "Spacing", "Spacing 2", "Strength", "Attack", "Release", "Pan" };

} // namespace

BalanceCard::BalanceCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(bal::Param::kCount));
	bands_ = std::make_unique<SegmentedControl>(control(bal::Param::Bands), registry, bal::kBandsNames, notifier_);
	addChild(bands_.get());
	keyTrack_ = std::make_unique<SwitchChip>(control(bal::Param::KeyTrack), "Key Track");
	addChild(keyTrack_.get());
	view_ = std::make_unique<BalanceView>(host_, device_);
	addChild(view_.get());
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		auto knob = std::make_unique<Knob>(control(kKnobs[i]), kCaptions[i], notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}

	// THE HEAD'S OCCUPANTS — `ClipCard`'s, word for word.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Stereo Balance on" : "Stereo Balance off");
			}
		}
	};
}

DeviceControl& BalanceCard::control(bal::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, bal::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* BalanceCard::enableModulatable() {
	return &control(bal::Param::Enabled);
}

bool BalanceCard::headEnabled() const {
	return control(bal::Param::Enabled).value() >= 0.5;
}

bool BalanceCard::isLive(bal::Param p) const {
	const device::ParamDesc& d = bal::kParamDescs[static_cast<size_t>(p)];
	if (d.gateControl < 0) {
		return true;
	}
	return device::relevantFor(d, host_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

void BalanceCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(bal::Param::kCount); ++i) {
		control(static_cast<bal::Param>(i)).setModulation(mod, history);
	}
}

float BalanceCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kSwitchHeight + kRowGap + kPictureHeight + kRowGap + 2.0f * (kCellHeight + kRowGap) + kPad - kRowGap;
}

float BalanceCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float BalanceCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

void BalanceCard::resized() { layOut(); }
void BalanceCard::foldedChanged() { layOut(); }

void BalanceCard::layOut() {
	const bool open = !folded();
	laidOutBands_ = static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(bal::Param::Bands))));
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float cell = width / static_cast<float>(kKnobsPerRow);

	// ROW 1 — how many bands, then Key Track at the row's end. `setBounds` unconditionally (CLAUDE.md's DPI rule).
	float y = kSlotHeadHeight + 4.0f;
	const float chips = kChipWidth;
	bands_->setBounds(left, y, std::max(0.0f, std::min(width - chips - kRowGap, 150.0f)), kSwitchHeight);
	keyTrack_->setBounds(left + width - chips, y + (kSwitchHeight - SwitchChip::kHeight) * 0.5f, kChipWidth, SwitchChip::kHeight);
	y += kSwitchHeight + kRowGap;

	// THE PICTURE — and the one row that gives way when the card is shorter than its open height (`SmoothCard`'s rule).
	const float below = kRowGap + 2.0f * kCellHeight + kRowGap + kPad;
	const float room = ownFrame().body.bottom() - y - below;
	const float pictureHeight = std::clamp(room, 0.0f, kPictureHeight);
	view_->setBounds(left, y, width, pictureHeight);
	y += pictureHeight + kRowGap;

	// THE KNOBS, CLOSED UP: a hidden one takes no cell.
	size_t at = 0;
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		const bool live = isLive(kKnobs[i]);
		const float row = static_cast<float>(at / kKnobsPerRow);
		const float col = static_cast<float>(at % kKnobsPerRow);
		knobs_[i]->setBounds(left + cell * col, y + row * (kCellHeight + kRowGap), cell, kCellHeight);
		knobs_[i]->setVisible(open && live);
		if (live) {
			++at;
		}
	}
	for (visage::Frame* f : std::initializer_list<visage::Frame*> { bands_.get(), keyTrack_.get(), view_.get() }) {
		f->setVisible(open);
	}
}

bool BalanceCard::updateContent() {
	bool changed = false;
	if (static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(bal::Param::Bands)))) != laidOutBands_) {
		layOut();
		changed = true;
	}
	if (view_->refresh()) {
		view_->redraw();
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

void BalanceCard::drawHeadExtras(visage::Canvas& canvas) {
	drawSlotMenu(canvas, kMuted);
}

void BalanceCard::drawContent(visage::Canvas&) {}

std::vector<bal::Param> BalanceCard::addressedParamsForTest() const {
	return { bal::Param::Bands, bal::Param::KeyTrack, bal::Param::Spacing, bal::Param::Spacing2,
		     bal::Param::Strength, bal::Param::Attack, bal::Param::Release, bal::Param::Pan, bal::Param::Enabled };
}

visage::Frame* BalanceCard::widgetForTest(bal::Param p) const {
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		if (kKnobs[i] == p) {
			return knobs_[i].get();
		}
	}
	switch (p) {
	case bal::Param::Bands: return bands_.get();
	case bal::Param::KeyTrack: return keyTrack_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
