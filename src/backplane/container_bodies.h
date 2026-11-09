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

#include "backplane/device_instance_id.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

// WHERE A CONTAINER'S BODY WAITS BETWEEN BEING READ AND HAVING SOMEWHERE TO GO.
//
// **IN BACKPLANE, NOT PLUGIN** (moved 2026-09-11): every host of a chain holds bodies — Feathers' bridge,
// the rack's, the shared patch road (`rack_state.h`) — and the rack is pinned to include nothing from a
// host (`test_rack_nesting.cpp`). It never depended on anything but backplane.
//
//------------------------------------------------------------------------------------------------//
// THE GAP THIS EXISTS TO SPAN, AND IT IS LONGER THAN IT LOOKS                                      //
//------------------------------------------------------------------------------------------------//
//
// A device's state arrives in three moves and they are far apart: `rack::parseRack` produces a ROW,
// `rack::applyPatchToRecord` puts that row in a chain, and provisioning builds the INSTANCE from its tag
// some time later. For every device in the tree that is fine, because a row is self-contained and
// `setFields` hands it over whenever the instance appears.
//
// **A CONTAINER'S BODY IS NOT A ROW AND CANNOT BECOME ONE.** `device_row.h` carries 128 floats and
// its own static_assert says the budget is spent; a chain's body is eight 536-byte rows plus its
// inner routes. So the body has to wait somewhere from the moment it is read until the container
// exists and has been adopted — and that somewhere must not be the row, must not be the chain, and
// must not be a pointer into the stream.
//
//------------------------------------------------------------------------------------------------//
// KEYED BY INSTANCE ID, WHICH IS THE ONLY KEY THAT SURVIVES THE GAP                                //
//------------------------------------------------------------------------------------------------//
//
// Not by slot: a chain is reordered, and `device_row.h`'s whole banner is about a lane travelling
// WITH its device rather than being derived from position. Not by tag: a chain may hold two
// composites and a tag names a KIND. The instance id is what a saved routing already names, what the
// record already carries, and what the patch reads with the device's tag (`rack_state.h`) — so it is
// available at exactly the moment a body is.
//
//------------------------------------------------------------------------------------------------//
// EACH BODY OWNS ITS BUFFER, AND THAT IS A STRUCTURAL CHOICE RATHER THAN A LAZY ONE                //
//------------------------------------------------------------------------------------------------//
//
// The obvious shape is one arena — a single `std::vector<uint8_t>` with offsets into it — and it is
// wrong here for a reason this repo has paid for twice. **A span into a growing vector is
// invalidated by the next push, silently**, and a caller holding one across a `store` would be
// reading freed memory with nothing to say so. That is the same shape as the pointer-identity rule
// in CLAUDE.md: an address is only an identity while the thing at it cannot be replaced.
//
// It could be documented against. It is better to make it impossible: one buffer per body means a
// stored body's bytes never move, so a span handed out stays valid until that body is erased, and no
// discipline is required of the caller at all. The cost is one allocation per container during a
// load, on the message thread, which is nothing — a load already allocates for the stream.
//
// MESSAGE THREAD ONLY. This allocates, and this repo forbids allocation on the audio thread after
// activate. Nothing here is reachable from a block, by construction: a body is read during a state
// load and consumed during provisioning, both of which are message-thread roads.

