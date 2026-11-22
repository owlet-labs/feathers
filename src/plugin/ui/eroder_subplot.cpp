/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/eroder_subplot.h"

#include "plugin/ui/modulated_row.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace ero = fx::eroder;

namespace {

constexpr const char* kHeadTip = "Where each side's read head has been behind the write, the last few seconds";
constexpr const char* kCarrierTip =
    "The multiplier Rust applies to the signal over two cycles: Source's mix of sine and noise, folded by Mod Fold, to one side by "
    "Rectify, with Bias of the signal let through whole. The noise is one fixed trace";
constexpr const char* kCurveTip = "What each Drive stage does to a level: input across, output up. A slot at zero is the straight line";
constexpr const char* kBarsTip =
    "What Tone and Character are turning right now: six quantities ride Tone and five ride Character, each swept at its own rate";
constexpr const char* kFeedbackTip =
    "How hard each loop's limiter is holding its return down, in decibels, and how near its threshold the return runs";

struct BarName {
	const char* full;
};
constexpr std::array<BarName, EroderPlot::kBars> kBarNames { { { "Delay" }, { "Fdbk" }, { "Clip" }, { "Fold" }, { "HP" }, { "Wob" },
                                                              { "Shake" }, { "Reso" }, { "Hold" }, { "Width" }, { "Pitch" } } };

void formatHz(char* out, size_t size, double hz) {
	if (hz >= 1000.0) {
		std::snprintf(out, size, "%.1f kHz", hz / 1000.0);
	}
	else {
		std::snprintf(out, size, "%.0f Hz", hz);
	}
}

void formatSeconds(char* out, size_t size, double seconds) {
	if (seconds >= 0.001) {
		std::snprintf(out, size, "%.2f ms", seconds * 1000.0);
	}
	else {
		std::snprintf(out, size, "%.3f ms", seconds * 1000.0);
	}
}

/// A DRIVE STAGE'S TRANSFER CURVE at `x`: the shaper's own function (Paunch-S and Paunch-H: the left channel for a mono input) at the stage's gain and make-up, blended by the stage's mix. A
/// stage that is not run, or a Type that is not a curve (Crush, Decimate, Slew, Lopside work in time), is the straight line.
float curveOutput(const ero::DriveSettings& s, float x) noexcept {
	if (ero::driveIsPaunchLaw(s.mode)) {
		// THE LEFT CHANNEL FOR A MONO INPUT (`l = r = x`, so the mid is `x`): `x + w s(x)`. The right is its mirror (`-f(-x)`).
		if (!s.driven) {
			return x;
		}
		float l = x;
		float r = x;
		ero::paunchLaw(s.mode, l, r, s.gain, s.shape);
		return x + s.mix * (l - x);
	}
	if (!s.driven || static_cast<int>(s.mode) > static_cast<int>(ero::DriveMode::Chonk)) {
		return x;
	}
	const float y = s.norm * ero::shapeDrive(s.mode, x * s.gain, s.shape, s.gain, s.curve);
	return x + s.mix * (y - x);
}

bool curveIsStraight(const ero::DriveSettings& s) noexcept {
	return !s.driven || s.mix <= 0.0f ||
	       (static_cast<int>(s.mode) > static_cast<int>(ero::DriveMode::Chonk) && !ero::driveIsPaunchLaw(s.mode));
}

} // namespace

