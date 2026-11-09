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

#include "plugin/ui/libra_door.h"
#include "plugin/ui/pixel_serial.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

visage::Bounds LibraDoor::markBounds() const {
	const float h = std::min(height(), width() / kLibraMarkAspect);
	const float w = h * kLibraMarkAspect;
	return { (width() - w) * 0.5f, (height() - h) * 0.5f, w, h };
}

void LibraDoor::draw(visage::Canvas& canvas) {
	const visage::Bounds box = markBounds();
	const float scale = canvas.dpiScale();
	if (scale <= 0.0f || box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}
	// ONE IMAGE PIXEL PER NATIVE PIXEL on the native grid — the header's `drawLogo`, for the same reason.
	const int w = std::max(1, static_cast<int>(std::lround(box.width() * scale)));
	const int h = std::max(1, static_cast<int>(std::lround(box.height() * scale)));
	const float x = std::round(box.x() * scale) / scale;
	const float y = std::round(box.y() * scale) / scale;
	if (w != pixelWidth_ || h != pixelHeight_) {
		pixels_ = rasteriseLibraMark(w, h);
		pixelsSerial_ = freshPixelSerial();
		pixelWidth_ = w;
		pixelHeight_ = h;
	}
	canvas.setColor(0xffffffffu);
	canvas.imageRaw(reinterpret_cast<const unsigned char*>(pixels_.data()), w, h, visage::ImageFormat::BGRA8, x, y,
	                static_cast<float>(w) / scale, static_cast<float>(h) / scale, pixelsSerial_);
}

void LibraDoor::mouseDown(const visage::MouseEvent&) {
	if (onClick) {
		onClick();
	}
}

} // namespace feathers::ui
