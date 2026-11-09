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

#include "plugin/device_row.h"
#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "plugin/params.h"
#include "rack/chain_record.h"
#include "rack/chain_state.h"
#include "rack/rack_state.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace feathers {

//--------------------------------------------------------------------------------------------------//
// FEATHERS' TWO CHAINS AS ONE RECORD'S — the shape, and the two views of it everything else still speaks //
//--------------------------------------------------------------------------------------------------//
//
// **ONE RECORD, TWO CHAINS**. The strip is chain 0 and the fx lane
// is chain 1. They were numbered in RANK order, every strip device ahead of every fx device, because that rank
// once decided that a modulator could reach a lane device and never the reverse; it decides nothing now —
// a route's legality is the graph's cycle test (backlog IZ: a modulator's position is only grouping), and
// since 2026-10-04 the lane holds modulators and note devices as well as audio devices (`maySitInLane`). Each
// chain's devices live in its band of the record's pool: the fx lane on 0..7, the strip on 8..11 — but
// **NOTHING ORDERS BY THAT** since kill-lanes step 3b. A chain IS its ordered run of devices, and the band
// says only where the record may park one.
//
// **`FxChain` AND `ModRack` ARE VIEWS NOW, NOT STORES.** The bridge's record is what Feathers' chains ARE;
// these two value types are how the editor, the state road and the synth still read them, derived from
// the record (message thread) or from a snapshot of what it published (audio thread) through the two
// functions below — one mapping, so the two directions cannot disagree about which POSITION a row is at.
//
// **AND NEITHER SIDE OF THE CROSSING CARRIES A LANE ANY MORE (kill-lanes step 3c).** For one step the
// state had none and the views did, so a lane was ignored on the way in and MINTED FROM THE POSITION on
// the way out; `chainsOfState` records what that cost and why it is gone. Both sides now say the same
// thing — which devices, in what order — so the mapping is a copy and there is no number for the two
// directions to disagree about.

inline constexpr size_t kStripChain = 0;
inline constexpr size_t kLaneChain = 1;

/// **HOW MANY DEVICES EACH CHAIN HOLDS, in chain order** — what a Feathers `rack::ChainRecord` is built
/// with. It was a pair of BANDS (the strip at slots 8..11, the fx lane at 0..7), and the address half of
/// that went at kill-lanes step 4d: a chain no longer owns a region of the pool, because nothing reads a
/// slot number to decide which chain a device is in. What is left is the capacity a hand can feel.
inline constexpr rack::ChainCapacity kFeathersChainCapacities[] = {
	kMaxRackSlots, // chain 0, the strip
	kMaxFxSlots,   // chain 1, the fx lane
};

/// ONE ROW AS A DEVICE OF THE STATE — tag, identity, fields. WHERE it stands is the state's own
/// (chain, position), not a field of the row.
inline rack::DeviceState deviceStateOf(const DeviceRow& row) noexcept {
	rack::DeviceState out;
	out.tag = row.tag;
	out.instanceId = row.instanceId;
	out.fieldsPresent = row.fieldsPresent < out.field.size() ? row.fieldsPresent : out.field.size();
	out.field = row.field;
	return out;
}

/// AND BACK. It took the lane the view would keep the row at; a view has none now, so this is the plain
/// inverse of `deviceStateOf` and WHERE the row goes is the caller's own index.
inline DeviceRow rowOfDeviceState(const rack::DeviceState& in) noexcept {
	DeviceRow out;
	out.tag = in.tag;
	out.instanceId = in.instanceId;
	out.fieldsPresent = in.fieldsPresent;
	out.field = in.field;
	return out;
}

