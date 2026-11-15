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

#include "plugin/ui/drum_vector_pad.h"
#include "plugin/ui/explore_pad_impl.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace feathers::ui {

SliceBasis<DrumVectorPolicy::kPhaseCount> DrumVectorPolicy::basisAt(const RowReader&, double, const Phases&) {
	SliceBasis<kPhaseCount> basis;
	basis.x = { 1.0f, 0.0f, 0.0f, 0.0f };
	basis.y = { 0.0f, 1.0f, 0.0f, 0.0f };
	return basis;
}

void DrumVectorPolicy::wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored) {
	for (size_t i = 0; i < kPhaseCount; ++i) {
		stored[i] = static_cast<float>(std::clamp(accumulator[i], -kDrumVectorReach, kDrumVectorReach));
	}
}

DrumVectorPolicy::Key DrumVectorPolicy::keyOf(const RowReader&, double rotate, double zoom, const Phases& standing,
                                              const Context& context) {
	return Key { rotate, zoom, standing, context };
}

void DrumVectorPolicy::paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane) {
	const Context& ctx = key.context;
	Character c = ctx.character;
	size_t found = std::numeric_limits<size_t>::max();
	for (size_t i = 0; i < std::min<size_t>(c.count, kMaxSounds); ++i) {
		if (c.sounds[i].slot == ctx.slot) {
			found = i;
		}
	}
	if (found == std::numeric_limits<size_t>::max()) {
		map.build([](float, float) { return 0.0f; });
		return;
	}
	// ONE SOUND, BUILT WHERE EACH CELL IS: the bank's own road (`buildDrumFor`), so the terrain is of what plays.
	c.sounds[0] = c.sounds[found];
	c.count = 1;
	c.drumKit = true;
	SoundCoord& sound = c.sounds[0];
	sound.drumLayer = ctx.layer == 1;
	const auto fieldsAt = [&](double x, double y) {
		sound.drumVectorX[ctx.layer] = x;
		sound.drumVectorY[ctx.layer] = y;
		if (ctx.layer == 0) {
			const AnchorPoint place = drumLayerPlace(c.drumStance, { x, y });
			sound.travelX = place.x;
			sound.travelY = place.y;
			return buildDrumFor(c).pads[sound.slot];
		}
		return buildDrumFor(c).layers[sound.slot];
	};
	const drum::FieldUse reads = drum::kindReads(ctx.kind);
	const auto distance = [&](const drum::Fields& a, const drum::Fields& b) {
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
	const drum::Fields home = fieldsAt(key.standing[0], key.standing[1]);
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
	// Dark is the sound where it stands; the farthest cell in view is full, at least a tenth of the fields' span so a
	// plane that hardly changes the sound draws dim rather than amplifying nothing.
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

template class ExplorePad<DrumVectorPolicy>;

} // namespace feathers::ui
