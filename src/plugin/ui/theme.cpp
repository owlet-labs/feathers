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

#include "plugin/ui/theme.h"

#include <algorithm>

#include <embedded/feathers_fonts.h>
#include <visage_graphics/font.h>

#include <cstdint>
#include <set>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace feathers::ui {

namespace {

/// DELIBERATELY IMMORTAL, and the leak is the point.
///
/// A visage::Font's destructor hands its packed font back to visage's FontCache, and that cache is
/// itself a function-local static (visage_graphics/font.h). Ours is constructed first — visage's is
/// built INSIDE our first font() call, when the first Font is made — so at process exit visage's is
/// destroyed FIRST and every Font left in here would then reach into a destroyed singleton. That is
/// a use-after-destruction on reused heap, which is why it crashed at exit roughly two runs in five
/// and looked like nothing at all the rest of the time.
///
/// Never destroying this map means no Font destructor can ever run during static teardown, whatever
/// order the module unwinds in. clearFontCache() below is what keeps that from being a real leak:
/// the editor empties it while visage's cache is still alive.
std::map<int, visage::Font>& fontCache() {
	static auto* cache = new std::map<int, visage::Font>();
	return *cache;
}

/// One UTF-8 character, as its codepoint and as the bytes it came from.
///
/// Iterating a std::string by `char` would split a multi-byte character into pieces and draw each
/// piece as its own glyph — which is exactly what happened to the byline's `·`, rendering as a stray
/// quote. Tracking has to advance per CHARACTER, so the string has to be read as characters.
struct Utf8Character {
	char32_t code = 0;
	std::string bytes;
};

std::vector<Utf8Character> decodeUtf8(const std::string& text) {
	std::vector<Utf8Character> characters;
	for (size_t i = 0; i < text.size();) {
		const auto lead = static_cast<unsigned char>(text[i]);
		size_t length = 1;
		char32_t code = lead;
		if ((lead & 0x80u) == 0x00u) {
			length = 1;
			code = lead;
		}
		else if ((lead & 0xE0u) == 0xC0u) {
			length = 2;
			code = lead & 0x1Fu;
		}
		else if ((lead & 0xF0u) == 0xE0u) {
			length = 3;
			code = lead & 0x0Fu;
		}
		else if ((lead & 0xF8u) == 0xF0u) {
			length = 4;
			code = lead & 0x07u;
		}
		if (i + length > text.size()) {
			// Truncated tail: take the byte as-is rather than reading past the end.
			length = 1;
			code = lead;
		}
		else {
			for (size_t k = 1; k < length; ++k) {
				code = (code << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
			}
		}
		characters.push_back({code, text.substr(i, length)});
		i += length;
	}
	return characters;
}

/// A string decoded into characters, each with the width the font gives it.
///
/// CACHED, because tracked text is now on an every-frame path. The Perform surface repaints
/// continuously while it is visible, and its caption went through decodeUtf8 TWICE a frame — once to
/// measure and once to draw — each time allocating a vector, a string per character, and asking
/// visage for a width per character. None of that varies: the same words in the same font are the
/// same glyphs at the same widths for the life of the editor.
///
/// Keyed on the font's rasterised size as well as the text, because the widths come from the font.
struct TrackedText {
	std::vector<Utf8Character> characters;
	std::vector<float> advances;
};

/// **A LEAST-RECENTLY-USED CACHE, NOT A FILL-THEN-STOP ONE** (2026-11-09). It used to cache the first 64 strings it
/// ever saw and then re-measure every other string on every draw, for the rest of the process. So a session got slower
/// the more it showed: on the Mac, grabbing a new control was slow while recently grabbed controls
/// responded faster, and a plugin restart made the UI faster again. Now a string drawn on this frame
/// is at the front and is never measured again while it stays in use; what is evicted is what has gone longest unseen,
/// and a readout counting up under a drag only costs the strings it actually shows.
///
/// THE CAP: a busy session's working set measured MUCH smaller (see `kMaxCachedStrings`'s note in the commit and
/// `test_chrome_hue`), and an entry is a few hundred bytes, so the cap is set an order of magnitude above it.
constexpr size_t kMaxCachedStrings = 4096;

/// Keyed on the font's rasterised size and its dpi scale as well as the text, because the widths come from the
/// rasterised face (one typeface — Lato — is the editor's only face; `font()` is keyed on size alone for the same
/// reason).
struct TrackedKey {
	int size = 0;
	float dpi = 1.0f;
	std::string text;
	bool operator<(const TrackedKey& o) const {
		if (size != o.size) {
			return size < o.size;
		}
		if (dpi != o.dpi) {
			return dpi < o.dpi;
		}
		return text < o.text;
	}
};

struct TrackedEntry {
	TrackedKey key;
	TrackedText measured;
};

struct TrackedCache {
	std::list<TrackedEntry> order; ///< most recently used first
	std::map<TrackedKey, std::list<TrackedEntry>::iterator> index;
	uint64_t measures = 0;  ///< every `measure`, i.e. every miss
	uint64_t evictions = 0;
	/// THE AUDIT (tests): while on, every key ever measured is remembered, and a measure of one measured before is a
	/// RE-measure. With nothing evicted there must be none: each string is measured once, however often it is drawn.
	bool audit = false;
	std::set<TrackedKey> everMeasured;
	uint64_t remeasures = 0;
	void clear() {
		index.clear();
		order.clear();
	}
};

TrackedCache& trackedCache() {
	static auto* cache = new TrackedCache();
	return *cache;
}

TrackedText measure(const visage::Font& face, const std::string& text) {
	TrackedText measured;
	measured.characters = decodeUtf8(text);
	measured.advances.reserve(measured.characters.size());
	for (const Utf8Character& character : measured.characters) {
		measured.advances.push_back(face.stringWidth(&character.code, 1));
	}
	++trackedCache().measures;
	return measured;
}

/// The decoded, measured form of a string — from the cache when it is there (and moved to the front), measured and kept
/// at the front when it is not, evicting the least recently used past the cap. The reference is valid until the next
/// call, which is all a caller holds it for.
const TrackedText& tracked(const visage::Font& face, const std::string& text) {
	TrackedCache& cache = trackedCache();
	TrackedKey key { static_cast<int>(face.size() + 0.5f), face.dpiScale(), text };
	if (const auto existing = cache.index.find(key); existing != cache.index.end()) {
		cache.order.splice(cache.order.begin(), cache.order, existing->second);
		return existing->second->measured;
	}
	if (cache.audit && !cache.everMeasured.insert(key).second) {
		++cache.remeasures;
	}
	cache.order.push_front(TrackedEntry { key, measure(face, text) });
	cache.index.emplace(std::move(key), cache.order.begin());
	while (cache.order.size() > kMaxCachedStrings) {
		cache.index.erase(cache.order.back().key);
		cache.order.pop_back();
		++cache.evictions;
	}
	return cache.order.front().measured;
}

} // namespace

const visage::Font& font(float size) {
	// Keyed on the rasterised size, since that is what visage packs a font at. Main thread only —
	// every draw in this editor arrives through the host's single callback — so no lock.
	std::map<int, visage::Font>& cache = fontCache();

	const int key = static_cast<int>(size + 0.5f);
	const auto existing = cache.find(key);
	if (existing != cache.end()) {
		return existing->second;
	}
	// THE DPI SCALE IS STATED, at the value a font that leaves it unset already behaves as.
	// `Font::dpiScale()` asserts on a zero scale and then falls back to 1.0, and every width measured
	// here goes through it — so leaving it unset is a Debug trap for the sake of a number that was
	// 1.0 either way. It cost quad-census its ability to run under Debug at all, which is where
	// visage's own atlas assertions live.
	//
	// 1.0 rather than the editor's scale because these fonts are MEASURED here and DRAWN by visage,
	// which applies the canvas scale itself at draw time (`Canvas::text` calls `withDpiScale`). The
	// measurement belongs in the same unscaled space the layout code works in.
	return cache
	    .emplace(key, visage::Font(static_cast<float>(key), resources::fonts::Lato_Regular_ttf, 1.0f))
	    .first->second;
}

int forceFontAtlasRepack() {
	// A SIZE AND A CHARACTER BLOCK NEITHER THIS EDITOR NOR AN EARLIER CALL HAS TOUCHED. The sizes walk
	// downward from well above anything the layout asks for, so they cannot collide with a real
	// widget's font and change what is drawn; the characters walk upward through a private range for
	// the same reason. Both advance on every call, which is what keeps the tenth call as effective as
	// the first.
	static int nextSize = 200;
	static char32_t nextCharacter = 0x2400;

	const uint64_t before = visage::fontAtlasRepacks();

	// Deliberately NOT through font() above: that cache is the editor's, and filling it with probe
	// sizes would leave them alive for the rest of the session. This Font is local, and the PackedFont
	// it mints is handed back when it dies.
	visage::Font probe(static_cast<float>(nextSize--), resources::fonts::Lato_Regular_ttf, 1.0f);

	// Measuring packs glyphs exactly as drawing does, and dirties nothing.
	std::u32string novel;
	for (int i = 0; i < 0x200; ++i) {
		novel.push_back(nextCharacter++);
	}
	probe.stringWidth(novel);

	return static_cast<int>(visage::fontAtlasRepacks() - before);
}

int forceEstablishedAtlasRepack(float size) {
	// THE EVENT THE BIRTH LEVER CANNOT STAGE, and the one still without a verdict.
	//
	// forceFontAtlasRepack above mints a NEW font, so its repack moves one glyph and then two, in an
	// atlas nothing is drawing from. An armed matrix has already landed that inside a drag in every
	// knob-and-surface combination and found every frame clean — which is coherent, because almost
	// nothing was in flight to corrupt.
	//
	// This is the other event: novel characters at a size the editor is ALREADY DRAWING WITH, so an
	// established atlas full of resident glyphs repacks and every one of them moves while real text
	// quads are in flight. That is the shape the corruption hypothesis actually needs.
	//
	// Deliberately THROUGH font(), unlike the birth lever: the whole point is to hit the very
	// PackedFont the widgets are using, and font() is where the editor's are kept. Pass a size the
	// editor really draws — the atlas trace's "FONT size N CREATED" lines name them.
	static char32_t nextCharacter = 0x4000;

	const uint64_t before = visage::fontAtlasRepacks();
	const visage::Font& established = font(size);

	// KEEP GOING UNTIL IT ACTUALLY REPACKS. How many novel characters that takes depends entirely on
	// how much room the atlas happens to have, and a fixed batch that fits makes the lever a silent
	// no-op — the failure that has already made two controls in this project vacuous.
	for (int attempt = 0; attempt < 32 && visage::fontAtlasRepacks() == before; ++attempt) {
		std::u32string novel;
		for (int i = 0; i < 0x200; ++i) {
			novel.push_back(nextCharacter++);
		}
		established.stringWidth(novel);
	}

	return static_cast<int>(visage::fontAtlasRepacks() - before);
}

size_t trackedTextCacheSize() noexcept { return trackedCache().order.size(); }
size_t trackedTextCacheCapacity() noexcept { return kMaxCachedStrings; }
uint64_t trackedTextMeasures() noexcept { return trackedCache().measures; }
uint64_t trackedTextEvictions() noexcept { return trackedCache().evictions; }

void setTrackedTextAudit(bool on) {
	TrackedCache& cache = trackedCache();
	cache.audit = on;
	cache.everMeasured.clear();
	cache.remeasures = 0;
}

uint64_t trackedTextRemeasures() noexcept { return trackedCache().remeasures; }

void clearFontCache() {
	// Called when the editor is destroyed, which is the last moment visage's own font cache is
	// guaranteed to be alive to receive them back.
	fontCache().clear();
	// The measured widths came from those fonts, so they go at the same moment. Nothing in there
	// holds a visage object — it is characters and floats — so this is tidiness rather than the
	// destruction-order rule the font cache exists for.
	trackedCache().clear();
}

float trackedWidth(const visage::Font& face, const std::string& text, float tracking) {
	if (text.empty()) {
		return 0.0f;
	}
	const float spacing = tracking * static_cast<float>(face.size());
	const TrackedText& measured = tracked(face, text);
	float width = 0.0f;
	for (const float advance : measured.advances) {
		width += advance + spacing;
	}
	// The trailing gap is spacing after the last glyph, which CSS also adds and which would push
	// centred text off-centre by half of it if it were kept.
	return width - spacing;
}

void drawBodyScrollBar(visage::Canvas& canvas, visage::Bounds body, float scroll, float content) {
	constexpr float kBarWidth = 3.0f;
	constexpr float kInset = 2.0f;
	if (content <= body.height() || body.height() <= 0.0f) {
		return; // NOTHING TO SCROLL, SO NOTHING TO SAY. See the header on "if applicable".
	}
	const float visible = body.height() / content;
	const float thumb = std::max(12.0f, body.height() * visible);
	const float travel = body.height() - thumb;
	const float maxScroll = std::max(1.0f, content - body.height());
	const float at = travel * std::clamp(scroll / maxScroll, 0.0f, 1.0f);
	const float x = body.right() - kInset - kBarWidth;
	canvas.setColor(withAlpha(kLine, 0.5f));
	canvas.roundedRectangle(x, body.y(), kBarWidth, body.height(), kBarWidth * 0.5f);
	canvas.setColor(withAlpha(kMuted, 0.9f));
	canvas.roundedRectangle(x, body.y() + at, kBarWidth, thumb, kBarWidth * 0.5f);
}

CardFrame cardFrameOf(visage::Bounds card, size_t outputCount, CardHeadEdge edge,
                      size_t outputRows) {
	// THE OUTPUTS STRIP IS TAKEN FROM THE BOTTOM FIRST, so the body is what is left rather than the
	// strip being laid over a body that already used the space. A card whose picture ran under its own
	// output arms would be the same class of overlap as a head drawn over its buttons.
	//
	// **AND THE STRIP STAYS AT THE FOOT IN BOTH ORIENTATIONS.** Outputs sit at the bottom of the card by
	// design; the HEAD flips with the lane and the strip does not, because "the foot of the card" is
	// where a hand looks for what a device publishes whichever way the cards run.
	// **ONE ROW PER `outputRows`, so a device with more chips than fit on a line gets a taller foot
	// rather than chips drawn off the card.** See the declaration for what went wrong with one row.
	const float wanted =
	    outputCount > 0 ? kCardOutputStripHeight * static_cast<float>(std::max<size_t>(outputRows, 1))
	                    : 0.0f;
	// **AND IT CANNOT BE TALLER THAN THE ROOM THE HEAD LEAVES.** A host whose card GROWS to fit its
	// content — both of Feathers' lanes — never reaches this; the rack's does not, because it grants
	// `min(kCardUnitHeight, laneHeight)` and a short window makes that small. Without the clamp such a
	// card gets an outputs rectangle hanging off its own bottom edge, which is the very "chips drawn
	// outside the card" this strip exists to end, arriving the other way up.
	//
	// **THE CLAMP KEEPS THE RECTANGLE INSIDE THE CARD AND THAT IS ALL IT DOES.** It does NOT keep the
	// CHIPS inside — an earlier version of this comment claimed it did, and a 19px chip centred in a
	// 22px band still hung below a strip clamped shorter than one row. What the rows past the clamped
	// height cost is PRESENCE: `layoutOutputChips` does not place them, and their hosts hide them. See
	// the rule there.
	//
	// **AND THE STRIP TAKES ITS ROOM FROM THE BODY, INCLUDING ALL OF IT.** `room` is the whole card
	// under a left head, so `frame.body` reaches zero BEFORE a single chip hides: a short enough card
	// is a spine, a foot and no controls at all. **Which should give way first is undecided** —
	// the body shrinking to nothing, or the foot yielding rows earlier — and this line is not that
	// decision; it is what the arithmetic happens to do.
	//
	// **IT IS REACHABLE THROUGH `RackEditor::setBounds` AND NOT BY A HOST WINDOW TODAY.** The sweep
	// case in `tests/test_rack_editor.cpp` reaches it that way. A player cannot: MEASURED on the rack
	// plugin at 720x196/218/260, the editor reports the short size through `get_size` and goes on
	// laying out at its design height, so the window CLIPS rather than re-lays and the card's foot is
	// below the sill either way. The frames differ from the TOP of the full-size editor by 2 pixels of
	// 141,120 at 196 — one LSB of green and blue each, on the macro row's soft gradient: rasteriser
	// noise, not geometry, which is what makes it a crop rather than a re-layout (a re-layout moves
	// edges). An earlier draft of this line said "pixel-identical", which it is not.
	// So this is an unreachable state with an open design question behind it: **backlog FT**, which
	// carries the measurement and a related decision on that surface. Do not "fix" the
	// arithmetic here without it.
	const float room = std::max(0.0f, (edge == CardHeadEdge::Left ? card.height()
	                                                              : card.height() - kCardHeadHeight));
	const float strip = std::min(wanted, room);
	CardFrame frame;
	if (edge == CardHeadEdge::Left) {
		const float bodyLeft = card.x() + kCardHeadHeight;
		const float bodyWidth = std::max(0.0f, card.width() - kCardHeadHeight);
		const float bodyHeight = std::max(0.0f, card.height() - strip);
		frame.head = { card.x(), card.y(), kCardHeadHeight, card.height() };
		frame.body = { bodyLeft, card.y(), bodyWidth, bodyHeight };
		frame.outputs = strip > 0.0f
		                    ? visage::Bounds { bodyLeft, card.y() + bodyHeight, bodyWidth, strip }
		                    : visage::Bounds {};
		return frame;
	}
	const float bodyTop = card.y() + kCardHeadHeight;
	const float bodyHeight = std::max(0.0f, card.height() - kCardHeadHeight - strip);
	frame.head = { card.x(), card.y(), card.width(), kCardHeadHeight };
	frame.body = { card.x(), bodyTop, card.width(), bodyHeight };
	frame.outputs = strip > 0.0f
	                    ? visage::Bounds { card.x(), bodyTop + bodyHeight, card.width(), strip }
	                    : visage::Bounds {};
	return frame;
}

size_t layoutOutputChips(std::span<const float> widths, visage::Bounds strip, float chipHeight,
                         std::vector<visage::Bounds>* out) {
	// **`out == nullptr` IS THE COUNTING MODE AND IT IS THE WHOLE DIFFERENCE BETWEEN THE TWO CALLERS.**
	// Counting asks how many rows are NEEDED and is given no real rectangle (`outputChipRows` passes a
	// width and zeroes); placing asks what FITS in one it has. **The wrap walk is identical in both and
	// that is the point** — it is why a count and a placement cannot disagree about where a row breaks,
	// and it is why they are one function rather than two that must be kept in step. What the mode
	// changes is only whether a rectangle is emitted and whether the fit tests run at all, so the brace
	// below is the boundary rather than a shortcut.
	if (out != nullptr) {
		out->clear();
	}
	if (widths.empty()) {
		return 0;
	}
	if (out != nullptr) {
		out->reserve(widths.size());
	}
	// **MEASURED RATHER THAN STEPPED.** The names are the devices' own and a fixed step is a guess
	// about their width that is wrong for somebody, so each chip is placed against the one before it.
	//
	// **AND LAID FROM THE LEFT, IN DECLARED ORDER, WHICH IT WAS NOT** (the mod outputs on the card
	// were badly laid out). It packed from the RIGHT, and on the card that
	// showed it — SKLITTER's, seven outputs in four rows in Feathers' fx lane — that drew:
	//
	//               [-> Perf Y]  [-> Perf X]
	//     [-> Weight 2]  [-> Weight 1]
	//     [-> Weight 4]  [-> Weight 3]
	//               [-> Perf Speed]
	//
	// **TWO FAULTS, AND THE SECOND IS THE WORSE ONE.** (a) Numbered outputs ran right to left — Weight 1
	// to the right of Weight 2, Weight 3 to the right of Weight 4. (b) Four right-anchored rows of
	// unequal width gave four different LEFT edges (measured at 248 / 180 / 180 / 383 px in a 250%
	// crop), and a ragged LEFT edge breaks the anchor the eye reads from. **A ragged RIGHT edge is what
	// ordinary text has and nobody notices; a ragged left edge is what makes a block look broken.** So
	// laying from the left fixes both with one change and trades a ragged edge for one nobody reads
	// against — which is why the narrower fix of reversing only the order WITHIN each row was
	// considered and rejected: it fixes (a) and leaves (b) standing.
	//
	// **AND IT DOES NOT FIGHT THE DECISION THIS COMMENT USED TO CARRY, which is what makes it a fix
	// rather than a reversal of that decision.** The old text defended VARIABLE WIDTHS — *"a
	// fixed step is a guess about their width that is wrong for somebody"* — and left-lay preserves
	// variable widths exactly: each chip is still measured from its own name and placed against the one
	// before it. The right-ANCHORING was incidental to that argument and protected by none of it.
	// (CLAUDE.md: check whether the redundancy is load-bearing before removing it. It was not.)
	//
	// **THE SINGLE-CHIP CARD MOVING TO THE LEFT EDGE IS THE POINT, NOT A COST.** A DAHDSR's one
	// `-> Envelope` now starts where a seven-output SKLITTER's `-> Perf X` starts, and where the value
	// boxes above it start: every strip begins at one x, so a foot is the same shape whatever publishes
	// it.
	//
	// **THE ROW COUNT IS UNCHANGED BY THE MIRROR**, which is what keeps this from moving any card's
	// height: same widths, same usable width, same greedy walk, so `outputChipRows` answers what it
	// always did and `FxCard::laneExtent` reserves what it always did. **And so is the HIDDEN SET** —
	// `fitsAcross` reads a chip's own width and `fitsDown` reads its ROW, and the mirror changes
	// neither, so a strip too short still drops a TAIL of the outputs (`Perf Speed` before `Perf Y`)
	// rather than a different chip. Measured in both directions rather than derived: see the case
	// `backlog HA: a card's output chips READ in declared order …`.
	const float usable = std::max(0.0f, strip.width() - 2.0f * kCardOutputChipPadX);
	float left = strip.x() + kCardOutputChipPadX;
	size_t row = 0;
	bool placedAny = false;
	for (size_t index = 0; index < widths.size(); ++index) {
		const float w = widths[index];
		// A HIDDEN OUTPUT (width 0: `DeviceHost::outputsShown`) takes no room and no gap — an empty rectangle, so its arm
		// hides and the chips after it close up.
		if (!(w > 0.0f)) {
			if (out != nullptr) {
				out->push_back(visage::Bounds {});
			}
			continue;
		}
		// **A CHIP WIDER THAN THE WHOLE STRIP DOES NOT WRAP**, because the next row would not hold it
		// either and wrapping would only move it. It is not placed at all — see the rule below.
		if (placedAny && left + w > strip.right() - kCardOutputChipPadX && w <= usable) {
			++row;
			left = strip.x() + kCardOutputChipPadX;
		}
		if (out != nullptr) {
			// **AN OUTPUT THAT CANNOT BE DRAWN INSIDE ITS CARD IS HIDDEN, NOT DRAWN OUTSIDE IT.** A
			// design decision, and it settles the one case the rule "all of a device's outputs are on
			// the card" does not: what "on the card" means when there is no room.
			//
			// **A HIDDEN CHIP IS A CARD THAT VISIBLY CANNOT SHOW EVERYTHING, and the player can resize;
			// a chip over the neighbour is a LIE ABOUT WHICH DEVICE PUBLISHES IT.** Hiding is also what
			// a FOLD already does to this strip, so it is not a new state for a host to learn.
			//
			// TWO WAYS A CHIP CAN FAIL TO FIT, and both answer the same way:
			//   ACROSS — wider than the strip's usable width, so no row could hold it. This is also
			//            what a strip of ZERO width answers for every chip, which is the frame a card
			//            too short for any foot at all produces (`cardFrameOf` returns an empty
			//            `outputs`): without this the whole row was laid at negative x AND SHOWN, which
			//            is precisely the mainline defect this arc photographed.
			//   DOWN   — its ROW lies past the strip's own height, which happens where `cardFrameOf`
			//            clamped the strip to a card too short to hold the rows the chips need. The
			//            test is on the whole ROW BAND rather than on the chip, so a chip centred in a
			//            band that only half fits is hidden too: the band is inside the strip or the
			//            chip is not drawn. **A previous draft tested neither and claimed the clamp
			//            was enough — it is not: the clamp keeps the RECTANGLE inside the card, and a
			//            19px chip centred in a 22px band still hung ~10px below a strip shorter than
			//            one row.**
			//
			// AN EMPTY RECTANGLE IS HOW IT SAYS SO — this file's idiom for "there is nothing here"
			// (`cardFoldBounds`, `CardFrame::outputs`), which every host already turns into
			// `setVisible(false)` without a second branch.
			//
			// **THE POSITIONS OF THE CHIPS THAT DO FIT ARE UNCHANGED BY ONE THAT DOES NOT** — the walk
			// still advances past a hidden chip. Closing the gap would make a chip's place depend on
			// how tall the card happens to be, so growing the card would MOVE the outputs a hand had
			// already learned rather than revealing the rest.
			const float top = strip.y() + static_cast<float>(row) * kCardOutputStripHeight;
			const bool fitsAcross = w <= usable;
			// +0.01 for the float sum only; a band is a multiple of an exact constant, so this admits
			// nothing a whole row's width of slack would.
			const bool fitsDown = static_cast<float>(row + 1) * kCardOutputStripHeight
			                      <= strip.height() + 0.01f;
			out->push_back(fitsAcross && fitsDown
			                   ? visage::Bounds { left,
				                                  top + (kCardOutputStripHeight - chipHeight) * 0.5f, w,
				                                  chipHeight }
			                   : visage::Bounds {});
		}
		left += w + kCardOutputChipGap;
		placedAny = true;
	}
	return row + 1;
}

size_t outputChipRows(std::span<const float> widths, float stripWidth) {
	// THE SAME WALK, WITH NOTHING TO PLACE — one function rather than two kept in step by hand, which
	// is the shape `ModPanel` was carrying. **NOT bit-identical to a placing call, and the declaration
	// says why**: this walks from x = 0 and a placement from `strip.x()`, so the agreement is
	// arithmetic. Only x and width are consulted when `out` is null — the zero y and height here stand
	// in for nothing, and in particular they do not mean "no room", because the fit tests that would
	// read them are skipped entirely without an `out`. This asks how many rows are NEEDED; what FITS
	// is a question only a real rectangle can answer.
	return layoutOutputChips(widths, { 0.0f, 0.0f, stripWidth, 0.0f }, 0.0f, nullptr);
}

// **THE DOTS SIT LEFT OF THE FOLD**, which is the order Feathers has always drawn them and which a
// case pins directly (`dots.right() <= foldBounds(source).x()`). I had them the other way round when
// the marks moved into the shared frame — invisible in a screenshot, and exactly the kind of detail a
// geometry assertion exists to hold.
visage::Bounds cardFoldBounds(visage::Bounds card, CardHeadEdge edge) {
	constexpr float kSize = 18.0f;
	if (edge == CardHeadEdge::Left) {
		// AT THE FOOT OF THE SPINE, LAST OF THE THREE. The marks run up the head in the same order they
		// run left along a top head, so a hand that learned one lane finds them in the same relative
		// place in the other.
		return { card.x() + (kCardHeadHeight - kSize) * 0.5f, card.bottom() - kModCardPadX - kSize,
			     kSize, kSize };
	}
	return { card.right() - kModCardPadX - kSize, card.y() + (kCardHeadHeight - kSize) * 0.5f, kSize,
		     kSize };
}

visage::Bounds cardMenuBounds(visage::Bounds card, CardHeadEdge edge) {
	constexpr float kSize = 18.0f;
	constexpr float kGap = 2.0f;
	if (edge == CardHeadEdge::Left) {
		return { card.x() + (kCardHeadHeight - kSize) * 0.5f,
			     card.bottom() - kModCardPadX - 2.0f * kSize - kGap, kSize, kSize };
	}
	// **ON A TOP HEAD IT IS THE FX CARD'S MENU RECTANGLE, to the pixel** — so the strip's row of
	// buttons ends where an fx card's does and packs inboard from there at the same gap
	// (`FxCard::enableBounds` lays the square out from this). It stood at `kModCardPadX + 2·18 + 2` off
	// the edge: room left for the fold mark, which was retired and left an empty square at the end.
	return { card.right() - kCardMenuMarkInset - kCardMenuMarkSize,
		     card.y() + (kCardHeadHeight - kCardMenuMarkSize) * 0.5f, kCardMenuMarkSize,
		     kCardMenuMarkSize };
}

visage::Bounds cardSourceDotBounds(visage::Bounds card, CardHeadEdge edge) {
	if (edge == CardHeadEdge::Left) {
		// UNDER THE ACTIVE SWITCH, both above the name.
		return { card.x() + (kCardHeadHeight - kModCardDotDiameter) * 0.5f,
			     card.y() + kModCardPadX + 12.0f + 4.0f, kModCardDotDiameter, kModCardDotDiameter };
	}
	return { card.x() + kModCardPadX, card.y() + (kCardHeadHeight - kModCardDotDiameter) * 0.5f,
		     kModCardDotDiameter, kModCardDotDiameter };
}

visage::Bounds cardNameBounds(visage::Bounds card, size_t outputCount, CardHeadEdge edge, float stopAt) {
	// THE GAP BEFORE THE NEXT OCCUPANT — the six pixels the head's buttons already keep between
	// themselves, so the name reads as one more item in the row rather than as text touching a button.
	constexpr float kGap = 6.0f;
	if (edge == CardHeadEdge::Left) {
		const float top = card.y() + kSpineNameTop;
		return { card.x(), top, kCardHeadHeight, std::max(0.0f, stopAt - kGap - top) };
	}
	float left = card.x() + kModCardPadX;
	if (outputCount > 0) {
		left += kModCardDotDiameter + 7.0f;
	}
	return { left, card.y(), std::max(0.0f, stopAt - kGap - left), kCardHeadHeight };
}

float textWidth(const std::string& text, const visage::Font& face) {
	std::u32string codes;
	for (const Utf8Character& c : decodeUtf8(text)) {
		codes.push_back(c.code);
	}
	return face.stringWidth(codes);
}

std::string fitText(const std::string& text, const visage::Font& face, float width) {
	if (textWidth(text, face) <= width) {
		return text;
	}
	if (textWidth("...", face) > width) {
		return {};
	}
	// THE LONGEST PREFIX THAT FITS WITH ITS DOTS, trailing spaces dropped so "Bus Utility" never elides
	// to "Bus ...". A name is a dozen characters, so a walk down from the full length is cheap enough —
	// and it runs only for text that did not fit, which in every head today is none.
	const std::vector<Utf8Character> characters = decodeUtf8(text);
	for (size_t count = characters.size(); count-- > 0;) {
		std::string prefix;
		for (size_t i = 0; i < count; ++i) {
			prefix += characters[i].bytes;
		}
		if (textWidth(prefix + "...", face) <= width) {
			while (!prefix.empty() && prefix.back() == ' ') {
				prefix.pop_back();
			}
			return prefix + "...";
		}
	}
	return "...";
}

std::string fitTracked(const std::string& text, const visage::Font& face, float tracking, float width) {
	if (trackedWidth(face, text, tracking) <= width) {
		return text;
	}
	if (trackedWidth(face, "...", tracking) > width) {
		return {};
	}
	// `fitText`'s walk, measured the way the caption is drawn: tracked, so the dots are spaced like the
	// letters they follow and what fits here is exactly what `drawTracked` puts inside the column.
	const std::vector<Utf8Character> characters = decodeUtf8(text);
	for (size_t count = characters.size(); count-- > 0;) {
		std::string prefix;
		for (size_t i = 0; i < count; ++i) {
			prefix += characters[i].bytes;
		}
		while (!prefix.empty() && prefix.back() == ' ') {
			prefix.pop_back();
		}
		if (trackedWidth(face, prefix + "...", tracking) <= width) {
			return prefix + "...";
		}
	}
	return "...";
}

FittedName fitName(const std::string& name, float room, float defaultSize, float floorSize, float tracking) {
	const float floorPx = std::min(floorSize, defaultSize); // a floor above the default would grow the name
	// WHOLE-PIXEL STEPS: `font()` rasterises at the rounded size, so a half step would measure one face and draw another.
	float size = defaultSize;
	for (; size > floorPx; size -= 1.0f) {
		if (trackedWidth(font(size), name, tracking) <= room) {
			return { name, size, false };
		}
	}
	size = std::max(size, floorPx);
	if (trackedWidth(font(size), name, tracking) <= room) {
		return { name, size, false };
	}
	return { fitTracked(name, font(size), tracking, room), size, true };
}

CardFrameStyle cardFrameStyleOf(bool selected, bool sourceFocused, bool armed, uint32_t tint) {
	CardFrameStyle style;
	// THE OUTLINE ANSWERS TO SELECTION AND NOTHING ELSE. It used to answer to `armed` as well —
	// `armed ? kModulation : (focused ? kAccent : ...)` — which is the one line that made the two
	// states indistinguishable on a card that was both.
	style.outline = selected;
	// IN THE INSTANCE'S EDGE COLOUR, the one its window's border wears, so the
	// selected card and its open window read as one device. A card with no instance keeps the accent.
	if (selected) {
		style.outlineColor = tint != 0 ? instanceEdgeColor(tint) : kAccent;
	}
	// THE GLOW IS ARMING AND ONLY ARMING — see `CardFrameStyle`. The first version also lit a
	// focused-but-disarmed source in the accent, the macro row's way, and MEASURED on the install
	// that made the armed card indistinguishable from the disarmed one (29,34,54 against 26,35,52).
	// A focused source that is not armed shows on the card by its arm chip's own colour, which is
	// what the chip is for; the card's ground says one thing, and it is the mode.
	style.glow = armed;
	style.glowColor = armed ? withAlpha(kModulation, kArmGlowAlpha) : 0;
	// THE NAME READS IN INK WHEN THE SOURCE IS FOCUSED TOO — the quiet form of "this card is the
	// one the rings are about", kept from the first version because it costs nothing and collides
	// with neither instrument.
	style.nameLit = style.outline || style.glow || sourceFocused;
	return style;
}

void drawCardFrame(visage::Canvas& canvas, visage::Bounds card, const char* name, visage::Bounds nameBox,
                   size_t outputCount, CardFrameStyle style, CardHeadMarks marks, CardHeadEdge edge) {
	const bool spine = edge == CardHeadEdge::Left;
	const bool focused = style.nameLit;
	canvas.setColor(kSurface);
	canvas.roundedRectangle(card.x(), card.y(), card.width(), card.height(), kCardCorner);
	// THE GLOW OVER THE SURFACE AND UNDER EVERYTHING ELSE, the same rounded rectangle the surface
	// is — a faint ground, not a halo: the macro row's column ground is inside the column too.
	if (style.glow) {
		canvas.setColor(style.glowColor);
		canvas.roundedRectangle(card.x(), card.y(), card.width(), card.height(), kCardCorner);
	}
	canvas.setColor(style.outline ? style.outlineColor : withAlpha(kLine, 0.6f));
	canvas.roundedRectangleBorder(card.x(), card.y(), card.width(), card.height(), kCardCorner, 1.0f);

	// THE SOURCE DOT ONLY WHERE THERE IS A SOURCE. It is what says "this device publishes something",
	// so drawing it on an effect would be the honesty rule broken in a decoration: a mark that
	// promises a capability the device does not have.
	//
	// **ON A SPINE IT RUNS UP THE HEAD INSTEAD OF ALONG IT** — dot at the top, name reading upward
	// from beneath it. Everything about the head transposes with the lane; nothing about it changes.
	if (spine) {
		// **A BAND BEHIND THE HEAD**: a bumper background in the header.
		// Drawn inside the border so the card still reads as one object, and darker rather than
		// lighter: the head is furniture and the body is the device, so the head recedes.
		canvas.setColor(withAlpha(kVoid, 0.5f));
		canvas.rectangle(card.x() + 1.0f, card.y() + 1.0f, kCardHeadHeight - 2.0f,
		                 card.height() - 2.0f);
		if (outputCount > 0) {
			// THE DOT SITS UNDER THE ACTIVE SWITCH, both above the name. A device that publishes a
			// source and a device that is switched on are two different facts and get two marks.
			const visage::Bounds dot = cardSourceDotBounds(card, edge);
			canvas.setColor(kValue);
			canvas.circle(dot.x(), dot.y(), kModCardDotDiameter);
		}
		if (name != nullptr && *name != '\0') {
			canvas.setColor(focused ? kInk : kMuted);
			// **SIDEWAYS, READING BOTTOM TO TOP, GLYPH BOTTOMS TO THE RIGHT** — the spec, and
			// `visage::Direction` carries it, so the text is ROTATED rather than stacked letter by
			// letter. (Stacking was the fallback I had costed before finding this parameter; a rail's
			// own note — *"THE GLYPH IS GEOMETRY, not a character"* — is about chevrons, not text.)
			//
			// **`Left` IS THE ONE, SETTLED BY RENDERING BOTH AND LOOKING.** The name says which way the
			// text's own "up" points, not which way it reads, and I was not going to get that right by
			// reasoning about it: `Left` rotates counter-clockwise, which sends the advance upward and
			// a glyph's foot rightward — exactly the two halves of the spec. `Right` puts the band
			// somewhere else entirely.
			//
			// The band runs from below the dot to above the marks at the foot — `nameBox`, which the
			// card derives from the marks it actually has — and the name arrives already fitted to it.
			const float bandTop = nameBox.y();
			const float band = nameBox.height();
			// **RIGHT-JUSTIFIED IN THE TEXT'S OWN AXIS, WHICH PUTS IT AT THE TOP OF THE BAR.** The
			// spec exactly, and the justification is expressed in the ROTATED frame — the text reads
			// bottom-to-top, so its "end" is the top of the spine. A long name then grows DOWNWARD from
			// the top rather than away from it, which is what keeps every card's name starting in the
			// same place.
			//
			// AND A LARGER FACE than the horizontal head uses: a rotated name is harder to read, and
			// it has a whole spine to itself rather than a row shared with marks.
			canvas.text(name, font(kSpineNameSize), visage::Font::kRight, card.x(), bandTop,
			            kCardHeadHeight, band, visage::Direction::Left);
		}
	}
	else {
		if (outputCount > 0) {
			const visage::Bounds dot = cardSourceDotBounds(card, edge);
			canvas.setColor(kValue);
			canvas.circle(dot.x(), dot.y(), kModCardDotDiameter);
		}

		if (name != nullptr && *name != '\0') {
			canvas.setColor(focused ? kInk : kMuted);
			canvas.text(name, font(kControlSize), visage::Font::kLeft, nameBox.x(), nameBox.y(),
			            nameBox.width(), nameBox.height());
		}
	}

	// THE HOST'S MARKS, DRAWN BY THE FRAME RATHER THAN BY THE HOST — and this is a correctness fix,
	// not tidiness. A card is a CHILD frame, so a host draws beneath it; the moment the card's frame
	// covered the head, the strip's fold and menu vanished under it. They travel with the head now.
	//
	// **NO FOLD MARK.** It drew one rule when open and two when folded, and it was retired because a
	// double-click already folds. `cardFoldBounds` survives for the
	// moment only as the rectangle nothing may occupy.

	// THE SLOT'S VERBS behind three dots. Geometry rather than a glyph, because this font has no
	// guarantee of an ellipsis that centres and a missing glyph is an invisible control.
	// THE INSTANCE'S TINT: a bar along the head's inner edge — the left edge of a top head, the top edge
	// of a spine — clear of the name and of every mark. See `CardHeadMarks::tint`.
	if (marks.tint != 0) {
		canvas.setColor(marks.tint);
		if (spine) {
			canvas.fill(card.x() + kTintBarInset, card.y() + 1.0f, kCardHeadHeight - 2.0f * kTintBarInset,
			            kTintBarWidth);
		}
		else {
			canvas.fill(card.x() + 1.0f, card.y() + kTintBarInset, kTintBarWidth,
			            kCardHeadHeight - 2.0f * kTintBarInset);
		}
	}

	if (marks.menu) {
		const visage::Bounds dots = cardMenuBounds(card, edge);
		const float dy = dots.y() + dots.height() * 0.5f - 1.0f;
		const float dx = dots.x() + dots.width() * 0.5f - 5.0f;
		canvas.setColor(kMuted);
		for (int i = 0; i < 3; ++i) {
			canvas.fill(dx + static_cast<float>(i) * 4.0f, dy, 2.0f, 2.0f);
		}
	}
}


void drawArmArrow(visage::Canvas& canvas, float cx, float cy, float size) {
	// ONE STROKE WIDTH FOR EVERY SIZE. The two call sites draw at 13 and 11 and both used 1.6f, so
	// scaling the stroke with the glyph would change the mark rather than share it.
	static constexpr float kStroke = 1.6f;
	const float reach = size * 0.5f;
	canvas.segment(cx - reach, cy, cx + reach, cy, kStroke, true);
	canvas.segment(cx + reach, cy, cx + reach * 0.2f, cy - reach * 0.6f, kStroke, true);
	canvas.segment(cx + reach, cy, cx + reach * 0.2f, cy + reach * 0.6f, kStroke, true);
}

void drawChevronPair(visage::Canvas& canvas, visage::Bounds box, ChevronDirection direction) {
	// **THE STROKE IS THE RAIL'S `kChevronWeight`, RESTATED HERE RATHER THAN INCLUDED.** `rail.h`
	// reaches this file and not the other way round — the floor does not depend on a widget — so the
	// number is written once here and `Rail::kChevronWeight` is kept for the rail's own callers. If
	// they are ever made to disagree, this is the one a mark is actually drawn with.
	static constexpr float kStroke = 1.6f;

	const bool horizontal = direction == ChevronDirection::Left || direction == ChevronDirection::Right;
	// **THE RAIL'S OWN ARITHMETIC, MOVED NOT REWRITTEN.** `half` is 0.22 of the box across the
	// chevron's spread and `step` 0.20 of it along the travel; `front` is where the leading tip sits,
	// a step in from the edge the mark points away from. Transposing for Up/Down swaps which of the
	// box's two dimensions plays each part, and nothing else changes.
	const float along = horizontal ? box.width() : box.height();
	const float across = horizontal ? box.height() : box.width();
	const float mid = (horizontal ? box.y() : box.x()) + across * 0.5f;
	const float half = across * 0.22f;
	const float step = along * 0.20f;

	const bool forward = direction == ChevronDirection::Right || direction == ChevronDirection::Down;
	const float lowEdge = horizontal ? box.x() : box.y();
	const float highEdge = horizontal ? box.right() : box.bottom();
	// Pointing BACKWARD (Left or Up) starts a step in from the low edge; pointing forward starts two
	// steps in from the high edge, which is what keeps the pair centred either way.
	const float front = !forward ? lowEdge + step : highEdge - step * 2.0f;

	for (int chevron = 0; chevron < 2; ++chevron) {
		const float tip =
		    front + static_cast<float>(chevron) * step * (!forward ? 1.0f : -1.0f) + (!forward ? 0.0f : step);
		const float tail = !forward ? tip + step : tip - step;
		if (horizontal) {
			canvas.segment(tip, mid, tail, mid - half, kStroke, true);
			canvas.segment(tip, mid, tail, mid + half, kStroke, true);
		}
		else {
			canvas.segment(mid, tip, mid - half, tail, kStroke, true);
			canvas.segment(mid, tip, mid + half, tail, kStroke, true);
		}
	}
}

void drawTracked(visage::Canvas& canvas, const std::string& text, const visage::Font& face,
                 float tracking, float x, float y, float height) {
	// visage's Font has no letter-spacing, and the mock's tracking is not decoration — the wordmark is
	// set at 0.42em and the small caps at 0.28em, which is most of what makes the type read as
	// hardware panel rather than as a web page. So the glyphs are placed one at a time.
	const float spacing = tracking * static_cast<float>(face.size());
	const TrackedText& measured = tracked(face, text);
	float pen = x;
	for (size_t i = 0; i < measured.characters.size(); ++i) {
		const float advance = measured.advances[i];
		canvas.text(measured.characters[i].bytes, face, visage::Font::kLeft, pen, y, advance, height);
		pen += advance + spacing;
	}
}

BoxOutline boxOutlineOf(bool selected, bool destination, bool assigning) {
	BoxOutline o;
	if (selected && destination) {
		o.kind = BoxOutlineKind::Both;
		o.outerColour = kAccent;
		o.innerColour = kModulation;
		o.strokes = 2;
	}
	else if (selected) {
		o.kind = BoxOutlineKind::Selected;
		o.outerColour = kAccent;
		o.strokes = 1;
	}
	else if (destination) {
		o.kind = BoxOutlineKind::Destination;
		o.outerColour = kModulation;
		o.innerColour = kModulation;
		o.strokes = 2;
	}
	o.innerGlow = destination && assigning;
	return o;
}

void drawBoxOutline(visage::Canvas& canvas, float x, float y, float w, float h, float radius, const BoxOutline& o,
                    float dim) {
	if (o.innerGlow) {
		// THREE NESTED STROKES, FADING INWARD: visage has no inner shadow, and a fill would wash the value text.
		// The strokes start past the doubled line (insets 3..5) and total under a fifth of the text's contrast.
		const float fades[3] = { 0.22f, 0.12f, 0.06f };
		for (int i = 0; i < 3; ++i) {
			const float inset = 3.0f + static_cast<float>(i);
			canvas.setColor(withAlpha(kModulation, fades[i] * dim));
			canvas.roundedRectangleBorder(x + inset, y + inset, w - 2.0f * inset, h - 2.0f * inset,
			                              std::max(0.0f, radius - inset), 1.0f);
		}
	}
	if (o.strokes >= 1) {
		canvas.setColor(withAlpha(o.outerColour, dim));
		canvas.roundedRectangleBorder(x, y, w, h, radius, 1.0f);
	}
	if (o.strokes >= 2) {
		canvas.setColor(withAlpha(o.innerColour, dim));
		canvas.roundedRectangleBorder(x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f, std::max(0.0f, radius - 2.0f), 1.0f);
	}
}

} // namespace feathers::ui
