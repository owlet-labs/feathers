/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "device/modulatable.h"
#include "device/registry.h"
#include "hostslots/slot_bus.h"
#include "hostslots/slot_pool.h" // `SlotIndex`, `kNoSlot`
#include "rack/chain_record.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// **A NESTED RACK'S SHARE OF THE HOST SLOTS** (backlog JU, T11; design §7, plan §3).
//
// One pool, owned by the outermost plugin (`rack::SlotOwner`). A rack inside Feathers' lane, inside the standalone
// rack, or inside another rack holds no pool, declares nothing to any host, and saves no slots (its body carries no
// `hostSlots`: the outermost patch stores them by path). What it holds is THIS: the link up (`HostSlotLink`), and
// the half of the four roads (`slot_owner.h`'s banner) that only the level owning the record can walk —
//
// - ITS ENGINE READS THE ONE BUS for the slots bound to ITS devices: the bindings ride its own record's
//   `ChainState` exactly as the owner's do (T5), pulled from the owner when its binding generation moves
//   (`bindingsUnder`), each resolved against this record by the device's kind and the control's key, and each
//   new one SEEDED FROM THIS RECORD before the record publishes. Then the owner is told (`adopted`), which is
//   what lets it hand the audio thread the declaration the host's writes are scaled by.
// - THE MIRROR: a host write the owner's audio thread put on the bus is copied into this record (no undo, not
//   dirty), and a record some road wrote without seeding is seeded back by one CAS — the owner's rules, here.
// - A HAND: a write to a bound control seeds the bus and sends the owner the value for the host; a gesture goes
//   up by path, where the owner keeps one gesture per slot across every level.
// - A QUESTION ("is this control on a slot?"), a verb ("Expose to host") and a removal go up by path, after this
//   level has REPORTED its devices, which is how the owner resolves and names a path it holds no pointer into.
//
// MESSAGE THREAD throughout, like the record; the engine reads only the bus and its own published bindings.

namespace feathers::rack {

class NestedSlots {
public:
	using Bus = hostslots::SlotValueBus<>;

	/// `record` must outlive this.
	explicit NestedSlots(ChainRecord& record);
	NestedSlots(const NestedSlots&) = delete;
	NestedSlots& operator=(const NestedSlots&) = delete;

	/// **THE LINK UP.** True when it changed (another owner or place): what was reported and pulled is forgotten,
	/// so the next service reports and pulls afresh. A link that is not live leaves this level with no slots.
	bool setLink(const device::HostSlotLink& link);
	[[nodiscard]] bool linked() const noexcept { return link_.live(); }
	[[nodiscard]] const device::HostSlotLink& link() const noexcept { return link_; }
	[[nodiscard]] Bus* bus() const noexcept { return linked() ? link_.bus : nullptr; }
	/// The link a container inside this level is handed: this one with the child's id appended — or none.
	[[nodiscard]] device::HostSlotLink linkForChild(DeviceInstanceId child) const noexcept {
		return link_.forChild(child);
	}

	//--------------------------------------------------------------------------------------------//
	// THE RIGHT-CLICK — up by path                                                                 //
	//--------------------------------------------------------------------------------------------//

	[[nodiscard]] device::HostSlotStatus statusOf(DeviceInstanceId device, size_t control);
	bool expose(DeviceInstanceId device, size_t control, std::string* displaced);
	bool unpin(DeviceInstanceId device, size_t control);
	/// A HAND TOOK THE CONTROL (host slots T9): up by path, where the owner's recent ring gives it a slot.
	void touch(DeviceInstanceId device, size_t control) noexcept;
	[[nodiscard]] uint64_t generation() const noexcept {
		return linked() && link_.generation != nullptr ? link_.generation(link_.host) : 0;
	}
	/// A device of this level was removed: its slots, and every slot under it, are freed.
	void deviceRemoved(DeviceInstanceId device);
	/// **BACKLOG OA**: a device put back under its old id gets its freed slots back from their notes; and a device
	/// moved into `container` of this level (`into`) or back out of it, its slots with it. False when refused.
	void deviceRestored(DeviceInstanceId device);
	bool deviceRebased(DeviceInstanceId device, DeviceInstanceId container, bool into);

