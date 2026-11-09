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

#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"

#include "plugin/editor_prefs.h"
#include "plugin/ui/chrome_hue.h"
#include "plugin/ui/chrome_hue_prefs.h"
#include "plugin/ui/emitters_front.h"
#include "plugin/ui/emitters_front_prefs.h"
#include "plugin/ui/frame_timing.h"
#include "plugin/ui/full_redraw.h"
#include "plugin/ui/open_url.h"
#include "plugin/ui/settings_screen.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/window_controls_side.h"
#include "plugin/ui/window_controls_side_prefs.h"

#include <unistd.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <vector>

// THE CHROME HUE (backlog LG), by VALUE. Every claim here is about a number a colour carries — OKLCH L, C
// and h of each token before and after the turn — never about a picture, which could differ for any reason.
//
// THE TOLERANCE IS THE 8-BIT GRID ITSELF, stated per colour rather than as one number: a turned colour is
// rounded to whole bytes once, so it can be off its exact OKLCH by up to half a byte in each channel. How far
// one byte moves OKLab depends on where the colour sits — near black it is large, because OKLab takes a cube
// root of linear light (MEASURED: the void, #010204, moved L by 0.0062 at +60°, where a fixed 0.004 had
// been written from the mid-tones). So the bound is `kGridSteps` times the largest OKLab move a single byte
// makes at the turned colour (`byteStep`). The first case prints the worst ratio it saw. A rotation that
// changes brightness for real — an HSV turn, the known-bad — misses it by an order of magnitude.

using namespace feathers;
using ui::Oklch;

namespace {

/// Half a byte in each of three channels, summed, is at most 1.5 single-byte moves.
constexpr double kGridSteps = 1.5;

/// The largest OKLab distance a ±1 change in one channel makes at `argb` — the size of the grid there.
double byteStep(uint32_t argb) {
	const Oklch at = ui::toOklch(argb);
	const double a0 = at.C * std::cos(at.h * std::numbers::pi / 180.0);
	const double b0 = at.C * std::sin(at.h * std::numbers::pi / 180.0);
	double worst = 0.0;
	for (int shift = 0; shift <= 16; shift += 8) {
		for (int delta : { -1, 1 }) {
			const int channel = static_cast<int>((argb >> shift) & 0xffu) + delta;
			if (channel < 0 || channel > 255) {
				continue;
			}
			const uint32_t moved = (argb & ~(0xffu << shift)) | (static_cast<uint32_t>(channel) << shift);
			const Oklch m = ui::toOklch(moved);
			const double a = m.C * std::cos(m.h * std::numbers::pi / 180.0);
			const double b = m.C * std::sin(m.h * std::numbers::pi / 180.0);
			worst = std::max(worst, std::sqrt((m.L - at.L) * (m.L - at.L) + (a - a0) * (a - a0) + (b - b0) * (b - b0)));
		}
	}
	return worst;
}

/// The offsets every case sweeps: the whole circle in 30s, both ends, and some that are not round.
constexpr std::array<double, 17> kOffsets { -180, -150, -120, -90, -60, -30, -7, 1, 30, 60, 90, 120, 150, 161, 180, 230 - 360, 45.5 };

struct Named {
	const char* name;
	uint32_t ui::ChromePalette::*member;
};
constexpr std::array<Named, 13> kChromeMembers { {
	{ "void", &ui::ChromePalette::voidGround },
	{ "surface", &ui::ChromePalette::surface },
	{ "surfaceRaised", &ui::ChromePalette::surfaceRaised },
	{ "line", &ui::ChromePalette::line },
	{ "ink", &ui::ChromePalette::ink },
	{ "muted", &ui::ChromePalette::muted },
	{ "disabled", &ui::ChromePalette::disabled },
	{ "pointer", &ui::ChromePalette::pointer },
	{ "notifyGround", &ui::ChromePalette::notifyGround },
	{ "accent", &ui::ChromePalette::accent },
	{ "value", &ui::ChromePalette::value },
	{ "modulation", &ui::ChromePalette::modulation },
	{ "disabledVeil", &ui::ChromePalette::disabledVeil },
} };

double wrapDegrees(double d) {
	return std::remainder(d, 360.0);
}

/// How far `turned` is from `base` turned by `offset` with ITS OWN chroma — the hue error as a distance in
/// the a/b plane.
double hueError(const Oklch& base, const Oklch& turned, double offset) {
	const double want = (base.h + offset) * std::numbers::pi / 180.0;
	const double got = turned.h * std::numbers::pi / 180.0;
	return std::hypot(turned.C * std::cos(got) - turned.C * std::cos(want),
	                  turned.C * std::sin(got) - turned.C * std::sin(want));
}

/// Everything the offset turns, as (name, base, turned) at `offset`.
struct Pair {
	std::string name;
	uint32_t base;
	uint32_t turned;
	bool tint;
};
std::vector<Pair> pairsAt(double offset) {
	const ui::ChromePalette turned = ui::rotatedPalette(ui::kChromeBase, offset);
	std::vector<Pair> out;
	for (const Named& n : kChromeMembers) {
		out.push_back({ n.name, ui::kChromeBase.*(n.member), turned.*(n.member), false });
	}
	for (size_t i = 0; i < ui::kInstanceTintCount; ++i) {
		out.push_back({ "tint" + std::to_string(i), ui::kChromeBase.instanceTints[i], turned.instanceTints[i], true });
	}
	return out;
}

/// Puts the process back at offset 0 whatever a case did — the palette is process-global.
struct ResetHue {
	~ResetHue() {
		(void)ui::setChromeHueOffset(0.0);
		(void)ui::setAccentHueOffset(0.0);
	}
};

} // namespace