EroderSubplot::EroderSubplot(backplane::DeviceHost& host, DeviceInstanceId id, const EroderPlot& plot)
    : host_(host), device_(id), plot_(plot) {
	// THE NOISE TRACE: a seeded stream through two one-poles, scaled to a peak of one. It stands for the band-limited noise the engine draws.
	uint32_t x = 0x1badf00du;
	float a = 0.0f;
	float b = 0.0f;
	float peak = 1.0e-6f;
	for (float& v : noise_) {
		x = x * 1664525u + 1013904223u;
		const float white = static_cast<float>(static_cast<int32_t>(x) >> 8) * (1.0f / 8388608.0f);
		a += (white - a) * 0.35f;
		b += (a - b) * 0.35f;
		v = b;
		peak = std::max(peak, std::abs(v));
	}
	for (float& v : noise_) {
		v /= peak;
	}
	// LAST — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

float EroderSubplot::fillFor(float reductionDb) noexcept { return std::clamp(reductionDb / kFloorDb, 0.0f, 1.0f); }

float EroderSubplot::meterFillWidthForTest(Which which) const noexcept {
	const Meter& m = which == Which::Inner ? inner_ : outer_;
	const Rect& bar = which == Which::Inner ? innerBar_ : outerBar_;
	return bar.w * fillFor(m.reductionDb);
}

static ero::DriveSettings slotSettings(const ero::Settings& s, EroderSubplot::Curve curve, double rate) {
	switch (curve) {
	case EroderSubplot::Curve::A:
		return ero::deriveDrive(s.slotA.amount, s.slotA.type, ero::driveCoordinate(s.slotA.type, s.slotA.shape, s.slotA.angle), rate, nullptr, s.slotA.mix, ero::DriveExtras { { s.slotA.hard, s.slotA.sharp, s.slotA.bias } });
	case EroderSubplot::Curve::B:
		return ero::deriveDrive(s.slotB.amount, s.slotB.type, ero::driveCoordinate(s.slotB.type, s.slotB.shape, s.slotB.angle), rate, nullptr, s.slotB.mix, ero::DriveExtras { { s.slotB.hard, s.slotB.sharp, s.slotB.bias } });
	default:
		return ero::deriveDrive(s.drive, s.driveMode, ero::driveCoordinate(s.driveMode, s.driveShape, s.driveAngle), rate, nullptr, 1.0f, ero::DriveExtras { { s.driveHard, s.driveSharp, s.driveBias } });
	}
}

float EroderSubplot::curveForTest(Curve curve, float input) const noexcept {
	return curveOutput(slotSettings(settings_, curve, rate_), input);
}

bool EroderSubplot::curveIsIdentityForTest(Curve curve) const noexcept {
	return curveIsStraight(slotSettings(settings_, curve, rate_));
}

void EroderSubplot::layout() {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const bool comb = settings_.mode == ero::Mode::Comb;
	if (comb) {
		// THE BARS TAKE THE WHOLE WIDTH under a slim strip that carries the feedback meters.
		right_ = { kInset, kInset, std::max(0.0f, w - 2.0f * kInset), kCombStripHeight };
		left_ = { kInset, kInset + kCombStripHeight + 6.0f, std::max(0.0f, w - 2.0f * kInset),
			      std::max(0.0f, h - 2.0f * kInset - kCombStripHeight - 6.0f) };
		bars_ = left_;
	}
	else {
		const float panelW = std::max(0.0f, (w - 3.0f * kInset) * 0.5f);
		const float panelH = std::max(0.0f, h - 2.0f * kInset);
		left_ = { kInset, kInset, panelW, panelH };
		right_ = { 2.0f * kInset + panelW, kInset, panelW, panelH };
		bars_ = {};
	}
	curveArea_ = { left_.x + 4.0f, left_.y + kCaptionHeight, std::max(0.0f, left_.w - 8.0f), std::max(0.0f, left_.h - kCaptionHeight - 4.0f) };
	// THE METERS' BARS. In a panel the two stand one over the other; in the Comb's strip, side by side.
	constexpr float label = 28.0f;
	constexpr float value = 46.0f;
	if (comb) {
		const float half = right_.w * 0.5f;
		const float barW = std::max(0.0f, half - label - value - 8.0f);
		innerBar_ = { right_.x + label, right_.y + kCaptionHeight + 3.0f, barW, 7.0f };
		outerBar_ = { right_.x + half + label, right_.y + kCaptionHeight + 3.0f, barW, 7.0f };
	}
	else {
		const float rowH = std::max(0.0f, (right_.h - kCaptionHeight) * 0.5f);
		const float barW = std::max(0.0f, right_.w - label - value - 8.0f);
		innerBar_ = { right_.x + label, right_.y + kCaptionHeight + rowH * 0.5f - 6.0f, barW, 7.0f };
		outerBar_ = { right_.x + label, right_.y + kCaptionHeight + rowH * 1.5f - 6.0f, barW, 7.0f };
	}
}

bool EroderSubplot::update() {
	ero::Row row {};
	const size_t present = readModulatedRow(host_, device_, ero::kDescriptor, row);
	const size_t count = host_.readingCountAt(device_);
	const bool publishing = count >= ero::kReadingCount;
	const auto at = [&](ero::Reading r) { return host_.readingAt(device_, static_cast<size_t>(r)); };
	const double rate = host_.sampleRate() > 0.0 ? host_.sampleRate() : 48000.0;
	bool changed = false;
	if (!computed_ || row != row_ || rate != rate_) {
		const bool modeMoved = !computed_ || ero::stateOf(row, present).settings.mode != settings_.mode;
		row_ = row;
		settings_ = ero::stateOf(row, present).settings;
		rate_ = rate;
		computed_ = true;
		++recomputes_;
		if (modeMoved) {
			layout();
		}
		changed = true;
	}
	if (publishing) {
		const Head now { at(ero::Reading::HeadNearL), at(ero::Reading::HeadFarL), at(ero::Reading::HeadNearR),
			             at(ero::Reading::HeadFarR) };
		if (!(now == lastHead_)) {
			lastHead_ = now;
			head_[headNext_] = now;
			headNext_ = (headNext_ + 1) % head_.size();
			headCount_ = std::min(headCount_ + 1, head_.size());
			changed = true;
		}
	}
	const auto meter = [&](ero::Reading gain, ero::Reading level, bool dim) {
		Meter m;
		if (publishing) {
			const float g = std::clamp(at(gain), 1.0e-4f, 1.0f);
			m.reductionDb = g >= 1.0f ? 0.0f : 20.0f * std::log10(g);
			m.level = std::clamp(at(level), 0.0f, 1.0f);
		}
		m.dim = dim;
		return m;
	};
	const bool comb = settings_.mode == ero::Mode::Comb;
	const Meter innerNow = meter(ero::Reading::LimiterInnerGain, ero::Reading::LimiterInnerLevel, !comb && settings_.resonance == 0.0f);
	const Meter outerNow = meter(ero::Reading::LimiterOuterGain, ero::Reading::LimiterOuterLevel, settings_.loop <= 0.0f);
	if (!(innerNow == inner_) || !(outerNow == outer_)) {
		inner_ = innerNow;
		outer_ = outerNow;
		changed = true;
	}
	// THE COMB'S BARS ARE THE PLOT'S (the one place they are computed): a change there is a change here.
	if (comb && plot_.barValues() != lastBars_) {
		lastBars_ = plot_.barValues();
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

const char* EroderSubplot::tooltipAt(visage::Point p) const {
	if (static_cast<float>(p.x) >= right_.x && static_cast<float>(p.x) < right_.x + right_.w && static_cast<float>(p.y) >= right_.y
	    && static_cast<float>(p.y) < right_.y + right_.h) {
		return kFeedbackTip;
	}
	switch (settings_.mode) {
	case ero::Mode::Comb: return kBarsTip;
	case ero::Mode::Etch: return kCurveTip;
	case ero::Mode::Rust: return kCarrierTip;
	default: return kHeadTip;
	}
}

void EroderSubplot::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 5.0f);
	canvas.setColor(kLine);
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 5.0f, 1.0f);
	if (!computed_) {
		return;
	}
	const visage::Font& face = font(kKnobReadoutSize);
	canvas.setColor(kMuted);
	switch (settings_.mode) {
	case ero::Mode::Comb:
		canvas.text("TONE", face, visage::Font::kLeft, left_.x + 2.0f, left_.y, 60.0f, 11.0f);
		drawBars(canvas);
		drawFeedback(canvas, right_, true);
		break;
	case ero::Mode::Etch:
		canvas.text("TRANSFER", face, visage::Font::kLeft, left_.x + 2.0f, left_.y, 90.0f, 11.0f);
		drawCurves(canvas);
		drawFeedback(canvas, right_, false);
		break;
	case ero::Mode::Rust:
		canvas.text("CARRIER", face, visage::Font::kLeft, left_.x + 2.0f, left_.y, 90.0f, 11.0f);
		drawCarrier(canvas);
		drawFeedback(canvas, right_, false);
		break;
	default:
		canvas.text("READ HEAD", face, visage::Font::kLeft, left_.x + 2.0f, left_.y, 90.0f, 11.0f);
		drawHead(canvas);
		drawFeedback(canvas, right_, false);
		break;
	}
	// A HAIRLINE BETWEEN THE TWO PANELS, where they are two.
	if (settings_.mode != ero::Mode::Comb) {
		canvas.setColor(withAlpha(kLine, 0.8f));
		canvas.fill(left_.x + left_.w + kInset * 0.5f - 0.5f, kInset, 1.0f, h - 2.0f * kInset);
	}
}

