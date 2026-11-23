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

#include "mod/macros/macros_variant.h"

// MACROS 8 — EIGHT MACROS IN ONE DEVICE. The same device as MACROS at a count of eight
// (`macros_core.h`); its card is two rows of four.

namespace feathers::mod::macros8 {

inline constexpr size_t kMacroCount = 8;

inline constexpr auto kParamDescs = mod::macros::makeVariantParams<kMacroCount>();
inline constexpr auto kOutputs = mod::macros::makeVariantOutputs<kMacroCount>();

inline constexpr size_t kEnabledField = mod::macros::enableField(kMacroCount);
static_assert(kParamDescs.size() == kEnabledField + 1);

inline constexpr device::ModOps kModOps = mod::macros::makeModOps<kMacroCount>();

inline constexpr device::Traits kTraits {
	.tag = "MACROS8",
	.name = "Macros 8",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(kEnabledField),
	.tags = mod::macros::kVariantTags,
	/// Two rows of four cells under the knobs, each chip "M1".."M8".
	.outputGridColumns = 4,
};

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::macros8
