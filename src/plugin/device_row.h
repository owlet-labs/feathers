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

#include "device/registry.h"
#include "plugin/device_instance.h"
#include "plugin/device_registry.h"

#include <array>
#include <span>
#include <cstddef>
#include <algorithm>
#include <string_view>

// HOW ONE DEVICE'S SETTINGS TRAVEL, WHATEVER DEVICE IT IS — the one convention for carrying a
// device's state across a boundary, so that the rack and the fx lane do not each invent their own.
//
// WHY THIS IS ONE FILE AND NOT TWO IMPLEMENTATIONS. feathers-1a and I were about to solve the same
// problem in two places on the same evening: making the rack hold an MSEG means its channel must
// carry rows of different widths and different meanings, and that is exactly what the fx lane's
// setFilterSettings(slot, State) has to do. Two sessions, one problem, two files — and 1a's point
// was the decisive one: the convention that is wrong will be wrong SILENTLY, so it should exist once
// and be used twice rather than be discovered twice.
//
//------------------------------------------------------------------------------------------------//
// THE RULE, AND IT IS PAID FOR TWICE OVER                                                          //
//------------------------------------------------------------------------------------------------//
//
//   THE ROW'S WIDTH AND THE COUNT OF FIELDS PRESENT TRAVEL WITH THE PAYLOAD.
//
// A reader must be able to tell "this device had fewer fields" from "these fields were zero". Those
// are different facts and a bare array cannot distinguish them.
//
// THIS IS NOT A PRECAUTION; IT IS THE SAME BUG TWICE IN ONE EVENING, both times by someone holding
// the warning:
//
//   - The rack's instance ids rode the FILE correctly and were zeroed by ParamBridge's flattened
//     channel, because that table carried settings rows and a count and nothing else. Every device
//     route then resolved against a rack whose slots had no identities and was DROPPED on load —
//     with all 24 suites green. Fixed in bf44659.
//   - The warning that led to that fix was one I had written to 1a an hour earlier, about their fx
//     channel needing a `State` rather than a `Settings` or the keytrack bit would be silently lost.
//     Same mechanism, different field, and I walked into it while the warning was in flight.
//
// THE MECHANISM, STATED PLAINLY SO IT IS RECOGNISABLE NEXT TIME: a flattened channel silently zeroes
// whatever the row does not carry, and every reader downstream sees a PLAUSIBLE DEFAULT rather than
// a corruption. Nothing warns, nothing crashes, and the value is simply wrong forever. It is the
// honesty rule's failure case applied to a data path instead of a control.
//
// 1a's line, and they earned it by proving it against themselves the same night: A PERSON WHO
// REMEMBERS IS NOT A MECHANISM.

