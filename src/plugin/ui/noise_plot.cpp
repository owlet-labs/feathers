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

#include "plugin/ui/noise_plot.h"

#include "dsp/random_events.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace nz = fx::noise;

namespace {

constexpr float kInset = 4.0f;
constexpr size_t kPoints = 120;
/// THE STRIP'S SAMPLE RATE: the hits' times only need to be placed to a pixel, so the preview runs the
/// generator at a coarse rate rather than the host's. The DISTRIBUTION is the same at any rate the gaps clear.
constexpr double kStripRate = 8000.0;

double log10Of(double v) { return std::log10(std::max(v, 1e-12)); }

} // namespace

NoisePlot::NoisePlot(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {
	controls_.resize(static_cast<size_t>(nz::Param::kCount));
	spectrumDb_.assign(kPoints, 0.0);
	filterDb_.assign(kPoints, 0.0);

	onMouseDown() = [this](const visage::MouseEvent& e) {
		DeviceControl& cutoff = control(nz::Param::Cutoff);
		drag_ = {};
		drag_.active = true;
		drag_.startY = e.position.y;
		if (!e.isAltDown() && cutoff.display().routable) {
			drag_.route = true;
			const float before = cutoff.display().amount;
			drag_.anchor = before != 0.0f ? before : Knob::kRouteSeedAmount;
			cutoff.beginAmountGesture();
			cutoff.setAmount(drag_.anchor);
		}
		else {
			cutoff.beginGesture();
			control(nz::Param::Resonance).beginGesture();
			place(e);
		}
		update();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!drag_.active) {
			return;
		}
		if (drag_.route) {
			const float travel = (drag_.startY - e.position.y) / Knob::kRouteDragRange;
			control(nz::Param::Cutoff).setAmount(std::clamp(drag_.anchor + travel, -device::kMaxAmount, device::kMaxAmount));
		}
		else {
			place(e);
		}
		update();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!drag_.active) {
			return;
		}
		DeviceControl& cutoff = control(nz::Param::Cutoff);
		if (drag_.route) {
			const float amount = cutoff.display().amount;
			if (std::abs(amount) < 0.05f && amount != 0.0f) {
				cutoff.setAmount(0.0f);
			}
			cutoff.endAmountGesture();
		}
		else {
			cutoff.endGesture();
			control(nz::Param::Resonance).endGesture();
		}
		drag_ = {};
	};
	installTooltipHover(*this, *this);
}

DeviceControl& NoisePlot::control(nz::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, nz::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

void NoisePlot::offeredControls(std::vector<Offered>& out) const {
	out.push_back({ &control(nz::Param::Cutoff), Road::Gesture });
	out.push_back({ &control(nz::Param::Resonance), Road::Gesture });
}

void NoisePlot::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<nz::Param>(i)).setModulation(mod, history);
	}
}

nz::Settings NoisePlot::settingsNow() const {
	nz::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return nz::stateOf(row, present);
}

double NoisePlot::rate() const noexcept { return host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0; }

bool NoisePlot::impulses() const noexcept { return drawn_.source == static_cast<int>(nz::Source::Impulses); }

