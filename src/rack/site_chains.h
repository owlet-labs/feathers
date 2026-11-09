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

#pragma once

#include "backplane/axes.h"
#include "backplane/device_instance_id.h"
#include "device/latency.h"
#include "device/registry.h"
#include "rack/inner_chain.h"
#include "rack/export_table.h"
#include "rack/inner_chain_bundle.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// **A CHAIN ACCESS SITE — THE PRE AND POST CHAINS EVERY EFFECT GETS** (backlog NR).
//
// **AT THE ENGINE SLOT, NOT IN THE DEVICE.** The slot is the one generic place — Feathers' fx lane and the rack both
// run on `RackEngine` — so a device gains a site without a line of its own code. A slot holds a pointer to one of these
// or null; null is what every device has until its chip is first used, and costs one load per block.
//
// **WHO OWNS IT.** The host's `Converger`, on the message thread, from the moment it builds one to the moment it frees
// it: the engine only ever holds the pointer it was offered (`RackEngine::offerSite`), installed at a drain. Nothing on
// the audio thread frees, so a device removed, replaced or reordered never takes its site with it — the converger
// offers it again to whatever slot then holds the device. **The identity is the device's id** (the slot's address is a
// contemporaneous handle, CLAUDE.md's ABA rule), checked at the drain against the slot's own.
//
// **BUS ONLY** (the design's measured reason: per voice would be ~45 MB a site): both chains have no voices, so a Poly
// device put inside one runs on the bus.

namespace feathers::rack {

/// A FADER POSITION AS A GAIN — `fx::busutil`'s `(2v)^2`, unity at `composite::kFaderUnity` (0.5), restated as the
/// rack device and the parallel rack each restate it.
[[nodiscard]] constexpr float siteFaderGain(float position) noexcept {
	const float p = position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position);
	return (2.0f * p) * (2.0f * p);
}
/// **WHAT A POLY SWITCH SAYS INSIDE A SITE** — the site is bus only, so a device in either lane runs once, on the bus.
inline constexpr const char* kPolyInsideSite =
    "Poly is unavailable here: a device's PRE and POST chains run once, on the bus — no voice reaches them";

static_assert(siteFaderGain(0.5f) == 1.0f, "the wet Gain's centre must be EXACTLY unity, or a device at its default "
                                           "is not the identity");

class SiteChains final {
public:
	/// The stable lane ids in a site's body (`rack::writeChainBundle`).
	static constexpr uint16_t kPreLane = 1;
	static constexpr uint16_t kPostLane = 2;

	/// MESSAGE THREAD. Built adopted (the catalogue is the host's, handed through its engine) and, when the host is
	/// already running, prepared — so a site built while the host plays is ready when the drain installs it.
	SiteChains(DeviceInstanceId owner, const device::Catalogue& catalogue, double sampleRate, int maxFrames);
	~SiteChains();

	SiteChains(const SiteChains&) = delete;
	SiteChains& operator=(const SiteChains&) = delete;

	/// THE DEVICE THIS SITE BELONGS TO — what the drain compares against a slot's id before installing it.
	[[nodiscard]] DeviceInstanceId owner() const noexcept { return owner_; }

	[[nodiscard]] InnerChain& pre() noexcept { return pre_; }
	[[nodiscard]] InnerChain& post() noexcept { return post_; }
	[[nodiscard]] const InnerChain& pre() const noexcept { return pre_; }
	[[nodiscard]] const InnerChain& post() const noexcept { return post_; }
	/// The two lanes, by stable id — what the bundle and the container table walk.
	[[nodiscard]] std::array<BundleLane, 2> lanes() noexcept {
		return { BundleLane { kPreLane, &pre_ }, BundleLane { kPostLane, &post_ } };
	}

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD                                                                               //
	//--------------------------------------------------------------------------------------------//

