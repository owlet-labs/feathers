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

#include "mod/arp/pattern_warp.h"

#include <memory>
#include <vector>

namespace feathers::mod::arp::genes {

SiteAngles anglesOf(const Place& place, size_t length) noexcept {
	SiteAngles out {};
	for (size_t r = kStepRowCount; r < kRowCount; ++r) {
		const auto row = static_cast<Row>(r);
		out[wideSite(row)] = static_cast<float>(angleOf(row, place.phase[r]));
	}
	for (size_t k = 0; k < kMaxSteps; ++k) {
		for (size_t i = 0; i < kStepRowCount; ++i) {
			out[stepSite(i, k)] = static_cast<float>(angleOf(static_cast<Row>(i), stepPhase(place, i, k, length)));
		}
	}
	return out;
}

bool sameWarp(const Warp& a, const Warp& b) noexcept {
	if (a.likes != b.likes) {
		return false;
	}
	for (size_t i = 0; i < kWarpSites; ++i) {
		const PhaseWarpSite& x = a.sites[i];
		const PhaseWarpSite& y = b.sites[i];
		if (x.live != y.live || x.lut != y.lut || x.pinned != y.pinned || x.pinAt != y.pinAt || x.pinTo != y.pinTo
		    || x.pinShift != y.pinShift) {
			return false;
		}
	}
	return true;
}

bool buildWarp(std::span<const SiteAngles> likes, double strength, Warp& out, const PhaseWarpParams& params,
               const std::function<bool()>& stop) {
	out.likes = 0;
	for (PhaseWarpSite& s : out.sites) {
		s = PhaseWarpSite {};
	}
	std::vector<double> x;
	// THE CHAIN (dsp/phase_warp.cpp's `buildPhaseWarp`, per site): grown one like at a time, the newest pinned to what it was
	// heard as under the warp before it.
	auto before = std::make_unique<Warp>(); // 850 sites: on the heap, never the stack
	for (size_t n = 1; n <= likes.size(); ++n) {
		const SiteAngles& newest = likes[n - 1];
		out.likes = static_cast<uint16_t>(n);
		for (size_t site = 0; site < kWarpSites; ++site) {
			if (stop && stop()) {
				return false;
			}
			x.clear();
			for (size_t i = 0; i < n; ++i) {
				x.push_back(static_cast<double>(likes[i][site]));
			}
			PhaseWarpSite& s = out.sites[site];
			s = PhaseWarpSite {};
			fitPhaseWarpSite(x, strength, params, s);
			pinPhaseWarpSite(s, newest[site], before->at(site, newest[site]));
		}
		if (n < likes.size()) {
			*before = out;
		}
	}
	return true;
}

} // namespace feathers::mod::arp::genes
