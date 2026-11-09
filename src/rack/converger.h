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

#include "backplane/chunked_pool.h"
#include "backplane/container_bodies.h"
#include "rack/chain_state.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace feathers::rack {

/// **AN ENGINE BECOMES WHAT ITS RECORD SAYS** — the one road by which a desired state (`ChainState`)
/// reaches a `RackEngine`, for every host: Feathers' lane and strip, the standalone rack, a rack in a
/// rack. It was `LaneRack`'s private mirror; the 2026-09-11 decision (desired state, for both hosts) made it
/// the protocol, so it lives here.
///
/// TWO HALVES ACROSS ONE HANDOVER:
///   - `provision` (MESSAGE THREAD) builds what the state wants and the engine does not have — a device
///     whose kind, tag, instance id or container body changed, and nothing else: a knob moving keeps
///     the instance and its memory. Built devices wait in one mailbox, each naming its device.
///   - `converge` (AUDIO THREAD, at a drain — no block in flight) adopts what arrived, removes what is
///     no longer wanted, puts the engine's order in the state's (chains in index order, each in its own
///     order — a reorder MOVES a device and keeps its state) and writes every row.
/// What the audio thread is done with goes back on a lock-free list for `reap`.
///
/// **BY ID, END TO END (kill-lanes step 3a, and 3b finished it).** A device is matched by its instance id
/// at every step, and the engine keeps it UNDER THAT ID in a slot it chooses (`RackEngine::adoptDevice`).
/// The state is read as what it is — each chain's devices in order — and no lane number is read at all.
/// The slot never comes back out of here.
///
/// **A REFUSAL IS NOT HIDDEN.** An arrival the engine could not take (the parking full, the engine
/// declining) goes back whole, and `reap` forgets what it built; a device the drain removed is reported
/// (`departed_`) and forgotten too. The next `provision` builds either again rather than believing in a
/// device that never landed or has left. The state is still what the hand made, and the engine keeps
/// converging toward it.
class Converger {
public:
	/// WHETHER A DEVICE MAY RUN IN A CHAIN — the host's policy (Feathers: audio devices in its fx lane,
	/// modulators and note devices in its strip; the rack: anything). A device that may not is kept in
	/// its record and not run.
	using Fits = bool (*)(const device::Descriptor& desc, size_t chain);

	/// Every device may run everywhere — the rack's policy.
	static bool fitsAnywhere(const device::Descriptor&, size_t) { return true; }

	/// ALLOCATES (a mailbox per lane) — construct where allocation is allowed. `engine` must outlive this.
	Converger(RackEngine& engine, Fits fits);
	~Converger();
	/// **EVERYTHING IN FLIGHT FREED AND THE ENGINE TOLD TO FORGET IT, NOW — the destructor's work, callable early and
	/// once** (later calls, and the destructor after one, do nothing). Its reap destroys what the engine parked, and a
	/// container dying there runs its own service (a nested rack's bridge, its converger, its slot link), which can call
	/// back into whatever HOSTS this converger. So a host whose members those callbacks reach calls this FIRST in its
	/// own destructor, while every member is still alive, rather than leaving it to member-destruction order
	/// (`RackBridge::~RackBridge`). Message thread, no block running.
	void shutdown() noexcept;
	Converger(const Converger&) = delete;
	Converger& operator=(const Converger&) = delete;

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD                                                                               //
	//--------------------------------------------------------------------------------------------//

	/// BUILD WHAT `want` NEEDS AND THE ENGINE DOES NOT HAVE, and post it for the next `converge`. Reaps
	/// first. Returns how many were built. `bodies` holds container contents by instance id (null: none).
	///
	/// **`forPublish` IS THE PUBLISH `want` WENT OUT AS** (`PublishedChainState::sequence`, after the
	/// publish) — so a drain still converging to an OLDER state knows an arrival is from its future and
	/// keeps it rather than handing it back. MEASURED, the first time a hand added while blocks ran: an
	/// arrival that reached the mailbox before the drain had its state was taken, wanted by nothing it
	/// knew, handed back — and never built again, because this side had already built it. One add in
	/// three lost. A host with no sequence (0) keeps the old rule: an unmatched arrival goes back.
	size_t provision(const ChainState& want, ContainerBodies* bodies, uint32_t forPublish);

