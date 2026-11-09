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

#include "plugin/ui/macro_menu.h"
#include "plugin/ui/tooltip.h"

#include <functional>
#include <string>
#include <visage/ui.h>

// **AN ENDLESS ENCODER — THE FACE OF A RELATIVE MACRO (backlog PQ).**
//
// A Relative macro has no value, so it has no position to draw: no arc, no end of travel, no reading but
// its name and the word "Relative". What the face shows is that it TURNS — a pointer that follows the hand
// round and round and wraps, purely as feedback (it is not stored and means nothing once the hand lets go).
// A vertical drag is a run of INCREMENTS in macro units, one unit per `kDragRange` pixels — the distance
// an absolute macro's knob takes for its whole sweep, so a route of the same depth moves its destination
// the same distance for the same hand movement. Shift is fine, as on a knob.
//
// WHAT AN INCREMENT DOES is the owner's (`Hooks`): Feathers' header row hands it to a
// `RelativeMacroGesture`, which edits every routed base as one undo step per drag. The right-click is the
// macro menu, so the type can be switched back.

namespace feathers::ui {

class EncoderFace : public visage::Frame, public TooltipSource {
public:
	struct Hooks {
		std::function<void()> begin;
		std::function<void(double)> turn;
		std::function<void()> end;
	};

	/// **WHAT THE ENCODER IS, ON HOVER** (the relative/encoder macro showed no tooltip) —
	/// it has no control of its own to declare a tooltip, so it says what a turn does.
	static constexpr const char* kTooltip =
	    "A relative macro: each turn nudges every destination it is routed to by that much, from wherever it is.";
	[[nodiscard]] const char* tooltipAt(visage::Point) const override { return kTooltip; }
	/// **THE CAPTION AND THE NAME IT STANDS FOR** (`ui::LabelFace`) — "M1" for "Macro 1", or a typed name cut to its
	/// column — as a knob's (`Knob::labelFaceAt`).
	[[nodiscard]] std::optional<LabelFace> labelFaceAt(visage::Point) const override;
	/// A turn writes no control — it edits the destinations' stored bases through the slot's relative hooks.
	void offeredControls(std::vector<Offered>&) const override {}
	/// The name a shortened caption stands for — the macro's effective name (`MacroSlot::refresh`).
	void setFullName(std::string name) { fullName_ = std::move(name); }

	/// `caption` is BORROWED (the slot owns the string and re-hands it on a rename).
	EncoderFace(const char* caption, Hooks hooks, MacroMenuHooks menu);

	void setCaption(const char* caption) {
		caption_ = caption;
		redraw();
	}
	void setFaceDiameter(float diameter) {
		faceDiameter_ = diameter;
		redraw();
	}
	[[nodiscard]] float faceDiameter() const noexcept { return faceDiameter_; }
	/// The pointer's visual travel, in macro units (wraps every unit). For a case.
	[[nodiscard]] double travel() const noexcept { return travel_; }
	[[nodiscard]] bool turning() const noexcept { return turning_; }

	/// Pixels of vertical drag per macro unit — `Knob::kDragRange`, a knob's whole sweep.
	static constexpr float kDragRange = 140.0f;
	static constexpr float kFineFactor = 0.2f;

	void draw(visage::Canvas& canvas) override;

private:
	const char* caption_;
	std::string fullName_;
	Hooks hooks_;
	MacroMenuHooks menu_;
	float faceDiameter_ = 0.0f;
	float lastY_ = 0.0f;
	bool turning_ = false;
	double travel_ = 0.0;
};

} // namespace feathers::ui
