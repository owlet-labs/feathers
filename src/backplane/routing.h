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
#include "backplane/endpoint.h"
#include "device/registry.h"

#include <cstddef>
#include <span>

// WHETHER A SOURCE MAY REACH A DESTINATION — the one authority, with the host's own rules taken out
// of it and handed back as a parameter.
//
// `plugin/params.h` states why there must be exactly one such authority: every road that can create a
// route asks it, and so does every surface that OFFERS one — the knob's armed glow is the same
// question as the drag's permission, and the honesty rule is violated the moment those two are
// computed separately. That argument is unchanged here. What changes is that the answer is now
// assembled from two halves that belong to different parties.
//
//   STRUCTURAL — the backplane's half. Are these indices in the table at all, and is there a live
//   device behind the ones that name devices? Derivable from the axes and from what is in the slots,
//   and identical at every host.
//
//   POLICY — the host's half. May THIS source reach THIS destination, given what the host knows about
//   its own parameter space? Feathers has exactly one such rule — a weight source may not reach a
//   puck destination — and the rack, having no core destinations at all, has none.
//
// THE SPLIT IS NOT COSMETIC AND THE ASYMMETRY IS THE POINT. A host cannot be asked about device
// liveness because it does not own the slot pool; the backplane cannot be asked about weights and
// pucks because it has never heard of either. Putting both behind one function was right when there
// was one host and would have been the thing that stopped there being two.

namespace feathers::backplane {

//------------------------------------------------------------------------------------------------//
// WHAT IS IN THE SLOTS                                                                             //
//------------------------------------------------------------------------------------------------//

/// THE DESCRIPTOR IN EVERY ROUTING LANE, as a view the host owns the storage for.
///
/// A SPAN AND NOT AN ARRAY, which is the whole reason this type exists rather than `DeviceSlots`
/// being reused: the array's length is `kMaxDeviceSlots`, a Feathers constant, and a second host has
/// a different one. The backplane allocates nothing and holds nothing — it borrows the host's table
/// for the duration of a question.
///
/// AND `slot` IS A ROUTING LANE, WHICH IS THE ONE THING IT MUST BE. Every caller here is asking about
/// the matrix, and the matrix is addressed by lane. Filling this by POSITION would agree with filling
/// it by lane on every chain that has only ever been appended to — which is every chain in every
/// fixture — and would disagree the first time a player dragged one device past another. That
/// sentence is `plugin/device_slots.h`'s, kept because the trap is the host's to fall into and this
/// is where a new host will read about it.
class SlotView {
public:
	// **THE RANK HALF IS GONE (kill-lanes step 5): `kUnranked`, the two-span constructor and
	// `rankAtLane` all stood here to serve one caller — the `mayRoute` overload that compared two
	// ranks, deleted in step 4b.** The 2026-09-05 routing rule (sources must be upstream of
	// destinations) is unchanged; what went is the claim that a RANK could answer it. A rank is a
	// position in ONE linearisation of a partial order and adding a route changes it, so the question
	// is reachability — `Graph::wouldCycle`, asked by device identity through `DeviceView`. What is
	// left here is what every remaining caller actually wants: WHAT is in a lane.

	SlotView() = default;
	/// DESCRIPTORS — a view for readers that ask WHAT is in a lane. `SourceView` and the two liveness
	/// predicates below are the consumers; none of them has ever needed an order.
	explicit SlotView(std::span<const device::Descriptor* const> at) noexcept : at_(at) {}
	/// **OR ASKED OF A HOST, LANE BY LANE** — for storage that is not one contiguous run: the rack engine's
	/// slots live in a pool that grows by chunks (unbounded devices), so there is no span to hand out.
	SlotView(const void* host, const device::Descriptor* (*at)(const void* host, size_t lane), size_t count) noexcept
	    : host_(host), atFn_(at), count_(count) {}

	/// The device in `slot`, or nullptr. A LANE PAST THE END ANSWERS THE SAME AS AN EMPTY ONE, and
	/// that is correct for both rather than merely convenient: neither has controls this build can
	/// name, so nothing may be routed to either.
	/// **BY LANE, AND THE NAME SAYS SO SINCE STEP 2.** This was `descriptorAt(slot)`, and `slot` meant
	/// a lane in both hosts even then — `DeviceSlots::at` has always been lane-indexed. What changed is
	/// that the RACK's device addresses became lanes too, so callers that had been passing a position
	/// and getting away with it stopped getting away with it. Four of them existed; the compiler saw
	/// none, because the signature had not moved. Hence the name.
	const device::Descriptor* descriptorAtLane(size_t lane) const noexcept {
		if (atFn_ != nullptr) {
			return lane < count_ ? atFn_(host_, lane) : nullptr;
		}
		return lane < at_.size() ? at_[lane] : nullptr;
	}

