/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/drum_device_pad.h"
#include "plugin/ui/explore_pad_impl.h"

#include "fx/drum/drum.h"
#include "plugin/ui/like_glyph.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace dd = feathers::fx::drumdevice;

SliceBasis<DrumDevicePadPolicy::kPhaseCount> DrumDevicePadPolicy::basisAt(const RowReader&, double, const Phases&) {
	SliceBasis<kPhaseCount> basis;
	basis.x = { 1.0f, 0.0f, 0.0f, 0.0f };
	basis.y = { 0.0f, 1.0f, 0.0f, 0.0f };
	return basis;
}

void DrumDevicePadPolicy::wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored) {
	for (size_t i = 0; i < kPhaseCount; ++i) {
		stored[i] = static_cast<float>(std::clamp(accumulator[i], -dd::kWindow, dd::kWindow)); // the declared window, never a limit that bites
	}
}

DrumDevicePadPolicy::Key DrumDevicePadPolicy::keyOf(const RowReader&, double rotate, double zoom, const Phases& standing,
                                                    const Context& context) {
	return Key { rotate, zoom, standing, context };
}

void DrumDevicePadPolicy::paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane) {
	dd::Settings s = key.context.settings;
	const auto kindIndex = std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1);
	const feathers::drum::FieldUse reads = feathers::drum::kindReads(static_cast<feathers::drum::Kind>(kindIndex));
	const auto fieldsAt = [&](double x, double y) {
		s.v[static_cast<size_t>(dd::Param::TravelX)] = static_cast<float>(x);
		s.v[static_cast<size_t>(dd::Param::TravelY)] = static_cast<float>(y);
		return dd::fieldsOf(s);
	};
	const auto distance = [&](const feathers::drum::Fields& a, const feathers::drum::Fields& b) {
		double sum = 0.0;
		int n = 0;
		const auto add = [&](bool read, float x, float y) {
			if (read) {
				const double d = static_cast<double>(x) - static_cast<double>(y);
				sum += d * d;
				++n;
			}
		};
		add(reads.pitch, a.pitch, b.pitch);
		add(reads.length, a.length, b.length);
		add(reads.sweep, a.sweep, b.sweep);
		add(reads.tone, a.tone, b.tone);
		add(reads.noise, a.noise, b.noise);
		add(reads.snap, a.snap, b.snap);
		add(reads.metal, a.metal, b.metal);
		return n > 0 ? std::sqrt(sum / n) : 0.0;
	};
	const feathers::drum::Fields home = fieldsAt(key.standing[0], key.standing[1]);
	// A WARPED VIEW IS SAMPLED FINER: a lens draws a liked place many times larger, and a coarse grid would blur what it gave room to.
	const size_t grid = key.context.likes != nullptr ? kWarpedGrid : kGrid;
	std::array<float, kWarpedGrid * kWarpedGrid> coarse {};
	float hi = 0.0f;
	for (size_t gy = 0; gy < grid; ++gy) {
		for (size_t gx = 0; gx < grid; ++gx) {
			const float ux = (static_cast<float>(gx) + 0.5f) / static_cast<float>(grid);
			const float uy = (static_cast<float>(gy) + 0.5f) / static_cast<float>(grid);
			const std::array<float, kPhaseCount> at = plane.travelAt(ux, uy);
			const float d = static_cast<float>(distance(fieldsAt(at[0], at[1]), home));
			coarse[gy * grid + gx] = std::isfinite(d) ? d : 0.0f;
			hi = std::max(hi, coarse[gy * grid + gx]);
		}
	}
	const float span = std::max(hi, 0.1f);
	for (size_t i = 0; i < grid * grid; ++i) {
		float& v = coarse[i];
		v = std::pow(std::clamp(v / span, 0.0f, 1.0f), 0.7f);
	}
	map.build([&](float ux, float uy) {
		const float fx = std::clamp(ux * static_cast<float>(grid) - 0.5f, 0.0f, static_cast<float>(grid - 1));
		const float fy = std::clamp(uy * static_cast<float>(grid) - 0.5f, 0.0f, static_cast<float>(grid - 1));
		const auto x0 = static_cast<size_t>(fx);
		const auto y0 = static_cast<size_t>(fy);
		const size_t x1 = std::min(x0 + 1, grid - 1);
		const size_t y1 = std::min(y0 + 1, grid - 1);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const float a = coarse[y0 * grid + x0] + (coarse[y0 * grid + x1] - coarse[y0 * grid + x0]) * tx;
		const float b = coarse[y1 * grid + x0] + (coarse[y1 * grid + x1] - coarse[y1 * grid + x0]) * tx;
		return a + (b - a) * ty;
	});
}