	//--------------------------------------------------------------------------------------------//
	// A HAND                                                                                       //
	//--------------------------------------------------------------------------------------------//

	void beginGesture(DeviceInstanceId device, size_t control) noexcept;
	void endGesture(DeviceInstanceId device, size_t control) noexcept;
	[[nodiscard]] hostslots::SlotIndex boundTo(DeviceInstanceId device, size_t control) const noexcept;
	/// `SlotOwner::recordHolds`, over this record.
	[[nodiscard]] bool recordHolds(DeviceInstanceId device, size_t control, float value) const noexcept {
		return record_.holds(device) && recordValueOf(device, control) == value;
	}
	/// `SlotOwner::onHandWrite`'s rules, this level's half: the bus first, and the owner told the value.
	void onHandWrite(DeviceInstanceId device, size_t control, float coerced, bool recordHadIt) noexcept;

	//--------------------------------------------------------------------------------------------//
	// THE LEVEL'S VISIT — on its host's beat (`ContainerOps::service`)                             //
	//--------------------------------------------------------------------------------------------//

	/// Report the devices, pull the bindings when the owner's generation moved (each new one seeded from the
	/// record), mirror the host's writes, then `publish()` (the level's own commit) when the record changed, and
	/// only then tell the owner it adopted. A publish that throws owes the adoption to the next visit.
	template <class Publish>
	void service(Publish&& publish) {
		if (!linked()) {
			return;
		}
		report();
		const uint64_t generation = link_.bindingGeneration(link_.host);
		bool changed = publishOwed_;
		if (generation != pulledGeneration_) {
			changed |= pullBindings();
		}
		changed |= mirror();
		if (changed) {
			publishOwed_ = true;
			try {
				publish();
			}
			catch (...) {
				return; // the record's change and the adoption are owed to the next visit
			}
			publishOwed_ = false;
		}
		if (generation != pulledGeneration_) {
			pulledGeneration_ = generation;
			link_.adopted(link_.host, link_.prefix, link_.lane, generation);
		}
	}

	/// Tell the owner what this level holds, if that changed since it was last told. Before any question.
	void report();

	/// **WHAT THIS CONTAINER CALLS ITS OWN CONTROLS** — a nested rack's renamed macros, which are controls of the
	/// rack DEVICE one level up: a slot on one is labelled "Chain · Wobble", and relabelled when it is renamed.
	/// Sent up now when linked (and again whenever a new link arrives); only the renamed ones.
	void setOwnCaptions(std::vector<device::HostSlotCaption> captions);

private:
	[[nodiscard]] device::SlotPath pathOf(DeviceInstanceId device) const noexcept { return link_.prefix.then(device); }
	[[nodiscard]] float recordValueOf(DeviceInstanceId device, size_t control) const noexcept;
	[[nodiscard]] bool handOn(DeviceInstanceId device, size_t control) const noexcept;
	/// This level's bindings from the owner's answer; true if the record changed.
	bool pullBindings();
	/// Steps 3 of `SlotOwner::service`, over this record's bindings; true if the record changed.
	bool mirror() noexcept;

	ChainRecord& record_;
	device::HostSlotLink link_ {};
	std::vector<device::HostSlotLevelDevice> reported_;
	bool hasReported_ = false;
	std::vector<device::HostSlotCaption> captions_;
	bool captionsSent_ = false;
	uint64_t pulledGeneration_ = 0;
	bool publishOwed_ = false;
	/// The bus generation each bound slot's value is in THIS record at (the owner keeps its own for its record).
	std::vector<uint32_t> committedGen_;
	struct Hand {
		DeviceInstanceId device = kNoInstance;
		size_t control = 0;
		bool open = false;
	};
	std::array<Hand, 8> hands_ {};
};

} // namespace feathers::rack
