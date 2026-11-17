/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#include "device/modulatable.h"

#include "device/format_value.h"

#include <algorithm>
#include <cmath>

// THE ARITHMETIC EVERY MODULATED WIDGET DOES, WRITTEN ONCE.
//
// It is here rather than left to each implementor for the reason the interface exists at all: two
// widgets that computed their own ring geometry would drift, and the drift would be invisible —
// a bar on a value box reaching a little further than the arc on a knob beside it, for the same
// amount on the same kind of control. "Unified" has to mean the same numbers, not merely the same
// colour.

namespace feathers::device {

namespace {

/// A control with no range cannot be normalised, and a zero span would divide. Both are a
/// misdeclared descriptor rather than a runtime condition, so the answer is the bottom of the sweep
/// rather than a NaN walking into the paint.
///
/// **`device::toNormal` IS THE ARITHMETIC** (format_value.h), so the position a widget draws and the
/// one a host slot reports to the DAW are one function rather than two that agree today.
double positionIn(const ParamDesc& desc, double value) {
	return toNormal(desc, value);
}


/// A RANGE TOO WIDE TO DRAW ON: a coordinate that spans 2^20 units or more (OWLTT's Character is a zoned one, 2^24
/// wide). Unlike `isDragWindow` this does not ask for `zones == 0`, because a zoned coordinate is as unreadable
/// against its whole range as an unzoned one.
bool hasWindowScale(const ParamDesc& d) {
	return d.unit == Unit::Number && (d.max - d.min) >= 1048576.0;
}
} // namespace

double Modulatable::normalized() const {
	return positionIn(desc(), value());
}

double Modulatable::normalizedModulated() const {
	return positionIn(desc(), display().modulated);
}

double Modulatable::normalizedModulatedR() const {
	return positionIn(desc(), display().modulatedR);
}

double Modulatable::restNormalized() const {
	const ParamDesc& d = desc();
	return d.centred ? positionIn(d, 0.5 * (d.min + d.max)) : 0.0;
}

double Modulatable::scalePosition(double value, double base) const {
	const ParamDesc& d = desc();
	if (hasWindowScale(d)) {
		const double span = routeSpanOf(d);
		return span > 0.0 ? 0.5 + (value - base) / (2.0 * span) : 0.5;
	}
	return positionIn(d, value);
}

Modulatable::Mark Modulatable::liveMark() const {
	const ModDisplay state = display();
	Mark out;
	out.base = static_cast<float>(scalePosition(state.base, state.base));
	out.at = out.base;
	if (!state.isDestination) {
		return out;
	}
	out.at = static_cast<float>(scalePosition(state.modulated, state.base));
	out.moved = std::abs(out.at - out.base) > 0.001f;
	return out;
}

Modulatable::Span Modulatable::amountSpan() const {
	const ModDisplay state = display();
	const auto base = static_cast<float>(scalePosition(state.base, state.base));
	if (!state.isDestination || state.amount == 0.0f) {
		return { base, base };
	}
	// AN AMOUNT IS IN UNITS OF THE ROUTE'S SPAN (`routeSpanOf`: a declared `modSpan`, a zone's width, or the whole
	// range), the same unit the engine applies it in and `Knob`'s ring draws it in. Reading it as a fraction of
	// the range drew a zoned or `modSpan` control's reach many times too long, and a drag window's as nothing.
	const ParamDesc& d = desc();
	const double range = d.max - d.min;
	const double span = routeSpanOf(d);
	const double unit = hasWindowScale(d) ? 0.5 : (range > 0.0 ? span / range : 1.0);
	// THE CURVE'S REACH (`reachLo`/`reachHi`), as the ring draws it: a bipolar source goes both ways from the base.
	const auto reachLo = static_cast<float>(base + state.reachLo() * state.amount * unit);
	const auto reachHi = static_cast<float>(base + state.reachHi() * state.amount * unit);
	const float lo = std::clamp(std::min(reachLo, reachHi), 0.0f, 1.0f);
	const float hi = std::clamp(std::max(reachLo, reachHi), 0.0f, 1.0f);
	return { lo, hi };
}



std::optional<std::string> Modulatable::editText() const {
	// THE DECLARED UNIT, AND ONLY WHILE IT IS WHAT THE CONTROL SHOWS: an adapter printing through some other
	// road (a host formatter, a device's own words) would otherwise take a number in a unit it is not showing.
	const double at = value();
	if (text() != formatValue(desc(), at)) {
		return std::nullopt;
	}
	return device::editText(desc(), at);
}

std::optional<double> Modulatable::parseText(std::string_view text, double shown) const {
	return parseValue(desc(), text, shown);
}

bool Modulatable::resetToDefault() {
	const double def = desc().def;
	// A CONTROL ALREADY AT ITS DEFAULT SPENDS NO UNDO STEP. Knob's rule, and the reason its comment
	// says a double-click is safe to spend a click on: the gesture is free when it is a no-op.
	if (value() == def) {
		return false;
	}
	beginGesture();
	setValue(def);
	endGesture();
	return true;
}

bool Modulatable::dropRoute() {
	if (display().amount == 0.0f) {
		return false;
	}
	beginAmountGesture();
	setAmount(0.0f);
	endAmountGesture();
	return true;
}

} // namespace feathers::device
