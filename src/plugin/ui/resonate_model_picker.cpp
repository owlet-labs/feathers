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

#include "plugin/ui/resonate_model_picker.h"

#include "fx/resonate/resonate_types.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace rs = fx::resonate;

static_assert(ResonateModelPicker::kCells == static_cast<size_t>(rs::Model::kCount),
              "one pictogram per model — a sixth model needs a sixth picture");

ResonateModelPicker::ResonateModelPicker(device::Modulatable& model) : model_(model) {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		const size_t at = cellAt(e.position);
		if (at >= kCells || at == selected()) {
			return;
		}
		model_.beginGesture();
		model_.setValue(static_cast<double>(at));
		model_.endGesture();
		redraw();
	};
	// A SCALE CHANGE MOVES NOTHING LOGICAL — see the ring view's note.
	onDpiChange() += [this] { setBounds(bounds()); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

const char* ResonateModelPicker::cellTooltip(size_t i) {
	// EACH CELL NAMES ITS MODEL AND SAYS, IN ONE LINE, WHAT IT DOES TO THE MOTION — not the pitch, which is
	// the scan rate, and not an instrument it imitates (the Model declaration's rule).
	// THE NAME IS THE DESCRIPTOR'S (`kModelNames`), so a renamed model renames its tooltip too.
	static constexpr const char* kWhat[kCells] = {
		"a closed loop of masses; the shape travels round it",
		"stiff, so the shape bends in broad, slow curves",
		"a chain pinned at both ends, read out and back",
		"a sheet read round a circle; a disturbance travels before it is heard",
		"springs that tighten as they stretch; hard playing moves tighter and faster",
	};
	static const std::array<std::string, kCells> kLines = [] {
		std::array<std::string, kCells> out;
		for (size_t c = 0; c < kCells; ++c) {
			out[c] = std::string(rs::kModelNames[c]) + " — " + kWhat[c];
		}
		return out;
	}();
	return i < kCells ? kLines[i].c_str() : nullptr;
}

const char* ResonateModelPicker::tooltipAt(visage::Point local) const {
	const size_t at = cellAt(local);
	return at < kCells ? cellTooltip(at) : tooltipOf(&model_);
}

visage::Bounds ResonateModelPicker::cellBounds(size_t i) const {
	const float w = static_cast<float>(width());
	const float cell = std::max(0.0f, (w - kGap * static_cast<float>(kCells - 1)) / static_cast<float>(kCells));
	return { (cell + kGap) * static_cast<float>(i), 0.0f, cell, static_cast<float>(height()) };
}

size_t ResonateModelPicker::cellAt(visage::Point local) const {
	for (size_t i = 0; i < kCells; ++i) {
		if (cellBounds(i).contains(local)) {
			return i;
		}
	}
	return kCells;
}

size_t ResonateModelPicker::selected() const {
	return static_cast<size_t>(std::clamp(static_cast<int>(std::lround(model_.value())), 0,
	                                      static_cast<int>(kCells) - 1));
}

void ResonateModelPicker::drawPictogram(visage::Canvas& canvas, size_t i, const visage::Bounds& box,
                                        uint32_t colour) {
	const float cx = box.x() + box.width() * 0.5f;
	const float cy = box.y() + box.height() * 0.5f;
	const float h = std::min(box.height() - 8.0f, box.width() - 10.0f);
	const float w = std::min(box.width() - 12.0f, h * 1.8f);
	canvas.setColor(colour);
	switch (static_cast<rs::Model>(i)) {
	case rs::Model::Ring:
		// A LOOP.
		canvas.ring(cx - h * 0.5f, cy - h * 0.5f, h, 1.5f);
		break;
	case rs::Model::Bar: {
		// A FREE BAR, gently bent, its ends loose.
		const float x0 = cx - w * 0.5f;
		constexpr int kSteps = 8;
		for (int s = 0; s < kSteps; ++s) {
			const float t0 = static_cast<float>(s) / kSteps;
			const float t1 = static_cast<float>(s + 1) / kSteps;
			const float y0 = cy + h * 0.22f * std::cos(3.14159265f * 2.0f * t0);
			const float y1 = cy + h * 0.22f * std::cos(3.14159265f * 2.0f * t1);
			canvas.segment(x0 + w * t0, y0, x0 + w * t1, y1, 1.5f, true);
		}
		break;
	}
	case rs::Model::Tube: {
		// A CHAIN PINNED AT BOTH ENDS: an arch between two posts.
		const float x0 = cx - w * 0.5f;
		constexpr int kSteps = 8;
		for (int s = 0; s < kSteps; ++s) {
			const float t0 = static_cast<float>(s) / kSteps;
			const float t1 = static_cast<float>(s + 1) / kSteps;
			const float y0 = cy - h * 0.3f * std::sin(3.14159265f * t0);
			const float y1 = cy - h * 0.3f * std::sin(3.14159265f * t1);
			canvas.segment(x0 + w * t0, y0, x0 + w * t1, y1, 1.5f, true);
		}
		canvas.fill(x0 - 1.0f, cy - h * 0.35f, 2.0f, h * 0.7f);
		canvas.fill(x0 + w - 1.0f, cy - h * 0.35f, 2.0f, h * 0.7f);
		break;
	}
	case rs::Model::Membrane: {
		// A GRID — three by three.
		const float cell = h / 3.0f;
		const float x0 = cx - h * 0.5f;
		const float y0 = cy - h * 0.5f;
		for (int r = 0; r < 3; ++r) {
			for (int c = 0; c < 3; ++c) {
				canvas.roundedRectangle(x0 + cell * static_cast<float>(c) + 0.75f,
				                        y0 + cell * static_cast<float>(r) + 0.75f, cell - 1.5f, cell - 1.5f, 1.0f);
			}
		}
		break;
	}
	case rs::Model::Stiffening: {
		// A SPRING WHOSE COILS TIGHTEN — it stiffens as it stretches.
		const float x0 = cx - w * 0.5f;
		float x = x0;
		float gap = w * 0.22f;
		bool up = true;
		while (x < x0 + w - 0.5f) {
			const float next = std::min(x0 + w, x + gap);
			canvas.segment(x, up ? cy + h * 0.3f : cy - h * 0.3f, next, up ? cy - h * 0.3f : cy + h * 0.3f, 1.3f,
			               true);
			x = next;
			gap = std::max(w * 0.07f, gap * 0.72f);
			up = !up;
		}
		break;
	}
	case rs::Model::kCount: break;
	}
}

void ResonateModelPicker::draw(visage::Canvas& canvas) {
	const size_t on = selected();
	for (size_t i = 0; i < kCells; ++i) {
		const visage::Bounds box = cellBounds(i);
		const bool sel = i == on;
		canvas.setColor(sel ? kSurfaceRaised : kVoid);
		canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 3.0f);
		if (sel) {
			canvas.setColor(kAccent);
			canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 3.0f, 1.0f);
		}
		drawPictogram(canvas, i, box, sel ? kInk : kMuted);
	}
}

} // namespace feathers::ui
