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

#pragma once

#include "device/macro_type.h"
#include "plugin/params.h"

#include <array>
#include <cstdint>
#include <string>

namespace feathers {

/// **WHICH WAY EACH STRUCTURAL MACRO SWINGS (backlog EG).** Per macro, because the switch is on the knob: one macro pulled to a centre-rest
/// pan-like control beside three that stay 0..1.
///
/// THE PARAMETER UNDERNEATH DOES NOT CHANGE. A macro stays a 0..1 CLAP parameter the DAW automates
/// by that name; bipolar is how the knob, the readout and the PUBLISHED SOURCE read it (`2v - 1`,
/// rest at the middle). So this is patch state beside the parameters rather than a parameter, saved
/// in its own block, and absent reads as every macro unipolar — which is exactly what every patch
/// from before the block was.
struct MacroModes {
	std::array<uint8_t, kMacroCount> bipolar {};
	/// How the knob drives the parameter (see `device/macro_type.h`); Knob is what every macro was.
	std::array<device::MacroType, kMacroCount> type {};
	/// The user's name for the macro, at most `device::kMacroNameMax` bytes of UTF-8. Empty = the
	/// default "Macro N".
	std::array<std::string, kMacroCount> name {};

	friend bool operator==(const MacroModes&, const MacroModes&) = default;
};

} // namespace feathers
