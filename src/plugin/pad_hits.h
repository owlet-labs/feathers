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

#include <array>
#include <cstdint>

#include "dsp/constellation.h"

namespace feathers {

/// **EVERY KIT PAD'S HITS, FOR THE PULSE** (2026-11-06 audition: the pads or cards could pulse when
/// triggered). The synth counts each pad's strikes and keeps the velocity of the last; the plugin publishes this beside the
/// master meter and the editor turns a count that moved into a pulse. A count, not a level, so a frame that misses a block
/// still sees the hit; the decay is the editor's, so nothing on the audio thread keeps time for a picture.
struct PadHits {
	std::array<uint32_t, kMaxSounds> count {};
	std::array<float, kMaxSounds> velocity {};
	friend bool operator==(const PadHits&, const PadHits&) = default;
};

} // namespace feathers
