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

#include <visage/ui.h>

#include <cstdint>
#include <functional>
#include <vector>

// THE LIBRA MARK AS A DOOR (backlog LG). Feathers' header draws the mark itself and takes a click on it;
// the rack has no header, so this is the same mark as a frame of its own, for the rack's macro row.
// Clicking it opens the settings screen.

namespace feathers::ui {

/// THE MARK AS PIXELS — `width`×`height` NATIVE pixels, BGRA8 in memory (0xAARRGGBB), white with the
/// coverage in alpha. The header's own rasteriser (header_bar.cpp says why it is ours and not visage's
/// path fill), shared so both doors are one mark.
std::vector<uint32_t> rasteriseLibraMark(int width, int height);
/// Width over height, from the asset's viewBox `800 135 109 143`.
inline constexpr float kLibraMarkAspect = 109.0f / 143.0f;

class LibraDoor : public visage::Frame {
public:
	void draw(visage::Canvas& canvas) override;
	void mouseDown(const visage::MouseEvent& e) override;

	/// Called on a click.
	std::function<void()> onClick;

	/// Where the mark sits in the frame: as tall as fits, centred, at the asset's aspect.
	visage::Bounds markBounds() const;

private:
	std::vector<uint32_t> pixels_;
	uint64_t pixelsSerial_ = 0; ///< `freshPixelSerial`, stamped when `pixels_` is rasterised
	int pixelWidth_ = 0;
	int pixelHeight_ = 0;
};

} // namespace feathers::ui
