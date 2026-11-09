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

#include "plugin/ui/follower_plot.h"

#include "mod/follower/follower_core.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/plot_grid.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/trig_pad.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

FollowerPlot::FollowerPlot(backplane::DeviceHost& host, DeviceInstanceId id, const device::Descriptor& desc,
                           size_t threshold)
    : host_(host), device_(id), desc_(desc), thresholdIndex_(threshold) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		DeviceControl& t = thresholdControl();
		drag_ = {};
		drag_.active = true;
		drag_.startY = e.position.y;
		if (!e.isAltDown() && t.display().routable) {
			drag_.route = true;
			const float before = t.display().amount;
			drag_.anchor = before != 0.0f ? before : Knob::kRouteSeedAmount;
			t.beginAmountGesture();
			t.setAmount(drag_.anchor);
		}
		else {
			t.beginGesture();
			place(e);
		}
		redraw();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (!drag_.active) {
			return;
		}
		if (drag_.route) {
			const float travel = (drag_.startY - e.position.y) / Knob::kRouteDragRange;
			thresholdControl().setAmount(std::clamp(drag_.anchor + travel, -device::kMaxAmount, device::kMaxAmount));
		}
		else {
			place(e);
		}
		redraw();
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (!drag_.active) {
			return;
		}
		DeviceControl& t = thresholdControl();
		if (drag_.route) {
			const float amount = t.display().amount;
			if (std::abs(amount) < 0.05f && amount != 0.0f) {
				t.setAmount(0.0f);
			}
			t.endAmountGesture();
		}
		else {
			t.endGesture();
		}
		drag_ = {};
	};
	installTooltipHover(*this, *this);
}

DeviceControl& FollowerPlot::thresholdControl() const {
	if (threshold_control_ == nullptr) {
		threshold_control_ = std::make_unique<DeviceControl>(host_, device_, desc_, thresholdIndex_, mod_, history_);
	}
	return *threshold_control_;
}

void FollowerPlot::offeredControls(std::vector<Offered>& out) const {
	out.push_back({ &thresholdControl(), Road::Gesture });
}

void FollowerPlot::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	thresholdControl().setModulation(mod, history);
}

float FollowerPlot::levelToY(float v) const noexcept {
	const float top = kInset + 12.0f; // the pulse marks' row
	const float foot = height() - kInset;
	return foot - std::clamp(v, 0.0f, 1.0f) * std::max(0.0f, foot - top);
}

float FollowerPlot::yToLevel(float y) const noexcept {
	const float top = kInset + 12.0f;
	const float foot = height() - kInset;
	const float span = std::max(1.0f, foot - top);
	return std::clamp((foot - y) / span, 0.0f, 1.0f);
}

void FollowerPlot::place(const visage::MouseEvent& e) {
	const float drawn = yToLevel(e.position.y);
	const float value = bipolar_ ? std::clamp(2.0f * drawn - 1.0f, 0.0f, 1.0f) : drawn;
	thresholdControl().setValue(static_cast<double>(value));
}

void FollowerPlot::setBipolar(bool bipolar) {
	if (bipolar != bipolar_) {
		bipolar_ = bipolar;
		redraw();
	}
}

void FollowerPlot::push(float level, bool fired) noexcept {
	const bool same = count_ > 0 && !fired && level == levelAtForTest(0) && !firedAtForTest(0);
	still_ = same ? still_ + 1 : 1;
	level_[head_] = level;
	fired_[head_] = fired ? 1 : 0;
	head_ = (head_ + 1) % kCapacity;
	count_ = std::min(count_ + 1, kCapacity);
}

float FollowerPlot::levelAtForTest(size_t back) const noexcept {
	return back < count_ ? level_[(head_ + kCapacity - 1 - back) % kCapacity] : 0.0f;
}

bool FollowerPlot::firedAtForTest(size_t back) const noexcept {
	return back < count_ && fired_[(head_ + kCapacity - 1 - back) % kCapacity] != 0;
}

