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

#include "plugin/denormal_guard.h"

#include <clap/clap.h>

/// **WHAT THE FPU LOOKS LIKE INSIDE A PLUGIN'S process(), READ FROM THE HOST'S SIDE OF THE CALL.**
///
/// The guard's own case (test_clap_render) proves the CLASS flushes and restores; it cannot see whether
/// process() constructs one, and deleting `const DenormalGuard denormals;` from either plugin left every
/// suite green. A plugin calls the host's `in_events->size()` from inside process(), after the guard is
/// up, so an input-event list whose `size` reads the control word and multiplies a denormal is a probe
/// the plugin cannot be built around: it reports what the plugin's own arithmetic would see at that
/// instant, and needs no seam in the plugin.
struct DenormalProbe {
	bool called = false;
	unsigned long long wordInside = 0;
	bool flushedInside = false;

	/// Whether a denormal operand yields exactly zero on this thread RIGHT NOW. Volatile on both sides,
	/// so the multiply is the FPU's and not the compiler's.
	static bool flushesNow() noexcept {
		volatile float tiny = 1.0e-40f;
		const volatile float product = tiny * 1.0f;
		return product == 0.0f;
	}

	clap_input_events events() noexcept {
		clap_input_events in {};
		in.ctx = this;
		in.size = [](const clap_input_events* list) -> uint32_t {
			auto* self = static_cast<DenormalProbe*>(list->ctx);
			if (!self->called) {
				self->called = true;
				self->wordInside = feathers::DenormalGuard::currentControlWord();
				self->flushedInside = flushesNow();
			}
			return 0;
		};
		in.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; };
		return in;
	}
};
