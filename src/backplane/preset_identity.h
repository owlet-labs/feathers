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
 */

#pragma once

#include <string>

// **WHICH PRESET THE PATCH CAME FROM** (backlog QC).
//
// The browser's "current preset" lived only in the EDITOR (`PresetBrowser`'s last saved or loaded entry), so anything
// that rebuilt the editor or the instance — an engine restart reloads the saved state into a fresh instance, and
// closing the editor rebuilds it — came back naming the bank's first entry while the sound was the old preset's.
// So it is PATCH STATE now: the bridge holds it beside the parameters, the `preset` block saves it keyed, and the
// editor restores its face from it.
//
// THE NAME CARRIES THE VARIANT. A variant is a file named "<base>_variant_<n>" (the root is variant 1), so naming the
// entry names the chip; there is no second field to drift. There is no dirty/modified mark to carry: the browser has
// none (the face names the preset the sound came from, edited or not).

namespace feathers {

struct PresetIdentity {
	std::string name;     ///< the entry's name as the bank lists it ("Glass", "Glass_variant_3"); empty: none
	std::string category; ///< its folder ("Pads", "Pads/Glass"); empty: the bank's top level
	bool operator==(const PresetIdentity&) const = default;
	[[nodiscard]] bool empty() const noexcept { return name.empty(); }
};

/// The keys the `preset` block stores them under (the schema-ledger rule: a value is named by a key).
inline constexpr const char* kPresetNameKey = "preset.name";
inline constexpr const char* kPresetCategoryKey = "preset.category";
/// The longest name or folder a stream may carry; longer is refused as corrupt rather than allocated.
inline constexpr size_t kPresetIdentityMaxBytes = 1024;

} // namespace feathers