TEST_CASE("backlog LG: the chrome hue turns every chrome token in OKLCH — L kept, h moved by exactly the offset, C "
          "kept where sRGB allows and never grown") {
	double worstL = 0.0;
	double worstRatio = 0.0;
	double largestTolerance = 0.0;
	double leastTintChroma = 1.0;
	for (const double offset : kOffsets) {
		for (const Pair& p : pairsAt(offset)) {
			CAPTURE(p.name);
			CAPTURE(offset);
			const Oklch before = ui::toOklch(p.base);
			const Oklch after = ui::toOklch(p.turned);
			const double tolerance = kGridSteps * byteStep(p.turned);
			CAPTURE(tolerance);
			largestTolerance = std::max(largestTolerance, tolerance);
			// ALPHA IS CARRIED.
			CHECK((p.turned >> 24) == (p.base >> 24));
			// L, THE CLAIM THE SETTING IS FOR: "keep all the brightness the same".
			CHECK(std::abs(after.L - before.L) <= tolerance);
			worstL = std::max(worstL, std::abs(after.L - before.L));
			worstRatio = std::max(worstRatio, std::abs(after.L - before.L) / tolerance);
			// C NEVER GROWS. Where the gamut cuts it (tints only, measured), it shrinks; the chrome keeps it.
			CHECK(after.C <= before.C + tolerance);
			if (!p.tint) {
				CHECK(std::abs(after.C - before.C) <= tolerance);
				worstRatio = std::max(worstRatio, std::abs(after.C - before.C) / tolerance);
			}
			else {
				leastTintChroma = std::min(leastTintChroma, after.C / before.C);
			}
			// h MOVED BY THE OFFSET, as a distance in the a/b plane at the colour's own chroma.
			CHECK(hueError(before, after, offset) <= tolerance);
			worstRatio = std::max(worstRatio, hueError(before, after, offset) / tolerance);
		}
	}
	MESSAGE("worst |dL| " << worstL << ", worst error as a fraction of its bound " << worstRatio
	                      << ", largest bound " << largestTolerance << ", least tint chroma kept " << leastTintChroma);
	// AND THE GAMUT CUT IS REAL, which is why C is "never grown" for the tints rather than "kept": a tint
	// that kept all its chroma at every offset would mean the map below was never exercised.
	CHECK(leastTintChroma < 0.9);
	CHECK(leastTintChroma > 0.3);
}

TEST_CASE("backlog LG: the hue moves the TINTED greys too — the mock's greys are navy, and the offset is what "
          "turns them") {
	// THE ENTRY'S PREMISE, CORRECTED BY MEASUREMENT: it called surfaces, ink and lines neutral. They are
	// not; each has a hue (≈270°) and a chroma a turn can see. Pinned so a re-skin to true greys says so.
	for (const Named& n : kChromeMembers) {
		CAPTURE(n.name);
		CHECK(ui::toOklch(ui::kChromeBase.*(n.member)).C > 0.005);
	}
	const ui::ChromePalette half = ui::rotatedPalette(ui::kChromeBase, 180.0);
	CHECK(half.line != ui::kChromeBase.line);
	CHECK(half.surface != ui::kChromeBase.surface);
	CHECK(half.muted != ui::kChromeBase.muted);
}

TEST_CASE("backlog LG: offset 0 is the mock's palette bit for bit, and a whole turn is too") {
	for (const double offset : { 0.0, 360.0, -360.0, 720.0 }) {
		CAPTURE(offset);
		for (const Pair& p : pairsAt(offset)) {
			CAPTURE(p.name);
			CHECK(p.turned == p.base);
		}
	}
	// NOT ONLY FOR TODAY'S TOKENS: a re-skin may pick any colour, and offset 0 must hand back every one of them
	// unchanged. Two things hold it: `rotateHue` does not turn at 0, and — MEASURED with that shortcut deleted
	// (a GREEN known-bad, kept on record) — the double-precision OKLab round trip is itself byte-exact over this
	// whole grid. So this pins the outcome, not the road.
	size_t moved = 0;
	for (uint32_t r = 0; r < 256; r += 3) {
		for (uint32_t g = 0; g < 256; g += 3) {
			for (uint32_t b = 0; b < 256; b += 3) {
				const uint32_t c = 0xff000000u | (r << 16) | (g << 8) | b;
				moved += ui::rotateHue(c, 0.0) != c ? 1u : 0u;
			}
		}
	}
	CHECK(moved == 0);
	// AND THE BASE IS THE MOCK'S: every member against the token it was written from.
	CHECK(ui::kChromeBase.accent == tokens::kAccent);
	CHECK(ui::kChromeBase.value == tokens::kValue);
	CHECK(ui::kChromeBase.modulation == tokens::kMod);
	CHECK(ui::kChromeBase.voidGround == tokens::kVoidPerform);
	CHECK(ui::kChromeBase.surfaceRaised == tokens::kSurface2);
	CHECK(ui::kChromeBase.disabledVeil == ui::withAlpha(tokens::kSurface2, 0.80f));
}

TEST_CASE("backlog LG: the theme's roles read the offset in force, and setting it tells every watch once") {
	ResetHue reset;
	REQUIRE(ui::chromeHueOffset() == 0.0);
	REQUIRE(ui::kAccent == tokens::kAccent);

	int heard = 0;
	double last = 0.0;
	ui::ChromeHueWatch watch([&](double d) {
		++heard;
		last = d;
	});
	CHECK(ui::setChromeHueOffset(120.0) == 1);
	CHECK(heard == 1);
	CHECK(last == 120.0);
	CHECK(ui::chromeHueOffset() == 120.0);
	// THE LIVE ROLES ARE THE TURNED PALETTE, read through theme.h's names.
	const ui::ChromePalette want = ui::rotatedPalette(ui::kChromeBase, 120.0);
	CHECK(ui::kAccent == want.accent);
	CHECK(ui::kValue == want.value);
	CHECK(ui::kModulation == want.modulation);
	CHECK(ui::kInk == want.ink);
	CHECK(ui::kMuted == want.muted);
	CHECK(ui::kDisabled == want.disabled);
	CHECK(ui::kSurface == want.surface);
	CHECK(ui::kSurfaceRaised == want.surfaceRaised);
	CHECK(ui::kLine == want.line);
	CHECK(ui::kVoid == want.voidGround);
	CHECK(ui::kPointer == want.pointer);
	CHECK(ui::kNotifyGround == want.notifyGround);
	CHECK(ui::kDisabledVeil == want.disabledVeil);
	CHECK(ui::instanceTintFor(1) == want.instanceTints[0]);
	CHECK(ui::instanceEdgeColor(ui::instanceTintFor(3)) == ui::withAlpha(want.instanceTints[2], ui::kPanelEdgeTintAlpha));
	// NOT VACUOUS: the turn moved them.
	CHECK(ui::kAccent != tokens::kAccent);
	CHECK(ui::kLine != tokens::kLine);

	// THE SAME OFFSET AGAIN TELLS NOBODY; THE RANGE HOLDS; A NON-NUMBER IS 0.
	CHECK(ui::setChromeHueOffset(120.0) == 0);
	CHECK(heard == 1);
	(void)ui::setChromeHueOffset(500.0);
	CHECK(ui::chromeHueOffset() == ui::kChromeHueMax);
	(void)ui::setChromeHueOffset(-500.0);
	CHECK(ui::chromeHueOffset() == ui::kChromeHueMin);
	(void)ui::setChromeHueOffset(std::nan(""));
	CHECK(ui::chromeHueOffset() == 0.0);
	CHECK(ui::kAccent == tokens::kAccent);
}