	/// Both chains and the furniture's dry delay, at the host's rate (no block running).
	void prepare(double sampleRate, int maxFrames);
	/// What the last `prepare` was told — the converger prepares again when the host's rate moved.
	[[nodiscard]] double preparedRate() const noexcept { return sampleRate_; }
	[[nodiscard]] int preparedFrames() const noexcept { return maxFrames_; }
	/// BOTH LANES IN ONE BODY, by stable id (`writeChainBundle`).
	bool saveState(const device::ByteSink& sink) const;
	/// ALL OR NOTHING (`readChainBundle`). Empty bytes are an empty site: present, holding nothing.
	bool loadState(const uint8_t* data, size_t bytes);
	/// INTO A LIVE SITE (`ContainerOps::applyState`, an undo or a preset): both lanes and the export table.
	bool applyState(const uint8_t* data, size_t bytes);
	/// Reap both lanes' parked devices; what either dropped, merged.
	device::RouteDrop service();
	/// Whether either lane holds anything — what the chip's count reads.
	[[nodiscard]] size_t deviceCount(uint16_t lane) const noexcept;

	/// **THE GATE OUTSIDE, PER LANE** (`ContainerOps::setOuterGate`). POST is handed the host's gate as it is. PRE is
	/// handed one that refuses this site's own exports, because a PRE control reading a PRE export would read its own
	/// lane through a one-tick cell.
	void setOuterGate(const device::OuterGate& gate);
	/// **THE HOST'S CELLS, FOR BOTH LANES** (rack roadmap §7's reader). Called before the site is offered, so no block is
	/// running either lane. It is how POST reads this site's exports.
	void setOuterSources(const device::OuterSources& outer);

	//--------------------------------------------------------------------------------------------//
	// THE EXPORT TABLE (backlog NZ)                                                               //
	//--------------------------------------------------------------------------------------------//
	//
	// A source in PRE is spelled outside the site as `Source::exported(owner, row)`. Each row names an inner source by
	// its id and output. Rows are minted on the message thread the first time a route needs one, saved with the body,
	// and read on the audio thread, which copies each row's value into a cell of this site's own after PRE has run. So
	// the host's pointer is into the site, never into a lane's engine, whose drain can replace the thing it points at.

	static constexpr size_t kNoRow = SIZE_MAX;
	/// MESSAGE THREAD. The row exporting PRE's `inner` source, minted if it has none: a PRE modulator's output, or one of
	/// PRE's macros, which are the site's one macro set (decision O7). `kNoRow` for anything else (a POST source is refused:
	/// it reaches only what follows it, decision O3), or when the table is full.
	[[nodiscard]] size_t exportRowFor(backplane::Source inner);
	/// The inner source a row names, or `Source::nobody()` for an empty row.
	[[nodiscard]] backplane::Source exportedSource(size_t row) const noexcept;
	/// AUDIO THREAD. The cell the host reads for a row, valid while this site lives; null past the table.
	[[nodiscard]] const float* exportCell(size_t row) const noexcept { return exports_.cell(row); }
	/// **RELEASE THE ROWS NO ROUTE READS ANY MORE** (backlog NZ: a site's own table, as a container's), MESSAGE THREAD —
	/// `ExportTable::sweep`'s rule: freed only once the host's audio thread has drained a publish made after the last
	/// reader went, and never a row minted and not yet routed. `usedRows` is indexed by row. Returns how many it freed.
	size_t sweepExports(const std::vector<bool>& usedRows, uint32_t published, uint32_t drained) {
		return exports_.sweep([&](size_t r) { return r < usedRows.size() && usedRows[r]; }, published, drained);
	}
	/// How many rows are minted.
	[[nodiscard]] size_t exportRowsUsed() const noexcept { return exports_.used(); }
	/// AUDIO THREAD, after PRE has run its block: every minted row's value into its cell.
	void publishExports() noexcept;

	//--------------------------------------------------------------------------------------------//
	// AUDIO THREAD — between a drain and the next                                                 //
	//--------------------------------------------------------------------------------------------//

	/// One lane's bus stage, with the host's clock and the note the slot's device hears.
	void process(InnerChain& lane, float* left, float* right, int frames, const device::TransportClock& clock,
	             const device::NoteContext& note);
	/// Drop both lanes' tails (the host's reset).
	void reset() noexcept;

