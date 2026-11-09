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

#include "fx/conv/ir_ref.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace feathers::fx::conv {

namespace {

void putU32(std::vector<uint8_t>& out, uint32_t v) {
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

uint32_t getU32(const uint8_t* p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16)
	       | (static_cast<uint32_t>(p[3]) << 24);
}

void putF32(std::vector<uint8_t>& out, float f) {
	uint32_t v = 0;
	std::memcpy(&v, &f, 4);
	putU32(out, v);
}

float getF32(const uint8_t* p) {
	const uint32_t v = getU32(p);
	float f = 0.0f;
	std::memcpy(&f, &v, 4);
	return f;
}

} // namespace

std::vector<uint8_t> encodeIrRef(const IrRef& ref) {
	const bool file = ref.kind == IrRef::Kind::File;
	const size_t pathBytes = file ? std::min(ref.path.size(), kIrRefMaxPathBytes) : 0;
	std::vector<uint8_t> out { 'F', 'I', 'R', '1', kIrRefVersion, static_cast<uint8_t>(file ? 1 : 0), 0, 0 };
	putU32(out, static_cast<uint32_t>(kIrShapeBytes + pathBytes));
	const IrShape& s = ref.shape;
	putF32(out, s.begin);
	putF32(out, s.end);
	putF32(out, s.stretch);
	putF32(out, s.envStart);
	putU32(out, s.envCount);
	for (const auto& seg : s.envSegment) {
		for (const float v : seg) {
			putF32(out, v);
		}
	}
	putU32(out, (s.lite ? 1u : 0u) | (ref.kind == IrRef::Kind::Generated ? static_cast<uint32_t>(ref.generated) << 8 : 0u));
	out.insert(out.end(), ref.path.begin(), ref.path.begin() + static_cast<std::ptrdiff_t>(pathBytes));
	return out;
}

bool decodeIrRef(const uint8_t* data, size_t bytes, IrRef& out) {
	if (data == nullptr || bytes < kIrRefHeaderBytes) {
		return false;
	}
	if (data[0] != 'F' || data[1] != 'I' || data[2] != 'R' || data[3] != '1' || data[4] != kIrRefVersion) {
		return false;
	}
	const uint32_t n = getU32(data + 8);
	if (n < kIrShapeBytes || n > kIrShapeBytes + kIrRefMaxPathBytes || bytes != kIrRefHeaderBytes + n) {
		return false;
	}
	const uint8_t kind = data[5];
	if (kind > 1) {
		return false; // 2, EMBEDDED, is the escape hatch: a later build's, refused here
	}
	const size_t pathBytes = n - kIrShapeBytes;
	if ((kind == 0) != (pathBytes == 0)) {
		return false; // a room with a name, or a file without one
	}
	const uint8_t* p = data + kIrRefHeaderBytes;
	IrShape s;
	s.begin = getF32(p);
	s.end = getF32(p + 4);
	s.stretch = getF32(p + 8);
	s.envStart = getF32(p + 12);
	s.envCount = getU32(p + 16);
	p += 20;
	for (auto& seg : s.envSegment) {
		for (float& v : seg) {
			v = getF32(p);
			p += 4;
		}
	}
	const uint32_t flags = getU32(p);
	p += 4;
	if ((flags & ~0xFF01u) != 0) {
		return false; // a flag this build does not know
	}
	s.lite = (flags & 1u) != 0;
	const uint32_t generated = (flags >> 8) & 0xFFu;
	if (generated >= kIrGeneratedRooms || (kind == 1 && generated != 0)) {
		return false; // a room this build does not make, or a file claiming to be one
	}
	const auto finite01 = [](float v) { return std::isfinite(v) && v >= 0.0f && v <= 1.0f; };
	if (!finite01(s.begin) || !finite01(s.end) || !(s.begin < s.end)) {
		return false;
	}
	if (!std::isfinite(s.stretch) || s.stretch < kIrStretchMin || s.stretch > kIrStretchMax) {
		return false;
	}
	if (!finite01(s.envStart) || s.envCount < 1 || s.envCount > kIrEnvSegments) {
		return false;
	}
	for (const auto& seg : s.envSegment) {
		if (!std::isfinite(seg[0]) || !finite01(seg[1]) || !std::isfinite(seg[2]) || std::fabs(seg[2]) > 1.0f) {
			return false;
		}
	}
	out.kind = kind == 1 ? IrRef::Kind::File : IrRef::Kind::Generated;
	out.path.assign(reinterpret_cast<const char*>(p), pathBytes);
	out.shape = s;
	out.generated = static_cast<uint8_t>(generated);
	return true;
}

std::string irDisplayName(const std::string& path) {
	const size_t cut = path.find_last_of("/\\");
	return cut == std::string::npos ? path : path.substr(cut + 1);
}

} // namespace feathers::fx::conv
