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

#include "clap/stream.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/mseg/mseg_types.h"
#include "plugin/device_instance.h"
#include "plugin/device_row.h"
#include "plugin/params.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// WHAT IS IN THE MODULATOR RACK AND WHAT EACH OF THEM IS SET TO.
//
// THE SHAPE IS fx_chain.h's — a count, then one record per device carrying its tag, the length of
// its private payload, and the payload — and that file's three properties are the reason:
//
//   - A SECOND DEVICE TYPE IS AN APPEND INSIDE THE BLOCK. The MSEG is the next one, and it will be a
//     new tag in this same record rather than a new block or a format change.
//   - A DEVICE THIS BUILD DOES NOT KNOW IS SKIPPED. Its length says how far to jump, so a patch
//     saved by a build with an MSEG opens in one without it, carrying the envelopes and dropping the
//     rest.
//   - A DEVICE'S PAYLOAD CAN GROW. A shape knob appended to the DAHDSR later is skipped by readers
//     that predate it, exactly as an appended block is.
//
// AND ONE THING IT DOES DIFFERENTLY, which is the whole reason this file exists rather than another
// entry in the fx chain. THE FILTER'S PAYLOAD IS EMPTY: everything about the filter is a parameter,
// so what its record carries is only that the filter is in the lane at all. A modulator's controls
// are NOT parameters — the decision of 2026-08-22, and see dahdsr_types.h::Settings for the reason
// the browser makes it necessary — so the payload here is where the envelope's settings actually
// live. The extension point fx_chain.h wrote and never used is load-bearing from the first device.

