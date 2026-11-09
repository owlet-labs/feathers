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

#include <algorithm>
#include <cmath>

#include "dsp/constellation.h"
#include "plugin/ui/chrome_hue.h"

#include <feathers/tokens.h>
#include <visage/graphics.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <span>
#include <string>
#include <vector>

// The shared look: the mock's tokens under the names the editor uses for them, plus the one piece of
// state a theme needs (a font, at a size).
//
// The colours live in the generated header rather than here — see cmake/GenerateTokens.cmake — so
// this file is about ROLES. `tokens::kValue` is a colour; `ink()` is what text is drawn in. Naming
// the role separately is what lets a re-skin move a token without every call site being re-read.

namespace feathers::ui {

//---------//
// Palette //
//---------//

// **THE CHROME ROLES ARE LIVE, NOT CONSTANT** (backlog LG): each is a read-only reference into the
// palette the user's hue offset turns (`chrome_hue.h`), so every call site reads the colour in force
// at the moment it draws. Consequences for a call site:
//   - not usable in a `constexpr` (the compiler says so, which is how every such site was found);
//   - **never copy one into a `static` local** — it would keep the colour of the first frame drawn.
// The DATA colours below them (seeds, the stereo ears, the meters) stay `constexpr` and never turn.

/// The dark the editor sits on. The mock keeps two, one per surface; this is the perform one, which
/// is what the chrome is drawn against.
inline const uint32_t& kVoid = chrome_detail::gChrome.voidGround;

/// Panel fills, one step up from the void, and the hairline between them.
inline const uint32_t& kSurface = chrome_detail::gChrome.surface;
inline const uint32_t& kSurfaceRaised = chrome_detail::gChrome.surfaceRaised;
inline const uint32_t& kLine = chrome_detail::gChrome.line;

/// Type. Bone is what you read, mute is what you glance at, dim is what is switched off.
inline const uint32_t& kInk = chrome_detail::gChrome.ink;
inline const uint32_t& kMuted = chrome_detail::gChrome.muted;
inline const uint32_t& kDisabled = chrome_detail::gChrome.disabled;

/// The mock's semantic roles, kept apart on purpose: `accent` is interaction, `value` is what a
/// parameter reads, and `mod` is modulation and nothing else. Collapsing any two of them is how a
/// design language stops meaning anything — which is also why the hue offset turns all three
/// together rather than the accent alone (at 180° a lone accent lands on value's orange).
inline const uint32_t& kAccent = chrome_detail::gChrome.accent;
inline const uint32_t& kValue = chrome_detail::gChrome.value;
inline const uint32_t& kModulation = chrome_detail::gChrome.modulation;
/// **A METER'S TWO COLOURS: DATA, SO THEY NEVER TURN** — the accent and value as the mock draws them,
/// fixed. A meter reads a level; its colour is not chrome (backlog LG).
inline constexpr uint32_t kMeterAccent = tokens::kAccent;
inline constexpr uint32_t kMeterValue = tokens::kValue;
/// **ALMOST CLIPPING** (backlog ML): a clip LED's -2..0 dBFS state (`clip_led.h`), a yellow apart from the value's
/// peach and the over's red. Data, so it does not turn either. PROVISIONAL: to be set by eye.
inline constexpr uint32_t kMeterNear = 0xffE8D25A;
/// **OVER FULL SCALE** (backlog MK): the red the master's and Bus Utility's clip LEDs light (`kOverRed` beside each), as
/// a token, for the card's courtesy meter that fills from its far end by the amount over. Data; it does not turn.
inline constexpr uint32_t kMeterClip = 0xffD9483B;
/// **A STEREO RING'S TWO EARS** (backlog KU) — either side of `kModulation`, cool left, warm right, so a split ring
/// still reads as modulation first. PROVISIONAL: to be set by eye.
inline constexpr uint32_t kModulationL = tokens::kModL;
inline constexpr uint32_t kModulationR = tokens::kModR;
/// **THE GLOW OF A DESTINATION THAT TAKES A VALUE PER EAR** — the potential-destination halo, tinted pink so a
/// stereo destination reads apart from a mono one.
inline constexpr uint32_t kModulationStereo = tokens::kModStereo;

/// The notify readout's own ground, from the mock's literal `rgba(7,10,18,.9)`. Not a token because
/// the mock does not make it one: the readout floats over whatever surface it happens to cover, so
/// it carries its own background rather than borrowing a panel's.
inline const uint32_t& kNotifyGround = chrome_detail::gChrome.notifyGround;

/// The brightest mark the palette has, for the one stroke on a control that has to be findable at a
/// glance against its own track. The mock spends it on star cores and knob pointers, and nothing
/// else — a second user would make it stop meaning "look here".
inline const uint32_t& kPointer = chrome_detail::gChrome.pointer;

/// A HUE PER SOUND, in the order the constellation assigns them — and ONE array, which is the point
/// of it being here. The rail, the Perform circle, Explore and the scope all paint sounds, and each
/// of them used to write this list out for itself; four copies of a palette is four chances for a
/// re-skin to reach three surfaces. A colour that named a sound on one surface and not on another
/// would be the same defect the hues block exists to prevent, one level up.
///
/// It is the LOOK only. Which entry a given sound wears is `SoundHues`, kept with the patch and read
/// through `hueIndexOf` — that split is what lets a re-theme move a colour without migrating
/// anybody's saved sounds. A role rather than a token because the mock's seed palette is a role: it
/// answers "what colour is this sound", which is not a question `tokens::kSeed3` can be asked.
///
/// Eight, because the constellation holds eight — a ninth sound would be a sound with no colour,
/// which is why the count is asserted against the engine's rather than trusted.
inline constexpr std::array<uint32_t, 8> kSeedHues { tokens::kSeed1, tokens::kSeed2, tokens::kSeed3,
	                                                 tokens::kSeed4, tokens::kSeed5, tokens::kSeed6,
	                                                 tokens::kSeed7, tokens::kSeed8 };
static_assert(kSeedHues.size() == kMaxSounds, "every sound needs a hue of its own");

//-------//
// Type  //
//-------//

/// The same colour at a different alpha. The editor fades exactly one thing — the notify readout —
/// and this is how, rather than by keeping faded copies of tokens around.
constexpr uint32_t withAlpha(uint32_t color, float alpha) {
	const float clamped = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
	const uint32_t a = static_cast<uint32_t>(clamped * 255.0f + 0.5f);
	return (color & 0x00FFFFFFu) | (a << 24);
}

/// A LINEAR BLEND BETWEEN TWO COLOURS, per channel including alpha.
///
/// Hoisted out of wave_display.cpp, which had the only copy while there was only one thing drawing a
/// gradient from computed points. There are two now — the filter card's response curve is the other
/// — and a second copy of a colour ramp is how two pictures in one editor end up not matching.
inline uint32_t blendColour(uint32_t from, uint32_t to, float t) {
	const float amount = std::clamp(t, 0.0f, 1.0f);
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const auto a = static_cast<float>((from >> shift) & 0xFFu);
		const auto b = static_cast<float>((to >> shift) & 0xFFu);
		const auto mixed = static_cast<uint32_t>(std::lround(a + (b - a) * amount));
		out |= (mixed & 0xFFu) << shift;
	}
	return out;
}

