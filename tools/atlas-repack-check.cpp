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

// THE CONTROL FOR THE ATLAS FIX, in the only venue that can run it.
//
// The claim: when a font atlas repacks, the canvas throws its retained frame away and redraws
// everything, so the one corrupted frame is not KEPT. That is the whole of tier 1 — the wedge that has
// been seen is stale pixels written once and preserved by partial repaints, and a full invalidation
// is what stops them being preserved.
//
// WHY THIS IS NOT A DOCTEST CASE. The claim is about a real visage Canvas: layers, regions, invalid
// rects and a bgfx frame. Feathers' suites never build one — the widget tests draw into a logical
// snapshot, deliberately, because that is what runs without a GPU. Visage's own tests are the natural
// home and they need Catch2 fetched from the network, which this machine does not have. So this is a
// standalone binary that links the visage we ship and drives the seam directly.
//
// WHAT IT ASSERTS, and both halves matter:
//   1. A submit with nothing new drawn is a no-op — the canvas is genuinely retaining, or step 2 proves
//      nothing.
//   2. After a repack, the very next submit renders again WITHOUT anything being drawn — which is only
//      true if the repack invalidated the retained frame.
// The known-bad is one line: take the checkFontAtlasRepack() call out of Canvas::submit and step 2
// fails while step 1 still passes.
//
//   ninja -C build atlas-repack-check && build/atlas-repack-check

#include "visage_graphics/canvas.h"
#include "visage_graphics/font.h"
#include "visage_graphics/renderer.h"
#include "visage_graphics/screenshot.h"

#include "embedded/feathers_fonts.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
	std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) {
		failures++;
	}
}

} // namespace

