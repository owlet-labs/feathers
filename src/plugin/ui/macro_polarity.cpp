/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/macro_polarity.h"

#include "plugin/ui/value_box.h"

#include <algorithm>
#include <utility>

namespace feathers::ui {

BipolarMacro::BipolarMacro(device::Modulatable& inner, std::function<bool()> bipolar)
    : inner_(inner), bipolar_(std::move(bipolar)) {}

const device::ParamDesc& BipolarMacro::desc() const {
	if (!bipolar()) {
		return inner_.desc();
	}
	// THE SAME CONTROL, RE-RANGED FOR THE EYE: -1..1 with its rest at the centre and its default THERE
	// (a bipolar knob defaults to 0). Name, unit and everything else are the inner's.
	bipolarDesc_ = inner_.desc();
	bipolarDesc_.min = -1.0;
	bipolarDesc_.max = 1.0;
	bipolarDesc_.def = 0.0;
	bipolarDesc_.centred = true;
	return bipolarDesc_;
}

double BipolarMacro::value() const {
	return bipolar() ? toBipolar(inner_.value()) : inner_.value();
}

void BipolarMacro::setValue(double value) {
	if (!bipolar()) {
		inner_.setValue(value);
		return;
	}
	// CLAMPED IN THE VIEW'S OWN RANGE before the map, so a widget that overshoots hands the store a
	// value inside 0..1 rather than one the store clamps to an edge it did not mean.
	inner_.setValue(toStored(std::clamp(value, -1.0, 1.0)));
}

device::ModDisplay BipolarMacro::display() const {
	device::ModDisplay out = inner_.display();
	if (bipolar()) {
		out.base = toBipolar(out.base);
		out.modulated = toBipolar(out.modulated);
	}
	return out;
}

std::string BipolarMacro::text() const {
	// SIGNED IN BIPOLAR MODE — `formatValue` prints a Percent with its sign exactly when the range
	// runs below zero, which the view's declaration does; unipolar prints whatever the inner prints.
	return bipolar() ? formatValue(desc(), value()) : inner_.text();
}

ModState* BipolarMacro::routingState() const {
	const auto* routed = dynamic_cast<const RoutedControl*>(&inner_);
	return routed != nullptr ? routed->routingState() : nullptr;
}

} // namespace feathers::ui
