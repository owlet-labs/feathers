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

#include <functional>
#include <string>
#include <utility>
#include <visage/ui.h>

// A PANEL THAT COLLAPSES TO A THIN LABELLED STRIP — the mock's `.min` rails, and the one mechanism
// behind all of them.
//
// THE MOCK'S OWN FORM, not an invention: `#modStrip.min{width:34px;padding:12px 5px}`, the cards
// hidden, the heading turned upright (`writing-mode:vertical-rl`), and the head stacked into a
// column with its button on top. The button is `◂` while the panel is open and `▸` once it is
// closed, mirrored on a rail that lives against the right edge, and the toggle announces itself
// through the readout the way the mock calls notify().
//
// A BASE RATHER THAN A BOOL IN EACH PANEL, because there are three of them: the Engine rail, the
// modulators, and the post-FX chain that does not exist yet. Written per-panel, the collapsed form
// is the same forty lines three times, and the third copy is where they start to disagree about how
// wide a closed rail is. A panel inherits this, says how wide it is when open, and hides its own
// contents when told; everything else — the width the editor lays out against, the toggle's
// rectangle, the upright label, the glyph — is here once.
//
// MINIMISED, NEVER GONE. This replaces an auto-hide: if the macros are hidden when the window is small,
// they go to a minimised state, never away entirely. A rail that
// vanishes takes its own control away with it and leaves nothing on screen to say where it went; a
// rail that closes to 34px is still there, still labelled, and one click from being back.
//
// EDITOR-LOCAL AND NOWHERE ELSE. The state is this bool, and this bool lives in the panel, which
// lives in the editor, which is built once and kept for the life of the process (see the Mesa
// swapchain note in Gui::create). So a rail you closed is still closed when the host reopens the
// editor, and NOTHING about it reaches the patch: which panels you like open is a fact about your
// desk, not about the sound, and a preset that carried it would rearrange someone else's editor.

namespace feathers::ui {

class Rail : public visage::Frame {
public:
	/// Which edge the rail lives against, which is the only thing that differs between the left rails
	/// and the post-FX one: the glyph points the way the panel will go.
	enum class Side : uint8_t { Left, Right };

	bool collapsed() const noexcept { return collapsed_; }

	/// Returns whether this changed anything, so a caller can announce a real toggle and stay quiet
	/// about a redundant one.
	bool setCollapsed(bool collapsed);
	bool toggleCollapsed() { return setCollapsed(!collapsed_); }

	/// WHAT THE EDITOR LAYS OUT AGAINST, and the reason the collapse needs no second sum anywhere:
	/// gui.cpp adds these up to find where the rails end and the stage begins, so a rail that closes
	/// gives its width to the stage without anyone else being told.
	float railWidth() const { return collapsed_ ? kCollapsedWidth : expandedWidth(); }

	/// How wide the panel is with its contents showing. The one thing a rail has to say for itself.
	///
	/// **IT MUST BE A PURE FUNCTION OF DEVICE/PATCH STATE AND CONSTANTS — NEVER OF ANY FRAME'S
	/// MEASURED SIZE, ITS OWN INCLUDED.** `gui.cpp` calls `railWidth()` *while deciding the rail's
	/// bounds* (`Engine rail->setBounds(railEdge, …, Engine rail->railWidth(), …)`), so on the first
	/// layout nothing under this rail has been placed. A width derived from a child would be zero
	/// there and correct on the next pass, which presents as a panel at the wrong width until the
	/// window is dragged — it self-heals, which is what makes it hard to catch.
	///
	/// All five implementations `return kWidth;`, which is why the ordering has never mattered and why
	/// nothing has had to say this before. **PINNED BY** *"INVARIANT, over every Rail subclass:
	/// `expandedWidth()` is the same before the rail has been laid out and after"* in
	/// `tests/gui_widgets/test_gui_widgets_devices.cpp` — which walks the five by hand, so what it
	/// enforces is that no EXISTING one starts measuring, not that a sixth was added to its list.
	virtual float expandedWidth() const = 0;

