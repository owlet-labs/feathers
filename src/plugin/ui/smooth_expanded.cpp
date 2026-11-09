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

#include "plugin/ui/smooth_expanded.h"

#include "plugin/ui/generic_card.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace sm = fx::smooth;

SmoothExpanded::SmoothExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry;
	controls_.resize(static_cast<size_t>(sm::Param::kCount));

	mode_ = std::make_unique<ValueBox>(control(sm::Param::HarmonicMode), "Mode");
	poly_ = std::make_unique<SwitchChip>(control(sm::Param::Poly), "Poly");
	// NO "On" CHIP BESIDE POLY ANY MORE: the device's enable is the window header's chip (2026-09-26).
	curve_ = std::make_unique<SmoothCurveView>(model_, control(sm::Param::Drive));
	curve_->setAnnotated(true);
	bars_ = std::make_unique<SmoothHarmonicsView>(model_);
	const auto knob = [&](sm::Param p, const char* caption) {
		auto k = std::make_unique<Knob>(control(p), caption, notifier_);
		k->setFaceDiameter(kKnobDiameter);
		return k;
	};
	drive_ = knob(sm::Param::Drive, "Drive");
	mix_ = knob(sm::Param::Mix, "Mix");
	// HARMONIC AND TWIST ARE THE CARD'S CONTROL — `makeSmoothCoordCell`, no dropdowns.
	harmonic_ = makeSmoothCoordCell(control(sm::Param::Harmonic), sm::Param::Harmonic, "Harmonic", kKnobDiameter);
	// THE TWO PHASES ARE DRAG WINDOWS, SO BOXES, NEVER KNOBS (`device::isDragWindow`; `gamma_box.h`).
	harmonicPhase_ = std::make_unique<GammaBox>(control(sm::Param::HarmonicPhase), "H Phase");
	gamma_ = std::make_unique<GammaBox>(control(sm::Param::Gamma), "Gamma");
	twist_ = makeSmoothCoordCell(control(sm::Param::Sweep), sm::Param::Sweep, "Twist", kKnobDiameter);
	sweepOn_ = std::make_unique<SwitchChip>(control(sm::Param::SweepOn), "Twist On");
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

	for (visage::Frame* f : std::initializer_list<visage::Frame*> {
	         mode_.get(), poly_.get(), curve_.get(), bars_.get(), drive_.get(), mix_.get(),
	         harmonic_.get(), harmonicPhase_.get(), gamma_.get(), twist_.get() }) {
		addChild(f);
	}
	(void)model_.refresh(settings());
}

DeviceControl& SmoothExpanded::control(sm::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, sm::kDescriptor, i, nullptr, nullptr);
	}
	return *controls_[i];
}

sm::Settings SmoothExpanded::settings() const {
	sm::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return sm::stateOf(row, row.size()).settings;
}

bool SmoothExpanded::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(sm::Reading::kCount);
}

void SmoothExpanded::modulationBound(ModState* mod, EditHistory* history) {
	for (Knob* k : { drive_.get(), mix_.get() }) {
		k->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(sm::Param::kCount); ++i) {
		control(static_cast<sm::Param>(i)).setModulation(mod, history);
	}
}

visage::Point SmoothExpanded::minimumSize() const {
	const float w = kPad + kCurveWidth + kRowGap * 2.0f + kSideWidth + kPad;
	const float h = kPad + ValueBox::kHeight + kRowGap + kCaptionHeight + kCurveHeight + kRowGap + kCellHeight + kRowGap + kPillHeight
	                + kPad;
	return { w, h };
}

visage::Point SmoothExpanded::maximumSize() const {
	const visage::Point natural = minimumSize();
	return { natural.x + kPad, natural.y + kPad };
}

std::string SmoothExpanded::title() const {
	return sm::kTraits.name;
}

void SmoothExpanded::resized() {
	layOut();
}

