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

#include "backplane/endpoint.h"
#include "device/registry.h"
#include "rack/rack_state.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace feathers::rack {

//------------------------------------------------------------------------------------------------//
// A CONTAINER'S EXPORT TABLE (backlog NZ (b); downstream modulation, decision O2)                    //
//------------------------------------------------------------------------------------------------//
//
// A modulator inside a Chain or a Rack reaches the audio devices after the container in its host, spelled there
// `Source::exported(container, row)`. Each row names an inner source by its id and output (or a macro). Rows are minted
// on the message thread the first time a route needs one and saved in the container's body; the audio thread copies
// each row's value into a cell of the table's own at the end of the container's block, so the host's pointer is into
// the container's storage, never into an inner engine whose drain can replace what it points at. A site keeps the
// same shape for its PRE (`SiteChains`), which this table was lifted from.

class ExportTable {
public:
	static constexpr size_t kNoRow = SIZE_MAX;

	/// MESSAGE THREAD. The row for `inner` (already checked by the container to be one of its own modulator outputs or
	/// macros), minted if it has none; `kNoRow` when the table is full.
	size_t mint(backplane::Source inner) {
		// AN INNER HOLDER'S EXPORT MAY BE A ROW (backlog NZ, any depth): this table re-exports it one hop
		// further out, and the inner table stays the owner of its own row. Never an OUTER source: that reads inward.
		if (inner.isOuter() || inner == backplane::Source::nobody()) {
			return kNoRow;
		}
		const uint64_t want = pack(inner);
		size_t free = kNoRow;
		for (size_t r = 0; r < rows_.size(); ++r) {
			const uint64_t v = rows_[r].load(std::memory_order_acquire);
			if (v == want) {
				condemnedAt_[r] = 0; // wanted again before it was freed
				return r;
			}
			if (v == 0 && free == kNoRow) {
				free = r;
			}
		}
		if (free != kNoRow) {
			rows_[free].store(want, std::memory_order_release);
			seenUsed_[free] = false;
			condemnedAt_[free] = 0;
		}
		return free;
	}
	/// **RELEASE UNUSED ROWS** (backlog NZ, the open half of (b)), MESSAGE THREAD. A row is freed when the last route
	/// reading it has gone, and only once the AUDIO thread has drained a publish made after that route went: freeing it
	/// sooner lets `mint` hand the row to another source while the engine's old route list still reads it for a block,
	/// which is the two-slot handoff's own trap (a row number is a handle, not an identity).
	/// `inUse(row)` asks the host whether a route still names the row; `published` is the host's current publish
	/// sequence (routes included) and `drained` the last one its audio thread adopted (equal or later counts as caught
	/// up; compared as a signed distance so a wrap is no trap). A row that was minted and never routed is NOT freed:
	/// arming mints before the route exists, and it would be swept out from under the gesture. Returns how many rows
	/// were freed.
	template <typename InUse>
	size_t sweep(InUse&& inUse, uint32_t published, uint32_t drained) {
		size_t freed = 0;
		for (size_t r = 0; r < rows_.size(); ++r) {
			if (rows_[r].load(std::memory_order_acquire) == 0) {
				continue;
			}
			if (inUse(r)) {
				seenUsed_[r] = true;
				condemnedAt_[r] = 0;
			}
			else if (seenUsed_[r]) {
				if (condemnedAt_[r] == 0) {
					condemnedAt_[r] = static_cast<uint64_t>(published) + 1;
				}
				else if (static_cast<int32_t>(drained - static_cast<uint32_t>(condemnedAt_[r] - 1)) >= 0) {
					rows_[r].store(0, std::memory_order_release);
					seenUsed_[r] = false;
					condemnedAt_[r] = 0;
					++freed;
				}
			}
		}
		return freed;
	}
	/// How many rows are minted.
	[[nodiscard]] size_t used() const noexcept {
		size_t n = 0;
		for (const auto& row : rows_) {
			n += row.load(std::memory_order_acquire) != 0 ? 1 : 0;
		}
		return n;
	}
	/// The inner source a row names, or `Source::nobody()` for an empty row.
	[[nodiscard]] backplane::Source source(size_t row) const noexcept {
		const uint64_t v = row < rows_.size() ? rows_[row].load(std::memory_order_acquire) : 0;
		return v == 0 ? backplane::Source::nobody() : unpack(v);
	}
	/// AUDIO THREAD. The cell for a row, valid while the table lives; null past it.
	[[nodiscard]] const float* cell(size_t row) const noexcept { return row < cells_.size() ? &cells_[row] : nullptr; }
	/// AUDIO THREAD, after the container's block: every minted row's value into its cell. `resolve(ctx, source)` answers
	/// the inner cell, or null (a device gone: the cell reads 0).
	void publish(const float* (*resolve)(const void* ctx, backplane::Source s), const void* ctx) noexcept {
		for (size_t r = 0; r < rows_.size(); ++r) {
			const uint64_t v = rows_[r].load(std::memory_order_acquire);
			if (v == 0) {
				cells_[r] = 0.0f; // a freed row reads silence
				continue;
			}
			const float* c = resolve(ctx, unpack(v));
			cells_[r] = c != nullptr ? *c : 0.0f;
		}
	}
	/// THE BODY'S HALF: the minted rows as a patch spells them, and back. `load` replaces the whole table.
	void save(std::vector<RackPatch::Export>* out) const {
		out->clear();
		for (size_t r = 0; r < rows_.size(); ++r) {
			const uint64_t v = rows_[r].load(std::memory_order_acquire);
			if (v != 0) {
				const backplane::Source s = unpack(v);
				out->push_back({ static_cast<uint32_t>(r), s.isDevice() ? s.device : kNoInstance, s.index });
			}
		}
	}
	void load(const std::vector<RackPatch::Export>& in) {
		for (auto& row : rows_) {
			row.store(0, std::memory_order_release);
		}
		seenUsed_.fill(false);
		condemnedAt_.fill(0);
		for (const RackPatch::Export& e : in) {
			if (e.row < rows_.size()) {
				seenUsed_[e.row] = true; // a saved row had a route; if none comes back, it goes
				const backplane::Source s =
				    e.device == kNoInstance ? backplane::Source::core(e.index) : backplane::Source::output(e.device, e.index);
				rows_[e.row].store(pack(s), std::memory_order_release);
			}
		}
	}

private:
	/// A MACRO'S ROW is filed under an id no counter issues, because a core source's id (`kNoInstance`) would read as
	/// an empty row. The inner id in the high word and the output in the low.
	static constexpr DeviceInstanceId kMacroRowId = static_cast<DeviceInstanceId>(-1);
	/// THE WHOLE INDEX, scope bits and all, so a row naming an inner holder's export keeps which road it is (NZ, any depth).
	static constexpr uint64_t pack(backplane::Source s) noexcept {
		return (static_cast<uint64_t>(s.isDevice() ? s.device : kMacroRowId) << 32) | static_cast<uint64_t>(s.index);
	}
	static constexpr backplane::Source unpack(uint64_t v) noexcept {
		const auto id = static_cast<DeviceInstanceId>(v >> 32);
		const auto index = static_cast<uint32_t>(v & 0xffffffffu);
		return id == kMacroRowId ? backplane::Source::core(index) : backplane::Source { id, index };
	}

