/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */
#pragma once

#include "plugin/ui/value_box.h"

#include "device/modulatable.h"

#include "visage_ui/frame.h"

#include <algorithm>

namespace feathers::ui {

/// **GAMMA'S SEATING. A `ValueBox`, NEVER A `Knob`, AND CONFIGURED ONCE.**
///
/// DECIDED, on seeing the automod window: gamma is not modulatable where it has unlimited range, which
/// is why it is a value box in some places. Elsewhere it remains a modulatable knob with a finite
/// range — the new automod window has one.
///
/// **THIS CLASS EXISTS BECAUSE A CENTRALISED CONFIGURATION IS NOT AN ENFORCED ONE.** The two calls
/// below — compact, and the drag span — sat in `automod_card.cpp` and were correct there. The window
/// built three days later did not call them wrongly; it never looked for them, and seated gamma in a
/// plain `Knob` across the whole declared range. A free factory would not have prevented that either,
/// for the same reason: nothing makes a new surface go looking. **A named seating does, because it
/// makes the structural test writable** — *no surface may seat gamma in a `Knob`* — and that test is
/// the thing that would have caught it.
///
/// A SUBCLASS RATHER THAN A WRAPPER so it drops into any `cells_` vector that already takes a
/// `visage::Frame*`, and so the check above is one `dynamic_cast` rather than an ownership walk.
class GammaBox final : public ValueBox {
public:
	/// **HOW MUCH GAMMA A FULL DRAG IS WORTH — SIXTEEN LAPS, AND THE TOP IS AUTOMATION-ONLY.**
	///
	/// Gamma's drag rate was far too fast, and the standard is that scrolling up to the top should be
	/// virtually impossible. That describes the DRAG — no control in this
	/// editor has a wheel handler at all, so a wheel over this box scrolls the lane and never the
	/// value.
	///
	/// WITHOUT THIS A `ValueBox` DRAGS ITS CONTROL'S WHOLE DECLARED RANGE ACROSS `Knob::kDragRange`,
	/// which for gamma is 0..16,777,215 over 140px — **119,837 laps per pixel**, one drag crossing
	/// everything. `ZoneCoordBox` already makes this call for its fine half (one named zone per drag);
	/// gamma was the box nobody made it on, and then the box somebody drew as a knob instead.
	///
	/// AT SIXTEEN: 8.8px per lap, and shift (`kFineFactor` 0.2) gives 44px per lap for fine work.
	///
	/// **AND THE TOP IS THEN REACHABLE BY NO GESTURE. SAID OUT LOUD RATHER THAN LEFT TO BE
	/// DISCOVERED**, because a declared maximum no hand can reach is the honesty rule failing quietly.
	/// 0 to max is 1,048,576 full drags — about 147km of mouse travel — so the upper range belongs to
	/// automation, a patch, or a host, and not to a hand. `ValueBox` has exactly three gestures (press,
	/// drag, double-click-to-reset) and no text entry; adding it would serve every box in the editor
	/// and is filed rather than built.
	///
	/// **THE REASON THAT IS ACCEPTABLE IS THAT THE TOP IS A WALL, NOT A DESTINATION.** `kPhaseMax` is
	/// 2^24-1 because that is where the control DIES — at 2^24 one float ulp is 2, so a whole lap moves
	/// nothing — and it is already ragged from ~1e6, where one representable step moves the sound 1.323
	/// laps. The useful range is roughly 0..1e6 and the declared range is seven orders of magnitude
	/// wide, which is why no single linear drag serves both ends. This one serves the end a hand uses.
	static constexpr double kLapsPerDrag = 16.0;

	/// @param control the gamma parameter this box edits
	/// @param label   almost always empty — see below
	///
	/// **AN EMPTY LABEL IS THE DEFAULT AND THAT IS THE POINT, not an omission.** On the automod card
	/// the first version passed "Gamma" and the box drew its own caption inside its own rectangle,
	/// which left the SHARED label line — TYPE, FLAVOUR, MOD — with a hole at gamma's position and
	/// named the control twice in two different ways. That is the exact form knee shipped for
	/// Explore's tray and was then replaced. A surface whose row does NOT name its cells passes a label.
	/// HOW FAR THE BOX IS INSET INSIDE THE CELL A KNOB'S FACE WOULD FILL.
	static constexpr float kInset = 6.0f;

	/// **WHERE THE BOX SITS INSIDE A KNOB ROW'S CELL, AS ONE PIECE OF ARITHMETIC.**
	///
	/// A compact box is CENTRED IN THE SPACE A KNOB'S FACE WOULD OCCUPY, which is what puts its number
	/// on its neighbours' readout line and leaves the shared label line free for the row to write
	/// `GAMMA` on. A photograph once caught this being computed at the tray's arithmetic instead of
	/// its neighbours', which is the sort of difference only a capture shows — so it lives in one
	/// place now and both surfaces call it.
	///
	/// @param cell          the whole cell the row gave this column
	/// @param knobDiameter  the face diameter the neighbouring knobs use
	[[nodiscard]] static visage::Bounds cellBounds(visage::Bounds cell, float knobDiameter) {
		const float h = ValueBox::kCompactHeight;
		const float w = std::max(cell.width() - 2.0f * kInset, 0.0f);
		return { cell.x() + kInset, cell.y() + (knobDiameter - h) * 0.5f, w, h };
	}

	explicit GammaBox(device::Modulatable& control, std::string label = {})
	    : ValueBox(control, std::move(label)) {
		setCompact(true);
		// **SIXTEEN LAPS, OR THE WHOLE RANGE IF THE RANGE IS SMALLER THAN THAT.**
		//
		// The three gammas in this codebase are not declared in one unit: automod's is a 2^24 lapping
		// counter, the shaper's is 0..1, and Feathers' sound gamma is a +/-4 window over an unbounded
		// base. **Applying sixteen laps verbatim to a 0..1 parameter is not conservative, it is
		// WORSE than doing nothing** — a full drag would cross that range eight times, where today it
		// crosses it once.
		//
		// So the span is `min(sixteen laps, the declared range)`, which is the same SENTENCE at every
		// seating — *a drag is worth sixteen laps, unless there are fewer than sixteen laps to have* —
		// and reduces to each parameter's current behaviour where that is already right. The shaper's
		// gamma keeps exactly the drag it has today; automod's keeps its sixteen. **And when the
		// shaper's field becomes unbounded (DECIDED: the shaper's gamma conforms, since its field is
		// infinite too), it inherits the sixteen with no edit here or there.**
		//
		// This is the seating owning the MAPPING rather than hiding it, which is the honest shape when
		// the underlying units genuinely differ.
		const device::ParamDesc& d = control.desc();
		const double range = d.max - d.min;
		setDragSpan(range > 0.0 ? std::min(kLapsPerDrag, range) : kLapsPerDrag);
	}
};

} // namespace feathers::ui
