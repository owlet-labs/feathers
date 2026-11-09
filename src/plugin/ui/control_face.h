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

#include "device/modulatable.h"

#include <vector>

// **WHICH CONTROLS A WIDGET OFFERS, ASKED OF THE WIDGET** (the rule: a device's window is strictly a
// superset of its card). The rule needs "what does this surface offer" for every card and every window, and a
// per-class list (`addressedParamsForTest`) has to be written for each card and each window and kept true by hand.
// This asks the WIDGETS instead, so a walk of any frame tree answers it, and a new card or window built from the
// existing widgets is covered without writing anything.
//
// TWO ROADS, because they are not the same offer:
//   - `Face` — the widget IS this control: its value is shown, a press or drag edits it, it can be armed for
//     modulation. A box, a chip, a segmented control, a pill's amount and power.
//   - `Gesture` — a picture that writes the control as a side road: a plot's axis drag. The card's comments say
//     why that is weaker (a count that leaned on the plot stayed whole with the box deleted), so the guard asks
//     that a card's FACE is a face in the window too, and lets a gesture be met by either.
//
// WHO MUST ANSWER. Every `TooltipSource` is one (it extends this), so a new hoverable widget class does not compile
// until it says what it offers. That covers most controls, NOT ALL: a frame that writes a control and speaks no
// tooltip of its own (the EQ's and OWLTT's graphs, HARM's scope, SKLITTER's perform pad, a drawn-card puck, a view's
// own hand-drawn menu) implements this directly, and a card that hand-draws a control overrides `FxCard`'s answer.
// **The rule: any frame that calls `beginGesture` / `setValue` on a device control declares it here**, or the guard
// cannot see that control. A widget over CORE parameters (the Engine rail's steppers) answers nothing.

namespace feathers::ui {

class ControlFace {
public:
	enum class Road { Face, Gesture };
	struct Offered {
		const device::Modulatable* control = nullptr;
		Road road = Road::Face;
	};
	virtual ~ControlFace() = default;
	/// Appends every device control this widget offers. Hidden or disabled widgets answer too: "offered" is
	/// what the surface holds, and a gate that closes it in this mode opens it in another.
	virtual void offeredControls(std::vector<Offered>& out) const = 0;

	/// **WHAT THIS WIDGET DRAWS FOR MODULATION ON `control`, ASKED OF THE WIDGET** (Pattern's Go chip
	/// did not glow). The glow-honesty rule — a face glows for an armed source iff a route can land on it, and shows a
	/// routed mark once one has — needs every face asked, as the superset guard asks every face what it offers. The
	/// paint's own inputs, since a repaint cannot be observed headlessly.
	struct ModLook {
		bool drawn = false;      ///< the widget draws modulation for this control at all (false: it never glows, never rings)
		bool armedGlow = false;  ///< lit NOW for the armed source
		bool routedMark = false; ///< a routed indication drawn NOW (a ring, a bar, an arc)
	};
	/// The default draws nothing: a face that should glow and does not is what the sweep catches
	/// (`test_gui_widgets_tooltips`, "every modulatable face ...").
	[[nodiscard]] virtual ModLook modLookFor(const device::Modulatable& control) const {
		(void)control;
		return {};
	}
};

} // namespace feathers::ui
