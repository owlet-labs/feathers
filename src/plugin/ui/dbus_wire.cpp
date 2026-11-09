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

#include "plugin/ui/dbus_wire.h"

namespace feathers::ui::dbus {

namespace {

/// Header field codes.
enum Field : uint8_t {
	kPath = 1,
	kInterface = 2,
	kMember = 3,
	kErrorName = 4,
	kReplySerial = 5,
	kDestination = 6,
	kSender = 7,
	kSignature = 8,
};

/// The spec's own ceiling on a message, 128 MiB. Anything claiming more is not a message.
constexpr size_t kMaxMessage = size_t { 1 } << 27;
/// Containers nest at most 32 deep each (arrays, structs), so 64 in all.
constexpr int kMaxDepth = 64;

size_t roundUp(size_t v, size_t n) { return (v + n - 1) / n * n; }

/// The length of the ONE complete type at the front of `sig`; 0 when there is not one.
size_t completeTypeLength(std::string_view sig) {
	if (sig.empty()) {
		return 0;
	}
	if (sig[0] == 'a') {
		const size_t inner = completeTypeLength(sig.substr(1));
		return inner == 0 ? 0 : 1 + inner;
	}
	if (sig[0] == '(' || sig[0] == '{') {
		const char close = sig[0] == '(' ? ')' : '}';
		size_t at = 1;
		while (at < sig.size() && sig[at] != close) {
			const size_t inner = completeTypeLength(sig.substr(at));
			if (inner == 0) {
				return 0;
			}
			at += inner;
		}
		return at < sig.size() ? at + 1 : 0;
	}
	return 1;
}

bool skipAt(Reader& r, std::string_view& sig, int depth);

} // namespace

size_t alignmentOf(char code) {
	switch (code) {
	case 'y':
	case 'g':
	case 'v':
		return 1;
	case 'n':
	case 'q':
		return 2;
	case 'x':
	case 't':
	case 'd':
	case '(':
	case '{':
		return 8;
	default:
		return 4; // b i u h s o a
	}
}

//------------------------------------------------------------------------------------------------//
// WRITER                                                                                           //
//------------------------------------------------------------------------------------------------//

void Writer::align(size_t n) { buf_.resize(roundUp(buf_.size(), n), 0); }

void Writer::byte(uint8_t v) { buf_.push_back(v); }

void Writer::u32(uint32_t v) {
	align(4);
	for (int i = 0; i < 4; ++i) {
		buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

void Writer::string(std::string_view s) {
	u32(static_cast<uint32_t>(s.size()));
	buf_.insert(buf_.end(), s.begin(), s.end());
	buf_.push_back(0);
}

void Writer::signature(std::string_view s) {
	byte(static_cast<uint8_t>(s.size()));
	buf_.insert(buf_.end(), s.begin(), s.end());
	buf_.push_back(0);
}

Writer::ArrayMark Writer::beginArray(size_t elementAlign) {
	align(4);
	const size_t lengthAt = buf_.size();
	u32(0);
	align(elementAlign);
	return { lengthAt, buf_.size() };
}

void Writer::endArray(ArrayMark mark) {
	const auto length = static_cast<uint32_t>(buf_.size() - mark.start);
	for (int i = 0; i < 4; ++i) {
		buf_[mark.lengthAt + static_cast<size_t>(i)] = static_cast<uint8_t>(length >> (8 * i));
	}
}

//------------------------------------------------------------------------------------------------//
// READER                                                                                           //
//------------------------------------------------------------------------------------------------//

void Reader::align(size_t n) {
	const size_t to = roundUp(pos_, n);
	if (to > size_) {
		ok_ = false;
		pos_ = size_;
		return;
	}
	pos_ = to;
}

uint8_t Reader::byte() {
	if (pos_ + 1 > size_) {
		ok_ = false;
		return 0;
	}
	return data_[pos_++];
}

uint32_t Reader::u32() {
	align(4);
	if (!ok_ || pos_ + 4 > size_) {
		ok_ = false;
		return 0;
	}
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i) {
		const uint32_t b = data_[pos_ + static_cast<size_t>(i)];
		v |= big_ ? b << (8 * (3 - i)) : b << (8 * i);
	}
	pos_ += 4;
	return v;
}

std::string Reader::string() {
	const uint32_t length = u32();
	// THE TERMINATING NUL IS PART OF THE WIRE FORMAT: a string without one is not a string.
	if (!ok_ || size_ - pos_ < size_t { length } + 1 || data_[pos_ + length] != 0) {
		ok_ = false;
		return {};
	}
	std::string out(reinterpret_cast<const char*>(data_ + pos_), length);
	pos_ += size_t { length } + 1;
	return out;
}

std::string Reader::signature() {
	const uint8_t length = byte();
	if (!ok_ || size_ - pos_ < size_t { length } + 1 || data_[pos_ + length] != 0) {
		ok_ = false;
		return {};
	}
	std::string out(reinterpret_cast<const char*>(data_ + pos_), length);
	pos_ += size_t { length } + 1;
	return out;
}

size_t Reader::beginArray(size_t elementAlign) {
	const uint32_t length = u32();
	align(elementAlign);
	if (!ok_ || size_ - pos_ < length) {
		ok_ = false;
		return pos_;
	}
	return pos_ + length;
}

bool Reader::skip(std::string_view& sig) { return skipAt(*this, sig, 0); }

namespace {

bool skipAt(Reader& r, std::string_view& sig, int depth) {
	if (sig.empty() || depth > kMaxDepth) {
		return false;
	}
	const char code = sig[0];
	switch (code) {
	case 'y':
		(void)r.byte();
		sig.remove_prefix(1);
		break;
	case 'n':
	case 'q':
		r.align(2);
		(void)r.byte();
		(void)r.byte();
		sig.remove_prefix(1);
		break;
	case 'b':
	case 'i':
	case 'u':
	case 'h':
		(void)r.u32();
		sig.remove_prefix(1);
		break;
	case 'x':
	case 't':
	case 'd':
		(void)r.u32(); // an 8-aligned pair of u32s is the same bytes
		(void)r.u32();
		sig.remove_prefix(1);
		break;
	case 's':
	case 'o':
		(void)r.string();
		sig.remove_prefix(1);
		break;
	case 'g':
		(void)r.signature();
		sig.remove_prefix(1);
		break;
	case 'v': {
		const std::string inner = r.signature();
		std::string_view view = inner;
		if (!r.ok() || !skipAt(r, view, depth + 1) || !view.empty()) {
			return false;
		}
		sig.remove_prefix(1);
		break;
	}
	case 'a': {
		const size_t element = completeTypeLength(sig.substr(1));
		if (element == 0) {
			return false;
		}
		const size_t end = r.beginArray(alignmentOf(sig[1]));
		// The elements are not looked at, only stepped over: the length says where they end.
		while (r.ok() && r.pos() < end) {
			(void)r.byte();
		}
		sig.remove_prefix(1 + element);
		break;
	}
	case '(':
	case '{': {
		const char close = code == '(' ? ')' : '}';
		r.align(8);
		sig.remove_prefix(1);
		while (!sig.empty() && sig[0] != close) {
			if (!skipAt(r, sig, depth + 1)) {
				return false;
			}
		}
		if (sig.empty()) {
			return false;
		}
		sig.remove_prefix(1);
		break;
	}
	default:
		return false;
	}
	return r.ok();
}

void headerField(Writer& w, Field code, char type, std::string_view value) {
	w.beginStruct();
	w.byte(code);
	const char sig[2] = { type, 0 };
	w.variantSignature(sig);
	if (type == 'g') {
		w.signature(value);
	} else {
		w.string(value);
	}
}

} // namespace

//------------------------------------------------------------------------------------------------//
// MESSAGES                                                                                         //
//------------------------------------------------------------------------------------------------//

std::vector<uint8_t> encode(const Message& m) {
	Writer w;
	w.byte('l');
	w.byte(static_cast<uint8_t>(m.type));
	w.byte(m.flags);
	w.byte(1); // protocol version
	w.u32(static_cast<uint32_t>(m.body.size()));
	w.u32(m.serial);
	const Writer::ArrayMark fields = w.beginArray(8);
	if (!m.path.empty()) {
		headerField(w, kPath, 'o', m.path);
	}
	if (!m.interface.empty()) {
		headerField(w, kInterface, 's', m.interface);
	}
	if (!m.member.empty()) {
		headerField(w, kMember, 's', m.member);
	}
	if (!m.errorName.empty()) {
		headerField(w, kErrorName, 's', m.errorName);
	}
	if (m.replySerial != 0) {
		w.beginStruct();
		w.byte(kReplySerial);
		w.variantSignature("u");
		w.u32(m.replySerial);
	}
	if (!m.destination.empty()) {
		headerField(w, kDestination, 's', m.destination);
	}
	if (!m.sender.empty()) {
		headerField(w, kSender, 's', m.sender);
	}
	if (!m.signature.empty()) {
		headerField(w, kSignature, 'g', m.signature);
	}
	w.endArray(fields);
	w.align(8);
	std::vector<uint8_t> out = w.take();
	out.insert(out.end(), m.body.begin(), m.body.end());
	return out;
}

size_t messageLength(const uint8_t* data, size_t size, bool* malformed) {
	if (malformed != nullptr) {
		*malformed = false;
	}
	if (size < 16) {
		return 0;
	}
	const bool big = data[0] == 'B';
	if ((data[0] != 'l' && !big) || data[3] != 1) {
		if (malformed != nullptr) {
			*malformed = true;
		}
		return 0;
	}
	Reader r(data, 16, big);
	(void)r.u32();
	const size_t body = r.u32();
	(void)r.u32();
	const size_t fields = r.u32();
	if (body > kMaxMessage || fields > kMaxMessage) {
		if (malformed != nullptr) {
			*malformed = true;
		}
		return 0;
	}
	const size_t total = roundUp(16 + fields, 8) + body;
	if (total > kMaxMessage) {
		if (malformed != nullptr) {
			*malformed = true;
		}
		return 0;
	}
	return total;
}

bool decode(const uint8_t* data, size_t size, Message& out) {
	bool malformed = false;
	if (messageLength(data, size, &malformed) != size || malformed) {
		return false;
	}
	out = Message {};
	out.bigEndian = data[0] == 'B';
	Reader r(data, size, out.bigEndian);
	(void)r.byte();
	out.type = static_cast<Type>(r.byte());
	out.flags = r.byte();
	(void)r.byte();
	const uint32_t bodyLength = r.u32();
	out.serial = r.u32();
	const size_t fieldsEnd = r.beginArray(8);
	while (r.ok() && r.pos() < fieldsEnd) {
		r.align(8);
		const uint8_t code = r.byte();
		const std::string type = r.signature();
		if (type == "s" || type == "o") {
			std::string value = r.string();
			switch (code) {
			case kPath: out.path = std::move(value); break;
			case kInterface: out.interface = std::move(value); break;
			case kMember: out.member = std::move(value); break;
			case kErrorName: out.errorName = std::move(value); break;
			case kDestination: out.destination = std::move(value); break;
			case kSender: out.sender = std::move(value); break;
			default: break;
			}
		} else if (type == "u") {
			const uint32_t value = r.u32();
			if (code == kReplySerial) {
				out.replySerial = value;
			}
		} else if (type == "g") {
			std::string value = r.signature();
			if (code == kSignature) {
				out.signature = std::move(value);
			}
		} else {
			// A FIELD THIS CODEC HAS NO USE FOR (UNIX_FDS, or one a later spec adds): stepped over.
			std::string_view sig = type;
			if (!r.skip(sig) || !sig.empty()) {
				return false;
			}
		}
	}
	r.align(8);
	if (!r.ok() || out.type == Type::Invalid || size - r.pos() != bodyLength) {
		return false;
	}
	out.body.assign(data + r.pos(), data + size);
	return true;
}

} // namespace feathers::ui::dbus