void EroderSubplot::drawHead(visage::Canvas& canvas) {
	const visage::Font& face = font(kKnobReadoutSize);
	const Rect& p = left_;
	if (headCount_ == 0) {
		canvas.setColor(kMuted);
		// BEFORE ANY AUDIO ONLY: the head has nothing to show until the device has run.
		canvas.text("no signal yet", face, visage::Font::kLeft, p.x + 2.0f, p.y + kCaptionHeight + 4.0f, 140.0f, 11.0f);
		return;
	}
	float farthest = 1.0e-6f;
	for (size_t i = 0; i < headCount_; ++i) {
		farthest = std::max({ farthest, head_[i].farL, head_[i].farR });
	}
	char text[32];
	formatSeconds(text, sizeof(text), static_cast<double>(farthest));
	canvas.setColor(kMuted);
	canvas.text(text, face, visage::Font::kRight, p.x + p.w - 72.0f, p.y, 70.0f, 11.0f);
	// TWO LANES, left over right; the newest column at the right edge.
	const float laneTop = p.y + kCaptionHeight;
	const float laneH = std::max(1.0f, (p.h - kCaptionHeight - 4.0f) * 0.5f - 1.0f);
	const float column = p.w / static_cast<float>(head_.size());
	for (size_t n = 0; n < headCount_; ++n) {
		const Head& hd = head_[(headNext_ + head_.size() - 1 - n) % head_.size()];
		const float x = p.x + p.w - column * static_cast<float>(n + 1);
		const auto lane = [&](float nearAt, float farAt, float y, uint32_t colour) {
			const float lo = y + laneH * (1.0f - std::clamp(farAt / farthest, 0.0f, 1.0f));
			const float hi = y + laneH * (1.0f - std::clamp(nearAt / farthest, 0.0f, 1.0f));
			canvas.setColor(colour);
			canvas.fill(x, lo, std::max(1.0f, column), std::max(1.0f, hi - lo));
		};
		lane(hd.nearL, hd.farL, laneTop, kModulationL);
		lane(hd.nearR, hd.farR, laneTop + laneH + 2.0f, kModulationR);
	}
}

