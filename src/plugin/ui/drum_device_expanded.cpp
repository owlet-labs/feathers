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

#include "plugin/ui/drum_device_expanded.h"
#include "plugin/ui/gamma_box.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/type_in.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace feathers::ui {

namespace {

constexpr float kGap = 6.0f;
constexpr float kRow = 28.0f; // a row's height: a box's, with a pill or a segment centred in it
constexpr float kPillW = 92.0f;
constexpr float kPillH = 22.0f;

/// A ROW-WRAPPING FLOW: items in a line, each centred in a row `kRow` tall, wrapping when the next would pass the right edge.
struct Flow {
	float x0, xMax, x, y;
	void place(visage::Frame* f, float width, float height) {
		if (x > x0 && x + width > xMax) {
			x = x0;
			y += kRow + 2.0f;
		}
		f->setBounds(x, y + (kRow - height) * 0.5f, width, height);
		x += width + kGap;
	}
	[[nodiscard]] float bottom() const { return y + kRow; }
};

/// The controls drawn as a box (a typed or dragged number, or a choice menu), a segmented choice, a zone cell, or a pill.
bool isBox(dd::Param p) {
	switch (p) {
	case dd::Param::Kind: case dd::Param::Note: case dd::Param::Seed: case dd::Param::Zone: case dd::Param::ZoneOffset:
	case dd::Param::Gamma: case dd::Param::Rotate: case dd::Param::TravelX: case dd::Param::TravelY: case dd::Param::Claps:
	case dd::Param::SweepSync: return true;
	default: return false;
	}
}
bool isSegmented(dd::Param p) {
	return p == dd::Param::Trigger || p == dd::Param::Keys || p == dd::Param::Voices || p == dd::Param::PitchMode || p == dd::Param::DriveCurve
	       || p == dd::Param::WireContour || p == dd::Param::BellLink;
}

} // namespace

