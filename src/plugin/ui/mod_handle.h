/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/modulatable.h"
#include "plugin/ui/theme.h"

#include <visage/graphics.h>
#include <visage/ui.h>

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

// **A PLOT HANDLE AS A FIRST-CLASS MODULATION TARGET — ONE HELPER FOR EVERY PLOT.**
//
// A draggable handle on a plot is direct manipulation with no modulation road of its own, which is why a knob so
// often sits beside one: the knob is what a source can be routed to. `ModHandle` gives a handle the same visual
// language a knob has (`drawModulationRing`, `drawModulationDot`) and the same press road, so the duplicate knob is
// no longer needed for modulation.
//
// WHAT THE PLOT SUPPLIES, AND ALL IT SUPPLIES: the `device::Modulatable` each axis edits, the map from that control's
// VALUE to a coordinate along the axis (the plot's own, which may be logarithmic — the helper never assumes linear),
// and the handle's drawn radius. WHAT THIS OWNS: the drawing plan (`ModHandlePlan`, a value so a case reads exactly
// what will be painted), the press and amount-drag road, the sources menu, and the hit rule.
//
// THE HANDLE ITSELF STAYS AT THE BASE: dragging it edits the base, exactly as dragging a knob does. The amount is a
// stroke from the base to base + amount along the axis; the modulated value is the dot, drawn last.

namespace feathers::ui {

/// One axis of travel: a control and where its values fall along the plot.
struct ModHandleAxis {
	device::Modulatable* control = nullptr;
	/// True when the axis runs along x (the stroke is horizontal), false for y.
	bool horizontal = true;
	/// A control VALUE (in the control's own units) to a coordinate along this axis, in the plot's frame.
	std::function<float(double)> coordOf;
};

/// What one axis will paint, in plot coordinates along its axis. Everything is absent for a control that is not a
/// modulation destination (the honesty rule), and `halo` is only the potential-destination state.
struct ModAxisPlan {
	bool offered = false;   ///< the control is a destination at all
	bool halo = false;      ///< a source is armed and would be accepted here, and Alt is not held
	bool hasStroke = false; ///< the amount stroke exists
	float strokeLo = 0.0f;  ///< ordered: strokeLo <= strokeHi
	float strokeHi = 0.0f;
	float glow = 0.0f;      ///< 1.0 while a source is armed, 0.8 otherwise (the knob's rule)
	bool hasDot = false;
	float dot = 0.0f;       ///< the live modulated position
	bool stereo = false;    ///< a stereo route is live: two dots, in the ear colours
	bool hasDotR = false;
	float dotR = 0.0f;
	float base = 0.0f;      ///< where the handle sits on this axis (the base value's coordinate)
};

struct ModHandlePlan {
	std::vector<ModAxisPlan> axes;
	/// Any axis lit: the halo around the handle.
	bool halo = false;
};

class ModHandle {
public:
	/// The grab radius never falls below this, however small the handle is drawn.
	static constexpr float kMinHitRadius = 8.0f;

	ModHandle() = default;
	explicit ModHandle(std::vector<ModHandleAxis> axes) : axes_(std::move(axes)) {}

	[[nodiscard]] size_t axisCount() const { return axes_.size(); }
	[[nodiscard]] const ModHandleAxis& axis(size_t i) const { return axes_[i]; }

	/// THE DRAWING PLAN, from the controls' own display — nothing here is painted anywhere it does not say.
	[[nodiscard]] ModHandlePlan plan() const;

	/// UNDER the handle: the halo, then each axis's amount stroke with its keyline. `x`,`y` is the handle's centre in
	/// the plot's frame; `radius` its drawn radius.
	void drawUnder(visage::Canvas& canvas, float x, float y, float radius) const;
	/// OVER the handle, last: each axis's dot with its keyline (two per stereo-live axis, in the ear colours).
	void drawOver(visage::Canvas& canvas, float x, float y, float radius) const;

	/// THE GRAB RADIUS for a handle drawn at `drawnRadius`.
	[[nodiscard]] static float hitRadius(float drawnRadius) { return std::max(kMinHitRadius, drawnRadius); }
	/// WHICH OF SEVERAL HANDLES A PRESS TAKES: the one whose centre is nearest, within its grab radius; `npos` when none.
	/// With two overlapping, the nearer centre wins — never the first or the last drawn.
	static constexpr size_t npos = static_cast<size_t>(-1);
	[[nodiscard]] static size_t nearest(std::span<const visage::Point> centres, visage::Point at, float drawnRadius);

	/// THE PRESS A KNOB TAKES WHILE ARMED: would a press route here (a source armed, allowed to reach an axis, Alt not
	/// held)? Returns the axis it would route, or `npos`. With two axes lit, the one whose stroke the press is nearer
	/// (given the handle's centre `x`,`y` and press `at`) — the first on a tie.
	[[nodiscard]] size_t routeAxisFor(bool altDown, visage::Point at, float x, float y) const;

	/// BEGIN THE AMOUNT GESTURE on `axisIndex`: the same road a knob's armed press takes — the control's own
	/// `beginAmountGesture` and a seeded `setAmount`, so one undo entry. The caller starts no value drag.
	void beginRoute(size_t axisIndex, const visage::MouseEvent& e);
	/// Vertical travel moves the amount, as on a knob. True while a route gesture is open.
	void dragRoute(const visage::MouseEvent& e);
	/// Closes the gesture (an amount dragged back to nothing removes the route). Returns whether one was open.
	bool endRoute();
	[[nodiscard]] bool routing() const { return routeAxis_ != npos; }

	/// RIGHT-CLICK: the same sources menu a modulated knob opens, on the axis whose control has a route (the first when
	/// several do). False when no axis has one, so the press falls through.
	bool showMenu(visage::Frame& anchor, size_t preferredAxis = 0) const;

	/// Every control this handle edits, for `ControlFace::offeredControls`.
	[[nodiscard]] std::vector<device::Modulatable*> controls() const;

private:
	[[nodiscard]] ModAxisPlan planAxis(const ModHandleAxis& axis) const;

	std::vector<ModHandleAxis> axes_;
	size_t routeAxis_ = npos;
	float routeAnchorY_ = 0.0f;
	float routeAnchorAmount_ = 0.0f;
};

} // namespace feathers::ui