/// THE SAME COLOUR WITH ITS CHROMA TAKEN OUT, pulled toward the grey of its OWN brightness. At 0 it
/// is untouched; at 1 it is that grey. Alpha is carried through unchanged.
///
/// NOT A BLEND TOWARD A GREY TOKEN, which is the other way to spell "greyer" and is a different
/// operation. Mixing toward kMuted lands a warm colour on neutral and leaves a cool one still blue,
/// because kMuted is itself a blue-grey — so a pair desaturated that way comes out lopsided, one end
/// tinted and the other dead. Pulling each colour toward its own luminance takes the same fraction of
/// chroma out of both and moves neither's BRIGHTNESS at all, which is what lets a two-colour gradient
/// be quietened without being redesigned.
inline uint32_t desaturate(uint32_t colour, float amount) {
	const auto channel = [colour](int shift) {
		return static_cast<float>((colour >> shift) & 0xFFu);
	};
	// Rec. 709, which is what the eye does with these three lights — an unweighted mean would make the
	// blue end darker than the warm one at the same nominal grey.
	const float luma = 0.2126f * channel(16) + 0.7152f * channel(8) + 0.0722f * channel(0);
	const auto level = static_cast<uint32_t>(std::lround(std::clamp(luma, 0.0f, 255.0f)));
	const uint32_t grey = (colour & 0xFF000000u) | (level << 16) | (level << 8) | level;
	return blendColour(colour, grey, amount);
}

/// A font at a size, built once and kept.
///
/// visage rasterises from font bytes and constructing a Font is not free, so the throwaway
/// `visage::Font f(size, data)` that every draw() used to make was a lookup per widget per frame.
/// Main thread only, which is why the cache behind it needs no lock — every draw call in this editor
/// arrives through the host's one callback.
const visage::Font& font(float size);

/// Hand every cached font back to visage and empty the cache. Call this while visage is still
/// standing — from the editor's own teardown — because the cache is deliberately never destroyed
/// and would otherwise hold its fonts until process exit. See the note in theme.cpp for why that
/// matters more than it sounds like it should.
void clearFontCache();

/// THE TRACKED-TEXT CACHE (`drawTracked` / `trackedWidth`), a least-recently-used cache of decoded, measured strings:
/// its fill, its cap, every measure it has made (each one a miss), and every eviction. Diagnostics (the settings
/// screen's session line) and the cases that hold a hot string to zero measures per frame.
size_t trackedTextCacheSize() noexcept;
size_t trackedTextCacheCapacity() noexcept;
uint64_t trackedTextMeasures() noexcept;
uint64_t trackedTextEvictions() noexcept;
/// TESTS: while the audit is on, a measure of a string measured before (since the audit began) counts as a RE-measure.
/// Turning it on or off starts the count again. Off by default: it remembers every string it sees.
void setTrackedTextAudit(bool on);
uint64_t trackedTextRemeasures() noexcept;

/// FORCE A FONT-ATLAS REPACK ON DEMAND — the lever a windowed probe needs in order to put the
/// suspected event INSIDE a gesture rather than waiting for one to happen by luck.
///
/// WHAT ACTUALLY CAUSES A REPACK, straight out of the audit of the fork: a PackedFont is cached per
/// typeface and PIXEL SIZE, its atlas is packed lazily as characters are first seen, and MEASURING a
/// string packs its glyphs exactly as drawing one does. So novel characters at a novel size force
/// the whole sequence — a font created, its atlas sized to a single glyph, then repacked to the
/// default width — without putting one new pixel on screen or dirtying any region. That last part is
/// what makes it usable mid-gesture: nothing about the frame under test changes except the repack.
///
/// Every call uses a size and a character block it has never used before, so repeated calls keep
/// working instead of quietly becoming no-ops once the atlas has room. That silent decay is exactly
/// what made an earlier version of the atlas control vacuous, and a lever that stops levering
/// without saying so is worse than none.
///
/// Returns how many repacks it actually caused, so a caller can ASSERT the lever worked rather than
/// assume it. Zero means it did nothing and any conclusion drawn from that run is void.
int forceFontAtlasRepack();

/// REPACK AN ATLAS THE EDITOR IS ALREADY DRAWING FROM — the event the birth lever above cannot stage.
///
/// forceFontAtlasRepack mints a NEW font, so its repack moves one glyph and then two, in an atlas
/// nothing is drawing from. That event has been staged inside a drag in every knob-and-surface
/// combination and every frame came back clean — which is coherent rather than surprising, since
/// almost nothing was in flight to corrupt.
///
/// This stages the other one: novel characters at a size the editor is ALREADY USING, so an
/// established atlas full of resident glyphs repacks and every one of them moves while real text
/// quads are in flight. Pass a size the editor genuinely draws with — the atlas trace's
/// "FONT size N CREATED" lines name them.
///
/// Returns how many repacks it caused. ZERO means the lever did nothing and any conclusion drawn
/// from that run is void.
int forceEstablishedAtlasRepack(float size);

