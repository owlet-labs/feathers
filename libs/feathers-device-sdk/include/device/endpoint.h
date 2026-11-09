/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include "device/instance_id.h"
#include "device/transfer_curve.h"

#include <compare>
#include <cstddef>
#include <cstdint>

// THE ENDS OF A ROUTE, BY IDENTITY (kill-lanes §2.1).
//
// The system is a graph: addresses are not sequential and ordering comes only from the graph, with no
// lanes. A device end names the DEVICE, by its `DeviceInstanceId` — never reused, never
// renumbered by an insert or a move — and the device's own name for the output or control. A host end is
// an index into the host's own table (Feathers' macros, weights and parameters; the rack's macros): a
// host's fixed contract, not a device address, and nothing inserts into it at runtime.
//
// **IN THE DEVICE VOCABULARY** (kill-lanes step 2): a widget asks how far a SOURCE reaches it
// (`Modulatable::amountFor`), so the source's type is part of what a device and its surface speak. The
// backplane names these `backplane::Source` and friends (`backplane/endpoint.h`).
//
// `kNoInstance` IN `device` MEANS "THE HOST'S", so one pair of words is either kind, and a value of these
// types is eight bytes, trivially copyable and totally ordered — what a published list and a sort want.

namespace feathers::device {

/// WHERE A ROUTE COMES FROM — a device's output, or one of the host's own sources.
struct Source {
	/// The device, or `kNoInstance` for one of the host's own sources.
	DeviceInstanceId device = kNoInstance;
	/// The device's OUTPUT (its own declared index), or the host's source index.
	uint32_t index = 0;

	static constexpr Source core(size_t i) noexcept { return { kNoInstance, static_cast<uint32_t>(i) }; }
	static constexpr Source output(DeviceInstanceId d, size_t out) noexcept {
		return { d, static_cast<uint32_t>(out) };
	}

	/// **A SOURCE NOTHING PUBLISHES — the "nobody" a surface excepts when no source is armed.** A control
	/// draws a "routed elsewhere" mark for every route but the armed source's; with no mod panel there is
	/// no armed source, and the honest exception is one that no route can name, so EVERY route counts as
	/// elsewhere. It is a host index past any host's own table.
	///
	/// HERE RATHER THAN IN EACH SEATING: four of them had spelled it out, and two had spelled it
	/// `core(0)` — macro 1 — so a control driven by macro 1 alone reported itself undriven (backlog DX).
	/// One fact, one home.
	static constexpr Source nobody() noexcept { return core(static_cast<uint32_t>(-1)); }

	/// **THE n-th OUTPUT OF A DEVICE NO CHAIN HOLDS — what a DORMANT SLOT answers when asked what it
	/// publishes.**
	///
	/// **THE OBVIOUS ANSWER IS A TRAP AND BOTH LANES FELL INTO IT SEPARATELY.** `kNoInstance` is the
	/// HOST's own device number, so `Source::output(kNoInstance, 0)` is `core(0)` — MACRO 1. A slot
	/// holding nothing would then have its geometry, its arming and its route lookups asked about the
	/// first macro. An id no counter issues keeps it a DEVICE source (`isDevice()` stays true) that no
	/// chain can hold, which every reader already answers as "not mine" by the lookup that answers
	/// "mine".
	///
	/// **HERE RATHER THAN IN EACH LANE, FOR THE REASON `nobody()` IS HERE**: `ModPanel` and `FxLane`
	/// had it spelled out verbatim with the whole rationale twice over, which is two places for the
	/// next lane to copy from and two places for one of them to be got wrong. One fact, one home.
	static constexpr Source dormantOutput(size_t out) noexcept {
		return { static_cast<DeviceInstanceId>(-1), static_cast<uint32_t>(out) };
	}

	[[nodiscard]] constexpr bool isDevice() const noexcept { return device != kNoInstance; }