namespace feathers {

/// THE WIDEST ROW ANY DEVICE MAY DECLARE — a bound on transport, not a statement about any device.
///
/// DISTINCT FROM the retired control stride, and confusing the two would be a real bug. That one is
/// the MODULATABLE stride, which sizes the destination matrix; this is the count of DECLARED
/// controls, which sizes a settings row. They are not the same number and the MSEG is why: it
/// declares thirty-seven controls of which four are modulatable. A row sized by the modulatable
/// stride would truncate it; a matrix sized by this would be five times too wide.
///
/// Chosen once and generously, for the reason the other strides are: a width is a transport
/// contract, so moving it later is a format change rather than an edit.
///
/// RAISED FROM 64 TO 128 ON 2026-08-27, FOR THE DRAWN-CURVE FILTER, and the tension is named here
/// rather than buried because it is real. That device stores four frames of six pole-zero bands at
/// four floats a band — 96 fields — plus seven globals, and 64 was sized when a device held a handful
/// of knobs. At 64 it could have held THREE bands, which is not "draw a curve".
///
/// WHAT IT COSTS, MEASURED on this host rather than estimated: `sizeof(DeviceRow)` 280 -> 536 bytes
/// and `sizeof(FxChain)` 2256 -> 4304, and EVERY slot pays it whether or not the device in it needs
/// one. That is widest-device sizing in the transport, which is the same shape that was objected to in
/// the instance pool (absurdly wasteful, as if there were a fixed set of effects). The
/// difference, and the reason it is still the right call: that objection was to 5.5 MB of DSP state,
/// and this is about two kilobytes per chain.
///
/// THE ALTERNATIVE WAS CONSIDERED AND REJECTED: a side channel carrying that one device's curve
/// outside the row. It breaks the uniformity the whole registry rests on — a row IS the transport,
/// and a device that needs a private one is a device the generic paths cannot carry.
///
/// (If the row ever needs to shrink, the lever is per-band cost rather than band count.)
/// **RAISED AGAIN, 128 -> 160, ON 2026-10-28** (backlog NR), for Drawn's Mix and wet Gain (129 fields).
/// MEASURED: `sizeof(DeviceRow)` 536 -> 664 bytes, the same 128 on `rack::DeviceState`; `ControlMask` two words -> three.
///
/// **IT LIVES IN `device/registry.h` NOW**, beside the retired control stride and `kMaxDeviceOutputs`, on
/// that file's own argument: a bound that names no device belongs where no device can see it. The
/// alias stays here because this file is where a reader looks for it and where the guard below is.
///
/// WHAT MOVED IT: a container has to carry its members' rows, and a container is a DEVICE. See the
/// declaration for the whole of that argument.
inline constexpr size_t kMaxDeviceFields = device::kMaxDeviceFields;

// **THE ROW'S FLOATS PAST `params.size()` ARE THE DEVICE'S OPAQUE STATE BLOB.**
//
// A device record
// already carries a length and that many floats, and NOTHING above the device reads one of them —
// the patch writes them and reads them back (`rack/rack_state.h`), the host stores them. So this is
// not an approximation of a blob contract; it IS one, with the element type fixed to `float` and a
// ceiling of `kMaxDeviceFields - params.size()`.
//
// **WHY A CEILING AND NOT A VARIABLE-LENGTH BLOB**, measured rather than asserted: `DeviceRow` is 536
// bytes, `FxChain` holds eight of them by value at 4304 bytes, the row is copied on the audio path and
// compared with a defaulted `operator==`. A `std::vector` member brings allocation where there must be
// none after activate and takes the free comparison away. A fixed BYTE array is the only non-allocating
// byte form and is pure overhead for every device declaring no blob: at a 256-byte ceiling it is +2 KB
// on every `FxChain`, a 48% growth of a copied struct, and it would still be a ceiling — a second one,
// beside the 128-float row that already exists. The float form costs zero bytes and zero new serialiser
// paths.
//
// **WHAT WOULD TRIGGER REVISITING IT:** a device that needs BYTES rather than floats, or one that needs
// more than `kMaxDeviceFields - params.size()` of them. Automod, the first user, needs 39 of the 108 it
// has left. Neither trigger is hypothetical forever, and the answer then is a byte-shaped record — its
// own piece of work, not a widening folded into a device change.

/// AND EVERY REGISTERED DEVICE FITS, checked here rather than hoped for. The same guard the
/// modulatable stride carries, for the same reason: a device wider than the transport would compile
/// cleanly and then have its tail silently truncated on the way through a channel, which is this
/// file's entire subject.
static_assert([] {
	size_t widest = 0;
	for (const device::Descriptor& d : kDevices) {
		if (d.params.size() > widest) {
			widest = d.params.size();
		}
	}
	return widest;
}() <= kMaxDeviceFields,
              "a device declares more controls than a settings row can carry — widening this is a "
              "transport format change, not an edit. drawn takes 129 of the 160 (raised from 128 "
              "on 2026-10-28 for its Mix and wet Gain), so a band-sized block, which costs 24, "
              "still fits once and no more. If "
              "you are here because you added one, the conversation is about the CARRIER — a wider "
              "row, a band count, or packing two fields into one float — and not about nudging this "
              "number; see the drawn budget case, which states the same arithmetic where a reader "
              "will meet it first");

// AND THE DEVICE THAT FORCED IT IS NOW IN `kDevices`, so the guard above is what checks the raise.
// The two static_asserts naming `fx::drawn::kParamDescs` directly that stood here while it was
// unregistered are gone: they existed because a bound checked against a table the device was not in
// is a bound with no live reason attached, and there is nothing left for them to say that
// `widest <= kMaxDeviceFields` does not.

/// WHAT NAMES ONE DEVICE — the serial a saved routing stores, and since kill-lanes step 3c that is
/// ALL it is.
///
/// ONE STRUCT RATHER THAN A BARE ID, and the reason is this file's own subject. Every road that
/// rebuilds a row out of fresh settings — setFxFilterAt, setDahdsrAt, setMsegAt, the bridge's
/// setFilterSettings — has to carry the device's identity across, because a settings edit is not a
/// new device.
///
/// **IT CARRIED A LANE BESIDE THE SERIAL AND NO LONGER DOES.** The pair existed because a live route
/// was addressed by lane while a saved one named a serial, so a road that rebuilt a row had to
/// remember both or the device would stop being modulated the first time somebody moved one of its
/// knobs. There is one name now: a route names the instance id, live and saved alike, so the half
/// that had to be remembered has nothing to remember. The struct stays because the reason it is a
/// struct is unchanged — a rebuilt row must carry identity across explicitly — and because a bare
/// `DeviceInstanceId` at these call sites is exactly the narrowing the deleted `size_t` overloads
/// elsewhere exist to catch.
struct DeviceIdentity {
	DeviceInstanceId instanceId { kNoInstance };