TEST_CASE("backlog LG: the data colours never turn — the sounds, the stereo ears, the meters") {
	ResetHue reset;
	// AT OFFSET 0 THE METERS' FIXED COLOURS ARE THE CHROME'S, so moving the meters onto them changed no
	// pixel of the default editor.
	REQUIRE(ui::chromeHueOffset() == 0.0);
	CHECK(ui::kMeterAccent == ui::kAccent);
	CHECK(ui::kMeterValue == ui::kValue);
	for (const double offset : kOffsets) {
		CAPTURE(offset);
		(void)ui::setChromeHueOffset(offset);
		CHECK(ui::kMeterAccent == tokens::kAccent);
		CHECK(ui::kMeterValue == tokens::kValue);
		CHECK(ui::kModulationL == tokens::kModL);
		CHECK(ui::kModulationR == tokens::kModR);
		CHECK(ui::kModulationStereo == tokens::kModStereo);
		const std::array<uint32_t, 8> seeds { tokens::kSeed1, tokens::kSeed2, tokens::kSeed3, tokens::kSeed4,
			                                  tokens::kSeed5, tokens::kSeed6, tokens::kSeed7, tokens::kSeed8 };
		CHECK(ui::kSeedHues == seeds);
	}
	// NOT VACUOUS: at the last offset the chrome did move while all of the above held.
	CHECK(ui::kAccent != ui::kMeterAccent);
}

TEST_CASE("backlog LG: the eight window hues stay eight, evenly turned, at every offset — and a nested chain still "
          "never wears its holder's") {
	ResetHue reset;
	for (const double offset : kOffsets) {
		CAPTURE(offset);
		(void)ui::setChromeHueOffset(offset);
		std::set<uint32_t> distinct;
		std::vector<double> hues;
		for (uint32_t id = 1; id <= ui::kInstanceTintCount; ++id) {
			// THE TINT IN FORCE IS THE BASE TINT TURNED — so the guarantees below are about the turned palette.
			CHECK(ui::instanceTintFor(id) == ui::rotateHue(ui::kChromeBase.instanceTints[id - 1], offset));
			distinct.insert(ui::instanceTintFor(id));
			hues.push_back(ui::toOklch(ui::instanceTintFor(id)).h);
		}
		CHECK(distinct.size() == ui::kInstanceTintCount);
		// THE SPACING IS THE BASE'S: each neighbour's hue gap is unchanged by the turn (to rounding).
		for (size_t i = 0; i + 1 < hues.size(); ++i) {
			const double baseGap = wrapDegrees(ui::toOklch(ui::kChromeBase.instanceTints[i + 1]).h
			                                   - ui::toOklch(ui::kChromeBase.instanceTints[i]).h);
			CHECK(std::abs(wrapDegrees(hues[i + 1] - hues[i]) - baseGap) < 3.0);
		}
		// `DeviceNest::under`'s guarantee: a chain's first seven devices never wear the holder's colour.
		for (uint32_t holder = 1; holder <= ui::kInstanceTintCount; ++holder) {
			const ui::DeviceNest nest = ui::DeviceNest::under({}, holder);
			for (uint32_t id = 1; id < ui::kInstanceTintCount; ++id) {
				CHECK(ui::instanceTintFor(id, nest) != ui::instanceTintFor(holder));
			}
		}
	}
}

TEST_CASE("backlog LG: the offset is a desk preference — written to the editor prefs, restored from them, and "
          "never anywhere else") {
	ResetHue reset;
	// THIS BINARY'S main() POINTS XDG_CONFIG_HOME AT A SCRATCH DESK (backlog Q: a ForTest seam once rewrote
	// the real preferences on every ctest). Refuse to run against anything else.
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	REQUIRE(xdg != nullptr);
	REQUIRE(std::string(xdg).find("feathers-test-config-") != std::string::npos);
	std::error_code code;
	std::filesystem::remove(EditorPrefs::path(), code);

	{
		EditorPrefs prefs;
		prefs.load();
		ui::restoreChromeHue(prefs);
		CHECK(ui::chromeHueOffset() == 0.0); // no line: the mock's colours
		const auto watch = ui::keepChromeHueIn(prefs, nullptr);
		(void)ui::setChromeHueOffset(-75.0);
		CHECK(prefs.number(ui::kChromeHuePrefKey, 0.0) == -75.0);
	}
	// A FRESH READ OF THE FILE, as the next editor makes it.
	{
		EditorPrefs again;
		again.load();
		CHECK(again.number(ui::kChromeHuePrefKey, 0.0) == -75.0);
		(void)ui::setChromeHueOffset(0.0); // no watch alive: the file keeps -75
		ui::restoreChromeHue(again);
		CHECK(ui::chromeHueOffset() == -75.0);
		CHECK(ui::kAccent == ui::rotateHue(tokens::kAccent, -75.0));
	}
	// THE FILE HOLDS THE LINE BY ITS KEY, and that is the only file this wrote.
	std::ifstream file(EditorPrefs::path());
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	CHECK(text.find("ui.hue=-75") != std::string::npos);
	std::filesystem::remove(EditorPrefs::path(), code);
}

