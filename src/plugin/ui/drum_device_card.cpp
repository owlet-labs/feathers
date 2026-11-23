/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/drum_device_card.h"

#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

DrumDeviceCard::DrumDeviceCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), notifier_(notifier), device_(id), model_(host, id) {
	picture_.setLabelsShown(false);
	addChild(&picture_);
	kind_ = std::make_unique<ValueBox>(model_.control(dd::Param::Kind), "Kind");
	note_ = std::make_unique<ValueBox>(model_.control(dd::Param::Note), "Note");
	keys_ = std::make_unique<SegmentedControl>(model_.control(dd::Param::Keys), registry, dd::kParamDescs[static_cast<size_t>(dd::Param::Keys)].names, notifier);
	level_ = std::make_unique<FaderPill>(model_.control(dd::Param::Level), "Level");
	tune_ = std::make_unique<FaderPill>(model_.control(dd::Param::Tune), "Tune");
	tune_->setWholeSteps(true); // semitones, Shift fine
	addChild(kind_.get());
	addChild(keys_.get());
	addChild(note_.get());
	addChild(level_.get());
	addChild(tune_.get());
	// THE PLAY BUTTON ONLY WHERE THE HOST HAS A NOTE ROAD (the honesty rule): the standalone rack has none, so it draws no button.
	if (host.canPostNote()) {
		play_ = std::make_unique<DrumPlayButton>(host, [this] { return model_.noteKey(); });
		addChild(play_.get());
	}
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		const visage::Point p = e.position;
		if (slotMenuAt(p)) {
			pressSlotMenu();
		}
		else if (enableBounds().contains(p)) {
			toggleEnabled();
		}
	};
}

float DrumDeviceCard::openHeight() {
	return kHeadHeight + kGap + kPictureHeight + kGap + ValueBox::kHeight + kGap + kKeysHeight + kGap + kPillHeight + kPad;
}

void DrumDeviceCard::toggleEnabled() {
	if (const auto nowOn = toggleEnable()) {
		if (notifier_ != nullptr) {
			notifier_->say(*nowOn ? "Drum on" : "Drum off");
		}
	}
}

void DrumDeviceCard::foldedChanged() {
	for (visage::Frame* f : { static_cast<visage::Frame*>(&picture_), static_cast<visage::Frame*>(kind_.get()), static_cast<visage::Frame*>(keys_.get()), static_cast<visage::Frame*>(note_.get()),
	                          static_cast<visage::Frame*>(level_.get()), static_cast<visage::Frame*>(tune_.get()), static_cast<visage::Frame*>(play_.get()) }) {
		if (f != nullptr) {
			f->setVisible(!folded_);
		}
	}
}

void DrumDeviceCard::setModulation(ModState* state, EditHistory* history) {
	model_.setModulation(state, history);
}

void DrumDeviceCard::resized() {
	const float left = bodyLeft() + kPad;
	const float w = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	float y = bodyTopEdge() + kGap;
	picture_.setBounds(left, y, w, kPictureHeight);
	y += kPictureHeight + kGap;
	// ROW ONE: Kind, Note, and the play button at the right.
	const float playW = play_ != nullptr ? 30.0f : 0.0f;
	const float playGap = play_ != nullptr ? kGap : 0.0f;
	const float boxes = w - playW - playGap;
	// KIND, THEN NOTE (One Key's alone: hidden, not greyed, in Any Key and Pitched, and Kind takes its width).
	const bool noteShown = model_.relevant(dd::Param::Note);
	noteShown_ = noteShown;
	const float noteW = noteShown ? 52.0f + kGap : 0.0f;
	const float kindW = std::max(0.0f, boxes - noteW);
	kind_->setBounds(left, y, kindW, ValueBox::kHeight);
	note_->setBounds(left + kindW + kGap, y, 52.0f, ValueBox::kHeight);
	note_->setVisible(!folded_ && noteShown);
	if (play_ != nullptr) {
		play_->setBounds(left + w - playW, y + (ValueBox::kHeight - 22.0f) * 0.5f, playW, 22.0f);
	}
	y += ValueBox::kHeight + kGap;
	// ROW TWO: the Keys choice across the width (three labels on the face, before one is applied).
	keys_->setBounds(left, y, w, kKeysHeight);
	y += kKeysHeight + kGap;
	// ROW THREE: Level and Tune, sharing the width.
	const float each = (w - kGap) * 0.5f;
	level_->setBounds(left, y, each, kPillHeight);
	tune_->setBounds(left + each + kGap, y, each, kPillHeight);
}

bool DrumDeviceCard::updateContent() {
	if (!isVisible() || folded()) {
		return false;
	}
	picture_.show(model_.kind(), model_.laws(), model_.decaySeconds(), model_.levelDb(), model_.handles());
	if (model_.relevant(dd::Param::Note) != noteShown_) {
		noteShown_ = !noteShown_;
		resized();
	}
	// A DRONE TAKES NO TRIGGER: the Trig button is hidden while it is on.
	if (play_ != nullptr && play_->isVisible() == model_.droneOn()) {
		play_->setVisible(!model_.droneOn());
	}
	const bool on = model_.control(dd::Param::Enabled).value() >= 0.5;
	for (visage::Frame* f : { static_cast<visage::Frame*>(level_.get()), static_cast<visage::Frame*>(tune_.get()) }) {
		(void)f;
	}
	(void)on;
	return true;
}

void DrumDeviceCard::drawHeadExtras(visage::Canvas& canvas) { drawSlotMenu(canvas, kMuted); }

void DrumDeviceCard::drawContent(visage::Canvas& canvas) {
	const bool on = model_.control(dd::Param::Enabled).value() >= 0.5;
	const visage::Bounds sw = enableBounds();
	canvas.setColor(on ? kAccent : kMuted);
	if (on) {
		canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
	}
	else {
		canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
	}
}

} // namespace feathers::ui
