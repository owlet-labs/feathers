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

// Single source of truth for the golden character capture: the tuple grid, the field walk, and the
// line format. dump_character writes the golden with it; test_character_golden re-derives the same
// lines in-process and diffs. Both sides share this code so a format change can never desync them.

#include "character/phi_morph.hpp"
#include "character/phi_stair.hpp"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace feathers::character_dump {

// A field gained by any of these three is a field the walk below would silently not emit.
// struct changed → update the dump walk AND regenerate the golden
static_assert(sizeof(owlet::dsp::PhiMorphSegment) == 24);
// struct changed → update the dump walk AND regenerate the golden
static_assert(sizeof(owlet::dsp::PhiMorphWavetable) == 312);
// struct changed → update the dump walk AND regenerate the golden
static_assert(sizeof(owlet::dsp::PhiStairParams) == 132);

// Phase offsets carry a literal label so the line key never depends on printf rounding of the
// value. Nothing checks the two halves against each other — edit them together, or the golden keys
// quietly stop describing the offsets they were built from.
struct PhaseOffset {
	float value;
	const char* label;
};

inline constexpr uint16_t kZones[] = {0, 1, 17, 64, 100, 127};
inline constexpr PhaseOffset kPhaseOffsets[] = {{0.0f, "0"}, {0.25f, "0.25"}, {1.618f, "1.618"}};

namespace detail {

// %a round-trips exactly, so a golden line pins the bit pattern rather than a rounded decimal.
inline std::string floatText(float v) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%a", static_cast<double>(v));
	return buf;
}

inline std::string intText(long long v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%lld", v);
	return buf;
}

inline std::string indexed(const char* name, int i) {
	char buf[64];
	std::snprintf(buf, sizeof(buf), "%s[%d]", name, i);
	return buf;
}

class Emitter {
public:
	Emitter(std::vector<std::string>& out, const char* table, uint16_t zone, const PhaseOffset& po) : out_(out) {
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%s z%u po%s ", table, static_cast<unsigned>(zone), po.label);
		prefix_ = buf;
	}

	void flt(const std::string& field, float v) { out_.push_back(prefix_ + field + " " + floatText(v)); }

	void integer(const std::string& field, long long v) { out_.push_back(prefix_ + field + " " + intText(v)); }

private:
	std::vector<std::string>& out_;
	std::string prefix_;
};

// Every slot of every array is emitted, not just the numSegments/step-count active prefix, so a
// layout or capacity change shows up as a line-count diff instead of hiding in dead storage.
inline void appendMorph(std::vector<std::string>& out, uint16_t zone, const PhaseOffset& po) {
	const owlet::dsp::PhiMorphWavetable t = owlet::dsp::buildPhiMorphWavetable(zone, po.value);
	Emitter e(out, "morph", zone, po);

	for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
		const owlet::dsp::PhiMorphSegment& s = t.segments[i];
		const std::string seg = indexed("seg", i);
		e.integer(seg + ".startAmp", s.startAmp);
		e.integer(seg + ".startPhase", s.startPhase);
		e.integer(seg + ".endPhase", s.endPhase);
		e.flt(seg + ".invWidth", s.invWidth);
		e.flt(seg + ".ampDeltaF", s.ampDeltaF);
		e.flt(seg + ".curvatureF", s.curvatureF);
	}

	e.integer("numSegments", t.numSegments);

	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		e.flt(indexed("phase", i), t.phases[i]);
	}
	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		e.flt(indexed("amp", i), t.amplitudes[i]);
	}
	for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
		e.flt(indexed("curv", i), t.curvatures[i]);
	}

	e.flt("gain", t.gain);
	e.flt("endpointAmp", t.endpointAmp);
	e.flt("morphAmpOvershoot", t.morphAmpOvershoot);
	e.flt("morphCurvBoost", t.morphCurvBoost);
	e.flt("morphPhaseDistort", t.morphPhaseDistort);
	e.flt("phaseJitter", t.phaseJitter);
	e.flt("ampNoise", t.ampNoise);
}

inline void appendStair(std::vector<std::string>& out, uint16_t zone, const PhaseOffset& po) {
	const owlet::dsp::PhiStairParams p = owlet::dsp::buildPhiStairParams(zone, po.value);
	Emitter e(out, "stair", zone, po);

	for (int i = 0; i < owlet::dsp::kPhiStairSteps; ++i) {
		e.flt(indexed("height", i), p.heights[i]);
	}
	for (int i = 0; i < owlet::dsp::kPhiStairSteps; ++i) {
		e.flt(indexed("width", i), p.widths[i]);
	}
	e.flt("slope", p.slope);
}

} // namespace detail

inline std::vector<std::string> collectLines() {
	std::vector<std::string> out;
	for (uint16_t zone : kZones) {
		for (const PhaseOffset& po : kPhaseOffsets) {
			detail::appendMorph(out, zone, po);
			detail::appendStair(out, zone, po);
		}
	}
	return out;
}

} // namespace feathers::character_dump
