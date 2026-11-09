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

#include <feathers/tokens.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

// THE CHROME HUE (backlog LG: "offset the main chrome and window hues but keep all
// the brightness the same"). One user preference, a hue offset in degrees, that ROTATES the chrome's
// colours in OKLCH: L and C are kept and h moves by the offset. Not HSV, whose "same value" is not the
// same brightness to an eye — a yellow and a blue at one HSV value are far apart in lightness.
//
// WHAT TURNS, measured before it was decided (LG's status line has the numbers):
//   - THE TINTED GREYS. The entry called them neutral; they are not — every grey in the mock is navy
//     at one hue (h ≈ 270°, C 0.008 for bone up to 0.055 for the line). That navy IS the chrome's hue,
//     so it turns with the rest. Their L is kept, so legibility is kept.
//   - THE SEMANTIC ROLES AS ONE FAMILY: accent, value, modulation. Turning the accent alone would walk
//     it onto value's orange at 180°, and the roles are kept apart on purpose (theme.h).
//   - THE EIGHT INSTANCE TINTS (window and card hues). A uniform rotation keeps their spacing, so
//     neighbours stay as distinct as they were.
// WHAT DOES NOT TURN: data. The seed hues (a sound's identity), the stereo ring's two ears and the
// stereo glow, the terrain and its nebula ramps, the Explore and Perform stages' grid and star marks,
// and the meters (which read `kMeterAccent`/`kMeterValue`, fixed copies of the base tokens).
//
// THE GAMUT IS THE ONE PLACE C CANNOT BE KEPT. Every chrome token fits sRGB at every offset; six of
// the eight instance tints do not (yellow keeps 45% of its chroma at +161°). There the colour keeps
// L and h EXACTLY and gives up chroma — never more than it had — which is the least visible thing to
// lose and leaves the tints' spacing intact.
//
// THE ACCENT HUE, A SECOND AXIS (a second colour axis in settings, for more control: a second hue
// for the accents). A second offset that turns the semantic-role family
// (accent, value, modulation) TOGETHER, on top of the chrome hue: the roles turn by chrome + accent, the
// greys and the instance tints by chrome alone. The family turns as one by one sum, so its internal
// spacing is the base's and no role can walk onto another; the same OKLCH rules hold (L kept, C given up
// only to the gamut). It is ONE rotation by the sum, never two in a row, so a role is rounded to bytes
// once — and an accent offset of 0 is the chrome-only palette bit for bit, because chrome + 0 is chrome.
// Data still does not turn.
//
// THE PALETTE IS PROCESS-GLOBAL, as the tooltip detail is: every editor in a process draws one chrome.
// It is read by theme.h's roles through references, so a call site that reads `kAccent` reads the live
// value; `setChromeHueOffset` recomputes it and tells every `ChromeHueWatch` (an editor's prefs and its
// repaint). Main thread only, like everything that draws.

