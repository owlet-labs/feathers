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
	dd::Settings s = key.context;
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
	std::array<float, kGrid * kGrid> coarse {};
	float hi = 0.0f;
	for (size_t gy = 0; gy < kGrid; ++gy) {
		for (size_t gx = 0; gx < kGrid; ++gx) {
			const float ux = (static_cast<float>(gx) + 0.5f) / static_cast<float>(kGrid);
			const float uy = (static_cast<float>(gy) + 0.5f) / static_cast<float>(kGrid);
			const std::array<float, kPhaseCount> at = plane.travelAt(ux, uy);
			const float d = static_cast<float>(distance(fieldsAt(at[0], at[1]), home));
			coarse[gy * kGrid + gx] = std::isfinite(d) ? d : 0.0f;
			hi = std::max(hi, coarse[gy * kGrid + gx]);
		}
	}
	const float span = std::max(hi, 0.1f);
	for (float& v : coarse) {
		v = std::pow(std::clamp(v / span, 0.0f, 1.0f), 0.7f);
	}
	map.build([&](float ux, float uy) {
		const float fx = std::clamp(ux * static_cast<float>(kGrid) - 0.5f, 0.0f, static_cast<float>(kGrid - 1));
		const float fy = std::clamp(uy * static_cast<float>(kGrid) - 0.5f, 0.0f, static_cast<float>(kGrid - 1));
		const auto x0 = static_cast<size_t>(fx);
		const auto y0 = static_cast<size_t>(fy);
		const size_t x1 = std::min(x0 + 1, kGrid - 1);
		const size_t y1 = std::min(y0 + 1, kGrid - 1);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const float a = coarse[y0 * kGrid + x0] + (coarse[y0 * kGrid + x1] - coarse[y0 * kGrid + x0]) * tx;
		const float b = coarse[y1 * kGrid + x0] + (coarse[y1 * kGrid + x1] - coarse[y1 * kGrid + x0]) * tx;
		return a + (b - a) * ty;
	});
}

template class ExplorePad<DrumDevicePadPolicy>;

} // namespace feathers::ui