namespace feathers {

/// **WHERE A DEVICE'S CHAIN ACCESS SITE IS FILED** (backlog NR) — the device's own id with the top bit set, in the same
/// carrier as container bodies, so every road that carries bodies (a load, a save, an undo of the record) carries a
/// site's too. An entry under this key IS the device's wish for a site: present (even empty) is "this device has
/// one", absent is "none". Instance ids are a serial from 1 and never reach the bit.
inline constexpr DeviceInstanceId kSiteKeyBit = DeviceInstanceId { 1 } << 31;
[[nodiscard]] constexpr DeviceInstanceId siteKeyOf(DeviceInstanceId device) noexcept {
	return device == kNoInstance ? kNoInstance : device | kSiteKeyBit;
}

/// THE BODIES READ OUT OF ONE STATE BLOB, WAITING FOR THEIR CONTAINERS.
///
/// A loader fills it; a provisioner drains it. It owns every byte it hands out.
class ContainerBodies {
public:
	/// TAKE A BODY. Copies, because the bytes it is given belong to a stream buffer that will not
	/// outlive the read.
	///
	/// **A SECOND BODY FOR ONE ID REPLACES THE FIRST**, and the choice is deliberate: two records
	/// naming one instance is a malformed file, and `backplane::Graph::restore` already refuses the
	/// second node outright for the same reason (*"two nodes under one id is two devices at one
	/// address"*). Replacing rather than appending means the state here matches the state the graph
	/// will end up in, instead of holding a body for a device that was refused.
	///
	/// Refuses `kNoInstance`, which addresses nothing, and a null pointer with a non-zero length.
	/// An EMPTY body is stored and is not the same as no body — the distinction `device_row.h` keeps
	/// making, between "carried nothing" and "was not carried".
	bool store(DeviceInstanceId id, const uint8_t* data, size_t bytes) {
		if (id == kNoInstance) {
			return false;
		}
		if (data == nullptr && bytes != 0) {
			return false;
		}
		std::vector<uint8_t> copy(bytes);
		if (bytes != 0) {
			std::copy(data, data + bytes, copy.begin());
		}
		const uint64_t generation = nextGeneration();
		for (auto& entry : bodies_) {
			if (entry.id == id) {
				entry.bytes = std::move(copy);
				entry.generation = generation;
				return true;
			}
		}
		bodies_.push_back({ id, std::move(copy), generation });
		return true;
	}

	/// **THE SAME BODY, BROUGHT UP TO DATE** — the bytes a running instance holds now, written over the stored
	/// ones WITHOUT moving the generation, because they are not a new body: they are what the held one became
	/// through edits in the instance's own editor. For an instance about to be thrown away and built again
	/// (`Converger::dropPending`), so the rebuild is of the patch as edited, not as last loaded. False (and
	/// nothing changed) for an id with no entry.
	bool refresh(DeviceInstanceId id, std::vector<uint8_t> bytes) {
		for (auto& entry : bodies_) {
			if (entry.id == id) {
				entry.bytes = std::move(bytes);
				return true;
			}
		}
		return false;
	}

	/// **WHICH VERSION OF THIS DEVICE'S BODY IS HELD** — zero for none, and otherwise a number no other
	/// store of any body, in any `ContainerBodies`, has ever been given.
	///
	/// IT EXISTS SO A PROVISIONER CAN TELL "SAME CHAIN" FROM "SAME CHAIN, NEW CONTENTS". A runner
	/// skips rebuilding a device whose descriptor, tag and instance id are unchanged; loading a second
	/// patch whose chain happens to carry the same id would pass all three, and without this the chain
	/// would keep the first patch's members.
	///
	/// **GLOBAL AND MONOTONIC, BECAUSE A COUNTER PER OBJECT WOULD BE ABA.** A state load builds a
	/// fresh `ContainerBodies` and hands it to the bridge; if each object counted from one, the fresh
	/// body would be generation 1 exactly like the body it replaced, and the comparison that exists to
	/// see the change would see nothing. CLAUDE.md's rule: key on a monotonic serial stamped where the
	/// thing is made, so contents and key move together by construction.
	///
	/// **AND OF ITS BLOCKS** (`storeBlocks`): the larger of the two, so storing either moves it — both are stamped from
	/// the one monotonic counter, so the larger is the later store.
	[[nodiscard]] uint64_t generationOf(DeviceInstanceId id) const {
		uint64_t generation = 0;
		for (const auto& entry : bodies_) {
			if (entry.id == id) {
				generation = entry.generation;
				break;
			}
		}
		for (const auto& entry : blocks_) {
			if (entry.id == id) {
				return std::max(generation, entry.generation);
			}
		}
		return generation;
	}

