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

#include "plugin/ui/delay_card.h"

#include "fx/delay/delay_lanes.h"
#include "plugin/ui/delay_strip.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace dl = fx::delay;

/// THE CHIP ON THE STRIP THAT OPENS THE WINDOW — a child frame, so it takes its own press before the card's.
class DelayCard::LaneChip final : public visage::Frame, public TooltipSource {
public:
	std::string text;
	std::function<void()> onPress;
	/// NOT A CONTROL: it opens the window, and says so.
	const char* tooltipAt(visage::Point) const override { return kLaneChipTip; }
	void offeredControls(std::vector<Offered>&) const override {}
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(withAlpha(kSurface, 0.85f));
		canvas.roundedRectangle(0.0f, 0.0f, width(), height(), 3.0f);
		canvas.setColor(kMuted);
		canvas.text(text, font(kKnobReadoutSize), visage::Font::kCenter, 0.0f, 0.0f, width(), height());
	}
	void mouseDown(const visage::MouseEvent&) override {
		if (onPress) {
			onPress();
		}
	}
};

DelayCard::DelayCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier)
    : GenericCard(host, id, dl::kDescriptor, notifier), chip_(std::make_unique<LaneChip>()) {
	chip_->onPress = [this] {
		if (onExpand) {
			onExpand();
		}
	};
	chip_->text = laneChipText();
	installTooltipHover(*chip_, *chip_);
	addChild(chip_.get());
	placeChip();
}

DelayCard::~DelayCard() = default;

visage::Frame& DelayCard::laneChipForTest() const { return *chip_; }

bool DelayCard::picture(dl::TapPicture* out) const { return delayPicture(ownHost(), deviceId(), out); }

double DelayCard::handUnitSeconds(const dl::TapPicture& p) const { return delayHandUnitSeconds(ownHost(), deviceId(), p); }

double DelayCard::spanSeconds() const {
	dl::TapPicture p;
	if (!picture(&p)) {
		return 0.0;
	}
	return dl::stripSpanSeconds(handUnitSeconds(p));
}

std::vector<dl::StripTick> DelayCard::ticksForTest() const {
	dl::TapPicture p;
	if (!picture(&p)) {
		return {};
	}
	return dl::stripTicks(p, dl::stripSpanSeconds(handUnitSeconds(p)));
}

std::vector<dl::StripGrain> DelayCard::grainsForTest() const {
	dl::TapPicture p;
	if (!picture(&p)) {
		return {};
	}
	return dl::stripGrains(p);
}

visage::Bounds DelayCard::stripBounds() const {
	const float left = bodyLeft() + kPad;
	const float top = kHeadHeight + kRowGap - bodyScroll();
	return { left, top, std::max(0.0f, bodyWidth() - 2.0f * kPad), kStripHeight };
}

float DelayCard::xAt(double seconds) const { return DelayStrip { stripBounds(), spanSeconds() }.xAt(seconds); }

float DelayCard::tipY(int channel, float gain) const { return DelayStrip { stripBounds(), spanSeconds() }.tipY(channel, gain); }

size_t DelayCard::laneDevices() const {
	const rack::InnerChain* lane = dl::feedbackLaneOf(ownHost().containerSurfaceOf(deviceId()));
	return lane != nullptr && lane->bridge() != nullptr ? lane->bridge()->chainLength() : 0;
}

std::string DelayCard::laneChipText() const {
	const size_t n = laneDevices();
	return n == 0 ? std::string(kLaneChipEmpty) : std::string(kLaneChipPrefix) + std::to_string(n);
}

void DelayCard::placeChip() {
	// BOTTOM RIGHT OF THE STRIP, inside it — where the R ticks thin out first, and clear of the live head's time.
	const visage::Bounds b = stripBounds();
	chip_->setBounds(b.right() - kLaneChipWidth - 3.0f, b.bottom() - kLaneChipHeight - 3.0f, kLaneChipWidth, kLaneChipHeight);
	chip_->setVisible(!folded() && b.width() > kLaneChipWidth + DelayStrip::kInsetLeft);
}

void DelayCard::resized() {
	GenericCard::resized();
	placeChip();
}

void DelayCard::drawBodyExtras(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	dl::TapPicture p;
	const bool has = picture(&p);
	const double unit = has ? handUnitSeconds(p) : 0.0;
	paintDelayStrip(canvas, DelayStrip { stripBounds(), has ? dl::stripSpanSeconds(unit) : 0.0 }, has ? &p : nullptr, unit);
}

bool DelayCard::updateContent() {
	bool moved = GenericCard::updateContent();
	// WHAT THE STRIP DRAWS, COMPARED RATHER THAN ASSUMED (`CompCard::updateContent`): a frame only when a
	// tick moved, and published-to-unpublished is a change too, so a strip that lost its data says so.
	dl::TapPicture p;
	const bool has = picture(&p);
	std::vector<dl::StripTick> ticks;
	std::vector<dl::StripGrain> grains;
	double span = 0.0;
	if (has) {
		span = dl::stripSpanSeconds(handUnitSeconds(p));
		ticks = dl::stripTicks(p, span);
		grains = dl::stripGrains(p);
	}
	if (has != drawnHad_ || span != drawnSpan_ || ticks != drawnTicks_ || grains != drawnGrains_) {
		drawnHad_ = has;
		drawnSpan_ = span;
		drawnTicks_ = std::move(ticks);
		drawnGrains_ = std::move(grains);
		moved = true;
	}
	// THE LANE'S COUNT, re-asked every tick (the surface is a contemporaneous handle, trap T9).
	if (std::string text = laneChipText(); text != chip_->text) {
		chip_->text = std::move(text);
		chip_->redraw();
		moved = true;
	}
	placeChip();
	// ASK FOR THE FRAME HERE — returning true is the report, not the delivery.
	if (moved) {
		redraw();
	}
	return moved;
}

} // namespace feathers::ui