/// Text with CSS-style letter-spacing, which visage's Font has no notion of.
///
/// Not decoration: the mock sets the wordmark at 0.42em and its small caps at 0.28em, and that
/// tracking is most of what makes the type read as a hardware panel rather than a web page. `tracking`
/// is a fraction of the font size, exactly as CSS `em` is. The glyphs are placed one at a time,
/// because there is no other way to do it here.
/// A CAPTION IN CAPITALS — the mock sets every widget label this way.
///
/// It was a TU-local helper in `knob.cpp` until `KnobTray` moved to its own file and needed it too.
/// Here rather than copied, because two capitalisations that could drift is exactly the divergence
/// this file's siblings exist to prevent.
///
/// **ASCII ONLY: a-z become A-Z and every other byte is left exactly as it was.** A macro's name is
/// the user's UTF-8, and `std::toupper` one byte at a time answers from the locale — under a Latin-1
/// locale it takes 0xE0-0xFE for small letters and subtracts 0x20, which rewrites the lead byte of
/// every three- and four-byte character and leaves a name that is no longer UTF-8. So "é" stays
/// lower case; a capital it cannot produce is better than a character it destroys.
inline std::string upper(const char* text) {
	std::string out = text != nullptr ? text : "";
	for (char& c : out) {
		if (c >= 'a' && c <= 'z') {
			c = static_cast<char>(c - 'a' + 'A');
		}
	}
	return out;
}

float trackedWidth(const visage::Font& face, const std::string& text, float tracking);
void drawTracked(visage::Canvas& canvas, const std::string& text, const visage::Font& face,
                 float tracking, float x, float y, float height);

/// THE ARM ARROW — the mark that says "point this source at something", drawn once.
///
/// A STEM AND TWO STROKES FOR THE HEAD, pointing right. `source_grid.cpp` stated the rule when it
/// copied the glyph: *"the modulator card's exactly... Same mark, same verb — a second glyph for the
/// same action would read as a second action."* It was right about the rule and then made the third
/// copy of it, which is how a design language drifts: nobody disagrees, everybody restates.
///
/// AND IT IS DRAWN RATHER THAN SET, which is not a style choice. `mod_panel.cpp:1291` records why:
/// *"this font has no `➤` and a missing glyph is an invisible control."* A control nobody can see is
/// a control nobody can press, so the mark cannot depend on a font's coverage.
///
/// THE CALLER SETS THE COLOUR, because the three sites disagree about it for real reasons — the
/// panel has a focused state the grid does not, and the grid has a dead state the panel cannot
/// reach. Only the SHAPE is shared, which is the part that was drifting.
///
/// WHY IT LIVES IN THE FLOOR: the rack drew its own and got it wrong — an arrow inconsistent with
/// Feathers' design language. A mark that two hosts must agree on belongs where both compile
/// against it, not in whichever of them drew it first.
void drawArmArrow(visage::Canvas& canvas, float cx, float cy, float size);

/// Which way a `drawChevronPair` points — **the way the thing it opens will TRAVEL**: outboard to put
/// a panel away, inboard to bring it back.
enum class ChevronDirection : uint8_t { Left, Right, Up, Down };

/// **THE "MOVE THIS WHOLE PANEL ASIDE" MARK, DRAWN FROM POINTS — AND IT HAS NO CALLER TODAY.**
///
/// It was `Rail::drawRailChrome`'s own loop, lifted here on 2026-09-19 when a SECOND kind of
/// collapsing surface arrived (`Tray`, which folded on the other axis) so that two hand-drawn copies
/// of one mark could not diverge — `drawArmArrow` above is here for that reason and says so. **THAT
/// CLASS IS GONE**: SKLITTER's two sections are `Rail`s now (`sklitter_expanded.h`), and the rail
/// keeps its own loop for an arithmetic reason `rail.cpp` states at the site. So this is kept as the
/// floor's stated answer for the next collapsing surface — anchored by DIRECTION, which is the only
/// rule a caller with no "side" can state — rather than because anything draws with it.
/// **THE HORIZONTAL ARITHMETIC IS THE RAIL'S, MOVED RATHER THAN REWRITTEN**; Up and Down are its
/// transpose.
///
/// **A GEOMETRIC MARK, NOT A CHARACTER**, which is A16's lesson and was paid for again on 2026-09-19:
/// SKLITTER's first tray header used `\xe2\x96\xbe` (U+25BE) and rendered as NOTHING, because this
/// atlas has no glyph for it — a header claiming to be collapsible with no mark to say so.
///
/// **AND A DOUBLE CHEVRON RATHER THAN A FILLED TRIANGLE.** On the rail's first render the expand/collapse
/// glyph was overloaded: the same mark already meant modulators and solo. A counting problem rather than a
/// taste one — this editor already spends a filled triangle on the modulator card's arrow and another
/// on a sound's solo. Two open strokes read as a different KIND of mark.
void drawChevronPair(visage::Canvas& canvas, visage::Bounds box, ChevronDirection direction);

/// ONE FRAME FOR EVERY CARD, MODULATOR OR EFFECT.
///
/// **IT WAS SIX FRAMES AND A HOST.** Every fx card restated the same three lines — a rounded surface,
/// a bordered edge, a name at 24px — and each declared its own `kHeadHeight = 24.0f`; there are five
/// such constants in this tree. The modulator cards drew no frame at all, because Feathers' strip
/// drew theirs. So "what a card looks like" had seven definitions and no single one to change.
/// The goal is one unified frame for both mod and fx.
///
/// **AND THE OUTPUTS GO AT THE BOTTOM BY CONTRACT RATHER THAN BY CONVENTION — that is the decision.**
/// Either would do, and a unified widget API would make convention enough. It would; the
/// contract is chosen because it COSTS NOTHING HERE. Neither host lets a card place its own output
/// widgets today — the rack's arms are children of `RackEditor`, Feathers' chips are drawn by
/// `ModPanel` — so having the FRAME reserve the strip means "outputs at the bottom" is true by
/// construction rather than by every future card remembering. A convention would be a comment, and
/// this repo has a written history of comments that described intent while the code did otherwise.
inline constexpr float kCardHeadHeight = 24.0f;
/// **THE OVERFLOW MENU AT THE RIGHT END OF A TOP HEAD: its size and its inset from the card's right
/// edge — ONE rectangle whichever lane draws it.** The fx lane's menu is the card's own
/// (`FxCard::slotMenuBounds`) and the modulator strip's is the host's (`cardMenuBounds`); they were
/// two sets of numbers, and the strip's still stood where the retired fold mark had put it, so its
/// whole row sat 25px inboard of the fx cards' (the mod card header widgets were not aligned
/// full right like the audio effects'). Both read these now.
inline constexpr float kCardMenuMarkSize = 12.0f;
inline constexpr float kCardMenuMarkInset = 10.0f;
/// THE SIDEWAYS NAME ON A SPINE, larger than the horizontal head's — a rotated word is harder to read
/// and it has the whole spine to itself rather than a row shared with the head's marks.
/// BREATHING ROOM EITHER SIDE OF A KNOB'S CAPTION when a card sizes its cells to fit them. Small,
/// because the caption is already the widest thing in the cell — this is the gap that stops two
/// adjacent captions touching, not a margin.
inline constexpr float kKnobLabelGutter = 8.0f;

