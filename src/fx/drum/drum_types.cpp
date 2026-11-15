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

#include "fx/drum/drum_types.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::drumdevice {

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	for (size_t i = 0; i < s.v.size(); ++i) {
		const device::ParamDesc& d = kParamDescs[i];
		float v = i < fieldsPresent ? row[i] : static_cast<float>(d.def);
		if (!std::isfinite(v)) {
			v = static_cast<float>(d.def);
		}
		if (d.stepped) {
			v = std::round(v);
		}
		s.v[i] = std::clamp(v, static_cast<float>(d.min), static_cast<float>(d.max));
	}
	return s;
}

Row rowOf(const Settings& settings) {
	return settings.v;
}

} // namespace feathers::fx::drumdevice