	//--------------------------------------------------------------------------------------------//
	// A SOURCE OUTSIDE THIS HOST — rack roadmap §7.                        //
	//--------------------------------------------------------------------------------------------//
	//
	// A chain device is a rack INSIDE a host, and a control inside it may read a source of the host
	// OUTSIDE — the outer MSEG, the outer macro. The design is the kill-lanes pull: the route lives
	// with its reader (the inner engine's table), and its source is spelled in the OUTER host's own
	// vocabulary with ONE bit of scope, so it cannot collide with an inner id. The bit rides in
	// `index` so the shape, the ordering, the codec (which writes `index` whole) and every equality
	// stay what they were. `nobody()` has every bit set and is not outer.
	static constexpr uint32_t kOuterBit = 1u << 30;
	static constexpr Source outerCore(size_t i) noexcept {
		return { kNoInstance, static_cast<uint32_t>(i) | kOuterBit };
	}
	static constexpr Source outerOutput(DeviceInstanceId d, size_t out) noexcept {
		return { d, static_cast<uint32_t>(out) | kOuterBit };
	}
	[[nodiscard]] constexpr bool isOuter() const noexcept {
		return (index & kOuterBit) != 0 && index != static_cast<uint32_t>(-1);
	}
	//
	// **ANY NUMBER OF HOSTS OUT** (modulation works more than two levels deep). The bit says
	// "outside"; a five-bit count beside it says how many MORE hosts out, so a source is spelled with the hops between
	// its reader and its host and nothing else — `asOuter()` adds one, `inner()` takes one off, and each host's reader
	// hands the rest to the host outside it (`RackEngine::sourceCell`, the gates' `upstream`).
	//
	// **FIVE BITS, BITS 23–27, SO MODULATION REACHES AS DEEP AS NESTING DOES**. Four bits stopped at 16
	// hosts while `rack::kMaxDepth` lets a device sit 20 in, so a host macro silently failed to reach depths 17–20 —
	// a wall where the recursion breaks, which users must not hit. `rack_device.h` asserts `kMaxOuterHops >= kMaxDepth`, so raising the nesting limit past this field
	// fails to compile rather than quietly re-opening the gap. Bits 28 (container export), 29 (site export) and 30
	// (outer) sit above it; every index under it (a core index, an output, an export row) is far below 2^23.
	static constexpr uint32_t kOuterDepthShift = 23;
	static constexpr uint32_t kOuterDepthMask = 0x1Fu << kOuterDepthShift;
	/// THE MOST HOSTS OUT A SOURCE MAY BE — the bit and thirty-one more.
	static constexpr uint32_t kMaxOuterHops = 32;
	/// How many hosts out: 0 for a source of this host.
	[[nodiscard]] constexpr uint32_t outerHops() const noexcept {
		return isOuter() ? 1u + ((index & kOuterDepthMask) >> kOuterDepthShift) : 0u;
	}
	/// The same source as the host ONE HOP OUT spells it — still outer if it lives further out.
	[[nodiscard]] constexpr Source inner() const noexcept {
		if ((index & kOuterDepthMask) != 0 && isOuter()) {
			return { device, index - (1u << kOuterDepthShift) };
		}
		return { device, index & ~kOuterBit };
	}
	/// This source as a host ONE HOP IN would spell it; `nobody()` past `kMaxOuterHops`.
	[[nodiscard]] constexpr Source asOuter() const noexcept {
		if (index == static_cast<uint32_t>(-1)) {
			return *this;
		}
		if (!isOuter()) {
			return { device, index | kOuterBit };
		}
		if (outerHops() >= kMaxOuterHops) {
			return { kNoInstance, static_cast<uint32_t>(-1) };
		}
		return { device, index + (1u << kOuterDepthShift) };
	}

