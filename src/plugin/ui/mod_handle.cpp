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

#include "plugin/ui/mod_handle.h"

#include "device/device.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {

/// THE KNOB'S OWN CONSTANTS (knob.h: `kRouteSeedAmount`, `kRouteDragRange`), restated rather than included so this
/// helper does not pull the knob's header; a case pins them equal to the knob's.
constexpr float kSeedAmount = 0.4f;
constexpr float kDragRange = 70.0f;
/// A THOUSANDTH OF THE RANGE: the knob's own threshold for "the value is off its base".
constexpr double kMovedBy = 0.001;

constexpr float kStrokeWidth = 3.0f;
constexpr float kDotRadius = 3.2f;

bool altHeld(const device::Modulatable& control) {
	const ModState* state = routingStateOf(control);
	return state != nullptr && state->bypassed();
}

} // namespace

ModAxisPlan ModHandle::planAxis(const ModHandleAxis& axis) const {
	ModAxisPlan plan;
	device::Modulatable* control = axis.control;
	if (control == nullptr || !axis.coordOf || control->unavailable() != nullptr) {
		return plan;
	}
	const device::ModDisplay state = control->display();
	if (!state.isDestination) {
		return plan; // THE HONESTY RULE: no destination, no halo, no stroke, no dot
	}
	const device::ParamDesc& desc = control->desc();
	plan.offered = true;
	plan.base = axis.coordOf(control->value());
	plan.halo = state.routable && !altHeld(*control);
	plan.glow = state.armed ? 1.0f : 0.8f;

	// THE AMOUNT STROKE: base to base + amount (both ways for a bipolar source), in the control's own units, clamped
	// to its range, through the plot's own mapping.
	if (state.amount != 0.0f) {
		const double span = device::routeSpanOf(desc);
		const double base = control->value();
		const auto edge = [&](float k) {
			const double v = std::clamp(base + static_cast<double>(k) * static_cast<double>(state.amount) * span, desc.min, desc.max);
			return axis.coordOf(v);
		};
		const float a = edge(state.reachHi());
		const float b = edge(state.reachLo());
		plan.strokeLo = std::min(a, b);
		plan.strokeHi = std::max(a, b);
		plan.hasStroke = plan.strokeHi > plan.strokeLo;
	}

	// THE DOT: where modulation has put the value, whenever it differs from the base — in both states.
	const double base = control->normalized();
	const double live = control->normalizedModulated();
	const auto live2 = [&](double raw) { return axis.coordOf(std::clamp(raw, desc.min, desc.max)); };
	if (std::abs(live - base) > kMovedBy) {
		plan.hasDot = true;
		plan.dot = live2(state.modulated);
	}
	if (state.stereoLive) {
		const double liveR = control->normalizedModulatedR();
		if (std::abs(live - base) > kMovedBy || std::abs(liveR - base) > kMovedBy) {
			plan.stereo = true;
			plan.hasDot = true;
			plan.dot = live2(state.modulated);
			plan.hasDotR = true;
			plan.dotR = live2(state.modulatedR);
		}
	}
	return plan;
}

ModHandlePlan ModHandle::plan() const {
	ModHandlePlan out;
	out.axes.reserve(axes_.size());
	for (const ModHandleAxis& axis : axes_) {
		out.axes.push_back(planAxis(axis));
		out.halo = out.halo || out.axes.back().halo;
	}
	return out;
}

void ModHandle::drawUnder(visage::Canvas& canvas, float x, float y, float radius) const {
	const ModHandlePlan p = plan();
	if (p.halo) {
		bool stereo = false;
		bool dim = false;
		for (size_t i = 0; i < axes_.size(); ++i) {
			if (p.axes[i].halo) {
				stereo = stereo || axes_[i].control->display().stereo;
			}
		}
		const uint32_t tint = stereo ? kModulationStereo : kModulation;
		const float halo = radius * 3.2f;
		canvas.setColor(visage::Brush::radial(
		    visage::Gradient(visage::Color(withAlpha(tint, dim ? 0.28f : 0.55f)), visage::Color(withAlpha(tint, 0.0f))),
		    { x, y }, halo));
		canvas.circle(x - halo, y - halo, halo * 2.0f);
	}
	for (size_t i = 0; i < axes_.size(); ++i) {
		const ModAxisPlan& a = p.axes[i];
		if (!a.hasStroke) {
			continue;
		}
		const auto line = [&](float width, uint32_t colour) {
			canvas.setColor(colour);
			if (axes_[i].horizontal) {
				canvas.segment(a.strokeLo, y, a.strokeHi, y, width, true);
			}
			else {
				canvas.segment(x, a.strokeLo, x, a.strokeHi, width, true);
			}
		};
		line(kStrokeWidth + 1.8f, withAlpha(kVoid, 0.85f)); // THE KEYLINE
		line(kStrokeWidth, withAlpha(kModulation, std::min(1.0f, a.glow)));
	}
}

