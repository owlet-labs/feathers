/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/control_face.h"

#include <visage_ui/frame.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// HOVER TOOLTIPS FOR EVERY DEVICE CONTROL, drawn by the CARD the control sits on.
//
// Starting from COMP's "SC HPF" box: every control, everywhere, gets a hover tooltip. Every control
// already DECLARED its text (`ParamDesc::tooltip`,
// guarded by `test_device_tooltips.cpp`); nothing displayed it except OWLTT's graph.
//
// **THE CARD DRAWS IT, NEVER THE WINDOW** — a standing requirement, noted in `owltt_graph.cpp`: tooltips
// are not coupled to the Feathers plugin, so they still work when the effects run standalone. So the
// layer is a child of the card (or of an expanded view), and a card hosted
// anywhere — the lane, the modulator strip, the rack editor, the chain column, a standalone effect —
// carries its tooltips with it. The price is stated rather than hidden: a tooltip cannot leave its
// card, so on a narrow card a long one WRAPS to more lines rather than growing wider.
//
// THREE PIECES:
//   - `TooltipSource` — a widget that can say what the point under the pointer means. Every widget
//     bound to a `device::Modulatable` answers from `desc().tooltip`.
//   - `installTooltipHover` — the hooks that carry a widget's hover to the nearest host. visage hands
//     enter/move/exit ONLY to the frame under the pointer, and the one way an ancestor hears them
//     (`setReceiveChildMouseEvents`) delivers the child's coordinates to the card's own press
//     handlers — `fx_card.h` records why that is unsafe. So the widget forwards its own events.
//   - `TooltipLayer` — the host's on-top child: the delay, the state, the wrapped lines, the box.