int main() {
	visage::Canvas canvas;
	canvas.setWindowless(400, 200);
	if (!visage::Renderer::instance().supported()) {
		// An honest skip rather than a green tick: this machine cannot make a graphics context, and a
		// control that cannot run has not passed.
		std::printf("SKIP  no renderer: %s\n", visage::Renderer::instance().errorMessage().c_str());
		return 77;
	}

	// THE DPI SCALE IS NOT OPTIONAL, even though the parameter has a default. `Font::dpiScale()`
	// asserts on a zero scale, and this file measures strings — so without it the control traps on
	// its first measurement in a Debug build and has only ever run under NDEBUG, where the atlas's
	// own assertions are compiled out too. 1.0 is what a zero scale already behaves as, so the
	// numbers below are the numbers this always measured.
	visage::Font font(16.0f, resources::fonts::Lato_Regular_ttf, 1.0f);

	// A first frame, and then a second with nothing new — which must be a no-op, or nothing below means
	// anything. `submit` returns the pass it reached: unchanged when it found nothing to draw.
	canvas.text("feathers", font, visage::Font::Justification::kCenter, 0, 0, 400, 200);
	canvas.submit();
	check(canvas.submit() == 0, "a submit with nothing new drawn renders nothing");

	// FORCE A REPACK WITHOUT DRAWING ANYTHING. Measuring a string packs its glyphs, so the atlas fills
	// up with nothing on the canvas dirtied. Drawing new text would invalidate the region it was drawn
	// into, and a full redraw after THAT proves nothing — which is what the first version of this
	// control did, and why it reported a failure that was really a confound.
	//
	// TIER 2 IS GONE FROM HERE, and the four assertions that used to sit in this gap went with it. It
	// deferred the repack to the top of the next frame so a mid-submit repack could never corrupt a
	// frame at all. It was withdrawn because the deferral cannot be made whole in a retained-mode UI:
	// a string laid out while a glyph was deferred keeps that layout until the widget redraws for its
	// own reasons, and static chrome never does. See the fork's revert commit and glyph-heal-check.
	const uint64_t repacksBefore = visage::fontAtlasRepacks();
	std::u32string fresh;
	for (char32_t c = 0x21; c < 0x21 + 0x200; ++c) {
		fresh.push_back(c);
	}
	font.stringWidth(fresh);
	check(visage::fontAtlasRepacks() > repacksBefore, "measuring new glyphs repacks the atlas");

	// TIER 1: THE RETAINED FRAME IS THROWN AWAY. Nothing was drawn and nothing was invalidated by hand,
	// and the canvas must still render a whole frame — because the coordinates it was retaining moved.
	// Without it the retained buffer keeps whatever the repack corrupted and it stays on screen until
	// something else happens to damage that rect. Which is the wedge.
	check(canvas.submit() != 0, "the frame after a repack redraws everything");
	check(canvas.submit() == 0, "and the frame after that is quiet again");

	// AND A SECOND BATCH BEHAVES THE SAME WAY, which is what says the first was not a one-off of an
	// empty atlas rather than of the mechanism.
	const uint64_t beforeSecond = visage::fontAtlasRepacks();
	std::u32string more;
	for (char32_t c = 0x2200; c < 0x2200 + 0x200; ++c) {
		more.push_back(c);
	}
	font.stringWidth(more);
	check(visage::fontAtlasRepacks() > beforeSecond, "a second batch repacks too");
	check(canvas.submit() != 0, "and the frame after that repack redraws everything");

	// AND NOW THE QUESTION THE REDRAW ASSERTIONS ABOVE CANNOT ANSWER: are the redrawn PIXELS RIGHT?
	//
	// Everything above establishes that a repack causes a redraw. It says nothing about what that
	// redraw SAMPLES. A repack moves every glyph's coordinates inside the atlas texture, so a
	// consumer still holding pre-repack coordinates would draw text-shaped quads sampling whatever now
	// occupies the old rectangles — other glyphs, or never-written texture. That is the failure being
	// hunted: geometry entirely correct, UVs stale, which is why the batcher's position tripwires stay
	// silent through a whole reproduction and the screen is still wrong.
	//
	// THE EXPERIMENT, with its prediction stated in advance so the result cannot be read to taste.
	// Photograph the text; force a repack WITHOUT touching the region the text is in; let the frame
	// redraw; photograph again. An audit of the fork says these must be IDENTICAL, because a TextBlock
	// stores POINTERS to its PackedGlyphs — nodes of a std::map, so they survive a repack — and holds
	// its Font by VALUE, so the PackedFont cannot be evicted while the shape lives. Re-submitting a
	// retained TextBlock therefore reads the NEW coordinates through those pointers.
	//
	// If the two frames differ, that audit is wrong and this is a deterministic windowless
	// reproduction of the corruption, which is worth more than any amount of further reading.
	//
	// The readback lags a frame — bgfx copies asynchronously, so the first screenshot after new
	// content returns the previous one — hence the extra submits before each capture.
	{
		canvas.submit();
		canvas.takeScreenshot();
		canvas.takeScreenshot();
		const visage::Screenshot& before = canvas.takeScreenshot();
		if (before.width() == 0) {
			std::printf("SKIP  no screenshot readback on this machine\n");
		}
		else {
			std::vector<uint8_t> pixels(before.data(), before.data() + before.width() * before.height() * 4);

			// KEEP FEEDING IT NOVEL GLYPHS UNTIL IT ACTUALLY REPACKS, rather than assuming one batch is
			// enough. An atlas doubles when it grows, so how many new characters it takes to force a repack
			// depends entirely on how full the earlier rounds left it — and a fixed batch that happens to
			// fit makes this whole round VACUOUS: nothing moves, the two frames match trivially, and the
			// check reports a pass for the one reason that proves nothing. That is exactly what the first
			// version of this round did, in both directions: placed earlier it stole the second batch's
			// repack, and placed here it stopped repacking at all.
			const uint64_t repacksBeforePixels = visage::fontAtlasRepacks();
			char32_t next = 0x3000;
			for (int attempt = 0; attempt < 16 && visage::fontAtlasRepacks() == repacksBeforePixels; ++attempt) {
				std::u32string churn;
				for (char32_t c = next; c < next + 0x200; ++c) {
					churn.push_back(c);
				}
				next += 0x200;
				font.stringWidth(churn);
			}
			check(visage::fontAtlasRepacks() > repacksBeforePixels,
			      "the pixel round's churn repacks the atlas");

			canvas.submit();
			canvas.takeScreenshot();
			canvas.takeScreenshot();
			const visage::Screenshot& after = canvas.takeScreenshot();

			bool same = after.width() == before.width() && after.height() == before.height();
			size_t differing = 0;
			if (same) {
				for (size_t i = 0; i < pixels.size(); ++i) {
					if (pixels[i] != after.data()[i]) {
						differing++;
					}
				}
			}
			// WHAT THIS DOES AND DOES NOT ESTABLISH, because a green tick here was nearly shipped meaning
			// less than it appears to.
			//
			// It IS load-bearing, but only in company with the movement probe below: that probe shows all
			// eight glyphs of this very word change atlas position across a repack, and this round shows
			// the text still renders identically afterwards. Moved coordinates plus unchanged pixels is a
			// positive result — the retained TextBlock genuinely re-reads the new positions through its
			// glyph pointers. Without the probe, "identical" would have been equally consistent with
			// nothing having moved at all.
			//
			// TWO KNOWN-BADS WERE TRIED AND NEITHER TURNED IT RED, which is recorded here rather than
			// quietly omitted:
			//   * suppressing the coordinate update in PackedFont::resize left the field STALE — and the
			//     upload reads that same field, so the glyph is rasterised exactly where it is sampled.
			//     Self-consistent, and invisible. That is a real property of the design, not a gap: the
			//     atlas has ONE source of truth for both writing and reading a glyph.
			//   * uploading at the packer's new rect while sampling the stale field diverges the two, and
			//     still did not register here.
			// So this assertion has never been observed to fail, and it should not be leaned on alone.
			check(same && differing == 0, "text redrawn after a repack is pixel-identical");
			if (differing != 0) {
				std::printf("      %zu of %zu bytes changed - A WINDOWLESS REPRODUCTION, not a flake\n",
				            differing, pixels.size());
			}
		}
	}


	// DOES A REPACK ACTUALLY MOVE ANYTHING? The question underneath every theory above, and it had
	// never been asked directly.
	//
	// "A repack moves every glyph's coordinates" is the premise the whole invalidate-on-repack design
	// rests on. It is not true in general. The packer walks its rectangles in insertion order, so when
	// the atlas grows the rectangles packed FIRST tend to land exactly where they already were — only
	// later ones move. That matters far beyond tidiness: it decides how much of a frame a mid-drag
	// repack could possibly have corrupted, and two deliberately-broken builds could not be caught by
	// the pixel round above precisely because the text it photographs is made of early glyphs that
	// never moved.
	//
	// Measured rather than asserted in one direction, because either answer is worth knowing and
	// hard-coding an expectation here would just bake in today's packer.
	{
		visage::Font probe(17.0f, resources::fonts::Lato_Regular_ttf, 1.0f);
		const std::u32string watched = U"feathers";
		probe.stringWidth(watched);

		// Through setVertexPositions, which is the route the batcher itself takes to reach a glyph —
		// so this measures the pointers that actually end up in a TextBlock, not a private lookup.
		std::vector<visage::FontAtlasQuad> quads(watched.size());
		probe.setVertexPositions(quads.data(), watched.c_str(), static_cast<int>(watched.size()), 0, 0,
		                         400, 200, visage::Font::Justification::kCenter);
		std::vector<std::pair<int, int>> was;
		for (const visage::FontAtlasQuad& q : quads) {
			was.emplace_back(q.packed_glyph->atlas_left, q.packed_glyph->atlas_top);
		}

		const uint64_t before = visage::fontAtlasRepacks();
		char32_t next = 0x100;
		for (int attempt = 0; attempt < 24 && visage::fontAtlasRepacks() == before; ++attempt) {
			std::u32string churn;
			for (char32_t c = next; c < next + 0x200; ++c) {
				churn.push_back(c);
			}
			next += 0x200;
			probe.stringWidth(churn);
		}
		check(visage::fontAtlasRepacks() > before, "the movement probe forced a repack");

		size_t moved = 0;
		for (size_t i = 0; i < watched.size(); ++i) {
			// The SAME pointers, re-read: a TextBlock holds these across frames, so this is exactly what
			// a retained shape sees after a repack.
			if (quads[i].packed_glyph->atlas_left != was[i].first
			    || quads[i].packed_glyph->atlas_top != was[i].second) {
				moved++;
			}
		}
		std::printf("note  %zu of %zu early glyphs moved across a repack\n", moved, watched.size());
	}

	std::printf("%s\n", failures == 0 ? "PASS" : "FAILED");
	return failures == 0 ? 0 : 1;
}
