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

#include "device/registry.h"

#include <cstdint>

// **HOW LONG A CHAIN MAY SOUND AFTER ITS INPUT STOPS — what `clap_plugin_tail` answers, for both plugins.**
//
// clap-wrapper maps it to VST3's `getTailSamples`, and a VST3 host that is told nothing may stop calling a
// plugin once its input goes silent: the reverb's tail is cut where the audio stopped. So the rack and
// Feathers' fx lane answer the same question the same way, here.
//
// **INFINITE WHEN ANY DEVICE PROCESSES AUDIO, ZERO WHEN NONE DOES — NOT A SUM.** A sum needs each device to
// declare its tail and none does; and several have no finite one to declare — a delay's feedback, a
// looper, a pitch shifter's regen, a reverb whose size is a modulatable control — so a declared number
// would be a guess that cuts the ring it guessed short. Even a filter rings at high resonance. Infinite is
// safe because it is not a promise to run forever: it hands the stopping decision back to `process()`,
// which answers SLEEP only on an exactly silent output block (the decision is made by the OUTPUT of the
// whole chain, never by a proxy). A lane of modulators alone makes no sound after its input, so it is zero.
namespace feathers::device {

/// `forEachDevice(f)` calls `f(const Descriptor*)` once per device the chain holds (null for one this
/// build cannot run, which runs nothing and so rings nothing).
template <class ForEachDevice>
[[nodiscard]] uint32_t chainTailSamples(ForEachDevice&& forEachDevice) {
	bool rings = false;
	forEachDevice([&](const Descriptor* desc) {
		if (desc != nullptr && desc->audio != nullptr) {
			rings = true;
		}
	});
	return rings ? UINT32_MAX : 0;
}

} // namespace feathers::device