std::pair<double, double> DrumDevicePadPolicy::worldFromView(const Context& c, const Phases& origin, double x, double y) {
	if (c.likes == nullptr) {
		return { x, y };
	}
	const std::array<double, 2> w = c.likes->worldAt(origin[0] + x, origin[1] + y);
	return { w[0] - origin[0], w[1] - origin[1] };
}

std::pair<double, double> DrumDevicePadPolicy::viewFromWorld(const Context& c, const Phases& origin, double x, double y) {
	if (c.likes == nullptr) {
		return { x, y };
	}
	const std::array<double, 2> v = c.likes->viewAt(origin[0] + x, origin[1] + y);
	return { v[0] - origin[0], v[1] - origin[1] };
}

template class ExplorePad<DrumDevicePadPolicy>;

visage::Point DrumDevicePad::likePixel(const fx::drumdevice::DrumLike& like) const {
	return pixelOfPlane(like.x - frameOrigin()[0], like.y - frameOrigin()[1]);
}

std::optional<size_t> DrumDevicePad::likeUnderStar() const {
	if (!applied) {
		return std::nullopt;
	}
	const visage::Point p = puck();
	const std::vector<fx::drumdevice::DrumLike>& likes = applied();
	std::optional<size_t> best;
	float bestD = kLikeTouchPixels;
	for (size_t i = 0; i < likes.size(); ++i) {
		const visage::Point m = likePixel(likes[i]);
		const float d = std::hypot(m.x - p.x, m.y - p.y);
		if (d <= bestD) {
			bestD = d;
			best = i;
		}
	}
	return best;
}

visage::Bounds DrumDevicePad::markRect(visage::Point at) const {
	const float r = kLikeMarkSize * 0.5f + 1.0f;
	return { at.x - r, at.y - r, 2.0f * r, 2.0f * r };
}

visage::Point DrumDevicePad::markAt(visage::Point place) const {
	// A MARK UNDER THE INSET (Rotate's box, drawn over the pad) IS MOVED OUT FROM UNDER IT, straight down to just below it: a like in that
	// corner stays visible, a little off its place, rather than hidden.
	const visage::Bounds mark = markRect(place);
	if (inset_.width() > 0.0f && mark.x() < inset_.right() && mark.right() > inset_.x() && mark.y() < inset_.bottom()
	    && mark.bottom() > inset_.y()) {
		return { place.x, inset_.bottom() + mark.height() * 0.5f };
	}
	return place;
}

void DrumDevicePad::draw(visage::Canvas& canvas) {
	ExplorePad::draw(canvas);
	likeMarksDrawn_ = 0;
	if (!applied) {
		return;
	}
	// EVERY LIKE OF THIS PLANE AND KIND AT ITS PLACE ON THE PICTURE (the warp carried it), a heart in the likes' one hue as the synth's chart draws them.
	for (const fx::drumdevice::DrumLike& like : applied()) {
		const visage::Point place = likePixel(like);
		if (!std::isfinite(place.x) || !std::isfinite(place.y) || place.x < 0.0f || place.y < 0.0f || place.x > width() || place.y > height()) {
			continue;
		}
		const visage::Point m = markAt(place);
		canvas.setColor(withAlpha(kLikeHue, 0.92f));
		drawHeart(canvas, m.x, m.y, kLikeMarkSize);
		++likeMarksDrawn_;
	}
}

} // namespace feathers::ui
