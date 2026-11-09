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

#include "state/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

#if defined(__APPLE__)
#include <cerrno>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <xlocale.h>
#endif

namespace feathers::state::json {

namespace {

//------------------------------------------------------------------------------------------------//
// THE NUMBER GRAMMAR — RFC 8259 §6, checked before `from_chars` sees the text                     //
//------------------------------------------------------------------------------------------------//

bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

/// How long the JSON number at the front of `s` is, or 0 if it does not start with one. `integerOnly`
/// refuses a fraction or an exponent.
size_t numberLength(std::string_view s, bool integerOnly) noexcept {
	size_t i = 0;
	if (i < s.size() && s[i] == '-') {
		++i;
	}
	if (i >= s.size()) {
		return 0;
	}
	if (s[i] == '0') {
		++i;
	}
	else if (s[i] >= '1' && s[i] <= '9') {
		while (i < s.size() && isDigit(s[i])) {
			++i;
		}
	}
	else {
		return 0;
	}
	if (i < s.size() && s[i] == '.') {
		if (integerOnly) {
			return 0;
		}
		++i;
		if (i >= s.size() || !isDigit(s[i])) {
			return 0;
		}
		while (i < s.size() && isDigit(s[i])) {
			++i;
		}
	}
	if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
		if (integerOnly) {
			return 0;
		}
		++i;
		if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
			++i;
		}
		if (i >= s.size() || !isDigit(s[i])) {
			return 0;
		}
		while (i < s.size() && isDigit(s[i])) {
			++i;
		}
	}
	return i;
}

bool wholeNumber(std::string_view s, bool integerOnly) noexcept {
	return !s.empty() && numberLength(s, integerOnly) == s.size();
}

template <typename T>
bool fromChars(std::string_view s, bool integerOnly, T* out) noexcept {
	if (!wholeNumber(s, integerOnly)) {
		return false;
	}
	T v {};
	const char* first = s.data();
	const char* last = s.data() + s.size();
	std::from_chars_result r {};
	if constexpr (std::is_floating_point_v<T>) {
#if defined(__APPLE__)
		// libc++ ships floating-point `from_chars` only from macOS 26 (CI's macOS 15 target refuses it), so
		// Apple parses with strtod_l/strtof_l under a C locale: a host's LC_NUMERIC can never make "0.5" read as
		// "0,5". `wholeNumber` has already checked the JSON number grammar, so the whole string must be taken.
		char buf[128];
		if (s.size() >= sizeof(buf)) {
			return false;
		}
		std::memcpy(buf, first, s.size());
		buf[s.size()] = '\0';
		static const locale_t cLocale = newlocale(LC_ALL_MASK, "C", nullptr);
		char* end = nullptr;
		errno = 0;
		if constexpr (std::is_same_v<T, float>) {
			v = strtof_l(buf, &end, cLocale);
		}
		else {
			v = static_cast<T>(strtod_l(buf, &end, cLocale));
		}
		if (end != buf + s.size() || (errno == ERANGE && !std::isfinite(v))) {
			return false;
		}
		*out = v;
		return true;
#else
		r = std::from_chars(first, last, v, std::chars_format::general);
#endif
	}
	else {
		r = std::from_chars(first, last, v);
	}
	if (r.ec != std::errc {} || r.ptr != last) {
		return false;
	}
	*out = v;
	return true;
}

template <typename T>
void toChars(std::string* out, T v) {
	char buf[64];
	const std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
	out->append(buf, r.ptr);
}

//------------------------------------------------------------------------------------------------//
// UTF-8                                                                                          //
//------------------------------------------------------------------------------------------------//