void EroderSubplot::drawCurves(visage::Canvas& canvas) {
	const Rect& a = curveArea_;
	if (a.w <= 4.0f || a.h <= 4.0f) {
		return;
	}
	// THE AXES: input across (-1..1), output up (-1..1), the zero lines faint and the identity line dim.
	const auto px = [&](float x) { return a.x + (std::clamp(x, -1.0f, 1.0f) * 0.5f + 0.5f) * a.w; };
	const auto py = [&](float y) { return a.y + (1.0f - (std::clamp(y, -1.0f, 1.0f) * 0.5f + 0.5f)) * a.h; };
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.fill(px(0.0f) - 0.5f, a.y, 1.0f, a.h);
	canvas.fill(a.x, py(0.0f) - 0.5f, a.w, 1.0f);
	canvas.setColor(withAlpha(kMuted, 0.3f));
	canvas.segment(px(-1.0f), py(-1.0f), px(1.0f), py(1.0f), 1.0f, true);
	struct Line {
		Curve curve;
		uint32_t colour;
		float alpha;
		const char* name;
	};
	const Line lines[] = { { Curve::Drive, kMuted, 0.6f, "drive" }, { Curve::A, kAccent, 0.95f, "A" }, { Curve::B, kValue, 0.95f, "B" } };
	float labelY = a.y + 1.0f;
	for (const Line& line : lines) {
		const ero::DriveSettings s = slotSettings(settings_, line.curve, rate_);
		const bool straight = curveIsStraight(s);
		canvas.setColor(withAlpha(line.colour, straight ? 0.3f : line.alpha));
		float prevX = 0.0f;
		float prevY = 0.0f;
		for (size_t i = 0; i < kCurvePoints; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kCurvePoints - 1);
			const float qx = px(x);
			const float qy = py(curveOutput(s, x));
			if (i > 0 && !straight) {
				canvas.segment(prevX, prevY, qx, qy, 1.4f, true);
			}
			prevX = qx;
			prevY = qy;
		}
		canvas.setColor(withAlpha(line.colour, straight ? 0.4f : 1.0f));
		canvas.text(line.name, font(kKnobReadoutSize), visage::Font::kLeft, a.x + a.w - 40.0f, labelY, 38.0f, 11.0f);
		labelY += 11.0f;
	}
}