	/// THE BODY FOR THIS DEVICE, or an EMPTY span for one that has none.
	///
	/// **AN EMPTY SPAN IS AMBIGUOUS AND `has` IS THE WAY TO DISAMBIGUATE IT** — a container that
	/// saved nothing and a device that never had a body both read as empty here. A caller that means
	/// "was there a body at all" must ask `has`; one that means "give me the bytes" may take the
	/// span. Two questions, two functions, because one function answering both is how a plausible
	/// default gets mistaken for a value.
	[[nodiscard]] std::span<const uint8_t> bodyFor(DeviceInstanceId id) const {
		for (const auto& entry : bodies_) {
			if (entry.id == id) {
				return { entry.bytes.data(), entry.bytes.size() };
			}
		}
		return {};
	}

	[[nodiscard]] bool has(DeviceInstanceId id) const {
		for (const auto& entry : bodies_) {
			if (entry.id == id) {
				return true;
			}
		}
		return false;
	}

	/// GIVE ONE UP once its container has taken it. A body consumed twice would mean two containers
	/// loaded one chain, so a provisioner that erases as it goes is one that cannot do that.
	bool erase(DeviceInstanceId id) {
		(void)uncarry(id);
		std::erase_if(blocks_, [id](const BlockEntry& e) { return e.id == id; });
		for (size_t i = 0; i < bodies_.size(); ++i) {
			if (bodies_[i].id == id) {
				bodies_.erase(bodies_.begin() + static_cast<std::ptrdiff_t>(i));
				return true;
			}
		}
		return false;
	}

	void clear() {
		bodies_.clear();
		carried_.clear();
		blocks_.clear();
	}

	//--------------------------------------------------------------------------------------------//
	// THE BLOCKS A DEVICE ARRIVED AT ITSELF (backlog NM, `device/state_blocks.h`)                    //
	//--------------------------------------------------------------------------------------------//
	//
	// **NOT A BODY**: `has`, `bodyFor` and `count` do not see them, and a device with blocks and no body has no body.
	// Kept by instance id beside the body, for the body's reason: a load reads them long before the instance exists.
	// Each block is the device's own index into its `StateOps::blocks` and its floats.

	struct Block {
		size_t index = 0;
		std::vector<float> floats;
	};

	/// **A LOAD'S BLOCKS FOR THIS DEVICE**, replacing whatever was kept — EVEN NONE, because storing is what moves the
	/// generation (`generationOf`), and a moved generation is what makes the converger build the device again and hand
	/// the new instance these (or, for none, nothing: it plays as freshly built, as the file says).
	void storeBlocks(DeviceInstanceId id, std::vector<Block> blocks) {
		if (id == kNoInstance) {
			return;
		}
		const uint64_t generation = nextGeneration();
		for (auto& entry : blocks_) {
			if (entry.id == id) {
				entry.blocks = std::move(blocks);
				entry.generation = generation;
				entry.serials.clear();
				return;
			}
		}
		blocks_.push_back({ id, std::move(blocks), generation, {} });
	}

	/// **ONE BLOCK BROUGHT UP TO DATE** with what the running instance published under `serial`, WITHOUT moving the
	/// generation: it is the same state, moved on (`refresh`'s reason). False when `serial` is the one last refreshed
	/// from, so a caller polling on a tick writes nothing while nothing moved.
	bool refreshBlock(DeviceInstanceId id, size_t index, std::span<const float> floats, uint64_t serial) {
		if (id == kNoInstance) {
			return false;
		}
		BlockEntry* entry = nullptr;
		for (auto& e : blocks_) {
			if (e.id == id) {
				entry = &e;
				break;
			}
		}
		if (entry == nullptr) {
			blocks_.push_back({ id, {}, 0, {} });
			entry = &blocks_.back();
		}
		const auto seen = std::find_if(entry->serials.begin(), entry->serials.end(),
		                               [index](const std::pair<size_t, uint64_t>& s) { return s.first == index; });
		if (seen != entry->serials.end() && seen->second == serial) {
			return false;
		}
		if (seen != entry->serials.end()) {
			seen->second = serial;
		}
		else {
			entry->serials.emplace_back(index, serial);
		}
		for (Block& b : entry->blocks) {
			if (b.index == index) {
				b.floats.assign(floats.begin(), floats.end());
				return true;
			}
		}
		entry->blocks.push_back({ index, std::vector<float>(floats.begin(), floats.end()) });
		return true;
	}

