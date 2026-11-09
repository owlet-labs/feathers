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

#include "plugin/ui/disperser_plot.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace feathers::ui {

namespace dsp = fx::disperser;

namespace {

/// Inset of the curve inside the frame: room for the handle's foot and, on the panel, the axis labels.
constexpr float kInsetCard = 4.0f;
constexpr float kInsetLarge = 8.0f;
constexpr float kFootHeight = 7.0f;
constexpr size_t kPointsCard = 96;
constexpr size_t kPointsLarge = 220;

double log10Of(double v) { return std::log10(std::max(v, 1e-9)); }

void formatMs(char* out, size_t size, double ms) {
	if (ms >= 1000.0) {
		std::snprintf(out, size, "%.0f s", ms / 1000.0);
	}
	else if (ms >= 1.0) {
		std::snprintf(out, size, "%.0f ms", ms);
	}
	else {
		std::snprintf(out, size, "%.1f ms", ms);
	}
}

void formatHz(char* out, size_t size, double hz) {
	if (hz >= 1000.0) {
		std::snprintf(out, size, "%.1f kHz", hz / 1000.0);
	}
	else {
		std::snprintf(out, size, "%.0f Hz", hz);
	}
}

} // namespace

DisperserPlot::DisperserPlot(backplane::DeviceHost& host, DeviceInstanceId id, bool large)
    : host_(host), device_(id), large_(large) {
	controls_.resize(static_cast<size_t>(dsp::Param::kCount));
	const size_t points = large ? kPointsLarge : kPointsCard;
	delayL_.assign(points, 0.0);
	delayR_.assign(points, 0.0);

	// THE AXIS IS THE FREQ CONTROL. A press places the centre under the pointer (or starts a route drag
	// with a source armed — `Knob`'s gesture, as OWLTT's graph takes it); a drag moves it.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const size_t index = draggedControl();
		DeviceControl& ctrl = control(index);
		drag_ = {};
		drag_.active = true;
		drag_.control = index;
		drag_.startY = e.position.y;
		if (!e.isAltDown() && ctrl.display().routable) {
			drag_.route = true;
			const float before = ctrl.display().amount;
			drag_.anchor = before != 0.0f ? before : Knob::kRouteSeedAmount;
			ctrl.beginAmountGesture();
			ctrl.setAmount(drag_.anchor);
		}
		else {
			ctrl.beginGesture();
			ctrl.setValue(valueForX(e.position.x));
			drag_.startQ = static_cast<double>(host_.fieldAt(device_, static_cast<size_t>(dsp::Param::QOffset)));
		}
		update();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!drag_.active) {
			return;
		}
		DeviceControl& ctrl = control(drag_.control);
		if (drag_.route) {
			const float travel = (drag_.startY - e.position.y) / Knob::kRouteDragRange;
			ctrl.setAmount(std::clamp(drag_.anchor + travel, -device::kMaxAmount, device::kMaxAmount));
		}
		else {
			ctrl.setValue(valueForX(e.position.x));
			// THE Q OFFSET, on the card and in the window: up sharpens. Relative to the press, past a dead zone,
			// so the X road above is the only thing a sideways drag writes.
			const float rise = drag_.startY - e.position.y;
			if (drag_.qGesture || std::abs(rise) > kQDeadZone) {
				DeviceControl& q = control(static_cast<size_t>(dsp::Param::QOffset));
				if (!drag_.qGesture) {
					drag_.qGesture = true;
					q.beginGesture();
				}
				const float past = std::max(std::abs(rise) - kQDeadZone, 0.0f) * (rise < 0.0f ? -1.0f : 1.0f);
				q.setValue(std::clamp(drag_.startQ + static_cast<double>(past / pixelsPerQOctave()),
				                      -dsp::kQOffsetOctaves, dsp::kQOffsetOctaves));
			}
		}
		update();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!drag_.active) {
			return;
		}
		DeviceControl& ctrl = control(drag_.control);
		if (drag_.route) {
			// A route dragged back to nothing is not a route at 0%, it is gone — OWLTT's graph's rule.
			const float amount = ctrl.display().amount;
			if (std::abs(amount) < 0.05f && amount != 0.0f) {
				ctrl.setAmount(0.0f);
			}
			ctrl.endAmountGesture();
		}
		else {
			ctrl.endGesture();
			if (drag_.qGesture) {
				control(static_cast<size_t>(dsp::Param::QOffset)).endGesture();
			}
		}
		drag_ = {};
	};
	// THE HOVER ROAD, LAST — `+=` beside the handlers above (`tooltip.h` says why it must come last).
	installTooltipHover(*this, *this);
}