	/// **HOW A CHAIN THIS HOST PROVISIONS IS TOLD ITS HOST** — rack roadmap §7. Called by the bridge once;
	/// every container built or re-attached by `provision` is then handed `make(host, its id)` BEFORE its
	/// body is loaded, so a loaded patch's outer routes meet the gate. Null: containers get no gate.
	void setOuterGateMaker(device::OuterGate (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept {
		makeGate_ = make;
		gateHost_ = host;
	}
	/// **EVERY LIVE CONTAINER'S OWN SERVICE, AND NOTHING ELSE** (host slots T11) — what `provision` does for a
	/// container already running, without building, reaping or forgetting anything: the host's slot visit runs
	/// it so a nested level pulls its bindings, mirrors the host's writes and adopts, even on a visit that is
	/// not an editor's tick (a host's `request_callback`, a save). What a chain drops is kept, as `provision`
	/// keeps it. Message thread.
	void serviceContainers(const ChainState& want, ContainerBodies* bodies);
	/// **HOW A CONTAINER THIS HOST PROVISIONS REACHES THE HOST SLOT POOL** — host slots T11, the same road as the
	/// gate: every container built or re-filed by `provision` is handed `make(host, its id)` beside the gate,
	/// BEFORE its body is loaded, so the devices a loaded body builds already have their link. Null: none.
	void setSlotLinkMaker(device::HostSlotLink (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept {
		makeLink_ = make;
		linkHost_ = host;
	}
	/// **WHAT THE CHAINS THIS HOST HOLDS DROPPED AT THEIR SERVICE** (`ContainerOps::service`, called by
	/// `provision` for every live container), accumulated until the host takes them to say — the first
	/// pair names the CHAIN as the destination (control 0) and the outer source as this host spells it,
	/// which is what the host's notice can name; the exact inner pair is the chain's own editor's to say.
	[[nodiscard]] RouteRefusals takeOuterDrops() noexcept {
		const RouteRefusals out = outerDrops_;
		outerDrops_ = RouteRefusals {};
		return out;
	}

	/// DESTROY WHAT THE AUDIO THREAD HAS FINISHED WITH — the engine's parked devices, and provisions it
	/// adopted (husks) or declined.
	void reap() noexcept;
	/// **EVERY CONTAINER THIS CONVERGER GATED, UNHOOKED FROM ITS HOST** (backlog PM): handed an empty `OuterGate`, so no
	/// teardown below — a nested rack's own service, run as its converger dies — walks an upstream test out into a host
	/// that is going. Called by the host's bridge FIRST in its destructor: the gates point into its record, and the
	/// containers live in an engine that outlives it (`rack_device.cpp`'s `State` frees the bridge before the engine).
	void unhookOuterGates() noexcept;
	/// **AND EVERY CONTAINER THIS CONVERGER LINKED TO A SLOT POOL, UNLINKED** (rack-teardown-uaf) — handed an empty
	/// `HostSlotLink`, which each level passes down to its own containers (`RackBridge::setHostSlots`). A link points at
	/// the pool's OWNER (`SlotOwner`), held by the outermost bridge, and the containers live in an engine that outlives
	/// that bridge: unlinked here, a nested rack's teardown (its converger's reap services its sites, and each site's
	/// bridge pulls its bindings through the link) asks the owner nothing. Called beside `unhookOuterGates`, by whoever
	/// owns the pool or the gates' host, before that dies.
	void unhookSlotLinks() noexcept;
	/// **EVERY SITE THIS CONVERGER HOLDS, HANDED ITS LINK AGAIN** from the maker (`setSlotLinkMaker`) — for a host whose
	/// own link just moved (`RackBridge::setHostSlots`): a site is not a row in the record, so the host's walk of its
	/// containers does not reach it, and an unlinked level must not leave its sites linked to a pool it no longer has.
	void relinkSites() noexcept;

	/// FORGET EVERY PROVISION NOT YET ADOPTED — before the engine is re-prepared, since a device built at
	/// the old rate must not be adopted at the new one. The next `provision` builds them again.
	void dropPending() noexcept;

	/// **WHAT THE ENGINE ALREADY RUNS IS WHAT `state` SAYS** — for a host whose record was built FROM an
	/// engine already holding devices (a bridge over a populated engine). Each device `state` names that
	/// the engine already runs under the same descriptor is recorded as that device, so the next `converge` keeps it
	/// rather than rebuilding it. MESSAGE THREAD, with no block running — it writes the drain's record.
	void adoptRunning(const ChainState& state) noexcept;

	//--------------------------------------------------------------------------------------------//
	// AUDIO THREAD — a drain point                                                                 //
	//--------------------------------------------------------------------------------------------//

	/// THE ENGINE BECOMES `want`: arrivals, departures, order, rows. An arrival is only adopted by a lane
	/// naming the device it was built for. One built for a LATER publish than `published` (the state this
	/// drain holds) is put back for a later drain; one built for this publish or an earlier one that this
	/// state does not want is stale, and is handed back — never installed under another device's name.
	void converge(const ChainState& want, uint32_t published);

	/// **WOULD `converge(want, published)` CHANGE NOTHING?** (backlog NR's first cheap win) — true only when the
	/// last converge was of this same publish and left nothing behind it: no arrival in the mailbox or held for
	/// later, no removal the parking refused, no reorder that failed, no growth waiting for a drain. Then the
	/// engine already IS the state, and a drain may skip the pass (most of an empty chain's ~1 µs a block).
	///
	/// **ONLY A HOST WHOSE STATE CHANGES ONLY WITH ITS PUBLISH MAY ASK** — `RackBridge`, which converges a snapshot
	/// of a sequenced publish. `LaneRack`'s value road converges values under `published == 0` that change with
	/// no publish at all, so it never asks. AUDIO THREAD, at a drain point.
	[[nodiscard]] bool settledAt(uint32_t published) const noexcept {
		return settled_ && settledFor_ == published && held_ == nullptr && !engine_.siteOffersPending()
		       && inbox_.load(std::memory_order_acquire) == nullptr && !engine_.growthPending();
	}

	/// THE DESCRIPTOR THE ENGINE RUNS FOR DEVICE `id` AS OF THE LAST `converge`, or null.
	[[nodiscard]] const device::Descriptor* runningDescriptorOf(DeviceInstanceId id) const noexcept {
		const size_t slot = engine_.slotOf(id);
		return slot < live_.size() && live_[slot].instanceId == id ? live_[slot].desc : nullptr;
	}

	/// HOW MANY PROVISIONS ARE WAITING — posted and not yet taken, and taken and held for a later drain.
	[[nodiscard]] size_t pendingCountForTest() const noexcept;

	/// **DOES DEVICE `row` (of kind `desc`, in `state`) NEED ITS VOICE INSTANCES BUILT?** (backlog NS) — in an engine
	/// running `voices` voices, an audio device with a per-voice road whose Poly the record holds ON, or whose Poly
	/// the AUDIO THREAD can flip with no message-thread visit in between: a modulatable switch (a route moves it in
	/// the resolve) or one a host slot drives (`applyHostSlots` writes it at the top of a block). Those keep the eager
	/// road, so no flip ever finds its instances missing. Asked on both sides of the handover with the same
	/// arguments, so the provision and the drain cannot disagree. Allocates nothing.
	[[nodiscard]] static bool wantsVoices(const device::Descriptor& desc, const DeviceState& row, const ChainState& state,
	                                      size_t voices) noexcept;
	/// **THE ROAD BEFORE BACKLOG NS, FOR A CASE TO COMPARE AGAINST**: every per-voice device built with its voices
	/// whatever its switch says. Set before the first `provision`, never while blocks run.
	void setEagerVoicesForTest(bool eager) noexcept { eagerVoices_ = eager; }

	//--------------------------------------------------------------------------------------------//
	// CHAIN ACCESS SITES (backlog NR) — MESSAGE THREAD                                            //
	//--------------------------------------------------------------------------------------------//

	/// **THE SITE THIS CONVERGER HOLDS FOR DEVICE `id`, or null** — built when the carrier wants one for it
	/// (`siteKeyOf(id)` filed), whether or not the engine has installed it yet. What a host's site editor and its
	/// save read. Valid until the device leaves the state or its site is unwanted, and an editor pins it
	/// (`ContainerBodies::pin`) against the free.
	[[nodiscard]] SiteChains* siteFor(DeviceInstanceId id) const noexcept;
	/// How many sites this converger holds, wanted or waiting to be freed.
	[[nodiscard]] size_t siteCountForTest() const noexcept { return sites_.size(); }

private:
	struct Posted {
		RackEngine::Provision provision;
		/// WHICH DEVICE IT WAS BUILT FOR — see `converge`.
		DeviceInstanceId instanceId = kNoInstance;
		/// WHICH PUBLISH IT WAS BUILT FOR — see `provision`. 0: the host keeps no sequence.
		uint32_t forPublish = 0;
		/// WHEN IT WAS POSTED — the later of two for one device supersedes the earlier.
		uint64_t serial = 0;
		/// **WHETHER THE MESSAGE THREAD MUST FORGET HAVING BUILT THIS** — decided by the drain, which is the
		/// only side that knows whether the device ended up running. Handing a post back is not by itself
		/// evidence that it never landed: a post for a device that IS running (superseded, or simply not
		/// needed because the device was adopted under that id in this same drain) must NOT be forgotten, or
		/// the message thread rebuilds it, the next drain throws the rebuild away, and the two of them do
		/// that forever. See `reap`.
		bool forget = false;
		Posted* next = nullptr;
		/// **A POST OF VOICE INSTANCES ALONE, FOR A DEVICE ALREADY BUILT** (backlog NS): `provision.voices` to adopt,
		/// or EMPTY to ask for the slot's voices back. Never `live()` (no bus storage), so no add takes it.
		bool voicesOnly = false;
		/// WHAT THE DRAIN SAID OF A VOICE POST — whether the device's slot holds voices after it (`voicesNow`), and
		/// whether it said anything at all (`stated`: a superseded post says nothing). `reap` believes it.
		bool stated = false;
		bool voicesNow = false;
		/// **THE ENGINE'S BLOCK EPOCH WHEN THE DRAIN HANDED IT BACK** (`retire`; 2026-11-07, per-device output counts). A post
		/// handed back after an adopt is a husk holding the slot's LAST device's buffers — its outputs among them, which an
		/// input re-taken at this same drain may have pointed into a moment before. `reap` frees it only once the epoch has
		/// moved past a whole block after this one (`epochPassed`): the old storage outlives the block that let it go.
		uint64_t retiredAt = 0;
	};

	/// WHAT A DEVICE IS, BY IDENTITY — the same four facts on both sides of the handover, so the two
	/// cannot disagree about whether a device changed.
	struct Identity {
		const device::Descriptor* desc = nullptr;
		device::Tag tag {};
		DeviceInstanceId instanceId = kNoInstance;
		uint64_t bodyGeneration = 0;
		/// **WHETHER THE MESSAGE THREAD BELIEVES THIS DEVICE'S VOICE INSTANCES BUILT** (backlog NS), and the serial of
		/// the post that last made it so — a drain's report on an OLDER post never overrules a newer one. Not part
		/// of what the device IS: a Poly flip never rebuilds a device, so `==` leaves both out.
		bool voices = false;
		uint64_t voiceSerial = 0;
		friend bool operator==(const Identity& a, const Identity& b) noexcept {
			return a.desc == b.desc && a.tag == b.tag && a.instanceId == b.instanceId && a.bodyGeneration == b.bodyGeneration;
		}
	};

	/// WHAT `want` ASKS OF ONE ROW OF `chain`: a descriptor, if the row's tag is one this build can run
	/// in that chain; null otherwise.
	const device::Descriptor* wantedAs(const DeviceState& row, size_t chain) const;
	/// THE ROW `want` HOLDS FOR DEVICE `id` AND ITS CHAIN, or null.
	static const DeviceState* rowOf(const ChainState& want, DeviceInstanceId id, size_t* chain) noexcept;

	/// AUDIO THREAD: hand a provision back. A lock-free push; the consumer takes the whole list at once,
	/// so there is no ABA to guard. `forget` says whether the message thread must stop believing it built
	/// this device — see `Posted::forget`; the drain decides it from whether the engine runs that device.
	void retire(Posted* posted, bool forget) noexcept;
	/// AUDIO THREAD: say a device left the engine, so the message thread's mirror forgets it and a state
	/// that wants it again builds it again. See `departed_`.
	void depart(DeviceInstanceId id) noexcept;

	/// `wantsVoices` over this engine's voices — or, under `setEagerVoicesForTest`, every device that has a road.
	[[nodiscard]] bool wants(const device::Descriptor& desc, const DeviceState& row, const ChainState& state) const noexcept {
		return eagerVoices_ ? engine_.voiceCount() > 0 && desc.audio != nullptr && desc.traits.perVoiceControl >= 0
		                    : wantsVoices(desc, row, state, engine_.voiceCount());
	}

	RackEngine& engine_;
	bool shutDown_ = false; ///< `shutdown` has run: the destructor has nothing left to do
	Fits fits_;
	bool eagerVoices_ = false;
	/// **THE MAILBOX — ONE LIST FOR EVERY DEVICE, BY ID** (kill-lanes step 3a; it was one mailbox per
	/// lane). The message thread pushes; the audio thread takes the whole list at once, so there is no ABA.
	std::atomic<Posted*> inbox_ { nullptr };
	/// THE AUDIO THREAD'S OWN: arrivals taken from the inbox and kept for a later drain — built for a
	/// publish this drain has not taken yet. Touched by the message thread only with no block running.
	Posted* held_ = nullptr;
	/// See `settledAt` — whether the last `converge` left nothing to do, and of which publish. Cleared by
	/// every message-thread verb that changes what a drain would find (`dropPending`, `adoptRunning`).
	bool settled_ = false;
	uint32_t settledFor_ = 0;
	uint64_t nextSerial_ = 1;
	/// THE MESSAGE THREAD'S MIRROR of what it last built, one entry per device it believes built.
	std::vector<Identity> provisioned_;
	/// THE HOST'S CARRIER, as last handed to `provision` — where a running container's handle is filed and
	/// cleared (rack roadmap §6). Message thread only, like the carrier itself.
	ContainerBodies* bodies_ = nullptr;
	/// **POSTS HANDED BACK WHOSE INSTANCE AN EDITOR STILL HOLDS** (backlog JX) — forgotten on time, freed
	/// only once unpinned (`ContainerBodies::pin`). The engine's parking waits the same way (`setReapHold`).
	/// Message thread only.
	Posted* pinnedSpent_ = nullptr;
	/// **POSTS HANDED BACK WHOSE BLOCK HAS NOT PASSED YET** (`Posted::retiredAt`) — kept for a later `reap`. Message thread only.
	Posted* epochSpent_ = nullptr;
	/// Whether a block has begun AND ended since `retiredAt` (odd: the block it was handed back in, which must end; even: the
	/// next one, which must begin and end), so nothing the drain was reading can still be read through it.
	[[nodiscard]] bool epochPassed(const Posted& p) const noexcept {
		return engine_.blockEpochNow() >= (p.retiredAt | 1u) + 1u;
	}
	/// Whether the host's carrier pins this storage — the engine's reap hold, and `reap`'s own test.
	[[nodiscard]] bool pinned(const void* storage) const noexcept {
		return bodies_ != nullptr && storage != nullptr && bodies_->pinned(storage);
	}
	/// Free `p` now, or keep it on `pinnedSpent_` while its instance is pinned.
	void release(Posted* p) noexcept;
	/// See `setOuterGateMaker`.
	device::OuterGate (*makeGate_)(const void* host, DeviceInstanceId self) = nullptr;
	const void* gateHost_ = nullptr;
	/// See `setSlotLinkMaker`.
	device::HostSlotLink (*makeLink_)(const void* host, DeviceInstanceId self) = nullptr;
	const void* linkHost_ = nullptr;
	/// Hand `storage` (a container of kind `desc`, instance `self`) its slot link, when this host makes one.
	void handSlotLink(const device::Descriptor* desc, void* storage, DeviceInstanceId self) const {
		if (desc->container != nullptr && desc->container->setHostSlots != nullptr && makeLink_ != nullptr
		    && storage != nullptr) {
			desc->container->setHostSlots(storage, makeLink_(linkHost_, self));
		}
	}
	/// See `takeOuterDrops`.
	RouteRefusals outerDrops_ {};
	/// THE AUDIO THREAD'S RECORD of what each engine slot runs — the engine's pool, kept beside it.
	backplane::ChunkedPool<Identity> live_;
	std::atomic<Posted*> spent_ { nullptr };
	/// **DEVICES THE DRAIN REMOVED, FOR THE MESSAGE THREAD TO FORGET.** Without it a device the drain took
	/// out while converging to one state, and wanted back by a later one, was never rebuilt whenever the
	/// message thread had not provisioned in between: its mirror still said "built". A single-producer
	/// ring, fixed at construction; an overflow says so, and the mirror is then forgotten whole — which
	/// costs rebuilds the drain throws away, and loses nothing.
	/// WHAT THIS DRAIN TOOK OUT, gathered as it goes and reported only at the END of `converge` — a device
	/// REPLACED under its own id is removed and re-adopted in one drain, and reporting the removal would
	/// make the message thread forget a device that is running. Sized at construction; `clear()` frees
	/// nothing, so the drain allocates nothing.
	backplane::ChunkedPool<DeviceInstanceId> departures_;
	size_t departureCount_ = 0;
	/// The engine tells this converger when it grows, so `live_` and `departures_` always reach every lane
	/// (unbounded devices step 4); the token unhooks it when this converger goes first.
	size_t growthToken_ = 0;
	std::vector<std::atomic<DeviceInstanceId>> departed_;
	std::atomic<size_t> departedHead_ { 0 };
	std::atomic<size_t> departedTail_ { 0 };
	std::atomic<bool> departedOverflow_ { false };

	/// **THE SITES THIS HOST OWNS** (backlog NR) — one per device whose carrier files a site, built here, offered to the
	/// engine, and freed here only once no slot holds it, no offer of it is in flight and the block epoch has moved
	/// past the moment both were last true. Message thread only.
	struct HeldSite {
		DeviceInstanceId id = kNoInstance;
		std::unique_ptr<SiteChains> site;
		bool wanted = false;
		size_t inFlight = 0;
		/// The block epoch when the site was last seen held by nothing; 0 while it is still held or wanted.
		uint64_t idleSince = 0;
	};
	std::vector<HeldSite> sites_;
	/// Build, offer, detach and free sites against `want` and the carrier (`provision`'s and `reap`'s last step).
	void serviceSites(const ChainState* want);
	void postSite(HeldSite& held, SiteChains* site);
};

} // namespace feathers::rack