/// The length of the valid UTF-8 sequence at the front of `s`, or 0 if there is none. Refuses overlong
/// forms, encoded surrogates and anything past U+10FFFF (RFC 3629 table 3-7).
size_t utf8Sequence(std::string_view s) noexcept {
	if (s.empty()) {
		return 0;
	}
	const auto b = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
	const unsigned char c = b(0);
	if (c < 0x80) {
		return 1;
	}
	const auto cont = [&](size_t i) { return i < s.size() && (b(i) & 0xC0) == 0x80; };
	if (c >= 0xC2 && c <= 0xDF) {
		return cont(1) ? 2 : 0;
	}
	if (c >= 0xE0 && c <= 0xEF) {
		if (s.size() < 3 || !cont(1) || !cont(2)) {
			return 0;
		}
		if (c == 0xE0 && b(1) < 0xA0) {
			return 0; // overlong
		}
		if (c == 0xED && b(1) > 0x9F) {
			return 0; // a surrogate, encoded
		}
		return 3;
	}
	if (c >= 0xF0 && c <= 0xF4) {
		if (s.size() < 4 || !cont(1) || !cont(2) || !cont(3)) {
			return 0;
		}
		if (c == 0xF0 && b(1) < 0x90) {
			return 0; // overlong
		}
		if (c == 0xF4 && b(1) > 0x8F) {
			return 0; // past U+10FFFF
		}
		return 4;
	}
	return 0;
}