namespace feathers::ui {

class TooltipLayer;

/// A FRAME THAT CAN SAY WHAT THE POINT UNDER THE POINTER MEANS. `local` is in the frame's own
/// coordinates. Null (or empty) is "nothing here" and shows no tooltip.
///
/// **AND IT SAYS WHICH CONTROLS IT OFFERS** (`ControlFace`): a hoverable widget is a widget that speaks for a
/// control, so the one interface carries both, and a new widget class cannot reach a card without answering the
/// window-superset guard's question about it (it does not compile until it does).
/// **A LABEL AS IT IS ON THE GLASS, AND THE NAME IT STANDS FOR** (hovering over a macro or knob name shows the full
/// parameter name whenever it is abbreviated or truncated). What a
/// widget answers `labelFaceAt` with; `fullNameToShow` decides from it.
struct LabelFace {
	/// The caption the widget was GIVEN, before any fit — "Len", "M1", a macro's typed name. Case as written.
	std::string label;
	/// The name it stands for — the control's declared name, or the user's own name for a macro.
	std::string full;
	/// **THE DRAWN TEXT DID NOT HOLD THE WHOLE LABEL** — cut with "...", emptied, or clipped at its box. Decided by
	/// the widget from what it draws NOW (its width, its scale, the compact layout), never remembered.
	bool cut = false;
};

/// **WHETHER `label` IS A SHORTENING OF `full`** — not the same name (ignoring case, spaces and punctuation), its
/// letters and digits appear in `full` in order and are fewer, and it is not made only of WHOLE words of `full`:
/// "Len" of "Length", "Oct" of "Octaves", "M1" of "Macro 1", "A" of "Env Attack" are; "Room" of "Featherverb Room"
/// (a device's own prefix dropped, nothing shortened) and "Bandwidth" of "Filter Macro 1" (a different name) are not.
[[nodiscard]] bool abbreviates(std::string_view label, std::string_view full);
/// **THE FULL NAME A HOVER SHOULD SAY, OR EMPTY.** `full` when the label abbreviates it; else the label itself when it
/// was cut; else nothing — a label shown whole and unabbreviated says nothing new.
[[nodiscard]] std::string fullNameToShow(const LabelFace& face);
/// **`tip` WITH `full` AT ITS START** — "Humanize Amount — How much of the feel". `tip` unchanged when `full` is
/// empty or `tip` already starts with it (ignoring case); `full` alone when there is no tip. The joint is not a
/// sentence end, so Brief's first sentence (`briefTooltip`) always carries the name.
[[nodiscard]] std::string withFullName(std::string_view full, const char* tip);

/// A FRAME THAT CAN SAY WHAT THE POINT UNDER THE POINTER MEANS. `local` is in the frame's own
/// coordinates. Null (or empty) is "nothing here" and shows no tooltip.
///
/// **AND IT SAYS WHICH CONTROLS IT OFFERS** (`ControlFace`): a hoverable widget is a widget that speaks for a
/// control, so the one interface carries both, and a new widget class cannot reach a card without answering the
/// window-superset guard's question about it (it does not compile until it does).
class TooltipSource : public ControlFace {
public:
	virtual ~TooltipSource() = default;
	[[nodiscard]] virtual const char* tooltipAt(visage::Point local) const = 0;
	/// **THE LABEL UNDER THE POINTER, IF THERE IS ONE** — see `LabelFace`. Empty for a widget that prints no name
	/// there. The hover road (`hoverText`) puts `fullNameToShow` of it at the start of `tooltipAt`'s text, so a widget
	/// that answers this needs nothing else to show a shortened label's full name.
	[[nodiscard]] virtual std::optional<LabelFace> labelFaceAt(visage::Point) const { return std::nullopt; }
};

/// **WHAT A HOVER AT `local` SHOWS** — `tooltipAt`, with the full name of a shortened label at its start
/// (`withFullName(fullNameToShow(labelFaceAt))`). The one composition the hover road and every case read.
[[nodiscard]] std::string hoverText(const TooltipSource& source, visage::Point local);

/// A FRAME THAT OWNS A `TooltipLayer` — `FxCard` and `ExpandedView`. A widget's hover goes to the
/// NEAREST one up its parent chain, found at the moment of the event and never remembered: an address
/// is only an identity while the thing at it cannot be replaced, and cards are rebuilt all the time.
class TooltipHost {
public:
	virtual ~TooltipHost() = default;
	[[nodiscard]] virtual TooltipLayer& tooltipLayer() = 0;
};

/// The nearest `TooltipHost` at or above `frame` (inclusive), and the frame that is it; null if none.
[[nodiscard]] TooltipHost* tooltipHostOf(visage::Frame& frame, visage::Frame** hostFrame = nullptr);

/// A CONTROL'S TOOLTIP — its declared text, or its name when it declares none (device.h's fallback).
[[nodiscard]] const char* tooltipText(const char* tooltip, const char* name);
/// The same, asked of a bound control; null for no control (a bare knob track says nothing).
[[nodiscard]] const char* tooltipOf(const device::Modulatable* control);

// HOW MUCH OF A TOOLTIP IS SHOWN: up to the first '.', with a hook for a later settings-page option.
// The declarations stay whole — a control's
// text is written once and says everything — and the cut is made where the text is RESOLVED FOR
// DISPLAY (`displayedTooltip`), so every control, whatever widget it is, gets it from one place.
//
// THE SETTING IS PROCESS-WIDE, like the chosen preset root: a card has no prefs and must not reach
// for any (the standalone-effect rule at the top of this file), so the editors that own prefs keep it
// there — `tooltip_prefs.h` — and everything else just asks `tooltipDetail()`. Read at the moment a
// tooltip is SHOWN, so a change takes effect on the next hover with no restart. MESSAGE THREAD.
enum class TooltipDetail { Brief, Full };

/// The current detail. **FULL until something says otherwise** — the default is verbose. Brief is the opt-in.
[[nodiscard]] TooltipDetail tooltipDetail() noexcept;
/// **THE HOOK A SETTINGS PAGE BINDS** (there is none yet). Changes the detail for the whole process and
/// tells every `TooltipDetailWatch` alive — the editors' prefs — so it is remembered. Returns how many
/// watches it told; none when the detail was already `detail`.
size_t setTooltipDetail(TooltipDetail detail);
/// How many `TooltipDetailWatch`es are alive — one per open editor; a lifetime test's count.
size_t tooltipDetailWatchCount() noexcept;

/// **THE FIRST SENTENCE** — `text` up to and including the first "." that ends a sentence, with
/// trailing whitespace trimmed; the whole text (trimmed) when no "." does. A "." ends a sentence when it
/// is followed by whitespace or the end of the text — so the point in "0.5 dB" or "44.1 kHz" never
/// does — and is not the last dot of "..." nor the end of one of a short list of abbreviations
/// (`e.g.`, `i.e.`, `etc.`, `vs.`, `cf.`, `approx.`; any case). "etc." ending a real sentence is
/// therefore read through to the next one: a longer brief, never a sentence cut in half.
[[nodiscard]] std::string briefTooltip(std::string_view text);
/// `text` as the current `tooltipDetail()` shows it: `briefTooltip` when Brief, as declared when Full.
[[nodiscard]] std::string displayedTooltip(std::string_view text);

/// **WHO KEEPS THE DETAIL** — an editor that owns prefs holds one for its lifetime, and every
/// `setTooltipDetail` tells every watch alive, so two editors in one process never hold different
/// copies of the key and write the stale one back. Registered for exactly its own lifetime. MESSAGE
/// THREAD. The prefs half is `keepTooltipDetailIn` in `tooltip_prefs.h`.
class TooltipDetailWatch {
public:
	explicit TooltipDetailWatch(std::function<void(TooltipDetail)> changed);
	~TooltipDetailWatch();
	TooltipDetailWatch(const TooltipDetailWatch&) = delete;
	TooltipDetailWatch& operator=(const TooltipDetailWatch&) = delete;

private:
	friend size_t setTooltipDetail(TooltipDetail detail);
	std::function<void(TooltipDetail)> changed_;
};

/// **HOOK `frame`'s HOVER TO THE NEAREST HOST.** Adds (`+=`) to enter, move, exit, down and drag (a
/// release needs nothing: a press suppresses until the pointer LEAVES, not until it lets go).
///
/// It also gives the widget a LIVENESS TOKEN (`TooltipAnchor`), owned by these hooks and so by the
/// frame's own callback lists: the layer keeps only a `weak_ptr` to it, which expires the moment the
/// frame is destroyed. That is how a hovered widget that goes away WITHOUT an exit (visage drops a
/// removed hovered frame silently) takes its tooltip with it — asked of a lifetime, never of an address.
///
/// **THE HAZARD, AND IT IS THE REASON THIS IS A CALL AND NOT A MEMBER: CALL IT LAST.** visage's
/// `onMouseDown() = ...` REPLACES the whole callback list — the widget's virtual handler and every
/// hook added before it. ValueBox, SwitchChip, HarmonicRow and most of this editor assign their
/// handlers with `=` in their constructor bodies, so a hook installed any earlier is silently gone,
/// and so is one installed by this call if ANYONE assigns with `=` afterwards — a card re-binding a
/// widget's press after construction included. Nothing at the call site can see that happen. The
/// coverage sweep in `tests/gui_widgets/test_gui_widgets_tooltips.cpp` ("every hoverable control on
/// every card shows its tooltip, and a press hides it", and its twins for expanded views, the chain
/// column and the rack editor) hovers and presses every leaf of every HOST-BUILT card and panel and is
/// the guard; an `=` added after this call turns that case red, naming the widget.
void installTooltipHover(visage::Frame& frame, const TooltipSource& source);

/// THE HOVERED WIDGET'S LIVENESS — see `installTooltipHover`. The frame pointer is read ONLY through a
/// locked `weak_ptr`, and the token lives exactly as long as the frame's callback lists do, so a lock
/// that succeeds is a frame that still exists.
struct TooltipAnchor {
	visage::Frame* frame = nullptr;
};

/// THE HOST'S LAYER: an on-top child covering the whole host, ignoring the mouse, invisible until a
/// hover has rested for `kDelayMs`.
///
/// **IT HOLDS COPIES, NEVER POINTERS** — the text, and the point in host coordinates; the hovered
/// widget only as a `weak_ptr` to its liveness token. It forgets everything when:
///   - the hovered widget is destroyed, removed from under the host, or hidden (checked every
///     `advance()`, because visage delivers no exit for any of those);
///   - the host is re-laid-out, hidden, removed or re-added, or changes scale (the host's hooks,
///     `watch()`).
class TooltipLayer final : public visage::Frame {
public:
	/// The rest before a tooltip appears — about what every desktop uses.
	static constexpr double kDelayMs = 500.0;
	/// The widest a box may be before it wraps (and never wider than its host allows).
	static constexpr float kMaxWidth = 200.0f;
	static constexpr float kPadX = 5.0f;
	static constexpr float kPadY = 3.0f;
	/// The gap between the pointer and the box's near edge.
	static constexpr float kGap = 8.0f;
	/// The margin kept between the box and the host's edge.
	static constexpr float kMargin = 2.0f;