inline constexpr float kSpineNameSize = 12.0f;
/// WHERE A SPINE'S NAME BAND BEGINS — below the source dot and the active switch, which share the top.
/// The name is right-justified so it ENDS here, which is why the switch above it is "above the last
/// letter of the name".
inline constexpr float kSpineNameTop = 2.0f * 12.0f + 3.0f * 9.0f;
inline constexpr float kCardCorner = 5.0f;
/// The strip along the bottom that a device's mod outputs are laid into. Zero-height for a device
/// that declares none — an effect pays nothing for a tier it is not in. **ONE ROW's worth**: a device
/// with more chips than fit on a line gets several of these, see `outputChipRows`.
inline constexpr float kCardOutputStripHeight = 22.0f;

/// **ONE UNIT: THE HEIGHT EVERY CARD OCCUPIES IN A LANE.** Shorter cards pad; taller ones scroll.
///
/// A maximum vertical size: below it a card pads with empty frame, above it the card scrolls (with a
/// scroll bar where applicable). That allows neat rows OR columns of effects and makes a unit-tall (+)
/// meaningful. The maximum is tall enough that most cards do not scroll; the few that do are the
/// ones whose UI needs tuning anyway.
///
/// **MEASURED, all nine registered devices, natural card height:**
///
///     Harm 146 · MSEG 186 · DAHDSR 190 · Shaper 230 · OWLTT 282
///     Automod 336 · Filter 362 · Drawn 420 · Waypoint 602
///
/// **THE SPREAD IS 4x, SO SOMETHING SCROLLS WHATEVER IS CHOSEN** — the question is only what to
/// minimise, and the design settles which: **most must fit.** The number is then read off the
/// distribution rather than picked:
///
///     unit 230 -> 4 of 9 fit    unit 282 -> 5 of 9    unit 336 -> 6 of 9
///     unit 362 -> 7 of 9        unit 420 -> 8 of 9    unit 460 -> 8 of 9 (the lane's own height)
///
/// **336 IS THE CHOICE AND THE THREE THAT SCROLL ARE THE THREE THAT WANT UI WORK ANYWAY** — which is
/// the scrolling landing exactly where it should. `Filter` (362) and `Waypoint` (602) are **generic knob grids with no custom card yet**, and
/// `Drawn` (420) carries four frames of six bands. Going taller buys the seventh and eighth card at a
/// real cost: 420 pads Harm by **274px**, which is the "too tall" complaint returning in a new shape.
///
/// AND IT IS AN EXACT CARD HEIGHT (Automod), so at least one card is a unit by construction rather
/// than by rounding.
///
/// **IT IS ONE CONSTANT ON PURPOSE.** The shape is the decision; the number is read from the table
/// above, which is here so the next reader can see what moving it costs.
inline constexpr float kCardUnitHeight = 336.0f;
/// **THE HORIZONTAL UNIT.** A card is one of these wide in a vertical lane, and its list columns are
/// one of these wide in a horizontal one.
///
/// **SEPARATE FROM `kCardUnitHeight` ON PURPOSE** — the horizontal and vertical units are chosen
/// separately. A unit is a lane's CROSS extent, and the two lanes cross in
/// different directions, so one number could only be right for one of them. 216 is what Feathers'
/// modulator strip has always laid a card out at, so the vertical lane keeps the width it was
/// designed around and the horizontal lane inherits a column width that is known to hold a control.
inline constexpr float kCardUnitWidth = 216.0f;

/// The name Feathers' strip has always used for the same number. Kept so its call sites read as they
/// did; there is one value.
inline constexpr float kModCardWidth = kCardUnitWidth;
inline constexpr float kModCardDotDiameter = 8.0f;
inline constexpr float kModCardPadX = 9.0f;

/// THE SIDE PADDING THE OUTPUT CHIPS ARE LAID INSIDE, and the gap between two of them.
///
/// **THEY ARE THE NUMBERS FEATHERS' STRIP HAS ALWAYS USED**, named here because the walk moved here:
/// `ModPanel` read its own `kCardPadX` (9) and `kCardGap` (10), and the second of those is the gap
/// between two CARDS, which happened to be the number wanted between two chips. Two meanings sharing
/// one constant is how a change to card spacing would silently re-lay the chips.
///
/// **SO THEY ARE SPELLED OUT RATHER THAN ALIASED, INCLUDING THE ONE THAT MATCHES.** The first draft
/// wrote `= kModCardPadX`, which is the same mistake in the other direction: these lay the chips on
/// EVERY host — Feathers' strip, its fx lane and the rack — and a mod CARD's padding is one host's
/// number. Equal today, and nothing about the rack's foot should move because the modulator strip
/// re-padded its cards.
inline constexpr float kCardOutputChipPadX = 9.0f;
inline constexpr float kCardOutputChipGap = 10.0f;
static_assert(kCardOutputChipPadX == kModCardPadX,
              "they are equal today and may stop being; this is the line that says a change to the "
              "mod card's padding is a DECISION about the chips rather than a side-effect on them");

