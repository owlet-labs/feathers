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

#include <atomic>
#include <cstdint>

namespace feathers::ui {

/// **A FRESH SERIAL FOR A RAW PICTURE, STAMPED WHERE ITS PIXELS ARE PRODUCED** (`Canvas::imageRaw`'s `serial`). The atlas
/// uploads a raw buffer only when the serial it is handed moves, so the serial is what says "these pixels changed".
/// PROCESS-WIDE AND MONOTONIC, never per owner: the atlas keys an entry on the buffer's pointer and size, and a freed
/// buffer's address can come back as a different owner's — two owners counting from 1 would hand the atlas the same
/// serial for different pixels at the same key (the ABA rule: an address is never an identity). Never zero, which is
/// the atlas's "upload every draw".
inline uint64_t freshPixelSerial() noexcept {
	static std::atomic<uint64_t> next { 0 };
	return next.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace feathers::ui