/// **A DEVICE HANDED IN AS A VALUE IS NAMED BY ITS LANE** — the identity the by-value road has (a case, a
/// synth with no bridge). The engine matches every device by id since kill-lanes step 3a, and these rows
/// carry no identity worth the name: an `FxChain` and a `ModRack` NUMBER THEIR DEVICES FROM THEIR OWN
/// COUNTERS, so the strip's first device and the lane's first device are both instance 1 — two devices
/// under one id, which the engine refuses (measured: HARM never adopted, because the strip's envelope had
/// its number). A row's own lane is unique across both chains by construction (the bands), it travels with
/// the row through every reorder, and it is the same on the provision and the sync that build a state from
/// the same chains. In a range the record's counter never reaches, so it can be told from a real id.
///
/// A PUBLISHED STATE IS NOT THIS ROAD: it comes from the record, whose ids are one sequence across both
/// chains (`chainsOfState`), and it keeps them.
///
/// **A ROW KEEPS ITS OWN ID WHERE THAT NAMES ONE DEVICE**; the stand-in is for the rows that cannot be
/// told apart otherwise — one with no id at all, and the SECOND of two rows that came in under one number.
/// The second happens because an `FxChain` and a `ModRack` number their devices from their own counters, so
/// the strip's first device and the lane's first are both instance 1 (measured: HARM was never adopted,
/// because the strip's envelope had its number). A row that keeps its id keeps everything keyed on it —
/// a container's body is stored under that id, and renaming it loses the contents.
///
/// **WHAT IT IS KEYED ON, AND WHY IT IS NOT THE POSITION (kill-lanes step 3c).** A stand-in must answer the
/// same for the same device on every call: a changed id is precisely what makes the engine tear a device
/// down and build it again (`LaneRack::provision`), so a key that moved when a device was dragged would
/// rebuild it on every reorder — silently, and audibly. It was keyed on the view's LANE, which was the
/// number that did not move; 3c deletes that, and the POSITION is exactly the number that does move.
///
/// So the two cases are keyed differently, because they are different questions:
///   - **A ROW UNDER A NAME ALREADY TAKEN** keys on its OWN id, which no reorder changes. This is the
///     common case and the measured one — the two counters colliding — and it is now stable where it was
///     before, by a better road than the lane: the id is the device's own name, not an address.
///   - **A ROW UNDER NO NAME AT ALL** keys on its place, and nothing better exists: an anonymous row has no
///     identity to be stable about, and two of them are indistinguishable by construction. It is unstable
///     across a reorder and always was, for the same reason.
///
/// THIS ROAD IS THE NO-BRIDGE ONE. The live plugin hands `LaneRack` the RECORD's state, whose ids the
/// record has already made unique, so nothing here is reached; it is a case, or a synth built without a
/// bridge. That is why the anonymous case is acceptable rather than a defect to chase.
inline constexpr DeviceInstanceId kStandInIdBase = 0x80000000u;
/// The chain's half of the stand-in space, so the strip and the lane cannot mint the same name.
inline constexpr DeviceInstanceId kStandInChainBit = 0x40000000u;
/// Marks the anonymous half, so a row with no id and a row whose id happens to equal a position cannot
/// collide with one another.
inline constexpr DeviceInstanceId kStandInAnonBit = 0x20000000u;

inline DeviceInstanceId standInIdOf(size_t chain, size_t position, DeviceInstanceId named) noexcept {
	const DeviceInstanceId chainBit = chain == kLaneChain ? kStandInChainBit : 0u;
	const DeviceInstanceId key = named != kNoInstance
	                                 ? (named & (kStandInAnonBit - 1u))
	                                 : (kStandInAnonBit | static_cast<DeviceInstanceId>(position));
	return kStandInIdBase | chainBit | key;
}