namespace feathers {

/// WHERE EVERY RACK MODULATOR STANDS, one reading per slot.
///
/// **IT LIVES HERE RATHER THAN ON `Synth` BECAUSE BOTH SIDES OF THE THREAD BOUNDARY NAME IT** — the
/// engine fills one and the bridge transports it, and putting the type on the producer would drag
/// `synth.h` into `gui_bridge.h`, which is a dependency the bridge has carefully never had.
///
/// **ONE PER STRIP POSITION, AS MANY AS THE STRIP HOLDS** (unbounded devices step 3): a vector the engine
/// sizes to its strip and grows with it at the drain, never on the audio thread. A reader takes `size()`.
using RackPlayheads = std::vector<mod::Playhead>;

/// **HOW HARD EACH FX SLOT IS BEING DRIVEN, 0..1, or negative where the device publishes no meter.**
/// Travels beside `RackPlayheads` on the same publish because it is the same kind of fact: a READING
/// the engine produces per block, not state a patch stores.
/// One per lane position, sized and grown like `RackPlayheads`.
using FxDrivePeaks = std::vector<float>;

/// **THE ALL-SENTINEL ARRAY — "nobody publishes a meter here".** The default for callers that have no
/// engine to ask, which is every test that publishes a rack to check something else. It is a named
/// value rather than a zeroed default because zero is a REAL level: a caller that omitted meters and
/// got zeros would paint every transfer curve as driven by silence, which is a plausible picture of a
/// wrong thing.
[[nodiscard]] inline const FxDrivePeaks& noFxDrivePeaks() noexcept {
	static const FxDrivePeaks kNone(kMaxFxSlots, -1.0f);
	return kNone;
}

/// **NO FX PLAYHEADS** — the default for callers with no engine to ask: nothing published, and a position
/// nobody has published for reads idle (a zero packed playhead is `mod::Playhead{}`).
[[nodiscard]] inline const RackPlayheads& noFxPlayheads() noexcept {
	static const RackPlayheads kNone;
	return kNone;
}

/// The bytes a device tag is written as. Four, like every other tag in this stream.
inline constexpr size_t kModTagBytes = 4;

/// The most payload bytes one device's record may claim. A patch is a file on somebody's disk and a
/// length read out of it decides how far a reader walks; this is the ceiling on that walk.
inline constexpr size_t kMaxModPayloadBytes = 4096;

/// The DAHDSR's tag. Spelled once, here, so the writer and the reader cannot disagree about it.
inline constexpr char kDahdsrTag[kModTagBytes] = { 'D', 'H', 'D', 'S' };

/// HOW MANY ENVELOPES THE RACK CAN HOLD — the source pool params.h reserved, not a number of its
/// own. A rack that could hold more envelopes than the route table has rows for would be a device
/// nothing could be routed from.
/// HOW MANY DEVICES THE RACK CAN HOLD — the rack's own slot count, not a number of its own.
///
/// IT USED TO READ kEnvSourceCount, when a slot WAS a source. A slot now owns a device and a device
/// declares how many sources it publishes, so "how many slots" and "how many sources" are different
/// questions with different answers (4 and 16). See params.h::deviceSourceOf.
inline constexpr size_t kMaxRackEnvelopes = kMaxRackSlots;

/// HOW MANY NUMBERS ONE DAHDSR IS — THE DEVICE'S ANSWER, not ours.
///
/// A first draft of this file declared the field order here, and that was the boundary violation the
/// whole modulator module exists to refuse. The order of a device's own controls is the DEVICE'S
/// knowledge: it is index-aligned with the descriptors the device publishes, so a host that wrote it
/// down separately would be a host that has to be edited every time the device grows a knob — and
/// would be the second copy of exactly the knowledge that makes a generic card possible.
///
/// So these are aliases. The plugin's business is where the row is STORED (a tagged state block,
/// because a modulator's controls are deliberately not clap params) and never what is in it.
inline constexpr size_t kDahdsrFieldCount = static_cast<size_t>(mod::dahdsr::Param::kCount);
using DahdsrRow = mod::dahdsr::Row;

// A device's identity is DeviceInstanceId, in plugin/device_instance.h — both chains use it, so
// neither declares it. See that header for why a slot index cannot be one.

struct ModRack {
	/// EACH SLOT HOLDS A DEVICE, NOT AN ENVELOPE — the line this whole arc was aimed at.
	///
	/// It was `std::array<mod::dahdsr::Settings, N>`, which is the rack declaring the TYPE of its
	/// contents in its own struct definition. Every other pin fell before this one: params.h stopped
	/// sizing the destination space from the DAHDSR, the source axis stopped assuming a slot is a
	/// source, the registry learned what an MSEG is — and none of it let a slot actually hold one,
	/// because the array's element type still said otherwise.
	///
	/// A DeviceRow carries the tag, the instance, the count of fields present and the fields
	/// themselves, together (plugin/device_row.h). Together rather than beside each other, because
	/// two copies of an identity is one that goes stale.
	///
	/// **A VECTOR WHOSE SIZE IS THE RACK'S CAPACITY** (unbounded devices step 3) — `FxChain::device`'s
	/// note says why, and it is the same reason: the audio thread copies the rack whole every tick
	/// (`Synth::modulated_`), and a copy between two of the same size allocates nothing.
	std::vector<DeviceRow> device = std::vector<DeviceRow>(kMaxRackEnvelopes);

	/// THE NEXT SERIAL TO HAND OUT — carried in the patch, so an id is never reused across a save and
	/// reload. A counter that reset would make yesterday's routing point at today's device, which is
	/// the whole failure the id exists to prevent, merely delayed by a file boundary.
	DeviceInstanceId nextInstanceId = 1;

	/// HOW MANY OF THEM ARE ACTUALLY THERE. One in every patch that exists today, because there is no
	/// affordance for adding a second yet — the browser is the next arc. It is carried anyway, and
	/// that is deliberate: the slots are permanent (a source index is), so what the browser will add
	/// is a number here rather than a format. A field that costs four bytes now and saves a stream
	/// migration later is a field worth writing.
	size_t count = 1;

	/// **BACK TO EMPTY, IN PLACE** — see `FxChain::reset`. Zero devices and the first serial, the storage kept.
	void reset() noexcept {
		for (DeviceRow& r : device) {
			r = DeviceRow {};
		}
		nextInstanceId = 1;
		count = 0;
	}

