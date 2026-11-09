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

Modulatable::Span Modulatable::amountSpan() const {
	const ModDisplay state = display();
	const auto base = static_cast<float>(positionIn(desc(), state.base));
	if (!state.isDestination || state.amount == 0.0f) {
		return { base, base };
	}
	// AN AMOUNT IS IN UNITS OF THE CONTROL'S OWN RANGE, which is what makes one amount mean the same
	// distance on a control that runs 0..1 and one that runs -100..100 — and so what lets this be
	// computed without asking what the control is. The reach is therefore the amount itself, on the
	// 0..1 sweep.
	const float reach = base + state.amount;
	const float lo = std::clamp(std::min(base, reach), 0.0f, 1.0f);
	const float hi = std::clamp(std::max(base, reach), 0.0f, 1.0f);
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