/// **THE CHAINS AS A DESIRED STATE** — chain 0 the strip in SLOT order, chain 1 the fx lane in its own
/// order, each row still held to its chain's band (a strip row naming an fx lane, or the reverse, is not
/// placed: a row outside its band is one `device_slots.h` cannot rank, and a device nothing can rank is on
/// nobody's bus). Into `out`, shaped `{kMaxDeviceSlots, 2}`; allocates nothing, so the audio thread may
/// call it.
///
/// **POSITION IS THE VIEW'S OWN INDEX** — strip position `n` is strip slot `n`, lane position `n` is fx
/// position `n` — which is what lets `Synth::stripDevice`/`fxDevice` ask the converged state by position
/// and get the device the view has there. A row REFUSED (out of its band, or a second row under a name
/// already placed) is the one thing that breaks that, and it shifts everything after it by one; a
/// well-formed chain never has one, and a malformed one has a device the ranking cannot see anyway.
inline void stateOfChains(const FxChain& chain, const ModRack& strip, rack::ChainState* out) noexcept {
	out->clear();
	// THE NAME A ROW GOES UNDER: its own id, unless nothing or another row already placed has it — see
	// `standInIdOf`. The question is asked of what this state already holds, so it is the same answer every
	// time these chains are turned into a state.
	const auto nameFor = [out](DeviceInstanceId id, size_t chainIndex, size_t position) {
		return (id == kNoInstance || out->holds(id)) ? standInIdOf(chainIndex, position, id) : id;
	};
	// **NO BAND TEST LEFT (kill-lanes step 3c).** Each row had to be held to its chain's band because the
	// row carried a LANE and a lane in the wrong range was one `device_slots.h` could not rank — a device
	// nothing can rank is on nobody's bus. A row carries no lane now, so there is no range to be wrong: a
	// view's device is in the chain it is in, and the band survives only as the record's private parking.
	for (size_t slot = 0; slot < strip.count && slot < strip.device.size(); ++slot) {
		rack::DeviceState row = deviceStateOf(strip.device[slot]);
		row.instanceId = nameFor(row.instanceId, kStripChain, slot);
		(void)out->append(kStripChain, row);
	}
	for (size_t position = 0; position < fxChainLength(chain); ++position) {
		rack::DeviceState row = deviceStateOf(fxDeviceAtPosition(chain, position).row);
		row.instanceId = nameFor(row.instanceId, kLaneChain, position);
		(void)out->append(kLaneChain, row);
	}
}

/// **AND THE STATE AS THE TWO VIEWS** — each chain's run, in order, from the front of its own array.
/// `nextInstanceId` is left to the caller: it is the record's counter, which a state does not carry.
/// Allocates nothing.
///
/// **THERE IS NOTHING LEFT TO MINT (kill-lanes step 3c), AND THE ONE-STEP HISTORY IS WORTH KEEPING**,
/// because it is the clearest example in this codebase of a decision that was right, then wrong, then
/// unnecessary:
///
///   - A view's row once carried a LANE — its address in the routing tables — and `DeviceRow::routeLane`
///     argued hard against deriving that address from a POSITION: *"correct only for as long as every road
///     that permutes a chain remembers to rewrite the table in the same breath."* That paragraph was paid
///     for by real bugs.
///   - **Step 3b removed the lane from the published state**, so this function had nothing to carry across
///     and MINTED one from the position — exactly the derivation that argument forbade. It was safe, but
///     only because of an audit, and it cost one property: a minted lane was ORDER-preserving and NOT
///     IDENTITY-preserving. It agreed with the record's parking on every insert and disagreed after every
///     reorder. That cost one reference case 248 disagreements and two suites a day of stale assertions.
///   - **Step 3c deletes the field.** A view is its order; a device is its id; a route names the id. So
///     there is no address to derive, no property to lose, and no audit to keep re-running. The trade the
///     mint was making has been settled by removing the thing being traded.
///
/// **WHAT THE AUDIT ESTABLISHED IS STILL THE REASON THIS IS SAFE, so it stays on the record.** Routes are
/// held by ENDPOINT, so no renumbering can move one; device controls are not CLAP params (`kParamCount` is
/// a fixed table of synth params, sounds and macros), so no host automation ever followed a position; a
/// patch stores no lane; `orderIndexOfLane` (deleted in step 5) and `Synth::fillBusFor` (deleted in §2.4b) found their
/// device by SEARCHING the views, so they only ever wanted the order. `backplane::Graph::endpointsOf` — which decoded a coordinate
/// through the record's PARKING — never had a caller in `src/plugin/` and is deleted with the coordinates (§2.4b); a pool slot
/// is now only what it always legitimately was, an engine's own storage handle.
///
/// **AND THE RULE THE WHOLE ARC TURNS ON, kept here because this is where it was nearly broken:** if
/// something ever needs a device's address again, the fix belongs at the CROSSING — never by re-parking a
/// pool so its slots track the order. That would make every stale position-shaped coordinate in the tree
/// correct AGAIN BY COINCIDENCE: the accident this arc exists to delete, restored under a new name and
/// with nothing left to reveal it. The design rule is that ordering comes only from the graph, and a pool
/// index that tracks the order is an ordering, whatever it is called.
inline void chainsOfState(const rack::ChainState& state, FxChain* chain, ModRack* strip) noexcept {
	chain->reset(); // IN PLACE: the audio thread runs this, and `FxChain {}` would allocate a vector
	const std::span<const rack::DeviceState> lane = state.chain(kLaneChain);
	chain->count = lane.size() < chain->device.size() ? lane.size() : chain->device.size();
	for (size_t position = 0; position < chain->count; ++position) {
		chain->device[position].row = rowOfDeviceState(lane[position]);
	}
	strip->reset();
	const std::span<const rack::DeviceState> rack = state.chain(kStripChain);
	strip->count = rack.size() < strip->device.size() ? rack.size() : strip->device.size();
	for (size_t slot = 0; slot < strip->count; ++slot) {
		strip->device[slot] = rowOfDeviceState(rack[slot]);
	}
}