	/// THE BLOCKS KEPT FOR THIS DEVICE, or null when none were ever stored or refreshed.
	[[nodiscard]] const std::vector<Block>* blocksOf(DeviceInstanceId id) const {
		for (const auto& entry : blocks_) {
			if (entry.id == id) {
				return &entry.blocks;
			}
		}
		return nullptr;
	}

	//--------------------------------------------------------------------------------------------//
	// WHAT A RE-SAVE MUST WRITE BACK THAT NO ROW CAN HOLD (backlog JF §3.6)                          //
	//--------------------------------------------------------------------------------------------//
	//
	// **NOT APPLIED, BUT CARRIED.** A value a newer build wrote under a key this build does not know, and
	// every value of a device this build cannot run at all, must survive a load and a save here — or
	// opening a patch in an older build and saving it quietly deletes the newer data. A row is floats by
	// index and has no room for a key, so the leftovers wait beside the body, under the same instance id,
	// as the document text `rack_state.cpp` wrote them in. NOT A BODY: `has`, `bodyFor` and `count` do not
	// see them, and a device with leftovers and no body has no body.

	/// Keep `text` for this device, replacing what was kept; an empty text keeps nothing.
	void carry(DeviceInstanceId id, std::string text) {
		if (id == kNoInstance) {
			return;
		}
		(void)uncarry(id);
		if (!text.empty()) {
			carried_.emplace_back(id, std::move(text));
		}
	}
	/// What is kept for this device, or null.
	[[nodiscard]] const std::string* carriedFor(DeviceInstanceId id) const {
		for (const auto& [held, text] : carried_) {
			if (held == id) {
				return &text;
			}
		}
		return nullptr;
	}
	bool uncarry(DeviceInstanceId id) {
		for (size_t i = 0; i < carried_.size(); ++i) {
			if (carried_[i].first == id) {
				carried_.erase(carried_.begin() + static_cast<std::ptrdiff_t>(i));
				return true;
			}
		}
		return false;
	}

	/// HOW MANY BODIES ARE HELD — during a load, how many were read; after a provisioner has drained
	/// what it claimed, **what was never claimed**.
	///
	/// ONE FUNCTION RATHER THAN A SECOND CALLED `unclaimed`, which the first draft of this file had:
	/// two names computing the same number is a redundancy that buys nothing, and this repo's rule is
	/// to find out what an apparent redundancy is buying before keeping it. This one was buying a
	/// sentence, and the sentence belongs here.
	///
	/// A NON-ZERO COUNT AFTER PROVISIONING IS A REPORT RATHER THAN AN ERROR, and it has a legitimate
	/// cause: a file written by a build with a container this one lacks. That is the road the patch
	/// already takes for an unknown tag (`rack_state.h`: kept, in its place, and saved back out), and the
	/// same posture — read it, do not pretend it was not there, and let the caller decide.
	[[nodiscard]] size_t count() const { return bodies_.size(); }

	//--------------------------------------------------------------------------------------------//
	// THE RUNNING INSTANCE, BESIDE ITS BODY (rack roadmap §6, Stage B)                             //
	//--------------------------------------------------------------------------------------------//

	/// **FILE THE RUNNING INSTANCE'S HANDLE UNDER ITS ID** — an opaque pointer the party that BUILT the
	/// instance (the converger, at provision, on the message thread) hands over, and the host's UI hands
	/// on to the module that knows what it is: a rack's expanded view reads a `RackBridge` out of it
	/// (`rack::bridgeOf`), and the host's save asks it for the chain's current contents
	/// (`ContainerOps::saveState`), so a chain edited through its own editor is saved as edited.
	///
	/// A device with no body yet — a fresh chain — gets an entry with an empty body and generation 0, so
	/// it has somewhere to be filed; `store` on it later moves the generation as for any body.
	///
	/// **THE ABA RULE APPLIES TO THIS POINTER AS TO ANY ADDRESS**: it is a contemporaneous handle, not an
	/// identity. A reader re-asks by id every tick and treats a different answer — or none — as a
	/// different instance, because a rebuilt device is one.
	void attach(DeviceInstanceId id, void* surface) {
		if (id == kNoInstance) {
			return;
		}
		for (auto& entry : bodies_) {
			if (entry.id == id) {
				entry.surface = surface;
				return;
			}
		}
		bodies_.push_back({ id, {}, 0, surface });
	}

