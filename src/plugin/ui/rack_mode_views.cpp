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

#include "plugin/ui/rack_mode_views.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace pr = feathers::rack::parallel;

namespace {

/// A BAND'S TINT, by its position — the strip's spans and (later) the rails read one palette. Data, so it does not turn.
uint32_t bandTint(int k) {
	static constexpr uint32_t kTints[] = { kMeterAccent, kMeterValue, 0xff7FB8A4, kMeterNear, 0xffB48CD9 };
	return kTints[static_cast<size_t>(k) % std::size(kTints)];
}

double log2Of(double hz) { return std::log2(std::max(hz, 1.0)); }

} // namespace

//------------------------------------------------------------------------------------------------//
// RackSpectrumStrip                                                                                //
//------------------------------------------------------------------------------------------------//

RackSpectrumStrip::RackSpectrumStrip(backplane::DeviceHost& host, DeviceInstanceId rack, std::array<DeviceControl*, 4> cuts,
                                     DeviceControl& mode, DeviceControl& bands)
    : host_(host), rack_(rack), cuts_(cuts), mode_(mode), bands_(bands) {
	pre_.fill(dsp::kAnalyserFloorDb);
	post_.fill(dsp::kAnalyserFloorDb);
	installTooltipHover(*this, *this);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE NEAREST CORNER WITHIN `kGrab` OF THE PRESS takes the drag; anywhere else does nothing.
		int best = -1;
		float bestDistance = kGrab;
		for (int k = 0; k < cornerCount(); ++k) {
			const float d = std::fabs(e.position.x - cornerXForTest(k));
			if (d <= bestDistance) {
				best = k;
				bestDistance = d;
			}
		}
		if (best >= 0) {
			beginCorner(best);
		}
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (dragging_ >= 0) {
			moveCorner(dragging_, e.position.x);
			redraw();
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (dragging_ >= 0) {
			endCorner();
			redraw();
		}
	};
}

bool RackSpectrumStrip::inBands() const {
	return std::lround(mode_.value()) == static_cast<long>(pr::Mode::Bands);
}

int RackSpectrumStrip::cornerCount() const { return inBands() ? pr::bandCountOf(bands_.value()) - 1 : 0; }

double RackSpectrumStrip::cornerHz(int k) const {
	float cuts[4];
	for (size_t j = 0; j < 4; ++j) {
		cuts[j] = static_cast<float>(cuts_[j]->value());
	}
	double hz[4];
	const double note = noteHz();
	pr::trackedCorners(cuts, note > 0.0, note, hz); // KEY TRACK: the corners as the engine places them on the note
	return hz[static_cast<size_t>(k)];
}

double RackSpectrumStrip::noteHz() const {
	const auto i = static_cast<size_t>(pr::Reading::NoteHz);
	return host_.readingCountAt(rack_) > i ? static_cast<double>(host_.readingAt(rack_, i)) : 0.0;
}

float RackSpectrumStrip::xOfHz(double hz) const noexcept {
	const double lo = log2Of(dsp::kAnalyserLowHz);
	const double hi = log2Of(dsp::kAnalyserHighHz);
	return static_cast<float>((log2Of(hz) - lo) / (hi - lo) * static_cast<double>(width()));
}

double RackSpectrumStrip::hzOfX(float x) const noexcept {
	const double lo = log2Of(dsp::kAnalyserLowHz);
	const double hi = log2Of(dsp::kAnalyserHighHz);
	const double w = std::max(1.0, static_cast<double>(width()));
	return std::exp2(lo + std::clamp(static_cast<double>(x) / w, 0.0, 1.0) * (hi - lo));
}

float RackSpectrumStrip::cornerXForTest(int k) const { return xOfHz(cornerHz(k)); }

void RackSpectrumStrip::beginCorner(int k) {
	dragging_ = k;
	touched_.clear();
}

