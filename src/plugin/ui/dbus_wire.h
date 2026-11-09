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
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// THE D-BUS WIRE FORMAT, AS MUCH OF IT AS THE FILE CHOOSER PORTAL NEEDS — and no libdbus, because the
// plugin links statically (the runtime-deps test) and a sandboxed host may not have the library.
//
// Written from the specification's "Message Protocol" section: a 16-byte fixed header, an array of
// (code, variant) header fields padded to 8, then the body, every value aligned to its own size
// measured from the START OF THE MESSAGE. Because the header ends on an 8-byte boundary, a body's
// alignment measured from the body's own start is the same thing, which is why `Writer` and `Reader`
// both count from zero.
//
// ENCODES LITTLE-ENDIAN ONLY (every platform this ships on); DECODES BOTH, because the bus daemon
// forwards a message in its sender's byte order.
//
// Pure bytes in, bytes out — no sockets — so it is compiled and tested on every platform, and the
// golden bytes in `tests/test_file_dialog.cpp` come from GLib's encoder (`gdbus` under `dbus-monitor
// --binary`), not from this one.

namespace feathers::ui::dbus {

enum class Type : uint8_t { Invalid = 0, MethodCall = 1, MethodReturn = 2, Error = 3, Signal = 4 };

/// Header flags.
constexpr uint8_t kNoReplyExpected = 0x1;

struct Message {
	Type type = Type::Invalid;
	uint8_t flags = 0;
	uint32_t serial = 0;
	std::string path;
	std::string interface;
	std::string member;
	std::string errorName;
	uint32_t replySerial = 0; ///< 0: none (a serial is never 0)
	std::string destination;
	std::string sender;
	std::string signature; ///< the body's
	std::vector<uint8_t> body;
	bool bigEndian = false; ///< the byte order `body` is in (decode; `encode` writes little-endian)
};

/// Appends values at their D-Bus alignment, little-endian.
class Writer {
public:
	void align(size_t n);
	void byte(uint8_t v);
	void boolean(bool v) { u32(v ? 1u : 0u); }
	void u32(uint32_t v);
	void string(std::string_view s);
	void objectPath(std::string_view s) { string(s); }
	void signature(std::string_view s);
	/// An ARRAY: `beginArray` writes the length and pads to the first element (padding the length does
	/// not count, even for an empty array); `endArray` fills the length in.
	struct ArrayMark {
		size_t lengthAt;
		size_t start;
	};
	ArrayMark beginArray(size_t elementAlign);
	void endArray(ArrayMark mark);
	/// A STRUCT or DICT_ENTRY starts on 8.
	void beginStruct() { align(8); }
	/// A VARIANT's type; its value follows, at its own alignment.
	void variantSignature(std::string_view s) { signature(s); }

	[[nodiscard]] const std::vector<uint8_t>& data() const noexcept { return buf_; }
	std::vector<uint8_t> take() { return std::move(buf_); }

private:
	std::vector<uint8_t> buf_;
};

/// Reads values back. A read past the end, or a malformed value, sets `ok()` false and returns zeros;
/// check once at the end.
class Reader {
public:
	Reader(const uint8_t* data, size_t size, bool bigEndian) : data_(data), size_(size), big_(bigEndian) {}
	explicit Reader(const Message& m) : Reader(m.body.data(), m.body.size(), m.bigEndian) {}

	[[nodiscard]] bool ok() const noexcept { return ok_; }
	[[nodiscard]] size_t pos() const noexcept { return pos_; }
	[[nodiscard]] bool atEnd() const noexcept { return pos_ >= size_; }

	void align(size_t n);
	uint8_t byte();
	bool boolean() { return u32() != 0; }
	uint32_t u32();
	std::string string();
	std::string signature();
	/// An ARRAY's length, then the padding to its first element. Elements run to the returned offset.
	size_t beginArray(size_t elementAlign);
	/// **SKIP ONE COMPLETE TYPE**, the one at the front of `sig`, and consume it from `sig`. How an
	/// unknown dictionary entry is stepped over without understanding it.
	bool skip(std::string_view& sig);

private:
	const uint8_t* data_;
	size_t size_;
	bool big_;
	size_t pos_ = 0;
	bool ok_ = true;
};

/// The alignment of the type at the front of `sig`.
size_t alignmentOf(char code);

/// The whole message, little-endian, header fields in ascending code order (GLib's order).
std::vector<uint8_t> encode(const Message& message);

/// **HOW MANY BYTES THE MESSAGE AT THE FRONT OF `data` OCCUPIES** — 0 while fewer than the 16 bytes
/// that say so are there. `malformed` is set for a header no message could have.
size_t messageLength(const uint8_t* data, size_t size, bool* malformed);

/// Decode exactly one message of `size` bytes. False for a malformed one.
bool decode(const uint8_t* data, size_t size, Message& out);

} // namespace feathers::ui::dbus
