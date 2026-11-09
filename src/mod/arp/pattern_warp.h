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

// **THE LIKES AS A PHASE WARP, ON THE PATTERN TERRAIN** (design §14-bis: "does the phase warp transfer? Yes, in shape").
// A pattern reads every row through `tri(wrap(...))` — a pattern-wide row once, a per-step row once per step — so each such
// READ is a SITE on its own circle, as Feathers' P-MÖBIUS numbers a sound's reads (dsp/phase_warp.h). The warp bends each
// site's angle toward the angles the slot's likes were heard at, by Feathers' own per-site fit (`fitPhaseWarpSite`), the
// newest like pinned to what it was heard as under the warp before it — so the chain is DERIVED from the likes and a
// reload walking them arrives at the same warp.
//
// THE LIKES' ANGLES are taken from each like's stored full point at the fingerprint's own length (`kLikeLength`, 16
// steps: what the heart measured). A pattern played at another Length reads step k's site with its own angle, pulled toward
// the 16-step likes' angles at step k: exact at 16, a resemblance elsewhere (design note, measured in the study).
//
// MESSAGE THREAD to build (it allocates); the read allocates nothing.

#include "dsp/phase_warp.h"
#include "mod/arp/genes.h"

#include <array>
#include <cstddef>
#include <functional>
#include <span>

namespace feathers::mod::arp::genes {

inline constexpr size_t kWarpSites = kStepSitesFrom + kStepRowCount * kMaxSteps;
/// The length a like's angles are taken at: the heart's fingerprint length.
inline constexpr size_t kLikeLength = 16;

struct Warp {
	uint16_t likes = 0;
	std::array<PhaseWarpSite, kWarpSites> sites {};
	[[nodiscard]] bool identity() const noexcept { return likes == 0; }
	[[nodiscard]] float at(size_t site, float theta) const noexcept {
		return site < kWarpSites ? phaseWarpSiteAt(sites[site], theta) : theta;
	}
};

/// Every site's wrapped angle at `place`, the per-step sites at an `length`-step cycle (the float each read sees).
using SiteAngles = std::array<float, kWarpSites>;
[[nodiscard]] SiteAngles anglesOf(const Place& place, size_t length = kLikeLength) noexcept;

/// **THE FIT**: from the likes' angles, oldest first; `strength` weighs each like against the uniform's prior, as
/// Feathers' does. No likes: the identity. `stop` (optional) is asked between sites; once it says true the fit returns
/// false and `out` is incomplete — a background fit that was superseded.
bool buildWarp(std::span<const SiteAngles> likes, double strength, Warp& out, const PhaseWarpParams& params = {},
               const std::function<bool()>& stop = {});
/// Exact field-by-field equality (no padding bytes compared).
[[nodiscard]] bool sameWarp(const Warp& a, const Warp& b) noexcept;

} // namespace feathers::mod::arp::genes
