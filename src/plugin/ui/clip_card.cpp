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

#include "plugin/ui/clip_card.h"

#include "fx/clip/clip_tips.h"
#include "fx/eroder/drive_tips.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace cl = fx::clip;

namespace {

/// ROW 2 — THE GLOBALS, live in every mode.
constexpr std::array<cl::Param, 3> kGlobals { cl::Param::Drive, cl::Param::Ceiling, cl::Param::Output };
constexpr std::array<const char*, 3> kGlobalCaptions { "Drive", "Ceiling", "Output" };

/// ROW 3 — THE MODE KNOBS, IN FLOW ORDER. Each mode's live subset reads left to right with the hidden
/// ones skipped; Bits' segmented control follows them (`layOut`).
constexpr std::array<cl::Param, 11> kModeKnobs {
	cl::Param::Knee, cl::Param::Freq, cl::Param::Foldback, cl::Param::Grit, cl::Param::Depth, cl::Param::Shape, cl::Param::Angle, cl::Param::Bal, cl::Param::Rate, cl::Param::Hard, cl::Param::Bias,
};
constexpr std::array<const char*, 11> kModeCaptions { "Knee", "Freq", "Foldback", "Grit", "Depth", "Shape", "Angle", "Bal", "Rate", "Hard", "Bias" };

/// HOW MANY CELLS BITS' SEGMENTED CONTROL TAKES — "1 bit | 2 bit" does not fit one knob's width.
constexpr size_t kBitsCells = 2;

} // namespace

ClipCard::ClipCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                   Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(cl::Param::kCount));
	const auto addKnob = [&](cl::Param p, const char* caption) {
		auto knob = std::make_unique<Knob>(control(p), caption, notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	};
	for (size_t i = 0; i < kGlobals.size(); ++i) {
		addKnob(kGlobals[i], kGlobalCaptions[i]);
	}
	for (size_t i = 0; i < kModeKnobs.size(); ++i) {
		addKnob(kModeKnobs[i], kModeCaptions[i]);
	}
	// ELEVEN NAMED MODES: a box whose menu shows them all before one is applied (too many for a segmented control).
	mode_ = std::make_unique<ValueBox>(control(cl::Param::Mode), "Mode");
	addChild(mode_.get());
	// TWO STATES, SO A CHIP. Its names are Off/4x rather than Off/On, so it prints the live one.
	oversample_ = std::make_unique<SwitchChip>(control(cl::Param::Oversample), "4x");
	addChild(oversample_.get());
	// THE DC BLOCK, a switch beside it, shown only in the modes that make an offset (Tube, Lean, Paunch-S, Paunch-H).
	dcBlock_ = std::make_unique<SwitchChip>(control(cl::Param::DcBlock), "DC");
	addChild(dcBlock_.get());
	bits_ = std::make_unique<SegmentedControl>(control(cl::Param::Bits), registry, cl::kBitsNames, notifier_);
	addChild(bits_.get());

	// THE HEAD'S OCCUPANTS — `CompCard`'s, word for word.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Clipper on" : "Clipper off");
			}
		}
	};
	// A CARD OPENED ON A STATE ALREADY SHOWS ITS MODE'S TEXTS, before any layout or tick.
	applyTips();
}

DeviceControl& ClipCard::control(cl::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, cl::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* ClipCard::enableModulatable() {
	return &control(cl::Param::Enabled);
}

bool ClipCard::headEnabled() const {
	return control(cl::Param::Enabled).value() >= 0.5;
}

void ClipCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(cl::Param::kCount); ++i) {
		control(static_cast<cl::Param>(i)).setModulation(mod, history);
	}
}

float ClipCard::openHeight() {
	return kSlotHeadHeight + 4.0f + ValueBox::kHeight + kRowGap + 2.0f * (kCellHeight + kRowGap) + kPad
	       - kRowGap;
}

float ClipCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float ClipCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

visage::Bounds ClipCard::headBounds() const {
	return ownFrame().head;
}

float ClipCard::contentRight() const {
	return bodyLeft() + bodyWidth() - kPad - kMeterWidth - kMeterGap;
}

visage::Bounds ClipCard::meterBounds() const {
	const float top = kSlotHeadHeight + 4.0f;
	return { contentRight() + kMeterGap, top, kMeterWidth, std::max(0.0f, openHeight() - kPad - top) };
}

void ClipCard::resized() { layOut(); }

int ClipCard::modeNow() const {
	return static_cast<int>(std::lround(host_.fieldAt(device_, static_cast<size_t>(cl::Param::Mode))));
}

bool ClipCard::isLive(cl::Param p) const {
	const device::ParamDesc& d = cl::kParamDescs[static_cast<size_t>(p)];
	if (d.gateControl < 0) {
		return true;
	}
	return device::relevantFor(d, host_.fieldAt(device_, static_cast<size_t>(d.gateControl)));
}

