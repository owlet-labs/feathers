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
#include <span>
#include <string>
#include <string_view>
#include <vector>

// THE STATE FORMAT'S TEXT — a JSON writer and reader of our own (backlog JF).
//
// **NUMBERS GO THROUGH `std::to_chars` AND `std::from_chars` AND NOTHING ELSE.** Both are exact
// (shortest round-trip text, measured bit-exact over every registry value and a strided sweep of every
// float32 pattern) and both ignore the C locale by definition. `printf("%g")` and `strtod` do neither: a
// host running under a comma-decimal locale would write `0,5`. No printf- or strtod-family call may touch
// a state number.
//
// **NaN AND ±Inf ARE NOT JSON**, so the writer writes `null` for them and a reader treats `null` where a
// number belongs as non-finite — the caller's rule then applies (a control's declared default).
//
// **RAW BYTES ARE BASE64 IN A TAGGED OBJECT**, `{"$bin": "<base64>", "size": <bytes>}`. The tag is what
// keeps a later move to a container cheap: every consumer asks for "the bytes at this node" (`binOf`) and
// never for the base64 text.
//
// **THE READER IS OURS TOO, NOT VENDORED** (decided 2026-09-25, JF step 1a). The design named yyjson
// first and our own parser as the fallback, and asked for yyjson's cost in the `.clap` to be checked.
// MEASURED (g++-14 -O2, reader-only configuration, writer/utils/incremental reader disabled): 117,533
// bytes of text from 19,947 lines of vendored source, against a grammar this file covers in a few
// hundred. What the design feared in a parser of our own — string escapes, UTF-8, surrogate pairs — is
// exactly what the mutation fuzzer (`test_state_json.cpp`) aims at, and owning the reader gives the
// three properties the format needs without a flag to find: numbers kept as their RAW TEXT (so a value
// a newer build wrote is carried back out byte for byte), a depth and a node budget enforced while
// reading, and duplicate keys refused.
//
// MESSAGE THREAD. Everything here allocates.

namespace feathers::state::json {

//------------------------------------------------------------------------------------------------//
// NUMBERS                                                                                        //
//------------------------------------------------------------------------------------------------//

/// Append `v` as JSON: the shortest text that reads back to the same bits, or `null` when it is not
/// finite. `-0` is written as `-0` and reads back as -0.
void appendNumber(std::string* out, float v);
void appendNumber(std::string* out, double v);
void appendNumber(std::string* out, uint64_t v);
void appendNumber(std::string* out, int64_t v);

/// Read a JSON number's text. False for text that is not a whole JSON number, and for a value that does
/// not fit — `1e999` read as a float is out of range, never infinity.
bool parseNumber(std::string_view text, float* out);
bool parseNumber(std::string_view text, double* out);
/// An integer only: no fraction, no exponent, no sign for the unsigned form.
bool parseNumber(std::string_view text, uint32_t* out);
bool parseNumber(std::string_view text, uint64_t* out);
bool parseNumber(std::string_view text, int64_t* out);

//------------------------------------------------------------------------------------------------//
// BASE64 — RFC 4648, standard alphabet, padded                                                   //
//------------------------------------------------------------------------------------------------//

void appendBase64(std::string* out, std::span<const uint8_t> bytes);
/// False for anything that is not canonical padded base64, or that would decode past `maxBytes`.
/// The length is checked BEFORE a byte is allocated.
bool decodeBase64(std::string_view text, std::vector<uint8_t>* out, size_t maxBytes);

//------------------------------------------------------------------------------------------------//
// THE DOCUMENT, AS READ                                                                          //
//------------------------------------------------------------------------------------------------//

struct Member;

/// ONE JSON VALUE. A number keeps its TEXT, exactly as the file spelled it: the consumer converts it at
/// the width it declares (`asFloat`, `asDouble`, `asU32`), and a value nobody here understands is written
/// back out unchanged (`Writer::value`).
struct Value {
	enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

	Type type = Type::Null;
	bool boolean = false;
	/// A number's raw text, or a string's DECODED UTF-8.
	std::string text;
	std::vector<Value> items;    ///< Array
	std::vector<Member> members; ///< Object, in the file's order; keys are unique (the reader refuses a repeat)

	[[nodiscard]] bool isNull() const noexcept { return type == Type::Null; }
	[[nodiscard]] bool isNumber() const noexcept { return type == Type::Number; }
	[[nodiscard]] bool isString() const noexcept { return type == Type::String; }
	[[nodiscard]] bool isArray() const noexcept { return type == Type::Array; }
	[[nodiscard]] bool isObject() const noexcept { return type == Type::Object; }