	friend bool operator==(const DeviceIdentity&, const DeviceIdentity&) = default;
};

/// One device's settings, in the device's own field order, with everything a reader needs to
/// interpret them and nothing it has to be told separately.
struct DeviceRow {
	/// WHICH DEVICE THIS IS, so a reader knows what the numbers MEAN. A row of floats without a tag
	/// is a row that can only be read by somebody who already knows what wrote it, which is exactly
	/// the assumption a heterogeneous chain removes.
	device::Tag tag {};

	/// WHICH device — the instance, not the kind. See device_instance.h for why a slot index cannot
	/// serve as identity.
	DeviceInstanceId instanceId { kNoInstance };

	// **`routeLane` WAS HERE AND IS GONE (kill-lanes step 3c).** It held the device's index into the
	// live source and destination tables — an address handed out when the device was added and freed
	// when it was removed — and it was IN THE ROW on purpose, so that it travelled with its device
	// across a reorder rather than being re-derived from a position by every road that permutes a
	// chain. The argument was sound and the premise is what expired:
	// A route names the device's `instanceId`, live and saved alike; there is one name now, it is
	// never reissued, and nothing has an address to keep. The pool that handed these out, the
	// aliasing a duplicate would have caused, and the ABA hazard of reissuing one with its old
	// amounts still in the table all went with it — every one of them a hazard of addressing a device
	// by where it sits. A patch never carried this field, which is why the format did not change.

	/// HOW MANY OF `field` ARE REAL. The rest are untouched and must be read as "absent" rather than
	/// as zero — a device's own settingsOf/stateOf puts absent fields at their DECLARED DEFAULTS,
	/// which is not the same as zero for most controls and is catastrophically different for some.
	size_t fieldsPresent { 0 };

	std::array<float, kMaxDeviceFields> field {};

