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

#include "rack/nested_slots.h"

#include "device/format_value.h"
#include "rack/rack_device.h"

namespace feathers::rack {

// A PATH MUST HOLD THE DEEPEST NEST A RACK MAY BE LOADED TO, plus the device at its bottom: past it, `then`
// answers an empty path and a control that deep simply has no slot.
static_assert(kMaxDepth + 1 <= device::SlotPath::kCapacity, "a slot path must reach the deepest rack");

NestedSlots::NestedSlots(ChainRecord& record)
    : record_(record), committedGen_(kSlotBindingCapacity, 0) {}

bool NestedSlots::setLink(const device::HostSlotLink& link) {
	if (link_.sameAs(link)) {
		return false;
	}
	link_ = link;
	hasReported_ = false;
	captionsSent_ = false;
	reported_.clear();
	pulledGeneration_ = 0;
	return true;
}

void NestedSlots::setOwnCaptions(std::vector<device::HostSlotCaption> captions) {
	if (captions != captions_) {
		captions_ = std::move(captions);
		captionsSent_ = false;
	}
	report();
}

void NestedSlots::report() {
	if (!linked()) {
		return;
	}
	if (!captionsSent_ && link_.captions != nullptr) {
		link_.captions(link_.host, link_.prefix, captions_);
		captionsSent_ = true;
	}
	std::vector<device::HostSlotLevelDevice> now;
	for (size_t chain = 0; chain < record_.chainCount(); ++chain) {
		for (size_t position = 0; position < record_.length(chain); ++position) {
			const DeviceInstanceId id = record_.deviceAt(chain, position);
			now.push_back(device::HostSlotLevelDevice { .id = id, .desc = record_.descriptorOf(id) });
		}
	}
	if (hasReported_ && now == reported_) {
		return;
	}
	link_.report(link_.host, link_.prefix, link_.lane, now);
	reported_ = std::move(now);
	hasReported_ = true;
}

float NestedSlots::recordValueOf(DeviceInstanceId device, size_t control) const noexcept {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (desc == nullptr || control >= desc->params.size()) {
		return 0.0f;
	}
	const DeviceState& row = record_.rowOf(device);
	return control < row.fieldsPresent ? row.field[control] : static_cast<float>(desc->params[control].def);
}

//-------------------------------------------------------------------------------------------------//
// THE RIGHT-CLICK                                                                                 //
//-------------------------------------------------------------------------------------------------//

device::HostSlotStatus NestedSlots::statusOf(DeviceInstanceId device, size_t control) {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (!linked() || desc == nullptr || control >= desc->params.size() || desc->params[control].key == nullptr) {
		return {}; // no pool above (a third party's host), or nothing the pool could name
	}
	report();
	return link_.status(link_.host, pathOf(device), control);
}

bool NestedSlots::expose(DeviceInstanceId device, size_t control, std::string* displaced) {
	if (displaced != nullptr) {
		displaced->clear();
	}
	if (!linked() || !record_.holds(device)) {
		return false;
	}
	report();
	return link_.expose(link_.host, pathOf(device), control, displaced);
}

bool NestedSlots::unpin(DeviceInstanceId device, size_t control) {
	if (!linked() || !record_.holds(device)) {
		return false;
	}
	report();
	return link_.unpin(link_.host, pathOf(device), control);
}

void NestedSlots::touch(DeviceInstanceId device, size_t control) noexcept {
	if (!linked() || !record_.holds(device) || link_.touch == nullptr) {
		return;
	}
	try {
		report(); // the owner names the path from what this level reported
		link_.touch(link_.host, pathOf(device), control);
	}
	catch (...) {
		// out of memory on the message thread: the control stays unslotted
	}
}

void NestedSlots::deviceRemoved(DeviceInstanceId device) {
	if (!linked()) {
		return;
	}
	const device::SlotPath path = pathOf(device);
	if (!path.empty()) {
		link_.freeUnder(link_.host, path);
	}
	report(); // and the owner stops resolving anything under it at its next visit
}

void NestedSlots::deviceRestored(DeviceInstanceId device) {
	if (!linked() || link_.restoreUnder == nullptr) {
		return;
	}
	if (const device::SlotPath path = pathOf(device); !path.empty()) {
		link_.restoreUnder(link_.host, path);
	}
	report();
}

bool NestedSlots::deviceRebased(DeviceInstanceId device, DeviceInstanceId container, bool into) {
	if (!linked() || link_.rebase == nullptr) {
		return true; // no pool reaches this level: nothing to carry, and nothing lost
	}
	const device::SlotPath flat = pathOf(device);
	const device::SlotPath deep = pathOf(container).then(device);
	if (flat.empty() || deep.empty()) {
		return false;
	}
	const bool ok = into ? link_.rebase(link_.host, flat, deep) : link_.rebase(link_.host, deep, flat);
	report();
	return ok;
}

//-------------------------------------------------------------------------------------------------//
// A HAND                                                                                          //
//-------------------------------------------------------------------------------------------------//

hostslots::SlotIndex NestedSlots::boundTo(DeviceInstanceId device, size_t control) const noexcept {
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	for (size_t n = 0; n < bindings.size(); ++n) {
		if (bindings[n].live && bindings[n].device == device && bindings[n].control == control) {
			return static_cast<hostslots::SlotIndex>(n);
		}
	}
	return hostslots::kNoSlot;
}

bool NestedSlots::handOn(DeviceInstanceId device, size_t control) const noexcept {
	for (const Hand& h : hands_) {
		if (h.open && h.device == device && h.control == control) {
			return true;
		}
	}
	return false;
}

void NestedSlots::beginGesture(DeviceInstanceId device, size_t control) noexcept {
	// THE HAND IS NOTED SLOT OR NO SLOT (the owner's rule): one bound mid-drag opens its Begin at its first Value.
	if (!handOn(device, control)) {
		for (Hand& h : hands_) {
			if (!h.open) {
				h = Hand { device, control, true };
				break;
			}
		}
	}
	const hostslots::SlotIndex n = boundTo(device, control);
	if (!linked() || n == hostslots::kNoSlot) {
		return;
	}
	link_.beginGesture(link_.host, pathOf(device), control, n);
}

void NestedSlots::endGesture(DeviceInstanceId device, size_t control) noexcept {
	for (Hand& h : hands_) {
		if (h.open && h.device == device && h.control == control) {
			h.open = false;
		}
	}
	if (linked()) {
		link_.endGesture(link_.host, pathOf(device), control); // the slot the Begin went out on, the owner knows
	}
}

void NestedSlots::onHandWrite(DeviceInstanceId device, size_t control, float coerced, bool recordHadIt) noexcept {
	const hostslots::SlotIndex n = boundTo(device, control);
	Bus* const b = bus();
	if (b == nullptr || n == hostslots::kNoSlot || n >= committedGen_.size()) {
		return;
	}
	const device::Descriptor* desc = record_.descriptorOf(device);
	if (desc == nullptr || control >= desc->params.size()) {
		return;
	}
	// `SlotOwner::onHandWrite`'S RULE: a write of the value already there is no edit — unless a hand puts it
	// there over a host value the mirror has not committed, which is a real edit and wins.
	if (recordHadIt && (b->load(n).gen == committedGen_[n] || !handOn(device, control))) {
		return;
	}
	// THE BUS FIRST: this level's engine applies it over the record at every drain.
	committedGen_[n] = b->seed(n, coerced);
	link_.handValue(link_.host, pathOf(device), control, n,
	                static_cast<float>(device::toNormal(desc->params[control], coerced)), handOn(device, control));
}

//-------------------------------------------------------------------------------------------------//
// THE LEVEL'S VISIT                                                                               //
//-------------------------------------------------------------------------------------------------//

bool NestedSlots::pullBindings() {
	Bus* const b = bus();
	if (b == nullptr) {
		return false;
	}
	struct Pull {
		const ChainRecord* record;
		std::vector<SlotBinding> want;
	} pull { &record_, std::vector<SlotBinding>(kSlotBindingCapacity) };
	link_.bindingsUnder(
	    link_.host, link_.prefix, &pull,
	    [](void* ctx, uint32_t slot, DeviceInstanceId device, const char* tag, const char* key) {
		    auto& p = *static_cast<Pull*>(ctx);
		    // RESOLVED HERE, AGAINST THIS RECORD: the device must be one this level holds (a SKLITTER lane is
		    // told its sibling's bindings too), of the kind the target names, declaring its key.
		    const device::Descriptor* d = p.record->holds(device) ? p.record->descriptorOf(device) : nullptr;
		    if (slot >= p.want.size() || d == nullptr || tag == nullptr || key == nullptr
		        || !device::sameTag(d->traits.tag, tag)) {
			    return;
		    }
		    for (size_t c = 0; c < d->params.size(); ++c) {
			    if (d->params[c].key != nullptr && device::sameKey(d->params[c].key, key)) {
				    p.want[slot] = SlotBinding { .device = device,
					                             .control = static_cast<uint16_t>(c),
					                             .slot = static_cast<uint16_t>(slot),
					                             .live = true };
				    return;
			    }
		    }
	    });
	bool changed = false;
	const std::span<const SlotBinding> have = record_.state().slotBindings();
	for (size_t n = 0; n < pull.want.size() && n < have.size(); ++n) {
		if (have[n] == pull.want[n]) {
			continue;
		}
		// **A BINDING NEW TO THIS RECORD IS SEEDED FROM THIS RECORD FIRST**, before the record changes, so no drain
		// sees the binding without the seed — the owner's rule (`SlotOwner::republishBindings`), at this level.
		if (pull.want[n].live) {
			committedGen_[n] = b->seed(n, recordValueOf(pull.want[n].device, pull.want[n].control));
		}
		(void)record_.setSlotBinding(n, pull.want[n]);
		changed = true;
	}
	return changed;
}

bool NestedSlots::mirror() noexcept {
	Bus* const b = bus();
	if (b == nullptr) {
		return false;
	}
	bool changed = false;
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	for (size_t n = 0; n < bindings.size() && n < committedGen_.size(); ++n) {
		const SlotBinding bound = bindings[n];
		const device::Descriptor* d = bound.live ? record_.descriptorOf(bound.device) : nullptr;
		if (d == nullptr || bound.control >= d->params.size()) {
			continue;
		}
		const Bus::Snapshot seen = b->load(n);
		if (seen.gen == committedGen_[n]) {
			// THE BACKSTOP, as the owner's: a road that wrote this record and forgot the bus is seeded back by ONE
			// CAS against the snapshot compared, which loses to a host write landing in between.
			const float record = recordValueOf(bound.device, bound.control);
			if (record != device::coerceControl(d->params[bound.control], seen.value)) {
				if (const uint32_t gen = b->seedIf(n, seen, record); gen != 0) {
					committedGen_[n] = gen;
				}
			}
			continue;
		}
		// THE HOST MOVED IT: this level's knob, its save and its undo base follow — no undo step, not dirty.
		const float value = device::coerceControl(d->params[bound.control], seen.value);
		try {
			changed |= record_.setField(bound.device, bound.control, value);
		}
		catch (...) {
			continue; // tried again at the next visit: the generation is not committed
		}
		committedGen_[n] = seen.gen;
	}
	return changed;
}

} // namespace feathers::rack