/// WHICH EDGE A CARD'S HEAD SITS ON.
///
/// **THE HEAD FLIPS WITH THE LANE**, carrying on the flip convention: the header sits on the left
/// vertical edge in h-flow mode and on the top in v-scroll mode. A lane's cards
/// run along its flow axis, and a head on the leading CROSS edge is the one that stays a constant
/// thickness whichever way they run — a top head on a horizontally-flowing card eats the height that
/// the unit rule just fixed, while a left head eats width the flow has to spare.
///
/// AND IT IS WHAT MAKES A COLLAPSED CARD MEAN SOMETHING: collapsed, a card IS its head, so in an
/// h-flow lane it becomes a narrow vertical spine, collapsing vertically the way the fx lane in
/// Feathers does.
enum class CardHeadEdge : uint8_t {
	Top,  ///< v-scroll: cards stack downward, the head is the top row.
	Left, ///< h-flow: cards run rightward, the head is the left spine.
};

/// WHERE A CARD'S THREE REGIONS ARE. The head it shares with every other card, the body it draws
/// itself, and the outputs strip its HOST fills.
struct CardFrame {
	visage::Bounds head;
	visage::Bounds body;
	/// EMPTY WHEN THE DEVICE PUBLISHES NOTHING, which is how an effect gets the same frame without a
	/// dead band along its bottom edge.
	visage::Bounds outputs;
};

/// Split a card's rectangle into head, body and outputs strip.
///
/// **`outputRows` IS HOW MANY ROWS THE CHIPS NEED, AND IT IS PASSED IN BECAUSE ONLY THE CALLER CAN
/// KNOW.** A chip is as wide as its device's name for that output, so how many fit on a row is a
/// question about text this function cannot see. One is the answer for every device that declares one
/// output, which was every device until the Note Expressions modulator declared seven.
///
/// **THE STRIP GREW BECAUSE A CARD CLIPPED FOUR OF THEM AWAY.** On an installed build only three mod
/// outputs were visible. The chips were laid right-to-left in one fixed-height row with no wrap and no
/// clamp, so the fourth onward had negative x and were drawn outside the card. Nothing was wrong for
/// one chip and nothing was right for seven — the same shape as the publish loop that wrote only the
/// first output.
CardFrame cardFrameOf(visage::Bounds card, size_t outputCount,
                      CardHeadEdge edge = CardHeadEdge::Top, size_t outputRows = 1);

/// **THE ONE WALK THAT PLACES A CARD'S OUTPUT CHIPS — left to right along the strip, in the order the
/// device declares them, wrapping onto a new row when the next chip would cross the right edge.**
///
/// **IT RAN THE OTHER WAY UNTIL 2026-09-20** (the mod outputs on the card were badly formatted),
/// and the definition photographs what that looked like on SKLITTER's card. The reading
/// order is the point: a device's outputs are numbered, and `Weight 1` drawn to the RIGHT of
/// `Weight 2` is wrong however neat the rectangles are.
///
/// `widths` is each output's chip width IN ORDER. A chip is as wide as its device's name for that
/// output, so **which row a chip lands on depends on every chip before it** and there is no
/// arithmetic that shortcuts the walk. The return is how many ROWS it used, which is what a host
/// passes to `cardFrameOf` as `outputRows`; `out`, when non-null, is cleared and filled with one
/// rectangle per width, in the same order.
///
/// **A CHIP THAT WOULD NOT LIE INSIDE `strip` GETS AN EMPTY RECTANGLE, AND ITS HOST HIDES IT.** The
/// decision, 2026-09-19: an output that cannot be drawn inside its card is hidden, not
/// drawn outside it. The definition has the two ways it can fail and why hiding is the honest answer.
/// A host therefore asks `width() > 0` before it shows an arm; it needs no other test.
///
/// **COUNTING AND PLACING ARE ONE FUNCTION BECAUSE THEY MUST AGREE.** They were two walks in
/// `ModPanel` — `outputRowsOf` and `outputChipBounds` — kept in step by both asking the same width
/// function, which is a discipline rather than a guarantee; and neither the fx lane nor the rack had
/// either walk — so a seven-output device marched its chips off the left edge of its card in the rack,
/// and got no chips at all in the fx lane. All of a device's outputs belong on its card.
///
/// **THE AGREEMENT IS ARITHMETIC RATHER THAN BIT-EXACT, and that is the honest claim.** The counting
/// call walks from x = 0 and a placing call from `strip.x()`, so the two compare the same quantities
/// shifted by a constant; in real arithmetic they wrap identically, and in floats a boundary landing
/// within an epsilon of `usable` could in principle differ by a row. Nothing closer is available
/// without making the count take a rectangle it does not have.
///
/// `chipHeight` is the chip's own height, centred in its row — passed in rather than named here so
/// `SourceArm::kHeight` stays the single definition of it.
size_t layoutOutputChips(std::span<const float> widths, visage::Bounds strip, float chipHeight,
                         std::vector<visage::Bounds>* out = nullptr);

/// HOW MANY ROWS CHIPS OF THESE WIDTHS NEED IN A STRIP THIS WIDE — the walk above with nothing to
/// place.
///
/// **A HOST ASKING THIS USUALLY HAS NO RECTANGLE YET**, because the height it is about to reserve is
/// what the rectangle is made of; the width is all the count depends on, so that is all it asks for.
/// Zero widths is zero rows, which is how a device that publishes nothing costs no strip at all.
size_t outputChipRows(std::span<const float> widths, float stripWidth);

/// Draw the shared frame: the surface, the border, the source dot and the name.
///
/// `focused` and `armed` colour the border, which is how the strip already says which source an arrow
/// would arm. A card with no outputs passes false for both and gets the plain edge.
/// The head marks a HOST offers on top of the frame. A card is told which it has rather than
/// guessing: the rack offers neither, Feathers' strip offers both, and the card is the same class.
struct CardHeadMarks {
	bool menu = false; ///< the three dots — remove and reorder
	/// **THE `(-)` RULE IS GONE**: with double-click available, the (-) mark is unnecessary. A card folds by double-clicking its head, which is one gesture in place of one gesture
	/// plus a target — and the mark cost a hit rectangle inside the head, which is where every
	/// double-counting bug on this card has come from.
	///
	/// `folded` STAYS, because it is not the mark: it is the state the frame draws the head in.
	bool folded = false;
	/// **THIS INSTANCE'S TINT**, or 0 for none — a thin bar along the head's inner edge in a colour that
	/// is the device INSTANCE's rather than its type's. With several instances of one device type,
	/// and more than one of them shown at a time, both the collapsed and the expanded view are tinted
	/// to say which one. The same tint marks the panel's title (`ExpandedPanel::setTint`),
	/// which is what lets a hand pair a floating card with the card it came from.
	uint32_t tint = 0;
};

