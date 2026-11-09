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

#include "plugin/ui/spectrum_source.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/param_knob.h"
#include "plugin/ui/spectrum_panel.h"
#include "plugin/ui/widget_registry.h"

namespace feathers::ui {

namespace {
clap_id idOf(size_t slot, SpectralControl c) { return static_cast<clap_id>(spectralParam(slot, c)); }
} // namespace

bool SpectrumSource::filterOpen(size_t slot, int filter) const {
	const auto ctl = [filter](SpectralControl c) { return spectralFilterControl(filter, c); };
	const auto mode =
	    static_cast<spectral::FilterMode>(steppedIndex(value(slot, ctl(SpectralControl::FilterMode)), kSpectralFilterModeNames.size()));
	const double cutoff = value(slot, ctl(SpectralControl::FilterCutoff));
	return spectral::filterOpenness(mode, cutoff, cutoff, value(slot, ctl(SpectralControl::FilterGain))) >= 1.0;
}

bool BridgeSpectrumSource::master() const { return bridge_.spectralOn(); }

double BridgeSpectrumSource::value(size_t slot, SpectralControl c) const { return bridge_.value(idOf(slot, c)); }

bool BridgeSpectrumSource::range(size_t slot, SpectralControl c, double& lo, double& hi) const {
	const ParamDesc* desc = findParam(idOf(slot, c));
	if (desc == nullptr) {
		return false;
	}
	lo = desc->min;
	hi = desc->max;
	return true;
}

const char* BridgeSpectrumSource::tooltip(size_t slot, SpectralControl c) const {
	const ParamDesc* desc = findParam(idOf(slot, c));
	return desc != nullptr ? desc->tooltip : nullptr;
}

std::unique_ptr<device::Modulatable> BridgeSpectrumSource::control(size_t slot, SpectralControl c) {
	return std::make_unique<ParamControl>(bridge_, findParam(idOf(slot, c)));
}

std::unique_ptr<Knob> BridgeSpectrumSource::knob(size_t slot, SpectralControl c, const char* caption, Notifier* notifier) {
	return paramKnob(bridge_, *registry_, findParam(idOf(slot, c)), caption, notifier);
}

std::unique_ptr<SegmentedControl> BridgeSpectrumSource::segmented(size_t slot, SpectralControl c,
                                                                  std::span<const char* const> names, Notifier* notifier) {
	return std::make_unique<SegmentedControl>(bridge_, *registry_, *findParam(idOf(slot, c)), names, notifier);
}

void BridgeSpectrumSource::watch(size_t slot, SpectralControl c, visage::Frame* frame, std::vector<Watch>& into) {
	if (registry_ != nullptr) {
		into.push_back(registry_->watchToken(idOf(slot, c), frame));
	}
}

spectral::DrawnCurve BridgeSpectrumSource::curve(size_t slot) const { return bridge_.spectralCurve(slot); }

void BridgeSpectrumSource::setCurveOn(size_t slot, bool on) { bridge_.setSpectralCurveOn(slot, on); }

void BridgeSpectrumSource::setCurveBand(size_t slot, size_t band, float gain) { bridge_.setSpectralCurveBand(slot, band, gain); }

void BridgeSpectrumSource::flattenCurve(size_t slot) { bridge_.flattenSpectralCurve(slot); }

std::function<void(const spectral::DrawnCurve&)> BridgeSpectrumSource::curveWriter(size_t slot) {
	ParamBridge* bridge = &bridge_; // the bridge outlives every editor and every undo record
	return [bridge, slot](const spectral::DrawnCurve& curve) { bridge->setSpectralCurve(slot, curve); };
}

std::unique_ptr<device::Modulatable> BridgeSpectrumSource::curveSwitch(size_t slot) {
	ParamBridge* bridge = &bridge_;
	auto sw = std::make_unique<StoredSwitch>(
	    kSpectralCurveOnKeys[slot], "Spectral Curve", "Switches this sound's drawn curve in: a gain per harmonic band.",
	    [bridge, slot] { return bridge->spectralCurve(slot).on; },
	    // ONE UNDO STEP PER FLIP (backlog JB) is StoredSwitch's own record; the setter only sets.
	    [bridge, slot](bool on) { bridge->setSpectralCurveOn(slot, on); });
	curveSwitches_.push_back(sw.get());
	return sw;
}

void BridgeSpectrumSource::bindModulation(ModState*, EditHistory* history) {
	for (StoredSwitch* sw : curveSwitches_) {
		sw->setHistory(history);
	}
}

uint32_t BridgeSpectrumSource::frameSerial() const { return bridge_.plotSerial(); }

bool BridgeSpectrumSource::readFrame(SpectralPlotFrame& out, uint32_t& serial) const { return bridge_.readPlot(out, serial); }

} // namespace feathers::ui
