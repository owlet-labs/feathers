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

// THE CONTROL FOR THE RAW-IMAGE ENTRY POINT: pixels handed to the canvas come out the colour they
// went in, and a buffer overwritten in place is re-uploaded rather than frozen at its first draw.
//
// THREE THINGS THIS EXISTS TO CATCH, all of which would otherwise be found by eye in a live editor:
//
//   1. BYTE ORDER. The atlas is RGBA8; a caller building pixels out of `visage::Color`-shaped uint32s
//      (0xAARRGGBB) has BGRA8 in memory. Upload those unswizzled and the picture is right in shape
//      and has red and blue exchanged — which looks like a colour-mapping bug in the CALLER and gets
//      hunted there first. Both orders are drawn here and both are checked against the colour that
//      was asked for.
//   2. STALENESS. The atlas keys an image on its data POINTER plus size, so a caller overwriting one
//      buffer in place hands over something the atlas considers unchanged. Without forcing the
//      update for raw images the first upload is the only one and the picture freezes. The second
//      round overwrites the same buffer and expects the new colour.
//   3. ALPHA. The terrain encodes "outside the reachable space" as alpha zero and the void has to
//      show through, so a transparent texel must not paint.
//
//   ninja -C build raw-image-check && build/raw-image-check

#include "visage_graphics/canvas.h"
#include "visage_graphics/renderer.h"
#include "visage_graphics/screenshot.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

constexpr int kMapSize = 8;
constexpr int kCanvasWidth = 400;
constexpr int kCanvasHeight = 200;

// Fills a buffer with one colour, written the way `visage::Color` writes it: 0xAARRGGBB in a uint32,
// which on a little-endian machine is B, G, R, A in memory — i.e. BGRA8.
void fillArgb(std::vector<uint32_t>& map, uint32_t argb) {
	for (uint32_t& texel : map) {
		texel = argb;
	}
}

unsigned int drawAndRead(visage::Canvas& canvas, const std::vector<uint32_t>& map,
                         visage::ImageFormat format) {
	canvas.clearDrawnShapes();
	canvas.setColor(0xff000000);
	canvas.fill(0, 0, kCanvasWidth, kCanvasHeight);
	canvas.setColor(0xffffffff);
	canvas.imageRaw(reinterpret_cast<const unsigned char*>(map.data()), kMapSize, kMapSize, format, 0,
	                0, kCanvasWidth, kCanvasHeight);

	// The readback lags a frame and the frame buffer is never cleared between frames, so the same
	// content is drawn until the readback has certainly caught up. Both traps are recorded in
	// tools/batch-overflow-check, which learned them the hard way.
	canvas.takeScreenshot();
	canvas.takeScreenshot();
	const visage::Screenshot& shot = canvas.takeScreenshot();
	if (shot.width() == 0) {
		return 0;
	}

	const uint8_t* pixel = shot.data() + (100 * shot.width() + 200) * 4;
	return (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
}

} // namespace

int main() {
	visage::Canvas canvas;
	canvas.setWindowless(kCanvasWidth, kCanvasHeight);
	if (!visage::Renderer::instance().supported()) {
		std::printf("SKIP  no renderer: %s\n", visage::Renderer::instance().errorMessage().c_str());
		return 77;
	}

	// ONE BUFFER FOR THE WHOLE RUN, which is the contract the entry point documents: the pointer is
	// the identity, so a caller with changing contents overwrites in place. It is also what makes
	// round two a test of staleness rather than of a second image.
	std::vector<uint32_t> map(kMapSize * kMapSize);

	// 1. BGRA8: pixels written as 0xAARRGGBB, the spelling visage::Color uses. A solid red map must
	// read back red, not blue.
	fillArgb(map, 0xffff0000);
	const unsigned int red = drawAndRead(canvas, map, visage::ImageFormat::BGRA8);
	std::printf("      asked for ff0000, read %06x\n", red);
	check(red == 0xff0000, "a BGRA8 buffer draws the colour it was given");

	// 2. STALENESS. Same buffer, same pointer, new contents. The atlas cannot see that the pixels
	// behind the pointer moved, so this only passes if raw images force their upload.
	fillArgb(map, 0xff00ff00);
	const unsigned int green = drawAndRead(canvas, map, visage::ImageFormat::BGRA8);
	std::printf("      overwrote in place, read %06x\n", green);
	check(green == 0x00ff00, "overwriting the buffer in place redraws the new contents");

	// 3. THE OTHER ORDER, so the format parameter is doing something rather than being ignored. The
	// same bytes read as RGBA8 must come out with red and blue exchanged.
	fillArgb(map, 0xffff0000);
	const unsigned int swapped = drawAndRead(canvas, map, visage::ImageFormat::RGBA8);
	std::printf("      same bytes as RGBA8, read %06x\n", swapped);
	check(swapped == 0x0000ff, "and reading the same bytes as RGBA8 exchanges red and blue");

	// 4. ALPHA IS RESPECTED. A fully transparent map must leave the black underneath alone rather
	// than painting over it — the terrain's void depends on it.
	fillArgb(map, 0x00ffffff);
	const unsigned int clear = drawAndRead(canvas, map, visage::ImageFormat::BGRA8);
	std::printf("      transparent map over black, read %06x\n", clear);
	check(clear == 0x000000, "a transparent texel paints nothing");

	std::printf("%s\n", failures == 0 ? "PASS" : "FAILED");
	return failures == 0 ? 0 : 1;
}
