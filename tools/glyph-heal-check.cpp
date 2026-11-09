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

// NO GLYPH IS EVER HANDED OUT UNPLACED, and no glyph changes its mind about how wide it is.
//
// THIS FILE IS THE RECORD OF WHY TIER 2 WAS WITHDRAWN. That change deferred an atlas repack to the
// top of the next frame, so a repack could never land mid-submit. To do it, a glyph that could not be
// placed was handed back as a null glyph for one frame. Two things went wrong:
//
//   * `PackedAtlasMap::addRect` registers the id and pushes a rect at (0, 0) BEFORE the packer places
//     it, and `packGlyph` tested `!hasId(character) && !addRect(...)`. On the SECOND lookup of a
//     deferred glyph `hasId` was already true, so the deferral was skipped and the glyph took the
//     unplaced (0, 0) — reading as packed forever, sampling the origin, and rasterizing its bitmap
//     over whichever glyph legitimately lives there. The first occurrence of a character came out
//     missing and the second came out wrong: "GAMMA" laid out as "GAMA".
//   * Fixing that made it WORSE in a live host, which is the finding that decided it. An honest
//     deferral leaves the string laid out without that glyph, and a TextBlock's layout is only rebuilt
//     when its widget redraws. Static chrome never redraws, so "one frame of narrower text" is really
//     "until something else happens to that widget" — for a panel label, never. Measured in guihost:
//     with the deferral honest, almost every label in the editor was blank.
//
// So the repack happens inline again, where it always did. Tier 1 — throwing away the retained frame
// when the atlas repacks — is what stops a corrupted frame being KEPT, and it stays.
//
// WHAT THIS ASSERTS, and either failing means a glyph is being handed out before it has a home:
//   1. Every glyph measures a real width on its FIRST lookup. No nulls, no deferral.
//   2. No glyph changes width without a repack between the two measurements.
//   3. THE PIXELS. Text drawn before the atlas has settled looks the same as the same text drawn
//      after it has. Width tests cannot see a glyph drawn from the WRONG PLACE — an unplaced glyph
//      still reports its FreeType width while sampling the atlas origin — so the only assertion that
//      covers placement is one that looks at what was actually drawn.
//
// WHAT THE KNOWN-BADS SAID, because a control's scope is worth writing down rather than assuming:
//
//   * Restoring tier-2's deferral fails checks 1 and 2 comprehensively — every one of A, M, G, 0, z
//     measured zero and then non-zero with no repack between. That is the failure this file exists
//     for, and it is the reason to keep it.
//   * Making the pack never happen at all — `addRect` without the `resize()` that follows it — PASSES
//     every check here. It should not, and it is worth knowing why: it puts every glyph at the atlas
//     origin uniformly, so check 3's early-vs-late comparison sees two equally wrong pictures and
//     calls them equal, while checks 1 and 2 only ever look at widths, which come from FreeType and
//     not from the atlas. Catching a uniformly wrong atlas needs an absolute reference — a golden
//     image, platform-pinned — which is a different test from this one.
//
// So: this file guards against a glyph being handed out DIFFERENTLY at different times. It does not
// guard against every glyph being wrong in the same way. Do not read a pass here as "the atlas is
// correct".
//
//   ninja -C build glyph-heal-check && build/glyph-heal-check

#include "visage_graphics/canvas.h"
#include "visage_graphics/font.h"
#include "visage_graphics/renderer.h"
#include "visage_graphics/screenshot.h"

#include "embedded/feathers_fonts.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

float widthOf(visage::Font& font, char32_t character) {
	return font.stringWidth(std::u32string(1, character));
}

} // namespace

