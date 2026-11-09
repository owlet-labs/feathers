/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/automod/automod_derived.h"
#include "fx/automod/automod_phase_space.h"

#include <algorithm>
#include <cmath>

// **THE TERRAIN'S READING, AT DEVICE LEVEL.** Moved here from `plugin/ui/automod_terrain.h`
// (2026-11-07) so a MODULATOR can read the same terrain AutoMod's pad draws: a device cannot include
// the UI. `automod_terrain.h` re-exports these names, so the pad calls the very same functions.
namespace feathers::fx::automod {

/// **HOW MUCH OF AUTOMOD'S MACHINERY IS ENGAGED AT A POSITION — THE FIELD, DECODED.**
///
/// BY DESIGN the terrain need not be render previews; it can be decoded from the field. So this decodes the DERIVE rather than rendering audio: a sum of the things that make
/// the device audible there — the comb's mix and feedback, the SVF's feedback, the filter's mod depth,
/// and the magnitude of the three band responses. Type drives the first group, Flavour the bands, so a
/// map made from these varies along BOTH axes.
///
/// The raw sum goes negative (measured over an 8x8-zone sweep: -0.206 to 5.366); `activityOf` clamps.
[[nodiscard]] inline float rawActivityOf(const AutomodDerived& d) {
	float bands = 0.0f;
	for (const float r : d.bandResponse) {
		bands += std::fabs(r);
	}
	// |FEEDBACK|, since it became bipolar (backlog IS.5): a negative comb rings as hard as a positive one.
	return d.comb.mix + std::fabs(d.comb.feedback) + d.svfFeedback + d.filterModDepth + bands;
}

/// **THE NORMALISER, MEASURED RATHER THAN CHOSEN:** twice the field's own mean. A map normalised by
/// its maximum is almost all floor; two-times-mean puts roughly a tenth of the plane at each end.
/// The measurement table is in `plugin/ui/automod_terrain.h`.
inline constexpr float kActivityFull = 3.556f;

/// 0 (nothing happening here) to 1 (everything happening here), for a reading already derived.
[[nodiscard]] inline float activityOf(const AutomodDerived& d) {
	return std::clamp(rawActivityOf(d) / kActivityFull, 0.0f, 1.0f);
}

/// **`rawActivityOf(deriveAutomodAt(coords, phases))` WITHOUT THE REST OF THE DERIVE** — the seven
/// triangles the sum reads, and nothing else (no LFO shape, no springs). For a caller that reads the
/// terrain on the AUDIO thread (`mod::twinfield`, sixteen points a tick), where the full derive's
/// shape build is pure waste.
///
/// **BIT-EQUAL TO THE DERIVE'S ROAD, AND A CASE SAYS SO** (`test_mod_twinfield`, "the fast terrain
/// reading is bit-equal to AutoMod's derive"): each term is the same `evalTriangle` call on the same
/// phase with the same config, and the sum is spelled in the same order as `rawActivityOf`. A change to
/// either side that is not made to both goes red there.
[[nodiscard]] inline float rawActivityAtPhases(const phase_space::Phases& ph) {
	namespace ps = phase_space;
	const auto tri = [&](size_t at, const owlet::dsp::phi::PhiTriConfig& cfg) {
		return owlet::dsp::phi::evalTriangle(ph[at], 1.0f, cfg);
	};
	// THE DERIVE'S OWN SCALINGS: comb feedback is `typeScalars[0] * 0.85f`.
	const float combFeedback = tri(ps::kTypeScalars + 0, kTypeScalarBank[0]) * 0.85f;
	const float combMix = tri(ps::kTypeScalars + 1, kTypeScalarBank[1]);
	const float svfFeedback = tri(ps::kTypeScalars + 2, kTypeScalarBank[2]);
	const float filterModDepth = tri(ps::kFlavourScalars + 2, kFlavorScalarBank[2]);
	float bands = 0.0f;
	for (size_t i = 0; i < 3; ++i) {
		bands += std::fabs(tri(ps::kFilterLfoResponse + i, kFilterLfoResponseBank[i]));
	}
	return combMix + std::fabs(combFeedback) + svfFeedback + filterModDepth + bands;
}

/// 0..1, as `activityOf`, from phases.
[[nodiscard]] inline float activityAtPhases(const phase_space::Phases& ph) {
	return std::clamp(rawActivityAtPhases(ph) / kActivityFull, 0.0f, 1.0f);
}

} // namespace feathers::fx::automod