//--------------------------------------------------------------------------------------------------//
// FEATHERS' HALF OF A ROUTE, AS THE ONE FORMAT NAMES IT                                             //
//--------------------------------------------------------------------------------------------------//
//
// The rack's file (`rack/rack_state.h`) names a device end itself — by instance id. What no device holds
// is the host's: here a macro or a sound's weight by its index, and a destination by its PARAM ID, so
// appending to the destination table cannot move a saved route..

namespace detail {

inline bool nameFeathersSource(const void*, size_t source, uint32_t* kind, uint32_t* a) {
	if (source < kMacroCount) {
		*kind = static_cast<uint32_t>(rack::SourceKind::Macro);
		*a = static_cast<uint32_t>(source);
		return true;
	}
	if (isWeightSource(source)) {
		*kind = static_cast<uint32_t>(rack::SourceKind::Weight);
		*a = static_cast<uint32_t>(slotOfWeightSource(source));
		return true;
	}
	if (isStackSource(source)) {
		*kind = static_cast<uint32_t>(rack::SourceKind::Stack);
		*a = static_cast<uint32_t>(source - kFirstStackSource);
		return true;
	}
	return false;
}

inline bool placeFeathersSource(const void*, uint32_t kind, uint32_t a, size_t* source) {
	if (kind == static_cast<uint32_t>(rack::SourceKind::Macro) && a < kMacroCount) {
		*source = a;
		return true;
	}
	if (kind == static_cast<uint32_t>(rack::SourceKind::Weight) && a < kWeightSourceCount) {
		*source = weightSourceOf(a);
		return true;
	}
	if (kind == static_cast<uint32_t>(rack::SourceKind::Stack) && a < kStackSourceCount) {
		*source = kFirstStackSource + a;
		return true;
	}
	return false; // a kind this build does not know
}

inline bool nameFeathersDestination(const void*, size_t destination, uint32_t* kind, uint32_t* a) {
	// THE INTERNAL REGION IS SPELLED BY ITS OWN STABLE ID, never by table position: the position is
	// `kParamDestinationCount + n`, which moves whenever a parameter destination is added ahead of it,
	// and a route that stored it would silently become a route to something else.
	if (isInternalDestination(destination)) {
		*kind = static_cast<uint32_t>(rack::DestKind::Internal);
		*a = internalDestinationId(destination - kParamDestinationCount);
		return true;
	}
	const ParamId id = destinationParamId(destination);
	if (id == static_cast<ParamId>(0)) {
		return false;
	}
	*kind = static_cast<uint32_t>(rack::DestKind::Param);
	*a = static_cast<uint32_t>(id);
	return true;
}

inline bool placeFeathersDestination(const void*, uint32_t kind, uint32_t a, size_t* destination) {
	if (kind == static_cast<uint32_t>(rack::DestKind::Internal)) {
		const int internal = internalIndexOfId(a);
		if (internal < 0) {
			return false; // an internal destination this build does not have
		}
		*destination = kParamDestinationCount + static_cast<size_t>(internal);
		return true;
	}
	if (kind != static_cast<uint32_t>(rack::DestKind::Param)) {
		return false;
	}
	const int index = modDestinationIndexOf(static_cast<ParamId>(a));
	if (index < 0) {
		return false; // a parameter this build does not have, or cannot modulate
	}
	*destination = static_cast<size_t>(index);
	return true;
}

} // namespace detail

/// FEATHERS' CODEC — macros and weights by index, destinations by ParamId.
inline rack::CoreCodec feathersCoreCodec() noexcept {
	return { nullptr, &detail::nameFeathersSource, &detail::placeFeathersSource, &detail::nameFeathersDestination,
		     &detail::placeFeathersDestination };
}

} // namespace feathers