void ClipCard::applyTips() {
	const int mode = modeNow();
	if (mode == tippedMode_) {
		return;
	}
	tippedMode_ = mode;
	const cl::ClipTip& tip = cl::modeTip(mode);
	control(cl::Param::Mode).setTooltipOverride(tip.type);
	control(cl::Param::Drive).setTooltipOverride(tip.amount);
	// THE MODES WITHOUT A SHAPE KNOB leave its declared text (the knob is not drawn there).
	control(cl::Param::Shape).setTooltipOverride(tip.shape != nullptr ? tip.shape : "");
	// THE MODE'S OWN CONTROLS ARE SHOWN ONLY FOR THEIR MODES, so their text is the declared one without its "X only:" clause.
	for (const cl::Param p : { cl::Param::Bal, cl::Param::Rate, cl::Param::Angle, cl::Param::Hard, cl::Param::Bias, cl::Param::DcBlock }) {
		DeviceControl& c = control(p);
		c.setTooltipOverride(fx::eroder::withoutOnlyClause(c.desc().tooltip));
	}
	if (mode == static_cast<int>(cl::Mode::Lean)) {
		control(cl::Param::Rate).setTooltipOverride(fx::eroder::kLeanRateTip);
	}
	else if (mode >= 0 && mode < static_cast<int>(cl::Mode::kCount) && ((cl::kPaunchModes >> static_cast<uint32_t>(mode)) & 1u) != 0u) {
		control(cl::Param::Rate).setTooltipOverride(cl::kPaunchRateTip);
	}
}

void ClipCard::layOut() {
	laidOutMode_ = modeNow();
	applyTips();
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, contentRight() - left);
	const float cell = width / static_cast<float>(kKnobsPerRow);

	// ROW 1 — the mode box, and the oversampling chip at the row's right end, centred on the box.
	float y = kSlotHeadHeight + 4.0f;
	const bool dcLive = isLive(cl::Param::DcBlock);
	const float chips = kChipWidth + (dcLive ? kChipWidth + kRowGap : 0.0f);
	mode_->setBounds(left, y, std::max(0.0f, width - chips - kRowGap), ValueBox::kHeight);
	mode_->setVisible(open);
	dcBlock_->setBounds(left + width - chips, y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f, kChipWidth, SwitchChip::kHeight);
	dcBlock_->setVisible(open && dcLive);
	oversample_->setBounds(left + width - kChipWidth, y + (ValueBox::kHeight - SwitchChip::kHeight) * 0.5f,
	                       kChipWidth, SwitchChip::kHeight);
	oversample_->setVisible(open);
	y += ValueBox::kHeight + kRowGap;

	// ROW 2 — the globals, one per cell. `setBounds` unconditionally (CLAUDE.md's DPI rule).
	for (size_t i = 0; i < kGlobals.size(); ++i) {
		knobs_[i]->setBounds(left + cell * static_cast<float>(i), y, cell, kCellHeight);
		knobs_[i]->setVisible(open);
	}
	y += kCellHeight + kRowGap;

	// ROW 3 — THE MODE'S OWN CONTROLS, CLOSED UP: a hidden one takes no cell.
	size_t at = 0;
	for (size_t i = 0; i < kModeKnobs.size(); ++i) {
		Knob& knob = *knobs_[kGlobals.size() + i];
		const bool live = isLive(kModeKnobs[i]);
		knob.setBounds(left + cell * static_cast<float>(at), y, cell, kCellHeight);
		knob.setVisible(open && live);
		if (live) {
			++at;
		}
	}
	const bool bitsLive = isLive(cl::Param::Bits);
	const float bitsWidth = cell * static_cast<float>(kBitsCells) - 2.0f * kBitsInset;
	// CENTRED ON THE KNOB FACES BESIDE IT, so the row reads as one line of controls.
	bits_->setBounds(left + cell * static_cast<float>(at) + kBitsInset, y + (kKnobDiameter - kSwitchHeight) * 0.5f,
	                 bitsWidth, kSwitchHeight);
	bits_->setVisible(open && bitsLive);
}

void ClipCard::foldedChanged() {
	layOut();
}

//------------------------------------------------------------------------------------------------//
// THE READINGS                                                                                    //
//------------------------------------------------------------------------------------------------//

bool ClipCard::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(cl::Reading::kCount);
}

float ClipCard::reading(cl::Reading r) const {
	return hasReadings() ? host_.readingAt(device_, static_cast<size_t>(r)) : 0.0f;
}

bool ClipCard::ledLit() const {
	return hasReadings() && reading(cl::Reading::Over) >= 0.5f;
}

ClipCard::MeterFractions ClipCard::meterFractions() const {
	if (!hasReadings()) {
		return {};
	}
	return { dyn_meter::levelFraction(reading(cl::Reading::InPeak)),
		     dyn_meter::levelFraction(reading(cl::Reading::OutPeak)) };
}

void ClipCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE SLOT MENU IS DRAWN HERE, OUTSIDE THE BODY'S CLAMP** — `FxCard::drawHeadExtras` defaults to
	// nothing, and a card that does not override it has a working "..." press with nothing drawn to
	// press, so the header's "..." item went missing. See `FilterCard::drawHeadExtras`.
	drawSlotMenu(canvas, kMuted);
}

bool ClipCard::updateContent() {
	bool changed = false;
	if (modeNow() != laidOutMode_) {
		layOut();
		mode_->redraw();
		changed = true;
	}
	// PUBLISHED TO UNPUBLISHED IS A CHANGE TOO — `CompCard::updateContent`.
	const bool has = hasReadings();
	if (has != drawnHadReadings_) {
		drawnHadReadings_ = has;
		changed = true;
	}
	if (has) {
		const float in = reading(cl::Reading::InPeak);
		const float out = reading(cl::Reading::OutPeak);
		// THE OVER IS IN THE COMPARISON — an over in an otherwise steady passage moves nothing else.
		const float over = reading(cl::Reading::Over);
		if (in != drawnIn_ || out != drawnOut_ || over != drawnOver_) {
			drawnIn_ = in;
			drawnOut_ = out;
			drawnOver_ = over;
			changed = true;
		}
	}
	// THE DELIVERY, NOT ONLY THE REPORT — `CompCard::updateContent`.
	if (changed) {
		redraw();
	}
	return changed;
}

//------------------------------------------------------------------------------------------------//
// DRAWING                                                                                         //
//------------------------------------------------------------------------------------------------//

void ClipCard::drawMeter(visage::Canvas& canvas) {
	const visage::Bounds box = meterBounds();
	if (!hasReadings()) {
		dyn_meter::drawNoSignal(canvas, box);
		return;
	}

	// THE LED, AT THE TOP OF THE COLUMN AND CAPTIONED. Lit is the value colour — the palette's answer,
	// as Bus Utility's first LED and OWLTT's clip square both took it; unlit is the seated ring, so the
	// indicator has a place on the card whether or not anything has gone over.
	const float cx = box.x() + box.width() * 0.5f;
	const float cy = box.y() + kLedRadius + 1.0f;
	if (ledLit()) {
		canvas.setColor(kMeterValue);
		canvas.circle(cx - kLedRadius, cy - kLedRadius, kLedRadius * 2.0f);
	}
	else {
		canvas.setColor(kMuted);
		canvas.ring(cx - kLedRadius, cy - kLedRadius, kLedRadius * 2.0f, 1.5f);
	}
	dyn_meter::drawCaption(canvas, "OVER", cx, cy + kLedRadius + 1.0f, box.width());

	const float barsTop = cy + kLedRadius + dyn_meter::kCaptionHeight + 4.0f;
	const float barHeight = std::max(0.0f, box.bottom() - barsTop - dyn_meter::kCaptionHeight - 2.0f);
	const float half = box.width() * 0.5f;
	const MeterFractions f = meterFractions();
	static constexpr std::array<const char*, 2> kCaps { "IN", "OUT" };
	const std::array<float, 2> fractions { f.in, f.out };
	for (size_t i = 0; i < kCaps.size(); ++i) {
		const float centre = box.x() + half * (static_cast<float>(i) + 0.5f);
		dyn_meter::drawBar(canvas, centre - dyn_meter::kBarWidth * 0.5f, barsTop, barHeight, fractions[i], false,
		                   kMeterAccent);
		dyn_meter::drawCaption(canvas, kCaps[i], centre, barsTop + barHeight + 2.0f, half);
	}
}

void ClipCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	drawMeter(canvas);
}

std::vector<cl::Param> ClipCard::addressedParamsForTest() const {
	// EVERY CONTROL THE SURFACE REACHES, HIDDEN OR NOT — the Mode box reveals the rest.
	std::vector<cl::Param> out(kGlobals.begin(), kGlobals.end());
	out.insert(out.end(), kModeKnobs.begin(), kModeKnobs.end());
	out.push_back(cl::Param::Mode);
	out.push_back(cl::Param::Oversample);
	out.push_back(cl::Param::DcBlock);
	out.push_back(cl::Param::Bits);
	out.push_back(cl::Param::Enabled);
	return out;
}

visage::Frame* ClipCard::widgetForTest(cl::Param p) const {
	for (size_t i = 0; i < kGlobals.size(); ++i) {
		if (kGlobals[i] == p) {
			return knobs_[i].get();
		}
	}
	for (size_t i = 0; i < kModeKnobs.size(); ++i) {
		if (kModeKnobs[i] == p) {
			return knobs_[kGlobals.size() + i].get();
		}
	}
	switch (p) {
	case cl::Param::Mode: return mode_.get();
	case cl::Param::Oversample: return oversample_.get();
	case cl::Param::DcBlock: return dcBlock_.get();
	case cl::Param::Bits: return bits_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