	/// The member named `key`, or null — for an object; null for anything else.
	[[nodiscard]] const Value* find(std::string_view key) const noexcept;

	bool asFloat(float* out) const noexcept { return type == Type::Number && parseNumber(text, out); }
	bool asDouble(double* out) const noexcept { return type == Type::Number && parseNumber(text, out); }
	bool asU32(uint32_t* out) const noexcept { return type == Type::Number && parseNumber(text, out); }
	bool asU64(uint64_t* out) const noexcept { return type == Type::Number && parseNumber(text, out); }
	bool asI64(int64_t* out) const noexcept { return type == Type::Number && parseNumber(text, out); }
	bool asBool(bool* out) const noexcept {
		if (type != Type::Bool) {
			return false;
		}
		*out = boolean;
		return true;
	}

	static Value number(std::string rawText);
	static Value string(std::string decoded);
};

struct Member {
	std::string key;
	Value value;
};

/// WHAT A READ MAY COST. A document is a file on somebody's disk, so every dimension a file could inflate
/// is bounded before it is paid for.
struct Limits {
	size_t maxBytes = size_t { 64 } << 20;
	/// Arrays and objects inside one another. Recursion in the reader is bounded by this.
	size_t maxDepth = 64;
	/// Every value counts one, containers included — what stops `[0,0,0,…]` in 64 MiB becoming a
	/// gigabyte of tree.
	size_t maxNodes = size_t { 1 } << 18;
};

/// READ ONE DOCUMENT: exactly one JSON value, surrounded by nothing but whitespace. False for anything
/// else — malformed text, a limit exceeded, a repeated key, invalid UTF-8, a lone surrogate — with
/// `error` (when given) saying where. `*out` is unspecified on false.
bool parse(std::string_view text, Value* out, const Limits& limits = {}, std::string* error = nullptr);

/// THE BYTES AT A `$bin` NODE: `{"$bin": "<base64>", "size": n}`, where the decoded length must equal
/// `size` and neither may pass `maxBytes`. False for any other node.
bool binOf(const Value& node, std::vector<uint8_t>* out, size_t maxBytes);
/// Whether a node is a `$bin` object at all (whatever its contents).
[[nodiscard]] bool isBin(const Value& node) noexcept;

//------------------------------------------------------------------------------------------------//
// THE WRITER                                                                                     //
//------------------------------------------------------------------------------------------------//

/// **ONE WRITER, SO THE OUTPUT IS DETERMINISTIC.** Members come out in the order the caller writes them,
/// indented with TABS, one member per line (a preset's diff in git is one line per changed control), or
/// compact. A caller that writes the same things in the same order gets the same bytes.
///
/// The caller is trusted to nest correctly: a key before every object member, none in an array, every
/// begin matched by an end. `ok()` goes false on a misuse it can see and stays false.
class Writer {
public:
	explicit Writer(std::string* out, bool indented = true) : out_(out), indented_(indented) {}

	void beginObject();
	void endObject();
	void beginArray();
	void endArray();
	void key(std::string_view name);

	void null();
	void boolean(bool v);
	void number(float v);
	void number(double v);
	void number(uint64_t v);
	void number(uint32_t v) { number(static_cast<uint64_t>(v)); }
	void number(int64_t v);
	/// A number exactly as spelled — for one carried through from a file. Refused (and `null` written) if
	/// it is not a JSON number.
	void rawNumber(std::string_view text);
	/// Written as valid UTF-8 whatever it is given: a byte that is not part of a valid sequence becomes
	/// U+FFFD, so this writer's output always reads back.
	void string(std::string_view v);
	/// `{"$bin": "<base64>", "size": n}`.
	void bytes(std::span<const uint8_t> data);
	/// A whole value read by `parse`, re-emitted at this depth — numbers as their original text.
	void value(const Value& v);

	[[nodiscard]] bool ok() const noexcept { return ok_ && frames_.empty(); }

private:
	struct Frame {
		bool object = false;
		size_t count = 0;
		bool keyed = false; ///< an object member's key is written and its value is next
	};

	void beforeValue();
	void newline();
	void close(char c, bool object);

	std::string* out_;
	bool indented_;
	bool ok_ = true;
	bool rootWritten_ = false;
	std::vector<Frame> frames_;
};

/// A value, compact — for a caller holding a DOM fragment as text.
[[nodiscard]] std::string toCompactText(const Value& v);

/// Whether `bytes` are valid UTF-8 (no overlongs, no encoded surrogates, nothing past U+10FFFF).
[[nodiscard]] bool validUtf8(std::string_view bytes) noexcept;

} // namespace feathers::state::json
