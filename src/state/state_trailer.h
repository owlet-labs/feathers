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

// **THE END MARKER — A STATE STREAM SAYS WHERE IT ENDS, AND THAT IT IS WHOLE**.
//
// A save is serialised whole into one buffer and then this trailer is appended over it:
//
//     'F' 'E' 'N' 'D'   magic
//     u64               the payload's length in bytes (everything before the trailer), little-endian
//     u32               CRC-32 (IEEE 802.3, the zlib polynomial) of the payload, little-endian
//
// A load REQUIRES it before anything is committed: a stream cut anywhere — at a block boundary, which every
// block's loader used to read as "a patch from before this block" and fill with defaults, or inside one —
// has lost its trailer and is refused; a stream with the wrong length or a flipped byte is refused by the
// count or the checksum. Used by Feathers' state (kStateVersion 50, `state_block_order.h`) and by the
// standalone rack's (`rack_clap.cpp`), never by the bodies and documents nested inside either.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace feathers::state {

inline constexpr uint8_t kEndMagic[4] = { 'F', 'E', 'N', 'D' };
inline constexpr size_t kTrailerBytes = 4 + 8 + 4;

/// CRC-32, IEEE 802.3 (reflected, polynomial 0xEDB88320, initial and final XOR 0xFFFFFFFF) — the zlib one,
/// so `crc32("123456789") == 0xCBF43926`. Table-driven; the table is built once at compile time.
inline uint32_t crc32(std::span<const uint8_t> bytes) noexcept {
	static constexpr std::array<uint32_t, 256> kTable = [] {
		std::array<uint32_t, 256> table {};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) {
				c = (c & 1u) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
		return table;
	}();
	uint32_t crc = 0xFFFFFFFFu;
	for (const uint8_t b : bytes) {
		crc = kTable[(crc ^ b) & 0xFFu] ^ (crc >> 8);
	}
	return crc ^ 0xFFFFFFFFu;
}

/// Append the trailer over everything `bytes` holds now.
inline void appendTrailer(std::vector<uint8_t>& bytes) {
	const uint64_t length = bytes.size();
	const uint32_t crc = crc32(bytes);
	bytes.insert(bytes.end(), kEndMagic, kEndMagic + 4);
	for (int i = 0; i < 8; ++i) {
		bytes.push_back(static_cast<uint8_t>((length >> (8 * i)) & 0xFFu));
	}
	for (int i = 0; i < 4; ++i) {
		bytes.push_back(static_cast<uint8_t>((crc >> (8 * i)) & 0xFFu));
	}
}

enum class TrailerCheck : uint8_t { Ok, Missing, LengthMismatch, Checksum };

/// The words the event log uses for each refusal.
inline const char* trailerWhy(TrailerCheck check) noexcept {
	switch (check) {
	case TrailerCheck::Ok: return "ok";
	case TrailerCheck::Missing: return "no-end-marker";
	case TrailerCheck::LengthMismatch: return "length-mismatch";
	case TrailerCheck::Checksum: return "checksum";
	}
	return "?";
}

/// Whether `bytes` ends in a trailer that vouches for everything before it; `payload` gets the payload's length
/// when it does. MISSING when the stream is too short or does not end in the magic — which is what a stream
/// cut anywhere looks like, since the trailer is the last thing written.
inline TrailerCheck checkTrailer(std::span<const uint8_t> bytes, size_t* payload = nullptr) noexcept {
	if (bytes.size() < kTrailerBytes) {
		return TrailerCheck::Missing;
	}
	const size_t at = bytes.size() - kTrailerBytes;
	if (std::memcmp(bytes.data() + at, kEndMagic, 4) != 0) {
		return TrailerCheck::Missing;
	}
	uint64_t length = 0;
	for (int i = 0; i < 8; ++i) {
		length |= static_cast<uint64_t>(bytes[at + 4 + static_cast<size_t>(i)]) << (8 * i);
	}
	if (length != at) {
		return TrailerCheck::LengthMismatch;
	}
	uint32_t crc = 0;
	for (int i = 0; i < 4; ++i) {
		crc |= static_cast<uint32_t>(bytes[at + 12 + static_cast<size_t>(i)]) << (8 * i);
	}
	if (crc != crc32(bytes.first(at))) {
		return TrailerCheck::Checksum;
	}
	if (payload != nullptr) {
		*payload = at;
	}
	return TrailerCheck::Ok;
}

/// **THE WHOLE BUFFER TO THE HOST'S STREAM, OR AS MUCH AS IT TAKES** — `written` gets how many bytes the host
/// accepted, so a short write is logged as one. False unless it took them all.
template <typename OStream>
bool writeWhole(const OStream* out, std::span<const uint8_t> bytes, uint64_t* written) {
	uint64_t done = 0;
	while (done < bytes.size()) {
		const int64_t n = out->write(out, bytes.data() + done, bytes.size() - done);
		if (n <= 0) {
			break;
		}
		done += static_cast<uint64_t>(n);
	}
	if (written != nullptr) {
		*written = done;
	}
	return done == bytes.size();
}

} // namespace feathers::state