/// THE INSTANCE TINTS: eight hues far enough apart to tell at a glance, taken by the instance's id so
/// the same device wears the same colour in every view and across a save. Zero — `kNoInstance` — is
/// no tint. Eight rather than more because the ninth device shares a hue with the first, and that is
/// the honest limit of "which one" by colour alone; the name is still on the head.
///
/// LIVE, like the chrome roles: the user's hue offset turns all eight together (backlog LG), which keeps
/// their spacing and so the guarantee below. The colours as drawn at offset 0 are
/// `kChromeBase.instanceTints` in chrome_hue.h.
inline const std::array<uint32_t, kInstanceTintCount>& kInstanceTints = chrome_detail::gChrome.instanceTints;
/// **A DEVICE'S ID IS NOT AN IDENTITY ONCE CHAINS NEST — WHICH CHAIN IT IS IN IS THE OTHER HALF.**
///
/// `backplane/device_instance_id.h` says so in its own words: *"Today each of Feathers' chains counts
/// separately from `kFirstInstance`"*, and D1's one-counter decision *"coexists"* with that until steps
/// 3 and 4 move Feathers' chains onto the graph. `rack::InnerChain` owns a whole `RackEngine`, and a
/// `RackEngine` owns an `InstanceCounter` — so the first device inside SKLITTER's post chain is
/// instance 1, and so is the first device of the rack's own chain. Two different devices, one integer.
///
/// **WHAT THAT COST, MEASURED RATHER THAN FEARED**: `ExpandedOverlay::keyOf` is `title#device` and the
/// layer keeps ONE PANEL PER KEY, so a nested device colliding with an outer one does not merely share
/// a hue and a remembered position — the second panel **replaces** the first, and a hand that opened
/// SKLITTER-inside-SKLITTER watches the outer panel vanish. The tint is the visible symptom of the
/// same fact.
///
/// **SO IDENTITY IS (WHICH CHAIN, WHICH ID), AND THIS CARRIES THE FIRST HALF.** `path` names the chain
/// — 0 is the host's own, and a nested chain mixes its holder's id into its holder's path, so nesting
/// composes to any depth. `hueShift` is the palette offset that keeps the flare legible; it is carried
/// beside the path rather than derived from it because it is answering a different question, and the
/// answer it needs — *"which hue does the HOLDER wear"* — is not recoverable from a hash.
///
/// **IT IS NOT A SECOND ID SPACE AND MUST NOT BECOME ONE.** Nothing is keyed on this but the panel
/// layer and the tint; routes, patches and the engine keep addressing devices by instance within their
/// own chain, which is what they already do correctly. The day D1's single counter arrives, `path`
/// becomes a constant 0 everywhere and this struct collapses to nothing — that is the intended end.
struct DeviceNest {
	/// WHICH CHAIN. 0 is the top-level host's own; anything else is a mix of the holders above.
	uint64_t path = 0;
	/// HOW FAR THIS CHAIN'S HUES ARE ROTATED — see `instanceTintFor` for the guarantee it buys.
	uint32_t hueShift = 0;
	/// **HOW MANY CHAINS DEEP THIS ONE IS** — 0 for the host's own, 1 for a chain inside one of its
	/// devices, and so on. Carried because a surface that lets a hand nest must stop where the SAVE
	/// FORMAT stops (`rack::kMaxDepth`): a patch a player can build and cannot reload is the honesty
	/// rule failing at the one moment it matters. It is not derivable from `path`, which is a hash.
	uint32_t depth = 0;

	friend constexpr bool operator==(const DeviceNest&, const DeviceNest&) = default;
	[[nodiscard]] constexpr bool top() const noexcept { return path == 0; }

	/// **THE CHAIN HELD BY `holder`, WHICH IS A DEVICE IN *THIS* CHAIN.**
	///
	/// **THE PATH IS A HASH, AND THE HONEST BOUND HAS TWO HALVES.** Four FNV-1a rounds over the
	/// holder's bytes, seeded from the outer path. Xor and multiply-by-an-odd-constant are both
	/// bijections on `uint64_t`, so **for a FIXED holder id the map from outer path to inner path is
	/// injective**: two chains whose holders sit at the same position never collide, whatever the
	/// depth. Across DIFFERENT holder ids the bound is the ordinary ~2⁻⁶⁴, with one extra point of
	/// contact where the `h == 0` clamp below folds one value onto another. Neither is "unique", and
	/// the honest alternative — a path STRING of ids — buys an exactness nothing here can use.
	///
	/// **AND THE HUE MOVES ON BY THE HOLDER'S OWN**: `holderIndex + 1`. A chain's first SEVEN devices
	/// therefore never wear the hue of the device holding them — which is exactly the pair a hand has
	/// on screen when it opens a panel from inside a panel, and it is a guarantee rather than a 7-in-8
	/// chance. **The EIGHTH device of a chain wears its holder's colour exactly**: eight hues less the
	/// one the holder is using leaves seven, and no arrangement of a fixed palette escapes that. The
	/// bound is asserted in `test_takeover.cpp` rather than only written here.
	[[nodiscard]] static constexpr DeviceNest under(DeviceNest outer, uint32_t holder) {
		uint64_t h = outer.path ^ 0xcbf29ce484222325ull;
		for (uint32_t byte = 0; byte < 4; ++byte) {
			h = (h ^ ((holder >> (byte * 8)) & 0xffu)) * 0x100000001b3ull;
		}
		// **NEVER 0**, because 0 is the reserved name of the host's own chain. A hash landing there
		// would make a nested device indistinguishable from a top-level one — the bug this exists to
		// close, arriving by the one road the hash can take to it.
		return { h == 0 ? 1ull : h,
			     static_cast<uint32_t>((outer.hueShift + holder) % kInstanceTintCount),
			     outer.depth + 1 };
	}
};