	/// **HOW LATE THE SITE LEAVES ITS SLOT'S OUTPUT** — the two lanes in series with the device between them.
	[[nodiscard]] uint32_t latencySamples(double sampleRate) const noexcept {
		return pre_.latencySamples(sampleRate) + post_.latencySamples(sampleRate);
	}
	[[nodiscard]] bool holdsTail() const noexcept { return pre_.holdsTail() || post_.holdsTail(); }

	/// **THE FURNITURE'S DRY, WAITING FOR THE SITE** — what the slot's Mix blends against, delayed by the device's
	/// latency plus the site's. The site's own, so a slot without one allocates nothing for it.
	[[nodiscard]] device::CompensationDelay& dryDelay() noexcept { return dryDelay_; }

private:
	/// THE BODY, framed with its export table or bare; loaded into a new site or applied to a running one.
	bool readBody(const uint8_t* data, size_t bytes, bool apply);

	DeviceInstanceId owner_ = kNoInstance;
	/// ONE COUNTER FOR BOTH LANES (gap G3): the container table finds a route's lane by the inner id alone. Behind a
	/// pointer because the chains borrow it.
	std::unique_ptr<backplane::InstanceCounter> ids_;
	InnerChain pre_;
	InnerChain post_;
	device::CompensationDelay dryDelay_;
	double sampleRate_ = 0.0;
	int maxFrames_ = 0;
	/// The host's gate as it was handed, which PRE's gate defers to for everything but an export.
	device::OuterGate outerGate_ {};
	/// THE EXPORT TABLE — the container's (`ExportTable`, which was lifted from here), so a site's rows are minted,
	/// published, saved and released by the same rules.
	ExportTable exports_;
};

/// THE CONTAINER TABLE A SITE ANSWERS — `laneContainerOps` over its two lanes, the surface being the `SiteChains`. The
/// host's route verbs into a site take this, exactly as they take a rack device's.
[[nodiscard]] const device::ContainerOps& siteContainerOps() noexcept;

/// **A DESCRIPTOR THAT NAMES A SITE AS A CONTAINER** — no audio, no params: what the host's inner-route helpers
/// (`backplane::innerRoutesOf` and its siblings) are handed with a site as the surface, so a route into a site and a
/// route into a rack in a slot are the same road.
[[nodiscard]] const device::Descriptor& siteDescriptor() noexcept;

/// **`DeviceHost::siteExport` FOR ANY HOST**, over the running site a host files under `siteKeyOf(owner)` (`surface`,
/// null while none runs).
[[nodiscard]] inline backplane::Source exportFromSite(void* surface, DeviceInstanceId owner, backplane::Source inner) {
	auto* site = static_cast<SiteChains*>(surface);
	if (site == nullptr || site->owner() != owner) {
		return backplane::Source::nobody();
	}
	const size_t row = site->exportRowFor(inner);
	return row == SiteChains::kNoRow ? backplane::Source::nobody() : backplane::Source::exported(owner, row);
}

/// Whether a device of kind `d` offers a site at all, and which lanes. A container may (D3 lifted for the Rack and the
/// Chain, 2026-10-29); one whose lanes are its own declares `None`.
[[nodiscard]] inline device::Traits::SiteLanes siteLanesOf(const device::Descriptor& d) noexcept {
	if (d.audio == nullptr) {
		return device::Traits::SiteLanes::None;
	}
	return d.traits.siteLanes;
}

/// **A SITE'S BODY WITH EVERY LANE'S BYTES THROUGH `map`** (backlog OA) — the `SIT1` frame and its export table kept,
/// or a bare bundle (a site with no rows). The table names PRE's own ids, which nothing here changes. False for bytes
/// that are neither; an empty body is an empty site and maps to itself.
bool mapSiteBody(std::span<const uint8_t> bytes, LaneMap map, void* ctx, std::vector<uint8_t>* out);

} // namespace feathers::rack