float EroderSubplot::carrierAt(float phase) const noexcept {
	const float p = std::clamp(phase, 0.0f, 1.0f);
	float sine = std::sin(2.0f * 3.14159265358979f * kCarrierCycles * p);
	if (settings_.modFold > 0.0f) {
		sine = ero::triFold(sine, 1.0f + ero::kModFoldSpan * settings_.modFold);
	}
	const float at = p * static_cast<float>(kCarrierPoints - 1);
	const auto i = static_cast<size_t>(at);
	const size_t j = std::min(i + 1, kCarrierPoints - 1);
	const float noise = noise_[i] + (noise_[j] - noise_[i]) * (at - static_cast<float>(i));
	const float source = std::clamp(settings_.source, 0.0f, 1.0f);
	const float mod = std::clamp(sine * (1.0f - source) + noise * source, -1.0f, 1.0f);
	const float carrier = mod + settings_.rustRectify * (std::abs(mod) - mod);
	return settings_.rustBias + (1.0f - settings_.rustBias) * carrier;
}

void EroderSubplot::drawCarrier(visage::Canvas& canvas) {
	const Rect& a = curveArea_;
	if (a.w <= 4.0f || a.h <= 4.0f) {
		return;
	}
	const auto py = [&](float y) { return a.y + (1.0f - (std::clamp(y, -1.0f, 1.0f) * 0.5f + 0.5f)) * a.h; };
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.fill(a.x, py(0.0f) - 0.5f, a.w, 1.0f);
	canvas.setColor(withAlpha(kMuted, 0.3f));
	canvas.fill(a.x, py(1.0f), a.w, 1.0f);
	canvas.setColor(kValue);
	float prevX = 0.0f;
	float prevY = 0.0f;
	for (size_t i = 0; i < kCarrierPoints; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kCarrierPoints - 1);
		const float x = a.x + t * a.w;
		const float y = py(carrierAt(t));
		if (i > 0) {
			canvas.segment(prevX, prevY, x, y, 1.4f, true);
		}
		prevX = x;
		prevY = y;
	}
	if (settings_.source > 0.0f) {
		canvas.setColor(kMuted);
		canvas.text("noise: one fixed trace", font(kKnobReadoutSize), visage::Font::kRight, a.x + a.w - 120.0f, left_.y, 118.0f, 11.0f);
	}
}