	enum class State { Idle, Pending, Shown, Suppressed };

	TooltipLayer();

	/// THE HOVER ARRIVED, OR MOVED. `at` is in the HOST's coordinates. A change of text (a new widget,
	/// or a new region of one) restarts the delay; the same text moving only moves the anchor.
	/// A hover while `Suppressed` stays suppressed until `leave()` — a press is not undone by a wiggle.
	/// `from` is the hovered widget's token (empty for a hover no widget made — a case's direct call).
	void hover(visage::Point at, const char* text, std::weak_ptr<TooltipAnchor> from = {});
	/// The pointer left whatever was being described. Hides at once and re-arms. Returns whether it hid
	/// a shown box — i.e. whether it asked for a repaint, as `advance()` reports.
	bool leave();
	/// A press (or a drag). Hides at once and stays hidden until `leave()`. Returns whether it hid.
	bool press();
	/// FORGET EVERYTHING — the host is being rebuilt, moved or re-scaled, so any anchor is stale.
	bool clear();

	/// **ADVANCE THE DELAY.** Called from the host's per-frame `update()`. Returns whether it asked for
	/// a repaint — a count the update road can report, per the repaint rule. Also where a hovered
	/// widget that has gone (see the class note) is noticed and its tooltip dropped.
	bool advance();