TEST_CASE("backlog LG: the settings screen lays every control inside its panel, clear of each other, at Feathers' "
          "size, the rack's and smaller") {
	const std::array<std::pair<float, float>, 4> sizes { { { 1240.0f, 800.0f }, { 720.0f, 466.0f }, { 640.0f, 420.0f },
		                                                   { 600.0f, 400.0f } } };
	for (const auto& [w, h] : sizes) {
		CAPTURE(w);
		CAPTURE(h);
		ui::SettingsScreen screen;
		screen.uiScaleNow = [] { return 1.0; };
		screen.setBounds(0.0f, 0.0f, w, h);
		const visage::Bounds panel = screen.panelBounds();
		std::vector<visage::Bounds> boxes { screen.closeBounds(), screen.hueStripBounds(), screen.hueResetBounds(),
		                                screen.accentStripBounds(), screen.accentResetBounds(),
			                                screen.tooltipSegmentBounds(0), screen.tooltipSegmentBounds(1),
		                                screen.windowSideSegmentBounds(0), screen.windowSideSegmentBounds(1),
		                                screen.arpsFrontSegmentBounds(0), screen.arpsFrontSegmentBounds(1) };
		for (size_t i = 0; i < ui::SettingsScreen::scaleLadder().size(); ++i) {
			boxes.push_back(screen.scaleSegmentBounds(i));
		}
		const auto inside = [&](visage::Bounds b) {
			return b.x() >= panel.x() && b.y() >= panel.y() && b.right() <= panel.right() + 0.01f
			       && b.bottom() <= panel.bottom() + 0.01f && b.width() > 0.0f && b.height() > 0.0f;
		};
		for (size_t i = 0; i < boxes.size(); ++i) {
			CAPTURE(i);
			CHECK(inside(boxes[i]));
			for (size_t j = i + 1; j < boxes.size(); ++j) {
				CAPTURE(j);
				const visage::Bounds a = boxes[i];
				const visage::Bounds b = boxes[j];
				const bool overlap = a.x() < b.right() - 0.01f && b.x() < a.right() - 0.01f
				                     && a.y() < b.bottom() - 0.01f && b.y() < a.bottom() - 0.01f;
				CHECK_FALSE(overlap);
			}
		}
		// THE STRIP MAPS ITS ENDS TO THE RANGE'S, AND AN OFFSET'S PLACE BACK TO THE OFFSET.
		CHECK(screen.hueAt(screen.hueStripBounds().x()) == ui::kChromeHueMin);
		CHECK(screen.hueAt(screen.hueStripBounds().right()) == ui::kChromeHueMax);
		CHECK(screen.hueAt(screen.hueX(37.0)) == 37.0);
	}
}

TEST_CASE("backlog OD: the window-controls side defaults to the platform's norm, is a desk preference written to the "
          "editor prefs and restored from them, and never anywhere else") {
	struct Reset {
		ui::WindowControlsSide before = ui::windowControlsSide();
		~Reset() { (void)ui::setWindowControlsSide(before); }
	} reset;
	// THE NORM, BOTH ANSWERS: left on macOS, right everywhere else.
	CHECK(ui::platformWindowControlsSide(true) == ui::WindowControlsSide::Left);
	CHECK(ui::platformWindowControlsSide(false) == ui::WindowControlsSide::Right);
#if defined(__APPLE__)
	CHECK(ui::platformDefaultWindowControlsSide() == ui::WindowControlsSide::Left);
#else
	CHECK(ui::platformDefaultWindowControlsSide() == ui::WindowControlsSide::Right);
#endif
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	REQUIRE(xdg != nullptr);
	REQUIRE(std::string(xdg).find("feathers-test-config-") != std::string::npos);
	std::error_code code;
	std::filesystem::remove(EditorPrefs::path(), code);
	const ui::WindowControlsSide opposite = ui::platformDefaultWindowControlsSide() == ui::WindowControlsSide::Left
	                                            ? ui::WindowControlsSide::Right
	                                            : ui::WindowControlsSide::Left;
	{
		EditorPrefs prefs;
		prefs.load();
		// NO LINE: the platform's norm, whatever the process held before.
		(void)ui::setWindowControlsSide(opposite);
		ui::restoreWindowControlsSide(prefs);
		CHECK(ui::windowControlsSide() == ui::platformDefaultWindowControlsSide());
		const auto watch = ui::keepWindowControlsSideIn(prefs);
		CHECK(ui::setWindowControlsSide(opposite) == 1);
		CHECK(ui::setWindowControlsSide(opposite) == 0); // unchanged: nobody told
		CHECK(prefs.text(ui::kWindowControlsSidePrefKey)
		      == (opposite == ui::WindowControlsSide::Left ? "left" : "right"));
	}
	{
		EditorPrefs again;
		again.load();
		(void)ui::setWindowControlsSide(ui::platformDefaultWindowControlsSide());
		ui::restoreWindowControlsSide(again); // the next editor
		CHECK(ui::windowControlsSide() == opposite);
	}
	std::ifstream file(EditorPrefs::path());
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	CHECK(text.find("ui.window_controls=") != std::string::npos);
	std::filesystem::remove(EditorPrefs::path(), code);
}

TEST_CASE("backlog OD: the settings screen's Window controls row sets the side, and the × follows it") {
	struct Reset {
		ui::WindowControlsSide before = ui::windowControlsSide();
		~Reset() { (void)ui::setWindowControlsSide(before); }
	} reset;
	ui::SettingsScreen screen;
	screen.uiScaleNow = [] { return 1.0; };
	screen.setBounds(0.0f, 0.0f, 1240.0f, 800.0f);
	const auto press = [&](visage::Bounds box) {
		visage::MouseEvent e;
		e.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		screen.mouseDown(e);
		screen.mouseUp(e);
	};
	press(screen.windowSideSegmentBounds(0));
	CHECK(ui::windowControlsSide() == ui::WindowControlsSide::Left);
	CHECK(screen.closeBounds().x() == doctest::Approx(screen.panelBounds().x()));
	press(screen.windowSideSegmentBounds(1));
	CHECK(ui::windowControlsSide() == ui::WindowControlsSide::Right);
	CHECK(screen.closeBounds().right() == doctest::Approx(screen.panelBounds().right()));
	// THE ROW CLEARS THE HUE STRIP and sits inside the panel at the smallest size.
	for (const float h : { 800.0f, 400.0f }) {
		screen.setBounds(0.0f, 0.0f, 600.0f, h);
		const visage::Bounds seg = screen.windowSideSegmentBounds(1);
		CHECK(seg.bottom() <= screen.hueStripBounds().y());
		CHECK(seg.y() >= screen.tooltipSegmentBounds(1).bottom());
		CHECK(seg.right() <= screen.panelBounds().right());
	}
}

