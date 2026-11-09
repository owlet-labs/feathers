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
#include "fx/filter/filter_types.h"
#include "fx/harm/harm_types.h"
#include "plugin/device_instance.h"
#include "plugin/device_row.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// WHAT IS IN THE FX LANE, AND IN WHAT ORDER — the one thing about the lane that its parameters
// cannot say.
//
// Every control a device has is a parameter, and that is where controls belong: a host can automate
// them, modulate them, show them and save them without this file existing. WHICH DEVICES ARE THERE
// is a different kind of fact. It is not a number on a knob, it has no range, and the second device
// will need it to say where it sits relative to the first.
//
// So the block is a CHAIN RECORD: a count, then one record per device carrying its tenant tag, the
// length of its private payload, and the payload itself. Three properties follow from that shape,
// and all three are pinned in tests/test_gui_bridge.cpp:
//
//   - A SECOND TENANT IS AN APPEND INSIDE THE BLOCK, not a new block and not a format change. The
//     count grows; the stream's block sequence does not move; every patch ever saved still loads.
//   - A DEVICE THIS BUILD DOES NOT KNOW IS SKIPPED, not fatal. Its length says how far to jump, so a
//     patch saved by a later build opens in an earlier one with the devices it recognises and
//     without the ones it does not.
//   - A DEVICE'S PAYLOAD CAN GROW. The length is the extension point, so a field appended inside a
//     payload is skipped by readers that predate it, exactly as an appended block is.
//
// The filter's payload is EMPTY today, and that is the design rather than an omission: everything
// about the filter is a parameter, and duplicating a setting here would be a second source of truth
// for it — the bug the routings block's own entry in feathers_clap.cpp warns about in as many words.
// What this block carries for the filter is that the filter is in the lane at all.

namespace feathers {

/// THE LANE'S STARTING SIZE, as `kMaxFxSlots` is (unbounded devices) — not a limit. The record is count-driven, so
/// a chain view starts this long and grows to what it holds.
inline constexpr size_t kMaxFxDevices = 8;

/// THE SAME BOUND HAS TWO NAMES IN TWO HEADERS, AND NOTHING TIED THEM UNTIL NOW.
///
/// `kMaxFxSlots` lives in params.h because the routing table is addressed by slot; `kMaxFxDevices`
/// lives here because it is the length of the array a patch is read into. They have been 8 and 8
/// since both existed, and `addFxDevice` guards against BOTH — which reads as belt and braces and is
/// really the only thing keeping them honest.
///
/// FOUND BY A KNOWN-BAD THAT REFUSED TO GO RED. Deleting the `slot >= kMaxFxSlots` half of that guard
/// changed nothing observable, because the surviving half is numerically identical — a mutation that
/// looked like a hole in a test and was actually two constants silently agreeing. Neither divergence
/// would corrupt memory (the array bound is checked either way), which is exactly what makes it the
/// bad kind: raise one and the lane quietly caps somewhere the chain does not, or the reverse, and
/// nothing says a word.
///
/// A REQUIRED AGREEMENT BEATS A REMEMBERED ONE — the playbook's rule about making forgetting
/// unrepresentable, at the cost of one line.
static_assert(kMaxFxDevices == kMaxFxSlots,
              "The fx lane's array length and its routing-slot bound are the same bound. If you are "
              "raising the number of fx devices, raise kMaxFxSlots in params.h with it — and check "
              "what the routing table does with the extra columns before you do.");

/// The bytes a tenant tag is written as. Four, like every other tag in this stream.
inline constexpr size_t kFxTagBytes = 4;

/// The most payload bytes one device's record may claim. A patch is a file on somebody's disk, and
/// a length read out of it decides how far a reader walks; this is the ceiling on that walk.
inline constexpr size_t kMaxFxPayloadBytes = 4096;

/// ONE DEVICE IN THE LANE — what kind it is, which one it is, and what it is set to.
///
/// IT CONTAINS A DeviceRow RATHER THAN GROWING FIELDS BESIDE ONE. `tag` and `instanceId` are the
/// first two members of DeviceRow already, so adding a settings payload alongside them would have
/// meant two copies of a device's identity in one struct — and the second copy is the one that goes
/// stale. Holding the row is what makes the de-duplication obvious rather than merely convenient.
///
/// AND IT IS THE SAME SHAPE THE RACK GETS, in the same pass. The lane and the rack carrying devices
/// two different ways would be the divergence device_row.h exists to prevent, arriving one level up
/// from the copy loops it already prevents.
struct FxDevice {
	/// tag, instanceId, fieldsPresent and the settings themselves. See plugin/device_row.h for the
	/// rule the row enforces: the width and the count of fields present travel WITH the payload.
	DeviceRow row {};