/// **THE FLARE, AND WHAT IT HONESTLY PROMISES.** Eight hues, so:
///
///   - Within ONE chain, any eight consecutive instance ids are eight different hues. The ninth
///     device repeats the first's — the bound `kInstanceTints` already states.
///   - A chain's devices are rotated off the hue of the device that HOLDS them (`DeviceNest::under`),
///     so a panel opened from inside a panel differs from it by construction and not by luck.
///   - Two SIBLING chains — two SKLITTERs' post chains, say — collide whenever their shifts and ids
///     happen to sum alike, which is 1 in 8. **The flare tells panels apart; it does not name them.**
///     The title on the panel's head and the name on the card's do that, and both are still drawn.
inline uint32_t instanceTintFor(uint32_t instanceId, DeviceNest nest = {}) {
	return instanceId == 0
	           ? 0u
	           : kInstanceTints[(nest.hueShift + instanceId - 1) % kInstanceTintCount];
}
/// The bar's width, and its inset from the head's edges.
inline constexpr float kTintBarWidth = 3.0f;
inline constexpr float kTintBarInset = 4.0f;
/// **HOW STRONGLY A FLOATING PANEL'S BORDER WEARS ITS INSTANCE'S HUE.** Rendered and looked at rather
/// than reasoned about: the plain edge is `kLine` at 0.9 and is nearly invisible against the scrim, so
/// a tint at the same alpha reads as a coloured rule and not as a neon outline. Under it the hue stops
/// being nameable at a glance, which is the one thing this number has to buy.
inline constexpr float kPanelEdgeTintAlpha = 0.9f;
/// **THE INSTANCE'S EDGE — ONE COLOUR FOR "THIS DEVICE", WHEREVER IT IS OUTLINED.** By design
/// the selected card's highlight should follow the window's outline colour, so a selected card and
/// its open window read as one identity. Both the floating panel's border (`ExpandedPanel::draw`) and
/// a selected card's border (`cardFrameStyleOf`) take their colour from here, over the same tint
/// (`instanceTintFor`), so the two cannot drift apart by one of them being retuned. Zero when the
/// device has no instance tint; each caller keeps its own untinted edge for that case.
inline constexpr uint32_t instanceEdgeColor(uint32_t tint) {
	return tint != 0 ? withAlpha(tint, kPanelEdgeTintAlpha) : 0u;
}

/// **WHAT THE FRAME SAYS ABOUT A CARD'S TWO STATES — AS VALUES, BEFORE ANY PAINT.**
///
/// A glow rather than an outline on mod assign, and a highlight/outline on a mod card selected by a
/// click anywhere on its face. Two states, two instruments that cannot collide:
///
///   - THE OUTLINE IS SELECTION — the card a key acts on, the one the hand last pressed. It is the
///     border the fx lane and the rack already draw for their focused card, unchanged.
///   - THE GLOW IS ARMING — a ground in the modulation colour over the whole card while the card's
///     source is ARMED, and nothing otherwise. It is the macro row's language (a ground behind the
///     column, *"which is what a card's border used to say"*) with one thing measured and changed:
///     the first version lit the card for a FOCUSED source too, in the accent, at the row's own
///     0.16 — and photographed on the installed build the armed tint (29,34,54) and the merely-
///     focused tint (26,35,52) were indistinguishable over a (7,10,18) surface, and the tint stayed
///     after disarming, so the glow was not visible on mod cards at all. Nothing on the card ever
///     said ARMED specifically. So the glow means armed
///     only, and it is stronger than the row's furniture ground — `kArmGlowAlpha` — because it is
///     a MODE indicator, not a focus rest.
///
/// BOTH ON ONE CARD DRAWS BOTH, and that is the whole point of making them different shapes. The
/// modulator strip used to draw the armed source's card with a BORDER — the same mark the other
/// hosts use for selection — so the day selection arrived in the strip the two would have fought
/// over one pixel-wide line (backlog CN2/CO, the two-outlines problem that stopped this work once).
///
/// A STRUCT RATHER THAN THREE BOOLS INTO THE DRAWER, so a case can assert what a card will be drawn
/// with without rendering it — assert the value, not the render — and `FxCard::frameStyle()` is where a card reports
/// it.
struct CardFrameStyle {
	bool outline = false; ///< selected: the border in `outlineColor`
	/// THE SELECTED BORDER'S COLOUR: the instance's edge (`instanceEdgeColor`) — the same colour its
	/// window is outlined in — or `kAccent` for a card with no instance tint. 0 when not selected.
	uint32_t outlineColor = 0;
	bool glow = false;    ///< the source is armed: a ground in `kModulation` at `kArmGlowAlpha`
	uint32_t glowColor = 0;
	/// The name reads in ink when either instrument is on, muted otherwise.
	bool nameLit = false;
};

/// **THE DISABLED VEIL — what a switched-off device's whole body is painted through.** One colour
/// for every card (see `ui::DisabledVeil`); the number is a matter of looking and it has been
/// looked at twice. A veil pulls everything toward its own colour, so a veil BRIGHTER than the
/// lane's ground can only lift the card above the lane — the first pick, a neutral grey at 0.55,
/// floated a pale slab (too light), and a higher alpha of it lifts MORE. MEASURED
/// (mean luma, ground 19.6): kDisabled@0.55 -> body 68; the same at 0.75 -> 82; a darker neutral
/// at 0.55/0.70 -> 42/45; the strip's own kSurfaceRaised at 0.80 -> 22, the only one at the
/// ground. So the veil is the strip's ground: the card recedes into the lane it sits in, still
/// grey-blue rather than black, arcs muted to (42,40,46).
inline const uint32_t& kDisabledVeil = chrome_detail::gChrome.disabledVeil; // kSurfaceRaised at 0.80

/// THE ARMED GROUND'S STRENGTH. MEASURED rather than reasoned: at the macro row's 0.16 the card
/// lifted from (7,10,18) to (29,34,54) on the installed build — a wash a hand does not notice while
/// it is looking at the knob it is about to drag. 0.34 puts it near (54,62,96): plainly periwinkle,
/// still a ground and not a fill, and the controls on it keep their own contrast (the value boxes
/// are darker than the surface, so they read AGAINST it).
inline constexpr float kArmGlowAlpha = 0.34f;

