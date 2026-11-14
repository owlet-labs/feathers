/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/harm_envelope_view.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace hrm = feathers::fx::harm;

HarmEnvelopeView::HarmEnvelopeView(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), device_(id) {
	rebuild();
}

visage::Bounds HarmEnvelopeView::plotBounds() const {
	return { 0.0f, 0.0f, width(), height() };
}

hrm::EnvelopeTimes HarmEnvelopeView::readTimes() const {
	// THE DEVICE'S OWN DECODER, as the scope reads its settings: a seam row, `stateOf`, and the seconds are what the DSP runs on.
	hrm::Row row {};
	const size_t fields = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < fields; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	const hrm::Settings s = hrm::stateOf(row, fields).settings;
	hrm::EnvelopeTimes t;
	t.delay = static_cast<double>(s.delaySeconds);
	t.attack = static_cast<double>(s.attackSeconds);
	t.hold = static_cast<double>(s.holdSeconds);
	t.decay = static_cast<double>(s.decaySeconds);
	t.sustain = static_cast<double>(s.sustain);
	t.release = static_cast<double>(s.releaseSeconds);
	return t;
}

bool HarmEnvelopeView::refresh() {
	const hrm::EnvelopeTimes now = readTimes();
	if (built_ && now.delay == times_.delay && now.attack == times_.attack && now.hold == times_.hold
	    && now.decay == times_.decay && now.sustain == times_.sustain && now.release == times_.release) {
		return false;
	}
	rebuild();
	redraw();
	return true;
}

void HarmEnvelopeView::rebuild() {
	times_ = readTimes();
	built_ = true;
	const std::vector<hrm::EnvelopeSegment> segments = hrm::envelopeSegments(times_);

	// A STAGE'S WIDTH IS ITS LENGTH IN SECONDS, the sustain plateau a fixed share, and a stage that happens never less than a
	// floor of the whole — the modulator card's law (env_card.cpp `shareOf`, `kSustainShare`, `kMinLiveShare`).
	std::vector<float> share(segments.size(), 0.0f);
	float timed = 0.0f;
	for (size_t i = 0; i < segments.size(); ++i) {
		if (segments[i].stage != hrm::Envelope::Stage::Sustain) {
			share[i] = static_cast<float>(std::max(0.0, segments[i].seconds));
			timed += share[i];
		}
	}
	for (size_t i = 0; i < segments.size(); ++i) {
		if (segments[i].stage == hrm::Envelope::Stage::Sustain) {
			share[i] = std::max(timed, 0.001f) * kSustainShare / (1.0f - kSustainShare);
		}
	}
	float total = 0.0f;
	for (const float s : share) {
		total += s;
	}
	for (int pass = 0; pass < 4 && total > 0.0f; ++pass) {
		bool raised = false;
		for (float& s : share) {
			const float floorShare = kMinLiveShare * total;
			if (s > 0.0f && s < floorShare) {
				total += floorShare - s;
				s = floorShare;
				raised = true;
			}
		}
		if (!raised) {
			break;
		}
	}
	if (!(total > 0.0f)) {
		share.assign(share.size(), 0.0f);
		total = 1.0f;
	}

	curve_.clear();
	spans_.clear();
	curve_.push_back({ 0.0f, 0.0f });
	float x0 = 0.0f;
	constexpr int kPointsPerStage = 24;
	for (size_t i = 0; i < segments.size(); ++i) {
		const hrm::EnvelopeSegment& seg = segments[i];
		const float x1 = x0 + share[i] / total;
		if (x1 > x0) {
			for (int p = 1; p <= kPointsPerStage; ++p) {
				const float t = static_cast<float>(p) / kPointsPerStage;
				const double at = static_cast<double>(t) * std::max(0.0, seg.seconds);
				curve_.push_back({ x0 + t * (x1 - x0), static_cast<float>(hrm::envelopeLevelAt(seg, times_, at)) });
			}
			spans_.push_back({ seg.stage, x0, x1 });
		}
		else {
			// A STAGE THAT DOES NOT TAKE TIME STILL MOVES THE LEVEL: a vertical edge to where it lands.
			curve_.push_back({ x0, static_cast<float>(seg.to) });
		}
		x0 = x1;
	}
}

void HarmEnvelopeView::draw(visage::Canvas& canvas) {
	const visage::Bounds box = plotBounds();
	if (box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 4.0f);

	const float left = box.x() + kPad;
	const float top = box.y() + kPad;
	const float w = std::max(0.0f, box.width() - 2.0f * kPad);
	const float h = std::max(0.0f, box.height() - 2.0f * kPad);
	const auto plot = [&](const visage::Point& p) {
		return visage::Point { left + std::clamp(p.x, 0.0f, 1.0f) * w, top + h - std::clamp(p.y, 0.0f, 1.0f) * h };
	};

	// THE STAGE BOUNDARIES, as hairlines — so the eye can see where one stage ends and which time box it is.
	canvas.setColor(withAlpha(kLine, 0.35f));
	for (const Span& span : spans_) {
		canvas.fill(plot({ span.to, 0.0f }).x, top, 1.0f, h);
	}
	// UNITY, so a sustain is read against the peak rather than against the top of a box.
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.fill(left, top, w, 1.0f);

	canvas.setColor(kValue);
	for (size_t i = 1; i < curve_.size(); ++i) {
		const visage::Point a = plot(curve_[i - 1]);
		const visage::Point b = plot(curve_[i]);
		if (!std::isfinite(a.y) || !std::isfinite(b.y)) {
			continue;
		}
		canvas.segment(a.x, a.y, b.x, b.y, 1.6f, true);
	}
}

} // namespace feathers::ui
