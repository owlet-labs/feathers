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

// THE RING PIN. The physics-model switch (weave_models.h) moved the Ring's topology behind a seam and
// promised to leave its output alone. That promise is this hash: FNV-1a over the bits of every scan
// sample on all three mips and every node, for a bowed, plucked, morphing Ring at four phases, pinned
// from the code as it stood at 56bf0918, before the switch existed. MEASURED 2026-09-25: the same hash
// from the pre-switch source and from this tree.
//
// RE-TAKEN ONCE, on purpose, 2026-09-25: the quality review dropped Excitation::extraForce, which the
// first render used (0.01). The pin was re-taken by building the 56bf0918 source with extraForce left
// at 0 — so it is still the pre-switch code's output, now of a render the current API can express.
//
// RE-TAKEN AGAIN, on purpose, 2026-10-06 (mac-resonate): the windows' trig moved off the C library's cosf/sinf to
// dsp/portable_trig.h, because macOS' cosf rounds differently and Resonate's ring rendered differently there. MEASURED on
// this render, old against new: 27,978 of 222,400 mip-0 scan samples differ, by at most 2.4e-7 (127 dB under its 0.56 peak).
//
// A deliberate change to the Ring's arithmetic turns this red, and then the pin is re-taken ON PURPOSE
// with a note of what changed and why — it is a tripwire, not a spec. It asserts float bits, so it is
// platform-pinned (x86-64 Linux, as vox-voice and flagship).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_weave.hpp"
#include "dsp/weave_string.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// FNV-1a over the bits of every scan sample (three mips) and every node, Ring, at four phases.
inline uint64_t ringRenderHash() {
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](float f) {
		uint32_t bits = 0;
		std::memcpy(&bits, &f, sizeof bits);
		for (int b = 0; b < 4; ++b) {
			h ^= (bits >> (8 * b)) & 0xffu;
			h *= 1099511628211ull;
		}
	};
	for (double phase : { 0.1, 0.3, 0.6, 0.85 }) {
		feathers::WeaveString s;
		s.setLaws(owlet::dsp::buildPhiWeaveLaws(phase), owlet::dsp::buildPhiWeaveLaws(phase + 0.3819660112501051));
		feathers::WeaveString::Excitation e;
		e.bowScale = 1.5f;
		s.setExcitation(e);
		s.prepare(48000.0);
		double scan = 0.0;
		for (int t = 0; t < 400; ++t) {
			if (t % 97 == 0) {
				s.pluck();
			}
			s.setCrossfade(0.5f + 0.5f * static_cast<float>(std::sin(t * 0.013)));
			const int m = s.samplesUntilTick();
			for (int k = 0; k < m; ++k) {
				for (int mip = 0; mip < 3; ++mip) {
					mix(s.sample(scan + s.travelPhaseAt(k), mip, k));
				}
				scan += 330.0 / 48000.0;
			}
			s.advance(m);
			for (int i = 0; i < 32; ++i) {
				mix(s.nodesForTest()[i]);
			}
		}
	}
	return h;
}

} // namespace

TEST_CASE("weave ring pin: the Ring renders bit-for-bit as it did before the model switch") {
	// 2026-10-06 (backlog LP): the home shape's sine made portable too (phi_weave.cpp), which moves glibc's
	// rounding at some of these four phases; was 0xd258f17e8f457468 (portable window trig), before that 0xb02bd241d24b6832.
	constexpr uint64_t kPinned = 0x6b0a094bb1565968ull;
	CHECK(ringRenderHash() == kPinned);
}