/// `tint` is the card's instance tint (`instanceTintFor`), which is what the selected border wears.
CardFrameStyle cardFrameStyleOf(bool selected, bool sourceFocused, bool armed, uint32_t tint = 0);

/// `nameBox` is where the name goes — `cardNameBounds` — and `name` is drawn into it as given, so a
/// caller hands over text that already fits (`fitText`). The frame no longer decides how far a name
/// may run: only the card knows what else is in its head.
void drawCardFrame(visage::Canvas& canvas, visage::Bounds card, const char* name, visage::Bounds nameBox,
                   size_t outputCount, CardFrameStyle style, CardHeadMarks marks, CardHeadEdge edge);

/// A SCROLL BAR DOWN THE RIGHT EDGE OF A BODY, drawn only when there is something to scroll.
///
/// Above the unit height a card scrolls, with a scroll bar where applicable — **"where applicable" is the
/// whole contract**: a bar on a card with nothing hidden is furniture that lies about the content. The
/// thumb's length is the visible FRACTION, which is the only thing that tells a hand how much more
/// there is; a fixed-length thumb says "there is more" and refuses to say how much.
void drawBodyScrollBar(visage::Canvas& canvas, visage::Bounds body, float scroll, float content);

/// WHERE THE HEAD'S MARKS SIT, so a host's hit test and the frame's drawing cannot disagree about
/// where a press lands. `ModPanel` had its own copies of these rectangles; they now come from here.
/// **NO DEFAULT EDGE, DELIBERATELY.** These had `= CardHeadEdge::Top`, and `FxCard::headMarkPressed`
/// called them without one — so the marks were DRAWN transposed onto the spine and HIT-TESTED at the
/// top of the card, which is why (...) and (-) did not respond. The drawing passed its
/// edge; the hit test silently took the default. A caller that must state the edge cannot make that
/// mistake, and this file has just been through the same lesson once already (`d8bf59d`: a dead
/// DEFAULT hides every bug that needs motion).
visage::Bounds cardMenuBounds(visage::Bounds card, CardHeadEdge edge);
visage::Bounds cardFoldBounds(visage::Bounds card, CardHeadEdge edge);
/// THE SOURCE DOT, where `drawCardFrame` paints it for a device that publishes outputs — one
/// rectangle so the frame's circle and a head's layout cannot disagree about where it is.
visage::Bounds cardSourceDotBounds(visage::Bounds card, CardHeadEdge edge);

/// **THE ROOM A CARD'S NAME MAY BE WRITTEN IN** — from after the source dot (if any) to `stopAt`, the
/// nearest edge of whatever the head holds beyond the name: an x on a top head, a y on a spine.
///
/// Text collided with widgets in the headers. The name's box ran
/// to the card's full width, so a long name was drawn straight under the buttons at the head's right
/// end. The box ends where the head's next occupant starts, and `fitText` makes the text fit the box.
visage::Bounds cardNameBounds(visage::Bounds card, size_t outputCount, CardHeadEdge edge, float stopAt);

/// **`text`, OR AS MUCH OF IT AS FITS `width` WITH "..." AFTER IT** — and empty when not even the
/// dots fit. Measured with `face`, the font the text will be drawn in, so what fits here fits there.
/// Three full stops rather than U+2026 because this font is not guaranteed that glyph, and a missing
/// glyph draws nothing.
std::string fitText(const std::string& text, const visage::Font& face, float width);
/// **`fitText` FOR TRACKED TEXT** — measured with `trackedWidth` at `tracking`, as `drawTracked` will
/// set it, so a knob column's caption can be fitted to the column it is drawn in (`Knob::columnCaption`).
std::string fitTracked(const std::string& text, const visage::Font& face, float tracking, float width);
/// **THE SMALLEST SIZE A NAME IS SHRUNK TO BEFORE IT IS CUT** (backlog OT: a clipping string is drawn
/// in a smaller font, with a floor so it never gets too tiny). A token, not a literal. `font()` is
/// keyed on whole pixels, so the sizes between the default and this are whole steps: an 8px caption fits at 7 or 6.
inline constexpr float kNameFitFloorSize = 6.0f;

/// **A NAME FITTED TO A BOX** — the text to draw and the size to draw it at.
struct FittedName {
	std::string text;
	float size = 0.0f;
	/// True when even the floor size did not hold the name and `text` was cut and ends in "...".
	bool elided = false;
};
/// **THE ONE FIT FOR A NAME THE USER TYPED** (backlog OT) — a macro's, wherever it is drawn. `name` as typed (no
/// forced capitals), at `defaultSize` when it fits `room` wide (measured with `trackedWidth` at `tracking`, as
/// `drawTracked` sets it); else one pixel smaller at a time down to `floorSize`; only a name that still does not fit
/// at the floor is cut, with `fitTracked`'s "..." and at the floor size. Empty text only when not even the dots fit.
FittedName fitName(const std::string& name, float room, float defaultSize, float floorSize, float tracking);

/// HOW WIDE `text` (UTF-8) IS IN `face` — decoded per character first, so a multi-byte character is
/// measured as the one glyph it draws as.
float textWidth(const std::string& text, const visage::Font& face);

/// The arm arrow's nominal size where the modulation panel draws it. The source grid uses 11.
inline constexpr float kArmArrowSize = 13.0f;

/// The type scale, in logical pixels, named for what it labels rather than for how big it is.
inline constexpr float kWordmarkSize = 15.0f;
inline constexpr float kBylineSize = 9.0f;
inline constexpr float kControlSize = 10.0f;
inline constexpr float kReadoutSize = 11.0f;
/// The knob column's own two, from the mock's `.kl` (8px) and `.kv` (9px).
inline constexpr float kKnobLabelSize = 8.0f;
inline constexpr float kKnobReadoutSize = 9.0f;

/// The mock's tracking, as a fraction of the font size — CSS `letter-spacing` in em.
inline constexpr float kWordmarkTracking = 0.42f;
inline constexpr float kBylineTracking = 0.28f;
inline constexpr float kControlTracking = 0.14f;
inline constexpr float kKnobLabelTracking = 0.22f;

} // namespace feathers::ui