	/// **EQUAL WHEN THEY HOLD THE SAME RACK**, whatever capacity each has grown to — see `FxChain`'s.
	friend bool operator==(const ModRack& a, const ModRack& b) noexcept {
		if (a.count != b.count || a.nextInstanceId != b.nextInstanceId) {
			return false;
		}
		for (size_t i = 0; i < a.count; ++i) {
			const bool inA = i < a.device.size();
			const bool inB = i < b.device.size();
			if (inA != inB || (inA && !(a.device[i] == b.device[i]))) {
				return false;
			}
		}
		return true;
	}

	//----------------------------------------------------------------------------------//
	// TYPED VIEWS OF A SLOT — how a caller that knows which device it wants asks for it.  //
	//----------------------------------------------------------------------------------//

	/// THE DAHDSR SETTINGS OF A SLOT THAT HOLDS ONE, and the declared defaults for a slot that does
	/// not.
	///
	/// THE DEFAULT ANSWER IS NOT A FALLBACK, IT IS THE HONEST ONE. A caller asking a slot for an
	/// envelope when the slot holds an MSEG has asked a question with no answer, and handing back
	/// somebody else's numbers reinterpreted as envelope fields would be far worse than handing back
	/// the shape a fresh envelope has. Callers that must distinguish ask `holds()` first.
	mod::dahdsr::Settings dahdsrAt(size_t slot) const;

	/// THE MSEG SETTINGS OF A SLOT THAT HOLDS ONE, on exactly the same terms.
	///
	/// Its existence beside dahdsrAt is the whole point of this pass: two typed views of one slot,
	/// neither privileged, and a slot that answers `holds()` for either. The rack used to be able to
	/// express only the first because its array said so.
	mod::mseg::Settings msegAt(size_t slot) const;
	void setMsegAt(size_t slot, const mod::mseg::Settings& settings);

	/// Write one slot as a DAHDSR, stamping the tag and the field count from the device's own
	/// declarations. The instance id already in the slot is preserved — a settings edit is not a new
	/// device.
	void setDahdsrAt(size_t slot, const mod::dahdsr::Settings& settings);

	/// Whether `slot` holds a device with this tag. The question a caller asks before assuming.
	bool holds(size_t slot, const char* tag) const;

	/// The instance in `slot`, or kNoInstance.
	DeviceInstanceId instanceAt(size_t slot) const;