DrumDeviceExpanded::DrumDeviceExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : model_(host, id) {
	picture_.setModHandles(true); // every handle whose control is a destination is a modulation target
	addChild(&picture_);
	for (size_t i = 0; i < widgets_.size(); ++i) {
		const auto p = static_cast<dd::Param>(i);
		DeviceControl& control = model_.control(p);
		const device::ParamDesc& d = dd::kParamDescs[i];
		std::unique_ptr<visage::Frame> w;
		if (p == dd::Param::Enabled) {
			continue; // THE HEADER CHIP IS THE ONLY DEVICE-ENABLE (a window body carries none of its own)
		}
		else if (isSegmented(p)) {
			// THE TRIGGER'S PLAIN FACE IS TWO-WAY (One-shot | Gate): Drone is offered only on a kind that can be held, by the three-way one beside it.
			w = std::make_unique<SegmentedControl>(control, registry, p == dd::Param::Trigger ? std::span<const char* const>(dd::kTriggerNamesNoDrone) : d.names, notifier);
			if (p == dd::Param::Trigger) {
				triggerDrone_ = std::make_unique<SegmentedControl>(control, registry, d.names, notifier);
				addChild(triggerDrone_.get());
			}
		}
		else if (p == dd::Param::Rotate || p == dd::Param::Gamma) {
			// AS AUTOMOD'S MINI TERRAIN: Gamma and Rotate are unbounded, so boxes (`GammaBox`), never knobs. ROTATE sits INSIDE the pad, in its
			// top-right corner (made after the pad, so it draws over the map it turns), one unit of rotate per drag.
			auto box = std::make_unique<GammaBox>(control, d.name);
			if (p == dd::Param::Rotate) {
				box->setDragSpan(1.0);
			}
			w = std::move(box);
		}
		else if (p == dd::Param::SweepDir) {
			w = std::make_unique<SwitchChip>(control, "Down"); // lit: sweeping down; unlit: up
		}
		else if (isBox(p)) {
			w = std::make_unique<ValueBox>(control, d.name);
		}
		else {
			auto pill = std::make_unique<FaderPill>(control, d.shortName != nullptr ? d.shortName : d.name);
			if (p == dd::Param::Tune) {
				pill->setWholeSteps(true); // semitones; Shift fine
			}
			w = std::move(pill);
		}
		if (p != dd::Param::Rotate) {
			addChild(w.get());
		}
		widgets_[i] = std::move(w);
	}
	// THE TERRAIN PAD over Travel X and Y (unbounded: the view follows the star, no edge), turned by Rotate as the other pads are.
	pad_ = std::make_unique<DrumDevicePad>(
	    [this](size_t index, float value) { model_.control(index == 0 ? dd::Param::TravelX : dd::Param::TravelY).setValue(static_cast<double>(value)); },
	    [this](size_t field) { return static_cast<float>(model_.control(field == 0 ? dd::Param::TravelX : dd::Param::TravelY).value()); },
	    model_.control(dd::Param::Rotate));
	pad_->context = [this]() -> const DrumPadContext& { return padContext_; };
	pad_->onHoldBegan = [this] {
		padGroup_ = historyOf(model_.control(dd::Param::TravelX));
		if (padGroup_ != nullptr) {
			padGroup_->beginGroup("Move drum");
		}
		model_.control(dd::Param::TravelX).beginGesture();
		model_.control(dd::Param::TravelY).beginGesture();
	};
	pad_->onHoldEnded = [this] {
		model_.control(dd::Param::TravelX).endGesture();
		model_.control(dd::Param::TravelY).endGesture();
		if (padGroup_ != nullptr) {
			padGroup_->endGroup();
			padGroup_ = nullptr;
		}
	};
	pad_->setTravelControls(&model_.control(dd::Param::TravelX), &model_.control(dd::Param::TravelY));
	installTooltipHover(*pad_, *pad_); // after the pad's own press handlers
	addChild(pad_.get());
	addChild(widgets_[static_cast<size_t>(dd::Param::Rotate)].get()); // over the pad
	// THE HEART AND MUTATE under it: the likes' verbs, the warp the pad is drawn through.
	likes_ = std::make_unique<DrumLikeBar>(model_, *pad_, notifier);
	installTooltipHover(*likes_, *likes_);
	addChild(likes_.get());
	// THE SETTLED PITCH over Tune: typed note or Hz sets Tune, refused where there is no one pitch.
	settledControl_ = std::make_unique<SettledPitchControl>(model_.control(dd::Param::Tune), [this] { return model_.settled(); });
	readout_ = std::make_unique<PitchReadout>(*settledControl_);
	addChild(readout_.get());
	if (host.canPostNote()) {
		play_ = std::make_unique<DrumPlayButton>(host, [this] { return model_.noteKey(); });
		addChild(play_.get());
	}
	(void)likes_->refresh();
	padContext_.settings = model_.settings();
	padContext_.likes = likes_->state();
	layOut();
}

std::string DrumDeviceExpanded::headerTitle() const {
	const auto k = static_cast<size_t>(model_.kind());
	return std::string("Drum \xc2\xb7 ") + dd::kKindLabels[std::min(k, std::size(dd::kKindLabels) - 1)];
}

void DrumDeviceExpanded::modulationBound(ModState* mod, EditHistory* history) {
	model_.setModulation(mod, history);
}

void DrumDeviceExpanded::resized() {
	shownKind_ = feathers::drum::Kind::kCount; // lay out again at the new size
	layOut();
}