void SmoothExpanded::layOut() {
	// `setBounds` UNCONDITIONALLY — a scale change moves nothing logical (CLAUDE.md's DPI rule).
	float y = kPad;
	const float left = kPad;
	mode_->setBounds(left, y, 180.0f, ValueBox::kHeight);
	const float chipY = y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f;
	poly_->setBounds(left + 180.0f + kRowGap, chipY, 44.0f, SwitchChip::kHeight);
	y += ValueBox::kHeight + kRowGap + kCaptionHeight;

	// THE TRANSFER, LARGE, AND ITS TWO GLOBALS UNDER IT.
	curve_->setBounds(left, y, kCurveWidth, kCurveHeight);
	const float under = y + kCurveHeight + kRowGap;
	const float cell = kCurveWidth / 4.0f;
	drive_->setBounds(left, under, cell, kCellHeight);
	mix_->setBounds(left + cell, under, cell, kCellHeight);
	twist_->setBounds(left + 2.0f * cell, under, cell, kCellHeight);
	gamma_->setBounds(left + 3.0f * cell + 6.0f, under + (kKnobDiameter - ValueBox::kHeight) * 0.5f, cell - 12.0f,
	                  ValueBox::kHeight);
	// TWIST'S POWER AND THE FOUR PILLS, ONE ROW UNDER THEM — the card's, laid flat.
	const float pillRow = under + kCellHeight + kRowGap;
	sweepOn_->setBounds(left, pillRow + (kPillHeight - SwitchChip::kHeight) * 0.5f, kSweepChipWidth, SwitchChip::kHeight);
	const float pillLeft = left + kSweepChipWidth + kRowGap;
	const float pill = (kCurveWidth - kSweepChipWidth - kRowGap - 3.0f * kRowGap) / 4.0f;
	for (size_t i = 0; i < pills_.size(); ++i) {
		pills_[i]->setBounds(pillLeft + static_cast<float>(i) * (pill + kRowGap), pillRow, pill, kPillHeight);
	}

	// THE HARMONICS AS TALL AS THE CURVE, AND WHAT MOVES THEM ON THE SAME ROW AS DRIVE AND MIX — one bottom row
	// across the panel, so neither column ends in air.
	const float side = left + kCurveWidth + kRowGap * 2.0f;
	bars_->setBounds(side, y, kSideWidth, kCurveHeight);
	const float row = under;
	const float half = kSideWidth / 2.0f;
	harmonic_->setBounds(side, row, half, kCellHeight);
	harmonicPhase_->setBounds(side + half + 4.0f, row + (kKnobDiameter - ValueBox::kHeight) * 0.5f, half - 8.0f,
	                          ValueBox::kHeight);
}

bool SmoothExpanded::updateContent() {
	if (!isVisible()) {
		return false;
	}
	bool changed = model_.refresh(settings());
	const bool has = hasReadings();
	const float peak = has ? host_.readingAt(device_, static_cast<size_t>(sm::Reading::InPeak)) : 0.0f;
	if (has != drawnHadPeak_ || (has && peak != drawnPeak_)) {
		drawnHadPeak_ = has;
		drawnPeak_ = peak;
		curve_->setInputPeak(has, peak);
		changed = true;
	}
	if (changed) {
		curve_->redraw();
		bars_->redraw();
		redraw();
	}
	return changed;
}

void SmoothExpanded::draw(visage::Canvas& canvas) {
	// THE TWO SECTION HEADINGS, over the pictures they name.
	const float y = kPad + ValueBox::kHeight + kRowGap;
	canvas.setColor(kMuted);
	canvas.text("TRANSFER", font(kKnobLabelSize), visage::Font::kLeft, kPad, y, kCurveWidth, kCaptionHeight);
	canvas.text("HARMONICS OF A FULL VOICE", font(kKnobLabelSize), visage::Font::kLeft,
	            kPad + kCurveWidth + kRowGap * 2.0f, y, kSideWidth, kCaptionHeight);

	// THE GAIN STAGING IN ONE LINE, at the head row's end: where this mode's input saturates, in dB of a full
	// voice — the number the dashed lines draw. In the FM zones nothing saturates, and it says that instead.
	const fx::smooth::Picture& pic = model_.picture;
	char line[96];
	if (!(pic.driveGain > 0.0f)) {
		std::snprintf(line, sizeof line, "DRIVE AT -inf: NOTHING REACHES THE SHAPER");
	}
	else if (pic.clips) {
		std::snprintf(line, sizeof line, "INPUT SATURATES AT %+.1f dB OF A FULL VOICE",
		              static_cast<double>(20.0f * std::log10(std::max(pic.ceilingInput, 1.0e-6f))));
	}
	else {
		std::snprintf(line, sizeof line, "NO CEILING: THE DRIVEN INPUT IS A PHASE (DRIVE %.2fx)",
		              static_cast<double>(pic.driveGain));
	}
	const float right = kPad + kCurveWidth + kRowGap * 2.0f + kSideWidth;
	canvas.setColor(kValue);
	canvas.text(line, font(kKnobLabelSize), visage::Font::kRight, kPad + 300.0f, kPad, right - kPad - 300.0f,
	            ValueBox::kHeight);
}

std::vector<sm::Param> SmoothExpanded::addressedParamsForTest() const {
	return { sm::Param::HarmonicMode, sm::Param::Poly,          sm::Param::Drive,
		     sm::Param::Mix,          sm::Param::Harmonic,      sm::Param::HarmonicPhase, sm::Param::Gamma,
		     sm::Param::Sweep,        sm::Param::SweepOn,       sm::Param::Width,   sm::Param::Evens,
		     sm::Param::Rect,         sm::Param::Fdbk,          sm::Param::WidthOn, sm::Param::EvensOn,
		     sm::Param::RectOn,       sm::Param::FdbkOn };
}

} // namespace feathers::ui