	/// CLEAR THE HANDLE, BUT ONLY IF IT IS STILL THIS ONE — a device replaced under its own id has a newer
	/// instance filed by the time the old one is forgotten, and that one must stay.
	void detach(DeviceInstanceId id, const void* surface) {
		for (auto& entry : bodies_) {
			if (entry.id == id && entry.surface == surface) {
				entry.surface = nullptr;
				return;
			}
		}
	}
	/// Clear whatever handle is filed — for an id the engine no longer runs at all.
	void detach(DeviceInstanceId id) {
		for (auto& entry : bodies_) {
			if (entry.id == id) {
				entry.surface = nullptr;
				return;
			}
		}
	}

	[[nodiscard]] void* surfaceOf(DeviceInstanceId id) const {
		for (const auto& entry : bodies_) {
			if (entry.id == id) {
				return entry.surface;
			}
		}
		return nullptr;
	}

	/// **AN INSTANCE AN EDITOR IS BUILT OVER IS NOT FREED UNDER IT** (backlog JX). A panel that follows the
	/// handle holds the bridge it read out of it until its NEXT follow — and the reap that frees a retired
	/// instance can run first: Feathers reaps after the beat's follow (`guiTick`), and every rack commit
	/// reaps from whatever handler made it. So the holder pins the storage and the freeing roads
	/// (`Converger::reap`, through `RackEngine::setReapHold`) leave a pinned instance parked until it is
	/// unpinned. The handle is still cleared on time — a pin keeps the MEMORY, not the answer — so the
	/// holder's next follow sees the instance gone, drops its editor, and unpins. Counted, because two
	/// panels may hold one instance. Kept apart from `bodies_`, so `clear()` on a load cannot drop a pin a
	/// live panel still holds. And while an address is pinned nothing new can be built at it, so the
	/// holder's comparison of a remembered handle against this tick's answer cannot be fooled by reuse.
	void pin(const void* surface) {
		if (surface == nullptr) {
			return;
		}
		for (auto& [held, count] : pins_) {
			if (held == surface) {
				++count;
				return;
			}
		}
		pins_.emplace_back(surface, 1);
	}
	void unpin(const void* surface) {
		for (size_t i = 0; i < pins_.size(); ++i) {
			if (pins_[i].first == surface) {
				if (--pins_[i].second == 0) {
					pins_.erase(pins_.begin() + static_cast<std::ptrdiff_t>(i));
				}
				return;
			}
		}
	}
	[[nodiscard]] bool pinned(const void* surface) const {
		for (const auto& [held, count] : pins_) {
			if (held == surface) {
				return true;
			}
		}
		return false;
	}

private:
	/// MESSAGE THREAD in every caller, and atomic anyway: two plugin instances in one host process
	/// share this counter, and a body's generation must be unique across both of them or a copy of
	/// one could shadow the other's.
	static uint64_t nextGeneration() {
		static std::atomic<uint64_t> counter { 1 };
		return counter.fetch_add(1, std::memory_order_relaxed);
	}

	struct Entry {
		DeviceInstanceId id = kNoInstance;
		/// ITS OWN BUFFER. See the banner: one arena with offsets would make every span a hostage to
		/// the next store.
		std::vector<uint8_t> bytes;
		uint64_t generation = 0;
		/// THE RUNNING INSTANCE'S HANDLE — see `attach`. Not a body; never saved; cleared when the instance goes.
		void* surface = nullptr;
	};

	std::vector<Entry> bodies_;
	std::vector<std::pair<DeviceInstanceId, std::string>> carried_;
	/// One device's blocks (`storeBlocks`), the generation they were stored under, and the serial each block was last
	/// refreshed from (`refreshBlock`).
	struct BlockEntry {
		DeviceInstanceId id = kNoInstance;
		std::vector<Block> blocks;
		uint64_t generation = 0;
		std::vector<std::pair<size_t, uint64_t>> serials;
	};
	std::vector<BlockEntry> blocks_;
	/// Pinned instance storage and how many holders each has — see `pin`.
	std::vector<std::pair<const void*, size_t>> pins_;
};

} // namespace feathers