void appendUtf8(std::string* out, uint32_t cp) {
	if (cp < 0x80) {
		out->push_back(static_cast<char>(cp));
	}
	else if (cp < 0x800) {
		out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
		out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	else if (cp < 0x10000) {
		out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
		out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
	else {
		out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
		out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
		out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
	}
}

void appendQuoted(std::string* out, std::string_view v) {
	static constexpr char kHex[] = "0123456789abcdef";
	out->push_back('"');
	size_t i = 0;
	while (i < v.size()) {
		const auto c = static_cast<unsigned char>(v[i]);
		if (c == '"' || c == '\\') {
			out->push_back('\\');
			out->push_back(static_cast<char>(c));
			++i;
		}
		else if (c < 0x20) {
			switch (c) {
			case '\b': out->append("\\b"); break;
			case '\f': out->append("\\f"); break;
			case '\n': out->append("\\n"); break;
			case '\r': out->append("\\r"); break;
			case '\t': out->append("\\t"); break;
			default:
				out->append("\\u00");
				out->push_back(kHex[c >> 4]);
				out->push_back(kHex[c & 0xF]);
				break;
			}
			++i;
		}
		else if (c < 0x80) {
			out->push_back(static_cast<char>(c));
			++i;
		}
		else {
			const size_t n = utf8Sequence(v.substr(i));
			if (n == 0) {
				out->append("\\ufffd"); // not UTF-8: the replacement character, so the output reads back
				++i;
			}
			else {
				out->append(v.substr(i, n));
				i += n;
			}
		}
	}
	out->push_back('"');
}

//------------------------------------------------------------------------------------------------//
// BASE64                                                                                         //
//------------------------------------------------------------------------------------------------//

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Digit(char c) noexcept {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}
	return -1;
}

//------------------------------------------------------------------------------------------------//
// THE READER                                                                                     //
//------------------------------------------------------------------------------------------------//

class Parser {
public:
	Parser(std::string_view text, const Limits& limits, std::string* error)
	    : s_(text), limits_(limits), error_(error) {}

	bool document(Value* out) {
		if (s_.size() > limits_.maxBytes) {
			return fail("the document is larger than the limit");
		}
		skipSpace();
		if (!value(out, 0)) {
			return false;
		}
		skipSpace();
		if (i_ != s_.size()) {
			return fail("text after the value");
		}
		return true;
	}

private:
	bool fail(const char* why) {
		if (error_ != nullptr && error_->empty()) {
			*error_ = "at byte " + std::to_string(i_) + ": " + why;
		}
		return false;
	}

	void skipSpace() noexcept {
		while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
			++i_;
		}
	}

	bool literal(std::string_view word) noexcept {
		if (s_.substr(i_, word.size()) != word) {
			return false;
		}
		i_ += word.size();
		return true;
	}

	bool value(Value* out, size_t depth) {
		if (++nodes_ > limits_.maxNodes) {
			return fail("more values than the limit");
		}
		if (i_ >= s_.size()) {
			return fail("a value was expected");
		}
		*out = Value {};
		const char c = s_[i_];
		switch (c) {
		case '{': return object(out, depth + 1);
		case '[': return array(out, depth + 1);
		case '"':
			out->type = Value::Type::String;
			return string(&out->text);
		case 't':
			out->type = Value::Type::Bool;
			out->boolean = true;
			return literal("true") || fail("not a literal");
		case 'f':
			out->type = Value::Type::Bool;
			return literal("false") || fail("not a literal");
		case 'n':
			out->type = Value::Type::Null;
			return literal("null") || fail("not a literal");
		default: break;
		}
		const size_t n = numberLength(s_.substr(i_), false);
		if (n == 0) {
			return fail("not a value");
		}
		out->type = Value::Type::Number;
		out->text.assign(s_.substr(i_, n));
		i_ += n;
		return true;
	}

	bool object(Value* out, size_t depth) {
		if (depth > limits_.maxDepth) {
			return fail("nested deeper than the limit");
		}
		out->type = Value::Type::Object;
		++i_; // '{'
		skipSpace();
		if (i_ < s_.size() && s_[i_] == '}') {
			++i_;
			return true;
		}
		for (;;) {
			skipSpace();
			if (i_ >= s_.size() || s_[i_] != '"') {
				return fail("a key was expected");
			}
			Member m;
			if (!string(&m.key)) {
				return false;
			}
			skipSpace();
			if (i_ >= s_.size() || s_[i_] != ':') {
				return fail("':' was expected");
			}
			++i_;
			skipSpace();
			if (!value(&m.value, depth)) {
				return false;
			}
			out->members.push_back(std::move(m));
			skipSpace();
			if (i_ < s_.size() && s_[i_] == ',') {
				++i_;
				continue;
			}
			if (i_ < s_.size() && s_[i_] == '}') {
				++i_;
				break;
			}
			return fail("',' or '}' was expected");
		}
		return uniqueKeys(*out) || fail("a key is repeated");
	}

	/// **A REPEATED KEY IS REFUSED, NOT RESOLVED.** Which of two values a reader keeps is exactly the
	/// kind of choice two readers make differently. Sorted rather than compared pairwise, so an object
	/// with a hundred thousand members costs a sort and not five billion comparisons.
	static bool uniqueKeys(const Value& v) {
		const size_t n = v.members.size();
		if (n < 2) {
			return true;
		}
		std::vector<const std::string*> keys;
		keys.reserve(n);
		for (const Member& m : v.members) {
			keys.push_back(&m.key);
		}
		std::sort(keys.begin(), keys.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
		for (size_t k = 1; k < n; ++k) {
			if (*keys[k] == *keys[k - 1]) {
				return false;
			}
		}
		return true;
	}

	bool array(Value* out, size_t depth) {
		if (depth > limits_.maxDepth) {
			return fail("nested deeper than the limit");
		}
		out->type = Value::Type::Array;
		++i_; // '['
		skipSpace();
		if (i_ < s_.size() && s_[i_] == ']') {
			++i_;
			return true;
		}
		for (;;) {
			skipSpace();
			Value item;
			if (!value(&item, depth)) {
				return false;
			}
			out->items.push_back(std::move(item));
			skipSpace();
			if (i_ < s_.size() && s_[i_] == ',') {
				++i_;
				continue;
			}
			if (i_ < s_.size() && s_[i_] == ']') {
				++i_;
				return true;
			}
			return fail("',' or ']' was expected");
		}
	}

	bool hex4(uint32_t* out) {
		if (s_.size() - i_ < 4) {
			return fail("a \\u escape is short");
		}
		uint32_t v = 0;
		for (size_t k = 0; k < 4; ++k) {
			const char c = s_[i_ + k];
			v <<= 4;
			if (c >= '0' && c <= '9') {
				v |= static_cast<uint32_t>(c - '0');
			}
			else if (c >= 'a' && c <= 'f') {
				v |= static_cast<uint32_t>(c - 'a' + 10);
			}
			else if (c >= 'A' && c <= 'F') {
				v |= static_cast<uint32_t>(c - 'A' + 10);
			}
			else {
				return fail("a \\u escape is not hexadecimal");
			}
		}
		i_ += 4;
		*out = v;
		return true;
	}

	bool string(std::string* out) {
		++i_; // '"'
		out->clear();
		while (i_ < s_.size()) {
			const auto c = static_cast<unsigned char>(s_[i_]);
			if (c == '"') {
				++i_;
				return true;
			}
			if (c < 0x20) {
				return fail("a control character inside a string");
			}
			if (c == '\\') {
				++i_;
				if (i_ >= s_.size()) {
					break;
				}
				const char e = s_[i_++];
				switch (e) {
				case '"': out->push_back('"'); break;
				case '\\': out->push_back('\\'); break;
				case '/': out->push_back('/'); break;
				case 'b': out->push_back('\b'); break;
				case 'f': out->push_back('\f'); break;
				case 'n': out->push_back('\n'); break;
				case 'r': out->push_back('\r'); break;
				case 't': out->push_back('\t'); break;
				case 'u': {
					uint32_t cp = 0;
					if (!hex4(&cp)) {
						return false;
					}
					if (cp >= 0xDC00 && cp <= 0xDFFF) {
						return fail("a low surrogate with no high one");
					}
					if (cp >= 0xD800 && cp <= 0xDBFF) {
						uint32_t low = 0;
						if (s_.substr(i_, 2) != "\\u") {
							return fail("a high surrogate with no low one");
						}
						i_ += 2;
						if (!hex4(&low)) {
							return false;
						}
						if (low < 0xDC00 || low > 0xDFFF) {
							return fail("a high surrogate with no low one");
						}
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					}
					appendUtf8(out, cp);
					break;
				}
				default: return fail("an unknown escape");
				}
				continue;
			}
			const size_t n = utf8Sequence(s_.substr(i_));
			if (n == 0) {
				return fail("invalid UTF-8");
			}
			out->append(s_.substr(i_, n));
			i_ += n;
		}
		return fail("an unterminated string");
	}

	std::string_view s_;
	const Limits& limits_;
	std::string* error_;
	size_t i_ = 0;
	size_t nodes_ = 0;
};

} // namespace

