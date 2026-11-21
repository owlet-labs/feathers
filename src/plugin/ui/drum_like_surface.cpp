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

#include "plugin/ui/drum_like_surface.h"

#include "fx/drum/drum.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {

/// The seven scalers, in `Fields`' order, as the row names them.
constexpr std::array<dd::Param, 7> kScalers = { dd::Param::FieldPitch, dd::Param::FieldLength, dd::Param::FieldSweep, dd::Param::FieldTone,
	                                              dd::Param::FieldNoise,  dd::Param::FieldSnap,   dd::Param::FieldMetal };

} // namespace

LikeVector drumFingerprint(const std::array<float, 7>& fields, feathers::drum::Kind kind) noexcept {
	const feathers::drum::FieldUse reads = feathers::drum::kindReads(kind);
	const std::array<bool, 7> read = { reads.pitch, reads.length, reads.sweep, reads.tone, reads.noise, reads.snap, reads.metal };
	LikeVector v {};
	for (size_t i = 0; i < fields.size(); ++i) {
		v[i] = read[i] ? static_cast<double>(fields[i]) : 0.0;
	}
	return v;
}

dd::DrumLike likeOf(const dd::Settings& s) {
	dd::DrumLike like;
	like.x = static_cast<double>(s.at(dd::Param::TravelX));
	like.y = static_cast<double>(s.at(dd::Param::TravelY));
	like.zone = static_cast<uint16_t>(s.index(dd::Param::Zone));
	like.offset = s.at(dd::Param::ZoneOffset);
	like.gamma = s.at(dd::Param::Gamma);
	like.rotate = s.at(dd::Param::Rotate);
	like.kind = static_cast<uint16_t>(std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
	like.fields = dd::fieldsArray(dd::fieldsOf(s));
	return like;
}

bool likeApplies(const dd::DrumLike& like, const dd::Settings& s) noexcept {
	return like.kind == static_cast<uint16_t>(std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1))
	       && like.zone == static_cast<uint16_t>(s.index(dd::Param::Zone)) && like.offset == s.at(dd::Param::ZoneOffset)
	       && like.gamma == s.at(dd::Param::Gamma) && like.rotate == s.at(dd::Param::Rotate);
}

void DrumLikeSurface::fill(const dd::Settings& base, std::span<const WarpPoint> points, std::vector<LikeVector>& out) const {
	dd::Settings s = base;
	const auto kind = static_cast<feathers::drum::Kind>(std::clamp(s.index(dd::Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
	for (const WarpPoint& p : points) {
		const WarpPoint t = drumTravelOf(p);
		s.v[static_cast<size_t>(dd::Param::TravelX)] = static_cast<float>(t.x);
		s.v[static_cast<size_t>(dd::Param::TravelY)] = static_cast<float>(t.y);
		out.push_back(drumFingerprint(dd::fieldsArray(dd::fieldsOf(s)), kind));
	}
}

void DrumLikeSurface::fingerprintsAt(std::span<const WarpPoint> points, std::vector<LikeVector>& out) const {
	fill(base_, points, out);
}

size_t DrumLikeSurface::controlCount() const {
	// NONE: the model projects out a knob that moves every place the same way, and each of the seven scalers is exactly that for ITS
	// field (the knob is the field's offset), so listing them removed the very axes the likes are made of (measured: with the scalers
	// listed, five of the seven field directions went and a place far from the likes scored 1). A scaler turned later moves what each place
	// sounds like, which the fingerprint reads through `fieldsOf`, scalers in.
	return 0;
}

const char* DrumLikeSurface::controlName(size_t) const {
	return "";
}

void DrumLikeSurface::nudgedFingerprintsAt(size_t, std::span<const WarpPoint> points, std::vector<LikeVector>& out) const {
	fill(base_, points, out); // (no controls are listed, so nothing asks)
}

std::array<float, 13> drumLikeKey(const dd::Settings& s) noexcept {
	std::array<float, 13> k {};
	k[0] = static_cast<float>(s.index(dd::Param::Kind));
	k[1] = static_cast<float>(s.index(dd::Param::Zone));
	k[2] = s.at(dd::Param::ZoneOffset);
	k[3] = s.at(dd::Param::Gamma);
	k[4] = s.at(dd::Param::Rotate);
	for (size_t i = 0; i < kScalers.size(); ++i) {
		k[5 + i] = s.at(kScalers[i]);
	}
	k[12] = 0.0f;
	return k;
}

std::array<double, 2> DrumLikeState::worldAt(double viewX, double viewY) const noexcept {
	const WarpPoint w = derivation.warp.worldAt(drumWarpOf(viewX, viewY));
	const WarpPoint t = drumTravelOf(w);
	return { t.x, t.y };
}

std::array<double, 2> DrumLikeState::viewAt(double worldX, double worldY) const noexcept {
	const WarpPoint v = derivation.warp.viewAt(drumWarpOf(worldX, worldY));
	const WarpPoint t = drumTravelOf(v);
	return { t.x, t.y };
}

double DrumLikeState::scoreAt(const dd::Settings& s, double x, double y) const {
	if (derivation.model.empty()) {
		return 0.0;
	}
	std::vector<LikeVector> at;
	const WarpPoint p = drumWarpOf(x, y);
	DrumLikeSurface(s).fingerprintsAt(std::span<const WarpPoint>(&p, 1), at);
	return derivation.model.score(at.front());
}

std::shared_ptr<const DrumLikeState> deriveDrumLikes(const dd::Settings& s, const std::vector<dd::DrumLike>& likes, double strength) {
	std::vector<dd::DrumLike> applied;
	for (const dd::DrumLike& like : likes) {
		if (likeApplies(like, s)) {
			applied.push_back(like);
		}
	}
	if (applied.empty()) {
		return nullptr;
	}
	const auto kind = static_cast<feathers::drum::Kind>(applied.front().kind);
	std::vector<WarpPoint> places;
	std::vector<LikeVector> prints;
	for (const dd::DrumLike& like : applied) {
		places.push_back(drumWarpOf(like.x, like.y));
		prints.push_back(drumFingerprint(like.fields, kind));
	}
	auto state = std::make_shared<DrumLikeState>();
	state->derivation = deriveLikes(DrumLikeSurface(s), places, prints, strength);
	state->applied = std::move(applied);
	state->key = drumLikeKey(s);
	state->strength = strength;
	return state;
}

std::array<double, 2> drumMutate(const DrumLikeState* state, double x, double y, double sideways, double up, double reach) noexcept {
	if (state == nullptr) {
		return { x + sideways * reach, y + up * reach };
	}
	const std::array<double, 2> from = state->viewAt(x, y);
	return state->worldAt(from[0] + sideways * reach, from[1] + up * reach);
}

} // namespace feathers::ui
