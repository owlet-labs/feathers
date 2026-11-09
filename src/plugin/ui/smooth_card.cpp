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

#include "plugin/ui/smooth_card.h"

#include "plugin/ui/generic_card.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace sm = fx::smooth;

SmoothCard::SmoothCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                       Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry; // unused since `Knob` stopped taking one — see generic_card.cpp
	controls_.resize(static_cast<size_t>(sm::Param::kCount));

	// THE HEAD ROW: which shaper (every mode on view in the menu before one is applied), and where it sits.
	mode_ = std::make_unique<ValueBox>(control(sm::Param::HarmonicMode), "Mode");
	addChild(mode_.get());
	poly_ = std::make_unique<SwitchChip>(control(sm::Param::Poly), "Poly");
	addChild(poly_.get());

	// THE PICTURE, WITH DRIVE ON IT — and Drive again as a knob under it, where the mod ring and a route drag
	// live in the form every other surface uses. The two are one control.
	curve_ = std::make_unique<SmoothCurveView>(model_, control(sm::Param::Drive));
	addChild(curve_.get());
	drive_ = std::make_unique<Knob>(control(sm::Param::Drive), "Drive", notifier_);
	drive_->setFaceDiameter(kKnobDiameter);
	addChild(drive_.get());

	harmonic_ = makeSmoothCoordCell(control(sm::Param::Harmonic), sm::Param::Harmonic, "Harmonic", kKnobDiameter);
	addChild(harmonic_.get());

	// TWIST, THE SWEEP ALONE, AND ITS POWER; then the four pills — `DisperserCard`'s, word for word.
	twist_ = makeSmoothCoordCell(control(sm::Param::Sweep), sm::Param::Sweep, "Twist", kKnobDiameter);
	addChild(twist_.get());
	sweepOn_ = std::make_unique<SwitchChip>(control(sm::Param::SweepOn), "On");
	addChild(sweepOn_.get());
	for (size_t i = 0; i < sm::kPillCount; ++i) {
		const auto pill = static_cast<sm::Pill>(i);
		auto widget = std::make_unique<FaderPill>(control(sm::pillAmountParam(pill)), control(sm::pillOnParam(pill)),
		                                          sm::kPillNames[i]);
		widget->setAnnouncer([this](const std::string& line) {
			if (notifier_ != nullptr) {
				notifier_->say(line);
			}
		});
		addChild(widget.get());
		pills_.push_back(std::move(widget));
	}

	mix_ = std::make_unique<Knob>(control(sm::Param::Mix), "Mix", notifier_);
	mix_->setFaceDiameter(kKnobDiameter);
	addChild(mix_.get());

	// THE HEAD'S OCCUPANTS — `ClipCard`'s, word for word.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Smooth Shaper on" : "Smooth Shaper off");
			}
		}
	};
	(void)model_.refresh(settings());
}

DeviceControl& SmoothCard::control(sm::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, sm::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* SmoothCard::enableModulatable() {
	return &control(sm::Param::Enabled);
}

bool SmoothCard::headEnabled() const {
	return control(sm::Param::Enabled).value() >= 0.5;
}

sm::Settings SmoothCard::settings() const {
	sm::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return sm::stateOf(row, row.size()).settings;
}

void SmoothCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	drive_->setModulation(mod, history);
	mix_->setModulation(mod, history);
	for (size_t i = 0; i < static_cast<size_t>(sm::Param::kCount); ++i) {
		control(static_cast<sm::Param>(i)).setModulation(mod, history);
	}
}

float SmoothCard::openHeight() {
	return kSlotHeadHeight + 4.0f + ValueBox::kHeight + kRowGap + kCurveHeight + kRowGap + kCellHeight + kRowGap
	       + kCellHeight + kPad;
}

float SmoothCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float SmoothCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

void SmoothCard::resized() {
	layOut();
}

void SmoothCard::foldedChanged() {
	layOut();
}