TEST_CASE("arps sent to front of chain: the settings row sets it, ON by default, kept in the editor's prefs and not the patch") {
	// A setting for "arps sent to front of chain", true by default.
	struct Reset {
		bool before = ui::emittersToFront();
		~Reset() { (void)ui::setEmittersToFront(before); }
	} reset;
	std::error_code code;
	std::filesystem::remove(EditorPrefs::path(), code);
	{
		EditorPrefs prefs;
		prefs.load();
		// NO LINE: ON, whatever the process held before.
		(void)ui::setEmittersToFront(false);
		ui::restoreEmittersToFront(prefs);
		CHECK(ui::emittersToFront());
		const auto watch = ui::keepEmittersToFrontIn(prefs);
		// THE ROW: Off (segment 1), then On (segment 0), each a press.
		ui::SettingsScreen screen;
		screen.uiScaleNow = [] { return 1.0; };
		screen.setBounds(0.0f, 0.0f, 1240.0f, 800.0f);
		const auto press = [&](visage::Bounds box) {
			visage::MouseEvent e;
			e.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
			screen.mouseDown(e);
			screen.mouseUp(e);
		};
		press(screen.arpsFrontSegmentBounds(1));
		CHECK_FALSE(ui::emittersToFront());
		CHECK(prefs.text(ui::kEmittersFrontPrefKey) == "off");
		press(screen.arpsFrontSegmentBounds(0));
		CHECK(ui::emittersToFront());
		CHECK(prefs.text(ui::kEmittersFrontPrefKey) == "on");
		press(screen.arpsFrontSegmentBounds(1));
	}
	{
		EditorPrefs again;
		again.load();
		(void)ui::setEmittersToFront(true);
		ui::restoreEmittersToFront(again); // the next editor reads OFF from the file
		CHECK_FALSE(ui::emittersToFront());
	}
	std::filesystem::remove(EditorPrefs::path(), code);
}