	/// Convenience readers, so the hundred existing call sites that ask a device its tag or its
	/// identity do not each have to know the row is where those now live.
	const device::Tag& tag() const noexcept { return row.tag; }
	DeviceInstanceId instanceId() const noexcept { return row.instanceId; }

	friend bool operator==(const FxDevice&, const FxDevice&) = default;
};

struct FxChain {
	/// **INDEXED BY POSITION, DENSE IN `[0, count)` (kill-lanes step 3c).** `device[p]` is the device
	/// drawn at `p` and the one the block walks `p`-th. There is no lane and no second array: a chain
	/// IS its order, so the order is the storage.
	///
	/// IT WAS LANE-INDEXED WITH AN `order[]` BESIDE IT, and the pair bought exactly one thing — a
	/// device's routing columns stayed put across a reorder, because its lane travelled in its row.
	/// Kill-lanes step 3b took the lane out of the published state, so a view's lane became a number
	/// minted from its own position (`feathers_chains.h`); a mapping that is the identity by
	/// construction is not an indirection, it is a place for the two to drift apart. **What the pair
	/// really bought is now bought by the instance id**, which is what a route names.
	///
	/// **A VECTOR WHOSE SIZE IS THE CHAIN'S CAPACITY, NOT ITS COUNT** (unbounded devices step 3). It was
	/// `std::array<FxDevice, kMaxFxDevices>`, and the audio thread copies a chain whole every block
	/// (`Synth::modulatedFx_`). A copy between two of the SAME size reuses the destination's storage and
	/// allocates nothing, so every copy the audio thread makes is between two chains grown together —
	/// grown on the message thread and swapped in at the drain (`Synth::requestChainCapacity`). Rows past
	/// `count` are cleared rows, as before. Starts at `kMaxFxDevices`, which is now a starting size.
	std::vector<FxDevice> device = std::vector<FxDevice>(kMaxFxDevices);
	/// How many devices are in the chain. `device[0..count)` are real; the rest are cleared by the
	/// verbs rather than left as they were — a stale row that still names a tag and a serial reads as
	/// an answer, which is a trap to leave lying about for the sake of one assignment.
	size_t count { 0 };

	/// THE NEXT SERIAL THIS LANE HANDS OUT — carried in the patch, so an id is never reused across a
	/// save and reload. A counter that reset would make yesterday's routing point at today's device,
	/// which is the whole failure the id exists to prevent, merely delayed by a file boundary.
	///
	/// THE LANE COUNTS SEPARATELY FROM THE RACK, which is safe only because a saved route carries a
	/// distinct KIND for an fx device and a rack device. See device_instance.h.
	DeviceInstanceId nextInstanceId{kFirstInstance};

	/// **BACK TO EMPTY, IN PLACE** — every row cleared, the count and the serial back to the start, and the
	/// storage KEPT. `*chain = FxChain {}` said the same thing and built a fresh vector to say it: an
	/// allocation and a free, which the audio thread (`chainsOfState`) may not do.
	void reset() noexcept {
		for (FxDevice& d : device) {
			d = FxDevice {};
		}
		count = 0;
		nextInstanceId = kFirstInstance;
	}

