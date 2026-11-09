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

#include <cstddef>

// THE SHAPE OF ONE HOST'S MODULATION SPACE — the two axes, as a value a host declares rather than
// as constants a header knows.
//
// This is the SHAPE of a host — how many of its own sources and destinations it has, how many device
// slots its pool holds, and how wide a device may be on each axis. It sizes pools; it is NOT an address
// space. The linearisation that once lived here — `[coreDestinations, destinations())` with a stride per
// slot, and the `deviceSlotOf…`/`…OfOrdinal` family that walked it — is deleted (kill-lanes §2.4b): a
// route names its ends by identity (`Source`/`Destination`), a node owns its outputs and a control pulls
// its inputs, and the only address is a pointer built at a drain.
//
// WHAT MOVED IS ONLY WHERE THE NUMBERS COME FROM. Feathers declares coreDestinations = 80 and
// coreSources = 60 because that is what its parameter space is; the rack declares 8 macros and no
// core destinations at all, because in a rack every control on the surface belongs to a device. Same
// mechanism, different numbers — which is the whole claim this file exists to make good on.
//
//------------------------------------------------------------------------------------------------//
// WHY THIS IS A RUNTIME VALUE AND NOT A TEMPLATE PARAMETER                                         //
//------------------------------------------------------------------------------------------------//
//
// `Axes` is a structural type and C++20 would let it be a non-type template parameter, which would
// keep the route table a `std::array` and keep today's compile-time bounds. That was the first
// design and it is rejected, for two reasons that are not about taste.
//
//   - TEMPLATE INFECTION REACHES FURTHER THAN THE TABLE. Everything that names the route table names
//     it too: the undo stack's per-slot slice, the edit history, the Engine rail, every control
//     adapter. A template parameter on the table is a template parameter on all of them, and those
//     are the files two other workstreams are editing.
//
//   - AND THE STRIDES CANNOT BE COMPILE-TIME IN THE WORLD THIS IS HEADING FOR. A host that admits a
//     third-party CLAP plugin as a device cannot know its widest device until one is loaded. This exact point was
//     already decided against the instance pool: the possible effects cannot be known at compile
//     time. A design that is
//     compile-time today and must be runtime the day guests arrive is a design that will be rewritten
//     by whoever adds them.
//
// WHAT IT COSTS is the four `static_assert`s that currently walk the registry — the modulatable
// stride, the output stride, the field budget and tag distinctness. Those become ADMISSION-TIME
// REFUSALS: a device that does not fit is declined at registration with a reason, rather than failing
// to compile. That is the same guarantee arriving at the only moment it still can, and it is the only
// moment that can also refuse a guest.
//
// THE TABLE ALLOCATES ONCE, AT HOST CONSTRUCTION, AND NEVER ON THE AUDIO THREAD. The no-allocation
// rule is about the audio thread and has always been; `FxRunner`'s own header records the decision that
// construction-time allocation is fine.

namespace feathers::backplane {

/// HOW MANY DESTINATION COLUMNS AND SOURCE ROWS A HOST HAS, and how they are divided.
///
/// EVERY FIELD IS A COUNT AND NONE OF THEM IS AN INDEX, which is what makes the derivations below
/// total rather than conditional. A host that holds no devices declares `deviceSlots = 0` and the
/// device halves of both axes are empty rather than absent — there is no second shape to handle.
struct Axes {
	/// The host's own modulation sources — macros, and whatever else it publishes that is not a
	/// device. Feathers: 4 macros + 8 weights + ... ; the rack: 8 macros.
	size_t coreSources = 0;

	/// The host's own parameters that may be modulated. Feathers: 80. THE RACK DECLARES ZERO, and
	/// that is the case worth naming because it is the one a reader will expect to be special: in a
	/// rack every control on the surface belongs to a device, so the core half of the destination
	/// axis is genuinely empty and every destination is a device destination.
	size_t coreDestinations = 0;

	/// How many devices the host can hold, ACROSS ALL ITS CHAINS. One pool, because a destination
	/// index is what a saved patch stores and a route does not care which chain its device sits in —
	/// `plugin/params.h` already divides one slot space between the fx lane and the rack for exactly
	/// this reason.
	size_t deviceSlots = 0;

	/// The source rows one slot owns — its modulation outputs. (The destination axis has no stride since backlog OJ:
	/// a control is named by its device and its own index, and the edge stores grow.)
	size_t outputStride = 0;


	friend bool operator==(const Axes&, const Axes&) = default;
};

//------------------------------------------------------------------------------------------------//
// THE SOURCE AXIS                                                                                  //
//------------------------------------------------------------------------------------------------//

//------------------------------------------------------------------------------------------------//
// VALIDITY                                                                                         //
//------------------------------------------------------------------------------------------------//

/// WHETHER THESE AXES DESCRIBE A SPACE THE DECODERS ABOVE CAN ACTUALLY WALK.
///
/// THE OUTPUT STRIDE MUST BE NON-ZERO WHENEVER THERE ARE SLOTS (the destination stride is gone, backlog OJ).
/// A host declaring slots with a zero stride is not describing a small space, it is describing one
/// whose every device query is undefined behaviour — so this is a real refusal and not a tidiness
/// check. A host with `deviceSlots == 0` may leave it zero and nothing ever divides.
///
/// IT IS A FUNCTION AND NOT A CONSTRUCTOR INVARIANT because `Axes` is an aggregate and should stay
/// one: it is written as a literal in exactly two places (each host's declaration) and read
/// everywhere, and an aggregate is what lets those two declarations be `constexpr` and reviewable in
/// one screen. The check belongs where the host is built, which is the moment it can be reported.
constexpr bool valid(const Axes& axes) {
	if (axes.deviceSlots == 0) {
		return true;
	}
	return axes.outputStride > 0;
}

} // namespace feathers::backplane
