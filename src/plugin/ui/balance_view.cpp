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

#include "plugin/ui/balance_view.h"

#include "fx/owltt/comb_split.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace bal = fx::balance;
namespace ow = fx::owltt;

namespace {

constexpr float kRowGap = 3.0f;
constexpr float kSliderHeight = 13.0f;
constexpr float kSliderLabel = 50.0f;
constexpr float kSliderValue = 38.0f;
constexpr float kSpacingRailHeight = 9.0f;
constexpr float kGrabSlop = 8.0f;
constexpr float kInvSqrt2 = 0.7071067811865476f;

/// "L 3.1", "R 2.0", "0.0": the imbalance as a hand reads it.
std::string imbalanceText(float db) {
	char out[24];
	if (std::fabs(db) < 0.05f) {
		std::snprintf(out, sizeof out, "0.0");
	}
	else {
		std::snprintf(out, sizeof out, "%c%.1f", db > 0.0f ? 'L' : 'R', static_cast<double>(std::fabs(db)));
	}
	return out;
}

/// IS A PRESS ABOUT THE ROUTE OR THE VALUE: armed, allowed to reach this control, and Alt not held (`Knob::mappable`'s question).
bool routeGestureWanted(const device::Modulatable& ctrl, const visage::MouseEvent& e) {
	return !e.isAltDown() && ctrl.display().routable;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE MODEL                                                                                       //
//------------------------------------------------------------------------------------------------//

std::array<std::string, 3> BalanceModel::bandNames() const {
	return bal::bandNames(settings, noteHz > 0.0f);
}

double BalanceModel::ratio21() const {
	return ow::combSpacingHz(settings.spacing2, true) / std::max(static_cast<double>(settings.spacing), 1.0e-9);
}

uint32_t BalanceView::bandColour(size_t band, size_t bands) noexcept {
	constexpr uint32_t kColours[3] = { 0xff3FCB8F, 0xffC9D64A, 0xffD65FB6 };
	return kColours[bands == 2 && band == 1 ? 2 : std::min<size_t>(band, 2)];
}

//------------------------------------------------------------------------------------------------//
// GEOMETRY                                                                                        //
//------------------------------------------------------------------------------------------------//

visage::Bounds BalanceView::curveBounds() const {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	return { 0.0f, 0.0f, w, std::clamp(std::floor(h * 0.24f), 26.0f, 64.0f) };
}

visage::Bounds BalanceView::spacingRail() const {
	const visage::Bounds c = curveBounds();
	return { 8.0f, c.bottom() - kSpacingRailHeight + 1.0f, c.width() - 16.0f, 4.0f };
}

visage::Bounds BalanceView::sliderRail(size_t which) const {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const float top = h - 3.0f * kSliderHeight;
	return { kSliderLabel, top + static_cast<float>(which) * kSliderHeight + 4.0f, std::max(8.0f, w - kSliderLabel - kSliderValue - 4.0f), 5.0f };
}

visage::Bounds BalanceView::gonioBounds() const {
	const visage::Bounds c = curveBounds();
	const float top = c.bottom() + 4.0f;
	const float bottom = static_cast<float>(height()) - 3.0f * kSliderHeight - 4.0f;
	const float g = std::max(0.0f, std::min(bottom - top, static_cast<float>(width()) * 0.55f));
	return { 0.0f, top, g, g };
}

visage::Bounds BalanceView::rowBounds(size_t band) const {
	const visage::Bounds g = gonioBounds();
	const float x0 = g.right() + 10.0f;
	const float rows = static_cast<float>(model_.bandCount());
	const float rh = std::max(1.0f, (g.height() - kRowGap * (rows - 1.0f)) / rows);
	return { x0, g.y() + static_cast<float>(band) * (rh + kRowGap), std::max(1.0f, static_cast<float>(width()) - x0), rh };
}

visage::Bounds BalanceView::barBounds(size_t band) const {
	const visage::Bounds r = rowBounds(band);
	const float top = r.y() + 11.0f;
	return { r.x(), top, std::max(8.0f, r.width() - 34.0f), std::max(3.0f, r.height() - 12.0f) };
}

float BalanceView::meterX(size_t band, float db) const {
	const visage::Bounds bar = barBounds(band);
	const float t = std::clamp(db / kMeterSpanDb, -1.0f, 1.0f);
	return bar.x() + bar.width() * (0.5f - 0.5f * t); // LEFT LOUDER IS TO THE LEFT
}

device::Modulatable& BalanceView::controlOf(size_t which) const {
	return *controls_[which];
}

float BalanceView::handleX(size_t which) const {
	const visage::Bounds r = which == 3 ? spacingRail() : sliderRail(which);
	const device::ParamDesc& d = controlOf(which).desc();
	const double v = controlOf(which).value();
	double t = 0.0;
	if (which == 3) {
		t = std::log(std::max(v, d.min) / d.min) / std::log(d.max / d.min);
	}
	else {
		t = (v - d.min) / (d.max - d.min);
	}
	return r.x() + r.width() * static_cast<float>(std::clamp(t, 0.0, 1.0));
}

double BalanceView::valueAt(size_t which, float x) const {
	const visage::Bounds r = which == 3 ? spacingRail() : sliderRail(which);
	const device::ParamDesc& d = controlOf(which).desc();
	const double t = std::clamp(static_cast<double>((x - r.x()) / std::max(r.width(), 1.0f)), 0.0, 1.0);
	return which == 3 ? d.min * std::pow(d.max / d.min, t) : d.min + t * (d.max - d.min);
}

int BalanceView::handleAt(visage::Point p) const {
	for (size_t which = 0; which < 4; ++which) {
		const visage::Bounds r = which == 3 ? spacingRail() : sliderRail(which);
		if (p.x >= r.x() - kGrabSlop && p.x <= r.right() + kGrabSlop && p.y >= r.y() - 6.0f && p.y <= r.bottom() + 6.0f) {
			return static_cast<int>(which);
		}
	}
	return -1;
}

void BalanceView::offeredControls(std::vector<Offered>& out) const {
	for (device::Modulatable* c : controls_) {
		out.push_back({ c, Road::Gesture });
	}
}

//------------------------------------------------------------------------------------------------//
// THE VIEW                                                                                        //
//------------------------------------------------------------------------------------------------//

BalanceView::BalanceView(backplane::DeviceHost& host, DeviceInstanceId device, device::Modulatable& spacing, device::Modulatable& strength,
                         device::Modulatable& widen, device::Modulatable& pan)
    : host_(host), device_(device), controls_ { &strength, &widen, &pan, &spacing } {
	for (auto& t : traces_) {
		t.assign(kTracePoints, TracePoint {});
	}
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const int which = handleAt(e.position);
		if (which < 0) {
			return;
		}
		device::Modulatable& c = controlOf(static_cast<size_t>(which));
		if (e.repeatClickCount() >= 2) {
			(void)c.resetToDefault();
			redraw();
			return;
		}
		dragging_ = which;
		gestureOpen_ = false;
		routeDragging_ = false;
		if (routeGestureWanted(c, e)) {
			routeDragging_ = true;
			routeAnchorY_ = e.position.y;
			const float before = c.display().amount;
			routeAnchorAmount_ = before != 0.0f ? before : Knob::kRouteSeedAmount;
			c.beginAmountGesture();
			c.setAmount(routeAnchorAmount_);
		}
		else {
			// A PRESS ON THE HANDLE WRITES NOTHING (the drag moves it by how far the hand has gone); a press elsewhere on the rail takes the handle to the hand.
			grabOffset_ = std::fabs(e.position.x - handleX(static_cast<size_t>(which))) <= kGrabSlop ? static_cast<double>(handleX(static_cast<size_t>(which)) - e.position.x) : 0.0;
		}
		redraw();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (dragging_ < 0) {
			return;
		}
		device::Modulatable& c = controlOf(static_cast<size_t>(dragging_));
		if (routeDragging_) {
			c.setAmount(std::clamp(routeAnchorAmount_ + (routeAnchorY_ - e.position.y) / Knob::kRouteDragRange, -device::kMaxAmount, device::kMaxAmount));
		}
		else {
			if (!gestureOpen_) {
				c.beginGesture();
				gestureOpen_ = true;
			}
			double v = valueAt(static_cast<size_t>(dragging_), e.position.x + static_cast<float>(grabOffset_));
			// PAN SNAPS TO THE CENTRE, so the untouched place is easy to find.
			if (dragging_ == 2 && std::fabs(v) < 0.03) {
				v = 0.0;
			}
			c.setValue(v);
		}
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (dragging_ < 0) {
			return;
		}
		device::Modulatable& c = controlOf(static_cast<size_t>(dragging_));
		if (routeDragging_) {
			if (std::fabs(c.display().amount) < device::kAmountUnroutedBelow) {
				c.setAmount(0.0f);
			}
			c.endAmountGesture();
		}
		else if (gestureOpen_) {
			c.endGesture();
		}
		gestureOpen_ = false;
		routeDragging_ = false;
		dragging_ = -1;
		redraw();
	};
	// AFTER THE HANDLERS: the hover hook wraps the press road (a later assignment would drop it).
	installTooltipHover(*this, *this);
}

void BalanceView::absorbPoints() {
	// AGE EVERYTHING, THEN TAKE THE PAIRS THE DEVICE WROTE SINCE LAST TIME (at most its ring's sixteen).
	for (auto& t : traces_) {
		for (TracePoint& p : t) {
			if (p.age < (1 << 20)) {
				++p.age;
			}
		}
	}
	if (!model_.hasReadings) {
		lastSeq_ = -1;
		return;
	}
	const int seq = static_cast<int>(host_.readingAt(device_, static_cast<size_t>(bal::Reading::PointSeq)));
	size_t fresh = 0;
	if (lastSeq_ >= 0) {
		fresh = std::min<size_t>(static_cast<size_t>((seq - lastSeq_ + 65536) % 65536), bal::kRingPairs);
	}
	lastSeq_ = seq;
	float peak = 0.0f;
	for (size_t c = 0; c < bal::kPointChannels; ++c) {
		for (size_t k = bal::kRingPairs - fresh; k < bal::kRingPairs; ++k) {
			const float l = host_.readingAt(device_, bal::pointIndex(c, k, 0));
			const float r = host_.readingAt(device_, bal::pointIndex(c, k, 1));
			TracePoint& p = traces_[c][head_[c]];
			p.x = (r - l) * kInvSqrt2;
			p.y = (l + r) * kInvSqrt2;
			p.age = 0;
			head_[c] = (head_[c] + 1) % kTracePoints;
			peak = std::max({ peak, std::fabs(l), std::fabs(r) });
		}
	}
	// THE SCALE FOLLOWS THE PEAK UP AT ONCE AND DOWN SLOWLY.
	if (peak > scale_) {
		scale_ = peak;
	}
	else {
		scale_ += (std::max(peak, 0.02f) - scale_) * 0.03f;
	}
}

bool BalanceView::refresh() {
	BalanceModel now;
	bal::Row row {};
	const size_t present = readModulatedRow(host_, device_, bal::kDescriptor, row);
	now.settings = bal::stateOf(row, present);
	now.hasReadings = host_.readingCountAt(device_) >= static_cast<size_t>(bal::Reading::kCount);
	if (now.hasReadings) {
		for (size_t b = 0; b < 3; ++b) {
			now.before[b] = host_.readingAt(device_, static_cast<size_t>(bal::bandReading(b, 0)));
			now.after[b] = host_.readingAt(device_, static_cast<size_t>(bal::bandReading(b, 1)));
			now.level[b] = host_.readingAt(device_, static_cast<size_t>(bal::bandReading(b, 2)));
			now.correlation[b] = host_.readingAt(device_, static_cast<size_t>(bal::Reading::Corr0) + b);
		}
		now.correlation[3] = host_.readingAt(device_, static_cast<size_t>(bal::Reading::CorrAll));
		now.spacing1Hz = host_.readingAt(device_, static_cast<size_t>(bal::Reading::Spacing1Hz));
		now.noteHz = host_.readingAt(device_, static_cast<size_t>(bal::Reading::NoteHz));
	}
	const bool modelChanged = !primed_ || !(now == model_);
	primed_ = true;
	model_ = now;
	const bool wasVisible = anyVisible_;
	const int before = lastSeq_;
	absorbPoints();
	anyVisible_ = false;
	for (const auto& t : traces_) {
		for (const TracePoint& p : t) {
			anyVisible_ = anyVisible_ || p.age <= kLife;
		}
	}
	return modelChanged || wasVisible || anyVisible_ || before != lastSeq_;
}

std::vector<float> BalanceView::curve(size_t band, size_t points) const {
	std::vector<float> out(points, 0.0f);
	const int stages = model_.settings.sharp ? 2 : 1;
	// A TWO-BAND SPLIT IS THE FIRST COMB ALONE: the second one at a spacing so high it passes everything.
	const double r21 = model_.bandCount() == 2 ? 1.0e9 : model_.ratio21();
	for (size_t i = 0; i < points; ++i) {
		const double u = kCurveSpan * static_cast<double>(i) / static_cast<double>(std::max<size_t>(points - 1, 1));
		out[i] = ow::combResponse(u, r21, stages, 0.0f, 0.0f)[model_.curveOf(band)];
	}
	return out;
}

//------------------------------------------------------------------------------------------------//
// DRAWING                                                                                         //
//------------------------------------------------------------------------------------------------//

void BalanceView::drawGonio(visage::Canvas& canvas) {
	const visage::Bounds g = gonioBounds();
	if (g.width() < 20.0f) {
		return;
	}
	const size_t nb = model_.bandCount();
	const float cx = g.x() + g.width() * 0.5f;
	const float cy = g.y() + g.height() * 0.5f;
	const float radius = g.width() * 0.5f - 2.0f;
	canvas.setColor(withAlpha(kLine, 0.12f));
	canvas.rectangle(g.x(), g.y(), g.width(), g.height());
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.ring(cx - radius, cy - radius, radius * 2.0f, 1.0f);
	// THE AXES: mono is the vertical, and the diagonals are all-left and all-right.
	canvas.setColor(withAlpha(kMuted, 0.45f));
	canvas.segment(cx, cy - radius, cx, cy + radius, 1.0f, true);
	canvas.setColor(withAlpha(kMuted, 0.22f));
	canvas.segment(cx - radius, cy, cx + radius, cy, 1.0f, true);
	canvas.segment(cx - radius * 0.7071f, cy - radius * 0.7071f, cx + radius * 0.7071f, cy + radius * 0.7071f, 1.0f, true);
	canvas.segment(cx + radius * 0.7071f, cy - radius * 0.7071f, cx - radius * 0.7071f, cy + radius * 0.7071f, 1.0f, true);
	canvas.setColor(withAlpha(kMuted, 0.7f));
	canvas.text("L", font(kKnobLabelSize), visage::Font::kLeft, g.x() + 3.0f, g.y() + 1.0f, 10.0f, 10.0f);
	canvas.text("R", font(kKnobLabelSize), visage::Font::kRight, g.right() - 13.0f, g.y() + 1.0f, 10.0f, 10.0f);
	canvas.text("M", font(kKnobLabelSize), visage::Font::kCenter, cx - 5.0f, g.y() + 1.0f, 10.0f, 10.0f);

	const auto plot = [&](const TracePoint& p, uint32_t colour, float alpha) {
		if (p.age > kLife) {
			return;
		}
		const float life = 1.0f - static_cast<float>(p.age) / static_cast<float>(kLife + 1);
		const float x = cx + std::clamp(p.x / (scale_ * 0.8f), -1.0f, 1.0f) * radius * 0.92f;
		const float y = cy - std::clamp(p.y / (scale_ * 0.8f), -1.0f, 1.0f) * radius * 0.92f;
		canvas.setColor(withAlpha(colour, alpha * life));
		canvas.rectangle(x - 1.0f, y - 1.0f, 2.0f, 2.0f);
	};
	// THE GHOST OF THE WHOLE INPUT FIRST, faint and grey, then the bands added together in their colours.
	for (const TracePoint& p : traces_[bal::kInputChannel]) {
		plot(p, ghostColour(), 0.3f);
	}
	canvas.setBlendMode(visage::BlendMode::Add);
	for (size_t b = 0; b < nb; ++b) {
		for (const TracePoint& p : traces_[b]) {
			plot(p, bandColour(b, nb), 0.75f);
		}
	}
	canvas.setBlendMode(visage::BlendMode::Alpha);
}

void BalanceView::drawHandles(visage::Canvas& canvas) {
	static constexpr const char* kLabels[3] = { "STRENGTH", "WIDEN", "PAN" };
	for (size_t i = 0; i < 3; ++i) {
		const visage::Bounds r = sliderRail(i);
		const device::ParamDesc& d = controlOf(i).desc();
		const float hx = handleX(i);
		const float cy = r.y() + r.height() * 0.5f;
		canvas.setColor(withAlpha(kMuted, 0.85f));
		canvas.text(kLabels[i], font(kKnobLabelSize), visage::Font::kLeft, 4.0f, cy - 5.0f, kSliderLabel - 6.0f, 10.0f);
		canvas.setColor(withAlpha(kLine, 0.7f));
		canvas.rectangle(r.x(), cy - 1.0f, r.width(), 2.0f);
		const float from = i == 2 ? r.x() + r.width() * 0.5f : r.x();
		if (i == 2) {
			canvas.setColor(withAlpha(kMuted, 0.6f));
			canvas.rectangle(from - 0.5f, cy - 4.0f, 1.0f, 8.0f);
		}
		canvas.setColor(withAlpha(kAccent, 0.75f));
		canvas.rectangle(std::min(from, hx), cy - 1.0f, std::max(1.0f, std::fabs(hx - from)), 2.0f);
		canvas.setColor(kValue);
		canvas.circle(hx - 4.0f, cy - 4.0f, 8.0f);
		char text[24];
		const double v = controlOf(i).value();
		if (i == 2) {
			if (std::fabs(v) < 0.005) {
				std::snprintf(text, sizeof text, "C");
			}
			else {
				std::snprintf(text, sizeof text, "%c %.0f", v < 0.0 ? 'L' : 'R', std::fabs(v) * 100.0);
			}
		}
		else {
			std::snprintf(text, sizeof text, "%.0f%%", (v - d.min) / (d.max - d.min) * 100.0);
		}
		canvas.setColor(kValue);
		canvas.text(text, font(kKnobLabelSize), visage::Font::kRight, r.right() + 4.0f, cy - 5.0f, kSliderValue - 4.0f, 10.0f);
	}
}

void BalanceView::draw(visage::Canvas& canvas) {
	const size_t nb = model_.bandCount();
	const visage::Bounds cb = curveBounds();
	const visage::Bounds rail = spacingRail();
	const visage::Bounds plot { cb.x(), cb.y(), cb.width(), cb.height() - kSpacingRailHeight };
	const std::array<std::string, 3> names = model_.bandNames();

	// THE COMB: guides at the harmonics of the first spacing, then each band's curve in its colour.
	for (int k = 0; k <= static_cast<int>(kCurveSpan); ++k) {
		const float gx = std::min(plot.x() + plot.width() * static_cast<float>(k) / static_cast<float>(kCurveSpan), plot.right() - 1.0f);
		canvas.setColor(withAlpha(kMuted, k == 1 ? 0.30f : 0.12f));
		canvas.rectangle(gx, plot.y(), 1.0f, plot.height());
	}
	char caption[64];
	const double hz = model_.hasReadings && model_.spacing1Hz > 0.0f ? static_cast<double>(model_.spacing1Hz) : ow::combSpacingHz(model_.settings.spacing, false);
	if (model_.settings.keyTrack && model_.noteHz > 0.0f) {
		std::snprintf(caption, sizeof caption, "TEETH EVERY %.0f Hz (%s OF THE NOTE)", hz, ow::combRatioText(ow::combRatioOfKnob(model_.settings.spacing, false)).c_str());
	}
	else {
		std::snprintf(caption, sizeof caption, "TEETH EVERY %.0f Hz", hz);
	}
	canvas.setColor(withAlpha(kMuted, 0.85f));
	canvas.text(caption, font(kKnobLabelSize), visage::Font::kLeft, 4.0f, 1.0f, plot.width() - 8.0f, 11.0f);
	constexpr float kBottomPad = 2.0f;
	constexpr float kTopMargin = 12.0f;
	const float yBase = plot.bottom() - kBottomPad;
	const float span = std::max(1.0f, plot.height() - kTopMargin - kBottomPad);
	const auto points = static_cast<size_t>(std::max(2.0f, plot.width())) * 4;
	for (size_t b = 0; b < nb; ++b) {
		const uint32_t colour = bandColour(b, nb);
		const std::vector<float> c = curve(b, points);
		float prevX = plot.x();
		float prevY = yBase - std::clamp(c.front(), 0.0f, 1.0f) * span;
		for (size_t i = 0; i < c.size(); ++i) {
			const float x = plot.x() + plot.width() * static_cast<float>(i) / static_cast<float>(c.size() - 1);
			const float y = yBase - std::clamp(c[i], 0.0f, 1.0f) * span;
			if (i % 4 == 0) {
				float top = y;
				for (size_t k = i; k < std::min(i + 4, c.size()); ++k) {
					top = std::min(top, yBase - std::clamp(c[k], 0.0f, 1.0f) * span);
				}
				canvas.setColor(withAlpha(colour, 0.05f));
				canvas.rectangle(x, top, plot.width() * 4.0f / static_cast<float>(c.size()) + 0.5f, yBase - top);
			}
			if (i > 0) {
				canvas.setColor(withAlpha(colour, 0.8f));
				canvas.segment(prevX, prevY, x, y, 1.2f, true);
			}
			prevX = x;
			prevY = y;
		}
	}
	// THE SPACING HANDLE, on its own log rail under the curves.
	{
		const float hx = handleX(3);
		const float cy = rail.y() + rail.height() * 0.5f;
		canvas.setColor(withAlpha(kLine, 0.7f));
		canvas.rectangle(rail.x(), cy - 1.0f, rail.width(), 2.0f);
		canvas.setColor(kValue);
		canvas.circle(hx - 4.0f, cy - 4.0f, 8.0f);
	}

	drawGonio(canvas);

	// ONE METER AND CORRELATION PER BAND, beside the goniometer.
	for (size_t b = 0; b < nb; ++b) {
		const visage::Bounds row = rowBounds(b);
		const visage::Bounds bar = barBounds(b);
		const uint32_t colour = bandColour(b, nb);
		const bool idle = !model_.hasReadings || model_.level[b] < 1.0e-4f;
		canvas.setColor(withAlpha(colour, idle ? 0.55f : 1.0f));
		canvas.text(names[b].c_str(), font(kKnobLabelSize), visage::Font::kLeft, row.x(), row.y(), row.width() * 0.6f, 10.0f);
		char readout[48];
		if (model_.hasReadings && !idle) {
			if (row.width() < 150.0f) {
				std::snprintf(readout, sizeof readout, "%s", imbalanceText(model_.after[b]).c_str()); // narrow: what is left; the bars say the rest
			}
			else {
				std::snprintf(readout, sizeof readout, "%s>%s", imbalanceText(model_.before[b]).c_str(), imbalanceText(model_.after[b]).c_str());
			}
		}
		else {
			std::snprintf(readout, sizeof readout, "-");
		}
		canvas.setColor(kValue);
		canvas.text(readout, font(kKnobLabelSize), visage::Font::kRight, row.x() + row.width() * 0.45f, row.y(), row.width() * 0.55f, 10.0f);
		canvas.setColor(withAlpha(kLine, 0.35f));
		canvas.rectangle(bar.x(), bar.y() + bar.height() * 0.5f - 0.5f, bar.width(), 1.0f);
		for (const float tick : { -3.0f, 3.0f }) {
			canvas.setColor(withAlpha(kMuted, 0.35f));
			canvas.rectangle(meterX(b, tick) - 0.5f, bar.y() + bar.height() * 0.3f, 1.0f, bar.height() * 0.4f);
		}
		const float cx = meterX(b, 0.0f);
		canvas.setColor(withAlpha(kMuted, 0.7f));
		canvas.rectangle(cx - 0.5f, bar.y(), 1.0f, bar.height());
		if (model_.hasReadings) {
			const float bx = meterX(b, model_.before[b]);
			canvas.setColor(withAlpha(colour, 0.28f));
			canvas.rectangle(std::min(cx, bx), bar.y() + 1.0f, std::max(std::fabs(bx - cx), 1.0f), std::max(1.0f, bar.height() - 2.0f));
			const float ax = meterX(b, model_.after[b]);
			const float inset = bar.height() * 0.28f;
			canvas.setColor(withAlpha(colour, idle ? 0.4f : 0.95f));
			canvas.rectangle(std::min(cx, ax), bar.y() + inset, std::max(std::fabs(ax - cx), 1.0f), std::max(1.0f, bar.height() - 2.0f * inset));
		}
		char corr[16];
		std::snprintf(corr, sizeof corr, idle ? "-" : "%+.2f", static_cast<double>(model_.correlation[b]));
		canvas.setColor(kMuted);
		canvas.text(corr, font(kKnobLabelSize), visage::Font::kRight, bar.right() + 1.0f, bar.y() - 1.0f, row.right() - bar.right() - 1.0f, bar.height() + 2.0f);
	}

	drawHandles(canvas);
}

const char* BalanceView::tooltipAt(visage::Point local) const {
	const int which = handleAt(local);
	if (which >= 0) {
		return tooltipOf(&controlOf(static_cast<size_t>(which)));
	}
	if (local.y <= curveBounds().bottom()) {
		return "The comb: each band's response against the harmonics of the first spacing. The bands are harmonic classes, so each curve sits on its own teeth. "
		       "The handle under it is Spacing";
	}
	if (local.x <= gonioBounds().right()) {
		return "The goniometer: left against right, turned a quarter. Mono is straight up; a band leaning left leans left; width is spread. Each trace is a band's corrected, "
		       "widened output in its colour; the grey ghost is the whole input before any of it";
	}
	return "Each band's left/right level difference in dB (left louder to the left): the pale bar is what the detector measures, the solid one what is left after the correction. "
	       "The number at the end is the band's left/right correlation";
}

} // namespace feathers::ui