	/// A SLOT'S IDENTITY — see `DeviceIdentity`, which carried two halves until kill-lanes step 3c and
	/// carries one now.
	///
	/// IT IS STILL A STRUCT AND STILL ONE SETTER, which was the point rather than an economy: it
	/// replaced `setInstanceAt`, which stamped a serial and left the lane unset — a rack slot holding a
	/// device with real settings, a real id, and no address the routing table could reach. Every caller
	/// was correct on the day it was written and every one would have been silently wrong the moment
	/// lanes existed. The second half is gone; the shape that made it impossible to forget stays.
	void setIdentityAt(size_t slot, DeviceIdentity identity);
};


//----------------------------------------------------------------------------------------------//
// THE RACK'S HALF OF THE ROUTING-LANE POOL IS GONE (kill-lanes step 3c).                          //
//----------------------------------------------------------------------------------------------//
//
// THERE WERE TWO POOLS AND THE SPLIT WAS NOT ARBITRARY: `isFxDeviceSlot` decided which chain a
// route's device was looked up in, and it decided it from the lane's NUMBER, so a rack device
// holding a lane below `kMaxFxSlots` would have been looked up in the fx chain, found nothing, and
// had its routes dropped. The ranges were pool boundaries rather than a layout.
//
// A route names an instance id now (`device/endpoint.h`) and an id says which device without saying
// which chain, so there is nothing to carve, nothing to allocate and nothing to collide. What the
// two ranges still describe is a COORDINATE space the surface and its routing gate speak
// (`deviceSlotOfRack`, `DeviceSlots`); kill-lanes steps 4 and 5 retire that too.

/// The rack a fresh instance has: one DAHDSR at its defaults.
///
/// AND THIS IS ALSO WHAT AN ABSENT BLOCK MEANS — re-argued 2026-08-22 once patch compatibility stopped
/// being a constraint (no saved presets, no other users). The first version of this note leaned on old files: an empty rack "would be
/// permanent for every patch written before today". There are no such patches, so that argument is
/// void and the choice has to stand on its own or be reversed.
///
/// IT STANDS, on a reason that has nothing to do with old state: there is no add-a-device affordance
/// anywhere in the editor. An empty rack is therefore a rack nothing can ever put a device into —
/// not a starting point but a dead end — and "absent" would be the one input that could reach it. A
/// fresh instance's rack is the only reading that leaves every patch in the same place, which is
/// what an absent optional block should mean anyway.
///
/// WHEN THE BROWSER LANDS this becomes a real choice rather than a forced one, and empty may well be
/// the better answer then. It is a line to revisit, not a line to keep out of habit.
ModRack defaultModRack();

/// WHICH SLOT HOLDS THE DEVICE WITH THIS ID, or `kNoPosition` when nothing does — which is the
/// answer for a routing whose device has been removed or replaced since the patch was written.
///
/// THE OUT-OF-RANGE ANSWER IS THE POINT rather than an edge case: a route that cannot be resolved is
/// DROPPED, not redirected. Bounded by `count` rather than by the array's end, so a stale id sitting
/// in a slot past the count cannot resurrect it.
size_t slotOfInstance(const ModRack& rack, DeviceInstanceId id);

/// The way back — what a slot's device is called, or kNoInstance for a slot holding nothing.
DeviceInstanceId instanceOfSlot(const ModRack& rack, size_t slot);

// (THE RACK SAVES AS THE RACK'S FILE, with the lane, since chain-as-a-device item 6 —
// `ParamBridge::chainsPatch`/`restoreChains` through `rack/rack_state.h`. `saveModRack`/`loadModRack`
// are gone.)


/// **ROOM FOR `rows` DEVICES, AT LEAST DOUBLING** (unbounded devices step 6) — `growFxChainFor`'s twin. MESSAGE
/// THREAD ONLY.
inline void growModRackFor(ModRack* rack, size_t rows) {
	if (rows > rack->device.size()) {
		rack->device.resize(std::max(rows, 2 * rack->device.size()));
	}
}

/// ADD A MODULATOR TO THE END OF THE RACK, returning the slot it took or `kNoPosition` on refusal.
///
/// The rack's half of addFxDevice, and the same rules for the same reasons — see that function. The
/// only difference is which capability is asked for: a modulator is a thing that PUBLISHES a signal,
/// so the test is maySitInModRack rather than maySitInAudioChain, and both are derived from the
/// device's declarations rather than from which lane it is being put in.
///
/// **AND THIS GATE IS WIDER THAN THE PICKER, ON PURPOSE — read this before concluding the rack is
/// broken.** "The rack accepts it, so why can I not add it?" is answered one layer up: the modulator
/// strip's menu offers `maySitInModRack(d) && isTaggedModulator(d)` (device_row.h), so a capable device
/// that does not tag itself `"modulator"` is seatable here and not offered there. That is the intended
/// split — capability is the device's ability, the tag is the device's claim — and it is why nothing in
/// this function asks about tags.
inline size_t addRackDevice(ModRack* rack, const char* tag) {
	if (rack == nullptr || tag == nullptr) {
		return kNoPosition;
	}
	const device::Descriptor* desc = deviceForTag(tag);
	if (desc == nullptr || !maySitInModRack(*desc)) {
		return kNoPosition;
	}
	const size_t slot = rack->count;
	growModRackFor(rack, slot + 1); // never full (unbounded devices step 6)
	rack->device[slot] = defaultRowFor(*desc, { rack->nextInstanceId });
	rack->nextInstanceId = rack->nextInstanceId + 1;
	rack->count = slot + 1;
	return slot;
}

/// The rack's half of `insertFxDeviceBefore` (backlog CR): a new device of `tag` immediately before
/// `anchor`, or at the end if the rack does not hold that device. Returns the POSITION it landed at,
/// or `kNoPosition` if refused.
///
/// **THE ANCHOR IS THE DEVICE, BY ITS INSTANCE ID** — see `insertFxDeviceBefore` for the whole of the
/// reasoning: the hand means "before THAT card", so the anchor has to be an identity, and an instance
/// id is the only thing here that no edit renumbers.
inline size_t insertRackDeviceBefore(ModRack* rack, DeviceInstanceId anchor, const char* tag) {
	if (rack == nullptr || tag == nullptr) {
		return kNoPosition;
	}
	const device::Descriptor* desc = deviceForTag(tag);
	if (desc == nullptr || !maySitInModRack(*desc)) {
		return kNoPosition;
	}
	growModRackFor(rack, rack->count + 1); // never full (step 6)
	size_t slot = rack->count;
	for (size_t at = 0; at < rack->count && at < rack->device.size(); ++at) {
		if (anchor != kNoInstance && rack->device[at].instanceId == anchor) {
			slot = at;
			break;
		}
	}
	for (size_t i = rack->count; i > slot; --i) {
		rack->device[i] = rack->device[i - 1];
	}
	rack->device[slot] = defaultRowFor(*desc, { rack->nextInstanceId });
	rack->nextInstanceId = rack->nextInstanceId + 1;
	rack->count += 1;
	return slot;
}


/// The rack's half of `removeFxDevice` — same rules, and see that function for why it no longer takes
/// a routing table: a removal's routes are the record's to drop, and there is no address to free.
inline bool removeRackDevice(ModRack* rack, size_t slot) {
	if (rack == nullptr || slot >= rack->count || slot >= rack->device.size()) {
		return false;
	}
	for (size_t i = slot + 1; i < rack->count && i < rack->device.size(); ++i) {
		rack->device[i - 1] = rack->device[i];
	}
	rack->device[rack->count - 1] = DeviceRow {};
	rack->count -= 1;
	return true;
}

/// The rack's half of moveFxDevice, and it takes no routing table for the same reason that one does
/// not: a lane travels in the row being moved.
///
/// REORDERING MODULATORS DOES NOT CHANGE A SOUND the way reordering effects does — a rack is a set of
/// sources rather than a signal path — but the LIST is what the player reads, and a rack whose
/// envelopes cannot be put in the order they think about them in is the same complaint as a chain
/// that cannot be reordered. It costs one function to answer both.
/// PUT A DEVICE BACK AT `slot`, WITH ITS ROUTINGS — the inverse of removeRackDevice, and the road an
/// UNDO of a rack remove calls. The fx lane's insertFxDevice, on the other lane; see it for why the
/// shift is a named permutation rather than an undo record's private business.
///
/// **IT CARRIED A ROUTE SLICE AND A TABLE AND NEEDS NEITHER (kill-lanes step 3c)**, for the reason
/// `insertFxDevice` gives. The paragraph that stood here is worth keeping as history, because it was
/// a real finding rather than foresight: a rack slot is a modulation SOURCE as well as a set of
/// destinations — a DAHDSR and an MSEG publish outputs, while the fx lane's devices declare
/// `.outputs = {}` — so a capture that held only destinations was complete on the fx lane and would
/// have silently lost half of a rack device's routings here. The record holds both directions as
/// edges on the device's id now, so an undo asks it for them and there is no capture to get wrong.
inline bool insertRackDevice(ModRack* rack, size_t slot, const DeviceRow& row) {
	if (rack == nullptr || slot > rack->count) {
		return false;
	}
	growModRackFor(rack, rack->count + 1); // never full (step 6)
	for (size_t i = rack->count; i > slot; --i) {
		rack->device[i] = rack->device[i - 1];
	}
	rack->device[slot] = row;
	rack->count += 1;
	// The serial is not reissued and nextInstanceId is not rewound — an undo puts back the device
	// that was there, id and all, and the counter only ever climbs.
	return true;
}

inline bool moveRackDevice(ModRack* rack, size_t from, size_t to) {
	if (rack == nullptr || from >= rack->count || to >= rack->count
	    || from >= rack->device.size() || to >= rack->device.size()) {
		return false;
	}
	if (from == to) {
		return true;
	}
	const DeviceRow moved = rack->device[from];
	if (from < to) {
		for (size_t i = from; i < to; ++i) {
			rack->device[i] = rack->device[i + 1];
		}
	}
	else {
		for (size_t i = from; i > to; --i) {
			rack->device[i] = rack->device[i - 1];
		}
	}
	rack->device[to] = moved;
	return true;
}

} // namespace feathers