	friend bool operator==(const DeviceRow&, const DeviceRow&) = default;
};

//------------------------------------------------------------------------------------------------//
// THE CONVERSION, WHICH IS WHERE THE RULE IS ENFORCED OR BROKEN                                    //
//------------------------------------------------------------------------------------------------//

// THE CARRIER WITHOUT THE CONVERSION IS HALF A CONVENTION, and feathers-1a caught that the half that
// was missing is the dangerous one. `field` is kMaxDeviceFields wide; a device's own Row is its own
// width — 10 for the filter, 15 for the DAHDSR, 37 for the MSEG — so every wiring needs a copy in
// each direction, and each copy is a place to get `fieldsPresent` wrong.
//
// If the fx lane wrote its pair and the rack wrote its own, we would be back to two conventions at
// the exact seam this file exists to unify, and a WORSE seam than the one avoided: four small
// correct-looking loops in two files rather than one struct in one.
//
// So the loops live here, once, with the clamping and the stamping inside them.

/// A DEVICE'S OWN ROW, PACKAGED FOR TRANSPORT. `fieldsPresent` is stamped from the device's ACTUAL
/// declared width — never from kMaxDeviceFields and never from a literal.
///
/// A WRITER THAT STAMPED 64 WOULD MAKE EVERY FUTURE READER BELIEVE IT HAS FIELDS IT DOES NOT, and
/// those fields would read as zero rather than as absent — which is the whole failure this file is
/// about, arriving through the door built to prevent it. Taking the array by its size rather than
/// taking a count is what removes the opportunity: there is no argument to get wrong.
///
/// THE IDENTITY ARRIVES AS ONE ARGUMENT because it is one fact — see DeviceIdentity for why the lane
/// travelling separately from the serial was a silent-failure shape rather than a tidiness question.
template <size_t N>
DeviceRow toDeviceRow(const char* tag, DeviceIdentity identity, const std::array<float, N>& row,
                      std::span<const float> extra = {}) {
	static_assert(N <= kMaxDeviceFields,
	              "this device's row is wider than the transport carries — see kMaxDeviceFields");
	DeviceRow out;
	if (tag != nullptr) {
		for (size_t i = 0; i < out.tag.size() && tag[i] != '\0'; ++i) {
			out.tag[i] = tag[i];
		}
	}
	out.instanceId = identity.instanceId;
	// **N PLUS WHATEVER EXTRA STATE THE CALLER CARRIES.** This line was `= N` — the declared param
	// count — and that was the truncation: a device storing anything past its params lost it here on
	// every conversion, silently, while `gui_bridge` grew the same field in the other direction. One
	// rule now, stated on `device::Traits::extraState`.
	out.fieldsPresent = std::min(N + extra.size(), kMaxDeviceFields);
	for (size_t i = 0; i < extra.size() && N + i < kMaxDeviceFields; ++i) {
		out.field[N + i] = extra[i];
	}
	for (size_t i = 0; i < N; ++i) {
		out.field[i] = row[i];
	}
	return out;
}

/// UNPACK INTO A DEVICE'S OWN ROW, AND RETURN HOW MANY FIELDS ARE ACTUALLY THERE.
///
/// THE RETURN VALUE IS THE POINT, and a void version of this function would be a trap. The caller's
/// next move is `stateOf(row, fieldsPresent)`, and that argument is what puts ABSENT fields at their
/// DECLARED DEFAULTS rather than at zero — which for most controls is a different value and for some
/// is a catastrophically different one. A caller that had to supply the count itself would supply
/// N, and every short row would silently gain zeros for the fields it never carried.
///
/// TWO CLAMPS, AND THEY GUARD DIFFERENT THINGS. `fieldsPresent` is read out of a file and may claim
/// anything: fifty fields for a ten-field device must not walk off the end of the caller's array,
/// and it must not be believed. What is copied is the smaller of what the row claims, what the row
/// can hold, and what the caller can take.
///
/// Fields past the returned count are LEFT UNTOUCHED rather than zeroed, so a caller may pre-fill
/// `out` with defaults if it prefers that road to stateOf's.
/// [[nodiscard]] BECAUSE DISCARDING IT IS THE BUG THIS FUNCTION EXISTS TO PREVENT — feathers-1a's
/// catch, and it is this file's own argument applied to this file's own code.
///
/// The doc above says a void version would be a trap. It is only half a fix to return the count and
/// then let a caller drop it, because the DROPPING version compiles silently and reads correctly:
///
///     std::array<float, N> row {};
///     fromDeviceRow(in, row);          // return ignored
///     auto state = stateOf(row, N);    // claims N present; a short row gains zeros
///
/// That is the failure, arriving through the one door the helper left open. The attribute makes it a
/// compile error rather than something the next caller has to remember — the same move as taking the
/// write-side count off the array's type instead of from an argument. A person who remembers is not
/// a mechanism.
///
/// AND THE TAIL IS DELIBERATELY LEFT UNTOUCHED rather than zeroed, which 1a argued and I agree with:
/// zeroing would make the discarding call site look MORE correct while still being wrong, which is
/// worse than garbage a caller can only reach by ignoring the count it was handed.
template <size_t N>
[[nodiscard]] size_t fromDeviceRow(const DeviceRow& in, std::array<float, N>& out) {
	static_assert(N <= kMaxDeviceFields,
	              "this device's row is wider than the transport carries — see kMaxDeviceFields");
	size_t take = in.fieldsPresent;
	if (take > kMaxDeviceFields) {
		take = kMaxDeviceFields; // a file may claim more than the carrier holds
	}
	if (take > N) {
		take = N; // ...or more than this device has
	}
	for (size_t i = 0; i < take; ++i) {
		out[i] = in.field[i];
	}
	return take;
}

/// WHAT THIS ROW IS CALLED — the argument every settings-edit road hands back to `toDeviceRow`, so
/// that a rebuilt row cannot silently arrive as a DIFFERENT device. It carried two halves until
/// kill-lanes step 3c and carries one now; the road it exists to protect is the same either way.
constexpr DeviceIdentity identityOf(const DeviceRow& row) {
	return { row.instanceId };
}

/// Whether this row names a device at all. A slot holding an empty row holds nothing.
constexpr bool rowIsPresent(const DeviceRow& row) {
	return row.instanceId != kNoInstance && row.tag[0] != '\0';
}

/// THE DEVICE A ROW NAMES, or nullptr when this build does not know it.
///
/// A ROW FROM A DEVICE THIS BUILD HAS NEVER HEARD OF IS NOT AN ERROR — it is the case the chain
/// record was designed around, and the honest answer is to carry it untouched rather than to
/// interpret it. Returning nullptr is what lets a caller tell "unknown device" from "no device".
inline const device::Descriptor* descriptorForRow(const DeviceRow& row) {
	return deviceForTag(row.tag.data());
}


//----------------------------------------------------------------------------------------------//
// ADDING A DEVICE — the primitive the browser's (+) is built on.                                 //
//----------------------------------------------------------------------------------------------//

/// A DEVICE'S DECLARED DEFAULTS AS A ROW, which is what a slot must hold the instant it is filled.
///
/// NOT A ZEROED ROW. `fieldsPresent` is the device's full width, so every field is PRESENT and
/// carries the value the device declared — not absent, and not zero. The distinction is the whole
/// subject of this header: absent means "put your default here", and a device added by a browser has
/// no earlier state to be absent from, so its defaults are stated rather than inferred. Zero would be
/// a different sound for most controls and a broken one for some.
inline DeviceRow defaultRowFor(const device::Descriptor& desc, DeviceIdentity identity) {
	DeviceRow row;
	row.tag = device::tagOf(desc.traits.tag);
	row.instanceId = identity.instanceId;
	// **THE ROW RULE: `params.size() + extraState`, NEVER MORE AND NEVER LESS.** See
	// `device::Traits::extraState`, and the blob contract stated on `kMaxDeviceFields`. The extra floats are left at zero — `row.field` is
	// value-initialised — which is what makes a device that has never been dragged read identically
	// to one from before the state existed.
	const size_t declared = std::min(desc.params.size(), kMaxDeviceFields);
	for (size_t i = 0; i < declared; ++i) {
		row.field[i] = static_cast<float>(desc.params[i].def);
	}
	row.fieldsPresent = std::min(declared + desc.traits.extraState, kMaxDeviceFields);
	return row;
}

/// MAY THIS DEVICE SIT IN AN AUDIO CHAIN? Derived, never declared.
///
/// A device belongs in the fx lane iff it can render audio, and it says so by having an AudioOps —
/// the same argument the pointer itself makes. A `Seating` flag beside these pointers could
/// CONTRADICT them, and the whole reason `audio` is a pointer rather than a bool is that a capability
/// and its entry point must not be able to disagree.
constexpr bool maySitInAudioChain(const device::Descriptor& desc) { return desc.audio != nullptr; }

/// MAY THIS DEVICE SIT IN THE MODULATOR RACK? A modulator is a thing that publishes a signal, so a
/// device belongs there iff it declares an output. The filter declares none and is refused; a future
/// note effect that both passes notes and publishes a source satisfies BOTH predicates, which is
/// exactly the case a lane-implies-capability rule would have got wrong.
constexpr bool maySitInModRack(const device::Descriptor& desc) { return desc.outputCount() > 0; }

/// **MAY THIS DEVICE SIT IN THE FX LANE?** An audio device, a modulator or a note device — every kind
/// the engine runs., which is what the standalone rack's chains already were (`Converger::fitsAnywhere`). A
/// modulator's place in the lane is only grouping (backlog IZ) except as a note WRITER: a Tracker governs
/// the bus devices after it in path order (`RackEngine::fillBusNotes`). `maySitInAudioChain` stays the
/// narrower question — whether a device renders audio — for the readers that ask exactly that.
constexpr bool maySitInLane(const device::Descriptor& desc) {
	return desc.audio != nullptr || desc.mod != nullptr || desc.notes != nullptr;
}

/// **WHAT THE MODULATOR STRIP OFFERS BY DEFAULT — a TAG, not a capability.**
///
/// `maySitInModRack` says a device CAN sit there and stays non-exclusive on purpose (see above); this says
/// the device CONSIDERS ITSELF a modulator, which only the device can say. `"modulator"` is the word the
/// four modulators already declare in their own `kTags`, so this reads a vocabulary that existed rather
/// than inventing one. Backlog FM: today this bakes into a menu; the browser that makes it a CLEARABLE
/// default — a player who wants the whole list getting it — is still to come. Until it does, the strip's
/// (+) is the only road a HAND has into the rack, and the invariant that makes that true is not a caller
/// count: `ModPanel::showAddMenu` is the ONE picker — the (+) and the card's dots menu both open it — and
/// BOTH seating functions, `addDevice` and `insertDeviceBefore`, take their tag from `addableForTest`. So a
/// device this refuses appears in no list a hand can pick from. What the RACK accepts is
/// untouched: `addRackDevice` and `insertRackDeviceBefore` still gate on `maySitInModRack` alone, which is
/// the point — the tag narrows an OFFER, never a capability.
inline constexpr std::string_view kModulatorTag = "modulator";

/// DOES THIS DEVICE TAG ITSELF A MODULATOR? A LINEAR WALK, because `Traits::tags` is an ordered span the
/// device wrote — the ORDER is meaningful to the browser (`rack::browseDevices` groups a device under
/// `tags.front()`) and means nothing here, so this asks about membership and not about position.
///
/// A null entry is skipped rather than dereferenced: the span is a device's own array and a guest's may
/// have a hole in it, which is not a reason to crash the menu that lists it.
constexpr bool isTaggedModulator(const device::Descriptor& desc) {
	for (const char* tag : desc.traits.tags) {
		if (tag != nullptr && std::string_view(tag) == kModulatorTag) {
			return true;
		}
	}
	return false;
}

/// **A NOTE EFFECT — a device that makes notes from notes (the arps), CLAP's own word (`CLAP_PLUGIN_FEATURE_NOTE_EFFECT`).**
/// A tag
/// and not the category, because the category is display metadata only (`device::Category`): what a menu OFFERS is
/// asked of the device's own statement, and the Note FX heading only files it.
inline constexpr std::string_view kNoteEffectTag = "note-effect";

constexpr bool isTaggedNoteEffect(const device::Descriptor& desc) {
	for (const char* tag : desc.traits.tags) {
		if (tag != nullptr && std::string_view(tag) == kNoteEffectTag) {
			return true;
		}
	}
	return false;
}

/// **WHAT FEATHERS' MODULATION RACK OFFERS: a modulator OR a note effect** — the strip's picker and the fx lane's
/// modulator offer both ask this, so the two cannot drift.
constexpr bool belongsInModulationRack(const device::Descriptor& desc) {
	return isTaggedModulator(desc) || isTaggedNoteEffect(desc);
}


//----------------------------------------------------------------------------------------------//
// HOW A ROUTE FOUND ITS DEVICE — THREE ANSWERS, AND THE THIRD IS NOT AN ADDRESS AT ALL.           //
//----------------------------------------------------------------------------------------------//

// **THE WHOLE ARC, KEPT BECAUSE EACH STEP WAS RIGHT WHEN IT WAS TAKEN AND THE REASONS ARE WHAT MATTER.**
//
// FIRST, A PERMUTATION AND A REMAP, AND IT WAS CORRECT. `DeviceSlotMap` described where every slot went
// across a removal, an insertion or a move, and `remappedRoutes` rebuilt the live table through it. It
// worked, and it was tested: a case named "reordering a chain moves a routing with the device, not with
// the position" walked the permutation AND its inverse, and deleting the remap line reddened two suites.
// It was never a bug fix.
//
// WHAT IT COST WAS VIGILANCE. Six call sites had to remember the remap — removal, insertion and move, on
// each of two chains — and every future road that permutes a chain was one more place to forget.
//
// SECOND, A ROUTING LANE IN EACH ROW: an index into the source and destination tables, handed out when
// the device was added, freed when it was removed, never rewritten in between. Both chains move a device
// by moving its whole row, so the lane travelled with it BY CONSTRUCTION and there was no remap left to
// maintain. **The hazard that bought, written down at the time:** a freed lane handed to a NEW device
// inherits the old device's routes unless its columns are cleared first — CLAUDE.md's ABA rule arriving
// in a pool of twelve, better than the two that rule was written about and not remotely enough. The
// guard was `clearDeviceSlotRoutes` on every free.
//
// **THIRD, AND NOW: THE DEVICE'S OWN ID (kill-lanes, 2026-09-11: the system is a graph; addresses are not sequential
// and ordering comes only from the graph).** A route is an edge between
// endpoints, and a device end names the instance id. An id is never reissued, so there is no free list,
// no allocator, no aliasing, and no ABA — every one of which was a hazard of the ADDRESS rather than of
// the routing. The remap went because the address travelled; the address went because it was never the
// thing a route meant.
//
// THE SHAPE TO TAKE FROM IT: each step removed a class of bookkeeping by making the thing it tracked
// unnecessary, not by tracking it more carefully. A fourth step that reintroduced an address — however
// well maintained — would be walking back up this list.
//
// **`DeviceRouteSlice`, `sliceOfDeviceSlot`, `writeSliceOfDeviceSlot` AND `clearDeviceSlotRoutes` STOOD
// HERE AND ARE DELETED (kill-lanes step 5, 2026-09-13), WITH `backplane::RouteTable` AND ITS OWN TWINS.**
// They lifted one device slot's rows and columns out of the dense `sources × destinations` matrix so an
// undo could put them back and a freed lane could be scrubbed before reissue — the ABA guard. Both jobs
// are the graph's now and neither needs a matrix: a route names a `DeviceInstanceId` at each end, ids
// are never reissued, and `Graph::remove` clears the edges naming one. **No caller in `src/` remained;
// the only suite that exercised them was their own.** `test_chain_record.cpp`'s ABA case is the guard
// that survives, and it was run as the known-bad when this went.
//
// A matrix indexed `slot * stride + n` is an ADDRESS, and by design nothing has one (2026-09-13: nothing has an index except maybe
// ui constructs and/or voices within an edge).

} // namespace feathers