void RackSpectrumStrip::moveCorner(int k, float x) {
	// THE CORNER GOES WHERE THE HAND PUTS IT, and its neighbours are PUSHED ALONG (§4a: "the UI's drag pushes
	// neighbours along"), each kept a third of an octave clear, each one gesture of its own.
	const auto write = [this](int j, double log2Hz) {
		DeviceControl& c = *cuts_[static_cast<size_t>(j)];
		const device::ParamDesc& d = c.desc();
		if (std::find(touched_.begin(), touched_.end(), j) == touched_.end()) {
			touched_.push_back(j);
			c.beginGesture();
		}
		c.setValue(std::clamp(log2Hz, d.min, d.max));
	};
	// TRACKING, THE DRAG WRITES THE OFFSET: the cut whose offset from the note puts the corner under the hand.
	const double note = noteHz();
	const double want = note > 0.0 ? pr::kCutKeyRefLog2 + std::log2(hzOfX(x) / note) : log2Of(hzOfX(x));
	write(k, want);
	const int last = cornerCount() - 1;
	for (int j = k + 1; j <= last; ++j) {
		const double floor = cuts_[static_cast<size_t>(j - 1)]->value() + pr::kMinCornerOctaves;
		if (cuts_[static_cast<size_t>(j)]->value() < floor) {
			write(j, floor);
		}
	}
	for (int j = k - 1; j >= 0; --j) {
		const double ceiling = cuts_[static_cast<size_t>(j + 1)]->value() - pr::kMinCornerOctaves;
		if (cuts_[static_cast<size_t>(j)]->value() > ceiling) {
			write(j, ceiling);
		}
	}
}

void RackSpectrumStrip::endCorner() {
	for (const int j : touched_) {
		cuts_[static_cast<size_t>(j)]->endGesture();
	}
	touched_.clear();
	dragging_ = -1;
}

void RackSpectrumStrip::dragCornerForTest(int k, float x) {
	beginCorner(k);
	moveCorner(k, x);
	endCorner();
}

bool RackSpectrumStrip::refresh() {
	if (!isVisible()) {
		return false;
	}
	// THE LEASE: the analyser runs while this strip is shown and asking (`Rack::wantReadings`).
	if (pr::Rack* rack = pr::rackOf(host_.containerSurfaceOf(rack_)); rack != nullptr) {
		rack->wantReadings();
	}
	if (host_.readingCountAt(rack_) < pr::kReadingCount) {
		return false;
	}
	const float a = host_.readingAt(rack_, static_cast<size_t>(pr::Reading::FrameA));
	const float b = host_.readingAt(rack_, static_cast<size_t>(pr::Reading::FrameB));
	if (a != b || a == frame_) {
		return false; // a frame half-written, or the one already drawn
	}
	for (size_t k = 0; k < dsp::kAnalyserBins; ++k) {
		pre_[k] = host_.readingAt(rack_, static_cast<size_t>(pr::Reading::PreFirst) + k);
		post_[k] = host_.readingAt(rack_, static_cast<size_t>(pr::Reading::PostFirst) + k);
	}
	frame_ = a;
	redraw();
	return true;
}

const char* RackSpectrumStrip::tooltipAt(visage::Point p) const {
	for (int k = 0; k < cornerCount(); ++k) {
		if (std::fabs(p.x - cornerXForTest(k)) <= kGrab) {
			return tooltipOf(cuts_[static_cast<size_t>(k)]);
		}
	}
	return inBands() ? "The rack's input (line) and output (fill); drag a corner to move it"
	                 : "The rack's input (line) and output (fill)";
}

void RackSpectrumStrip::offeredControls(std::vector<Offered>& out) const {
	for (int k = 0; k < cornerCount(); ++k) {
		out.push_back({ cuts_[static_cast<size_t>(k)], Road::Face });
	}
}

void RackSpectrumStrip::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	canvas.setColor(withAlpha(kLine, 0.35f));
	canvas.roundedRectangle(0.0f, 0.0f, w, h, 3.0f);
	// THE BANDS' SPANS, each tinted as its chain will be.
	const int corners = cornerCount();
	for (int k = 0; k <= corners && corners > 0; ++k) {
		const float x0 = k == 0 ? 0.0f : cornerXForTest(k - 1);
		const float x1 = k == corners ? w : cornerXForTest(k);
		canvas.setColor(withAlpha(bandTint(k), 0.14f));
		canvas.fill(x0, 0.0f, std::max(0.0f, x1 - x0), h);
	}
	const auto yOf = [&](float db) {
		const float t = std::clamp((db - kBottomDb) / (kTopDb - kBottomDb), 0.0f, 1.0f);
		return h - t * h;
	};
	// POST, FILLED; PRE, A LINE — bin by bin, each as wide as its share of the axis.
	const float binW = w / static_cast<float>(dsp::kAnalyserBins);
	canvas.setColor(withAlpha(kMeterValue, 0.55f));
	for (size_t k = 0; k < dsp::kAnalyserBins; ++k) {
		const float y = yOf(post_[k]);
		canvas.fill(static_cast<float>(k) * binW, y, binW, h - y);
	}
	canvas.setColor(withAlpha(kInk, 0.7f));
	for (size_t k = 0; k < dsp::kAnalyserBins; ++k) {
		canvas.fill(static_cast<float>(k) * binW, yOf(pre_[k]), binW, 1.0f);
	}
	// THE CORNERS: a line each, the handle a hand drags.
	for (int k = 0; k < corners; ++k) {
		canvas.setColor(dragging_ == k ? kAccent : withAlpha(kInk, 0.85f));
		canvas.fill(std::round(cornerXForTest(k)) - 0.5f, 0.0f, 1.5f, h);
	}
}