	/// The toggle's rectangle in the rail's own coordinates — REAL IN BOTH STATES, because the way
	/// back from a closed rail is the same button that closed it.
	visage::Bounds toggleBounds() const;
	bool toggleHit(visage::Point pixels) const { return toggleBounds().contains(pixels); }

	/// TOLD, NOT ASSUMED. A rail cannot resize itself — its width is one term in an arithmetic that
	/// belongs to the editor — so it announces the change and the editor re-derives the columns. This
	/// is the whole of the coupling between the two, and it is what keeps a single place deciding
	/// where the rails end and the stage begins.
	void onRailWidthChanged(std::function<void()> callback) {
		widthChanged_ = std::move(callback);
	}

	/// The mock's `.min` width, and its `padding:12px 5px`.
	static constexpr float kCollapsedWidth = 34.0f;
	static constexpr float kCollapsedPadX = 5.0f;
	static constexpr float kPadTop = 12.0f;
	/// The mock's `.iconbtn`, square and small enough to sit in a 34px rail with its padding.
	static constexpr float kToggleSize = 20.0f;
	/// The chevron's stroke. OPEN STROKES rather than a filled arrowhead are the whole point of the
	/// mark — see drawRailChrome: a filled arrowhead read as overloaded beside modulators and solo.
	static constexpr float kChevronWeight = 1.6f;
	/// THE CLOSED RAIL'S LABEL SIZE, 2.5x the knob caption the first render used (2.5-3x was the
	/// target). A closed rail says exactly one word and has the whole 34px
	/// column to say it in, so it is set at reading size rather than at the size of a knob's label.
	static constexpr float kCollapsedLabelSize = 20.0f;

	/// **THE SHORTEST A CLOSED RAIL MAY BE DRAWN AND STILL SAY WHAT IT IS**: the toggle, the gap
	/// `drawRailChrome` leaves under it (`toggle.bottom() + kPadTop`), and a column for the upright
	/// label.
	///
	/// **WHAT IT MUST HOLD: the LONGEST `railLabel()` ANY `Rail` SUBCLASS CARRIES, set upright at
	/// `kCollapsedLabelSize`.** Today those are `ENGINE`, `MODULATORS`, `POST-FX`, `PERFORM`,
	/// `FX LANES` and `CONTROLS`; the widest of them at 20px is what this number is for, and a rail
	/// whose label grows past it must grow this. Stating the job rather than the number is the point: a
	/// chosen constant with no written job is the one somebody rounds to 128 for tidiness.
	///
	/// **CHOSEN RATHER THAN DERIVED — `FxLanesRail::kWidth` IS THE PRECEDENT — BUT NOT MERELY
	/// PHOTOGRAPHED.** How long a word is at a size is a FONT measurement and this has to be a
	/// compile-time constant, so the arithmetic cannot be written here. What CAN be written is a case
	/// that asks the font at run time, which is `ZoneCoordCell::minimumSquareSide`'s arrangement one
	/// widget over: *"a closed rail's strip is tall enough for the longest label any rail carries"*
	/// walks all six live rails, measures each `railLabel()` through `font(kCollapsedLabelSize)`, and
	/// requires this to clear the widest with margin. The capture is the second reading, not the only
	/// one.
	///
	/// **BECAUSE THE FAILURE IS SILENT.** A strip shorter than its word clips it and says nothing —
	/// `Canvas::text` draws inside the box it is given — so there is no red anywhere to find it by, and
	/// no wrong pixel either: just a rail called `CONTROL`.
	///
	/// **IT BINDS ONLY WHEN NOTHING ELSE IS ASKING FOR HEIGHT.** A rail is as tall as whoever owns its
	/// column, so in the editor this is never reached: the stage decides. The one owner that can end up
	/// with no open content at all is a device panel whose BODY and whose rails are all closed, which is
	/// `GenericExpanded::minimumSize`'s collapsed answer — three strips and nothing else, a state that
	/// has to be legal rather than merely possible.
	/// **MEASURED, AND THE FIRST NUMBER WAS WRONG.** 130 was chosen by eye and the case above it went
	/// red on its first run: `MODULATORS` is **140px** at `kCollapsedLabelSize`, so the floor would have
	/// clipped the widest label this editor has. It never bound in the editor — the stage sets a rail's
	/// height there and only an all-folded device panel reaches this — which is precisely why it needed
	/// a case and not a look. 160 is that 140 with one label-size of air.
	static constexpr float kCollapsedLabelRun = 160.0f;

