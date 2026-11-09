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

#include "rack/converger.h"

#include "rack/rack_state.h"
#include "rack/site_chains.h"

#include <algorithm>
#include <span>

namespace feathers::rack {

namespace {

/// WHETHER PUBLISH `a` CAME AFTER PUBLISH `b` — wrap-safe, and never for a host with no sequence (0).
bool isLater(uint32_t a, uint32_t b) noexcept {
	return a != 0 && static_cast<int32_t>(a - b) > 0;
}

/// Delete a whole list of posts — message thread only.
template <typename Posted>
void deleteAll(Posted* list) noexcept {
	while (list != nullptr) {
		Posted* next = list->next;
		delete list;
		list = next;
	}
}

} // namespace

Converger::Converger(RackEngine& engine, Fits fits)
    : engine_(engine), fits_(fits != nullptr ? fits : &Converger::fitsAnywhere), live_(engine.slotCount()),
      departures_(engine.slotCount()), departed_(engine.slotCount() * 4) {
	// **AND AS THE ENGINE GROWS** (unbounded devices step 4) — on the main thread, before a lane past the old
	// room can hold a device, so neither the drain nor the provision ever meets a lane these do not reach.
	growthToken_ = engine_.addGrowthListener([this](size_t capacity) {
		live_.grow(capacity);
		departures_.grow(capacity);
	});
	// **WHAT THE HOST'S EDITORS HOLD IS NOT FREED UNDER THEM** (backlog JX) — see `ContainerBodies::pin`.
	engine_.setReapHold(
	    [](const void* self, const void* storage) noexcept {
		    return static_cast<const Converger*>(self)->pinned(storage);
	    },
	    this);
}

void Converger::unhookOuterGates() noexcept {
	const device::OuterGate none {};
	for (size_t lane = 0; lane < engine_.slotCount(); ++lane) {
		const Slot& slot = engine_.slotAtLane(lane);
		const device::Descriptor* desc = slot.desc.load(std::memory_order_acquire);
		if (desc != nullptr && desc->container != nullptr && desc->container->setOuterGate != nullptr
		    && slot.storage != nullptr) {
			desc->container->setOuterGate(slot.storage.get(), none);
		}
	}
	for (HeldSite& held : sites_) {
		if (held.site != nullptr) {
			siteContainerOps().setOuterGate(held.site.get(), none);
		}
	}
}

void Converger::unhookSlotLinks() noexcept {
	const device::HostSlotLink none {};
	for (size_t lane = 0; lane < engine_.slotCount(); ++lane) {
		const Slot& slot = engine_.slotAtLane(lane);
		const device::Descriptor* desc = slot.desc.load(std::memory_order_acquire);
		if (desc != nullptr && desc->container != nullptr && desc->container->setHostSlots != nullptr
		    && slot.storage != nullptr) {
			desc->container->setHostSlots(slot.storage.get(), none);
		}
	}
	for (HeldSite& held : sites_) {
		if (held.site != nullptr && siteContainerOps().setHostSlots != nullptr) {
			siteContainerOps().setHostSlots(held.site.get(), none);
		}
	}
}

void Converger::relinkSites() noexcept {
	const device::ContainerOps& ops = siteContainerOps();
	if (ops.setHostSlots == nullptr) {
		return;
	}
	for (HeldSite& held : sites_) {
		if (held.site != nullptr) {
			ops.setHostSlots(held.site.get(), makeLink_ != nullptr ? makeLink_(linkHost_, held.id) : device::HostSlotLink {});
		}
	}
}

Converger::~Converger() {
	shutdown();
}

void Converger::shutdown() noexcept {
	if (shutDown_) {
		return;
	}
	shutDown_ = true;
	// THE MESSAGE THREAD, WITH NO BLOCK RUNNING — so everything in flight dies here: arrivals never
	// taken, arrivals held for a later drain, and whatever the audio thread handed back. NOTHING IS HELD
	// NOW: a pin cannot outlive the host whose carrier it is in, and the engine must not ask a dead one.
	engine_.removeGrowthListener(growthToken_);
	engine_.setReapHold(nullptr, nullptr);
	bodies_ = nullptr;
	deleteAll(inbox_.exchange(nullptr, std::memory_order_acquire));
	deleteAll(held_);
	held_ = nullptr;
	reap();
	deleteAll(epochSpent_); // no block runs: whatever waited on one is free to go
	epochSpent_ = nullptr;
	deleteAll(pinnedSpent_);
	// THE SITES AND THEIR ENVELOPES (backlog NR): no block runs, so nothing the engine still points at is read again.
	for (RackEngine::SiteOffer* spent = engine_.takeSpentSiteOffers(); spent != nullptr;) {
		RackEngine::SiteOffer* next = spent->next;
		delete spent;
		spent = next;
	}
	engine_.dropSites(); // and the engine forgets every pointer to them before they go
	sites_.clear();
	pinnedSpent_ = nullptr;
}

void Converger::release(Posted* p) noexcept {
	if (pinned(p->provision.storage.get())) {
		p->next = pinnedSpent_;
		pinnedSpent_ = p;
		return;
	}
	delete p;
}

void Converger::dropPending() noexcept {
	settled_ = false; // see `settledAt`
	// NO BLOCK RUNNING (the host re-prepares) — so the held list, the audio thread's own, may be taken here.
	Posted* lists[2] = { inbox_.exchange(nullptr, std::memory_order_acquire), held_ };
	held_ = nullptr;
	for (Posted* list : lists) {
		while (list != nullptr) {
			Posted* next = list->next;
			const DeviceInstanceId id = list->instanceId;
			// **A VOICE POST DROPPED LEAVES THE DEVICE AS IT RUNS** (backlog NS): the voices it carried were never
			// adopted, and the ones it asked back are still in the slot — so the belief goes back to that, and the
			// next `provision` asks again, at the new rate. The device itself is not forgotten.
			if (list->voicesOnly) {
				for (Identity& p : provisioned_) {
					if (p.instanceId == id && p.voiceSerial <= list->serial) {
						p.voices = list->provision.voices.empty();
						p.voiceSerial = list->serial;
					}
				}
				release(list);
				list = next;
				continue;
			}
			std::erase_if(provisioned_, [id](const Identity& p) { return p.instanceId == id; });
			// **AN INSTANCE THROWN AWAY HERE IS BUILT AGAIN FROM THE STORED BODY — SO THE STORE IS BROUGHT UP TO IT
			// FIRST.** Its handle was filed at build, so its editor could already reach it: an IR picked, a chain
			// edited while the engine was stopped (a load while inactive builds and posts, and nothing adopts until a
			// block runs) lived only in this instance. Without this the restart's rebuild put back the body as last
			// loaded. The generation stays: it is the same body, edited. And the handle goes,
			// because the instance it names is about to be freed.
			const void* storage = list->provision.storage.get();
			const device::Descriptor* desc = list->provision.desc;
			if (bodies_ != nullptr && storage != nullptr && desc != nullptr && bodies_->surfaceOf(id) == storage) {
				if (desc->bodySaver() != nullptr) {
					try {
						std::vector<uint8_t> body;
						if (bodyOf(desc, storage, &body) && !body.empty()) {
							(void)bodies_->refresh(id, std::move(body));
						}
					} catch (...) {
						// an allocation that failed keeps the stored body: the rebuild is as last loaded
					}
				}
				bodies_->detach(id, storage);
			}
			// ITS BLOCKS TOO (backlog NM): what it had published, kept so the rebuild restores it. An instance that never
			// ran published nothing, and the stored blocks stand.
			if (bodies_ != nullptr && storage != nullptr && desc != nullptr && desc->state != nullptr) {
				try {
					(void)refreshBlocks(*desc, storage, bodies_, id, /*onTickOnly=*/false);
				} catch (...) {
					// an allocation that failed keeps the stored blocks
				}
			}
			release(list);
			list = next;
		}
	}
}

bool Converger::wantsVoices(const device::Descriptor& desc, const DeviceState& row, const ChainState& state,
                            size_t voices) noexcept {
	if (voices == 0 || desc.audio == nullptr || desc.traits.perVoiceControl < 0) {
		return false;
	}
	const auto poly = static_cast<size_t>(desc.traits.perVoiceControl);
	if (poly >= desc.params.size()) {
		return false;
	}
	// A SWITCH THE AUDIO THREAD CAN FLIP — see the declaration. None of today's Poly switches is modulatable; a
	// device that makes one so is built eagerly rather than found without instances by a route.
	if (desc.params[poly].modulatable) {
		return true;
	}
	for (const SlotBinding& b : state.slotBindings()) {
		if (b.live && b.device == row.instanceId && b.control == poly) {
			return true;
		}
	}
	// THE RECORD'S VALUE, read as the engine reads it (`polyInRow`): absent is the declared default.
	const float value = poly < row.fieldsPresent && poly < row.field.size() ? row.field[poly]
	                                                                          : static_cast<float>(desc.params[poly].def);
	return value >= 0.5f;
}

const device::Descriptor* Converger::wantedAs(const DeviceState& row, size_t chain) const {
	const device::Descriptor* desc = engine_.catalogue()(row.tag.data());
	if (desc == nullptr) {
		return nullptr; // a tag this build cannot run is kept in its record and not run
	}
	return fits_(*desc, chain) ? desc : nullptr;
}

const DeviceState* Converger::rowOf(const ChainState& want, DeviceInstanceId id, size_t* chain) noexcept {
	size_t c = 0;
	size_t position = 0;
	if (!want.find(id, &c, &position)) {
		return nullptr;
	}
	*chain = c;
	return &want.at(c, position);
}

void Converger::adoptRunning(const ChainState& state) noexcept {
	settled_ = false; // see `settledAt`
	for (size_t chain = 0; chain < state.chainCount(); ++chain) {
		for (const DeviceState& row : state.chain(chain)) {
			const device::Descriptor* desc = wantedAs(row, chain);
			const size_t slot = engine_.slotOf(row.instanceId);
			if (desc == nullptr || slot >= live_.size() || engine_.slotAtLane(slot).live_desc() != desc) {
				continue;
			}
			// AND ITS VOICES AS THEY ARE (backlog NS) — an engine built by hand builds them eagerly; the next `provision`
			// asks for them back if the record's Poly is off.
			const Identity running { desc, row.tag, row.instanceId, 0, !engine_.slotAtLane(slot).voices.empty(), 0 };
			live_[slot] = running;
			std::erase_if(provisioned_, [&](const Identity& p) { return p.instanceId == row.instanceId; });
			provisioned_.push_back(running);
		}
	}
}

size_t Converger::pendingCountForTest() const noexcept {
	size_t n = 0;
	for (const Posted* p = inbox_.load(std::memory_order_acquire); p != nullptr; p = p->next) {
		++n;
	}
	for (const Posted* p = held_; p != nullptr; p = p->next) {
		++n;
	}
	return n;
}

void Converger::serviceContainers(const ChainState& want, ContainerBodies* bodies) {
	if (bodies == nullptr) {
		return;
	}
	for (size_t chain = 0; chain < want.chainCount(); ++chain) {
		for (const DeviceState& row : want.chain(chain)) {
			const device::Descriptor* desc = wantedAs(row, chain);
			if (desc == nullptr || desc->container == nullptr || desc->container->service == nullptr) {
				continue;
			}
			void* surface = bodies->surfaceOf(row.instanceId);
			if (surface == nullptr) {
				continue;
			}
			const device::RouteDrop dropped = desc->container->service(surface);
			if (dropped.count > 0) {
				if (outerDrops_.count == 0) {
					outerDrops_.firstSource = dropped.firstSource.inner();
					outerDrops_.firstDestination = backplane::Destination::control(row.instanceId, 0);
				}
				outerDrops_.count += dropped.count;
			}
		}
	}
}

size_t Converger::provision(const ChainState& want, ContainerBodies* bodies, uint32_t forPublish) {
	bodies_ = bodies;
	reap();
	// **AND ROOM FOR EVERY DEVICE THE STATE NAMES** (unbounded devices step 4) — asked here, where allocation is
	// allowed, so the drain that converges to this state finds a graph big enough to place them in.
	{
		size_t wanted = 0;
		for (size_t chain = 0; chain < want.chainCount(); ++chain) {
			wanted += want.length(chain);
		}
		(void)engine_.requestCapacity(wanted);
	}
	// A DEVICE THE STATE NO LONGER WANTS IS FORGOTTEN — so one that leaves and comes back (an undo puts it
	// back under the same id) is built again rather than looking unchanged.
	std::erase_if(provisioned_, [&](const Identity& p) {
		size_t chain = 0;
		const DeviceState* row = rowOf(want, p.instanceId, &chain);
		return row == nullptr || wantedAs(*row, chain) == nullptr;
	});
	size_t built = 0;
	for (size_t chain = 0; chain < want.chainCount(); ++chain) {
		for (const DeviceState& row : want.chain(chain)) {
			const device::Descriptor* desc = wantedAs(row, chain);
			if (desc == nullptr || row.instanceId == kNoInstance) {
				continue;
			}
			// THE BODY'S VERSION, for a container — zero for everything else, so every other device compares
			// on its kind, tag and instance id alone.
			const auto held = std::find_if(provisioned_.begin(), provisioned_.end(),
			                               [&](const Identity& p) { return p.instanceId == row.instanceId; });
			// **NO CARRIER IS NOT A NEW BODY.** A caller that hands no bodies (`Synth::activate`'s provision from its
			// own chains, which runs on EVERY activate) knows nothing of the device's body, so it must not read as
			// "the body moved to generation 0": that rebuilt every body device — an EMPTY one, then the next
			// provision rebuilt it again from the STORED body, the one the last load put there. Whatever was edited
			// in the running instance since (a Featherverb's IR, a chain's contents, which live only there) was
			// lost on an audio engine restart. The held generation stands when there is no
			// carrier to say otherwise.
			const uint64_t bodyGeneration = !desc->hasStoredState()           ? 0
			                                : bodies != nullptr               ? bodies->generationOf(row.instanceId)
			                                : held != provisioned_.end()      ? held->bodyGeneration
			                                                                  : 0;
			Identity wanted { desc, row.tag, row.instanceId, bodyGeneration };
			const bool voices = wants(*desc, row, want);
			if (held != provisioned_.end() && *held == wanted) {
				// **THE SAME DEVICE WITH ITS POLY MOVED: ITS VOICE INSTANCES, AND NOTHING ELSE** (backlog NS) — built
				// here when the switch went on, or asked back when it went off. The drain swaps them in or out;
				// `converge` keeps the switch reading off until they have arrived, so no block runs Poly without them.
				if (voices != held->voices) {
					auto* posted = new Posted { voices ? engine_.provisionVoices(*desc) : RackEngine::Provision {},
						                        row.instanceId, forPublish, nextSerial_++, /*forget=*/false, nullptr };
					posted->voicesOnly = true;
					posted->provision.desc = desc;
					if (voices && posted->provision.voices.empty()) {
						delete posted; // nothing to build (no voices here): believed as it is
					}
					else {
						posted->next = inbox_.load(std::memory_order_relaxed);
						while (!inbox_.compare_exchange_weak(posted->next, posted, std::memory_order_release,
						                                     std::memory_order_relaxed)) {
						}
						held->voices = voices;
						held->voiceSerial = posted->serial;
					}
				}
				// THE CONTAINER'S OWN SERVICE, ON THIS BEAT (rack roadmap §7, hygiene): a live chain prunes inner
				// routes whose outer source this host no longer has upstream; what it dropped is kept here for
				// this host to say, naming the chain as the destination and the source as this host spells it.
				if (desc->container != nullptr && desc->container->service != nullptr && bodies != nullptr) {
					if (void* surface = bodies->surfaceOf(row.instanceId); surface != nullptr) {
						const device::RouteDrop dropped = desc->container->service(surface);
						if (dropped.count > 0) {
							if (outerDrops_.count == 0) {
								outerDrops_.firstSource = dropped.firstSource.inner();
								outerDrops_.firstDestination = backplane::Destination::control(row.instanceId, 0);
							}
							outerDrops_.count += dropped.count;
						}
					}
				}
				// A CONTAINER ALREADY RUNNING WITH NO HANDLE ON FILE — the carrier was replaced whole (a load) while
				// the instance stayed — is filed again from the engine's own slot, so its editor can find it.
				if (desc->hasStoredState() && bodies != nullptr && bodies->surfaceOf(row.instanceId) == nullptr) {
					const size_t slot = engine_.slotOf(row.instanceId);
					if (slot != RackEngine::kNoSlot) {
						bodies->attach(row.instanceId, engine_.slotAtLane(slot).storage.get());
						if (desc->container != nullptr && desc->container->setOuterGate != nullptr && makeGate_ != nullptr) {
							desc->container->setOuterGate(engine_.slotAtLane(slot).storage.get(), makeGate_(gateHost_, row.instanceId));
						}
						handSlotLink(desc, engine_.slotAtLane(slot).storage.get(), row.instanceId);
					}
				}
				continue;
			}
			// A DEVICE, NOT A PLACE — the engine chooses the slot when it adopts it.
			auto* posted = new Posted { engine_.provision(row.tag.data(), voices), row.instanceId, forPublish,
				                        nextSerial_++, /*forget=*/false, nullptr };
			if (!posted->provision.live()) {
				delete posted;
				continue; // tried again next visit
			}
			// THE HOST OUTSIDE, BEFORE THE BODY (rack roadmap §7): a chain's inner gate must know its host
			// when the body's routes are loaded, or an outer-sourced route saved yesterday is refused today.
			if (desc->container != nullptr && desc->container->setOuterGate != nullptr && makeGate_ != nullptr) {
				desc->container->setOuterGate(posted->provision.storage.get(), makeGate_(gateHost_, row.instanceId));
			}
			// AND THE POOL OUTSIDE (host slots T11), for the same reason: the devices the body builds are linked.
			handSlotLink(desc, posted->provision.storage.get(), row.instanceId);
			// **A CONTAINER'S CONTENTS, STILL ON THIS SIDE OF THE HANDOVER.** Loading builds members and
			// allocates. A refused body leaves the container as it was built — empty — because `loadState`
			// is all-or-nothing by contract.
			if (desc->bodyLoader() != nullptr && bodies != nullptr
			    && bodies->has(row.instanceId)) {
				const std::span<const uint8_t> body = bodies->bodyFor(row.instanceId);
				(void)desc->bodyLoader()(posted->provision.storage.get(), body.data(), body.size());
			}
			// **AND THE BLOCKS IT ARRIVED AT ITSELF** (backlog NM), on the same side of the handover and for the same
			// reason: the instance the drain swaps in already holds what was saved, so nothing is copied on the audio
			// thread and nothing has to tell a load from a hand's edit there.
			if (desc->state != nullptr && bodies != nullptr) {
				restoreBlocks(*desc, posted->provision.storage.get(), *bodies, row.instanceId);
			}
			// **THE RUNNING INSTANCE'S HANDLE, FILED BESIDE ITS BODY** (rack roadmap §6): the host's UI reaches the
			// container's own editor through it, and the host's save asks it for the current contents. Filed
			// here, before the drain adopts it, so an editor opened early queues edits the drain will take.
			if (desc->hasStoredState() && bodies != nullptr) {
				bodies->attach(row.instanceId, posted->provision.storage.get());
			}
			// ITS VOICES AS BUILT, read before the post is the drain's.
			wanted.voices = !posted->provision.voices.empty();
			wanted.voiceSerial = posted->serial;
			// ONTO THE ONE MAILBOX — a lock-free push; the drain takes the whole list.
			posted->next = inbox_.load(std::memory_order_relaxed);
			while (!inbox_.compare_exchange_weak(posted->next, posted, std::memory_order_release,
			                                     std::memory_order_relaxed)) {
			}
			if (held != provisioned_.end()) {
				*held = wanted;
			}
			else {
				provisioned_.push_back(wanted);
			}
			++built;
		}
	}
	// AND THE SITES THIS STATE WANTS (backlog NR): built, offered, or let go.
	serviceSites(&want);
	return built;
}

void Converger::reap() noexcept {
	engine_.reapRetired();
	// WHAT AN EDITOR HELD LAST TIME AND HAS LET GO — freed first, the rest kept for the next reap.
	Posted* kept = pinnedSpent_;
	pinnedSpent_ = nullptr;
	while (kept != nullptr) {
		Posted* next = kept->next;
		release(kept);
		kept = next;
	}
	const auto forget = [this](DeviceInstanceId id) {
		std::erase_if(provisioned_, [id](const Identity& p) { return p.instanceId == id; });
		// AND ITS HANDLE, unless the id is running again under a newer instance (replaced under its own id).
		if (bodies_ != nullptr && engine_.slotOf(id) == RackEngine::kNoSlot) {
			bodies_->detach(id);
		}
	};
	// WHAT THE DRAIN REMOVED — see `departed_`. An overflow forgets everything: the rebuilds it causes are
	// thrown away by a drain that finds the same device running, and nothing is lost.
	if (departedOverflow_.exchange(false, std::memory_order_acq_rel)) {
		provisioned_.clear();
	}
	const size_t head = departedHead_.load(std::memory_order_acquire);
	size_t tail = departedTail_.load(std::memory_order_relaxed);
	while (tail != head) {
		forget(departed_[tail % departed_.size()].load(std::memory_order_relaxed));
		++tail;
	}
	departedTail_.store(tail, std::memory_order_release);

	// THE POSTS WHOSE BLOCK HAD NOT PASSED AT THE LAST REAP: freed now if it has, kept otherwise. Their forget and voice
	// decisions were already taken then; only the free waited.
	{
		Posted* waiting = epochSpent_;
		epochSpent_ = nullptr;
		while (waiting != nullptr) {
			Posted* next = waiting->next;
			if (epochPassed(*waiting)) {
				release(waiting);
			}
			else {
				waiting->next = epochSpent_;
				epochSpent_ = waiting;
			}
			waiting = next;
		}
	}
	Posted* list = spent_.exchange(nullptr, std::memory_order_acquire);
	while (list != nullptr) {
		Posted* next = list->next;
		// **A DEVICE HANDED BACK THAT NEVER LANDED AND THAT THIS SIDE STILL BELIEVES IT BUILT IS FORGOTTEN,
		// so the next `provision` builds it again.** That was the lost add, in the rack first and then in
		// Feathers' lane: the drain was converging to an older state, or the engine declined the arrival, and
		// the mirror still said "built", so nothing would ever build it again.
		//
		// **BUT "HANDED BACK" IS NOT "NEVER LANDED", AND READING IT THAT WAY COST A LOOP.** A post for a
		// device that IS running comes back too — superseded by a newer one, or simply not needed because the
		// device was adopted under that id in the same drain (a REPLACEMENT under one id: the record does
		// exactly that). Forgetting those made the message thread rebuild a running device, the next drain
		// throw the rebuild away, and the two of them repeat it for the life of the session — with a
		// container, removing and re-adopting the live nested rack every drain. So the DRAIN decides, where
		// it can see whether the engine runs the device (`Posted::forget`), and this only obeys.
		// **A VOICE POST COMES BACK WITH THE DRAIN'S WORD ON THE SLOT** (backlog NS): whether it holds voices now. The
		// belief follows it unless a newer post has spoken since — and then that post's own return will.
		if (list->voicesOnly && list->stated) {
			for (Identity& p : provisioned_) {
				if (p.instanceId == list->instanceId && p.desc == list->provision.desc && p.voiceSerial <= list->serial) {
					p.voices = list->voicesNow;
					p.voiceSerial = list->serial;
				}
			}
		}
		if (list->forget) {
			const DeviceInstanceId id = list->instanceId;
			const device::Descriptor* desc = list->provision.desc;
			// THE HANDLE OF AN INSTANCE HANDED BACK UNADOPTED — by pointer, so a newer one filed meanwhile stays.
			if (bodies_ != nullptr && list->provision.storage != nullptr) {
				bodies_->detach(id, list->provision.storage.get());
			}
			std::erase_if(provisioned_,
			              [id, desc](const Identity& p) { return p.instanceId == id && p.desc == desc; });
		}
		// FREED ONCE ITS BLOCK HAS PASSED (`Posted::retiredAt`), and then unless an editor still holds its instance (backlog JX).
		if (epochPassed(*list)) {
			release(list);
		}
		else {
			list->next = epochSpent_;
			epochSpent_ = list;
		}
		list = next;
	}
	// AND THE SITES ON THIS BEAT (backlog NR): an offer for a device that has since landed, a free that is now safe.
	serviceSites(nullptr);
}

void Converger::retire(Posted* posted, bool forget) noexcept {
	posted->forget = forget;
	posted->retiredAt = engine_.blockEpochNow();
	posted->next = spent_.load(std::memory_order_relaxed);
	while (!spent_.compare_exchange_weak(posted->next, posted, std::memory_order_release,
	                                     std::memory_order_relaxed)) {
	}
}

void Converger::depart(DeviceInstanceId id) noexcept {
	const size_t head = departedHead_.load(std::memory_order_relaxed);
	if (head - departedTail_.load(std::memory_order_acquire) >= departed_.size()) {
		departedOverflow_.store(true, std::memory_order_release);
		return;
	}
	departed_[head % departed_.size()].store(id, std::memory_order_relaxed);
	departedHead_.store(head + 1, std::memory_order_release);
}

void Converger::converge(const ChainState& want, uint32_t published) {
	// **ROOM FIRST** (unbounded devices step 4): a bigger graph the main thread built for this state is
	// adopted before anything is placed, so the devices that needed it land on this same drain.
	engine_.adoptGrowth();
	// WHETHER THIS PASS LEAVES ANYTHING FOR THE NEXT ONE — see `settledAt`.
	bool settled = true;
	// **EVERYTHING THAT HAS ARRIVED** — the mailbox taken whole, joined to what earlier drains held. An
	// arrival FROM THIS DRAIN'S FUTURE — built for a publish this drain has not taken yet — is held again,
	// untouched, for the drain that has (see `provision` for the add this once lost). The rest are this
	// drain's to adopt or hand back.
	Posted* arrivals = inbox_.exchange(nullptr, std::memory_order_acquire);
	while (arrivals != nullptr) {
		Posted* next = arrivals->next;
		arrivals->next = held_;
		held_ = arrivals;
		arrivals = next;
	}
	Posted* now = nullptr;
	Posted* later = nullptr;
	while (held_ != nullptr) {
		Posted* p = held_;
		held_ = p->next;
		Posted*& into = isLater(p->forPublish, published) ? later : now;
		p->next = into;
		into = p;
	}
	held_ = later;
	// **MUST THE MESSAGE THREAD FORGET HAVING BUILT A POST HANDED BACK?** Only if the engine is not running
	// that device AS THIS POST BUILT IT. "Is the device running" alone is not sharp enough: a replacement
	// under one id whose removal the full parking refused leaves the OLD device running under that id, and
	// reading that as "landed" would suppress the rebuild the state still needs. With the descriptor in the
	// question it asks the thing that matters — is what this post carried now running?
	const auto mustForget = [this](const Posted* p) {
		const size_t slot = engine_.slotOf(p->instanceId);
		return slot == RackEngine::kNoSlot || engine_.slotAtLane(slot).live_desc() != p->provision.desc;
	};
	// A NEWER POST FOR ONE DEVICE SUPERSEDES AN OLDER — handed back whole, which `reap` reads as "not
	// landed" only if its mirror still names it, and the newer post keeps that from being wrong.
	for (Posted** link = &now; *link != nullptr;) {
		Posted* p = *link;
		bool superseded = false;
		for (const Posted* q = now; q != nullptr && !superseded; q = q->next) {
			// ONE KIND BY ONE KIND: a device's voices never stand in for the device, nor it for them (backlog NS).
			superseded = q != p && q->instanceId == p->instanceId && q->voicesOnly == p->voicesOnly && q->serial > p->serial;
		}
		if (superseded) {
			*link = p->next;
			// A SUPERSEDED VOICE POST SAYS NOTHING (`stated` stays false): the newer one speaks for the slot.
			retire(p, !p->voicesOnly && mustForget(p));
		}
		else {
			link = &p->next;
		}
	}
	// THE ARRIVAL FOR DEVICE `id` BUILT AS `desc`, taken out of `now` — or null.
	const auto take = [&now](DeviceInstanceId id, const device::Descriptor* desc) -> Posted* {
		for (Posted** link = &now; *link != nullptr; link = &(*link)->next) {
			Posted* p = *link;
			if (p->instanceId == id && p->provision.desc == desc && p->provision.live()) {
				*link = p->next;
				p->next = nullptr;
				return p;
			}
		}
		return nullptr;
	};
	const auto hasArrival = [&now](DeviceInstanceId id, const device::Descriptor* desc) {
		for (const Posted* p = now; p != nullptr; p = p->next) {
			if (p->instanceId == id && p->provision.desc == desc && p->provision.live()) {
				return true;
			}
		}
		return false;
	};

	// **DEPARTURES — EVERY SLOT RUNNING A DEVICE THE STATE DOES NOT WANT AS IT IS.** The same device, not
	// merely the same kind (the two-of-a-kind lesson — `FxRunner`, then `ModRunner`, whose envelopes swapped
	// stages when they swapped places): an instance id does not move when a knob does, so this keeps a
	// device's memory across every edit but a replacement.
	for (size_t slot = 0; slot < live_.size(); ++slot) {
		Identity& live = live_[slot];
		if (!engine_.slotAtLane(slot).live()) {
			live = Identity {};
			continue;
		}
		size_t chain = 0;
		const DeviceState* row = rowOf(want, live.instanceId, &chain);
		const device::Descriptor* desc = row != nullptr ? wantedAs(*row, chain) : nullptr;
		const bool same = desc != nullptr && live.desc == desc && live.tag == row->tag;
		// A DEVICE THAT ARRIVED UNDER ITS OWN ID IS NEW CONTENTS — a body or blocks loaded over it — and is taken though
		// the device is "the same". Only a device the host keeps state for beside its row is ever re-posted so.
		const bool newContents = same && desc->hasStoredState() && hasArrival(live.instanceId, desc);
		if (same && !newContents) {
			continue;
		}
		const DeviceInstanceId leaving = live.instanceId;
		if (!engine_.removeDeviceAtLane(slot)) {
			settled = false;
			// THE PARKING IS FULL. The old device stays, and the next drain tries again; an arrival meant
			// to replace it is handed back below and built again.
			continue;
		}
		live = Identity {};
		// GATHERED, NOT REPORTED YET — the arrivals pass below may put this very device back (a replacement
		// keeps its id), and telling the message thread it left would have it rebuild what is running.
		if (DeviceInstanceId* into = departures_.at(departureCount_)) {
			*into = leaving;
			++departureCount_;
		}
	}

	// **ARRIVALS — EVERY DEVICE THE STATE WANTS THAT THE ENGINE DOES NOT RUN**, adopted under its own id at
	// the end; the order pass below puts it where it belongs.
	for (size_t chain = 0; chain < want.chainCount(); ++chain) {
		for (const DeviceState& row : want.chain(chain)) {
			const device::Descriptor* desc = wantedAs(row, chain);
			if (desc == nullptr || engine_.slotOf(row.instanceId) != RackEngine::kNoSlot) {
				continue;
			}
			Posted* arrived = take(row.instanceId, desc);
			if (arrived == nullptr) {
				continue;
			}
			const size_t slot = engine_.adoptDevice(engine_.chainLength(), std::move(arrived->provision), row.instanceId);
			if (slot < live_.size()) {
				live_[slot] = Identity { desc, row.tag, row.instanceId, 0 };
			}
			// ADOPTED (a husk holding the slot's old buffers) OR REFUSED (whole): the message thread frees
			// it, and one whose device is NOT running tells it to build again.
			retire(arrived, mustForget(arrived));
		}
	}
	// **VOICES — A RUNNING DEVICE'S INSTANCES PER VOICE, IN OR OUT AS ITS ROW SAYS** (backlog NS). After the arrivals,
	// so a device adopted in this same drain can take its voices too. Swaps only: what goes out rides home in the
	// post and is destructed and freed by `reap`, off this thread. Every post comes back saying what the slot holds.
	for (Posted** link = &now; *link != nullptr;) {
		Posted* p = *link;
		if (!p->voicesOnly) {
			link = &p->next;
			continue;
		}
		*link = p->next;
		p->next = nullptr;
		const size_t slot = engine_.slotOf(p->instanceId);
		const bool running = slot != RackEngine::kNoSlot && engine_.slotAtLane(slot).live_desc() == p->provision.desc;
		if (running) {
			size_t chain = 0;
			const DeviceState* row = rowOf(want, p->instanceId, &chain);
			const bool wanted = row != nullptr && wants(*p->provision.desc, *row, want);
			if (wanted) {
				(void)engine_.adoptVoices(slot, p->provision); // refused when the slot has its own already
			}
			else if (p->provision.voices.empty()) {
				(void)engine_.releaseVoices(slot, p->provision);
			}
		}
		p->stated = true;
		p->voicesNow = running && !engine_.slotAtLane(slot).voices.empty();
		retire(p, false);
	}
	// WHAT NOBODY WANTED IS HANDED BACK — to be forgotten only if its device is not running. One for a
	// device that IS running is simply surplus (its device was adopted here, or never left).
	while (now != nullptr) {
		Posted* next = now->next;
		retire(now, mustForget(now));
		now = next;
	}
	// **AND NOW THE DEPARTURES — only the devices this drain took out AND did not put back.** See
	// `departures_`: a replacement leaves and returns under one id in this same call.
	for (size_t i = 0; i < departureCount_; ++i) {
		const DeviceInstanceId id = departures_[i];
		if (engine_.slotOf(id) == RackEngine::kNoSlot) {
			depart(id);
		}
	}
	departureCount_ = 0;

	// **THE ORDER IS THE GRAPH'S (kill-lanes 4c).** The record publishes `Graph::order()` in the state —
	// one topological order over every node, routes included as precedences — and the engine runs it, so a
	// source runs before the control that reads it whatever chains the two are in, and a route's read is
	// pass-through by construction. This ran THE CHAINS IN INDEX ORDER (Feathers: the strip, then the
	// lane), which agreed with the graph only because nothing orders two chains against each other and no
	// hand had yet routed a lane device's output into a strip modulator — the day one did, the reader ran
	// first and read the tick before, silently.
	//
	// **A STATE WITH NO ORDER IS A STATE NO GRAPH SPOKE FOR** (built by hand, or by the by-value fixture
	// road), and the chains in index order, each by its path, is all the order it has. A reorder MOVES
	// devices — nothing is rebuilt, so a reordered device keeps its state. A device the engine does not
	// run — the host's own node among them — is skipped.
	size_t target = 0;
	const auto place = [&](DeviceInstanceId id) {
		const size_t slot = engine_.slotOf(id);
		if (slot >= live_.size() || live_[slot].desc == nullptr) {
			return;
		}
		const size_t at = engine_.positionOfLane(slot);
		if (at != RackEngine::kNoSlot && at != target) {
			if (!engine_.moveDevice(at, target)) {
				settled = false;
			}
		}
		++target;
	};
	if (!want.order().empty()) {
		for (const DeviceInstanceId id : want.order()) {
			place(id);
		}
	}
	else {
		for (size_t chain = 0; chain < want.chainCount(); ++chain) {
			for (const DeviceState& row : want.chain(chain)) {
				place(row.instanceId);
			}
		}
	}

	// **EVERY ROW** — the base the engine resolves modulation against. THE WHOLE ROW, extra state
	// included (automod's folded travel, the shaper's phase space ride past the parameters). A control the
	// row does not carry is at its declared default, so it is written as the default rather than left at
	// whatever it last was; an absent extra float is zero, as a default row's is.
	for (size_t chain = 0; chain < want.chainCount(); ++chain) {
		for (const DeviceState& row : want.chain(chain)) {
			const size_t slot = engine_.slotOf(row.instanceId);
			if (slot >= live_.size() || live_[slot].desc == nullptr) {
				continue;
			}
			const device::Descriptor* desc = live_[slot].desc;
			const size_t present = std::min(row.fieldsPresent, row.field.size());
			const size_t width = engine_.fieldsPresentAt(slot);
			// **POLY READS OFF UNTIL ITS VOICES HAVE ARRIVED** (backlog NS). The record's switch went on and the
			// instances are built on the message thread after the publish (Feathers provisions on its editor tick),
			// so a drain can hold the new row before they come. Written as on, the block would run the device on
			// the bus with its switch reading on — a state the eager road never had. Written as off, the device
			// stays exactly as it was until the drain that seats its voices, and then flips as it always flipped.
			const int poly = desc->traits.perVoiceControl;
			const bool waiting = poly >= 0 && static_cast<size_t>(poly) < desc->params.size() && desc->audio != nullptr
			                     && engine_.voiceCount() > 0
			                     && engine_.slotAtLane(slot).voices.empty();
			for (size_t c = 0; c < width; ++c) {
				const float fallback = c < desc->params.size() ? static_cast<float>(desc->params[c].def) : 0.0f;
				const float value = c < present ? row.field[c] : fallback;
				engine_.setField(slot, c, waiting && c == static_cast<size_t>(poly) ? static_cast<float>(desc->params[c].min) : value);
			}
		}
	}

	// **AND THE HOST SLOTS' BINDINGS, AFTER THE ROWS** (host slots, T5) — resolved against where the engine
	// keeps each device now, arrivals included. Handing them over also resets what the engine last applied,
	// so the bus's value goes back over the row just written: a drain adopting a record older than a host
	// write cannot revert it (plan §0.3).
	engine_.setHostSlotBindings(want.slotBindings());
	// AND LAST, THE SITES OFFERED SINCE THE LAST DRAIN (backlog NR) — after the arrivals, so a device that landed in
	// this drain takes its site in this drain, and after the departures, which let go of a removed device's.
	engine_.adoptSites();
	settled_ = settled && held_ == nullptr;
	settledFor_ = published;
}

//------------------------------------------------------------------------------------------------//
// CHAIN ACCESS SITES (backlog NR)                                                                  //
//------------------------------------------------------------------------------------------------//

SiteChains* Converger::siteFor(DeviceInstanceId id) const noexcept {
	for (const HeldSite& held : sites_) {
		if (held.id == id && held.wanted) {
			return held.site.get();
		}
	}
	return nullptr;
}

void Converger::postSite(HeldSite& held, SiteChains* site) {
	auto* offer = new (std::nothrow) RackEngine::SiteOffer { held.id, site, nullptr };
	if (offer == nullptr) {
		return; // tried again on the next beat
	}
	++held.inFlight;
	engine_.offerSite(offer);
}

void Converger::serviceSites(const ChainState* want) {
	// **WHAT THE DRAIN HAS FINISHED WITH** — each envelope back, its site's count of offers in flight down by one. An
	// envelope for a site already freed cannot come back: a site is freed only with nothing in flight.
	RackEngine::SiteOffer* spent = engine_.takeSpentSiteOffers();
	while (spent != nullptr) {
		RackEngine::SiteOffer* next = spent->next;
		for (HeldSite& held : sites_) {
			if (held.id == spent->id && held.inFlight > 0) {
				--held.inFlight;
				break;
			}
		}
		delete spent;
		spent = next;
	}

	// **WHICH DEVICES WANT ONE** — when the state is in hand (`provision`): a live row whose carrier files a site, of a
	// kind that offers one. Built here, where building allocates, adopted and prepared at the host's rate, its routes'
	// outer gate and the host's slot link handed over before the body, as a container's are.
	if (want != nullptr) {
		for (HeldSite& held : sites_) {
			held.wanted = false;
		}
		for (size_t chain = 0; chain < want->chainCount(); ++chain) {
			for (const DeviceState& row : want->chain(chain)) {
				const device::Descriptor* desc = wantedAs(row, chain);
				if (desc == nullptr || row.instanceId == kNoInstance || bodies_ == nullptr
				    || siteLanesOf(*desc) == device::Traits::SiteLanes::None || !bodies_->has(siteKeyOf(row.instanceId))) {
					continue;
				}
				auto held = std::find_if(sites_.begin(), sites_.end(),
				                         [&](const HeldSite& h) { return h.id == row.instanceId; }); // an idle one is taken back whole
				if (held == sites_.end()) {
					HeldSite fresh;
					fresh.id = row.instanceId;
					fresh.site = std::make_unique<SiteChains>(row.instanceId, engine_.catalogue().inner(), engine_.sampleRate(),
					                                          engine_.maxFrames());
					const device::ContainerOps& ops = siteContainerOps();
					// THE HOST OUTSIDE, AS A CHAIN DEVICE IS HANDED IT: its routes' upstream test, and the dirty mark an edit
					// inside either lane raises on the host, so a save cannot miss it. AND NO VOICES: a site runs on the bus,
					// so a Poly switch inside one changes nothing and says so (`kPolyInsideSite`).
					device::OuterGate gate = makeGate_ != nullptr ? makeGate_(gateHost_, row.instanceId) : device::OuterGate {};
					gate.perVoiceUnavailable = kPolyInsideSite;
					ops.setOuterGate(fresh.site.get(), gate);
					// AND THE HOST'S CELLS, which is how POST reads what PRE exports (backlog NZ). Before the offer, so no
					// block is running either lane.
					fresh.site->setOuterSources(engine_.outerSourcesFor(row.instanceId));
					if (makeLink_ != nullptr) {
						ops.setHostSlots(fresh.site.get(), makeLink_(linkHost_, row.instanceId));
					}
					const std::span<const uint8_t> body = bodies_->bodyFor(siteKeyOf(row.instanceId));
					(void)fresh.site->loadState(body.data(), body.size());
					// THE RUNNING SITE'S HANDLE, BESIDE ITS BODY — what the host's save and its site editor read.
					bodies_->attach(siteKeyOf(row.instanceId), fresh.site.get());
					sites_.push_back(std::move(fresh));
					held = sites_.end() - 1;
				}
				held->wanted = true;
				// AT THE HOST'S RATE AND BLOCK — a site built before the host was prepared, or one the host re-prepared
				// under (a new rate is only ever set with no block running: both hosts prepare inside `activate`, and this is
				// the provision that follows it, so a site the engine still holds is not running either).
				if (engine_.sampleRate() > 0.0
				    && (held->site->preparedRate() != engine_.sampleRate() || held->site->preparedFrames() != engine_.maxFrames())) {
					held->site->prepare(engine_.sampleRate(), engine_.maxFrames());
				}
			}
		}
	}

	// **OFFER, LET GO, OR FREE** — every beat. A wanted site the engine does not hold is offered (again: an offer for a
	// device that had not landed came back installing nothing); an unwanted one the engine holds is detached; an
	// unwanted one held by nothing, with nothing in flight, waits for the block epoch to move and is freed — unless an
	// editor still has it pinned.
	const uint64_t now = engine_.blockEpochNow();
	for (HeldSite& held : sites_) {
		SiteChains* site = held.site.get();
		const bool installed = engine_.siteOf(held.id) == site;
		if (held.wanted) {
			held.idleSince = 0;
			if (!installed && held.inFlight == 0) {
				postSite(held, site);
			}
			(void)site->service();
			continue;
		}
		if (installed) {
			if (held.inFlight == 0) {
				postSite(held, nullptr);
			}
			continue;
		}
		if (held.inFlight > 0 || engine_.holdsSite(site)) {
			held.idleSince = 0;
			continue;
		}
		if (held.idleSince == 0) {
			held.idleSince = now + 1; // the epoch it was first seen idle, plus one so it is never zero
		}
	}
	std::erase_if(sites_, [&](HeldSite& held) {
		// SAFE ONCE NO BLOCK THAT COULD HAVE READ IT IS STILL RUNNING: none was in flight when it went idle (an even
		// epoch), or the epoch has moved past that block (`RackEngine::reapRetired`'s rule).
		const uint64_t idle = held.idleSince - 1;
		const bool passed = held.idleSince != 0 && ((idle & 1u) == 0u || now > idle);
		if (held.wanted || !passed || held.inFlight > 0 || pinned(held.site.get())) {
			return false;
		}
		if (bodies_ != nullptr) {
			// **ITS CONTENTS FILED BACK FIRST, while the entry is still there** — a host that keeps a removed device's
			// bodies (Feathers' lane, for its undo) then rebuilds the site WITH what it held when the device comes back,
			// rather than the bytes it was first built from. A host that dropped the entry (`refresh` finds none) loses
			// nothing it meant to keep.
			std::vector<uint8_t> bytes;
			const device::ByteSink sink { &bytes, [](void* ctx, const uint8_t* data, size_t n) {
				                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
				                             v->insert(v->end(), data, data + n);
			                             } };
			if (held.site->saveState(sink)) {
				(void)bodies_->refresh(siteKeyOf(held.id), std::move(bytes));
			}
			bodies_->detach(siteKeyOf(held.id), held.site.get());
		}
		return true;
	});
}

} // namespace feathers::rack
