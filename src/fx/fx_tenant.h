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

#include "device/device.h"

#include <cstddef>
#include <cstdint>
#include <span>

// WHAT A TENANT OF THE FX LANE DECLARES ABOUT ITSELF — which since 2026-08-22 is what ANY DEVICE
// declares about itself, and lives in device/device.h.
//
// THE VOCABULARY MOVED AND THE NAMES DID NOT. Every word of the argument that used to be written
// here was about DEVICES and none of it was about effects: a device owns its DSP, its control
// declarations, its state payload and its card, and includes nothing from src/plugin. The only
// reason it lived under src/fx is that the filter was the first one. the fx and modulators
// are intended to be standalone capable, ie not coupled to the params of feathers — the modulator
// rack is the second host of the same idea, and a second copy of this vocabulary would have been the
// thing that stopped one generic card from serving both.
//
// So the types below are aliases, and `fx::ParamDesc` goes on meaning what it has always meant. What
// changed is that `mod::dahdsr::kParamDescs` and `fx::filter::kParamDescs` are now the SAME TYPE, so
// anything that can render one can render the other. tests/test_fx_filter.cpp still reads these
// sources and refuses a src/plugin include; device/device.h is under the same rule and
// tests/test_mod_dahdsr.cpp checks it for src/mod.

namespace feathers::fx {

using ParamUnit = device::Unit;
using ParamDesc = device::ParamDesc;
using TenantTraits = device::Traits;

/// NOTES FOR WHOEVER TESTS THE NEXT TENANT, all three of them bought with time on the first one.
/// Each is a way for a test to PASS FOR THE WRONG REASON, and none of them is visible by reading the
/// test — every one was found by measuring something the test claimed to already know.
///
///   - A SPECTRAL MEASUREMENT WHOSE BIN IS SHARED. A case comparing two voices measured the high
///     one's fundamental at 1046.5 Hz, which is also the low one's FOURTH HARMONIC, so the bin held
///     both and could not tell them apart: it read a 1.4 dB difference where there was a 15 dB one,
///     and sent an afternoon hunting a wiring bug that did not exist. Pick intervals whose ratio is
///     not one of small whole numbers — a tritone plus an octave has no such coincidence.
///
///   - A PRESENCE-SHAPED ASSERTION CANNOT SEE AN ADDED TRANSIENT. A case checking that a voice takes
///     no filter ringing from the note before it asked "is the gap silent, is the new note audible",
///     and PASSED with the fix deliberately removed — because carried-over state ADDS a transient
///     rather than removing one, and nothing phrased as "is there sound here" can see that. Compare
///     samples against a known-good render instead.
///
///   - A MEASUREMENT NEEDS ITS OWN PRECONDITION. Verifying a drawn response against a measured
///     impulse response is only valid if the filter finished ringing inside the window; a truncated
///     one is the response of the first N samples, and it disagrees exactly where a filter rings
///     longest. The tempting fix is a wider tolerance, which blinds the case to the real
///     disagreements it exists to find. Assert that the tail energy is negligible, and derive that
///     threshold from measurements rather than picking it.
///
} // namespace feathers::fx