TEST_CASE("repaint: the settings row sets the canvas's full-redraw flag, the build's default without a line, kept in the editor's prefs") {
	// The full-redraw choice lives in the settings screen. The value is visage's own flag
	// (`Canvas::fullRedraw`, which `Canvas::submit` reads), so the claim is made against THAT, not a copy of it.
	struct Reset {
		bool before = visage::Canvas::fullRedraw();
		~Reset() { visage::Canvas::setFullRedraw(before); }
	} reset;
	// THE COMPILE-TIME DEFAULT IS THE BUILD'S FEATHERS_FULL_REDRAW, stated from the define rather than from the code.
#ifdef FEATHERS_FULL_REDRAW
	constexpr bool kBuildDefault = true;
#else
	constexpr bool kBuildDefault = false;
#endif
	CHECK(ui::fullRedrawDefault() == kBuildDefault);
	CHECK(visage::Canvas::kFullRedrawCompiledDefault == kBuildDefault);
	std::error_code code;
	std::filesystem::remove(EditorPrefs::path(), code);
	{
		EditorPrefs prefs;
		prefs.load();
		// NO LINE: the build's default, whatever the process held before.
		visage::Canvas::setFullRedraw(!kBuildDefault);
		ui::restoreFullRedraw(prefs);
		CHECK(visage::Canvas::fullRedraw() == kBuildDefault);
		int repaints = 0;
		const auto watch = ui::keepFullRedrawIn(prefs, [&] { ++repaints; });
		ui::SettingsScreen screen;
		screen.uiScaleNow = [] { return 1.0; };
		screen.setBounds(0.0f, 0.0f, 1240.0f, 800.0f);
		const auto press = [&](visage::Bounds box) {
			visage::MouseEvent e;
			e.position = { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
			screen.mouseDown(e);
			screen.mouseUp(e);
		};
		// THE ROW: first the segment the default is NOT (so the press moves something in either build), then back.
		const int away = kBuildDefault ? 0 : 1; // Partial is 0, Every frame is 1
		press(screen.repaintSegmentBounds(away));
		CHECK(visage::Canvas::fullRedraw() == !kBuildDefault);
		CHECK(prefs.text(ui::kFullRedrawPrefKey) == (kBuildDefault ? "off" : "on"));
		press(screen.repaintSegmentBounds(1 - away));
		CHECK(visage::Canvas::fullRedraw() == kBuildDefault);
		CHECK(prefs.text(ui::kFullRedrawPrefKey) == (kBuildDefault ? "on" : "off"));
		CHECK(repaints == 2);
		// A press on the segment already chosen moves nothing, writes nothing, repaints nothing.
		press(screen.repaintSegmentBounds(1 - away));
		CHECK(repaints == 2);
		// THE SECTION: in the About column, under its DEBUG OPTIONS header, inside the panel.
		CHECK(screen.repaintSegmentBounds(0).x() > screen.arpsFrontSegmentBounds(1).right());
		CHECK(screen.repaintSegmentBounds(0).y() > screen.debugOptionsTop());
		CHECK(screen.debugOptionsTop() >= screen.plugLinkBounds().bottom()); // below the About lines, the plug link too
		CHECK(screen.panelBounds().contains(screen.repaintSegmentBounds(1).x() + screen.repaintSegmentBounds(1).width() - 1.0f,
		                                    screen.repaintSegmentBounds(1).y() + 1.0f));
		// Leave the file saying the OPPOSITE of the build's default, so the next editor's read is not the default.
		press(screen.repaintSegmentBounds(kBuildDefault ? 0 : 1));
	}
	{
		EditorPrefs again;
		again.load();
		visage::Canvas::setFullRedraw(kBuildDefault);
		ui::restoreFullRedraw(again); // the next editor reads the stored choice, not the default
		CHECK(visage::Canvas::fullRedraw() == !kBuildDefault);
	}
	std::filesystem::remove(EditorPrefs::path(), code);
}

TEST_CASE("frame timing: rolling p50/p95/max over the window, intervals only between presented frames") {
	ui::FrameTiming timing;
	// 1..100 ms of draw, one per presented frame, 16 ms apart; and a no-op draw that presented nothing between each.
	for (int i = 1; i <= 100; ++i) {
		const double start = 16.0 * i;
		timing.drawn(start, 0.5, static_cast<double>(i), true);
		timing.drawn(start + 8.0, 0.0, 999.0, false);
	}
	const ui::FrameTiming::Stats draw = timing.draw.stats();
	CHECK(draw.count == 100);
	CHECK(draw.p50 == 50.0);
	CHECK(draw.p95 == 95.0);
	CHECK(draw.max == 100.0);
	const ui::FrameTiming::Stats interval = timing.interval.stats();
	CHECK(interval.count == 99); // the first frame has no predecessor
	CHECK(interval.p50 == 16.0);
	CHECK(interval.max == 16.0);
	// A pause (the editor hidden) is not an interval.
	timing.restartIntervals();
	timing.drawn(100000.0, 0.0, 1.0, true);
	CHECK(timing.interval.stats().count == 99);
	CHECK(timing.interval.stats().max == 16.0);
	// THE WINDOW ROLLS: after kWindow more samples of 2 ms, the old ones are gone.
	for (size_t i = 0; i < ui::FrameTiming::kWindow; ++i) {
		timing.ticked(2.0);
	}
	timing.ticked(7.0);
	const ui::FrameTiming::Stats tick = timing.tick.stats();
	CHECK(tick.count == ui::FrameTiming::kWindow);
	CHECK(tick.p50 == 2.0);
	CHECK(tick.max == 7.0);
	// 101 draws now (the one after the pause too): nearest-rank p50 is the 51st smallest (1,1,2,3.. so 50), p95 the 96th (95). Waits: 100 x 0.5, 1 x 0.
	CHECK(timing.summary().find("draw p50 50.0 ms, p95 95.0 ms, max 100.0 ms; wait p50 0.5 ms, p95 0.5 ms") == 0);
}

TEST_CASE("tracked text: a least-recently-used cache, so a string in use is measured once however many distinct strings "
          "the session has shown") {
	// On the Mac, grabbing a new control was slow while recently grabbed controls responded faster
	// in the UI. The cache used to keep the FIRST 64 strings it saw and re-measure every other string on
	// every draw for the rest of the process. BY COUNT: `trackedTextMeasures` is every measure the cache made.
	ui::clearFontCache();
	const visage::Font& face = ui::font(ui::kControlSize);
	const size_t cap = ui::trackedTextCacheCapacity();
	REQUIRE(cap >= 1024); // a working set's worth, well above the old 64
	// A LONG SESSION FIRST: more distinct strings than the cache holds, so the cache is full before the hot string
	// arrives — the order in which the old cache failed.
	for (size_t i = 0; i < cap + 100; ++i) {
		(void)ui::trackedWidth(face, "seen " + std::to_string(i), 0.0f);
	}
	CHECK(ui::trackedTextCacheSize() == cap);
	CHECK(ui::trackedTextEvictions() == 100);
	// THEN A FRAME LOOP: each frame shows the same 20 captions and one new readout (a value counting up under a drag).
	std::vector<std::string> captions;
	for (int i = 0; i < 20; ++i) {
		captions.push_back("CAPTION " + std::to_string(i));
	}
	const auto frame = [&](int n) {
		for (const std::string& c : captions) {
			(void)ui::trackedWidth(face, c, 0.28f);
		}
		(void)ui::trackedWidth(face, std::to_string(n) + ".0 dB", 0.0f);
	};
	const uint64_t before = ui::trackedTextMeasures();
	frame(0);
	CHECK(ui::trackedTextMeasures() - before == 21); // the first frame measures each new string once
	for (int n = 1; n <= static_cast<int>(2 * cap); ++n) {
		const uint64_t start = ui::trackedTextMeasures();
		frame(n);
		CHECK(ui::trackedTextMeasures() - start == 1); // only the new readout: the captions are never measured again
		if (ui::trackedTextMeasures() - start != 1) {
			break; // one failure says it; 8192 would bury it
		}
	}
	// And the widths are the measured ones, from the cache as from a cold measure.
	const float cached = ui::trackedWidth(face, captions.front(), 0.28f);
	ui::clearFontCache();
	CHECK(ui::trackedWidth(ui::font(ui::kControlSize), captions.front(), 0.28f) == cached);
	ui::clearFontCache();
}

// THE ACCENT HUE: a second colour axis in settings,
// a hue for accents. A second offset turns the role family further than the chrome hue does. By VALUE: each
// role's OKLCH h is base + chrome + accent, and everything else ignores the accent.

namespace {

constexpr std::array<Named, 3> kRoleMembers { {
	{ "accent", &ui::ChromePalette::accent },
	{ "value", &ui::ChromePalette::value },
	{ "modulation", &ui::ChromePalette::modulation },
} };

bool isRole(uint32_t ui::ChromePalette::*member) {
	return member == &ui::ChromePalette::accent || member == &ui::ChromePalette::value
	       || member == &ui::ChromePalette::modulation;
}

/// A coarser grid for the composed sweeps: both offsets over the circle, with some that are not round.
constexpr std::array<double, 9> kAxisOffsets { -180, -120, -45, -7, 0, 30, 90, 161, 180 };

} // namespace

TEST_CASE("accent hue: a role's OKLCH h is base + chrome + accent, L kept, C never grown — and the family keeps its "
          "spacing") {
	double worstRatio = 0.0;
	for (const double chrome : kAxisOffsets) {
		for (const double accent : kAxisOffsets) {
			CAPTURE(chrome);
			CAPTURE(accent);
			const ui::ChromePalette turned = ui::rotatedPalette(ui::kChromeBase, chrome, accent);
			std::array<double, 3> hues {};
			for (size_t i = 0; i < kRoleMembers.size(); ++i) {
				const Named& n = kRoleMembers[i];
				CAPTURE(n.name);
				const Oklch before = ui::toOklch(ui::kChromeBase.*(n.member));
				const Oklch after = ui::toOklch(turned.*(n.member));
				const double tolerance = kGridSteps * byteStep(turned.*(n.member));
				CHECK(std::abs(after.L - before.L) <= tolerance);
				CHECK(after.C <= before.C + tolerance);
				CHECK(hueError(before, after, chrome + accent) <= tolerance);
				worstRatio = std::max(worstRatio, hueError(before, after, chrome + accent) / tolerance);
				hues[i] = after.h;
			}
			// THE FAMILY'S SPACING IS THE BASE'S, so accent never walks onto value: every pair's hue gap is unchanged.
			for (size_t i = 0; i < hues.size(); ++i) {
				for (size_t j = i + 1; j < hues.size(); ++j) {
					const double baseGap = wrapDegrees(ui::toOklch(ui::kChromeBase.*(kRoleMembers[j].member)).h
					                                   - ui::toOklch(ui::kChromeBase.*(kRoleMembers[i].member)).h);
					CHECK(std::abs(wrapDegrees(hues[j] - hues[i]) - baseGap) < 3.0);
				}
			}
		}
	}
	MESSAGE("worst role hue error as a fraction of its bound " << worstRatio);
	// NOT VACUOUS: the accent alone moves the roles.
	CHECK(ui::rotatedPalette(ui::kChromeBase, 0.0, 90.0).accent != ui::kChromeBase.accent);
}

TEST_CASE("accent hue: the greys and the eight window hues ignore the accent offset — they turn by the chrome hue "
          "alone, byte for byte") {
	for (const double chrome : kAxisOffsets) {
		const ui::ChromePalette chromeOnly = ui::rotatedPalette(ui::kChromeBase, chrome);
		for (const double accent : kAxisOffsets) {
			CAPTURE(chrome);
			CAPTURE(accent);
			const ui::ChromePalette both = ui::rotatedPalette(ui::kChromeBase, chrome, accent);
			for (const Named& n : kChromeMembers) {
				if (isRole(n.member)) {
					continue;
				}
				CAPTURE(n.name);
				CHECK(both.*(n.member) == chromeOnly.*(n.member));
			}
			CHECK(both.instanceTints == chromeOnly.instanceTints);
		}
	}
}

TEST_CASE("accent hue: offset 0 is the chrome-only palette bit for bit at every chrome offset, and the composed "
          "case is pinned to the byte") {
	for (const double chrome : kOffsets) {
		CAPTURE(chrome);
		const ui::ChromePalette a = ui::rotatedPalette(ui::kChromeBase, chrome);
		const ui::ChromePalette b = ui::rotatedPalette(ui::kChromeBase, chrome, 0.0);
		for (const Named& n : kChromeMembers) {
			CAPTURE(n.name);
			CHECK(b.*(n.member) == a.*(n.member));
		}
		CHECK(b.instanceTints == a.instanceTints);
	}
	// BOTH AT 0 IS THE MOCK.
	const ui::ChromePalette none = ui::rotatedPalette(ui::kChromeBase, 0.0, 0.0);
	for (const Named& n : kChromeMembers) {
		CAPTURE(n.name);
		CHECK(none.*(n.member) == ui::kChromeBase.*(n.member));
	}
	// THE COMPOSED CASE, chrome +60 and accent +45: one rotation of the roles by 105 (never two in a row, which
	// would round to bytes twice), the greys by 60 — and the bytes themselves, so a change to the maths says so.
	const ui::ChromePalette composed = ui::rotatedPalette(ui::kChromeBase, 60.0, 45.0);
	CHECK(composed.accent == ui::rotateHue(ui::kChromeBase.accent, 105.0));
	CHECK(composed.value == ui::rotateHue(ui::kChromeBase.value, 105.0));
	CHECK(composed.modulation == ui::rotateHue(ui::kChromeBase.modulation, 105.0));
	CHECK(composed.line == ui::rotateHue(ui::kChromeBase.line, 60.0));
	CHECK(composed.accent == 0xffde8b9fu);
	CHECK(composed.value == 0xff7ccba7u);
	CHECK(composed.modulation == 0xffe98b8du);
	CHECK(composed.line == 0xff462b43u);
	CHECK(composed.instanceTints[0] == 0xffb18edeu);
}

TEST_CASE("accent hue: the live roles read chrome + accent, the accent tells only its own watches, and the range "
          "holds") {
	ResetHue reset;
	REQUIRE(ui::chromeHueOffset() == 0.0);
	REQUIRE(ui::accentHueOffset() == 0.0);
	int chromeHeard = 0;
	int accentHeard = 0;
	double last = 0.0;
	ui::ChromeHueWatch chromeWatch([&](double) { ++chromeHeard; });
	ui::AccentHueWatch accentWatch([&](double d) {
		++accentHeard;
		last = d;
	});
	CHECK(ui::setAccentHueOffset(45.0) == 1);
	CHECK(accentHeard == 1);
	CHECK(chromeHeard == 0);
	CHECK(last == 45.0);
	CHECK(ui::chromeHueOffset() == 0.0);
	CHECK(ui::kAccent == ui::rotateHue(tokens::kAccent, 45.0));
	CHECK(ui::kLine == tokens::kLine); // a grey ignores the accent
	// THE CHROME HUE MOVED UNDER A STANDING ACCENT KEEPS THE ACCENT COMPOSED, and tells only its own watch.
	CHECK(ui::setChromeHueOffset(60.0) == 1);
	CHECK(accentHeard == 1);
	const ui::ChromePalette want = ui::rotatedPalette(ui::kChromeBase, 60.0, 45.0);
	CHECK(ui::kAccent == want.accent);
	CHECK(ui::kValue == want.value);
	CHECK(ui::kModulation == want.modulation);
	CHECK(ui::kLine == want.line);
	CHECK(ui::kInk == want.ink);
	CHECK(ui::instanceTintFor(1) == want.instanceTints[0]);
	CHECK(hueError(ui::toOklch(tokens::kAccent), ui::toOklch(ui::kAccent), 105.0) <= kGridSteps * byteStep(ui::kAccent));
	// THE DATA STILL DOES NOT TURN.
	CHECK(ui::kMeterAccent == tokens::kAccent);
	CHECK(ui::kMeterValue == tokens::kValue);
	CHECK(ui::kModulationL == tokens::kModL);
	CHECK(ui::kModulationR == tokens::kModR);
	// SAME AGAIN TELLS NOBODY; THE RANGE; A NON-NUMBER IS 0.
	CHECK(ui::setAccentHueOffset(45.0) == 0);
	(void)ui::setAccentHueOffset(500.0);
	CHECK(ui::accentHueOffset() == ui::kAccentHueMax);
	(void)ui::setAccentHueOffset(-500.0);
	CHECK(ui::accentHueOffset() == ui::kAccentHueMin);
	(void)ui::setAccentHueOffset(std::nan(""));
	CHECK(ui::accentHueOffset() == 0.0);
	CHECK(ui::kAccent == ui::rotateHue(tokens::kAccent, 60.0));
}

TEST_CASE("accent hue: a desk preference — written to the editor prefs, restored from them, and old prefs without the "
          "key load as 0") {
	ResetHue reset;
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	REQUIRE(xdg != nullptr);
	REQUIRE(std::string(xdg).find("feathers-test-config-") != std::string::npos);
	std::error_code code;
	std::filesystem::remove(EditorPrefs::path(), code);
	// OLD PREFS: a chrome hue line and no accent line, as every file written before this existed.
	{
		EditorPrefs old;
		old.load();
		old.setNumber(ui::kChromeHuePrefKey, 30.0);
	}
	{
		EditorPrefs prefs;
		prefs.load();
		(void)ui::setAccentHueOffset(70.0); // whatever the process held before
		ui::restoreAccentHue(prefs);
		CHECK(ui::accentHueOffset() == 0.0);
		const auto watch = ui::keepAccentHueIn(prefs, nullptr);
		(void)ui::setAccentHueOffset(-110.0);
		CHECK(prefs.number(ui::kAccentHuePrefKey, 0.0) == -110.0);
		CHECK(prefs.number(ui::kChromeHuePrefKey, 0.0) == 30.0); // the other axis's line is left alone
	}
	{
		EditorPrefs again;
		again.load();
		CHECK(again.number(ui::kAccentHuePrefKey, 0.0) == -110.0);
		(void)ui::setAccentHueOffset(0.0); // no watch alive: the file keeps -110
		ui::restoreChromeHue(again);
		ui::restoreAccentHue(again);
		CHECK(ui::chromeHueOffset() == 30.0);
		CHECK(ui::accentHueOffset() == -110.0);
		CHECK(ui::kAccent == ui::rotateHue(tokens::kAccent, 30.0 - 110.0));
	}
	std::ifstream file(EditorPrefs::path());
	const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	CHECK(text.find("ui.accent_hue=-110") != std::string::npos);
	std::filesystem::remove(EditorPrefs::path(), code);
}

TEST_CASE("accent hue: the settings strip sets the accent offset by press and drag, resets, and leaves the chrome hue "
          "alone") {
	ResetHue reset;
	ui::SettingsScreen screen;
	screen.uiScaleNow = [] { return 1.0; };
	screen.setBounds(0.0f, 0.0f, 1240.0f, 800.0f);
	const visage::Bounds strip = screen.accentStripBounds();
	// THE STRIP MAPS ITS ENDS TO THE RANGE'S, AND AN OFFSET'S PLACE BACK TO THE OFFSET.
	CHECK(screen.accentAt(strip.x()) == ui::kAccentHueMin);
	CHECK(screen.accentAt(strip.right()) == ui::kAccentHueMax);
	CHECK(screen.accentAt(screen.accentX(37.0)) == 37.0);
	(void)ui::setChromeHueOffset(20.0);
	const float mid = strip.y() + strip.height() * 0.5f;
	visage::MouseEvent down;
	down.position = { screen.accentX(-40.0), mid };
	screen.mouseDown(down);
	CHECK(ui::accentHueOffset() == -40.0);
	visage::MouseEvent drag;
	drag.position = { screen.accentX(90.0), mid + 30.0f }; // a drag may leave the strip vertically
	screen.mouseDrag(drag);
	CHECK(ui::accentHueOffset() == 90.0);
	CHECK(ui::chromeHueOffset() == 20.0);
	screen.mouseUp(drag);
	// AFTER THE RELEASE A DRAG MOVES NOTHING.
	drag.position = { screen.accentX(-150.0), mid };
	screen.mouseDrag(drag);
	CHECK(ui::accentHueOffset() == 90.0);
	// THE LIVE ROLE IS chrome + accent.
	CHECK(ui::kAccent == ui::rotateHue(tokens::kAccent, 110.0));
	// RESET.
	const visage::Bounds r = screen.accentResetBounds();
	visage::MouseEvent press;
	press.position = { r.x() + r.width() * 0.5f, r.y() + r.height() * 0.5f };
	screen.mouseDown(press);
	screen.mouseUp(press);
	CHECK(ui::accentHueOffset() == 0.0);
	CHECK(ui::chromeHueOffset() == 20.0);
	// AND THE CHROME STRIP'S DRAG DOES NOT MOVE THE ACCENT.
	(void)ui::setAccentHueOffset(15.0);
	const visage::Bounds hue = screen.hueStripBounds();
	down.position = { screen.hueX(-60.0), hue.y() + hue.height() * 0.5f };
	screen.mouseDown(down);
	drag.position = { screen.hueX(75.0), hue.y() + hue.height() * 0.5f };
	screen.mouseDrag(drag);
	screen.mouseUp(drag);
	CHECK(ui::chromeHueOffset() == 75.0);
	CHECK(ui::accentHueOffset() == 15.0);
	// THE ACCENT STRIP SITS BELOW THE CHROME STRIP'S END LABELS, inside the panel at the smallest size.
	for (const auto& [w, h] : { std::pair { 1240.0f, 800.0f }, std::pair { 720.0f, 466.0f }, std::pair { 600.0f, 400.0f } }) {
		screen.setBounds(0.0f, 0.0f, w, h);
		CHECK(screen.accentStripBounds().y() >= screen.hueStripBounds().bottom() + 14.0f);
		CHECK(screen.accentResetBounds().bottom() <= screen.panelBounds().bottom());
	}
}

int main(int argc, char** argv) {
	// A SCRATCH DESK BEFORE ANY CASE, as the widget suites' main does: no case here may write the
	// developer's real editor.conf.
	const std::filesystem::path desk = std::filesystem::temp_directory_path()
	                                   / ("feathers-test-config-" + std::to_string(static_cast<long>(::getpid())));
	std::error_code code;
	std::filesystem::create_directories(desk, code);
	::setenv("XDG_CONFIG_HOME", desk.c_str(), 1);
	doctest::Context context(argc, argv);
	const int result = context.run();
	std::filesystem::remove_all(desk, code);
	return result;
}

TEST_CASE("about plug: the SoundCloud link sits in the About column and a click opens exactly that URL") {
	ui::SettingsScreen screen;
	screen.uiScaleNow = [] { return 1.0; };
	std::vector<std::string> opened;
	// The seam records what the real opener would be asked; the default opener is never called here.
	screen.onOpenLink = [&](const char* url) { opened.emplace_back(url); };
	CHECK(std::string(ui::kPlugUrl) == "https://soundcloud.com/greatowlmusic");
	for (const float h : { 800.0f, 400.0f }) {
		screen.setBounds(0.0f, 0.0f, 1240.0f, h);
		const visage::Bounds link = screen.plugLinkBounds();
		const visage::Bounds panel = screen.panelBounds();
		const float split = panel.x() + panel.width() * 0.5f; // the About part is the right of the rule
		CHECK(link.width() > 40.0f);
		CHECK(link.x() > split);
		CHECK(link.right() <= panel.right());
		CHECK(link.y() >= panel.y());
		CHECK(link.bottom() <= panel.bottom());
	}
	screen.setBounds(0.0f, 0.0f, 1240.0f, 800.0f);
	const visage::Bounds link = screen.plugLinkBounds();
	visage::MouseEvent miss;
	miss.position = { link.x() + 2.0f, link.bottom() + 10.0f };
	screen.mouseDown(miss);
	CHECK(opened.empty());
	visage::MouseEvent hit;
	hit.position = { link.x() + link.width() * 0.5f, link.y() + link.height() * 0.5f };
	screen.mouseDown(hit);
	REQUIRE(opened.size() == 1);
	CHECK(opened[0] == "https://soundcloud.com/greatowlmusic");
}

TEST_CASE("about plug: the opener refuses any URL but the plug, so it cannot become a launcher") {
	CHECK_FALSE(ui::openInBrowser(nullptr));
	CHECK_FALSE(ui::openInBrowser("file:///etc/passwd"));
	CHECK_FALSE(ui::openInBrowser("https://soundcloud.com/greatowlmusic; rm -rf ~"));
}