int main() {
	visage::Canvas canvas;
	canvas.setWindowless(400, 200);
	if (!visage::Renderer::instance().supported()) {
		std::printf("SKIP  no renderer: %s\n", visage::Renderer::instance().errorMessage().c_str());
		return 77;
	}

	// THE DPI SCALE IS NOT OPTIONAL here, even though the parameter has a default: `Font::dpiScale()`
	// asserts on a zero scale and every measurement below goes through it, so without this the whole
	// control traps on its first lookup in a Debug build. It has therefore only ever run under
	// NDEBUG — where the atlas's own duplicate-registration assertion is compiled out as well, which
	// is precisely the assertion this file exists to be downstream of. 1.0 is what a zero scale
	// already behaves as, so the widths are unchanged.
	visage::Font font(16.0f, resources::fonts::Lato_Regular_ttf, 1.0f);

	// ONE CHARACTER AT A TIME, so each character's FIRST lookup is what gets observed. Measuring a
	// long string first would spend every first lookup inside it and leave nothing to see — which is
	// exactly how the bug this file records survived a control that measured 0x200 characters in one
	// call.
	// THE COMBINING DIACRITICAL MARKS (U+0300..U+036F) ARE NOT ASKED FOR A WIDTH, because zero IS their
	// width. visage v1.0.0 (2026-11-05) made this matter: the Lato we embed carries none of them
	// (MEASURED with FreeType: index 0 for every one), and v1.0.0 sends a missing glyph to a fontconfig
	// system fallback, whose marks advance by zero as Unicode intends. The old pin drew Lato's .notdef
	// box at 9 px for them, which is why this check never saw them. A mark measuring zero first and
	// non-zero later is still caught by check 2, and check 4 below asks the mark question by value.
	int nulls = 0;
	int unstable = 0;
	for (char32_t c = 0x21; c < 0x21 + 0x300; ++c) {
		const bool mark = c >= 0x300 && c <= 0x36f;
		const uint64_t repacks = visage::fontAtlasRepacks();
		const float first = widthOf(font, c);
		if (first == 0.0f && !mark) {
			if (nulls < 64) {
				std::printf("      U+%04X measured zero on its first lookup\n", static_cast<unsigned>(c));
			}
			nulls++;
		}

		// Asked again with nothing in between. A repack IS allowed to change the answer — an inline
		// pack is the whole design — so only a change without one is a fault.
		const float again = widthOf(font, c);
		if (again != first && visage::fontAtlasRepacks() == repacks) {
			if (unstable == 0) {
				std::printf("      U+%04X changed width with no repack: %.2f then %.2f\n",
				            static_cast<unsigned>(c), first, again);
			}
			unstable++;
		}
	}

	check(nulls == 0, "every glyph (bar combining marks) has a real width on its first lookup");
	check(unstable == 0, "and no glyph changes width without a repack");

	// 4. A COMBINING MARK ADDS NOTHING TO ITS BASE — by value, on a fresh font so the mark's lookup is
	// its first. This depends on the HOST having a fallback font that carries U+0301 (fontconfig);
	// without one the mark is .notdef and this reports a real difference, which is information too.
	{
		visage::Font marks(19.0f, resources::fonts::Lato_Regular_ttf, 1.0f);
		const float base = marks.stringWidth(U"e");
		const float withMark = marks.stringWidth(U"é");
		std::printf("      e = %.3f, e + U+0301 = %.3f\n", base, withMark);
		check(base > 0.0f && withMark == base, "a combining mark adds no width to its base");
	}

	// 3. THE SAME TEXT, DRAWN EARLY AND DRAWN LATE, MUST LOOK THE SAME.
	//
	// This is the assertion that corresponds to what a person sees. A glyph handed out unplaced still
	// measures its true width — the width comes from FreeType, not from the atlas rect — so it slips
	// past every check above while drawing from the atlas origin. Comparing ink catches it.
	//
	// A fresh font, so its atlas starts empty and the first draw is the risky one. GAMMA is not an
	// arbitrary choice: a doubled letter is where the fault showed, because the two occurrences hit
	// different atlas states.
	visage::Font fresh(16.0f, resources::fonts::Lato_Regular_ttf, 1.0f);
	auto drawSample = [&canvas, &fresh]() {
		canvas.clearDrawnShapes();
		canvas.setColor(0xff000000);
		canvas.fill(0, 0, 400, 200);
		canvas.setColor(0xffffffff);
		canvas.text(U"GAMMA WEATHER 0123", fresh, visage::Font::Justification::kCenter, 0, 0, 400, 200);
		canvas.takeScreenshot();
		canvas.takeScreenshot();
		const visage::Screenshot& shot = canvas.takeScreenshot();
		return std::string(reinterpret_cast<const char*>(shot.data()),
		                   static_cast<size_t>(shot.width()) * shot.height() * 4);
	};

	const std::string early = drawSample();

	// Settle the atlas thoroughly, so the second draw is definitely from a stable packing.
	std::u32string lots;
	for (char32_t c = 0x21; c < 0x21 + 0x200; ++c) {
		lots.push_back(c);
	}
	fresh.stringWidth(lots);
	const std::string late = drawSample();

	check(!early.empty(), "the sample rendered at all");
	check(early == late, "text drawn before the atlas settled looks like text drawn after");

	std::printf("%s\n", failures == 0 ? "PASS" : "FAILED");
	return failures == 0 ? 0 : 1;
}