void DrumDeviceExpanded::layOut() {
	const float w = width() > 0.0f ? width() : kWidth;
	const float xMax = w - kPad;
	const feathers::drum::Kind kind = model_.kind();
	shownKind_ = kind;
	shownGate_ = model_.settings().index(dd::Param::Trigger) == 1;
	shownKeys_ = model_.settings().index(dd::Param::Keys);
	const feathers::drum::SettledPitch settled = model_.settled();
	shownSettled_ = settled.defined;
	picture_.setBounds(kPad, kPad, w - 2.0f * kPad, kPictureHeight);
	std::array<bool, static_cast<size_t>(dd::Param::kCount)> placed {};
	const auto show = [&](dd::Param p, bool shown) {
		visage::Frame* f = widgets_[static_cast<size_t>(p)].get();
		if (f != nullptr && f->isVisible() != shown) {
			f->setVisible(shown);
		}
		return f;
	};
	// A SECTION'S FLOW: each listed control is seated if the Kind reads it (and hidden, not greyed, if not).
	const auto seat = [&](Flow& flow, dd::Param p, float width, float height) {
		visage::Frame* f = show(p, model_.relevant(p));
		placed[static_cast<size_t>(p)] = true;
		if (f != nullptr && model_.relevant(p)) {
			flow.place(f, width, height);
		}
	};
	// STRIKE: what a pad has around its sound.
	Flow strike { kPad, xMax, kPad, kPad + kPictureHeight + kGap }; // x0, xMax, x, y
	seat(strike, dd::Param::Kind, 86.0f, ValueBox::kHeight);
	seat(strike, dd::Param::Keys, 168.0f, 22.0f);
	seat(strike, dd::Param::Note, 56.0f, ValueBox::kHeight);
	if (play_ != nullptr) {
		strike.place(play_.get(), 30.0f, 22.0f);
	}
	// ONE OF THE TWO TRIGGER FACES: with Drone on a kind that can hold it, else the plain two-way (the engine's `kindSustains`).
	const bool sustains = feathers::drum::kindSustains(kind);
	if (triggerDrone_ != nullptr) {
		triggerDrone_->setVisible(sustains);
	}
	if (sustains) {
		placed[static_cast<size_t>(dd::Param::Trigger)] = true;
		show(dd::Param::Trigger, false);
		strike.place(triggerDrone_.get(), 190.0f, 22.0f);
	}
	else {
		seat(strike, dd::Param::Trigger, 128.0f, 22.0f);
	}
	seat(strike, dd::Param::Voices, 96.0f, 22.0f);
	for (const dd::Param p : { dd::Param::Level, dd::Param::Pan, dd::Param::Tune, dd::Param::Decay, dd::Param::Release, dd::Param::Bend,
	                           dd::Param::Delay, dd::Param::HighPass }) {
		seat(strike, p, kPillW, kPillH);
	}
	seat(strike, dd::Param::Seed, 80.0f, ValueBox::kHeight);
	// VELOCITY: ONE ROW OF DEPTHS, velocity to each destination the hit has (Level, Tone, Snap, Tension where the kind has a glide, Noise).
	// Each stops at its end: nothing wraps into another mode.
	Flow velocity { kPad, xMax, kPad, strike.bottom() + 4.0f };
	for (const dd::Param p : { dd::Param::VelLevel, dd::Param::VelTone, dd::Param::VelSnap, dd::Param::VelTension, dd::Param::VelocityNoise }) {
		seat(velocity, p, kPillW, kPillH);
	}
	// TERRAIN: the pad at the left; Zone, Offset, Gamma, Rotate and the travel beside it, then the pitch mode, the readout and the scalers.
	const float terrainY = velocity.bottom() + 12.0f;
	pad_->setBounds(kPad, terrainY, kTerrainWidth, kTerrainHeight);
	if (visage::Frame* rotate = widgets_[static_cast<size_t>(dd::Param::Rotate)].get()) {
		constexpr float kInset = 6.0f;
		constexpr float kRotateWidth = 118.0f;
		const float natural = static_cast<GammaBox*>(rotate)->naturalHeight();
		rotate->setBounds(kPad + kTerrainWidth - kRotateWidth - kInset, terrainY + kInset, kRotateWidth, natural);
		pad_->setInset({ kTerrainWidth - kRotateWidth - kInset, kInset, kRotateWidth, natural });
		placed[static_cast<size_t>(dd::Param::Rotate)] = true;
	}
	likes_->setBounds(kPad, terrainY + kTerrainHeight + 4.0f, DrumLikeBar::kHeartWidth + DrumLikeBar::kGap + DrumLikeBar::kMutateWidth, DrumLikeBar::kHeight);
	Flow terrain { kPad + kTerrainWidth + 12.0f, xMax, kPad + kTerrainWidth + 12.0f, terrainY };
	for (const dd::Param p : { dd::Param::Zone, dd::Param::ZoneOffset, dd::Param::Gamma, dd::Param::TravelX, dd::Param::TravelY }) {
		seat(terrain, p, 84.0f, ValueBox::kHeight);
	}
	seat(terrain, dd::Param::PitchMode, 196.0f, 22.0f);
	readout_->setVisible(settled.defined);
	if (settled.defined) {
		terrain.place(readout_.get(), 188.0f, 24.0f);
	}
	for (const dd::Param p : { dd::Param::FieldPitch, dd::Param::FieldLength, dd::Param::FieldSweep, dd::Param::FieldTone, dd::Param::FieldNoise,
	                           dd::Param::FieldSnap, dd::Param::FieldMetal }) {
		seat(terrain, p, kPillW, kPillH);
	}
	// BODY: the kind's own controls, then the snare's wire and bell amounts.
	const float bodyY = std::max(terrain.bottom(), terrainY + kTerrainHeight + 4.0f + DrumLikeBar::kHeight) + 12.0f;
	Flow body { kPad, xMax, kPad, bodyY };
	seat(body, dd::Param::Drive, kPillW, kPillH);
	seat(body, dd::Param::DriveCurve, 184.0f, 22.0f);
	seat(body, dd::Param::DriveTone, kPillW, kPillH);
	for (const dd::Param p : { dd::Param::Density, dd::Param::Grain, dd::Param::Pressure, dd::Param::Speed, dd::Param::Rate, dd::Param::Rise, dd::Param::Flow, dd::Param::Splash, dd::Param::SweepRange, dd::Param::SweepTime, dd::Param::Width, dd::Param::Spread, dd::Param::ClapQ, dd::Param::ClapHighPass,
	                           dd::Param::WireDecay, dd::Param::ClickDecay, dd::Param::BellStart, dd::Param::BellEnd, dd::Param::BellTime,
	                           dd::Param::Bell2Hz, dd::Param::Bell2Time, dd::Param::KickHold, dd::Param::KickDip, dd::Param::KickDipTime,
	                           dd::Param::ClickSweep, dd::Param::ClickSweepTime, dd::Param::StartPhase }) {
		seat(body, p, kPillW, kPillH);
	}
	seat(body, dd::Param::Claps, 56.0f, ValueBox::kHeight);
	seat(body, dd::Param::SweepDir, 56.0f, 22.0f);
	seat(body, dd::Param::SweepSync, 96.0f, ValueBox::kHeight);
	// THE SNARE'S WIRES AND BELLS: one amount each, where the named zones were.
	seat(body, dd::Param::WireContour, 150.0f, 22.0f);
	for (const dd::Param p : { dd::Param::WireBuzz, dd::Param::WireThreshold, dd::Param::WireGate, dd::Param::BodyGate, dd::Param::Bell1Level,
	                           dd::Param::Bell2Level, dd::Param::BellQ }) {
		seat(body, p, kPillW, kPillH);
	}
	seat(body, dd::Param::BellLink, 110.0f, 22.0f);
	const float zy = body.bottom();
	contentBottom_ = zy;
	for (size_t i = 0; i < placed.size(); ++i) {
		(void)placed[i]; // every control is seated above; none is left to a default place
	}
	(void)kind;
	redraw();
}

bool DrumDeviceExpanded::updateContent() {
	follow();
	const feathers::drum::Kind kind = model_.kind();
	const bool gate = model_.settings().index(dd::Param::Trigger) == 1;
	const bool settled = model_.settled().defined;
	const int keys = model_.settings().index(dd::Param::Keys);
	bool moved = false;
	if (kind != shownKind_ || gate != shownGate_ || settled != shownSettled_ || keys != shownKeys_) {
		layOut();
		moved = true;
	}
	return moved;
}

void DrumDeviceExpanded::follow() {
	picture_.show(model_.kind(), model_.laws(), model_.decaySeconds(), model_.levelDb(), model_.handles());
	(void)likes_->refresh();
	padContext_.settings = model_.settings();
	padContext_.likes = likes_->state();
	readout_->refresh(drum_pitch::readoutText(model_.settled()));
}

void DrumDeviceExpanded::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, width(), height());
}

} // namespace feathers::ui