double NoisePlot::hzAt(size_t i) const noexcept {
	const double t = static_cast<double>(i) / static_cast<double>(kPoints - 1);
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float NoisePlot::hzToX(double hz) const noexcept {
	const float w = std::max(1.0f, width() - 2.0f * kInset);
	const double t = (log10Of(hz) - log10Of(kMinHz)) / (log10Of(kMaxHz) - log10Of(kMinHz));
	return kInset + static_cast<float>(std::clamp(t, 0.0, 1.0)) * w;
}

double NoisePlot::xToHz(float x) const noexcept {
	const float w = std::max(1.0f, width() - 2.0f * kInset);
	const double t = std::clamp(static_cast<double>((x - kInset) / w), 0.0, 1.0);
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float NoisePlot::plotTop() const noexcept { return kInset + 11.0f; }

/// THE SPECTRUM'S FOOT — above the strip of hits when there is one.
float NoisePlot::plotFoot() const noexcept {
	return height() - kInset - (impulses() ? kStripHeight + 3.0f : 0.0f);
}

float NoisePlot::dbToY(double db) const noexcept {
	const double t = std::clamp((db - kFloorDb) / (kTopDb - kFloorDb), 0.0, 1.0);
	return plotFoot() - static_cast<float>(t) * (plotFoot() - plotTop());
}

float NoisePlot::resonanceToY(float res) const noexcept {
	return plotFoot() - std::clamp(res, 0.0f, 1.0f) * (plotFoot() - plotTop());
}

float NoisePlot::yToResonance(float y) const noexcept {
	const float span = std::max(1.0f, plotFoot() - plotTop());
	return std::clamp((plotFoot() - y) / span, 0.0f, 1.0f);
}

void NoisePlot::place(const visage::MouseEvent& e) {
	control(nz::Param::Cutoff).setValue(std::log2(xToHz(e.position.x)));
	control(nz::Param::Resonance).setValue(static_cast<double>(yToResonance(e.position.y)));
}

void NoisePlot::recompute(const nz::Settings& s) {
	const double fs = rate();
	nz::Tilt tilt;
	tilt.design(s.colour, fs);
	nz::Svf svf;
	// THE KNOB'S CORNER: the picture has no note, so Key Track is drawn at C4, where it moves nothing.
	svf.design(s.filterType, std::exp2(static_cast<double>(s.cutoff)), s.resonance, fs);
	const bool noise = s.source == static_cast<int>(nz::Source::Noise);
	for (size_t i = 0; i < kPoints; ++i) {
		const double hz = hzAt(i);
		const double f = svf.gain(hz);
		const double t = noise ? tilt.gain(hz) : 1.0;
		filterDb_[i] = 20.0 * log10Of(f);
		spectrumDb_[i] = 20.0 * log10Of(f * t);
	}

	hitTimes_.clear();
	hitValues_.clear();
	if (!noise) {
		// THE DEVICE'S OWN GENERATOR, ON THE LEFT EAR'S STREAM, from the seed — the pattern a Stable note opens on.
		dsp::RandomEvents events;
		events.setSampleRate(kStripRate);
		events.seed(nz::eventStreamSeed(static_cast<uint64_t>(s.seed)));
		const dsp::RandomEventSettings e = nz::eventSettingsOf(s, 0.0);
		const auto frames = static_cast<int>(kStripSeconds * kStripRate);
		for (int i = 0; i < frames && hitTimes_.size() < kMaxHits; ++i) {
			const float v = events.next(e);
			if (v != 0.0f) {
				hitTimes_.push_back(static_cast<float>(static_cast<double>(i) / kStripRate));
				hitValues_.push_back(v);
			}
		}
	}
	drawn_ = s;
	drawnRate_ = fs;
	computed_ = true;
}

bool NoisePlot::update() {
	const nz::Settings s = settingsNow();
	const device::ModDisplay shown = control(nz::Param::Cutoff).display();
	const bool armed = shown.armed && shown.isDestination;
	const float amount = armed ? shown.amount : 0.0f;
	bool changed = false;
	if (!computed_ || !(s == drawn_) || rate() != drawnRate_) {
		recompute(s);
		changed = true;
	}
	if (armed != drawnArmed_ || amount != drawnAmount_) {
		drawnArmed_ = armed;
		drawnAmount_ = amount;
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

const char* NoisePlot::tooltipAt(visage::Point p) const {
	if (p.y > plotFoot()) {
		return "Two seconds of the clicks the current settings make";
	}
	return tooltipOf(&control(nz::Param::Cutoff));
}

void NoisePlot::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 5.0f);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 5.0f, 1.0f);

	const float top = plotTop();
	const float foot = plotFoot();
	// THE GRID: decades across, white's 0 dB across.
	canvas.setColor(withAlpha(kLine, 0.6f));
	for (const double hz : { 100.0, 1000.0, 10000.0 }) {
		canvas.fill(hzToX(hz), top, 1.0f, foot - top);
	}
	canvas.fill(kInset, dbToY(0.0), w - 2.0f * kInset, 1.0f);

	// WHAT IS ADDED — filled from the foot, the source's spectrum through the filter.
	canvas.setColor(withAlpha(kAccent, 0.22f));
	for (size_t i = 1; i < kPoints; ++i) {
		const float x0 = hzToX(hzAt(i - 1));
		const float x1 = hzToX(hzAt(i));
		const float y = std::min(dbToY(spectrumDb_[i - 1]), dbToY(spectrumDb_[i]));
		canvas.fill(x0, y, std::max(1.0f, x1 - x0), std::max(0.0f, foot - y));
	}
	canvas.setColor(kAccent);
	for (size_t i = 1; i < kPoints; ++i) {
		canvas.segment(hzToX(hzAt(i - 1)), dbToY(spectrumDb_[i - 1]), hzToX(hzAt(i)), dbToY(spectrumDb_[i]), 1.5f, true);
	}
	// THE FILTER ALONE, under Noise, so the tilt and the filter read apart.
	if (!impulses()) {
		canvas.setColor(withAlpha(kValue, 0.6f));
		for (size_t i = 1; i < kPoints; ++i) {
			canvas.segment(hzToX(hzAt(i - 1)), dbToY(filterDb_[i - 1]), hzToX(hzAt(i)), dbToY(filterDb_[i]), 1.0f, true);
		}
	}

	// THE MODULATION'S REACH on the corner, along the axis — `DisperserPlot`'s.
	const double cornerHz = std::exp2(static_cast<double>(drawn_.cutoff));
	const float markerX = hzToX(cornerHz);
	if (drawnArmed_ && drawnAmount_ != 0.0f) {
		const auto& desc = nz::kParamDescs[static_cast<size_t>(nz::Param::Cutoff)];
		const double octaves = static_cast<double>(drawnAmount_) * (desc.max - desc.min);
		const float xUp = hzToX(cornerHz * std::exp2(octaves));
		canvas.setColor(withAlpha(kModulation, 0.55f));
		canvas.fill(std::min(xUp, markerX), foot - 3.0f, std::abs(xUp - markerX), 3.0f);
	}
	// THE HANDLE: Cutoff across, Resonance up.
	const float handleY = resonanceToY(drawn_.resonance);
	canvas.setColor(withAlpha(kInk, 0.4f));
	canvas.fill(markerX - 0.5f, top, 1.0f, foot - top);
	canvas.setColor(kInk);
	canvas.circle(markerX - 3.5f, handleY - 3.5f, 7.0f);

	// THE STRIP OF HITS — sticks up for positive, down for negative, height the size.
	if (impulses()) {
		const float stripTop = foot + 3.0f;
		const float mid = stripTop + 0.5f * kStripHeight;
		canvas.setColor(withAlpha(kLine, 0.6f));
		canvas.fill(kInset, mid, w - 2.0f * kInset, 1.0f);
		const bool signedHits = drawn_.randomPolarity;
		const float reach = signedHits ? 0.5f * kStripHeight - 1.0f : kStripHeight - 2.0f;
		const float base = signedHits ? mid : stripTop + kStripHeight - 1.0f;
		canvas.setColor(kValue);
		for (size_t i = 0; i < hitTimes_.size(); ++i) {
			const float x = kInset + static_cast<float>(hitTimes_[i] / kStripSeconds) * (w - 2.0f * kInset);
			const float len = std::max(1.0f, std::abs(hitValues_[i]) * reach);
			const float y = hitValues_[i] >= 0.0f ? base - len : base;
			canvas.fill(x - 0.5f, y, 1.0f, len);
		}
	}

	// THE CAPTION — what is drawn, and the rate under Impulses.
	char text[48];
	canvas.setColor(kMuted);
	if (impulses()) {
		const double hits = nz::hitsPerSecond(drawn_, 0.0);
		if (hits >= 10.0) {
			std::snprintf(text, sizeof(text), "impulses · %.0f / s", hits);
		}
		else {
			std::snprintf(text, sizeof(text), "impulses · %.1f / s", hits);
		}
	}
	else {
		const double slope = nz::slopeDbPerOctave(drawn_.colour);
		std::snprintf(text, sizeof(text), "noise · %+.1f dB / oct", slope);
	}
	canvas.text(text, font(kKnobReadoutSize), visage::Font::kLeft, kInset + 2.0f, kInset - 1.0f, w * 0.8f, 11.0f);
}

} // namespace feathers::ui