	/// **EQUAL WHEN THEY HOLD THE SAME CHAIN** — the same devices in the same order and the same next
	/// serial — whatever capacity each has grown to. The defaulted form compared capacities too, which
	/// would call a chain unequal to itself across a growth.
	friend bool operator==(const FxChain& a, const FxChain& b) noexcept {
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
};


//----------------------------------------------------------------------------------------------//
// THE ORDER'S VOCABULARY — a place in a chain, asked for BY NAME.                                //
//----------------------------------------------------------------------------------------------//
//
// **THE ROUTING-LANE POOL IS GONE (kill-lanes step 3c).** `fxLaneIsFree` and `freeFxLane` allocated
// an address out of a fixed pool, and everything they guarded — the ABA hazard of reissuing an
// address whose old amounts were still in the table, two devices aliasing one set of columns — was a
// hazard of ADDRESSING A DEVICE BY WHERE IT SITS. A route names an instance id now (`device/
// endpoint.h`), an id is never reissued, and there is nothing left to allocate or to collide.
//
// **THE RACK PAID FOR THE LESSON THIS SECTION KEEPS.** `rack_engine.h` on its own rename: *"an
// unchanged signature whose meaning moved is the silent wrong-order bug rather than a crash, and the
// compiler cannot see one."* `chain.device[i]` has now meant "the device drawn i-th", then "the
// device living at lane i", and now the first again — same spelling, different device, no
// diagnostic, twice. That is why these are named roads and not raw indexing, and why they stay.

/// HOW MANY DEVICES ARE IN THE CHAIN'S PATH. `count`, asked for by a name that will still be true
/// when the array's length stops being the answer.
inline size_t fxChainLength(const FxChain& chain) noexcept {
	return std::min(chain.count, chain.device.size());
}

/// **ROOM FOR `rows` DEVICES, AT LEAST DOUBLING** (unbounded devices step 6) — what an add does where it used
/// to refuse. MESSAGE THREAD ONLY: it allocates, and the audio thread's chains are grown at the drain instead
/// (`Synth::requestChainCapacity`).
inline void growFxChainFor(FxChain* chain, size_t rows) {
	if (rows > chain->device.size()) {
		chain->device.resize(std::max(rows, 2 * chain->device.size()));
	}
}

/// **THE DEVICE DRAWN AT `position`.** What `chain.device[position]` spells, asked for by name —
/// see the section banner for why this stays a road after the indirection under it has gone.
///
/// A POSITION PAST THE END ANSWERS A STATIC EMPTY DEVICE rather than reading off the end. A caller
/// that must tell "nothing there" from "a device with no tag" asks `fxChainLength` first, which is
/// the same contract `fxFilterAt` already keeps for a slot holding something else.
inline const FxDevice& fxDeviceAtPosition(const FxChain& chain, size_t position) noexcept {
	static const FxDevice kNone {};
	if (position >= fxChainLength(chain)) {
		return kNone;
	}
	return chain.device[position];
}

/// WHERE THE DEVICE WITH THIS SERIAL IS DRAWN, or `kNoPosition`. By IDENTITY rather than by lane or
/// position, which is what a caller asking "where did it end up" actually holds.
inline size_t fxPositionOfInstance(const FxChain& chain, DeviceInstanceId id) noexcept {
	for (size_t at = 0; at < fxChainLength(chain); ++at) {
		if (fxDeviceAtPosition(chain, at).row.instanceId == id) {
			return at;
		}
	}
	return kNoPosition;
}

/// THE FILTER STATE OF A SLOT THAT HOLDS ONE, and the declared defaults for a slot that does not.
///
/// The lane's answer to ModRack::dahdsrAt, and deliberately the same shape: a typed view of a slot,
/// with the honest default for a slot holding something else. A caller that must distinguish asks
/// `fxHolds` first.
inline fx::filter::State fxFilterAt(const FxChain& chain, size_t slot) {
	// A POSITION, as every caller has always passed — routed through the order now that the subscript
	// means something else.
	if (!device::sameTag(fxDeviceAtPosition(chain, slot).tag().data(), fx::filter::kTraits.tag)) {
		return {};
	}
	fx::filter::Row row {};
	// THROUGH THE SHARED CONVERSION, whose return value is the count of fields that were really
	// there — and stateOf uses it to put ABSENT fields at their declared defaults rather than at
	// zero. That is why fromDeviceRow is [[nodiscard]].
	const size_t fields = fromDeviceRow(fxDeviceAtPosition(chain, slot).row, row);
	return fx::filter::stateOf(row, fields);
}

/// THE HARM STATE OF A SLOT THAT HOLDS ONE, and the declared defaults for a slot that does not.
///
/// `fxFilterAt`'s twin, and a DELIBERATE SECOND COPY rather than a template. Two is where a
/// generalisation is least informed — "a helper is only as general as its first user" — and the
/// third occurrence is the one that will know what varies. Whoever adds it: make this a template
/// then, not now.
///
/// IT EXISTS BECAUSE A PICTURE NEEDS THE WHOLE STATE AT ONCE. `fxFilterAt`'s own note records what
/// the alternative costs: the filter card used to rebuild its settings field by field out of eight
/// published values, and shipped a curve drawing a filter the engine was not running TWICE, once per
/// field somebody forgot. HARM's card reads its controls one at a time, which is fine for a knob and
/// is exactly the wrong shape for a curve.
inline fx::harm::State fxHarmAt(const FxChain& chain, size_t slot) {
	if (slot >= chain.device.size()
	    || !device::sameTag(chain.device[slot].tag().data(), fx::harm::kTraits.tag)) {
		return {};
	}
	fx::harm::Row row {};
	const size_t fields = fromDeviceRow(chain.device[slot].row, row);
	return fx::harm::stateOf(row, fields);
}

/// Write one slot as a filter, stamping the tag and the field count from the device's own
/// declarations. The instance id already in the slot is preserved — a settings edit is not a new
/// device.
inline void setFxFilterAt(FxChain* chain, size_t slot, const fx::filter::State& state) {
	if (chain == nullptr || slot >= chain->device.size()) {
		return;
	}
	// THE WHOLE IDENTITY IS KEPT, not just the serial. A settings edit is not a new device, so it
	// keeps the lane its routes are addressed to as well as the id a saved routing names it by.
	const DeviceIdentity keep = identityOf(chain->device[slot].row);
	chain->device[slot].row = toDeviceRow(fx::filter::kTraits.tag, keep, fx::filter::rowOf(state));
}

/// Whether `slot` holds a device with this tag.
inline bool fxHolds(const FxChain& chain, size_t slot, const char* tag) {
	return slot < fxChainLength(chain)
	       && device::sameTag(fxDeviceAtPosition(chain, slot).tag().data(), tag);
}

/// WHICH SLOT HOLDS THE DEVICE WITH THIS ID, or `kNoPosition` when nothing does — the answer for a
/// routing whose device has been removed or replaced since the patch was written.
///
/// THE OUT-OF-RANGE ANSWER IS THE POINT rather than an edge case: a route that cannot be resolved is
/// DROPPED, not redirected. Bounded by `count` rather than by the array's end, so a stale id sitting
/// in a slot past the count cannot resurrect it.
size_t slotOfFxInstance(const FxChain& chain, DeviceInstanceId id);

/// The way back — what a slot's device is called, or kNoInstance for a slot holding nothing.
DeviceInstanceId instanceOfFxSlot(const FxChain& chain, size_t slot);

/// The lane a fresh instance has: the filter, switched off by its own parameter.
///
/// THIS IS ALSO WHAT AN ABSENT BLOCK MEANS, and the reasoning is worth having in one place. The
/// obvious reading of "absent" is an EMPTY chain, and for every patch that predates this block the
/// two readings are indistinguishable: such a patch has no filter parameters either, so its filter
/// is off, and a lane holding a switched-off filter renders exactly what a lane holding nothing
/// renders — the bypass is a return, and the bit-exactness case in test_synth.cpp says so.
///
/// They come apart in one place, and it decides this: there is no add-a-device affordance. A patch
/// that loaded to an empty lane would have no way to ever get a filter back, so every patch saved
/// before this block would be permanently filterless. Defaulting to the lane a fresh instance has
/// costs nothing measurable and leaves nothing unreachable.
///
/// (THE LANE SAVES AS THE RACK'S FILE since chain-as-a-device item 6: `ParamBridge::chainsPatch` and
/// `restoreChains`, through `rack/rack_state.h`. `saveFxChain`/`loadFxChain` and their device record
/// are gone — one format, not two.)
FxChain defaultFxChain();

/// Whether `tag` is in the chain — how anything downstream asks "is this device here".
bool fxChainHas(const FxChain& chain, const char* tag);

/// ADD A DEVICE TO THE END OF THE LANE, returning the slot it took or `kNoPosition` on refusal.
///
/// APPEND, BECAUSE THE GESTURE IS A `(+)` AT THE END OF THE LIST — and now only because of that. The
/// old reason was structural: appending was the one mutation that could not move an existing device's
/// COLUMNS, so insertion and reorder were separate jobs needing the table remapped with them. A lane
/// lives in its device's row, so no mutation moves anybody's columns any more and inserting in the
/// middle would cost this function nothing. It still appends because that is the gesture.
///
/// REFUSED FOR THREE REASONS, all of them the device's own declarations answering: a tag this build
/// does not know, a device the engine cannot run in a chain (see maySitInLane), and a full lane.
///
/// THE SERIAL COMES FROM THE LANE AND IS NEVER REUSED. This is the ABA rule that device_instance.h
/// exists for: a routing names its device by identity, so handing a new device an id a dropped one
/// used would silently re-point every routing that named the old one.
inline size_t addFxDevice(FxChain* chain, const char* tag) {
	if (chain == nullptr || tag == nullptr) {
		return kNoPosition;
	}
	const device::Descriptor* desc = deviceForTag(tag);
	if (desc == nullptr || !maySitInLane(*desc)) {
		return kNoPosition;
	}
	const size_t slot = chain->count;
	// NEVER FULL (unbounded devices step 6): a lane at its size grows. A chain copied for an edit is the
	// message thread's, so it may allocate here; the audio thread never runs a verb.
	growFxChainFor(chain, slot + 1);
	// **ONE DECISION NOW, WHERE THERE WERE TWO (kill-lanes step 3c).** It used to append at `count`
	// AND take the lowest free lane out of a pool — two different numbers that had to be kept together,
	// and the shape that produced three bugs in the rack. A device goes at the end of the chain; that
	// is the whole of it.
	chain->device[slot].row = defaultRowFor(*desc, { chain->nextInstanceId });
	chain->nextInstanceId = chain->nextInstanceId + 1;
	chain->count = slot + 1;
	return slot;
}

/// ADD A NEW DEVICE OF `tag` IMMEDIATELY BEFORE THE DEVICE HOLDING `anchorLane` — backlog CR, an
/// 'add before' that inserts a new effect into the chain before the effect whose ellipsis/menu was
/// used. Returns the POSITION it landed at, or `kNoPosition` if refused, exactly as
/// `addFxDevice` does.
///
/// **THE ANCHOR IS THE DEVICE, BY ITS INSTANCE ID** (kill-lanes step 3c; it was a LANE, by the
/// 2026-09-06 decision). That decision's REASON is unchanged and is why this is an id now:
/// the hand's intent is "before THAT card", so the anchor must be the card's IDENTITY and not where it
/// happens to sit — a menu that captured a position would insert before whatever had since moved
/// there. A lane was standing in for an identity because a device kept one; an instance id IS one, and
/// is the only thing here that no edit can renumber. An anchor no longer in the chain — removed
/// between the menu opening and the pick — appends, which is the only honest reading of "before a
/// device that is not there".
inline size_t insertFxDeviceBefore(FxChain* chain, DeviceInstanceId anchor, const char* tag) {
	if (chain == nullptr || tag == nullptr) {
		return kNoPosition;
	}
	const device::Descriptor* desc = deviceForTag(tag);
	if (desc == nullptr || !maySitInLane(*desc)) {
		return kNoPosition;
	}
	growFxChainFor(chain, chain->count + 1); // never full (step 6)
	const size_t anchorAt = fxPositionOfInstance(*chain, anchor);
	const size_t slot = anchorAt < chain->count ? anchorAt : chain->count;
	// **THE ROWS MOVE, BECAUSE THERE IS NO LONGER AN ADDRESS TO KEEP STILL.** This shifted an `order[]`
	// and left the storage where it lay, so that every device's lane — and therefore its routing
	// columns — survived the insert. A route names an instance id now, so nothing is disturbed by a row
	// changing index, and the array can simply say what the chain is. MEASURED cost: an `FxDevice` is
	// ~536 bytes and there are at most eight, so the worst shift is ~4 KB on the message thread.
	for (size_t i = chain->count; i > slot; --i) {
		chain->device[i] = chain->device[i - 1];
	}
	chain->device[slot] = FxDevice {};
	chain->device[slot].row = defaultRowFor(*desc, { chain->nextInstanceId });
	chain->nextInstanceId = chain->nextInstanceId + 1;
	chain->count += 1;
	return slot;
}


/// REMOVE THE DEVICE AT `position`, closing the gap behind it. False if there is nothing there.
///
/// **IT TOOK A `ModRoutes*` AND NO LONGER DOES (kill-lanes step 3c), AND THE ARGUMENT DIED WITH THE
/// THING IT GUARDED.** It was required — not optional — because a removal freed an ADDRESS back into a
/// fixed pool, and an address reissued with the old device's amounts still in it is ABA: the next
/// device would arrive already modulated, by routes no card drew. There is no pool and no address now.
/// A route names an instance id (`device/endpoint.h`), ids are never reissued, and the routes a
/// removal must drop are dropped by the record that owns them (`ChainRecord::remove`), which is the
/// one place a structural change and its routes can be made to agree.
///
/// Its serial is not reissued — nextInstanceId only ever climbs — so a routing that named it resolves
/// to nothing rather than to whatever lands in its place next.
inline bool removeFxDevice(FxChain* chain, size_t position) {
	if (chain == nullptr || position >= chain->count || position >= chain->device.size()) {
		return false;
	}
	// **THE ROWS CLOSE UP.** This shifted an `order[]` and left the storage where it lay, precisely so
	// that the survivors kept their lanes and their columns. With no lane to preserve, the array says
	// what the chain is and a removal is what it looks like.
	for (size_t i = position + 1; i < chain->count && i < chain->device.size(); ++i) {
		chain->device[i - 1] = chain->device[i];
	}
	// THE VACATED TAIL IS CLEARED rather than left as it was. Every reader bounds by `count`, so a
	// stale row past the end cannot resurrect a device — but a row that still carries a tag and a
	// serial is a device as far as anything reading the array directly is concerned, and that is a trap
	// to leave lying about for the sake of one assignment.
	chain->device[chain->count - 1] = FxDevice {};
	chain->count -= 1;
	return true;
}

/// PUT A DEVICE BACK AT `position` — the inverse of `removeFxDevice`, and the road an UNDO of a
/// remove calls.
///
/// **IT TOOK A ROUTE SLICE AND A TABLE, AND NEEDS NEITHER NOW (kill-lanes step 3c).** The device came
/// back with its row, its SERIAL and its LANE, and the routings came back separately from a `slice`,
/// because removing a device CLEARED the columns its lane addressed — they were not recoverable and
/// had to have been lifted out before the removal. Every word of that was about an address. A route
/// names an instance id now and a removal takes its device's edges with it in the RECORD, so an undo
/// puts the device back under the same id and asks the record for its routes (`routesOf(id)`); there
/// is no slice to carry and no table to write. The lane check that guarded against two devices
/// aliasing one set of columns goes with them: nothing is addressable by where it sits.
///
/// THE DEVICE COMES BACK WITH ITS SERIAL INTACT, which is what makes a routing that named it still
/// name it. `position` may be the count itself, which appends — that is what undoing an ADD's redo
/// does.
inline bool insertFxDevice(FxChain* chain, size_t position, const FxDevice& device) {
	if (chain == nullptr || position > chain->count) {
		return false;
	}
	growFxChainFor(chain, chain->count + 1); // never full (step 6)
	// THE ROWS OPEN UP — see `insertFxDeviceBefore` for why the storage moves now rather than an order.
	for (size_t i = chain->count; i > position; --i) {
		chain->device[i] = chain->device[i - 1];
	}
	chain->device[position] = device;
	chain->count += 1;
	// THE SERIAL IS NOT REISSUED AND THE COUNTER IS NOT REWOUND. An undo puts back the device that
	// was there, id and all; nextInstanceId only ever climbs, so nothing else can be handed the id
	// this device is using again.
	return true;
}

/// MOVE THE DEVICE AT `from` TO `to`, shifting whatever lies between them. False if either end names
/// no device; a move onto itself succeeds and changes nothing.
///
/// THE ORDER OF AN AUDIO CHAIN IS ITS SOUND, which is why this exists at all — it was asked for
/// by name as "reorderable".
///
/// IT DOES NOT TAKE THE ROUTING TABLE, AND THAT IS THE POINT OF THE STAGE THAT WROTE THIS LINE. It
/// used to, and the argument was a good one while the table was addressed by position: a reorder
/// permuted every moved device's columns, so the mutation could not be allowed to happen without the
/// table in hand. A reorder changes no route now, not because something carefully rewrites them, but
/// because there is nothing about a route that mentions where a device sits — a route names the
/// device's instance id, and a move does not touch one.
///
/// A REMOVED PARAMETER IS THE HONEST FORM OF THIS. Keeping `ModRoutes*` here and ignoring it would
/// leave a signature claiming a guard it no longer performs, and the next reader would go looking for
/// the remap.
inline bool moveFxDevice(FxChain* chain, size_t from, size_t to) {
	if (chain == nullptr || from >= chain->count || to >= chain->count
	    || from >= chain->device.size() || to >= chain->device.size()) {
		return false;
	}
	if (from == to) {
		return true;
	}
	// **A ROTATION OF THE ROWS, AND THAT IS DELIBERATELY WHAT IT WAS BEFORE (kill-lanes step 3c).** An
	// earlier stage made this permute an `order[]` and leave every row where it lay, so that a device's
	// LANE — and therefore its routing columns — survived a drag. That was bought for the lane, and the
	// lane is gone: a route names an instance id, which no rotation touches. So the array can once again
	// simply BE the chain, and a reorder is what it looks like. MEASURED, so the cost is stated rather
	// than assumed: `FxDevice` is ~536 bytes (a `DeviceRow` holds 128 floats) and `kMaxFxDevices` is 8,
	// so the worst move copies ~4 KB on the message thread — beneath noticing, and the reason it was
	// never the real argument.
	const FxDevice moved = chain->device[from];
	if (from < to) {
		for (size_t i = from; i < to; ++i) {
			chain->device[i] = chain->device[i + 1];
		}
	}
	else {
		for (size_t i = from; i > to; --i) {
			chain->device[i] = chain->device[i - 1];
		}
	}
	chain->device[to] = moved;
	return true;
}

} // namespace feathers


