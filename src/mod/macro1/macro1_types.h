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

// MACRO 1 — ONE MACRO, AS A ONE-KNOB CARD. The same device as MACROS at a count of one
// (`macros_core.h`): the value is a modulation destination and a source, with the polarity and the type
// a macro has. Its card is a single large knob.

namespace feathers::mod::macro1 {

inline constexpr size_t kMacroCount = 1;

enum class Param : size_t { Macro1 = 0, Bipolar1, Type1, Enabled, kCount };
static_assert(static_cast<size_t>(Param::Enabled) == mod::macros::enableField(kMacroCount));

inline constexpr auto kParamDescs = mod::macros::makeVariantParams<kMacroCount>();
inline constexpr auto kOutputs = mod::macros::makeVariantOutputs<kMacroCount>();
static_assert(kParamDescs.size() == static_cast<size_t>(Param::kCount));

inline constexpr device::ModOps kModOps = mod::macros::makeModOps<kMacroCount>();

inline constexpr device::Traits kTraits {
	.tag = "MACRO1",
	.name = "Macro 1",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = mod::macros::kVariantTags,
};

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::macro1