void EroderSubplot::drawBars(visage::Canvas& canvas) {
	const Rect& p = bars_;
	if (p.w <= 40.0f || p.h <= 40.0f) {
		return;
	}
	const auto& bars = plot_.barValues();
	const ero::Derived& c = plot_.combDerived();
	const float labels = 26.0f;
	const float top = p.y + 12.0f;
	const float foot = p.y + p.h - labels;
	const float groupGap = 18.0f;
	const float cell = (p.w - groupGap) / static_cast<float>(EroderPlot::kBars);
	const float barW = std::max(2.0f, cell * 0.5f);
	const visage::Font& face = font(kKnobReadoutSize);
	canvas.setColor(kMuted);
	canvas.text("CHARACTER", face, visage::Font::kLeft, p.x + 2.0f + cell * static_cast<float>(EroderPlot::kToneBars) + groupGap, p.y,
	            90.0f, 11.0f);
	char text[32];
	for (size_t i = 0; i < EroderPlot::kBars; ++i) {
		const float x = p.x + cell * static_cast<float>(i) + (i >= EroderPlot::kToneBars ? groupGap : 0.0f);
		const float barX = x + (cell - barW) * 0.5f;
		const float tall = std::max(0.0f, foot - top);
		canvas.setColor(withAlpha(kLine, 0.8f));
		canvas.fill(barX, top, barW, tall);
		canvas.setColor(i < EroderPlot::kToneBars ? kAccent : kValue);
		const float fill = tall * std::clamp(bars[i], 0.0f, 1.0f);
		canvas.fill(barX, foot - fill, barW, fill);
		canvas.setColor(kMuted);
		canvas.text(kBarNames[i].full, face, visage::Font::kCenter, x - 8.0f, foot + 1.0f, cell + 16.0f, 11.0f);
		switch (i) {
		case 0: formatSeconds(text, sizeof(text), c.baseDelaySeconds); break;
		case 1: std::snprintf(text, sizeof(text), "%.0f%%", c.feedback * 100.0); break;
		case 2: std::snprintf(text, sizeof(text), "x%.1f", c.clipIntensity > 0.0 ? c.clipDrive : 0.0); break;
		case 3: std::snprintf(text, sizeof(text), "x%.1f", c.foldIntensity > 0.0 ? c.foldDrive : 0.0); break;
		case 4: formatHz(text, sizeof(text), c.hpfHz); break;
		case 5: std::snprintf(text, sizeof(text), "%.0f%%", c.triangleDepth * 100.0); break;
		case 6: std::snprintf(text, sizeof(text), "%.0f%%", c.depth * 100.0); break;
		case 7: std::snprintf(text, sizeof(text), "%.0f%%", c.resonance * 100.0); break;
		case 8: std::snprintf(text, sizeof(text), "%.0f%%", c.shBlend * 100.0); break;
		case 9: std::snprintf(text, sizeof(text), "%.0f%%", c.width * 100.0); break;
		default: formatHz(text, sizeof(text), c.triangleHz); break;
		}
		canvas.setColor(kInk);
		canvas.text(text, face, visage::Font::kCenter, x - 8.0f, foot + 13.0f, cell + 16.0f, 11.0f);
	}
}

void EroderSubplot::drawMeter(visage::Canvas& canvas, const char* label, const Meter& meter, const Rect& bar, bool strip) {
	(void)strip;
	const visage::Font& face = font(kKnobReadoutSize);
	const float dim = meter.dim ? 0.35f : 1.0f;
	canvas.setColor(withAlpha(kMuted, dim));
	canvas.text(label, face, visage::Font::kLeft, bar.x - 28.0f, bar.y - 2.0f, 26.0f, 11.0f);
	canvas.setColor(withAlpha(kLine, 0.9f * dim));
	canvas.fill(bar.x, bar.y, bar.w, bar.h);
	const float fill = bar.w * fillFor(meter.reductionDb);
	canvas.setColor(withAlpha(kAccent, dim));
	canvas.fill(bar.x, bar.y, fill, bar.h);
	// THE RETURN'S LEVEL, thin under it.
	canvas.setColor(withAlpha(kLine, 0.6f * dim));
	canvas.fill(bar.x, bar.y + bar.h + 2.0f, bar.w, 2.0f);
	canvas.setColor(withAlpha(kValue, dim));
	canvas.fill(bar.x, bar.y + bar.h + 2.0f, bar.w * meter.level, 2.0f);
	char text[24];
	if (meter.reductionDb == 0.0f) {
		std::snprintf(text, sizeof(text), "0 dB");
	}
	else {
		std::snprintf(text, sizeof(text), "%.1f dB", static_cast<double>(meter.reductionDb));
	}
	canvas.setColor(withAlpha(meter.reductionDb < 0.0f ? kInk : kMuted, dim));
	canvas.text(text, face, visage::Font::kLeft, bar.x + bar.w + 4.0f, bar.y - 2.0f, 46.0f, 11.0f);
}

void EroderSubplot::drawFeedback(visage::Canvas& canvas, const Rect& panel, bool strip) {
	canvas.setColor(kMuted);
	canvas.text("FEEDBACK", font(kKnobReadoutSize), visage::Font::kLeft, panel.x + 2.0f, panel.y, 90.0f, 11.0f);
	drawMeter(canvas, "reso", inner_, innerBar_, strip);
	drawMeter(canvas, "loop", outer_, outerBar_, strip);
}

} // namespace feathers::ui