void ModHandle::drawOver(visage::Canvas& canvas, float x, float y, float /*radius*/) const {
	const ModHandlePlan p = plan();
	const auto dotAt = [&](size_t i, float along, uint32_t colour, uint32_t core) {
		const float cx = axes_[i].horizontal ? along : x;
		const float cy = axes_[i].horizontal ? y : along;
		const float key = kDotRadius + 1.3f;
		canvas.setColor(withAlpha(kVoid, 0.92f));
		canvas.circle(cx - key, cy - key, key * 2.0f);
		canvas.setColor(colour);
		canvas.circle(cx - kDotRadius, cy - kDotRadius, kDotRadius * 2.0f);
		if (core != 0) {
			const float c = kDotRadius * 0.5f;
			canvas.setColor(core);
			canvas.circle(cx - c, cy - c, c * 2.0f);
		}
	};
	for (size_t i = 0; i < axes_.size(); ++i) {
		const ModAxisPlan& a = p.axes[i];
		if (a.hasDot) {
			dotAt(i, a.dot, a.stereo ? kModulationL : kModulatedDot, a.stereo ? kModulatedDot : 0u);
		}
		if (a.hasDotR) {
			dotAt(i, a.dotR, kModulationR, kModulatedDot);
		}
	}
}

size_t ModHandle::nearest(std::span<const visage::Point> centres, visage::Point at, float drawnRadius) {
	const float reach = hitRadius(drawnRadius);
	size_t best = npos;
	float bestDistance = reach * reach;
	for (size_t i = 0; i < centres.size(); ++i) {
		const float dx = at.x - centres[i].x;
		const float dy = at.y - centres[i].y;
		const float d2 = dx * dx + dy * dy;
		if (d2 <= bestDistance && (best == npos || d2 < bestDistance)) {
			best = i;
			bestDistance = d2;
		}
	}
	return best;
}

size_t ModHandle::routeAxisFor(bool altDown, visage::Point at, float x, float y) const {
	size_t best = npos;
	float bestDistance = 0.0f;
	for (size_t i = 0; i < axes_.size(); ++i) {
		const device::Modulatable* c = axes_[i].control;
		if (altDown || c == nullptr || c->unavailable() != nullptr || !c->display().routable) {
			continue;
		}
		const float d = axes_[i].horizontal ? std::abs(at.y - y) : std::abs(at.x - x);
		if (best == npos || d < bestDistance) {
			best = i;
			bestDistance = d;
		}
	}
	return best;
}

void ModHandle::beginRoute(size_t axisIndex, const visage::MouseEvent& e) {
	if (axisIndex >= axes_.size() || axes_[axisIndex].control == nullptr) {
		return;
	}
	device::Modulatable& control = *axes_[axisIndex].control;
	routeAxis_ = axisIndex;
	routeAnchorY_ = e.position.y;
	const float before = control.display().amount;
	routeAnchorAmount_ = before != 0.0f ? before : kSeedAmount;
	// THE KNOB'S OWN SEQUENCE: open the amount gesture, commit the seed so the stroke exists before the hand moves.
	control.beginAmountGesture();
	control.setAmount(routeAnchorAmount_);
}

void ModHandle::dragRoute(const visage::MouseEvent& e) {
	if (routeAxis_ == npos) {
		return;
	}
	const float travel = (routeAnchorY_ - e.position.y) / kDragRange;
	axes_[routeAxis_].control->setAmount(
	    std::clamp(routeAnchorAmount_ + travel, -device::kMaxAmount, device::kMaxAmount));
}

bool ModHandle::endRoute() {
	if (routeAxis_ == npos) {
		return false;
	}
	device::Modulatable& control = *axes_[routeAxis_].control;
	const float amount = control.display().amount;
	if (std::abs(amount) < device::kAmountUnroutedBelow && amount != 0.0f) {
		control.setAmount(0.0f); // dragged back to nothing is not a route at 0%, it is gone
	}
	control.endAmountGesture();
	routeAxis_ = npos;
	return true;
}

bool ModHandle::showMenu(visage::Frame& anchor, size_t preferredAxis) const {
	for (size_t k = 0; k < axes_.size(); ++k) {
		const size_t i = (preferredAxis + k) % axes_.size();
		if (axes_[i].control != nullptr && !inboundSources(*axes_[i].control).empty()) {
			return showRouteMenu(anchor, *axes_[i].control);
		}
	}
	return false;
}

std::vector<device::Modulatable*> ModHandle::controls() const {
	std::vector<device::Modulatable*> out;
	for (const ModHandleAxis& axis : axes_) {
		if (axis.control != nullptr) {
			out.push_back(axis.control);
		}
	}
	return out;
}

} // namespace feathers::ui