	/// **THE CHROME ABOVE THE LABEL, AS `drawRailChrome` ACTUALLY LAYS IT OUT**: the toggle at
	/// `kPadTop/2`, its own height, and the `kPadTop` gap the label starts after
	/// (`top = toggle.bottom() + kPadTop`). Written as that sum rather than as `kPadTop + kToggleSize +
	/// kPadTop`, which is what it said first and is 6px too generous — a floor that is accidentally
	/// larger than the thing it floors passes every check while meaning something else.
	static constexpr float kCollapsedChromeHeight = kPadTop * 0.5f + kToggleSize + kPadTop;
	static constexpr float kCollapsedMinimumHeight = kCollapsedChromeHeight + kCollapsedLabelRun;

protected:
	Rail(std::string label, Side side) noexcept : label_(std::move(label)), side_(side) {}

	/// The toggle, and — while the rail is closed — the upright label beside it. A panel calls this
	/// from its own draw and draws its contents only when it is open.
	void drawRailChrome(visage::Canvas& canvas);

	/// Hide what the rail holds, which by default is EVERY CHILD FRAME: a rail draws its own chrome
	/// rather than parenting it, so anything hanging off a rail is content by construction. A control
	/// left visible in a 34px strip would draw across the hairline and answer a click meant for the
	/// stage behind it.
	///
	/// A panel that grows chrome of its own one day can override; nothing needs to yet, and a panel
	/// that does not override cannot forget to hide a child it gains later.
	virtual void collapsedChanged();

	/// WHERE A HEADING MAY START without running under the toggle, given the panel's own left padding.
	/// On a left-hand rail the toggle is at the far end and the answer is just the padding; on a
	/// right-hand one they share an end, and a heading that ignored this is drawn straight through the
	/// chevron. The first render of the fx lane did exactly that — "P⟩ST-FX" — and no geometry case
	/// could see it, because nothing had asked whether two rectangles this class hands out overlap.
	///
	/// It lives here rather than in the lane because which end the toggle sits at is the BASE's
	/// decision, and the next right-hand rail should inherit the answer rather than rediscover it.
	float railHeadingLeft(float pad) const {
		return side_ == Side::Left ? pad : toggleBounds().right() + pad;
	}

	const std::string& railLabel() const noexcept { return label_; }

	/// **A CLOSED RAIL THAT SAYS ITS NAME ITSELF** (backlog OC: the Rack's chain strips are usable while collapsed, and
	/// draw their short label in the strip): false leaves the toggle and omits the upright label. True is the default.
	void setUprightLabel(bool shown) noexcept { uprightLabel_ = shown; }

public:
	/// **THE SAME WORD, PUBLIC, SO THE ONE CONSTANT THAT HAS TO HOLD IT CAN BE CHECKED AGAINST IT.**
	/// `kCollapsedLabelRun` is a chosen number whose job is "the longest label any rail carries", and a
	/// case can only enforce that by asking each rail what its label IS. Restating the six strings in a
	/// test would pin the test to itself rather than to the rails.
	[[nodiscard]] const std::string& labelForTest() const noexcept { return label_; }

	/// **A RAIL WHOSE WORD CHANGES WITH WHAT IT HOLDS** — the stage's, which is EXPLORE or PERFORM as
	/// the header's switch says (`StageRail`). Every other rail names itself once, in its constructor.
	/// `kCollapsedLabelRun` must still hold the new word; the case that pins it asks the live rails.
	void setRailLabel(std::string label) {
		if (label != label_) {
			label_ = std::move(label);
			redraw();
		}
	}

private:
	std::string label_;
	Side side_;
	bool collapsed_ = false;
	bool uprightLabel_ = true;
	std::function<void()> widthChanged_;
};

} // namespace feathers::ui
