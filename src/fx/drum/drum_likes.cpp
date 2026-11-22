/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fx/drum/drum_likes.h"

#include <cmath>

namespace feathers::fx::drumdevice {

std::array<float, 7> fieldsArray(const feathers::drum::Fields& f) noexcept {
	return { f.pitch, f.length, f.sweep, f.tone, f.noise, f.snap, f.metal };
}

feathers::drum::Fields fieldsOfArray(const std::array<float, 7>& a) noexcept {
	feathers::drum::Fields f;
	f.pitch = a[0];
	f.length = a[1];
	f.sweep = a[2];
	f.tone = a[3];
	f.noise = a[4];
	f.snap = a[5];
	f.metal = a[6];
	return f;
}

void writeLikeBlock(const std::vector<DrumLike>& likes, float* out) noexcept {
	device::BlockWriter w(out, kLikesBlockWords);
	const size_t n = likes.size() < kMaxDrumLikes ? likes.size() : kMaxDrumLikes;
	w.u16(static_cast<uint32_t>(n));
	for (size_t i = 0; i < kMaxDrumLikes; ++i) {
		const DrumLike l = i < n ? likes[i] : DrumLike {};
		w.f64(l.x);
		w.f64(l.y);
		w.u16(l.zone);
		w.f32(l.offset);
		w.f32(l.gamma);
		w.f32(l.rotate);
		w.u16(l.kind);
		for (const float f : l.fields) {
			w.f32(f);
		}
	}
}

bool readLikeBlock(const float* in, size_t n, std::vector<DrumLike>* out) {
	if (n != kLikesBlockWords) {
		return false;
	}
	device::BlockReader r(in, n);
	const uint32_t count = r.u16();
	std::vector<DrumLike> got;
	bool sane = count <= kMaxDrumLikes;
	for (size_t i = 0; i < kMaxDrumLikes; ++i) {
		DrumLike l;
		l.x = r.f64();
		l.y = r.f64();
		l.zone = static_cast<uint16_t>(r.u16());
		l.offset = r.f32();
		l.gamma = r.f32();
		l.rotate = r.f32();
		l.kind = static_cast<uint16_t>(r.u16());
		for (float& f : l.fields) {
			f = r.f32();
		}
		if (i < count) {
			bool ok = std::isfinite(l.x) && std::isfinite(l.y) && std::isfinite(l.offset) && std::isfinite(l.gamma)
			          && std::isfinite(l.rotate) && l.zone <= 1023 && l.kind < feathers::drum::kKindCount;
			for (const float f : l.fields) {
				ok = ok && std::isfinite(f) && f >= 0.0f && f <= 1.0f;
			}
			sane = sane && ok;
			got.push_back(l);
		}
	}
	if (!r.ok() || !sane) {
		return false;
	}
	if (out != nullptr) {
		*out = std::move(got);
	}
	return true;
}

} // namespace feathers::fx::drumdevice
