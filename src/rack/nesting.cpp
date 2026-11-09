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

#include "rack/nesting.h"

#include "rack/inner_chain_bundle.h"
#include "rack/parallel_rack.h"
#include "rack/rack_device.h"
#include "rack/site_chains.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace feathers::rack {

namespace {

/// PAST THIS EVERYTHING IS "TOO DEEP" ALREADY, so a walk stops here rather than following a hostile file's nesting.
constexpr size_t kGiveUp = kMaxDepth + 2;

size_t bodyHeightAt(std::span<const uint8_t> body, const device::Catalogue& catalogue, size_t level);

size_t patchHeightAt(const RackPatch& chain, const device::Catalogue& catalogue, size_t level) {
	size_t tallest = 0;
	for (const RackPatch::Device& d : chain.device) {
		size_t h = 0;
		const device::Descriptor* desc = catalogue(d.tag.data());
		if (desc != nullptr && desc->container != nullptr) {
			h = std::max<size_t>(1, bodyHeightAt(d.body, catalogue, level + 1));
		}
		if (d.hasSite) {
			h = std::max(h, std::max<size_t>(1, bodyHeightAt(d.site, catalogue, level + 1)));
		}
		tallest = std::max(tallest, h);
	}
	return tallest;
}

struct Walk {
	const device::Catalogue* catalogue = nullptr;
	size_t level = 0;
	size_t tallest = 0;
};

/// One chain's document inside a frame: one chain, plus its tallest device. Copies the bytes out, which a frame asks of
/// a map; nothing is kept.
bool chainOfFrame(void* ctx, std::span<const uint8_t> in, std::vector<uint8_t>* out) {
	auto* w = static_cast<Walk*>(ctx);
	RackPatch chain;
	if (parseRackText(std::string_view(reinterpret_cast<const char*>(in.data()), in.size()), &chain)) {
		w->tallest = std::max(w->tallest, 1 + patchHeightAt(chain, *w->catalogue, w->level));
	}
	out->assign(in.begin(), in.end());
	return true;
}

size_t bodyHeightAt(std::span<const uint8_t> body, const device::Catalogue& catalogue, size_t level) {
	if (body.empty()) {
		return 0;
	}
	if (level >= kGiveUp) {
		return kGiveUp;
	}
	Walk w { &catalogue, level, 0 };
	std::vector<uint8_t> scratch;
	// THE SAME FOUR FRAMINGS `remapOuterSources` reads (`fragment.cpp`): a Chain's document, a bundle of lanes
	// (SKLITTER, Resonate), a Rack's chains, a site's two lanes.
	if (body.front() == '{') {
		(void)chainOfFrame(&w, body, &scratch);
	}
	else if (body.size() >= sizeof(uint32_t)) {
		const uint32_t magic = static_cast<uint32_t>(body[0]) | (static_cast<uint32_t>(body[1]) << 8)
		                       | (static_cast<uint32_t>(body[2]) << 16) | (static_cast<uint32_t>(body[3]) << 24);
		if (magic == kBundleMagic) {
			(void)mapChainBundle(body, &chainOfFrame, &w, &scratch);
		}
		else if (magic == parallel::kBodyMagic) {
			(void)parallel::mapRackBody(body, &chainOfFrame, &w, &scratch);
		}
		else {
			(void)mapSiteBody(body, &chainOfFrame, &w, &scratch);
		}
	}
	return w.tallest;
}

} // namespace

size_t deviceHeight(const RackPatch::Device& device, const device::Catalogue& catalogue) {
	RackPatch one;
	one.device.push_back(device);
	return patchHeightAt(one, catalogue, 0);
}

size_t patchHeight(const RackPatch& chain, const device::Catalogue& catalogue) { return patchHeightAt(chain, catalogue, 0); }

size_t bodyHeight(std::span<const uint8_t> body, const device::Catalogue& catalogue) {
	return bodyHeightAt(body, catalogue, 0);
}

bool containerBodyFits(size_t holderDepth, std::span<const uint8_t> body, const device::Catalogue& catalogue) {
	return fitsNesting(holderDepth, std::max<size_t>(1, bodyHeight(body, catalogue)));
}

bool bodyFitsChainsAt(size_t chainsDepth, std::span<const uint8_t> body, const device::Catalogue& catalogue) {
	const size_t h = bodyHeight(body, catalogue);
	return h == 0 || fitsNesting(chainsDepth, h - 1);
}

} // namespace feathers::rack