	//--------------------------------------------------------------------------------------------//
	// A SOURCE INSIDE A DEVICE'S SITE, EXPORTED AS ITS OUTPUT — backlog NZ.                         //
	//--------------------------------------------------------------------------------------------//
	//
	// The other direction from `kOuterBit`: a modulator in a device's PRE chain reaches the device and its
	// POST chain. The design keeps the pull shape — the
	// route lives with its reader — so the source is spelled as an output of the CONTAINER'S node, `device`
	// the device that holds the site and `index` a row of the site's export table with this bit on. A row,
	// not the inner id itself, because a row is a stable small name that a table can map to any inner source
	// (decision O2). In the host it is a source like any other; inside the site's POST lane it is spelled
	// `asOuter()` of that, through the reader §7 already built.
	static constexpr uint32_t kExportBit = 1u << 29;
	static constexpr Source exported(DeviceInstanceId holder, size_t row) noexcept {
		return { holder, static_cast<uint32_t>(row) | kExportBit };
	}
	//
	// **A SOURCE INSIDE A CHAIN OR A RACK, EXPORTED AS ITS OUTPUT** (backlog NZ (b)) has a bit of its OWN:
	// a Rack or a Chain may hold a site too, so one holder has TWO export tables, each counting its rows from 0. The
	// road is read from the bit — never from what kind of device the holder is — so row 0 of the site and row 0 of
	// the container are two sources, two cells and two codec kinds.
	static constexpr uint32_t kContainerExportBit = 1u << 28;
	static constexpr Source containerExported(DeviceInstanceId holder, size_t row) noexcept {
		return { holder, static_cast<uint32_t>(row) | kContainerExportBit };
	}
	/// Either road out of a holder: its site's PRE, or its container's inside.
	[[nodiscard]] constexpr bool isExport() const noexcept { return isSiteExport() || isContainerExport(); }
	[[nodiscard]] constexpr bool isSiteExport() const noexcept {
		return device != kNoInstance && (index & kExportBit) != 0 && index != static_cast<uint32_t>(-1);
	}
	[[nodiscard]] constexpr bool isContainerExport() const noexcept {
		return device != kNoInstance && (index & kContainerExportBit) != 0 && index != static_cast<uint32_t>(-1);
	}
	/// The export table's row this source names (the scope bits off).
	[[nodiscard]] constexpr size_t exportRow() const noexcept {
		return index & ~(kExportBit | kContainerExportBit | kOuterBit | kOuterDepthMask);
	}

	friend constexpr auto operator<=>(const Source&, const Source&) = default;
};

/// WHERE A ROUTE ARRIVES — a device's control, or one of the host's own parameters.
struct Destination {
	/// The device, or `kNoInstance` for one of the host's own parameters.
	DeviceInstanceId device = kNoInstance;
	/// **The device's CONTROL INDEX** — its own name for the control, the file's spelling too
	/// (`rack/rack_state.h`) — not the modulatable ordinal, which existed only to pack a column. Or the
	/// host's parameter index.
	uint32_t index = 0;

	static constexpr Destination core(size_t i) noexcept { return { kNoInstance, static_cast<uint32_t>(i) }; }
	static constexpr Destination control(DeviceInstanceId d, size_t c) noexcept {
		return { d, static_cast<uint32_t>(c) };
	}

	/// **A DESTINATION NOTHING HOLDS — the twin of `Source::nobody()`, and it exists because a default one
	/// is not it.** `Destination {}` is `{kNoInstance, 0}`, which is a REAL host destination: the host's
	/// first parameter. So "no destination" cannot be spelled by leaving one alone, and every place that
	/// needs the idea — a refused route whose end the file wrote under no device, a host end whose kind the
	/// codec refuses outright (`rack/rack_state.h`'s `RouteRefusals`) — must say it the same way or two
	/// readers will disagree about whether core 0 was meant. A host index past any host's own table, as
	/// `Source::nobody()` is.
	static constexpr Destination nobody() noexcept { return core(static_cast<uint32_t>(-1)); }

	[[nodiscard]] constexpr bool isDevice() const noexcept { return device != kNoInstance; }

	friend constexpr auto operator<=>(const Destination&, const Destination&) = default;
};

/// ONE ROUTE — the one shape a route has in every place it is held: the record, the published list, the
/// file (through a host's codec), the widget that draws its ring and the undo step that puts it back.
struct Route {
	Source source;
	Destination destination;
	float amount = 0.0f;
	/// THE CURVE THE SOURCE IS READ THROUGH before the amount (`device/transfer.h`). Lin is today's road.
	Transfer transfer = Transfer::Lin;

	friend bool operator==(const Route&, const Route&) = default;
};

} // namespace feathers::device