//------------------------------------------------------------------------------------------------//
// NUMBERS                                                                                        //
//------------------------------------------------------------------------------------------------//

void appendNumber(std::string* out, float v) {
	if (!std::isfinite(v)) {
		out->append("null");
		return;
	}
	toChars(out, v);
}

void appendNumber(std::string* out, double v) {
	if (!std::isfinite(v)) {
		out->append("null");
		return;
	}
	toChars(out, v);
}

void appendNumber(std::string* out, uint64_t v) { toChars(out, v); }
void appendNumber(std::string* out, int64_t v) { toChars(out, v); }

bool parseNumber(std::string_view text, float* out) { return fromChars(text, false, out); }
bool parseNumber(std::string_view text, double* out) { return fromChars(text, false, out); }
bool parseNumber(std::string_view text, uint32_t* out) {
	return (text.empty() || text[0] != '-') && fromChars(text, true, out);
}
bool parseNumber(std::string_view text, uint64_t* out) {
	return (text.empty() || text[0] != '-') && fromChars(text, true, out);
}
bool parseNumber(std::string_view text, int64_t* out) { return fromChars(text, true, out); }

//------------------------------------------------------------------------------------------------//
// BASE64                                                                                         //
//------------------------------------------------------------------------------------------------//

void appendBase64(std::string* out, std::span<const uint8_t> bytes) {
	out->reserve(out->size() + (bytes.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 3 <= bytes.size(); i += 3) {
		const uint32_t v = (uint32_t { bytes[i] } << 16) | (uint32_t { bytes[i + 1] } << 8) | bytes[i + 2];
		out->push_back(kBase64[(v >> 18) & 63]);
		out->push_back(kBase64[(v >> 12) & 63]);
		out->push_back(kBase64[(v >> 6) & 63]);
		out->push_back(kBase64[v & 63]);
	}
	const size_t rest = bytes.size() - i;
	if (rest == 1) {
		const uint32_t v = uint32_t { bytes[i] } << 16;
		out->push_back(kBase64[(v >> 18) & 63]);
		out->push_back(kBase64[(v >> 12) & 63]);
		out->append("==");
	}
	else if (rest == 2) {
		const uint32_t v = (uint32_t { bytes[i] } << 16) | (uint32_t { bytes[i + 1] } << 8);
		out->push_back(kBase64[(v >> 18) & 63]);
		out->push_back(kBase64[(v >> 12) & 63]);
		out->push_back(kBase64[(v >> 6) & 63]);
		out->push_back('=');
	}
}

bool decodeBase64(std::string_view text, std::vector<uint8_t>* out, size_t maxBytes) {
	out->clear();
	if (text.size() % 4 != 0) {
		return false;
	}
	size_t pad = 0;
	if (!text.empty() && text.back() == '=') {
		pad = (text.size() >= 2 && text[text.size() - 2] == '=') ? 2 : 1;
	}
	const size_t bytes = text.size() / 4 * 3 - pad;
	if (bytes > maxBytes) {
		return false;
	}
	out->reserve(bytes);
	for (size_t i = 0; i < text.size(); i += 4) {
		const bool last = i + 4 == text.size();
		int d[4];
		for (size_t k = 0; k < 4; ++k) {
			const bool padded = last && k >= 4 - pad;
			d[k] = padded ? 0 : base64Digit(text[i + k]);
			if (d[k] < 0 || (padded && text[i + k] != '=')) {
				return false;
			}
		}
		const uint32_t v = (static_cast<uint32_t>(d[0]) << 18) | (static_cast<uint32_t>(d[1]) << 12)
		                   | (static_cast<uint32_t>(d[2]) << 6) | static_cast<uint32_t>(d[3]);
		out->push_back(static_cast<uint8_t>(v >> 16));
		if (!(last && pad == 2)) {
			out->push_back(static_cast<uint8_t>(v >> 8));
		}
		if (!(last && pad >= 1)) {
			out->push_back(static_cast<uint8_t>(v));
		}
		// CANONICAL ONLY: the bits a padded quantum does not use must be zero, so one byte string has one
		// spelling and a save of what was loaded is the same text.
		if (last && ((pad == 2 && (v & 0xFFFF) != 0) || (pad == 1 && (v & 0xFF) != 0))) {
			return false;
		}
	}
	return true;
}

//------------------------------------------------------------------------------------------------//
// THE DOCUMENT                                                                                   //
//------------------------------------------------------------------------------------------------//

const Value* Value::find(std::string_view key) const noexcept {
	if (type != Type::Object) {
		return nullptr;
	}
	for (const Member& m : members) {
		if (m.key == key) {
			return &m.value;
		}
	}
	return nullptr;
}

Value Value::number(std::string rawText) {
	Value v;
	v.type = Type::Number;
	v.text = std::move(rawText);
	return v;
}

Value Value::string(std::string decoded) {
	Value v;
	v.type = Type::String;
	v.text = std::move(decoded);
	return v;
}

bool parse(std::string_view text, Value* out, const Limits& limits, std::string* error) {
	if (out == nullptr) {
		return false;
	}
	if (error != nullptr) {
		error->clear();
	}
	Parser parser(text, limits, error);
	return parser.document(out);
}

bool isBin(const Value& node) noexcept {
	return node.isObject() && node.find("$bin") != nullptr;
}

bool binOf(const Value& node, std::vector<uint8_t>* out, size_t maxBytes) {
	out->clear();
	const Value* b64 = node.find("$bin");
	const Value* size = node.find("size");
	uint64_t declared = 0;
	if (b64 == nullptr || !b64->isString() || size == nullptr || !size->asU64(&declared) || declared > maxBytes) {
		return false;
	}
	if (!decodeBase64(b64->text, out, static_cast<size_t>(declared)) || out->size() != declared) {
		out->clear();
		return false;
	}
	return true;
}

bool validUtf8(std::string_view bytes) noexcept {
	size_t i = 0;
	while (i < bytes.size()) {
		const size_t n = utf8Sequence(bytes.substr(i));
		if (n == 0) {
			return false;
		}
		i += n;
	}
	return true;
}

//------------------------------------------------------------------------------------------------//
// THE WRITER                                                                                     //
//------------------------------------------------------------------------------------------------//

void Writer::newline() {
	if (!indented_) {
		return;
	}
	out_->push_back('\n');
	out_->append(frames_.size(), '\t');
}

void Writer::beforeValue() {
	if (frames_.empty()) {
		if (rootWritten_) {
			ok_ = false; // a second top-level value
		}
		rootWritten_ = true;
		return;
	}
	Frame& top = frames_.back();
	if (top.object) {
		if (!top.keyed) {
			ok_ = false; // an object member with no key
		}
		top.keyed = false;
		return;
	}
	if (top.count > 0) {
		out_->push_back(',');
	}
	newline();
	++top.count;
}

void Writer::key(std::string_view name) {
	if (frames_.empty() || !frames_.back().object || frames_.back().keyed) {
		ok_ = false;
		return;
	}
	Frame& top = frames_.back();
	if (top.count > 0) {
		out_->push_back(',');
	}
	newline();
	appendQuoted(out_, name);
	out_->append(indented_ ? ": " : ":");
	top.keyed = true;
	++top.count;
}

void Writer::close(char c, bool object) {
	if (frames_.empty() || frames_.back().object != object || frames_.back().keyed) {
		ok_ = false;
		return;
	}
	const bool any = frames_.back().count > 0;
	frames_.pop_back();
	if (any) {
		newline();
	}
	out_->push_back(c);
}

void Writer::beginObject() {
	beforeValue();
	out_->push_back('{');
	frames_.push_back({ true, 0, false });
}

void Writer::endObject() { close('}', true); }

void Writer::beginArray() {
	beforeValue();
	out_->push_back('[');
	frames_.push_back({ false, 0, false });
}

void Writer::endArray() { close(']', false); }

void Writer::null() {
	beforeValue();
	out_->append("null");
}

void Writer::boolean(bool v) {
	beforeValue();
	out_->append(v ? "true" : "false");
}

void Writer::number(float v) {
	beforeValue();
	appendNumber(out_, v);
}

void Writer::number(double v) {
	beforeValue();
	appendNumber(out_, v);
}

void Writer::number(uint64_t v) {
	beforeValue();
	appendNumber(out_, v);
}

void Writer::number(int64_t v) {
	beforeValue();
	appendNumber(out_, v);
}

void Writer::rawNumber(std::string_view text) {
	beforeValue();
	if (wholeNumber(text, false)) {
		out_->append(text);
	}
	else {
		out_->append("null");
	}
}

void Writer::string(std::string_view v) {
	beforeValue();
	appendQuoted(out_, v);
}

void Writer::bytes(std::span<const uint8_t> data) {
	beginObject();
	key("$bin");
	beforeValue();
	out_->push_back('"');
	appendBase64(out_, data);
	out_->push_back('"');
	key("size");
	number(static_cast<uint64_t>(data.size()));
	endObject();
}

void Writer::value(const Value& v) {
	switch (v.type) {
	case Value::Type::Null: null(); break;
	case Value::Type::Bool: boolean(v.boolean); break;
	case Value::Type::Number: rawNumber(v.text); break;
	case Value::Type::String: string(v.text); break;
	case Value::Type::Array:
		beginArray();
		for (const Value& item : v.items) {
			value(item);
		}
		endArray();
		break;
	case Value::Type::Object:
		beginObject();
		for (const Member& m : v.members) {
			key(m.key);
			value(m.value);
		}
		endObject();
		break;
	}
}

std::string toCompactText(const Value& v) {
	std::string out;
	Writer w(&out, false);
	w.value(v);
	return out;
}

} // namespace feathers::state::json