	/// **THE HOST'S HOOKS, ONCE** — cover on resize and on this layer's DPI change, and clear on every
	/// change that makes an anchor stale: resize, DPI, the host's visibility, and the host leaving or
	/// joining a tree. Called by `FxCard` and `ExpandedView` in their constructors.
	void watch(visage::Frame& host);

	/// The clock, in milliseconds. Defaults to `steady_clock`; a case sets its own.
	void setClock(std::function<double()> clock) { clock_ = std::move(clock); }

	void draw(visage::Canvas& canvas) override;

	[[nodiscard]] State state() const noexcept { return state_; }
	[[nodiscard]] bool shown() const noexcept { return state_ == State::Shown; }
	[[nodiscard]] const std::string& text() const noexcept { return text_; }
	/// THE TEXT AS SHOWN — `displayedTooltip(text())`, resolved when the box appeared, so at the detail
	/// that was current then. Empty unless shown.
	[[nodiscard]] const std::string& displayed() const noexcept { return displayed_; }
	/// THE WRAPPED LINES, as they are drawn — computed by visage's own `Font::lineBreaks` at this
	/// frame's DPI scale, which is the same measurement its renderer uses. Empty unless shown.
	[[nodiscard]] const std::vector<std::string>& lines() const noexcept { return lines_; }
	/// THE BOX, in the host's coordinates. Empty unless shown.
	[[nodiscard]] visage::Bounds box() const noexcept { return box_; }
	/// The point the box was placed against, in the host's coordinates.
	[[nodiscard]] visage::Point anchor() const noexcept { return anchor_; }

	/// Keep this layer covering its host. Called on the host's resize and on this frame's DPI change —
	/// UNCONDITIONALLY, per the setBounds/DPI rule: a scale change moves nothing logical.
	void cover(const visage::Frame& host);

	/// How many times the text has been MEASURED (wrapped) — a move over a shown box only re-places it.
	[[nodiscard]] size_t measuresForTest() const noexcept { return measures_; }

private:
	/// Wrap `text_` at this frame's scale into `lines_` and the box's size. Once per text and scale.
	void measure();
	/// Put the measured box against `anchor_`, clamped inside the host. Cheap; every move.
	void place();
	/// Whether the widget that made this hover is still alive, still under the host, and drawn.
	[[nodiscard]] bool anchorAlive() const;

	std::function<double()> clock_;
	State state_ = State::Idle;
	std::string text_;
	std::string displayed_;
	std::weak_ptr<TooltipAnchor> from_;
	bool fromWidget_ = false;
	visage::Point anchor_ {};
	double since_ = 0.0;
	std::vector<std::string> lines_;
	visage::Point size_ {};
	visage::Bounds box_ {};
	size_t measures_ = 0;
};

} // namespace feathers::ui
