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

#pragma once

#include "fx/delay/delay_taps.h"
#include "plugin/ui/generic_card.h"

#include <memory>
#include <string>
#include <vector>

// THE DELAY'S CARD — the taps strip above the grid (backlog LF: a taps visualizer, left
// channel on top and right below).
//
// **THE STRIP IS WHAT AN IMPULSE ARRIVING NOW WOULD BECOME**, read off the heads the device is actually
// reading at (`delay_taps.h`): a time axis, the left output's arrivals as ticks ABOVE the centre line and
// the right output's BELOW, each as tall as its gain, so the feedback's repeats shrink along the line and
// show the tail. The dry path is the grey tick at zero. A tick is amber when it is heard on the side its
// input came in on and bone when the loop CROSSED it there — which is what makes Ping-Pong visible on a
// centred input, where the crossed and the uncrossed arrivals land at the same times.
//
// PER MODE, each drawn from what the device runs rather than from a picture of the mode: Clean and Tape
// are one head per channel (Tape's wow and flutter are ON the published head, so the ticks wander);
// Xfade draws its old head too while a fade is running; Multi-Tap draws the pattern and repeats it from
// the base tap, as the loop does; Granular draws each pass with a whisker over the window its grains can
// land in, widening with every pass through the granulator, and the grains in flight as dots. Reverse
// (backlog MB) draws each pass's TWO returns — one per windowed head, weighted so they sum to the pass —
// each with a whisker over the chunk it plays back in and an arrowhead pointing back along it; the ticks
// slide as the chunks turn over, because where an impulse lands depends on where in its chunk it went in.
//
// **HEIGHTS ARE DECIBELS** (`stripHeightOf`, -48 dB to unity), so a feedback tail falls as a straight
// slope.
//
// **THE GRID BELOW IS `GenericCard`'S, UNCHANGED** — sections, the stacked Mode/Dub cell, the gated flex
// row — so this derives from it and uses only `bodyTopInset` and `drawBodyExtras`, SKLITTER's pattern.
// The strip reaches no control: it is a reading. **THE WINDOW (delay-love S2) is opened from the head, or from the
// strip's `FB FX` chip** — bottom right, counting the devices in the FEEDBACK FX lane — and its superset guard walks
// this card. (Before S2 there was no window, so the
// window-superset guard had nothing to walk.)
//
// **THE AXIS FOLLOWS THE HAND, THE TICKS FOLLOW THE DEVICE.** The span is snapped from the tap the
// UNMODULATED controls ask for (`stripSpanSeconds`), and faint rules mark multiples of it; a modulated
// Time, a glide or the tape's wobble moves the ticks off those rules rather than dragging the axis along.

namespace feathers::ui {

class DelayCard final : public GenericCard {
public:
	DelayCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — see `GenericCard`.
	DelayCard(backplane::DeviceHost&, size_t, Notifier* = nullptr) = delete;
	~DelayCard() override;

	/// THE FB FX CHIP: its words, its size. "FB FX +" while the lane holds nothing (or was never built), else the count.
	static constexpr const char* kLaneChipEmpty = "FB FX +";
	static constexpr const char* kLaneChipPrefix = "FB FX · ";
	static constexpr float kLaneChipWidth = 54.0f;
	static constexpr float kLaneChipHeight = 13.0f;
	static constexpr const char* kLaneChipTip
	    = "FEEDBACK FX: effects inside the loop, so every repeat passes through them again. Opens the window.";

	/// HOW TALL THE STRIP IS: room for a tick either side of the line at a readable height.
	static constexpr float kStripHeight = 58.0f;
	/// The gap between the strip and the first row.
	static constexpr float kStripGap = 8.0f;

	bool updateContent() override;
	void resized() override;

	//------------//
	// Test seams //
	//------------//

	/// THE READINGS AS THIS CARD DECODED THEM; false while nothing is published.
	[[nodiscard]] bool pictureForTest(fx::delay::TapPicture* out) const { return picture(out); }
	/// EVERY TICK THE STRIP DRAWS, in seconds and linear gain — the picture's numbers, not its pixels.
	[[nodiscard]] std::vector<fx::delay::StripTick> ticksForTest() const;
	/// THE GRAINS IN FLIGHT THE STRIP DRAWS.
	[[nodiscard]] std::vector<fx::delay::StripGrain> grainsForTest() const;
	/// THE SPAN THE AXIS COVERS, in seconds.
	[[nodiscard]] double spanForTest() const { return spanSeconds(); }
	/// WHERE THE STRIP SITS, in the card's coordinates with the scroll applied.
	[[nodiscard]] visage::Bounds stripBoundsForTest() const { return stripBounds(); }
	/// THE X a time lands at, and the Y a tick of `gain` on `channel` reaches — `drawBodyExtras` places by these.
	[[nodiscard]] float xForTest(double seconds) const { return xAt(seconds); }
	[[nodiscard]] float tipYForTest(int channel, float gain) const { return tipY(channel, gain); }
	/// THE FB FX CHIP: the frame (its press opens the window) and what it says.
	[[nodiscard]] visage::Frame& laneChipForTest() const;
	[[nodiscard]] std::string laneChipTextForTest() const { return laneChipText(); }

protected:
	float bodyTopInset() const override { return kStripHeight + kStripGap; }
	void drawBodyExtras(visage::Canvas& canvas) override;

private:
	[[nodiscard]] bool picture(fx::delay::TapPicture* out) const;
	/// THE TAP THE UNMODULATED CONTROLS ASK FOR, seconds — the axis's unit. Zero while nothing is published.
	[[nodiscard]] double handUnitSeconds(const fx::delay::TapPicture& p) const;
	[[nodiscard]] double spanSeconds() const;
	[[nodiscard]] visage::Bounds stripBounds() const;
	[[nodiscard]] float xAt(double seconds) const;
	[[nodiscard]] float tipY(int channel, float gain) const;
	[[nodiscard]] size_t laneDevices() const;
	[[nodiscard]] std::string laneChipText() const;
	void placeChip();

	class LaneChip;
	std::unique_ptr<LaneChip> chip_;

	/// WHAT WAS LAST DRAWN — `updateContent` asks whether the picture actually moved.
	bool drawnHad_ = false;
	std::vector<fx::delay::StripTick> drawnTicks_;
	std::vector<fx::delay::StripGrain> drawnGrains_;
	double drawnSpan_ = -1.0;
};

} // namespace feathers::ui