	std::array<std::atomic<uint64_t>, device::kSiteExportRows> rows_ {};
	std::array<float, device::kSiteExportRows> cells_ {};
	/// MESSAGE THREAD ONLY: whether `sweep` has seen a route read the row, and the publish (+1) it was first found unused at.
	std::array<bool, device::kSiteExportRows> seenUsed_ {};
	std::array<uint64_t, device::kSiteExportRows> condemnedAt_ {};
};


/// **A SOURCE INSIDE CONTAINER `holder`, AS ITS HOST NAMES IT** (backlog NZ (b)): `Source::exported(holder, row)` with
/// the row minted in the running container's table, or `Source::nobody()` (not a container that exports, none
/// running, not one of its modulators or macros, or a full table). `inner` is spelled as the container's records do.
[[nodiscard]] inline backplane::Source exportFromContainer(const device::Descriptor* desc, void* surface,
                                                          DeviceInstanceId holder, backplane::Source inner) {
	if (desc == nullptr || surface == nullptr || !device::exportsContainer(*desc) || desc->container->exportRow == nullptr) {
		return backplane::Source::nobody();
	}
	const size_t row = desc->container->exportRow(surface, inner);
	return row == ExportTable::kNoRow ? backplane::Source::nobody() : backplane::Source::containerExported(holder, row);
}

/// **RELEASE A CONTAINER'S UNUSED EXPORT ROWS** (backlog NZ): the rows of the container at `surface` that no route in
/// the host reads any more (`ExportTable::sweep`). `usedRows` is indexed by row. No-op for a container that exports nothing.
inline size_t sweepContainerExports(const device::Descriptor* desc, void* surface, const std::vector<bool>& usedRows,
                                    uint32_t published, uint32_t drained) {
	if (desc == nullptr || surface == nullptr || !device::exportsContainer(*desc) || desc->container->sweepExports == nullptr) {
		return 0;
	}
	return desc->container->sweepExports(surface, &usedRows, published, drained);
}
} // namespace feathers::rack
