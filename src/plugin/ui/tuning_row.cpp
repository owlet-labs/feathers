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

#include "plugin/ui/tuning_row.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace feathers::ui {

TuningRow::TuningRow(std::string label, std::vector<Step> steps, std::function<double()> read,
                     std::function<void(double)> write, std::function<void()> onChanged)
    : label_(std::move(label)), steps_(std::move(steps)), read_(std::move(read)),
      write_(std::move(write)), onChanged_(std::move(onChanged)) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (steps_.empty() || width() <= 0.0f) {
			return;
		}
		// The step under the pointer, so a two-value control is "click the half you want" rather than a
		// toggle whose next state you have to remember. The same gesture reaches a three-value one.
		const float t = std::clamp(e.position.x / width(), 0.0f, 0.999f);
		const auto picked = static_cast<size_t>(t * static_cast<float>(steps_.size()));
		write_(steps_[std::min(picked, steps_.size() - 1)].value);
		redraw();
		if (onChanged_) {
			// Same reason OrientationRow calls its own: the scope draws the character it is about to
			// hear and only redraws when a PARAMETER moves, and this deliberately is not one.
			onChanged_();
		}
	};
}

size_t TuningRow::index() const {
	if (steps_.empty()) {
		return 0;
	}
	const double live = read_();
	size_t best = 0;
	double bestDistance = std::abs(live - steps_[0].value);
	for (size_t i = 1; i < steps_.size(); ++i) {
		const double distance = std::abs(live - steps_[i].value);
		if (distance < bestDistance) {
			bestDistance = distance;
			best = i;
		}
	}
	return best;
}

void TuningRow::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	if (steps_.empty() || w <= 0.0f) {
		return;
	}
	const float trackY = h * 0.55f;
	const float trackH = std::max(2.0f, h * 0.28f);
	const float cell = w / static_cast<float>(steps_.size());
	const size_t live = index();

	canvas.setColor(kSurfaceRaised);
	canvas.fill(0.0f, trackY, w, trackH);
	canvas.setColor(kValue);
	canvas.fill(cell * static_cast<float>(live), trackY, cell, trackH);

	const visage::Font& face = font(std::max(8.0f, h * 0.34f));
	canvas.setColor(kMuted);
	canvas.text(label_, face, visage::Font::kLeft, 0.0f, 0.0f, w, trackY);
	canvas.text(steps_[live].name, face, visage::Font::kRight, 0.0f, 0.0f, w, trackY);
}

} // namespace feathers::ui