void SmoothCard::layOut() {
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);

	// ROW 1 — the mode box, and Poly at the row's end, centred on the box. `setBounds` unconditionally
	// (CLAUDE.md's DPI rule): a scale change moves nothing logical and must still re-set the native size.
	float y = kSlotHeadHeight + 4.0f;
	mode_->setBounds(left, y, std::max(0.0f, width - kChipWidth - kRowGap), ValueBox::kHeight);
	poly_->setBounds(left + width - kChipWidth, y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f, kChipWidth,
	                 SwitchChip::kHeight);
	y += ValueBox::kHeight + kRowGap;

	// THE CURVE, THE WHOLE WIDTH — AND THE ONE ROW THAT GIVES WAY when the card is shorter than its open
	// height (backlog KQ). The rack's lane grants `kCardUnitHeight` (336) rather than asking, and the rows
	// below the curve need their full height to stay readable; at a fixed 150 the Twist row ended 28px
	// below the card and was clipped off. In the fx lane the card IS its open height, so this is 150 there.
	const float below = kRowGap + kCellHeight + kRowGap + kCellHeight + kPad;
	const float room = ownFrame().body.bottom() - y - below;
	const float curveHeight = std::clamp(room, 0.0f, kCurveHeight);
	curve_->setBounds(left, y, width, curveHeight);
	y += curveHeight + kRowGap;

	// DRIVE, HARMONIC AND MIX, spread across the row: the ends flush with the curve's, Harmonic centred.
	const float middle = left + std::max(0.0f, width - kKnobCell) * 0.5f;
	drive_->setBounds(left, y, kKnobCell, kCellHeight);
	harmonic_->setBounds(middle, y, kKnobCell, kCellHeight);
	mix_->setBounds(left + std::max(0.0f, width - kKnobCell), y, kKnobCell, kCellHeight);
	y += kCellHeight + kRowGap;

	// TWIST UNDER DRIVE, ITS POWER AND THE FOUR PILLS BESIDE IT: the power on top, the pills two by two under it.
	twist_->setBounds(left, y, kKnobCell, kCellHeight);
	const float right = left + kKnobCell + kRowGap;
	const float pillRoom = std::max(0.0f, left + width - right);
	const float pill = std::min(kPillWidth, std::max(0.0f, (pillRoom - kPillGap) * 0.5f));
	const float block = SwitchChip::kHeight + 2.0f * (kPillGap + kPillHeight);
	const float top = y + std::max(0.0f, (kCellHeight - block) * 0.5f);
	sweepOn_->setBounds(right, top, kChipWidth, SwitchChip::kHeight);
	for (size_t i = 0; i < pills_.size(); ++i) {
		const auto row = static_cast<float>(i / 2);
		const auto column = static_cast<float>(i % 2);
		pills_[i]->setBounds(right + column * (pill + kPillGap),
		                     top + SwitchChip::kHeight + kPillGap + row * (kPillHeight + kPillGap), pill, kPillHeight);
		pills_[i]->setVisible(open);
	}

	for (visage::Frame* f : std::initializer_list<visage::Frame*> { mode_.get(), poly_.get(), curve_.get(),
	                                                                 drive_.get(), harmonic_.get(), twist_.get(),
	                                                                 sweepOn_.get(), mix_.get() }) {
		f->setVisible(open);
	}
}

bool SmoothCard::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(sm::Reading::kCount);
}

float SmoothCard::inputPeak() const {
	return hasReadings() ? host_.readingAt(device_, static_cast<size_t>(sm::Reading::InPeak)) : 0.0f;
}

bool SmoothCard::updateContent() {
	bool changed = false;
	if (model_.refresh(settings())) {
		changed = true;
	}
	// THE MARKER MOVES ONLY WHEN THE READING DOES; published-to-unpublished is a change too (`ClipCard`).
	const bool has = hasReadings();
	const float peak = inputPeak();
	if (has != drawnHadPeak_ || (has && peak != drawnPeak_)) {
		drawnHadPeak_ = has;
		drawnPeak_ = peak;
		curve_->setInputPeak(has, peak);
		changed = true;
	}
	// THE DELIVERY, NOT ONLY THE REPORT — `CompCard::updateContent`.
	if (changed) {
		curve_->redraw();
		redraw();
	}
	return changed;
}

void SmoothCard::drawHeadExtras(visage::Canvas& canvas) {
	// THE SLOT MENU, outside the body's clamp — `ClipCard::drawHeadExtras`.
	drawSlotMenu(canvas, kMuted);
}

void SmoothCard::drawContent(visage::Canvas&) {
}

std::vector<sm::Param> SmoothCard::addressedParamsForTest() const {
	// EVERY CONTROL A HAND REACHES HERE — the widgets and the enable in the head. Drive is a knob now as well as
	// the ceiling line's gesture.
	return { sm::Param::HarmonicMode, sm::Param::Poly,    sm::Param::Harmonic, sm::Param::Sweep,  sm::Param::SweepOn,
		     sm::Param::Width,        sm::Param::Evens,   sm::Param::Rect,     sm::Param::Fdbk,   sm::Param::WidthOn,
		     sm::Param::EvensOn,      sm::Param::RectOn,  sm::Param::FdbkOn,   sm::Param::Mix,    sm::Param::Drive,
		     sm::Param::Enabled };
}

visage::Frame* SmoothCard::widgetForTest(sm::Param p) const {
	switch (p) {
	case sm::Param::HarmonicMode: return mode_.get();
	case sm::Param::Poly: return poly_.get();
	case sm::Param::Harmonic: return harmonic_.get();
	case sm::Param::Sweep: return twist_.get();
	case sm::Param::SweepOn: return sweepOn_.get();
	case sm::Param::Width:
	case sm::Param::WidthOn: return pills_[0].get();
	case sm::Param::Evens:
	case sm::Param::EvensOn: return pills_[1].get();
	case sm::Param::Rect:
	case sm::Param::RectOn: return pills_[2].get();
	case sm::Param::Fdbk:
	case sm::Param::FdbkOn: return pills_[3].get();
	case sm::Param::Mix: return mix_.get();
	case sm::Param::Drive: return drive_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