DeviceControl& DisperserPlot::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, dsp::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

void DisperserPlot::offeredControls(std::vector<Offered>& out) const {
	// A SIDE ROAD for each: x is Freq, or Offset while a note is tracked; y is Q Offset.
	out.push_back({ &control(static_cast<size_t>(dsp::Param::Freq)), Road::Gesture });
	out.push_back({ &control(static_cast<size_t>(dsp::Param::Offset)), Road::Gesture });
	out.push_back({ &control(static_cast<size_t>(dsp::Param::QOffset)), Road::Gesture });
}

void DisperserPlot::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
}

size_t DisperserPlot::draggedControl() const noexcept {
	const bool tracking = host_.fieldAt(device_, static_cast<size_t>(dsp::Param::KeyTrack)) >= 0.5f;
	return static_cast<size_t>(tracking ? dsp::Param::Offset : dsp::Param::Freq);
}

dsp::Settings DisperserPlot::settingsNow() const {
	dsp::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return dsp::stateOf(row, present).settings;
}

double DisperserPlot::hzAt(size_t i) const noexcept {
	const double t = static_cast<double>(i) / static_cast<double>(std::max<size_t>(delayL_.size() - 1, 1));
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float DisperserPlot::hzToX(double hz) const noexcept {
	const float inset = large_ ? kInsetLarge : kInsetCard;
	const float w = std::max(1.0f, width() - 2.0f * inset);
	const double t = (log10Of(hz) - log10Of(kMinHz)) / (log10Of(kMaxHz) - log10Of(kMinHz));
	return inset + static_cast<float>(t) * w;
}

double DisperserPlot::xToHz(float x) const noexcept {
	const float inset = large_ ? kInsetLarge : kInsetCard;
	const float w = std::max(1.0f, width() - 2.0f * inset);
	const double t = std::clamp(static_cast<double>((x - inset) / w), 0.0, 1.0);
	return kMinHz * std::pow(kMaxHz / kMinHz, t);
}

float DisperserPlot::plotTop() const noexcept { return (large_ ? kInsetLarge : kInsetCard) + (large_ ? 12.0f : 10.0f); }

float DisperserPlot::plotFoot() const noexcept { return height() - (large_ ? kInsetLarge : kInsetCard) - kFootHeight; }

float DisperserPlot::pixelsPerQOctave() const noexcept {
	return std::max(1.0f, plotFoot() - plotTop()) / static_cast<float>(2.0 * dsp::kQOffsetOctaves);
}

float DisperserPlot::qOffsetToY(double octaves) const noexcept {
	const double o = std::clamp(octaves, -dsp::kQOffsetOctaves, dsp::kQOffsetOctaves);
	return 0.5f * (plotTop() + plotFoot()) - static_cast<float>(o) * pixelsPerQOctave();
}

float DisperserPlot::msToY(double ms) const noexcept {
	const float top = plotTop();
	const float bottom = plotFoot();
	const double lo = log10Of(axisTopMs_ / kAxisSpan);
	const double hi = log10Of(axisTopMs_);
	const double t = std::clamp((log10Of(ms) - lo) / (hi - lo), 0.0, 1.0);
	return bottom - static_cast<float>(t) * (bottom - top);
}

double DisperserPlot::valueForX(float x) const noexcept {
	// THE POINTER'S HERTZ, INTO THE UNITS OF THE CONTROL BEING DRAGGED — the inverse of what the marker is
	// drawn from (`freqIndexOf`), each road its own.
	const double hz = xToHz(x);
	if (draggedControl() == static_cast<size_t>(dsp::Param::Freq)) {
		return std::log2(hz);
	}
	if (model_.tracking) {
		const double noteHz = model_.markerHz / std::exp2(static_cast<double>(drawnSettings_.offsetOctaves));
		return std::log2(hz / noteHz);
	}
	// NO NOTE YET: the Offset knob IS the absolute knob — index 64 is 0 octaves.
	const double index = std::log2(hz) * dsp::kFreqIndexMax / dsp::kFreqOctaves;
	return (index - dsp::kOffsetCentreIndex) * dsp::kOffsetOctavesPerHalf / dsp::kOffsetCentreIndex;
}

void DisperserPlot::recompute(const dsp::Settings& s, bool haveNote, double note, const float* gains) {
	const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;
	model_ = dsp::responseModelOf(s, haveNote, note, rate);
	dsp::useStageGains(model_, gains);
	for (size_t i = 0; i < delayL_.size(); ++i) {
		const auto d = dsp::groupDelayMs(model_, hzAt(i));
		delayL_[i] = d[0];
		delayR_[i] = d[1];
	}
	snapAxis();
	drawnSettings_ = s;
	drawnHeard_ = haveNote;
	drawnNote_ = note;
	drawnRate_ = rate;
	computed_ = true;
	++recomputes_;
}

void DisperserPlot::snapAxis() {
	double peak = 0.0;
	for (size_t i = 0; i < delayL_.size(); ++i) {
		peak = std::max({ peak, delayL_[i], delayR_[i] });
	}
	peak = std::max(peak, 0.01);
	// A DECADE, WITH HYSTERESIS: up only past 95% of the current top, down only under 40% of the decade
	// below. A small drag moves the curve inside a fixed frame.
	const double decade = std::pow(10.0, std::ceil(std::log10(peak / 0.95)));
	if (!computed_ || peak > axisTopMs_ * 0.95 || peak < axisTopMs_ * 0.1 * 0.4) {
		axisTopMs_ = decade;
	}
}

bool DisperserPlot::update() {
	const dsp::Settings s = settingsNow();
	const size_t count = host_.readingCountAt(device_);
	const bool heard = count > static_cast<size_t>(dsp::Reading::NoteHeard)
	                   && host_.readingAt(device_, static_cast<size_t>(dsp::Reading::NoteHeard)) >= 0.5f;
	const double note =
	    heard ? static_cast<double>(host_.readingAt(device_, static_cast<size_t>(dsp::Reading::NoteSemitones))) : 60.0;
	const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;
	const device::ModDisplay shown = control(draggedControl()).display();
	const bool armed = shown.armed && shown.isDestination;
	const float amount = armed ? shown.amount : 0.0f;

	// THE ENGINE'S OWN EMPHASIS GAINS, when it has published them — see `Reading::StageGain0`.
	const bool hasGains = count >= static_cast<size_t>(dsp::Reading::kCount);
	std::array<float, dsp::kMaxStages> gains {};
	if (hasGains) {
		for (size_t i = 0; i < gains.size(); ++i) {
			gains[i] = host_.readingAt(device_, static_cast<size_t>(dsp::Reading::StageGain0) + i);
		}
	}

	bool changed = false;
	if (!computed_ || !(s == drawnSettings_) || heard != drawnHeard_ || note != drawnNote_ || rate != drawnRate_
	    || hasGains != drawnHasGains_ || (hasGains && gains != drawnGains_)) {
		recompute(s, heard, note, hasGains ? gains.data() : nullptr);
		drawnHasGains_ = hasGains;
		drawnGains_ = gains;
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

const char* DisperserPlot::tooltipAt(visage::Point p) const {
	// THE Q HANDLE SPEAKS FOR THE Q OFFSET.
	if (std::abs(p.x - hzToX(model_.markerHz)) < 10.0f
	    && std::abs(p.y - qOffsetToY(static_cast<double>(drawnSettings_.qOffsetOctaves))) < 10.0f) {
		return tooltipOf(&control(static_cast<size_t>(dsp::Param::QOffset)));
	}
	// THE AXIS IS A CONTROL, SO IT SPEAKS WITH THAT CONTROL'S DECLARED WORDS — Freq's, or Offset's when Key
	// Track is on. What the curve IS, the caption on its face says.
	return tooltipOf(&control(draggedControl()));
}

void DisperserPlot::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	const float inset = large_ ? kInsetLarge : kInsetCard;
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 5.0f);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 5.0f, 1.0f);

	// THE GRID: decades of frequency, decades of delay.
	canvas.setColor(withAlpha(kLine, 0.6f));
	for (const double hz : { 100.0, 1000.0, 10000.0 }) {
		const float x = hzToX(hz);
		canvas.fill(x, inset, 1.0f, h - 2.0f * inset - kFootHeight);
	}
	for (double ms = axisTopMs_ / kAxisSpan; ms <= axisTopMs_ * 1.001; ms *= 10.0) {
		canvas.fill(inset, msToY(ms), w - 2.0f * inset, 1.0f);
	}

	// THE SPREAD: a tick at every stage's hertz along the foot — on the panel its height is its Q.
	const float foot = h - inset - kFootHeight;
	canvas.setColor(withAlpha(kMuted, 0.8f));
	for (int i = 0; i < model_.layout.count; ++i) {
		const auto k = static_cast<size_t>(i);
		const float x = hzToX(0.5 * (static_cast<double>(model_.layout.hzL[k]) + model_.layout.hzR[k]));
		const float tall = large_ ? 2.0f + 5.0f * std::log2(model_.layout.q[k] / 0.5f) / std::log2(40.0f) : 3.0f;
		canvas.fill(x - 0.5f, foot + 1.0f, 1.0f, tall);
	}

	// THE CURVES: right first, left over it, so a mono setting shows one line.
	const auto curve = [&](const std::vector<double>& d, uint32_t colour, float thickness) {
		canvas.setColor(colour);
		for (size_t i = 1; i < d.size(); ++i) {
			canvas.segment(hzToX(hzAt(i - 1)), msToY(d[i - 1]), hzToX(hzAt(i)), msToY(d[i]), thickness, true);
		}
	};
	curve(delayR_, withAlpha(kValue, 0.85f), 1.2f);
	curve(delayL_, kAccent, 1.5f);

	// THE MODULATION REACH, along the axis, in the dragged control's own units.
	const size_t index = draggedControl();
	const device::ModDisplay shown = control(index).display();
	const float markerX = hzToX(model_.markerHz);
	if (shown.armed && shown.isDestination && shown.amount != 0.0f) {
		const auto& desc = dsp::kParamDescs[index];
		// Both controls are octaves per unit, so a reach of `amount * range` units is that many octaves.
		const double octaves = static_cast<double>(shown.amount) * (desc.max - desc.min);
		const float xUp = hzToX(model_.markerHz * std::exp2(octaves));
		const float xDown = shown.bipolar ? hzToX(model_.markerHz * std::exp2(-octaves)) : markerX;
		canvas.setColor(withAlpha(kModulation, 0.55f));
		canvas.fill(std::min(xUp, xDown), foot + 1.0f, std::abs(xUp - xDown), 3.0f);
	}

	// THE HANDLE: a line through the plot and a foot on the axis — the note's colour when it is tracked.
	canvas.setColor(withAlpha(model_.tracking ? kValue : kInk, 0.5f));
	canvas.fill(markerX - 0.5f, inset, 1.0f, foot - inset);
	canvas.setColor(model_.tracking ? kValue : kInk);
	// THE DOT RIDES AT THE Q OFFSET'S HEIGHT, on the card and in the window; the foot keeps a tick for the axis.
	const float handleY = qOffsetToY(static_cast<double>(drawnSettings_.qOffsetOctaves));
	canvas.fill(markerX - 3.0f, foot - 1.0f, 6.0f, 2.0f);
	canvas.circle(markerX - 3.5f, handleY - 3.5f, 7.0f);

	// THE CAPTIONS: the axis top, and on the panel the frequency labels and the centre.
	char text[32];
	canvas.setColor(kMuted);
	formatMs(text, sizeof(text), axisTopMs_);
	canvas.text(text, font(kKnobReadoutSize), visage::Font::kRight, w - inset - 60.0f, inset - 1.0f, 60.0f, 11.0f);
	canvas.text(large_ ? "group delay — stages at rest, echoes not drawn" : "group delay", font(kKnobReadoutSize),
	            visage::Font::kLeft, inset + 2.0f, inset - 1.0f, w * 0.7f, 11.0f);
	if (large_) {
		for (const double hz : { 100.0, 1000.0, 10000.0 }) {
			formatHz(text, sizeof(text), hz);
			canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, hzToX(hz) - 30.0f, foot - 12.0f, 60.0f,
			            11.0f);
		}
		formatHz(text, sizeof(text), model_.markerHz);
		canvas.setColor(model_.tracking ? kValue : kInk);
		canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, markerX - 30.0f, inset + 12.0f, 60.0f, 11.0f);
		// THE Q OFFSET BESIDE ITS DOT, on whichever side has the room, ON A PLATE — the curve runs through the
		// handle, so bare text crossed it (photographed at 0). The automod strip's plate: the plot's own fill.
		std::snprintf(text, sizeof(text), "Q %+.2f oct", static_cast<double>(drawnSettings_.qOffsetOctaves));
		const visage::Font face = font(kKnobReadoutSize);
		const std::string label(text);
		const float textW = face.stringWidth(std::u32string(label.begin(), label.end()));
		const bool left = markerX > w * 0.75f;
		const float textX = left ? markerX - 8.0f - textW : markerX + 8.0f;
		const float textY = handleY < inset + 30.0f ? handleY + 5.0f : handleY - 5.5f; // clear of the hertz
		canvas.setColor(withAlpha(kSurface, 0.9f));
		canvas.roundedRectangle(textX - 3.0f, textY, textW + 6.0f, 11.0f, 3.0f);
		canvas.setColor(model_.tracking ? kValue : kInk);
		canvas.text(text, face, visage::Font::kLeft, textX, textY, textW + 1.0f, 11.0f);
	}
}

} // namespace feathers::ui
