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

#include "plugin/ui/drum_device_ui.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/trig_pad.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

DeviceControl& DrumDeviceModel::control(dd::Param p) const {
	const auto i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, id_, dd::kDescriptor, i);
	}
	return *controls_[i];
}

dd::Settings DrumDeviceModel::settings() const {
	dd::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(id_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.modulatedFieldAt(id_, i);
	}
	return dd::stateOf(row, present);
}

feathers::drum::Kind DrumDeviceModel::kind() const {
	return static_cast<feathers::drum::Kind>(std::clamp(settings().index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
}

bool DrumDeviceModel::relevant(dd::Param p) const {
	const device::ParamDesc& d = dd::kParamDescs[static_cast<size_t>(p)];
	if (d.gateControl < 0 || static_cast<size_t>(d.gateControl) >= static_cast<size_t>(dd::Param::kCount)) {
		return true;
	}
	return device::relevantFor(d, settings().v[static_cast<size_t>(d.gateControl)]);
}

feathers::drum::Laws DrumDeviceModel::laws() const {
	const dd::Settings s = settings();
	const feathers::drum::Kind k = kind();
	// IN PITCHED, THE PICTURE IS THE DRUM AS PLAYED ON MIDDLE C (the key is the pitch, and a picture needs one).
	return feathers::drum::lawsFor(k, dd::fieldsOf(s), dd::controlsOf(s), dd::baseHzOf(k, s, kPictureKey, 0.0), 0.8);
}

feathers::drum::SettledPitch DrumDeviceModel::settled() const {
	const dd::Settings s = settings();
	if (s.pitched()) {
		return {}; // THE KEY SETS THE PITCH: there is no one settled pitch to read or to type
	}
	const feathers::drum::Kind k = kind();
	return feathers::drum::settledPitch(k, dd::fieldsOf(s), dd::controlsOf(s), dd::baseHzOf(k, s, 0.0));
}

double DrumDeviceModel::decaySeconds() const { return std::exp2(static_cast<double>(settings().at(dd::Param::Decay))); }
double DrumDeviceModel::levelDb() const { return static_cast<double>(settings().at(dd::Param::Level)); }
int DrumDeviceModel::noteKey() const {
	// PITCHED answers every key and Note is not read: the play button strikes the picture's key.
	if (settings().pitched()) {
		return kPictureKey;
	}
	return static_cast<int>(std::lround(host_.fieldAt(id_, static_cast<size_t>(dd::Param::Note))));
}

HitPicture::Controls DrumDeviceModel::handles() const {
	const feathers::drum::ControlUse uses = feathers::drum::kindControls(kind());
	const auto c = [&](dd::Param p) -> device::Modulatable* { return &control(p); };
	HitPicture::Controls h;
	h.tune = c(dd::Param::Tune);
	h.decay = c(dd::Param::Decay);
	h.level = c(dd::Param::Level);
	if (uses.sweepTime) {
		h.sweepTime = c(dd::Param::SweepTime);
	}
	if (uses.drive) {
		h.drive = c(dd::Param::Drive);
		h.driveTone = c(dd::Param::DriveTone);
	}
	if (uses.snareEnvelopes) {
		h.wireDecay = c(dd::Param::WireDecay);
		h.clickDecay = c(dd::Param::ClickDecay);
	}
	if (uses.bell) {
		h.bellStart = c(dd::Param::BellStart);
		h.bellEnd = c(dd::Param::BellEnd);
		h.bellTime = c(dd::Param::BellTime);
		h.bell2 = c(dd::Param::Bell2Hz);
		h.bell2Time = c(dd::Param::Bell2Time);
	}
	if (uses.kickShape) {
		h.hold = c(dd::Param::KickHold);
		h.dip = c(dd::Param::KickDip);
		h.dipTime = c(dd::Param::KickDipTime);
		h.clickSweep = c(dd::Param::ClickSweep);
		h.clickSweepTime = c(dd::Param::ClickSweepTime);
		h.startPhase = c(dd::Param::StartPhase);
	}
	if (uses.spread) {
		h.spread = c(dd::Param::Spread);
	}
	if (uses.clapFilter) {
		h.clapHighPass = c(dd::Param::ClapHighPass);
		h.clapQ = c(dd::Param::ClapQ);
	}
	return h;
}

void DrumDeviceModel::refreshTooltips() const {
	const dd::Settings s = settings();
	const auto k = static_cast<size_t>(std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
	const int state = static_cast<int>(k) * 2 + (s.pitched() ? 1 : 0);
	if (state == tipState_) {
		return; // setTooltipOverride takes a string: only a change of Kind or Pitched pays for one
	}
	tipState_ = state;
	control(dd::Param::FieldTone).setTooltipOverride(feathers::fx::drumdevice::kToneTips[k]);
	control(dd::Param::FieldSnap).setTooltipOverride(feathers::fx::drumdevice::kSnapTips[k]);
	control(dd::Param::Decay).setTooltipOverride(feathers::fx::drumdevice::kDecayTips[k]);
	control(dd::Param::Tune).setTooltipOverride(s.pitched() ? feathers::fx::drumdevice::kTunePitchedTip : feathers::fx::drumdevice::kTuneTips[k]);
	control(dd::Param::HighPass).setTooltipOverride(feathers::fx::drumdevice::kHighPassTips[k]);
}

void DrumDeviceModel::setModulation(ModState* mod, EditHistory* history) {
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<dd::Param>(i)).setModulation(mod, history);
	}
}

bool DrumDeviceModel::droneOn() const {
	return settings().index(dd::Param::Trigger) == feathers::drum::kPlayDrone && feathers::drum::kindSustains(kind());
}

} // namespace feathers::ui