bool FollowerPlot::update() {
	float level = 0.0f;
	const bool haveLevel = host_.playheadLevelAt(device_, level);
	uint32_t fires = 0;
	const bool haveFires = host_.trigFiresAt(device_, fires);
	// A PULSE IS A CHANGE OF COUNT SINCE THE LAST FRAME — the first reading only sets where counting starts.
	const bool fired = haveFires && haveFires_ && fires != lastFires_;
	haveFires_ = haveFires;
	lastFires_ = fires;
	push(haveLevel && std::isfinite(level) ? level : 0.0f, fired);

	bool changed = false;
	const float t = std::clamp(host_.modulatedFieldAt(device_, thresholdIndex_), 0.0f, 1.0f);
	if (t != threshold_) {
		threshold_ = t;
		changed = true;
	}
	const device::ModDisplay shown = thresholdControl().display();
	const bool armed = shown.armed && shown.isDestination;
	const float amount = armed ? shown.amount : 0.0f;
	if (armed != drawnArmed_ || amount != drawnAmount_) {
		drawnArmed_ = armed;
		drawnAmount_ = amount;
		changed = true;
	}
	const auto shownWidth = static_cast<size_t>(std::max(0.0f, width() - 2.0f * kInset));
	if (still_ <= std::min(shownWidth, count_) + 1) {
		changed = true;
	}
	if (changed) {
		redraw();
	}
	return changed;
}

const char* FollowerPlot::tooltipAt(visage::Point) const {
	return tooltipOf(&thresholdControl());
}

void FollowerPlot::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 4.0f);
	canvas.setColor(withAlpha(kLine, 0.3f));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, 4.0f, 1.0f);
	plot::drawGrid(canvas, visage::Bounds { 0.0f, 0.0f, w, h });

	const float left = kInset;
	const float right = w - kInset;
	const float newestX = right - 3.0f;
	const size_t shown = std::min(count_, static_cast<size_t>(std::max(0.0f, right - left - 3.0f)));

	// THE ENVELOPE — filled faintly from its zero (the foot, or the middle when bipolar), then the line, newest at the
	// right.
	const float foot = levelToY(bipolar_ ? 0.5f : 0.0f);
	if (bipolar_) {
		canvas.setColor(withAlpha(kLine, 0.45f));
		canvas.fill(left, foot - 0.5f, right - left, 1.0f);
	}
	canvas.setColor(withAlpha(kValue, 0.18f));
	for (size_t back = 0; back < shown; ++back) {
		const float y = levelToY(levelAtForTest(back));
		canvas.fill(newestX - static_cast<float>(back), std::min(y, foot), 1.0f, std::abs(foot - y));
	}
	canvas.setColor(kValue);
	for (size_t back = 1; back < shown; ++back) {
		const float x0 = newestX - static_cast<float>(back);
		canvas.segment(x0, levelToY(levelAtForTest(back)), x0 + 1.0f, levelToY(levelAtForTest(back - 1)), 1.5f, true);
	}

	// THE PULSES — a mark in the top row for each one, in the Trig pad's colour.
	canvas.setColor(TrigPad::kLitColour);
	for (size_t back = 0; back < shown; ++back) {
		if (firedAtForTest(back)) {
			canvas.fill(newestX - static_cast<float>(back) - 0.5f, kInset + 2.0f, 2.0f, 8.0f);
		}
	}

	// THE RE-ARM LEVEL, faint and dotted: the line the envelope must fall through before it can fire again.
	const float rearmY = levelToY(drawnOf(threshold_ * mod::follower::kRearmRatio));
	canvas.setColor(withAlpha(TrigPad::kLitColour, 0.35f));
	for (float x = left; x < right; x += 6.0f) {
		canvas.fill(x, rearmY, 2.0f, 1.0f);
	}
	// THE MODULATION'S REACH on the threshold, beside the handle — the amount times the control's span (0..1).
	const float thresholdY = levelToY(drawnOf(threshold_));
	if (drawnArmed_ && drawnAmount_ != 0.0f) {
		const float yTo = levelToY(drawnOf(threshold_ + drawnAmount_));
		canvas.setColor(withAlpha(kModulation, 0.55f));
		canvas.fill(right - 3.0f, std::min(yTo, thresholdY), 3.0f, std::abs(yTo - thresholdY));
	}
	// THE THRESHOLD AND ITS HANDLE.
	canvas.setColor(TrigPad::kLitColour);
	canvas.fill(left, thresholdY - 0.5f, right - left, 1.0f);
	canvas.circle(right - 14.0f, thresholdY - 3.5f, 7.0f);

	// THE CAPTION: the threshold's value, at the line's OLD end, so the handle and the newest trace stay clear.
	char text[24];
	std::snprintf(text, sizeof(text), "%.0f%%", static_cast<double>(threshold_) * 100.0);
	canvas.setColor(kMuted);
	const float textY = thresholdY > kInset + 24.0f ? thresholdY - 13.0f : thresholdY + 2.0f;
	canvas.text(text, font(kKnobReadoutSize), visage::Font::kLeft, left + 3.0f, textY, 42.0f, 11.0f);
}

} // namespace feathers::ui
