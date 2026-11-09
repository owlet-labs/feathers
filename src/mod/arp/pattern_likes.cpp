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

#include "mod/arp/pattern_likes.h"

#include "device/state_blocks.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::arp::likes {

namespace {

/// A BLOCK SUMMED TO 1 (left all zero when it holds nothing).
template <size_t N>
void normalise(float* block) {
	double sum = 0.0;
	for (size_t i = 0; i < N; ++i) {
		sum += block[i];
	}
	if (sum > 0.0) {
		for (size_t i = 0; i < N; ++i) {
			block[i] = static_cast<float>(block[i] / sum);
		}
	}
}

double hellinger(const float* a, const float* b, size_t n) {
	double s = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double d = std::sqrt(std::max(0.0f, a[i])) - std::sqrt(std::max(0.0f, b[i]));
		s += d * d;
	}
	return std::sqrt(0.5 * s);
}

} // namespace

size_t familyOf(genes::Mode mode) noexcept {
	switch (mode) {
	case genes::Mode::Melody: return 0;
	case genes::Mode::MelodyMono: return 1;
	case genes::Mode::Chords:
	case genes::Mode::Progression: return 2; // chords, both: a progression is liked beside the Chords mode's
	// DRUM HAS NO BIN, which keeps the fingerprint 48 floats: an all-zero family block differs from every one-hot one, so
	// `distance` never compares a drum like with a pitched pattern, and two drum likes compare as a family of their own.
	case genes::Mode::Drum:
	case genes::Mode::Modulation: return kModeBins; // Modulation is the Drum decode, liked beside it
	default: return 3;
	}
}

Fingerprint fingerprintOf(const pattern::PatternCycle& cycle, genes::Mode mode, int rootKey) noexcept {
	Fingerprint f {};
	float* rhythm = f.data();
	float* contour = rhythm + kRhythm;
	float* reg = contour + kContour;
	float* art = reg + kRegister;
	float* chords = art + kArticulation;
	float* fam = chords + kChords;
	if (const size_t family = familyOf(mode); family < kModeBins) {
		fam[family] = 1.0f;
	}
	if (!cycle.playable || cycle.length == 0) {
		return f;
	}
	const auto length = static_cast<double>(cycle.length);
	int last = -1;
	int lastDegree = cycle.steps[0].degree;
	double tones = 0.0;
	double span = 0.0;
	size_t notes = 0;
	size_t chordSteps = 0;
	size_t degreeMoves = 0;
	for (size_t k = 0; k < cycle.length; ++k) {
		const pattern::PatternStep& st = cycle.steps[k];
		art[st.kind == pattern::PatternStep::Kind::Rest ? 2 : (st.kind == pattern::PatternStep::Kind::Tie ? 1 : 0)] += 1.0f;
		if (st.degree != lastDegree) {
			++degreeMoves;
			lastDegree = st.degree;
		}
		if (st.kind != pattern::PatternStep::Kind::Note) {
			continue;
		}
		++notes;
		const auto pos = static_cast<size_t>(std::floor(static_cast<double>(k) / length * kRhythm));
		rhythm[std::min(pos, kRhythm - 1)] += st.velocity;
		const int key = cycle.keyAt(k, 0);
		if (last >= 0) {
			const int step = std::clamp(key - last, -6, 5);
			contour[static_cast<size_t>(step + 6)] += 1.0f;
		}
		last = key;
		const int band = std::clamp((key - rootKey + 18) / 6, 0, static_cast<int>(kRegister) - 1);
		reg[static_cast<size_t>(band)] += 1.0f;
		art[3] += st.accent ? 1.0f : 0.0f;
		art[4] += st.slide ? 1.0f : 0.0f;
		art[5] += st.ratchet > 1 ? 1.0f : 0.0f;
		if (st.count > 1) {
			++chordSteps;
			tones += st.count;
			span += st.keys[static_cast<size_t>(st.count - 1)] - st.keys[0];
		}
	}
	chords[0] = static_cast<float>(static_cast<double>(chordSteps) / length);
	chords[1] = chordSteps > 0 ? static_cast<float>(tones / chordSteps / 7.0) : 0.0f;
	chords[2] = static_cast<float>(static_cast<double>(degreeMoves) / length);
	chords[3] = chordSteps > 0 ? static_cast<float>(span / chordSteps / 36.0) : 0.0f;
	normalise<kRhythm>(rhythm);
	normalise<kContour>(contour);
	normalise<kRegister>(reg);
	normalise<kArticulation>(art);
	normalise<kChords>(chords);
	(void)notes;
	return f;
}

double distance(const Fingerprint& a, const Fingerprint& b) noexcept {
	const size_t famAt = kFingerprint - kModeBins;
	for (size_t i = 0; i < kModeBins; ++i) {
		if ((a[famAt + i] > 0.5f) != (b[famAt + i] > 0.5f)) {
			return kAcrossFamilies;
		}
	}
	size_t at = 0;
	double d = 0.0;
	const std::array<std::pair<size_t, double>, 5> blocks { { { kRhythm, 0.35 },
		                                                      { kContour, 0.25 },
		                                                      { kRegister, 0.15 },
		                                                      { kArticulation, 0.15 },
		                                                      { kChords, 0.10 } } };
	for (const auto& [n, w] : blocks) {
		d += w * hellinger(a.data() + at, b.data() + at, n);
		at += n;
	}
	return d;
}

double score(const SlotLikes& likes, const Fingerprint& f) noexcept {
	double s = 0.0;
	for (size_t i = 0; i < std::min(likes.count, kMaxLikes); ++i) {
		const double d = distance(likes.like[i].heard, f);
		if (d < kAcrossFamilies) {
			s += std::exp(-(d * d) / (kScoreSigma * kScoreSigma));
		}
	}
	return std::min(1.0, s);
}

void write(const AllLikes& likes, float* out) noexcept {
	device::BlockWriter w(out, kBlockWords);
	for (const SlotLikes& slot : likes) {
		w.u16(static_cast<uint32_t>(std::min(slot.count, kMaxLikes)));
		for (size_t i = 0; i < kMaxLikes; ++i) {
			const Like& l = slot.like[i];
			for (const float v : { l.zone, l.zoneOffset, l.gamma, l.placeX, l.placeY, l.radius, l.theta, l.rotate }) {
				w.f32(v);
			}
			for (const float v : l.heard) {
				w.f32(v);
			}
			for (const double v : l.point) {
				const auto hi = static_cast<float>(v);
				w.f32(hi);
				w.f32(static_cast<float>(v - static_cast<double>(hi)));
			}
		}
	}
}

bool read(const float* in, size_t n, AllLikes* out) noexcept {
	if (n != kBlockWords || out == nullptr) {
		return false;
	}
	device::BlockReader r(in, n);
	AllLikes likes {};
	for (SlotLikes& slot : likes) {
		slot.count = std::min<size_t>(r.u16(), kMaxLikes);
		for (size_t i = 0; i < kMaxLikes; ++i) {
			Like& l = slot.like[i];
			for (float* v : { &l.zone, &l.zoneOffset, &l.gamma, &l.placeX, &l.placeY, &l.radius, &l.theta, &l.rotate }) {
				*v = r.f32();
			}
			for (float& v : l.heard) {
				v = r.f32();
			}
			for (double& v : l.point) {
				const auto hi = static_cast<double>(r.f32());
				v = hi + static_cast<double>(r.f32());
			}
		}
	}
	if (!r.ok()) {
		return false;
	}
	*out = likes;
	return true;
}

} // namespace feathers::mod::arp::likes
