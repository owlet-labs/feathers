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

#include "plugin/ui/balance_expanded.h"

#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace bal = fx::balance;

namespace {

constexpr std::array<bal::Param, 9> kKnobs { bal::Param::Spacing, bal::Param::Spacing2, bal::Param::Strength, bal::Param::Focus, bal::Param::Tolerance,
	                                        bal::Param::Attack,  bal::Param::Release,  bal::Param::Widen,    bal::Param::Pan };
constexpr std::array<const char*, 9> kCaptions { "Spacing", "Spacing 2", "Strength", "Focus", "Tolerance", "Attack", "Release", "Widen", "Pan" };

} // namespace

BalanceExpanded::BalanceExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(bal::Param::kCount));
	bands_ = std::make_unique<SegmentedControl>(control(bal::Param::Bands), registry, bal::kBandsNames, notifier_);
	keyTrack_ = std::make_unique<SwitchChip>(control(bal::Param::KeyTrack), "Key Track");
	sharp_ = std::make_unique<SwitchChip>(control(bal::Param::Sharp), "Sharp");
	view_ = std::make_unique<BalanceView>(host_, device_);
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		auto knob = std::make_unique<Knob>(control(kKnobs[i]), kCaptions[i], notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}
	// HP WET AND SUB THRU: the shared element. Hertz only: the highpass is not placed by the note.
	hp_ = std::make_unique<WetHighpassControl>(control(bal::Param::HpWet), nullptr, control(bal::Param::SubThru), notifier_, kKnobDiameter);
	hp_->addTo(*this);
	for (visage::Frame* f : std::initializer_list<visage::Frame*> { bands_.get(), keyTrack_.get(), sharp_.get(), view_.get() }) {
		addChild(f);
	}
}

DeviceControl& BalanceExpanded::control(bal::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, bal::kDescriptor, i, nullptr, nullptr);
	}
	return *controls_[i];
}

bool BalanceExpanded::isLive(bal::Param p) const {
	const device::ParamDesc& d = bal::kParamDescs[static_cast<size_t>(p)];
	if (d.gateControl < 0) {
		return true;
	}
	return device::relevantFor(d, host_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

void BalanceExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	hp_->setModulation(mod, history);
	for (size_t i = 0; i < static_cast<size_t>(bal::Param::kCount); ++i) {
		control(static_cast<bal::Param>(i)).setModulation(mod, history);
	}
}

visage::Point BalanceExpanded::minimumSize() const {
	return { kPad * 2.0f + kPictureWidth, kPad + kSwitchHeight + kRowGap + kPictureHeight + kRowGap + kCellHeight + kPad };
}

visage::Point BalanceExpanded::maximumSize() const {
	const visage::Point natural = minimumSize();
	return { natural.x + kPad, natural.y + kPad };
}

std::string BalanceExpanded::title() const {
	return bal::kTraits.name;
}

void BalanceExpanded::resized() { layOut(); }

void BalanceExpanded::layOut() {
	laidOutBands_ = static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(bal::Param::Bands))));
	const float left = kPad;
	float y = kPad;
	bands_->setBounds(left, y, 150.0f, kSwitchHeight);
	const float chipY = y + (kSwitchHeight - SwitchChip::kHeight) * 0.5f;
	keyTrack_->setBounds(left + 150.0f + kRowGap, chipY, kChipWidth, SwitchChip::kHeight);
	sharp_->setBounds(left + 150.0f + kRowGap * 2.0f + kChipWidth, chipY, kChipWidth, SwitchChip::kHeight);
	hp_->subThru()->setBounds(left + kPictureWidth - kChipWidth - 8.0f, chipY, kChipWidth + 8.0f, SwitchChip::kHeight);
	y += kSwitchHeight + kRowGap;

	view_->setBounds(left, y, kPictureWidth, kPictureHeight);
	y += kPictureHeight + kRowGap;

	// SEVEN CELLS, CLOSED UP: Spacing 2 is drawn only in three bands, and HP Wet takes the last live cell.
	const float cell = kPictureWidth / static_cast<float>(kCells);
	size_t at = 0;
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		const bool live = isLive(kKnobs[i]);
		knobs_[i]->setBounds(left + cell * static_cast<float>(at), y, cell, kCellHeight);
		knobs_[i]->setVisible(live);
		if (live) {
			++at;
		}
	}
	hp_->hertz()->setBounds(left + cell * static_cast<float>(at), y, cell, kCellHeight);
	hp_->showFaces(true, false);
	const bal::Settings now = bal::stateOf([&] { bal::Row r {}; readStoredRow(host_, device_, r); return r; }(), static_cast<size_t>(bal::Param::kCount));
	hp_->refresh(now.hpWet > static_cast<float>(fx::kWetHighpassLog2Min), true);
}

bool BalanceExpanded::updateContent() {
	if (static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(bal::Param::Bands)))) != laidOutBands_) {
		layOut();
	}
	if (!isVisible()) {
		return false;
	}
	bal::Row r {};
	const size_t present = readStoredRow(host_, device_, r);
	// SUB THRU GIVES BACK WHAT HP WET TOOK, so it is nothing with no filter.
	hp_->refresh(bal::stateOf(r, present).hpWet > static_cast<float>(fx::kWetHighpassLog2Min), true);
	const bool changed = view_->refresh();
	if (changed) {
		view_->redraw();
		redraw();
	}
	return changed;
}

void BalanceExpanded::draw(visage::Canvas&) {}

std::vector<bal::Param> BalanceExpanded::addressedParamsForTest() const {
	return { bal::Param::Bands,    bal::Param::KeyTrack, bal::Param::Sharp,   bal::Param::Spacing, bal::Param::Spacing2,
		     bal::Param::Strength, bal::Param::Focus, bal::Param::Tolerance, bal::Param::Attack, bal::Param::Release, bal::Param::Widen, bal::Param::Pan, bal::Param::HpWet,
		     bal::Param::SubThru };
}

visage::Frame* BalanceExpanded::widgetForTest(bal::Param p) const {
	for (size_t i = 0; i < kKnobs.size(); ++i) {
		if (kKnobs[i] == p) {
			return knobs_[i].get();
		}
	}
	switch (p) {
	case bal::Param::Bands: return bands_.get();
	case bal::Param::KeyTrack: return keyTrack_.get();
	case bal::Param::Sharp: return sharp_.get();
	case bal::Param::HpWet: return hp_->hertz();
	case bal::Param::SubThru: return hp_->subThru();
	default: return nullptr;
	}
}

} // namespace feathers::ui
