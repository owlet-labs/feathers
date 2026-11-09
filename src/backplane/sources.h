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

#include "backplane/axes.h"
#include "backplane/routing.h"
#include "device/registry.h"

#include <cstddef>
#include <span>

// WHAT THE SOURCES ARE — one question, asked by index, answered the same way whether the source is a
// macro the host owns or an output a device publishes.
//
// THE POINT IS THAT THE CALLER DOES NOT FIND OUT WHICH. A source grid drawing a row, an Engine rail
// naming a route, a card deciding whether to draw a bipolar rest position — none of them has any
// business branching on "is this a macro or a device". They branch today because the answer lives in
// two places: the host's own table for the core half, and `Descriptor::outputs` for the device half.
// This joins them, and `device::SourceDesc` is already the type both halves speak.
//
// WHAT A HOST SUPPLIES IS A SPAN IT OWNS, exactly as `SlotView` borrows the slot table. The backplane
// allocates nothing and outlives nothing.
//
//------------------------------------------------------------------------------------------------//
// WHAT THIS DELIBERATELY DOES NOT ANSWER                                                           //
//------------------------------------------------------------------------------------------------//
//
// **WHETHER A SOURCE IS OFFERED TO A PLAYER.** Feathers has sources that exist, are routable in the
// table, and are shown to nobody: the per-sound weights are *"INTERNAL sources, they spend no ids,
// they are not enumerated to the host and they are not routable from outside"*, and the modulator
// strip builds its cards over `kMacroCount` alone so that arming refuses anything above it.
//
// That is a HOST decision about its own surface, and it is not derivable from anything here — a
// weight has a name and a polarity like any other source, and looks identical from this side. Putting
// an `offerable` flag in would be this header guessing at a rule it cannot check, and the guess would
// be wrong for the rack, whose sources are all macros and all offered.
//
// A FIELD IS EARNED BY A READER. When a shared source grid exists — it is host-side today — it will
// have an opinion, and that is the moment to decide where the opinion lives.

namespace feathers::backplane {

/// EVERY SOURCE IN ONE HOST, core and device, behind one question — asked BY ENDPOINT since kill-lanes
/// step 2: a host source by its index, a device's output by the device's id.
///
/// Holds two borrowed views and no storage. Cheap to construct per query, which is how the surfaces
/// use it: build one, ask it about the rows being drawn, drop it.
class SourceView {
public:
	SourceView() = default;

	/// `core` is the host's own sources in index order — index i IS host source i. `devices` is what the
	/// routing authority reads.
	///
	/// THE CORE SPAN MAY BE SHORTER THAN `axes.coreSources` AND THAT IS NOT AN ERROR. A host may
	/// reserve room it has not described — Feathers' weight rows exist whether or not anything has
	/// published a descriptor for them — and a source with no descriptor answers the same as a device
	/// that is not there: nullptr, not live.
	SourceView(std::span<const device::SourceDesc> core, DeviceView devices) noexcept
	    : core_(core), devices_(devices) {}

	/// WHAT THIS SOURCE IS, or nullptr when nothing publishes it — a host row nobody described, a device
	/// the host does not hold, or an output that device does not declare. None may be routed from and
	/// none may be drawn.
	const device::SourceDesc* descOf(Source s) const noexcept {
		if (!s.isDevice()) {
			return s.index < core_.size() ? &core_[s.index] : nullptr;
		}
		const device::Descriptor* desc =
		    devices_.descriptorOf != nullptr ? devices_.descriptorOf(devices_.host, s.device) : nullptr;
		if (desc == nullptr) {
			return nullptr;
		}
		return s.index < desc->outputs.size() ? &desc->outputs[s.index] : nullptr;
	}

	/// Whether anything publishes this source — `descOf`'s question, derived from it so the two cannot
	/// disagree about what a live source is.
	bool isLive(Source s) const noexcept { return descOf(s) != nullptr; }

	/// What this source is CALLED, or nullptr.
	const char* nameOf(Source s) const noexcept {
		const device::SourceDesc* desc = descOf(s);
		return desc != nullptr ? desc->name : nullptr;
	}

	/// Whether this source swings both ways, so a surface can draw its rest position. FALSE FOR A
	/// SOURCE THAT DOES NOT EXIST, which is the safe default.
	bool isBipolar(Source s) const noexcept {
		const device::SourceDesc* desc = descOf(s);
		return desc != nullptr && desc->bipolar;
	}

private:
	std::span<const device::SourceDesc> core_ {};
	DeviceView devices_ {};
};

} // namespace feathers::backplane