namespace feathers::ui {

/// A colour in OKLCH. `h` in degrees, [-180, 180).
struct Oklch {
	double L = 0.0;
	double C = 0.0;
	double h = 0.0;
};

/// The colour's OKLCH coordinates. Alpha is ignored.
Oklch toOklch(uint32_t argb) noexcept;
/// The 8-bit sRGB colour nearest `colour`, with `alpha` in the top byte. A colour outside sRGB keeps L
/// and h and loses chroma until it fits (a bisection on C), so C never grows.
uint32_t fromOklch(Oklch colour, uint32_t alpha = 0xffu) noexcept;
/// `argb` with its hue turned by `degrees`, L kept, C kept where the gamut allows. Alpha is carried.
/// An offset of 0 (or any multiple of 360) answers `argb` UNCHANGED, bit for bit, by not turning at all — so the
/// default editor draws exactly as it did before this existed. (MEASURED: the round trip is byte-exact too.)
uint32_t rotateHue(uint32_t argb, double degrees) noexcept;

inline constexpr size_t kInstanceTintCount = 8;

/// EVERY COLOUR THE OFFSET TURNS, in one place, so "what is shifted" is a list a test can walk.
struct ChromePalette {
	uint32_t voidGround = 0;
	uint32_t surface = 0;
	uint32_t surfaceRaised = 0;
	uint32_t line = 0;
	uint32_t ink = 0;
	uint32_t muted = 0;
	uint32_t disabled = 0;
	uint32_t pointer = 0;
	uint32_t notifyGround = 0;
	uint32_t accent = 0;
	uint32_t value = 0;
	uint32_t modulation = 0;
	/// `kSurfaceRaised` at 0.80 — see `kDisabledVeil` in theme.h. Turned with its colour, alpha kept.
	uint32_t disabledVeil = 0;
	std::array<uint32_t, kInstanceTintCount> instanceTints {};
};

/// The palette as the mock draws it: offset 0.
inline constexpr ChromePalette kChromeBase {
	tokens::kVoidPerform, tokens::kSurface, tokens::kSurface2, tokens::kLine, tokens::kBone, tokens::kMute,
	tokens::kDim, tokens::kStar,
	// The notify readout's own ground, from the mock's literal `rgba(7,10,18,.9)` — see theme.h.
	0xff070A12u, tokens::kAccent, tokens::kValue, tokens::kMod,
	// kSurfaceRaised at 0.80: round(0.80 * 255) = 204 = 0xcc, as `withAlpha` computes it.
	(tokens::kSurface2 & 0x00FFFFFFu) | 0xcc000000u,
	{
	    0xff5aa9e6, // blue
	    0xffe6a25a, // orange
	    0xff7fd07f, // green
	    0xffd56fd1, // magenta
	    0xffe6d65a, // yellow
	    0xff5ad6cf, // teal
	    0xffe66f6f, // red
	    0xffa88be6, // violet
	},
};

/// `base` with every member turned by `degrees` — pure, so a test can ask for any offset.
ChromePalette rotatedPalette(const ChromePalette& base, double degrees) noexcept;
/// `base` with the greys and tints turned by `chromeDegrees` and the semantic roles (accent, value,
/// modulation) by `chromeDegrees + accentDegrees`, each in one rotation. `accentDegrees == 0` answers
/// `rotatedPalette(base, chromeDegrees)` exactly.
ChromePalette rotatedPalette(const ChromePalette& base, double chromeDegrees, double accentDegrees) noexcept;

namespace chrome_detail {
/// THE LIVE PALETTE. Constant-initialised from the base, so a role read during static initialisation
/// anywhere still sees the mock's colours. Written only by `setChromeHueOffset`.
inline ChromePalette gChrome = kChromeBase;
} // namespace chrome_detail

/// The offset's range and its default. The ends are the same hue; both are allowed so a strip can be
/// dragged to either end without a jump.
inline constexpr double kChromeHueMin = -180.0;
inline constexpr double kChromeHueMax = 180.0;
/// Held to the range; a non-finite value is 0.
double clampChromeHue(double degrees) noexcept;

/// The offset in force, in degrees.
double chromeHueOffset() noexcept;
/// Set the offset (clamped), recompute the live palette and tell every watch. Returns how many watches
/// were told — 0 when the offset did not move, so a caller can assert a change was delivered.
size_t setChromeHueOffset(double degrees);
/// How many `ChromeHueWatch`es / `AccentHueWatch`es are alive — one per open editor; a lifetime test's count.
size_t chromeHueWatchCount() noexcept;
size_t accentHueWatchCount() noexcept;

/// SOMETHING THAT MUST HEAR THE OFFSET MOVE: an editor, which writes it to its prefs and repaints.
/// Main thread only.
class ChromeHueWatch {
public:
	explicit ChromeHueWatch(std::function<void(double)> changed);
	~ChromeHueWatch();
	ChromeHueWatch(const ChromeHueWatch&) = delete;
	ChromeHueWatch& operator=(const ChromeHueWatch&) = delete;

private:
	friend size_t setChromeHueOffset(double degrees);
	std::function<void(double)> changed_;
};

/// THE ACCENT HUE's range: the chrome hue's, and held the same way.
inline constexpr double kAccentHueMin = kChromeHueMin;
inline constexpr double kAccentHueMax = kChromeHueMax;
/// Held to the range; a non-finite value is 0.
double clampAccentHue(double degrees) noexcept;

/// The accent offset in force, in degrees — what the roles turn by beyond the chrome hue.
double accentHueOffset() noexcept;
/// Set the accent offset (clamped), recompute the live palette and tell every `AccentHueWatch` (and no
/// `ChromeHueWatch`). Returns how many were told — 0 when the offset did not move.
size_t setAccentHueOffset(double degrees);

/// SOMETHING THAT MUST HEAR THE ACCENT OFFSET MOVE: an editor, which writes it to its prefs and repaints.
class AccentHueWatch {
public:
	explicit AccentHueWatch(std::function<void(double)> changed);
	~AccentHueWatch();
	AccentHueWatch(const AccentHueWatch&) = delete;
	AccentHueWatch& operator=(const AccentHueWatch&) = delete;

private:
	friend size_t setAccentHueOffset(double degrees);
	std::function<void(double)> changed_;
};

} // namespace feathers::ui
