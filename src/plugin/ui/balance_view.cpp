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
#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace bal = fx::balance;
namespace ow = fx::owltt;

namespace {

constexpr float kNameWidth = 74.0f;
constexpr float kReadoutWidth = 92.0f;
constexpr float kRowGap = 3.0f;
constexpr float kPlotShare = 0.46f;

/// "L 3.1", "R 2.0", "0.0": the imbalance as a hand reads it.
std::string imbalanceText(float db) {
	char out[24];
	if (std::fabs(db) < 0.05f) {
		std::snprintf(out, sizeof out, "0.0");
	}
	else {
		std::snprintf(out, sizeof out, "%c %.1f", db > 0.0f ? 'L' : 'R', static_cast<double>(std::fabs(db)));
	}
	return out;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE MODEL                                                                                       //
//------------------------------------------------------------------------------------------------//

std::array<std::string, 3> BalanceModel::bandNames() const {
	// HONEST ABOUT KEY TRACK: harmonic classes only while a note is tracked, else the comb's teeth in Hz (`bal::bandNames`).
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
// THE VIEW                                                                                        //
//------------------------------------------------------------------------------------------------//

BalanceView::BalanceView(backplane::DeviceHost& host, DeviceInstanceId device) : host_(host), device_(device) {
	installTooltipHover(*this, *this);
	refresh();
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
		}
		now.spacing1Hz = host_.readingAt(device_, static_cast<size_t>(bal::Reading::Spacing1Hz));
		now.noteHz = host_.readingAt(device_, static_cast<size_t>(bal::Reading::NoteHz));
	}
	const bool changed = !primed_ || !(now == model_);
	primed_ = true;
	model_ = now;
	return changed;
}

visage::Bounds BalanceView::plotBounds() const {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	return { 0.0f, 0.0f, w, std::floor(h * kPlotShare) };
}

visage::Bounds BalanceView::rowBounds(size_t band) const {
	const visage::Bounds plot = plotBounds();
	const float top = plot.bottom() + kRowGap * 2.0f;
	const float rows = static_cast<float>(model_.bandCount());
	const float rh = std::max(1.0f, (static_cast<float>(height()) - top - kRowGap * (rows - 1.0f)) / rows);
	return { 0.0f, top + static_cast<float>(band) * (rh + kRowGap), static_cast<float>(width()), rh };
}

visage::Bounds BalanceView::barBounds(size_t band) const {
	const visage::Bounds r = rowBounds(band);
	const float left = kNameWidth;
	const float right = std::max(left + 8.0f, r.width() - kReadoutWidth);
	return { left, r.y(), right - left, r.height() };
}

float BalanceView::meterX(size_t band, float db) const {
	const visage::Bounds bar = barBounds(band);
	const float t = std::clamp(db / kMeterSpanDb, -1.0f, 1.0f);
	// LEFT LOUDER IS TO THE LEFT.
	return bar.x() + bar.width() * (0.5f - 0.5f * t);
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

void BalanceView::draw(visage::Canvas& canvas) {
	const size_t nb = model_.bandCount();
	const visage::Bounds plot = plotBounds();
	const std::array<std::string, 3> names = model_.bandNames();

	// THE PLOT'S GUIDES: a line at each harmonic of the first spacing, the first one stronger.
	for (int k = 0; k <= static_cast<int>(kCurveSpan); ++k) {
		const float gx = std::min(plot.x() + plot.width() * static_cast<float>(k) / static_cast<float>(kCurveSpan), plot.right() - 1.0f);
		canvas.setColor(withAlpha(kMuted, k == 1 ? 0.30f : 0.12f));
		canvas.rectangle(gx, plot.y(), 1.0f, plot.height());
	}
	char caption[48];
	const double hz = model_.hasReadings && model_.spacing1Hz > 0.0f ? static_cast<double>(model_.spacing1Hz) : ow::combSpacingHz(model_.settings.spacing, false);
	if (model_.settings.keyTrack && model_.noteHz > 0.0f) {
		std::snprintf(caption, sizeof caption, "TEETH EVERY %.0f Hz (%s OF THE NOTE)", hz,
		              ow::combRatioText(ow::combRatioOfKnob(model_.settings.spacing, false)).c_str());
	}
	else {
		std::snprintf(caption, sizeof caption, "TEETH EVERY %.0f Hz", hz);
	}
	canvas.setColor(withAlpha(kMuted, 0.85f));
	canvas.text(caption, font(kKnobLabelSize), visage::Font::kLeft, 4.0f, 1.0f, plot.width() - 8.0f, 11.0f);

	// THE CURVES, overlaid on one axis: a faint fill and a line each (four samples to the pixel, so a sharp tooth is not stepped over).
	constexpr float kBottomPad = 3.0f;
	constexpr float kTopMargin = 13.0f;
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

	// ONE METER PER BAND.
	for (size_t b = 0; b < nb; ++b) {
		const visage::Bounds row = rowBounds(b);
		const visage::Bounds bar = barBounds(b);
		const uint32_t colour = bandColour(b, nb);
		const bool idle = !model_.hasReadings || model_.level[b] < 1.0e-4f;
		canvas.setColor(withAlpha(colour, idle ? 0.55f : 1.0f));
		canvas.text(names[b].c_str(), font(kKnobLabelSize), visage::Font::kLeft, 4.0f, row.y(), kNameWidth - 6.0f, row.height());
		// THE TRACK AND ITS CENTRE.
		canvas.setColor(withAlpha(kLine, 0.35f));
		canvas.rectangle(bar.x(), bar.y() + bar.height() * 0.5f - 0.5f, bar.width(), 1.0f);
		// THE SCALE: a tick every 3 dB, and which end is which side.
		for (const float tick : { -3.0f, 3.0f }) {
			canvas.setColor(withAlpha(kMuted, 0.35f));
			canvas.rectangle(meterX(b, tick) - 0.5f, bar.y() + bar.height() * 0.35f, 1.0f, bar.height() * 0.3f);
		}
		canvas.setColor(withAlpha(kMuted, 0.6f));
		canvas.text("L", font(kKnobLabelSize), visage::Font::kLeft, bar.x() + 1.0f, row.y(), 8.0f, row.height());
		canvas.text("R", font(kKnobLabelSize), visage::Font::kRight, bar.right() - 9.0f, row.y(), 8.0f, row.height());
		const float cx = meterX(b, 0.0f);
		canvas.setColor(withAlpha(kMuted, 0.7f));
		canvas.rectangle(cx - 0.5f, bar.y(), 1.0f, bar.height());
		if (model_.hasReadings) {
			// THE IMBALANCE THE DETECTOR MEASURES: the pale, full-height bar.
			const float bx = meterX(b, model_.before[b]);
			canvas.setColor(withAlpha(colour, 0.28f));
			canvas.rectangle(std::min(cx, bx), bar.y() + 1.0f, std::max(std::fabs(bx - cx), 1.0f), std::max(1.0f, bar.height() - 2.0f));
			// WHAT IS LEFT AFTER THE CORRECTION: the solid, slimmer one.
			const float ax = meterX(b, model_.after[b]);
			const float inset = bar.height() * 0.28f;
			canvas.setColor(withAlpha(colour, idle ? 0.4f : 0.95f));
			canvas.rectangle(std::min(cx, ax), bar.y() + inset, std::max(std::fabs(ax - cx), 1.0f), std::max(1.0f, bar.height() - 2.0f * inset));
		}
		char readout[64];
		if (model_.hasReadings && !idle) {
			std::snprintf(readout, sizeof readout, "%s > %s dB", imbalanceText(model_.before[b]).c_str(), imbalanceText(model_.after[b]).c_str());
		}
		else {
			std::snprintf(readout, sizeof readout, "-");
		}
		canvas.setColor(kValue);
		canvas.text(readout, font(kKnobLabelSize), visage::Font::kRight, bar.right() + 4.0f, row.y(), kReadoutWidth - 8.0f, row.height());
	}
}

const char* BalanceView::tooltipAt(visage::Point local) const {
	if (local.y <= plotBounds().bottom()) {
		return "The comb: each band's response against the harmonics of the first spacing. The bands are harmonic classes, so each curve sits on its own teeth";
	}
	return "Each band's left/right level difference, in dB (left louder to the left). The pale bar is what the detector measures; the solid one is what is left after the correction";
}

} // namespace feathers::ui
