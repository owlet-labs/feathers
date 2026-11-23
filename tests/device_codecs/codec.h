/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#pragma once

#include "device/registry.h"

#include <cstddef>
#include <vector>

// THE VOCABULARY OF A DEVICE'S ROW CODEC, shared by tests/test_device_ranges.cpp and every
// tests/device_codecs/<device>.h (backlog NY.1: a device registers its codec from its own file).

namespace feathers::codecs {

using RowVec = std::vector<float>;

/// **A DEVICE'S ROW CODEC, FLATTENED TO ONE SIGNATURE.** Every registered device has one; three
/// spellings of it exist (`stateOf`/`settingsOf`, with and without `fieldsPresent`) and the tracker
/// has none at all. The templates below absorb the spelling so the case can hold one table.
template <typename Row, typename State, State (*Decode)(const Row&, size_t), Row (*Encode)(const State&)>
inline RowVec tripSized(const RowVec& in) {
	Row row {};
	for (size_t i = 0; i < in.size() && i < row.size(); ++i) {
		row[i] = in[i];
	}
	const Row out = Encode(Decode(row, row.size()));
	return RowVec(out.begin(), out.end());
}

template <typename Row, typename State, State (*Decode)(const Row&), Row (*Encode)(const State&)>
inline RowVec tripPlain(const RowVec& in) {
	Row row {};
	for (size_t i = 0; i < in.size() && i < row.size(); ++i) {
		row[i] = in[i];
	}
	const Row out = Encode(Decode(row));
	return RowVec(out.begin(), out.end());
}

/// A device whose row is `Tabs` copies of one `PerTab`-long block, then fields that belong to no tab (the view
/// tab). Each block goes through the device's own per-tab conversion; the trailing fields are carried as they came.
/// (`tripSized` alone covers only the first block, so the other tabs' ranges would escape the sweep.)
template <typename Row, typename State, State (*Decode)(const Row&, size_t), Row (*Encode)(const State&), size_t Tabs, size_t PerTab>
inline RowVec tripTabbed(const RowVec& in) {
	RowVec out(in);
	for (size_t tab = 0; tab < Tabs; ++tab) {
		Row row {};
		for (size_t i = 0; i < PerTab && tab * PerTab + i < in.size(); ++i) {
			row[i] = in[tab * PerTab + i];
		}
		const Row enc = Encode(Decode(row, row.size()));
		for (size_t i = 0; i < PerTab && tab * PerTab + i < out.size(); ++i) {
			out[tab * PerTab + i] = enc[i];
		}
	}
	return out;
}


struct Codec {
	const char* tag;
	RowVec (*trip)(const RowVec&);
};

} // namespace feathers::codecs
