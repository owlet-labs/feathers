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
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace feathers::fx::conv {

/// **HOW THE IR IS SHAPED BEFORE IT RUNS** — every control that rebuilds the kernel rather than moving a
/// parameter (backlog JD). Applied to the generated room and to a file alike.
///
/// - **MARKERS** (`begin`, `end`): the span of the source that runs, as fractions of it.
/// - **STRETCH**: the span's length multiplied, by resampling it — longer is also lower, as a slowed
///   tape is. 1 is as recorded.
/// - **ENVELOPE**: an MSEG-shaped gain drawn over the span (an mseg style env which draws over the
///   active part of the impulse). Stored as the MSEG's own breakpoints — a start level and up to
///   `kIrEnvSegments` segments of (share, level, shape) — so the editor that draws it is the MSEG's, and
///   evaluated by the MSEG's geometry in the x it draws. The conv layer only carries the numbers; the
///   device turns them into gains.
inline constexpr size_t kIrEnvSegments = 8;
inline constexpr float kIrStretchMin = 0.25f;
inline constexpr float kIrStretchMax = 4.0f;
struct IrShape {
	float begin = 0.0f;
	float end = 1.0f;
	float stretch = 1.0f;
	float envStart = 1.0f;
	uint32_t envCount = 1;
	/// share (the MSEG's log2 units), level (0..1), shape (-1..1). Flat at 1 by default.
	std::array<std::array<float, 3>, kIrEnvSegments> envSegment {{{0.0f, 1.0f, 0.0f}}};
	/// LITE: the late tail at half rate (`conv::kLiteSplit`) — cheaper, and duller above a quarter of the rate.
	bool lite = false;
	bool operator==(const IrShape&) const = default;
};

/// **WHICH IMPULSE RESPONSE A DEVICE RUNS, AS A PATCH SAVES IT** — a device's body (`device::BodyOps`).
///
/// The IR's audio is not saved with the patch, only its filename — with an escape hatch in case a full
/// save is added later. So this build writes a PATH, and the format carries a KIND so a
/// later build can write the audio itself:
///
///     'F' 'I' 'R' '1'   magic
///     u8  version       1
///     u8  kind          0 generated room, 1 file path, 2 EMBEDDED (reserved: the escape hatch)
///     u16 reserved      0
///     u32 bytes         the payload's length
///     ... payload       the shape (`kIrShapeBytes`, below), then for kind 1 the path, UTF-8, no
///                       terminator
///
///     shape:  f32 begin, f32 end, f32 stretch, f32 envStart, u32 envCount,
///             then kIrEnvSegments x (f32 share, f32 level, f32 shape), then u32 flags (bit 0: Lite; bits 8-15:
///             which generated room, kind 0 only;
///             every other bit must be 0)
///
/// **THE ESCAPE HATCH IS `Embedded`:** a build that embeds writes kind 2 with the audio after the shape.
/// This build REFUSES kind 2 rather than guessing at it (all-or-nothing, as `loadState` promises), and a
/// build that embeds still reads every body this one writes. Little-endian throughout.
struct IrRef {
	enum class Kind : uint8_t { Generated = 0, File = 1, Embedded = 2 };
	Kind kind = Kind::Generated;
	std::string path;
	IrShape shape;
	/// Which generated room, for kind 0 (`conv::generateRoom`: 0 Room, 1 Plate, 2 Hall, 3 Ambience); 0 for a file.
	uint8_t generated = 0;
};

inline constexpr uint8_t kIrRefVersion = 1;
inline constexpr size_t kIrRefHeaderBytes = 12;
/// How many generated rooms a body may name (`conv::kGeneratedRooms`, asserted equal where both are seen).
inline constexpr uint32_t kIrGeneratedRooms = 4;
inline constexpr size_t kIrShapeBytes = 5 * 4 + kIrEnvSegments * 3 * 4 + 4;
/// A path longer than this is not a path: the bound a reader of a file on somebody's disk needs.
inline constexpr size_t kIrRefMaxPathBytes = 4096;
inline constexpr size_t kIrRefMaxBytes = kIrRefHeaderBytes + kIrShapeBytes + kIrRefMaxPathBytes;

[[nodiscard]] std::vector<uint8_t> encodeIrRef(const IrRef& ref);
/// False, and `out` untouched, for anything this build cannot read — including kind 2, markers that are
/// not a span, a stretch outside its range, and an envelope with no segments or more than it holds.
[[nodiscard]] bool decodeIrRef(const uint8_t* data, size_t bytes, IrRef& out);

/// The file's name without its folders, for a label.
[[nodiscard]] std::string irDisplayName(const std::string& path);

} // namespace feathers::fx::conv
