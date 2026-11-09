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

#include "dsp/constellation.h"

#include <algorithm>
#include <cmath>

namespace feathers {

AnchorPoint tourPoint(std::span<const AnchorPoint> anchors, double path) {
	if (anchors.empty()) {
		return {};
	}
	if (anchors.size() == 1) {
		return anchors[0];
	}
	const double clamped = std::clamp(path, 0.0, 1.0);
	const double legs = static_cast<double>(anchors.size() - 1);
	const double scaled = clamped * legs;

	// The leg index, held one short of the end so that path == 1 lands ON the last anchor rather than
	// one past it. Written as a clamp rather than as a special case, because "the last leg, fully
	// walked" and "path is exactly 1" are the same situation and should not take different code.
	auto leg = static_cast<size_t>(scaled);
	leg = std::min(leg, anchors.size() - 2);
	const double t = scaled - static_cast<double>(leg);

	const AnchorPoint& from = anchors[leg];
	const AnchorPoint& to = anchors[leg + 1];
	// Written so that t == 0 returns `from` and t == 1 returns `to` EXACTLY, rather than to within a
	// rounding of them. The ends of a leg are where a sound is supposed to be purely itself, and
	// `from + (to - from) * t` misses them by an ulp often enough to matter.
	return { from.x * (1.0 - t) + to.x * t, from.y * (1.0 - t) + to.y * t };
}

void blendWeights(std::span<const AnchorPoint> anchors, AnchorPoint puck, std::span<double> out,
                  double power) {
	const size_t n = std::min(anchors.size(), out.size());
	if (n == 0) {
		return;
	}

	// ON AN ANCHOR IS ITS OWN CASE, and it is checked before anything is accumulated. Inverse-distance
	// at zero distance is a division by zero, and the limit it approaches is "all of this sound"; a
	// caller that hit it would otherwise get an infinity into a normalisation and come out with a NaN
	// per voice, which is silence at best and a loud one at worst.
	size_t coincident = 0;
	for (size_t i = 0; i < n; ++i) {
		const double dx = anchors[i].x - puck.x;
		const double dy = anchors[i].y - puck.y;
		if (std::sqrt(dx * dx + dy * dy) <= kAnchorSnapDistance) {
			coincident++;
		}
	}
	if (coincident > 0) {
		const double share = 1.0 / static_cast<double>(coincident);
		for (size_t i = 0; i < n; ++i) {
			const double dx = anchors[i].x - puck.x;
			const double dy = anchors[i].y - puck.y;
			out[i] = std::sqrt(dx * dx + dy * dy) <= kAnchorSnapDistance ? share : 0.0;
		}
		return;
	}

	double total = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double dx = anchors[i].x - puck.x;
		const double dy = anchors[i].y - puck.y;
		const double distance = std::sqrt(dx * dx + dy * dy);
		// pow rather than a hand-rolled reciprocal, so the exponent stays a number somebody can change
		// without also having to change the arithmetic that reads it — which is what lets the A/B be a
		// live toggle rather than a second build.
		out[i] = 1.0 / std::pow(distance, power);
		total += out[i];
	}

	// A total that is not finite means every weight overflowed, which needs a puck within an ulp of an
	// anchor that the snap above did not catch. Sharing evenly is the honest answer rather than
	// propagating a NaN into the mix — and it is unreachable rather than merely unlikely, which is why
	// it is a guard and not a branch anyone should have to think about.
	if (!(total > 0.0) || !std::isfinite(total)) {
		const double share = 1.0 / static_cast<double>(n);
		for (size_t i = 0; i < n; ++i) {
			out[i] = share;
		}
		return;
	}

	const double scale = 1.0 / total;
	for (size_t i = 0; i < n; ++i) {
		out[i] *= scale;
	}
}

} // namespace feathers
