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

#include "plugin/ui/master_meter.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace {

/// THE OVER AND WARN COLOURS — LOCAL, for the reason the bus utility's card gives beside its own
/// `kOverRed`: this palette has no danger token, and a `theme.h` entry is a promise to every surface.
/// The red is the card's, so the two surfaces that say "over" say it in one colour; the orange is the
/// warning at -6 LUFS. If a third surface wants either, that is the moment for a token.
constexpr uint32_t kOverRed = 0xffD9483B;
constexpr uint32_t kWarnOrange = 0xffE08A2A;

} // namespace

MasterMeter::Zone MasterMeter::zoneFor(float lufs) noexcept {
	if (lufs >= kRedLufs) {
		return Zone::Red;
	}
	if (lufs >= kOrangeLufs) {
		return Zone::Orange;
	}
	return Zone::Ink;
}

float MasterMeter::fillFractionFor(float lufs) noexcept {
	if (!std::isfinite(lufs)) {
		return 0.0f;
	}
	return std::clamp((lufs - kFloorLufs) / (0.0f - kFloorLufs), 0.0f, 1.0f);
}

bool MasterMeter::ledCoreLit() const noexcept {
	return readings_.clipAge < kCoreHoldSeconds;
}

ClipLedState MasterMeter::ledStateFor(const MasterMeterReadings& r) noexcept {
	return clipLedStateFor(r.rawPeak, r.clipAge < kCoreHoldSeconds);
}

float MasterMeter::ledGlow() const noexcept {
	const float age = readings_.clipAge;
	if (age >= fx::busutil::kClipAgeMax) {
		return 0.0f;
	}
	// EXPONENTIAL, NOT LINEAR — the bus utility card's reasoning: a decay reads as something that
	// HAPPENED, a linear fade as a light being turned down by a hand.
	return std::clamp(std::exp(-age / kGlowFallSeconds), 0.0f, 1.0f);
}

bool MasterMeter::update() {
	const MasterMeterReadings now = bridge_.masterMeter();
	const bool moved = now.momentaryLufs != readings_.momentaryLufs || now.rawPeak != readings_.rawPeak
	                   || now.clipAge != readings_.clipAge;
	readings_ = now;
	if (moved) {
		redraw();
	}
	return moved;
}

void MasterMeter::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const float cx = w * 0.5f;

	// THE LED, AT THE TOP: the glow first, under the core, so a fresh over is a bright disc in a halo
	// and an old one is the halo alone.
	const float ledY = kPadY + kLedDiameter * 0.5f;
	const float glow = ledGlow();
	if (glow > 0.0f) {
		canvas.setColor(withAlpha(kOverRed, 0.55f * glow));
		canvas.circle(cx - kLedDiameter, ledY - kLedDiameter, kLedDiameter * 2.0f);
	}
	// THE CORE'S THREE STATES (backlog ML): red at full scale, the near colour within 2 dB of it, the seated grey under.
	switch (ledStateFor(readings_)) {
	case ClipLedState::Clip:
		canvas.setColor(kOverRed);
		break;
	case ClipLedState::Near:
		canvas.setColor(kMeterNear);
		break;
	case ClipLedState::Normal:
		canvas.setColor(withAlpha(kLine, 0.9f));
		break;
	}
	canvas.circle(cx - kLedDiameter * 0.5f, ledY - kLedDiameter * 0.5f, kLedDiameter);

	// THE BAR, from under the LED to above the readout, filled from the bottom in the zone's colour.
	const float barTop = kPadY + kLedDiameter + kPadY;
	const float barBottom = h - kPadY - kReadoutHeight;
	const float barW = 10.0f;
	const float barX = cx - barW * 0.5f;
	const float barH = std::max(0.0f, barBottom - barTop);
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.roundedRectangle(barX, barTop, barW, barH, 3.0f);
	const float fill = fillFractionFor(readings_.momentaryLufs) * barH;
	uint32_t colour = kInk;
	switch (zoneFor(readings_.momentaryLufs)) {
	case Zone::Red:
		colour = kOverRed;
		break;
	case Zone::Orange:
		colour = kWarnOrange;
		break;
	case Zone::Ink:
		break;
	}
	if (fill > 0.0f) {
		canvas.setColor(colour);
		canvas.roundedRectangle(barX, barBottom - fill, barW, fill, 3.0f);
	}
	// THE THRESHOLDS, AS TICKS BESIDE THE BAR, so the colour change has a place on the scale.
	canvas.setColor(withAlpha(kMuted, 0.8f));
	for (const float mark : { kOrangeLufs, kRedLufs }) {
		const float y = barBottom - fillFractionFor(mark) * barH;
		canvas.fill(barX + barW + 2.0f, y - 0.5f, 4.0f, 1.0f);
	}

	// THE NUMBER, in the same colour: silence reads as a dash rather than a large negative.
	char printed[16] = {};
	if (readings_.momentaryLufs <= kFloorLufs) {
		std::snprintf(printed, sizeof(printed), "—");
	}
	else {
		std::snprintf(printed, sizeof(printed), "%.1f", static_cast<double>(readings_.momentaryLufs));
	}
	canvas.setColor(fill > 0.0f ? colour : kMuted);
	canvas.text(printed, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, barBottom + 2.0f, w, kReadoutHeight);
}

} // namespace feathers::ui