	size_t size() const noexcept { return atFn_ != nullptr ? count_ : at_.size(); }

private:
	std::span<const device::Descriptor* const> at_ {};
	const void* host_ = nullptr;
	const device::Descriptor* (*atFn_)(const void*, size_t) = nullptr;
	size_t count_ = 0;
};


//------------------------------------------------------------------------------------------------//
// THE HOST'S HALF                                                                                  //
//------------------------------------------------------------------------------------------------//

/// MAY THIS SOURCE REACH THIS DESTINATION, BY THE HOST'S OWN RULES — asked only for pairs the axes
/// have already admitted, so an implementation never has to bounds-check.
///
/// A PLAIN FUNCTION POINTER because the answer is a pure function of the two ends and nothing else.
/// Feathers' rule — a weight may not reach the puck — reads the two HOST ends and closes over no
/// state; a `std::function` here would buy the ability to capture, which is precisely the ability that
/// would let a policy go stale against the host it was built from.
///
/// **BY ENDPOINT SINCE KILL-LANES STEP 2**: a policy is about the host's own table, so it reads the host
/// ends (`!isDevice()`) and has no business with a device's id. It is asked only of pairs the axes and
/// the devices have already admitted.
///
/// A HOST WITH NO RULES PASSES `permitAll` RATHER THAN NULL. A null that means "permit" is a null
/// that also means "somebody forgot", and those must not be spelled the same — the same objection
/// this codebase raises everywhere else to a flag beside a pointer.
using CorePolicy = bool (*)(Source source, Destination destination);

/// The policy of a host that has no opinion about its own axes. What the rack passes: with no core
/// destinations there is no pair it could have a rule about.
constexpr bool permitAll(Source, Destination) noexcept {
	return true;
}

//------------------------------------------------------------------------------------------------//
// THE DEVICES, BY IDENTITY                                                                          //
//------------------------------------------------------------------------------------------------//

/// **WHAT A ROUTING HOST ANSWERS ABOUT ITS DEVICES, BY ID** — what is there, and whether a route would
/// close a cycle. `SlotView` asked both by lane; a route names a device, so the authority asks by device.
/// Function pointers over `void*`, the vocabulary's idiom.
struct DeviceView {
	const void* host = nullptr;
	/// The device `id`, or null for one the host does not hold.
	const device::Descriptor* (*descriptorOf)(const void* host, DeviceInstanceId id) = nullptr;
	/// **WOULD A ROUTE FROM `s` TO `d` HAVE TO BE READ BEFORE IT IS WRITTEN?** The host asks its graph
	/// (`Graph::wouldCycle`). A host that supplies none refuses every device-to-device route, which is
	/// the safe direction: "nothing routes" rather than "everything does".
	bool (*wouldCycle)(const void* host, Source s, Destination d) = nullptr;
	/// **IS THIS OUTER SOURCE UPSTREAM OF THIS HOST'S OWN NODE IN THE HOST OUTSIDE?** — rack roadmap §7.
	/// `s` is spelled the outer host's way (the scope bit already taken off). A host that is not inside
	/// another, or has not been handed its reader yet, supplies none and refuses every outer source.
	bool (*outerUpstream)(const void* outerHost, Source s) = nullptr;
	/// What `outerUpstream` is asked of — the ENGINE that holds the reader, which is not `host` (the
	/// graph) and must not be confused with it.
	const void* outerHost = nullptr;
};

/// **IS `source` — ONE OF THIS HOST'S OWN — UPSTREAM OF THE DEVICE `self`?** Rack roadmap §7: what a host
/// answers for a chain it holds, so the chain's INNER gate can admit an inner route reading this source.
/// The source must exist here (a core index in the table; a device output a modulator this host holds
/// publishes), and a route from it into `self` must not have to be read before it is written —
/// `mayRoute`'s own ordering half with the chain as the destination; the control index is not consulted
/// by that half. One function, so the two hosts and their records cannot answer it two ways.
inline bool sourceUpstreamOf(const Axes& axes, const DeviceView& devices, Source source, DeviceInstanceId self) {
	if (self == kNoInstance) {
		return false;
	}
	// **A SOURCE FURTHER OUT STILL**: upstream of `self` iff it is upstream of
	// this whole host in the host outside it — the same question, one hop out, through this host's own outer gate.
	if (source.isOuter()) {
		return devices.outerUpstream != nullptr && devices.outerUpstream(devices.outerHost, source.inner());
	}
	// **A SITE'S OWN EXPORT IS UPSTREAM OF THE SITE** (backlog NZ): a PRE source, read by the POST chain, which runs
	// after the device and so after PRE. Only a site asks with its holder as `self`, and only PRE sources are exported.
	// **AND OF ANY CONTAINER AFTER THE HOLDER** (NZ rung 5): a Chain or Rack (or a later device's site) placed after the
	// holder runs its inner chains inside its own audio pass, after the holder's PRE has published, so the devices inside
	// it read the export as the audio devices after the holder do (rung 4). The same two tests `Graph::admits` and the
	// cycle rule make of such a device: an audio device without a mod road, and one the holder comes before.
	if (source.isExport()) {
		if (source.exportRow() >= device::kSiteExportRows || devices.descriptorOf == nullptr) {
			return false;
		}
		const device::Descriptor* holder = devices.descriptorOf(devices.host, source.device);
		if (holder == nullptr || !device::exportsRoadOf(*holder, source)) {
			return false;
		}
		if (source.device == self) {
			// NOT THE HOLDER'S OWN: a site's POST is admitted by the site's own gate (`SiteChains::setOuterGate`), and a
			// Rack's or a Chain's chains asking with the same id must not reach its PRE (rule 5 stays out, 2026-10-29) nor
			// read its own inside back from outside.
			return false;
		}
		const device::Descriptor* container = devices.descriptorOf(devices.host, self);
		if (container == nullptr || container->audio == nullptr || container->mod != nullptr) {
			return false;
		}
		return devices.wouldCycle != nullptr && !devices.wouldCycle(devices.host, source, Destination::control(self, 0));
	}
	if (source.isDevice()) {
		const device::Descriptor* from = devices.descriptorOf != nullptr ? devices.descriptorOf(devices.host, source.device) : nullptr;
		if (from == nullptr || from->mod == nullptr || source.index >= from->outputCount()) {
			return false;
		}
	}
	else if (source.index >= axes.coreSources) {
		return false;
	}
	// **A HOLDER IS NOT UPSTREAM OF ITSELF, SAID OUTRIGHT** (trap T1; backlog HS). The line below asks
	// about `control(self, 0)` because the ordering half never consulted the control — and since a device
	// may DECLARE some of its own controls downstream of its outputs, it now does. That declaration is
	// about the holder's controls, not about the chain inside it (which reads the holder's outputs as its
	// CORE sources, backlog HM), so whatever control 0 happens to be must not answer for the chain.
	if (source.isDevice() && source.device == self) {
		return false;
	}
	return devices.wouldCycle != nullptr && !devices.wouldCycle(devices.host, source, Destination::control(self, 0));
}

/// **WHETHER THIS SOURCE MAY REACH THIS DESTINATION AT ALL — BY ENDPOINT.** The authority `mayRoute`
/// below states, asked of identities: a host end inside the host's table; a device end naming a device
/// the host holds, with such an output (and a modulator) or such a MODULATABLE control; the host's
/// policy; and, between two devices, **the source upstream of the destination**. The same four tests in the same order.
inline bool mayRoute(const Axes& axes, const DeviceView& devices, Source s, Destination d, CorePolicy core) {
	if (core == nullptr) {
		return false;
	}
	const device::Descriptor* from = nullptr;
	const device::Descriptor* to = nullptr;
	// **A SOURCE OUTSIDE THIS HOST** (rack roadmap §7): the host outside says whether it is upstream of
	// this whole host; the destination is tested as any other; there is no cycle to ask about, because
	// the source is not in this graph at all.
	if (s.isOuter()) {
		if (devices.outerUpstream == nullptr || !devices.outerUpstream(devices.outerHost, s.inner())) {
			return false;
		}
		if (!d.isDevice()) {
			return false; // an outer source into this host's own core is the outer host's route, not this one's
		}
		to = devices.descriptorOf(devices.host, d.device);
		if (to == nullptr || d.index >= to->params.size() || !to->params[d.index].modulatable) {
			return false;
		}
		return core(s, d);
	}
	if (s.isExport()) {
		// A SOURCE IN A DEVICE'S PRE CHAIN, EXPORTED AS THE DEVICE'S OWN (backlog NZ). It reaches the holder's own controls
		// and, out the back, the audio devices after it (`Graph::admits` says which; the cycle rule below says "after").
		// The site's POST reads it from inside (`sourceUpstreamOf`).
		from = devices.descriptorOf(devices.host, s.device);
		if (from == nullptr || !device::exportsRoadOf(*from, s) || s.exportRow() >= device::kSiteExportRows || !d.isDevice()) {
			return false;
		}
		if (d.device == s.device && !s.isSiteExport()) {
			return false; // a container's export never reaches its own controls (NZ (b)) — read from the bit, 2026-10-29
		}
		if (d.device != s.device) {
			const device::Descriptor* after = devices.descriptorOf(devices.host, d.device);
			if (after == nullptr || after->audio == nullptr || after->mod != nullptr) {
				return false;
			}
		}
	}
	else if (s.isDevice()) {
		from = devices.descriptorOf(devices.host, s.device);
		if (from == nullptr || from->mod == nullptr || s.index >= from->outputCount() || s.index >= axes.outputStride) {
			return false;
		}
	}
	else if (s.index >= axes.coreSources) {
		return false;
	}
	if (d.isDevice()) {
		to = devices.descriptorOf(devices.host, d.device);
		if (to == nullptr || d.index >= to->params.size() || !to->params[d.index].modulatable) {
			return false;
		}
	}
	else if (d.index >= axes.coreDestinations) {
		return false;
	}
	if (!core(s, d)) {
		return false;
	}
	// **THE ORDERING HALF OF THE RULE — A CYCLE, NOT A RANK COMPARISON (kill-lanes step 4b).** The
	// 2026-09-05 routing rule is that sources must be upstream of destinations. That was read as "the
	// source's rank is below the destination's", and a rank is a position in ONE arbitrary linearisation
	// of a partial order. Adding a route CHANGES that linearisation — so comparing current ranks refused
	// acyclic routes whenever the current order happened to run the other way, which is exactly what a
	// modulator added AFTER the audio device it modulates looks like. MEASURED at 4a by deleting the
	// tie-break that had been hiding it: five suites red, none of them cyclic.
	//
	// The question the rule actually means is whether the route would have to be read before it is
	// written, and that is reachability. It covers a device reaching itself as the shortest backwards
	// distance rather than as an exception. A node's bus does not carry its own device's output, and a
	// patch that still carries such a route has it thrown away.
	//
	// **A HOST THAT ANSWERS NOTHING REFUSES**, deliberately, in the safe direction. And it applies to a
	// CORE end too since 4a: the host's own node owns its destinations, so a route looping through the
	// voice is refused by the same line rather than escaping the rule for want of a device id.
	if (devices.wouldCycle == nullptr || devices.wouldCycle(devices.host, s, d)) {
		return false;
	}
	return true;
}

//------------------------------------------------------------------------------------------------//
// THE AUTHORITY                                                                                    //
//------------------------------------------------------------------------------------------------//

// **A SECOND `mayRoute` OVER `SlotView` AND FLAT COORDINATES STOOD HERE, AND IS DELETED (kill-lanes
// step 5).** It asked the same four tests — bounds, the host's rules, each axis's liveness — and then
// compared two RANKS for the ordering half. Step 4b established that a rank comparison is not the rule:
// a rank is a position in ONE linearisation of a partial order, and adding a route changes the
// linearisation, so it refused acyclic routes whose current order happened to run the other way.
//
// It survived step 4 only because cases used it, which is the wrong reason for a rule to exist — and
// `device_registry.h` already named two implementations of one question as the shape that produced the
// arm-glow bug. Its callers are on the endpoint authority above; Feathers' `maySourceRouteTo`, which
// wrapped it, went with it.
//
// THE AUTHORITY IS THE ONE ABOVE: by endpoint, asking the host whether the route would close a cycle.

//------------------------------------------------------------------------------------------------//
// THE SAFE DOOR FOR A CALLER HOLDING A CONTROL INDEX                                               //
//------------------------------------------------------------------------------------------------//

} // namespace feathers::backplane
