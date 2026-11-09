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

#include "rack/slot_owner.h"

#include "device/format_value.h"
#include "device/registry.h"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <utility>

namespace feathers::rack {

namespace {

/// **WHAT THE TRAY SAYS WHEN A TAKE IS NEWS** (T9;), or empty when it is not. Slots are
/// numbered from 1, as the host lists them.
/// - A HOST-MARKED slot: "Slot 12 had host automation or modulation; it now drives Filter · Cutoff" — the
///   host's lanes on that id are still there, and now move this control.
/// - A used Recent recycled: "Host slot 23: Delay · Mix -> Filter · Cutoff" — the label it held and the one
///   it holds now (" -> ": the editor's font, Lato, has no arrow glyph).
std::string takenNotice(const hostslots::Allocation& took, const hostslots::Target& now) {
	const std::string number = std::to_string(took.slot + 1);
	if (took.hostMarked) {
		return "Slot " + number + " had host automation or modulation; it now drives " + now.label;
	}
	if (took.announce && took.previous.has_value()) {
		return "Host slot " + number + ": " + took.previous->label + " -> " + now.label;
	}
	return {};
}

} // namespace

//-------------------------------------------------------------------------------------------------//
// TARGETS AND NAMES                                                                            //
//-------------------------------------------------------------------------------------------------//

std::optional<hostslots::Target> SlotOwner::targetOf(DeviceInstanceId device, size_t control) const {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	// A CONTROL NO SLOT MAY CARRY (`ParamDesc::hostSlot`) HAS NO TARGET, so neither a pin nor a touch can seat it.
	if (desc == nullptr || control >= desc->params.size() || desc->params[control].key == nullptr
	    || !desc->params[control].hostSlot) {
		return std::nullopt;
	}
	hostslots::Target target;
	target.path = { device };
	target.key = desc->params[control].key;
	target.tag = desc->traits.tag != nullptr ? desc->traits.tag : "";
	target.label = labelOf(device, control);
	return target;
}

std::string SlotOwner::labelOf(DeviceInstanceId device, size_t control) const {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (desc == nullptr || control >= desc->params.size() || desc->traits.name == nullptr) {
		return {};
	}
	// " · ", the route list's separator. The caption is the container's own name for it, when it gave one (T11).
	return ownDeviceName(device) + " \xc2\xb7 " + captionAt(std::span<const DeviceInstanceId>(&device, 1), *desc, control);
}

std::string SlotOwner::captionAt(std::span<const DeviceInstanceId> path, const device::Descriptor& desc,
                                 size_t control) const {
	for (const Caption& c : captions_) {
		if (c.caption.control == control && std::equal(c.path.begin(), c.path.end(), path.begin(), path.end())) {
			return c.caption.name;
		}
	}
	return captionOf(desc, control);
}

std::string SlotOwner::labelForTarget(const hostslots::Target& target) const {
	const device::Descriptor* desc = nullptr;
	device::SlotPath path;
	for (size_t i = 0; i < target.path.size() && i < device::SlotPath::kCapacity; ++i) {
		path.ids[path.depth++] = target.path[i];
	}
	if (target.path.size() == 1) {
		desc = record_.holds(target.path[0]) ? record_.descriptorOf(target.path[0]) : nullptr;
	}
	else if (target.path.size() > 1 && target.path.size() <= device::SlotPath::kCapacity) {
		desc = reportedDescriptorOf(path);
	}
	if (desc != nullptr) {
		for (size_t c = 0; c < desc->params.size(); ++c) {
			if (desc->params[c].key != nullptr && device::sameKey(desc->params[c].key, target.key.c_str())) {
				return target.path.size() == 1 ? labelOf(target.path[0], c) : nestedLabelOf(path, c);
			}
		}
	}
	return target.label;
}

bool SlotOwner::reportCaptions(const device::SlotPath& path, std::span<const device::HostSlotCaption> captions) {
	if (path.empty()) {
		return false;
	}
	const std::span<const DeviceInstanceId> p = path.span();
	std::vector<Caption> next;
	for (const Caption& c : captions_) {
		if (!std::equal(c.path.begin(), c.path.end(), p.begin(), p.end())) {
			next.push_back(c);
		}
	}
	for (const device::HostSlotCaption& c : captions) {
		next.push_back(Caption { .path = { p.begin(), p.end() }, .caption = c });
	}
	const auto same = [](const Caption& a, const Caption& b) { return a.path == b.path && a.caption == b.caption; };
	if (std::equal(next.begin(), next.end(), captions_.begin(), captions_.end(), same)) {
		return false;
	}
	captions_ = std::move(next);
	// EVERY SLOT UNDER THE CONTAINER RELABELLED — a rebase onto the same path moves nothing and renames each,
	// so the host hears the new names at the next visit as one rescan.
	(void)pool_.rebase(p, p, [this](const hostslots::Target& t) { return labelForTarget(t); });
	return true;
}

uint32_t SlotOwner::retextUnder(const device::SlotPath& path) {
	return path.empty() ? 0u : pool_.retextUnder(path.span());
}

void SlotOwner::retextIfWordsFollow(DeviceInstanceId device, size_t control) noexcept {
	// **A CONTROL THAT GATES ANOTHER, ON A DEVICE THAT PRINTS ITS OWN WORDS IN THE DAW** (`TextOps::parse`): the gated
	// control's text may follow it — the Rack's position reads "Chain 2" in Select and a percentage in Parallel — so
	// the host is owed new text for the device's slots. Declared, not special-cased: the gate says which control.
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (desc == nullptr || desc->text == nullptr || desc->text->format == nullptr || desc->text->parse == nullptr) {
		return;
	}
	// OR THE CONTROL SAYS SO ITSELF (`ParamDesc::retextsWords`): the words follow it without it gating anything.
	if (control < desc->params.size() && desc->params[control].retextsWords) {
		(void)pool_.retextUnder(device::SlotPath::of(device).span());
		return;
	}
	for (const device::ParamDesc& p : desc->params) {
		if (p.gateControl >= 0 && static_cast<size_t>(p.gateControl) == control) {
			(void)pool_.retextUnder(device::SlotPath::of(device).span());
			return;
		}
	}
}

bool SlotOwner::textSite(const hostslots::Target& target, DeviceInstanceId* device, hostslots::TextSite* site) const {
	// ONE LEVEL ONLY: a nested level reports its devices' declarations, not their rows or their instances, so a
	// slot two racks down prints in its declared unit (rack stage 2 leaves that open).
	if (target.path.size() != 1 || !record_.holds(target.path[0]) || site == nullptr) {
		return false;
	}
	const DeviceInstanceId id = target.path[0];
	const device::Descriptor* d = record_.descriptorOf(id);
	if (d == nullptr || (!target.tag.empty() && !device::sameTag(d->traits.tag, target.tag.c_str()))) {
		return false;
	}
	for (size_t i = 0; i < d->params.size(); ++i) {
		if (d->params[i].key != nullptr && device::sameKey(d->params[i].key, target.key.c_str())) {
			const DeviceState& row = record_.rowOf(id);
			site->text = d->text;
			site->control = i;
			site->fields.resize(d->params.size());
			for (size_t f = 0; f < d->params.size(); ++f) {
				site->fields[f] = f < row.fieldsPresent ? row.field[f] : static_cast<float>(d->params[f].def);
			}
			if (device != nullptr) {
				*device = id;
			}
			return true;
		}
	}
	return false;
}

std::string SlotOwner::ownDeviceName(DeviceInstanceId device) const {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (desc == nullptr || desc->traits.name == nullptr) {
		return {};
	}
	std::string label = desc->traits.name;
	// NUMBERED ONLY WHEN THERE IS ANOTHER OF ITS KIND to tell it from — by its position in its own chain, the
	// number the route list gives it (Feathers' `modSourceName`: the rack's position, else the lane's; the
	// standalone rack's `destinationName`: its one chain's).
	bool twin = false;
	for (size_t chain = 0; chain < record_.chainCount() && !twin; ++chain) {
		for (size_t position = 0; position < record_.length(chain); ++position) {
			const DeviceInstanceId other = record_.deviceAt(chain, position);
			if (other != device && record_.descriptorOf(other) == desc) {
				twin = true;
				break;
			}
		}
	}
	if (twin) {
		if (const size_t position = record_.positionOf(device); position != ChainRecord::kNoPosition) {
			label += " ";
			label += std::to_string(position + 1);
		}
	}
	return label;
}

std::string SlotOwner::captionOf(const device::Descriptor& desc, size_t control) {
	// THE CONTROL AS ITS CARD CAPTIONS IT (`generic_card.cpp`'s `captionFor`): a declared short form wins,
	// else the name with the device's own leading "<device> " dropped — "Filter · Cutoff", not "Filter ·
	// Filter Cutoff". The label already says which device, which is the reason a card strips it too.
	if (control >= desc.params.size()) {
		return {};
	}
	const device::ParamDesc& p = desc.params[control];
	if (p.shortName != nullptr && p.shortName[0] != '\0') {
		return p.shortName;
	}
	const std::string_view declared = p.name == nullptr ? std::string_view {} : p.name;
	const std::string_view deviceName = desc.traits.name == nullptr ? std::string_view {} : desc.traits.name;
	if (!deviceName.empty() && declared.size() > deviceName.size() + 1
	    && declared.compare(0, deviceName.size(), deviceName) == 0 && declared[deviceName.size()] == ' ') {
		return std::string(declared.substr(deviceName.size() + 1));
	}
	return std::string(declared);
}

bool SlotOwner::resolve(const hostslots::Target& target, const device::ParamDesc** desc, double* value) const {
	// A NESTED PATH (T11): resolved from what its levels reported. A PENDING one — a level on its path has not
	// reported yet, as none has in the moment after a load — still resolves here, named from the catalogue, so
	// a reconcile keeps it (an anchor must survive the load it was saved for) and the host still sees its
	// name; it binds nothing until its level reports (`republishBindings` asks `reachNested` itself). Its value
	// is the bus's once a level has bound it and adopted the binding (the bus is the authority the level's
	// engine applies, seeded from its record), else the control's default.
	if (target.path.size() > 1) {
		const device::Descriptor* leaf = nullptr;
		if (reachNested(target, &leaf) == Reach::Gone || leaf == nullptr) {
			return false;
		}
		for (const device::ParamDesc& p : leaf->params) {
			if (p.key != nullptr && device::sameKey(p.key, target.key.c_str())) {
				if (desc != nullptr) {
					*desc = &p;
				}
				if (!p.hostSlot) {
					return false; // no slot may carry it: a reconcile frees whatever names it
				}
				if (value != nullptr) {
					*value = p.def;
					const hostslots::SlotIndex n = pool_.slotOf(target);
					if (n < nestedBindings_.size() && nestedBindings_[n].live && !held_[n]) {
						*value = device::coerceControl(p, bus_.valueOf(n));
					}
				}
				return true;
			}
		}
		return false;
	}
	// ONE LEVEL: this record's own chains.
	if (target.path.size() != 1 || !record_.holds(target.path[0])) {
		return false;
	}
	const DeviceInstanceId device = target.path[0];
	const device::Descriptor* d = record_.descriptorOf(device);
	if (d == nullptr || (!target.tag.empty() && !device::sameTag(d->traits.tag, target.tag.c_str()))) {
		return false;
	}
	for (size_t i = 0; i < d->params.size(); ++i) {
		if (d->params[i].key != nullptr && device::sameKey(d->params[i].key, target.key.c_str())) {
			const DeviceState& row = record_.rowOf(device);
			if (desc != nullptr) {
				*desc = &d->params[i];
			}
			if (!d->params[i].hostSlot) {
				return false; // no slot may carry it: a reconcile frees whatever names it
			}
			if (value != nullptr) {
				*value = i < row.fieldsPresent ? static_cast<double>(row.field[i]) : d->params[i].def;
			}
			return true;
		}
	}
	return false;
}

uint32_t SlotOwner::reconcile() {
	// ONE SLOT AT A TIME (`SlotPool::free`), each keeping its note, so a reconcile renames exactly the slots
	// it frees and no unrelated undo note is lost. Nothing to free is nothing done — a load whose devices all
	// still stand costs the host no rescan.
	// A NESTED LEVEL WHOSE PREFIX NO LONGER REACHES — its container removed, replaced by a device that holds
	// nothing, or its own holder gone — is forgotten first, so what it reported cannot resolve a target (T11).
	const size_t levelsBefore = levels_.size();
	std::erase_if(levels_, [this](const NestedLevel& level) { return !prefixReaches(level.prefix); });
	if (levels_.size() != levelsBefore) {
		++levelsRevision_;
	}
	std::erase_if(captions_, [this](const Caption& c) { return !prefixReaches(c.path); });
	uint32_t freed = 0;
	for (hostslots::SlotIndex n = 0; n < pool_.capacity(); ++n) {
		if (pool_.state(n) != hostslots::SlotState::Empty && !resolve(pool_.target(n), nullptr, nullptr)
		    && pool_.free(n)) {
			++freed;
		}
	}
	reconciledStructure_ = slotStructure();
	reconciledGeneration_ = pool_.generation();
	reconciledLevelsRevision_ = levelsRevision_;
	return freed;
}

SlotOwner::SlotStructure SlotOwner::slotStructure() const noexcept {
	SlotStructure s;
	s.valid = true;
	for (size_t chain = 0; chain < record_.chainCount() && !s.overflow; ++chain) {
		for (size_t position = 0; position < record_.length(chain); ++position) {
			if (s.count >= s.devices.size()) {
				s.overflow = true;
				break;
			}
			const DeviceInstanceId id = record_.deviceAt(chain, position);
			s.devices[s.count++] = SlotStructure::Entry { id, record_.descriptorOf(id) };
		}
	}
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	std::copy(bindings.begin(),
	          bindings.begin() + static_cast<std::ptrdiff_t>(std::min(bindings.size(), s.bindings.size())),
	          s.bindings.begin());
	return s;
}

uint32_t SlotOwner::reconcileIfMoved() {
	// **GATED ON THE STRUCTURE, NOT THE RECORD'S REVISION** (T6b review): the revision moves on every field
	// write, and while host automation plays the mirror writes one every visit, so a revision gate ran the
	// whole reconcile — a target string compare per slot — on every visit for nothing. A target stops
	// resolving only when a device goes, or another kind stands under its id, and the structure is exactly
	// which device (by id and declaration) is where. The pool's generation covers a mapping change.
	// AND ON WHAT THE NESTED LEVELS REPORTED (T11): a device removed inside a rack changes nothing here.
	if (reconciledStructure_ == slotStructure() && pool_.generation() == reconciledGeneration_
	    && levelsRevision_ == reconciledLevelsRevision_) {
		return 0;
	}
	return reconcile();
}

SlotOwner::Refused SlotOwner::restore(const std::optional<hostslots::PoolSnapshot>& snapshot) {
	// **THE LOAD WINDOW** (integration review): from here until the next visit rebinds, a slot's declaration
	// is the OLD patch's, and its host-wrote and touched bits are about the OLD pool. Left alone, a host
	// write in the window would be scaled by the old declaration, and its bit would anchor whatever the
	// restored pool put on that slot. So every declaration is nulled — a host write in the window is dropped,
	// as in the remap window — the bits taken so far are discarded, and the next visit is made to rebind
	// (which republishes every declaration after the owner's publish).
	for (std::atomic<const device::ParamDesc*>& d : desc_) {
		d.store(nullptr, std::memory_order_release);
	}
	(void)bus_.takeHostWrote();
	(void)bus_.takeRecTouched();
	(void)bus_.takeHostMarked();
	touched_.fill(false); // a touch on the old pool reports nothing about the restored one (T9)
	nestedTakeoverOwed_.fill(false);
	boundGeneration_ = UINT64_MAX;
	// THE NESTED BINDINGS ARE THE OLD POOL'S TOO (T11): forgotten, so every one the restored pool makes is NEW —
	// held off the audio thread until its level has pulled it, seeded its bus from its record and published.
	std::fill(nestedBindings_.begin(), nestedBindings_.end(), NestedBinding {});
	++nestedBindingGen_;
	if (!snapshot.has_value() || !pool_.restore(*snapshot)) {
		(void)pool_.restore(hostslots::PoolSnapshot {});
	}
	// A SAVED ENTRY NAMING A CONTROL NO SLOT MAY CARRY (`ParamDesc::hostSlot`) — a map written before the control said so
	// — is counted here for the report; the reconcile below frees it, because `resolve` refuses it.
	Refused refused;
	for (hostslots::SlotIndex n = 0; n < pool_.capacity(); ++n) {
		const device::ParamDesc* desc = nullptr;
		if (pool_.state(n) != hostslots::SlotState::Empty && !resolve(pool_.target(n), &desc, nullptr)
		    && desc != nullptr && !desc->hostSlot) {
			if (refused.count++ == 0) {
				refused.first = pool_.target(n).label.empty() ? pool_.target(n).key : pool_.target(n).label;
			}
		}
	}
	(void)reconcile();
	return refused;
}

device::HostSlotStatus SlotOwner::statusOf(DeviceInstanceId device, size_t control) const {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (desc == nullptr || control >= desc->params.size() || desc->params[control].key == nullptr
	    || !desc->params[control].hostSlot) {
		return {}; // nothing the pool could name (or may: `ParamDesc::hostSlot`): no entry, nothing to expose
	}
	// ONE WALK, TWO ANSWERS, NO `Target` BUILT: `targetOf` composes a label (a twin search over every chain)
	// that identity does not need. Identity is the path and the key (`Target::sameControl`); this record is
	// the outermost level, so the path is `[device]`.
	return statusFor(std::span<const DeviceInstanceId>(&device, 1), desc->params[control].key);
}

device::HostSlotStatus SlotOwner::statusFor(std::span<const DeviceInstanceId> path, const char* key) const {
	using State = device::HostSlotStatus::State;
	device::HostSlotStatus out;
	out.state = State::Unslotted;
	for (hostslots::SlotIndex n = 0; n < pool_.capacity(); ++n) {
		const hostslots::SlotState st = pool_.state(n);
		// A PIN FINDS A SLOT exactly when `SlotPool::takeFor` would: an Empty, a Default (every Default is
		// untouched — a touch leaves it Default and only an anchor protects it), or a Recent as the last
		// resort. Only a pool of Pinned and Anchored has nothing to give.
		if (st == hostslots::SlotState::Empty || st == hostslots::SlotState::Default
		    || st == hostslots::SlotState::Recent) {
			out.canExpose = true;
		}
		if (st == hostslots::SlotState::Empty) {
			continue;
		}
		const hostslots::Target& t = pool_.target(n);
		if (std::equal(t.path.begin(), t.path.end(), path.begin(), path.end()) && device::sameKey(key, t.key.c_str())) {
			out.slot = n;
			switch (st) {
				case hostslots::SlotState::Default: out.state = State::Default; break;
				case hostslots::SlotState::Recent: out.state = State::Recent; break;
				case hostslots::SlotState::Pinned: out.state = State::Pinned; break;
				case hostslots::SlotState::Anchored: out.state = State::Anchored; break;
				case hostslots::SlotState::Empty: break;
			}
		}
	}
	// A CONTROL THAT HAS A SLOT CAN ALWAYS BE PINNED: the pin keeps its own slot and takes nothing.
	if (out.slotted()) {
		out.canExpose = true;
	}
	return out;
}

bool SlotOwner::expose(DeviceInstanceId device, size_t control, std::string* displaced) {
	if (displaced != nullptr) {
		displaced->clear();
	}
	const std::optional<hostslots::Target> target = targetOf(device, control);
	if (!target.has_value()) {
		return false;
	}
	const uint64_t before = pool_.generation();
	const hostslots::Allocation took = pool_.pin(*target);
	if (took.slot == hostslots::kNoSlot) {
		return false; // every slot Pinned or Anchored: the menu offered this switched off
	}
	// THE LAST RESORT IS SAID (design §2): a used Recent slot now names this control, and a host lane on it,
	// if the host kept one, now drives something else. The caller has the tray.
	if (displaced != nullptr && took.announce && took.previous.has_value()) {
		*displaced = took.previous->label;
	}
	// AND A HOST-MARKED SLOT IS SAID IN ITS OWN WORDS (2026-09-26) — through the tray's poll (`takeNotice`),
	// which the knob's "taken from" line cannot carry: a marked Empty had no label to be taken from.
	if (took.hostMarked) {
		notice_ = takenNotice(took, *target);
	}
	return pool_.generation() != before; // already Pinned (or Anchored over a pin): nothing moved
}

bool SlotOwner::unpin(DeviceInstanceId device, size_t control) {
	const device::HostSlotStatus status = statusOf(device, control);
	return status.state == device::HostSlotStatus::State::Pinned
	       && pool_.unpin(static_cast<hostslots::SlotIndex>(status.slot));
}

std::optional<hostslots::PoolSnapshot> SlotOwner::snapshotForSave() const {
	hostslots::PoolSnapshot slots = pool_.snapshot();
	if (slots.slots.empty() && slots.hostMarked.empty()) { // a mark alone is saved: the host's lane outlives it
		return std::nullopt;
	}
	return slots;
}

//-------------------------------------------------------------------------------------------------//
// THE WRITE ROADS                                                                                 //
//-------------------------------------------------------------------------------------------------//

bool SlotOwner::onHostValue(hostslots::SlotIndex n, double normal) noexcept {
	const device::ParamDesc* desc = descOf(n);
	if (desc == nullptr) {
		bus_.onHostTouched(n); // nothing to write to, but the host has a lane on this id: marked at the visit
		return false;
	}
	bus_.onHostValue(n, static_cast<float>(device::fromNormal(*desc, normal)));
	return true;
}

hostslots::SlotIndex SlotOwner::boundTo(DeviceInstanceId device, size_t control) const noexcept {
	// THE RECORD'S TABLE, NOT THE POOL: it is what the engine applies, so it is what a write must tell. A
	// mapping the pool made since the last visit is not bound yet, and the engine does not drive it either.
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	for (size_t n = 0; n < bindings.size(); ++n) {
		if (bindings[n].live && bindings[n].device == device && bindings[n].control == control) {
			return static_cast<hostslots::SlotIndex>(n);
		}
	}
	return hostslots::kNoSlot;
}

float SlotOwner::recordValueOf(DeviceInstanceId device, size_t control) const noexcept {
	const device::Descriptor* desc = record_.holds(device) ? record_.descriptorOf(device) : nullptr;
	if (desc == nullptr || control >= desc->params.size()) {
		return 0.0f;
	}
	const DeviceState& row = record_.rowOf(device);
	return control < row.fieldsPresent ? row.field[control] : static_cast<float>(desc->params[control].def);
}

bool SlotOwner::handOn(DeviceInstanceId device, size_t control) const noexcept {
	for (const OpenGesture& g : handGestures_) {
		if (g.open && g.device == device && g.control == control) {
			return true;
		}
	}
	return false;
}

void SlotOwner::openSlotGesture(hostslots::SlotIndex n, DeviceInstanceId device, size_t control) noexcept {
	if (n >= gestureOpen_.size()) {
		return;
	}
	// THIS CONTROL'S GESTURE ON ANY OTHER SLOT IS OVER (a remap moved it), and whatever is still open on THIS
	// slot — a control destroyed mid-drag, or another control the slot named before — is closed before the new
	// Begin, so the host never holds two gestures on one param or one gesture across two controls.
	for (size_t m = 0; m < gestureOpen_.size(); ++m) {
		OpenGesture& g = gestureOpen_[m];
		if (g.open && (m == n || (g.device == device && g.control == control))) {
			g.open = false;
			(void)outbox_.end(m);
		}
	}
	closeNestedGestureOn(n); // a control inside a nested rack that held this slot (T11)
	gestureOpen_[n] = { device, control, true };
	(void)outbox_.begin(n);
	// A HAND ON THE KNOB WHILE THE HOST RECORDS: whatever the host is writing into a lane, it is writing it
	// on this slot, so the slot must never be recycled (design §5). Folded into an anchor at the next visit.
	if (hostRecording()) {
		bus_.onRecTouched(n);
	}
}

void SlotOwner::beginGesture(DeviceInstanceId device, size_t control) noexcept {
	// THE HAND IS NOTED WHETHER OR NOT THE CONTROL HAS A SLOT: one bound mid-drag opens its Begin at its first
	// Value (`onHandWrite`). A full table forgets the hand, which costs only that late Begin.
	if (!handOn(device, control)) {
		for (OpenGesture& g : handGestures_) {
			if (!g.open) {
				g = { device, control, true };
				break;
			}
		}
	}
	const hostslots::SlotIndex n = boundTo(device, control);
	if (n == hostslots::kNoSlot) {
		// NOT BOUND (YET): nothing to tell the host now. A slot the recent ring gave it just before this
		// (`touch`) is bound at the next visit, which opens this hand's gesture there (`reportTakeover`).
		return;
	}
	openSlotGesture(n, device, control);
}

bool SlotOwner::touch(DeviceInstanceId device, size_t control) noexcept {
	try {
		const std::optional<hostslots::Target> target = targetOf(device, control);
		return target.has_value() && touchTarget(*target); // else nothing the pool could name
	}
	catch (...) {
		return false; // out of memory on the main thread: the control stays unslotted, as before T9
	}
}

bool SlotOwner::touchTarget(const hostslots::Target& target) {
	const hostslots::Allocation took = pool_.touch(target);
	if (took.slot == hostslots::kNoSlot || took.took == hostslots::Allocation::Took::Existing) {
		// EXISTING: a recency bump, and the Begin that follows goes out on the bound slot as it always did.
		// NONE: every slot Pinned or Anchored — nothing given, and NOTHING SAID: a hand turning a knob in a
		// pool the player has filled on purpose is not news, and a tray line on every such drag would be.
		return false;
	}
	// A slot this record binds reports its takeover at the rebind (`reportTakeover`). A NESTED target's binding
	// lives in its level's record, so there the flag is simply cleared at the rebind, and the level's hand opens
	// its Begin at its first Value on the slot (T6b's late Begin).
	if (took.slot < touched_.size()) {
		touched_[took.slot] = true;
	}
	// THE LAST RESORT IS SAID (design §2): a used Recent slot now names this control, and a lane the host
	// kept on it drives something else; a HOST-MARKED slot the same, in its own words. An Empty or an
	// untouched Default that the host never touched is taken silently.
	if (std::string said = takenNotice(took, target); !said.empty()) {
		notice_ = std::move(said);
	}
	return true;
}

std::string SlotOwner::takeNotice() {
	for (const OpenGesture& g : handGestures_) {
		if (g.open) {
			return {}; // a hand is down: its knob's readout would write over this a frame later
		}
	}
	return std::exchange(notice_, std::string {});
}

void SlotOwner::reportTakeover(size_t n, const SlotBinding& binding) noexcept {
	const device::Descriptor* desc = record_.descriptorOf(binding.device);
	if (desc == nullptr || binding.control >= desc->params.size() || n >= gestureOpen_.size()) {
		return;
	}
	const auto normal = static_cast<float>(
	    device::toNormal(desc->params[binding.control], recordValueOf(binding.device, binding.control)));
	// 1. THE TAKEOVER, OUTSIDE ANY GESTURE (design §4). `republishBindings` has already closed a gesture still
	// open on this slot for the control it named before, so this Value is filed under nothing.
	(void)outbox_.value(n, normal);
	// 2. THE HAND'S GESTURE, from where the hand has the control now: Begin (anchoring if the host records),
	// and the value again inside it, so a host recording has the drag's first point.
	openSlotGesture(static_cast<hostslots::SlotIndex>(n), binding.device, binding.control);
	(void)outbox_.value(n, normal);
	// 3. A HAND ALREADY GONE: its edit was one gesture, closed here. One still down closes it at `endGesture`.
	if (!handOn(binding.device, binding.control)) {
		gestureOpen_[n].open = false;
		(void)outbox_.end(n);
	}
}

void SlotOwner::endGesture(DeviceInstanceId device, size_t control) noexcept {
	for (OpenGesture& g : handGestures_) {
		if (g.open && g.device == device && g.control == control) {
			g.open = false;
		}
	}
	// THE SLOT THE BEGIN WENT OUT ON, not the one bound now: a binding rebuilt mid-drag must not leave the
	// host holding a gesture nobody closes.
	for (size_t n = 0; n < gestureOpen_.size(); ++n) {
		OpenGesture& g = gestureOpen_[n];
		if (g.open && g.device == device && g.control == control) {
			g.open = false;
			(void)outbox_.end(n);
		}
	}
}

void SlotOwner::onHandWrite(DeviceInstanceId device, size_t control, float coerced, bool recordHadIt) noexcept {
	if (!recordHadIt) {
		retextIfWordsFollow(device, control);
	}
	const hostslots::SlotIndex n = boundTo(device, control);
	if (n == hostslots::kNoSlot || n >= gestureOpen_.size()) {
		return;
	}
	const device::Descriptor* desc = record_.descriptorOf(device);
	if (desc == nullptr || control >= desc->params.size()) {
		return;
	}
	// **A WRITE OF THE VALUE ALREADY THERE IS NOT AN EDIT** (T6b review): with a bus the host has not moved,
	// nothing to do. **AND WITH ONE THE HOST HAS MOVED, IT IS AN EDIT ONLY WHEN A HAND IS ON THE CONTROL**
	// (integration, hs-int): a knob dragged back to the record's value while the host's newer one is
	// unmirrored is a real edit and wins; but a ROW WRITER carrying this control at the record's value — the
	// rack's settings roads write field by field through here (`ControlGestureScope`), where Feathers' whole-row
	// road has `seedChangedBound` — would otherwise put the record's OLD value back over the host's, the very
	// loss `seedChangedBound` exists to prevent. `ControlGestureScope` opens no gesture on a field it does not
	// change, so "no hand" is exactly "carried, not written".
	if (recordHadIt && (bus_.load(n).gen == committedGen_[n] || !handOn(device, control))) {
		return;
	}
	committedGen_[n] = bus_.seed(n, coerced);
	// **NO VALUE OUTSIDE ITS GESTURE AFTER A REMAP** (T6b review): a hand that took this control before it had
	// this slot (bound or remapped mid-drag) opens a Begin here first; and a gesture still open on the slot
	// for ANOTHER control is closed, so this Value is not filed under it.
	OpenGesture& g = gestureOpen_[n];
	const bool mine = g.open && g.device == device && g.control == control;
	if (!mine && handOn(device, control)) {
		openSlotGesture(n, device, control);
	}
	else if (!mine && g.open) {
		g.open = false;
		(void)outbox_.end(n);
	}
	if (!mine) {
		closeNestedGestureOn(n); // nor under a nested control's (T11)
	}
	(void)outbox_.value(n, static_cast<float>(device::toNormal(desc->params[control], coerced)));
}

SlotOwner::BoundBefore SlotOwner::captureBound(DeviceInstanceId device) const noexcept {
	BoundBefore before;
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	for (size_t n = 0; n < bindings.size() && n < before.had.size(); ++n) {
		const SlotBinding b = bindings[n];
		if (b.live && (device == kNoInstance || b.device == device) && record_.holds(b.device)) {
			before.value[n] = recordValueOf(b.device, b.control);
			before.had[n] = true;
		}
	}
	return before;
}

void SlotOwner::seedChangedBound(const BoundBefore& before) noexcept {
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	for (size_t n = 0; n < bindings.size() && n < before.had.size(); ++n) {
		const SlotBinding b = bindings[n];
		if (!before.had[n] || !b.live || !record_.holds(b.device)) {
			continue;
		}
		// ONLY A CONTROL THIS WRITE CHANGED. A row rewritten whole carries every other bound control at the
		// value the record already had — which, while automation plays, may be OLDER than the bus: the host
		// wrote since the mirror last copied. Seeding those put the record's old value back over the host's.
		const float after = recordValueOf(b.device, b.control);
		if (after == before.value[n]) {
			continue;
		}
		const Bus::Snapshot seen = bus_.load(n);
		if (seen.gen != committedGen_[n]) {
			// THE HOST HAS WRITTEN THIS SLOT SINCE THE RECORD LAST HEARD: its value is newer than this road's,
			// and the mirror puts it in the record at the next visit.
			continue;
		}
		// ONE CAS FROM WHAT WAS OBSERVED: a host write landing between the load and here makes it fail, and
		// the host's value stands.
		if (const uint32_t gen = bus_.seedIf(n, seen, after); gen != 0) {
			committedGen_[n] = gen;
		}
	}
}

bool SlotOwner::republishBindings() noexcept {
	bool changed = false;
	bool nestedMoved = false;
	descsPending_ = true;
	for (size_t n = 0; n < kSlotBindingCapacity; ++n) {
		SlotBinding want {};
		NestedBinding wantNested {};
		const device::ParamDesc* wantDesc = nullptr;
		if (n < pool_.capacity() && pool_.state(static_cast<hostslots::SlotIndex>(n)) != hostslots::SlotState::Empty
		    && pool_.target(static_cast<hostslots::SlotIndex>(n)).path.size() > 1) {
			// **A CONTROL INSIDE A NESTED LEVEL (T11)**: bound in THAT level's record, which pulls it
			// (`HostSlotLink::bindingsUnder`); here, only which level and device, and the declaration the audio
			// thread will scale by. Only a RESOLVED target binds — a pending one waits for its level's report.
			const hostslots::Target& target = pool_.target(static_cast<hostslots::SlotIndex>(n));
			const device::Descriptor* leaf = nullptr;
			if (reachNested(target, &leaf) == Reach::Resolved && leaf != nullptr) {
				for (const device::ParamDesc& p : leaf->params) {
					if (p.key != nullptr && device::sameKey(p.key, target.key.c_str())) {
						device::SlotPath prefix;
						for (size_t i = 0; i + 1 < target.path.size() && i < device::SlotPath::kCapacity; ++i) {
							prefix.ids[prefix.depth++] = target.path[i];
						}
						wantNested = NestedBinding { .prefix = prefix, .device = target.path.back(), .live = true };
						wantDesc = &p;
						break;
					}
				}
			}
		}
		else if (n < pool_.capacity() && pool_.state(static_cast<hostslots::SlotIndex>(n)) != hostslots::SlotState::Empty) {
			const hostslots::Target& target = pool_.target(static_cast<hostslots::SlotIndex>(n));
			const device::ParamDesc* pd = nullptr;
			bool resolved = false;
			try {
				resolved = resolve(target, &pd, nullptr);
			}
			catch (...) {
				resolved = false;
			}
			const device::Descriptor* d = resolved ? record_.descriptorOf(target.path[0]) : nullptr;
			if (d != nullptr && pd != nullptr && pd >= d->params.data() && pd < d->params.data() + d->params.size()) {
				want = SlotBinding { .device = target.path[0],
					                 .control = static_cast<uint16_t>(pd - d->params.data()),
					                 .slot = static_cast<uint16_t>(n),
					                 .live = true };
				wantDesc = pd;
			}
		}
		if (record_.state().slotBindings()[n] != want) {
			// **A BINDING NEW TO THE RECORD IS SEEDED FROM THE RECORD FIRST.** This is the load's road (a load
			// clears every binding, `ChainRecord::clear`), an undone removal's, and a fresh mapping's: the bus
			// may hold whatever the slot last carried, and the engine would put it over the control the moment
			// the binding lands. Before the record changes, so no drain sees the binding without the seed.
			//
			// **AND THE AUDIO THREAD'S DECLARATION FOR THE SLOT IS NULLED FIRST** (T6b review): until the new
			// binding is published, a host write must not be denormalised by the new target's range and applied
			// by the engine to the old one (or the reverse). A write in that window is dropped; `service`
			// publishes the new pointer after the owner's publish.
			desc_[n].store(nullptr, std::memory_order_release);
			if (want.live) {
				committedGen_[n] = bus_.seed(n, recordValueOf(want.device, want.control));
			}
			// A GESTURE STILL OPEN ON THE SLOT FOR THE CONTROL IT NAMED BEFORE is closed: its later Values go
			// nowhere, and the new control's hand opens its own Begin (`onHandWrite`).
			if (OpenGesture& g = gestureOpen_[n];
			    g.open && (!want.live || g.device != want.device || g.control != want.control)) {
				g.open = false;
				(void)outbox_.end(n);
			}
			(void)record_.setSlotBinding(n, want);
			changed = true;
		}
		// **THE NESTED HALF (T11).** A slot that moved into, out of, or between nested levels keeps NO declaration
		// on the audio thread until every level has adopted the move (`levelAdopted`): the level that loses it
		// may still be applying it, and the level that gains it has not seeded its bus from its record yet. A
		// gesture open on it for another control is closed, as above.
		if (n < nestedBindings_.size() && nestedBindings_[n] != wantNested) {
			desc_[n].store(nullptr, std::memory_order_release);
			const NestedGesture& g = nestedGestures_[n];
			if (g.open
			    && (!wantNested.live || !(g.path == wantNested.prefix.then(wantNested.device)))) {
				closeNestedGestureOn(n);
			}
			nestedBindings_[n] = wantNested;
			held_[n] = true;
			nestedMoved = true;
		}
		pendingDescs_[n] = wantDesc;
		// A SLOT A HAND'S TOUCH TOOK (T9) is reported now that it is bound: the takeover, and the hand's
		// gesture on it (`touch`'s list). A touched slot that did not bind (its device went) reports nothing.
		if (touched_[n]) {
			touched_[n] = false;
			if (want.live) {
				reportTakeover(n, want);
			}
			// A NESTED ONE IS OWED until its level adopts the binding and seeds the bus (backlog NG).
			nestedTakeoverOwed_[n] = wantNested.live;
		}
	}
	if (nestedMoved) {
		heldAt_ = ++nestedBindingGen_;
	}
	return changed;
}

void SlotOwner::publishPendingDescs() noexcept {
	if (!descsPending_) {
		return;
	}
	for (size_t n = 0; n < pendingDescs_.size(); ++n) {
		if (n < held_.size() && held_[n]) {
			heldDesc_[n] = pendingDescs_[n]; // the remap window across levels (T11): after the levels adopt
			continue;
		}
		desc_[n].store(pendingDescs_[n], std::memory_order_release);
	}
	descsPending_ = false;
	releaseHeldIfAdopted(); // no level left to wait for (the one that held it was removed)
}

SlotOwner::Visit SlotOwner::serviceRecord() noexcept {
	Visit visit;
	// 1. ANCHORS — the host wrote, or a hand moved a slotted control while the host recorded.
	// A HOST WRITE OR A NON-ZERO HOST MODULATION ALSO MARKS THE ID (2026-09-26): the host's lanes stay on it
	// after any release, so the pool reuses it last. A recording touch is the hand's, not the host's: no mark.
	const auto anchorAll = [&](const Bus::Bits& bits, bool host) {
		bits.forEach([&](size_t n) {
			if (n >= pool_.capacity()) {
				return;
			}
			if (host) {
				(void)pool_.markHost(static_cast<hostslots::SlotIndex>(n));
			}
			if (pool_.anchor(static_cast<hostslots::SlotIndex>(n))) {
				++visit.anchored;
			}
		});
	};
	const uint64_t poolBefore = pool_.generation();
	anchorAll(bus_.takeHostWrote(), true);
	anchorAll(bus_.takeRecTouched(), false);
	// A HOST EVENT ON AN ID THAT DROVE NOTHING: marked, not anchored (there is no mapping to protect).
	bus_.takeHostMarked().forEach([&](size_t n) {
		if (n < pool_.capacity()) {
			(void)pool_.markHost(static_cast<hostslots::SlotIndex>(n));
		}
	});
	// `anchor` and `markHost` bump the generation only on a transition, so this is exactly "a slot was newly
	// anchored or newly marked" — both saved with the pool.
	visit.poolChanged = pool_.generation() != poolBefore;

	// 2. THE BINDINGS, when the structure or the pool moved — not on every field write (see
	// `reconcileIfMoved`).
	if (!(boundStructure_ == slotStructure()) || pool_.generation() != boundGeneration_
	    || levelsRevision_ != boundLevelsRevision_) {
		visit.recordChanged = republishBindings();
	}

	// 3. THE RECORD MIRROR.
	const std::span<const SlotBinding> bindings = record_.state().slotBindings();
	for (size_t n = 0; n < bindings.size(); ++n) {
		const SlotBinding b = bindings[n];
		const device::Descriptor* d = b.live ? record_.descriptorOf(b.device) : nullptr;
		if (d == nullptr || b.control >= d->params.size()) {
			continue;
		}
		// ONE SNAPSHOT — the value and the generation that wrote it, never two reads that could pair them wrong.
		const Bus::Snapshot seen = bus_.load(n);
		if (seen.gen == committedGen_[n]) {
			// **THE BACKSTOP (T6b review: may it come back? yes, as a CAS).** The host has not moved the slot,
			// so the record should hold the bus's value; if it does not, a road wrote the record and forgot the
			// bus. Seeded by ONE CAS against the very snapshot compared, so a host write landing after `load`
			// makes it fail and the host's value stands — which is what the first version, two loads and a plain
			// seed, could not promise (it lost such a write with no hand anywhere near it, and was taken out).
			const float record = recordValueOf(b.device, b.control);
			if (record != device::coerceControl(d->params[b.control], seen.value)) {
				if (const uint32_t gen = bus_.seedIf(n, seen, record); gen != 0) {
					committedGen_[n] = gen;
				}
			}
			continue;
		}
		// THE HOST MOVED IT: the knob, the save and the undo base follow. The coerced number, which is the
		// one the engine applied.
		const float busValue = device::coerceControl(d->params[b.control], seen.value);
		try {
			visit.recordChanged |= record_.setField(b.device, b.control, busValue);
		}
		catch (...) {
			continue; // tried again at the next visit: the generation is not committed
		}
		committedGen_[n] = seen.gen;
	}
	boundStructure_ = slotStructure();
	boundGeneration_ = pool_.generation();
	boundLevelsRevision_ = levelsRevision_;
	return visit;
}

//-------------------------------------------------------------------------------------------------//
// NESTED LEVELS (T11)                                                                             //
//-------------------------------------------------------------------------------------------------//

namespace {

SlotOwner& ownerAt(void* host) { return *static_cast<SlotOwner*>(host); }
const SlotOwner& ownerAt(const void* host) { return *static_cast<const SlotOwner*>(host); }

} // namespace

device::HostSlotLink SlotOwner::linkFor(DeviceInstanceId self) noexcept {
	device::HostSlotLink link;
	link.host = this;
	link.prefix = device::SlotPath::of(self);
	link.bus = &bus_;
	// EVERY TRAMPOLINE SWALLOWS WHAT THE MESSAGE THREAD MAY THROW (out of memory building a name or a report):
	// a nested knob's right-click or drag must not unwind through a C function pointer.
	link.report = [](void* host, const device::SlotPath& prefix, uint8_t lane,
	                 std::span<const device::HostSlotLevelDevice> devices) {
		try {
			ownerAt(host).reportLevel(prefix, lane, devices);
		}
		catch (...) {
		}
	};
	link.captions = [](void* host, const device::SlotPath& path, std::span<const device::HostSlotCaption> captions) {
		try {
			(void)ownerAt(host).reportCaptions(path, captions);
		}
		catch (...) {
		}
	};
	link.retext = [](void* host, const device::SlotPath& path) {
		try {
			(void)ownerAt(host).retextUnder(path);
		}
		catch (...) {
		}
	};
	link.status = [](const void* host, const device::SlotPath& path, size_t control) {
		try {
			return ownerAt(host).nestedStatusOf(path, control);
		}
		catch (...) {
			return device::HostSlotStatus {};
		}
	};
	// A VERB THAT CHANGED THE POOL MARKS THE OWNER'S PATCH, which is where the pool is saved (design §7).
	link.expose = [](void* host, const device::SlotPath& path, size_t control, std::string* displaced) {
		SlotOwner& owner = ownerAt(host);
		bool changed = false;
		try {
			changed = owner.nestedExpose(path, control, displaced);
		}
		catch (...) {
		}
		if (changed) {
			owner.markOwnerDirty();
		}
		return changed;
	};
	link.unpin = [](void* host, const device::SlotPath& path, size_t control) {
		SlotOwner& owner = ownerAt(host);
		bool changed = false;
		try {
			changed = owner.nestedUnpin(path, control);
		}
		catch (...) {
		}
		if (changed) {
			owner.markOwnerDirty();
		}
		return changed;
	};
	link.touch = [](void* host, const device::SlotPath& path, size_t control) {
		SlotOwner& owner = ownerAt(host);
		if (owner.nestedTouch(path, control)) {
			owner.markOwnerDirty();
		}
	};
	link.freeUnder = [](void* host, const device::SlotPath& path) {
		try {
			ownerAt(host).nestedFreeUnder(path);
		}
		catch (...) {
		}
	};
	link.restoreUnder = [](void* host, const device::SlotPath& path) {
		try {
			(void)ownerAt(host).restoreUnderPath(path.span());
		}
		catch (...) {
		}
	};
	link.rebase = [](void* host, const device::SlotPath& from, const device::SlotPath& to) {
		try {
			return ownerAt(host).rebasePath(from.span(), to.span());
		}
		catch (...) {
			return false;
		}
	};
	link.beginGesture = [](void* host, const device::SlotPath& path, size_t control, uint32_t slot) {
		ownerAt(host).nestedBeginGesture(path, control, slot);
	};
	link.endGesture = [](void* host, const device::SlotPath& path, size_t control) {
		ownerAt(host).nestedEndGesture(path, control);
	};
	link.handValue = [](void* host, const device::SlotPath& path, size_t control, uint32_t slot, float value01,
	                    bool hand) { ownerAt(host).nestedHandValue(path, control, slot, value01, hand); };
	link.generation = [](const void* host) { return ownerAt(host).pool().generation(); };
	link.bindingGeneration = [](const void* host) { return ownerAt(host).nestedBindingGeneration(); };
	link.bindingsUnder = [](const void* host, const device::SlotPath& prefix, void* ctx,
	                        void (*fn)(void*, uint32_t, DeviceInstanceId, const char*, const char*)) {
		ownerAt(host).forEachBindingUnder(prefix, [&](hostslots::SlotIndex n, DeviceInstanceId device, const char* tag,
		                                              const char* key) { fn(ctx, n, device, tag, key); });
	};
	link.adopted = [](void* host, const device::SlotPath& prefix, uint8_t lane, uint64_t generation) {
		ownerAt(host).levelAdopted(prefix, lane, generation);
	};
	return link;
}

void SlotOwner::reportLevel(const device::SlotPath& prefix, uint8_t lane,
                            std::span<const device::HostSlotLevelDevice> devices) {
	if (prefix.empty()) {
		return;
	}
	const std::span<const DeviceInstanceId> p = prefix.span();
	for (NestedLevel& level : levels_) {
		if (level.lane == lane && std::equal(level.prefix.begin(), level.prefix.end(), p.begin(), p.end())) {
			if (!std::equal(level.devices.begin(), level.devices.end(), devices.begin(), devices.end())) {
				level.devices.assign(devices.begin(), devices.end());
				// A LEVEL WHOSE DEVICES CHANGED HAS ADOPTED NOTHING about them yet: what it binds is pulled again.
				level.adopted = 0;
				++levelsRevision_;
			}
			return;
		}
	}
	levels_.push_back(NestedLevel { .prefix = { p.begin(), p.end() },
	                                .lane = lane,
	                                .devices = { devices.begin(), devices.end() },
	                                .adopted = 0 });
	++levelsRevision_;
}

bool SlotOwner::prefixReaches(std::span<const DeviceInstanceId> prefix) const {
	if (prefix.empty() || !record_.holds(prefix[0])) {
		return false;
	}
	const device::Descriptor* d = record_.descriptorOf(prefix[0]);
	if (d == nullptr || d->container == nullptr) {
		return false;
	}
	for (size_t j = 1; j < prefix.size(); ++j) {
		bool reported = false;
		const device::Descriptor* found = nullptr;
		forEachLevelAt(prefix.first(j), [&](const NestedLevel& level) {
			reported = true;
			for (const device::HostSlotLevelDevice& dev : level.devices) {
				if (dev.id == prefix[j]) {
					found = dev.desc;
				}
			}
		});
		if (!reported) {
			return true; // PENDING: nothing below is known yet, and nothing below is denied
		}
		if (found == nullptr || found->container == nullptr) {
			return false;
		}
	}
	return true;
}

SlotOwner::Reach SlotOwner::reachNested(const hostslots::Target& target, const device::Descriptor** leaf) const {
	*leaf = nullptr;
	const std::vector<DeviceInstanceId>& path = target.path;
	if (path.size() < 2 || path.size() > device::SlotPath::kCapacity
	    || !prefixReaches(std::span<const DeviceInstanceId>(path.data(), 1))) {
		return Reach::Gone;
	}
	for (size_t j = 1; j < path.size(); ++j) {
		bool reported = false;
		const device::Descriptor* found = nullptr;
		forEachLevelAt(std::span<const DeviceInstanceId>(path.data(), j), [&](const NestedLevel& level) {
			reported = true;
			for (const device::HostSlotLevelDevice& dev : level.devices) {
				if (dev.id == path[j]) {
					found = dev.desc;
				}
			}
		});
		if (!reported) {
			// PENDING: the level has not reported. The leaf's declaration from the catalogue, by its kind, so the
			// host can still be told the slot's name and its value text.
			*leaf = target.tag.empty() ? nullptr : record_.descriptorFor(target.tag.c_str());
			return Reach::Pending;
		}
		if (found == nullptr) {
			return Reach::Gone;
		}
		if (j + 1 < path.size()) {
			if (found->container == nullptr) {
				return Reach::Gone;
			}
			continue;
		}
		// THE LEAF: of the target's kind (ids are patch-scoped, so another kind may stand under the id after a
		// load), declaring its key.
		if (!target.tag.empty() && !device::sameTag(found->traits.tag, target.tag.c_str())) {
			return Reach::Gone;
		}
		for (const device::ParamDesc& p : found->params) {
			if (p.key != nullptr && device::sameKey(p.key, target.key.c_str())) {
				*leaf = found;
				return Reach::Resolved;
			}
		}
		return Reach::Gone;
	}
	return Reach::Gone;
}

const device::Descriptor* SlotOwner::reportedDescriptorOf(const device::SlotPath& path) const {
	if (path.depth == 1) {
		return record_.holds(path.ids[0]) ? record_.descriptorOf(path.ids[0]) : nullptr;
	}
	if (path.depth < 2 || !prefixReaches(path.span().first(path.depth - 1))) {
		return nullptr;
	}
	const device::Descriptor* found = nullptr;
	forEachLevelAt(path.span().first(path.depth - 1), [&](const NestedLevel& level) {
		for (const device::HostSlotLevelDevice& dev : level.devices) {
			if (dev.id == path.ids[path.depth - 1]) {
				found = dev.desc;
			}
		}
	});
	return found;
}

std::string SlotOwner::levelDeviceName(std::span<const DeviceInstanceId> prefix, DeviceInstanceId device) const {
	// THE SAME RULE AS `ownDeviceName`, in the level's own chain: numbered by position only beside a twin.
	std::string name;
	forEachLevelAt(prefix, [&](const NestedLevel& level) {
		for (size_t i = 0; i < level.devices.size(); ++i) {
			const device::HostSlotLevelDevice& dev = level.devices[i];
			if (dev.id != device || dev.desc == nullptr || dev.desc->traits.name == nullptr) {
				continue;
			}
			name = dev.desc->traits.name;
			const bool twin = std::any_of(level.devices.begin(), level.devices.end(), [&](const auto& other) {
				return other.id != device && other.desc == dev.desc;
			});
			if (twin) {
				name += " ";
				name += std::to_string(i + 1);
			}
		}
	});
	return name;
}

std::string SlotOwner::nestedLabelOf(const device::SlotPath& path, size_t control) const {
	const device::Descriptor* leaf = path.depth >= 2 ? reportedDescriptorOf(path) : nullptr;
	if (leaf == nullptr || control >= leaf->params.size()) {
		return {};
	}
	// "Chain · Delay · Mix": every container on the way, named in ITS level, then the control as its card says.
	std::string label = ownDeviceName(path.ids[0]);
	for (size_t j = 1; j < path.depth; ++j) {
		label += " \xc2\xb7 ";
		label += levelDeviceName(path.span().first(j), path.ids[j]);
	}
	label += " \xc2\xb7 ";
	label += captionAt(path.span(), *leaf, control);
	return label;
}

std::optional<hostslots::Target> SlotOwner::nestedTargetOf(const device::SlotPath& path, size_t control) const {
	const device::Descriptor* leaf = path.depth >= 2 ? reportedDescriptorOf(path) : nullptr;
	if (leaf == nullptr || control >= leaf->params.size() || leaf->params[control].key == nullptr
	    || !leaf->params[control].hostSlot) { // `ParamDesc::hostSlot`: no nested target either
		return std::nullopt;
	}
	hostslots::Target target;
	target.path.assign(path.span().begin(), path.span().end());
	target.key = leaf->params[control].key;
	target.tag = leaf->traits.tag != nullptr ? leaf->traits.tag : "";
	target.label = nestedLabelOf(path, control);
	return target;
}

device::HostSlotStatus SlotOwner::nestedStatusOf(const device::SlotPath& path, size_t control) const {
	const device::Descriptor* leaf = path.depth >= 2 ? reportedDescriptorOf(path) : nullptr;
	if (leaf == nullptr || control >= leaf->params.size() || leaf->params[control].key == nullptr
	    || !leaf->params[control].hostSlot) {
		return {}; // a level that has not reported, or a control with no key: nothing the pool could name
	}
	return statusFor(path.span(), leaf->params[control].key);
}

bool SlotOwner::nestedExpose(const device::SlotPath& path, size_t control, std::string* displaced) {
	if (displaced != nullptr) {
		displaced->clear();
	}
	const std::optional<hostslots::Target> target = nestedTargetOf(path, control);
	if (!target.has_value()) {
		return false;
	}
	const uint64_t before = pool_.generation();
	const hostslots::Allocation took = pool_.pin(*target);
	if (took.slot == hostslots::kNoSlot) {
		return false;
	}
	if (displaced != nullptr && took.announce && took.previous.has_value()) {
		*displaced = took.previous->label;
	}
	return pool_.generation() != before;
}

bool SlotOwner::nestedUnpin(const device::SlotPath& path, size_t control) {
	const device::HostSlotStatus status = nestedStatusOf(path, control);
	return status.state == device::HostSlotStatus::State::Pinned
	       && pool_.unpin(static_cast<hostslots::SlotIndex>(status.slot));
}

bool SlotOwner::nestedTouch(const device::SlotPath& path, size_t control) noexcept {
	try {
		const std::optional<hostslots::Target> target = nestedTargetOf(path, control);
		return target.has_value() && touchTarget(*target);
	}
	catch (...) {
		return false;
	}
}

bool SlotOwner::restoreUnderPath(std::span<const DeviceInstanceId> path) {
	if (path.empty() || pool_.restoreUnder(path) == 0) {
		return false;
	}
	markOwnerDirty(); // the pool is saved with the owner's patch
	return true;
}

bool SlotOwner::rebasePath(std::span<const DeviceInstanceId> from, std::span<const DeviceInstanceId> to) {
	const std::optional<uint32_t> moved =
	    pool_.rebase(from, to, [this](const hostslots::Target& t) { return labelForTarget(t); });
	if (!moved.has_value()) {
		return false;
	}
	if (*moved > 0) {
		markOwnerDirty();
	}
	return true;
}

void SlotOwner::nestedFreeUnder(const device::SlotPath& path) {
	if (path.depth < 2) {
		return;
	}
	// FREED WITH THEIR NOTES, as a removal in this record frees (design §6); and every level under it forgotten,
	// so what they reported resolves nothing. The owner's patch changed: its pool is saved there.
	const std::span<const DeviceInstanceId> p = path.span();
	if (pool_.freeUnder(p) > 0) {
		markOwnerDirty();
	}
	const size_t before = levels_.size();
	std::erase_if(levels_, [&](const NestedLevel& level) {
		return level.prefix.size() >= p.size() && std::equal(p.begin(), p.end(), level.prefix.begin());
	});
	if (levels_.size() != before) {
		++levelsRevision_;
	}
}

void SlotOwner::closeNestedGestureOn(size_t n) noexcept {
	if (n < nestedGestures_.size() && nestedGestures_[n].open) {
		nestedGestures_[n].open = false;
		(void)outbox_.end(n);
	}
}

void SlotOwner::nestedBeginGesture(const device::SlotPath& path, size_t control, hostslots::SlotIndex n) noexcept {
	if (n >= nestedGestures_.size() || n >= gestureOpen_.size() || path.empty()) {
		return;
	}
	// `openSlotGesture`'S RULES, BY PATH: this control's gesture on any other slot is over, and whatever is open
	// on this slot — a nested control or one of this record's — is closed before the new Begin.
	for (size_t m = 0; m < nestedGestures_.size(); ++m) {
		NestedGesture& g = nestedGestures_[m];
		if (g.open && (m == n || (g.path == path && g.control == control))) {
			g.open = false;
			(void)outbox_.end(m);
		}
	}
	if (gestureOpen_[n].open) {
		gestureOpen_[n].open = false;
		(void)outbox_.end(n);
	}
	nestedGestures_[n] = NestedGesture { .path = path, .control = control, .open = true };
	(void)outbox_.begin(n);
	if (hostRecording()) {
		bus_.onRecTouched(n); // a hand inside a rack while the host records anchors the slot too (design §5)
	}
}

void SlotOwner::nestedEndGesture(const device::SlotPath& path, size_t control) noexcept {
	for (size_t m = 0; m < nestedGestures_.size(); ++m) {
		NestedGesture& g = nestedGestures_[m];
		if (g.open && g.path == path && g.control == control) {
			g.open = false;
			(void)outbox_.end(m);
		}
	}
}

void SlotOwner::nestedHandValue(const device::SlotPath& path, size_t control, hostslots::SlotIndex n, float value01,
                                bool hand) noexcept {
	if (n >= nestedGestures_.size() || n >= gestureOpen_.size() || path.empty()) {
		return;
	}
	// `onHandWrite`'S RULE: no Value outside its gesture. A hand that took the control before it had this slot
	// opens its Begin here; a gesture still open on the slot for anything else is closed.
	const NestedGesture& g = nestedGestures_[n];
	const bool mine = g.open && g.path == path && g.control == control;
	if (!mine && hand) {
		nestedBeginGesture(path, control, n);
	}
	else if (!mine) {
		closeNestedGestureOn(n);
		if (gestureOpen_[n].open) {
			gestureOpen_[n].open = false;
			(void)outbox_.end(n);
		}
	}
	(void)outbox_.value(n, value01);
}

void SlotOwner::levelAdopted(const device::SlotPath& prefix, uint8_t lane, uint64_t bindingGeneration) noexcept {
	const std::span<const DeviceInstanceId> p = prefix.span();
	for (NestedLevel& level : levels_) {
		if (level.lane == lane && std::equal(level.prefix.begin(), level.prefix.end(), p.begin(), p.end())) {
			level.adopted = std::max(level.adopted, bindingGeneration);
		}
	}
	releaseHeldIfAdopted();
}

void SlotOwner::releaseHeldIfAdopted() noexcept {
	// NOT WHILE A REBUILD'S DECLARATIONS WAIT FOR THE OWNER'S OWN PUBLISH (`descsPending_`): `heldDesc_` is
	// written at that publish, and releasing before it would hand over the previous hold's.
	if (descsPending_ || std::none_of(held_.begin(), held_.end(), [](bool h) { return h; })) {
		return;
	}
	for (const NestedLevel& level : levels_) {
		if (level.adopted < heldAt_) {
			return;
		}
	}
	for (size_t n = 0; n < held_.size() && n < desc_.size(); ++n) {
		if (held_[n]) {
			held_[n] = false;
			desc_[n].store(heldDesc_[n], std::memory_order_release);
			// EVERY LEVEL HAS ADOPTED, so the bus holds the nested control's value: the takeover can say it (NG).
			if (std::exchange(nestedTakeoverOwed_[n], false)) {
				reportNestedTakeover(n);
			}
		}
	}
}

void SlotOwner::reportNestedTakeover(size_t n) noexcept {
	if (n >= nestedBindings_.size() || n >= nestedGestures_.size() || n >= gestureOpen_.size() || !nestedBindings_[n].live) {
		return;
	}
	const device::ParamDesc* desc = nullptr;
	double value = 0.0;
	try {
		if (!resolve(pool_.target(static_cast<hostslots::SlotIndex>(n)), &desc, &value) || desc == nullptr) {
			return;
		}
	}
	catch (...) {
		return;
	}
	const auto normal = static_cast<float>(device::toNormal(*desc, value));
	// `reportTakeover`'s steps, by slot: the takeover outside any gesture, then the edit as one gesture. A hand
	// whose gesture is already open here (it moved again after its level pulled the binding) is telling the host
	// itself, and a Begin from here would close nothing and open a second.
	(void)outbox_.value(n, normal);
	if (nestedGestures_[n].open || gestureOpen_[n].open) {
		return;
	}
	(void)outbox_.begin(n);
	(void)outbox_.value(n, normal);
	(void)outbox_.end(n);
}

} // namespace feathers::rack
