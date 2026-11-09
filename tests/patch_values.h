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

#include "rack/rack_state.h"

#include <string_view>
#include <vector>

// A PATCH DEVICE'S VALUES AS A ROW AND BACK, FOR CASES — through the SAME translation every load and save
// takes (`rack::valuesOfRow`, `rack::rowOfValues`), so a case that builds a patch from a row of floats and
// reads one back is exercising the keyed road rather than going round it. Since backlog JF a patch holds
// values by key; these keep the cases written in the row terms they were written in.

namespace feathers::test {

/// Set `d`'s values from a row of `desc`'s fields — every control, and the extra state if the row reaches it.
inline void setRow(rack::RackPatch::Device& d, const device::Descriptor& desc, const std::vector<float>& row) {
	rack::valuesOfRow(desc, row, row.size(), &d);
}

/// `desc`'s row as `d`'s values give it — a missing key at its default, trimmed to what the values carried.
inline std::vector<float> rowOf(const rack::RackPatch::Device& d, const device::Descriptor& desc,
                                rack::LoadReport* report = nullptr) {
	std::vector<float> row(desc.params.size() + desc.traits.extraState);
	row.resize(rack::rowOfValues(desc, d, row, report, desc.traits.name, nullptr));
	return row;
}

/// The value stored under `key`, or null.
inline const rack::RackPatch::Value* valueOf(const rack::RackPatch::Device& d, std::string_view key) {
	for (const rack::RackPatch::Value& v : d.values) {
		if (v.key == key) {
			return &v;
		}
	}
	return nullptr;
}

} // namespace feathers::test