//------------------------------------------------------------------------------------------------//
// RackPositionBar                                                                                  //
//------------------------------------------------------------------------------------------------//

RackPositionBar::RackPositionBar(backplane::DeviceHost& host, DeviceInstanceId rack, DeviceControl& position,
                                 DeviceControl& mode)
    : host_(host), rack_(rack), position_(position), mode_(mode) {
	installTooltipHover(*this, *this);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		dragging_ = true;
		position_.beginGesture();
		setFromX(e.position.x);
		redraw();
	};
	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (dragging_) {
			setFromX(e.position.x);
			redraw();
		}
	};
	onMouseUp() = [this](const visage::MouseEvent&) {
		if (dragging_) {
			dragging_ = false;
			position_.endGesture();
			redraw();
		}
	};
}

void RackPositionBar::setFromX(float x) {
	// THE CURSOR'S MAP INVERTED: chain k's segment CENTRE is position k/(N−1).
	const float w = std::max(1.0f, static_cast<float>(width()));
	if (chains_ <= 1) {
		return;
	}
	const double seg = static_cast<double>(w) / static_cast<double>(chains_);
	const double p = (static_cast<double>(x) / seg - 0.5) / static_cast<double>(chains_ - 1);
	position_.setValue(std::clamp(p, 0.0, 1.0));
}

void RackPositionBar::dragForTest(float x, float toX) {
	dragging_ = true;
	position_.beginGesture();
	setFromX(x);
	setFromX(toX);
	dragging_ = false;
	position_.endGesture();
}

bool RackPositionBar::refresh() {
	const pr::Rack* rack = pr::rackOf(host_.containerSurfaceOf(rack_));
	const size_t chains = rack != nullptr ? rack->chainCount() : 0;
	const double position = position_.value();
	const double mode = mode_.value();
	if (chains == chains_ && position == drawnPosition_ && mode == drawnMode_) {
		return false;
	}
	chains_ = chains;
	drawnPosition_ = position;
	drawnMode_ = mode;
	redraw();
	return true;
}

float RackPositionBar::cursorXForTest() const {
	// POSITION k/(N−1) STANDS AT CHAIN k's SEGMENT CENTRE, so the cursor is over the chain Select hears.
	if (chains_ == 0) {
		return 0.0f;
	}
	const float seg = static_cast<float>(width()) / static_cast<float>(chains_);
	return seg * (0.5f + static_cast<float>(position_.value()) * static_cast<float>(chains_ > 1 ? chains_ - 1 : 0));
}

float RackPositionBar::fillForTest(size_t k) const {
	const auto mode = static_cast<pr::Mode>(std::lround(mode_.value()));
	return pr::positionWeight(mode, position_.value(), k, chains_);
}

void RackPositionBar::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	if (chains_ == 0) {
		return;
	}
	// ONE SEGMENT PER CHAIN, chain k centred at k/(N−1) — the position's own map — filled to its weight.
	const float seg = w / static_cast<float>(chains_);
	for (size_t k = 0; k < chains_; ++k) {
		const float x = static_cast<float>(k) * seg;
		canvas.setColor(withAlpha(kLine, 0.5f));
		canvas.roundedRectangle(x + 1.0f, 0.0f, seg - 2.0f, h, 3.0f);
		const float fill = fillForTest(k);
		if (fill > 0.0f) {
			canvas.setColor(withAlpha(bandTint(static_cast<int>(k)), 0.35f + 0.5f * fill));
			canvas.roundedRectangle(x + 1.0f, h * (1.0f - fill), seg - 2.0f, h * fill, 3.0f);
		}
		canvas.setColor(kInk);
		canvas.text(std::to_string(k + 1), font(kKnobLabelSize), visage::Font::kCenter, x, 0.0f, seg, h);
	}
	// THE CURSOR.
	canvas.setColor(kAccent);
	canvas.fill(std::round(cursorXForTest()) - 1.0f, 0.0f, 2.0f, h);
}

} // namespace feathers::ui
