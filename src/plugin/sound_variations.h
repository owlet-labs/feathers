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

#include <cmath>

#include "dsp/constellation.h"
#include "dsp/slice_basis.h"
#include "dsp/spectral_shaper.h"
#include "plugin/params.h"

#include <array>
#include <cstdint>

// VARIATIONS: the stances a sound remembers. They are also called SUBSOUNDS, the mock draws them as the
// child rows under each card, and the design doc calls them variation rows — three names for the
// second level of the tree this editor has been a one-level narrowing of since it was built.
//
// WHAT ONE IS, in current state terms — V20's answer, which OVERTURNED this file's original
// "remembered placement" definition. A variation is a remembered EXPLORATION of the sound: the
// Explore box's state — zone, offset, the gamma corner's base, drift, the plane with its banked
// residue, and width. PLACE is exactly what a variation does NOT store any more — "place
// leaves the stance", a deliberate behavior change from the mock's places-era shape: where you
// stand in the blend circle is per-sound live state a variant switch never touches, so switching
// variants moves the sound through its own space and leaves its star standing. The membership rule
// is standing: a control that joins the Explore box joins this record in the same change.
//
// WRITE-THROUGH, THE MOCK'S MODEL — V19, and the reversal of this file's own first decision. The
// snapshot-compare model shipped first ("a save-point, not a mirror"), and the question whether
// turning width should take you off a stance or mutate it exposed it as a
// divergence from the mock, whose `activePos(sd)` returns `sd.places[sd.act]` and whose edits write
// INTO the active record. Decided to follow the mock for everything: editing a sound mutates its ACTIVE
// stance; there is no step-off and no standing-on comparison; preserving a stance is the
// clone-stance verb FIRST, then edit; a recall switches which record you occupy.
//
// SPELLED AS A DERIVATION, not a write on every edit road: while a stance is active, its stored
// entry here is DEAD and the live parameters are its contents — the accessors read the params for
// the active index, so hand edits and automation alike "update the stance as you play" without a
// single mid-gesture write to this struct. The stored entry is BANKED from the live values at the
// moments it becomes observable again: switching away (recall, clone-stance) and saving (the
// variations block banks active entries from the params). That is also what dissolved the old
// model's objection — the roads the editor does not own reach the active record exactly as
// faithfully as the hand does, because nothing has to chase them.
//
// EMPTY MEANS "NEVER STORED ONE", not "no exploration". A sound with no variations has exactly the
// exploration its parameters and state channels hold, which is every patch that exists today: the
// default below is therefore the state of every sound before this file existed, and a patch that
// never stores a variation loads and plays bit-identically to what it always did.

namespace feathers {

/// How many stances one sound can remember.
///
/// A judgement rather than a measurement: eight is the number of sounds, the number of slots the rail
/// draws, and enough that running out is a decision rather than an accident. It is a fixed array
/// because this is patch state that the editor writes during a gesture, and a growing list would be
/// an allocation in the middle of one.
inline constexpr size_t kMaxVariations = 8;

/// THE RIDE-ALONG VALUE MEMBERS — V18,
///
/// The category is distinct, and it is DISTINCT FROM PLACEMENT: place answers where-the-sound-STANDS
/// and stays with the slot (A34 holds — clones do not stack); a ride-along answers
/// how-the-sound-SOUNDS and follows the sound — copied by a clone from its source, carried inside
/// every stored stance, restored by a recall. Width is the first member. "Later to include more" is
/// the reason this struct exists now rather than when the second member arrives: a future per-sound
/// param joins the family by joining THIS struct (and its read/write sites, which the compiler
/// lists), and the defaulted comparison is what lets every family pin compare ONE object — a member
/// added here is automatically part of every clone, stance and round-trip claim.
struct RideAlongValues {
	/// The per-sound width, 0..1 — the SoundField::Width parameter's value. THE FIRST MEMBER ON THE
	/// WIRE AND IT STAYS FIRST: shipped FSV2 records carry width alone, and the count-driven loader
	/// reads members front-to-back, so width holding position 0 is what lets those patches load
	/// with no special case while everything after it live-fills.
	double width = 0.0;

	// THE EXPLORE BOX, V20, with the boundary case decided separately: the place leaves the
	// stance. So a stance stores the sound's EXPLORATION — the identity the box edits — and place,
	// which Perform edits, is per-sound live state a variant switch no longer touches (a deliberate
	// behaviour change from the mock's stored-perform-positions shape). The gamma member is the
	// CORNER's unbounded BASE; the vernier is a tray knob and stays out by the surface rule. The
	// membership criterion is standing: a future Explore-box control joins the family by joining
	// THIS struct, its read/write roads, and the wire member list — the box IS the criterion.
	double zone = 0.0;
	double offset = 0.0;
	double gammaBase = 0.0;
	double driftX = 0.0;
	double driftY = 0.0;
	/// The plane AND what its rotations banked, together always (A26): a turn restored without its
	/// residue restores a different sound.
	SliceOrientationPin plane {};
	PlaneResidue residue {};
	/// The Radius and Theta corners (`PolarOffset`) — Explore-box controls, so they join by the rule
	/// above. APPENDED on the wire, after the residue — and LAST here too, because this struct
	/// is built positionally (`SoundStack::liveRideAlong`).
	double orbitRadius = 0.0;
	double orbitTheta = 0.0;
	/// **THE SOUND'S KNOBS AND ITS SPECTRAL DRAWER** (2026-10-10: stances carry them — V18's ride-along values follow
	/// the clone and the stance, V20's everything in the explore box): every per-sound knob, the drawer's
	/// controls among them, in `perSoundKnobParam`'s order — THE SAME LIST a clone copies (`ui::SlotState::knobs`),
	/// so the stance list and the clone list cannot drift — and the sound's drawn curve. APPENDED on the wire after the
	/// partial rows. NOT positional: the live reads set them after the positional build.
	std::array<double, kPerSoundKnobCount> knobs {};
	spectral::DrawnCurve curve {};

	friend bool operator==(const RideAlongValues&, const RideAlongValues&) = default;
};

/// How many ride-along value members each stored stance carries ON THE WIRE — the third count in
/// the FSV2 header, beside the slot and stance counts. BUMPED WHEN RideAlongValues GROWS, together
/// with the member's write and read lines in saveSoundVariations/loadSoundVariations: the count is
/// what lets a build read a patch from a build with more members (read and drop) or fewer (absent
/// members take their declared fills) without another tag bump, ever — the orientation blocks'
/// both-counts idiom, applied per stance.
///
/// THE HONEST BOUNDARY OF THAT "EVER", per the V20 review: the promise begins at builds carrying
/// the 4096 garbage cap. Every loader shipped BEFORE it caps a stance at 64 members and refuses
/// this block whole at the header — so the V20 growth was, for those builds, exactly as breaking
/// as a tag bump (deterministic whole-load refusal, the V2 posture; install the new build before
/// saving with it). From here forward the count absorbs growth as advertised.
///
/// V-C2 IS THE FIRST GROWTH TO COLLECT ON THAT PROMISE, and it collects because vox APPENDED to the
/// residue order rather than inserting into it (see kPlaneResidueValues): 96 members became 120 then,
/// every one of the old 96 still means what it meant, and a stance from a width-and-plane-era patch
/// live-fills its twenty-four new ones — which for a sound that never rotated a vox character is
/// zero, the only reading of a row that could not have been banked. No tag bump here; the STATE
/// block's residue tag did have to bump, for a reason that does not apply to a count-driven member
/// list — see kResidueTag in audition.cpp. The same append has been collected twice since, by the
/// weave rows (2026-09-25) and the partial rows (2026-10-08), both placed after the two orbit members:
/// the record is 10 + kPlaneResidueValues members now, and it grows with that order.
/// AND THE SOUND'S DRAWN CURVE AND KNOBS after them (2026-10-10), appended, so
/// a record written before them reads every older member where it was and live-fills these. THE FIXED-SIZE CURVE FIRST
/// (its switch, then its 64 bands), THEN A COUNT OF THE KNOBS, THEN THE KNOBS — the one GROWABLE run last (stances review,
/// 2026-10-10): a knob added to `perSoundKnobParam` lengthens only the tail, so the curve never shifts under it, and the
/// count is what lets a future member appended AFTER the knobs be found from a record whose knob run is a different
/// length. (This build reads the knobs by position and drops the count: its run is the last thing in its record.)
inline constexpr size_t kRideAlongCurveWire = 10 + kPlaneResidueValues;
inline constexpr size_t kRideAlongKnobCountWire = kRideAlongCurveWire + 1 + spectral::kCurveBands;
inline constexpr size_t kRideAlongKnobsWire = kRideAlongKnobCountWire + 1;
inline constexpr size_t kRideAlongWireValues = kRideAlongKnobsWire + kPerSoundKnobCount;

/// Wire member `i` of a ride-along record, as the double the stream carries — and its setter below.
/// THE ONE PLACE THE MEMBER ORDER IS SPELLED: the writer, the loader and the absent-member fill all
/// index through this pair, so the order cannot fork. WIDTH IS MEMBER 0 AND STAYS THERE — shipped
/// FSV2 records carry width alone, and the count-driven loader reads members front-to-back, so
/// width holding position 0 is what lets those patches load with no special case. New members
/// APPEND: a case pair here, a field in the struct, and a bump of the count above.
inline double rideAlongWireValue(const RideAlongValues& values, size_t i) {
	switch (i) {
	case 0: return values.width;
	case 1: return values.zone;
	case 2: return values.offset;
	case 3: return values.gammaBase;
	case 4: return values.driftX;
	case 5: return values.driftY;
	case 6: return values.plane.pinned ? 1.0 : 0.0;
	case 7: return static_cast<double>(values.plane.generator.rotate);
	default:
		if (i - 8 < kPreWeaveResidueValues) {
			return planeResidueValue(values.residue, i - 8);
		}
		// AFTER the residue, so every earlier member keeps its position.
		if (i == 8 + kPreWeaveResidueValues) {
			return values.orbitRadius;
		}
		if (i == 9 + kPreWeaveResidueValues) {
			return values.orbitTheta;
		}
		// AND THE WEAVE ROWS AFTER THE ORBIT (2026-09-25), which were already shipped members when the
		// Resonate engine grew its residue: its rows append to the WIRE, not inside it.
		// WHY `i - 10` ADDRESSES THEM: members 0-7 are the eight scalars, 8 .. 8 + kPreWeave - 1 the older rows,
		// then the two orbit members, so wire member i >= 10 + kPreWeave is flat residue row i - 10 — which is
		// kPreWeave and up, exactly the weave rows. (A smaller i never reaches here: the branches above took
		// it.) The bound stops a member past the record's end reading off the residue.
		// THE PARTIAL ROWS (the additive engine, 2026-10-08) FOLLOW THE WEAVE ROWS here by the same
		// arithmetic: they follow them in the flat order, so wire member 10 + kPrePartial is partial row 0.
		// Appended to the wire, so every record written before them keeps its positions and live-fills them.
		// THE CURVE (its switch, its bands), THE KNOB COUNT AND THE KNOBS after the partial rows (2026-10-10).
		if (i >= kRideAlongCurveWire) {
			if (i == kRideAlongCurveWire) {
				return values.curve.on ? 1.0 : 0.0;
			}
			if (i < kRideAlongKnobCountWire) {
				return static_cast<double>(values.curve.gain[i - kRideAlongCurveWire - 1]);
			}
			if (i == kRideAlongKnobCountWire) {
				return static_cast<double>(kPerSoundKnobCount);
			}
			return i - kRideAlongKnobsWire < kPerSoundKnobCount ? values.knobs[i - kRideAlongKnobsWire] : 0.0;
		}
		return i - 10 < kPlaneResidueValues ? planeResidueValue(values.residue, i - 10) : 0.0;
	}
}

inline void setRideAlongWireValue(RideAlongValues& values, size_t i, double value) {
	switch (i) {
	case 0: values.width = value; return;
	case 1: values.zone = value; return;
	case 2: values.offset = value; return;
	case 3: values.gammaBase = value; return;
	case 4: values.driftX = value; return;
	case 5: values.driftY = value; return;
	case 6: values.plane.pinned = value != 0.0; return;
	case 7: values.plane.generator.rotate = static_cast<float>(value); return;
	default:
		if (i - 8 < kPreWeaveResidueValues) {
			setPlaneResidueValue(values.residue, i - 8, value);
		} else if (i == 8 + kPreWeaveResidueValues) {
			values.orbitRadius = value;
		} else if (i == 9 + kPreWeaveResidueValues) {
			values.orbitTheta = value;
		} else if (i >= kRideAlongCurveWire) { // the curve, the knob count, the knobs: see the reader above
			if (i == kRideAlongCurveWire) {
				values.curve.on = value != 0.0;
			} else if (i < kRideAlongKnobCountWire) {
				values.curve.gain[i - kRideAlongCurveWire - 1] = static_cast<float>(value);
			} else if (i >= kRideAlongKnobsWire && i - kRideAlongKnobsWire < kPerSoundKnobCount) {
				values.knobs[i - kRideAlongKnobsWire] = value;
			} // the count itself is the record's, not a value: see kRideAlongCurveWire
		} else if (i - 10 < kPlaneResidueValues) { // the weave then partial rows, after the orbit: see the reader above
			setPlaneResidueValue(values.residue, i - 10, value);
		}
		return;
	}
}

/// **CAN THIS STANCE BE PUT BACK?** Every value it carries has to be finite, because a recall that
/// meets one that is not does not fail — it half-applies.
///
/// THE ROADS OUT OF A STANCE ALL GUARD, WHICH IS THE PROBLEM. `writeRideAlong` hands each member to
/// a setter that refuses a non-finite one — `ParamBridge::setValue`, `setSoundGamma`,
/// `setSoundTravel`, `setSoundOrientation`, `setSoundResidue` — so a spoilt member is dropped on the
/// floor while every sound member around it lands. The engine is safe and the VERB is a liar: the
/// light moves, the history gains a step, and the sound is a mixture of the stance you asked for and
/// the one you were standing in.
///
/// ASKED OVER THE WIRE VALUES rather than over the struct's members, because the wire is exactly the
/// set a patch can carry — `loadSoundVariations` reads these as raw doubles — and a member the wire
/// does not reach cannot have arrived spoilt.
///
/// **THE FIX IS TO REFUSE, NOT TO SANITISE.** Defaulting the bad member silently would make the
/// recall succeed at restoring something the player never stored, which is the same lie wearing a
/// tidier coat. A stance that cannot be restored says so and leaves the sound alone.
[[nodiscard]] inline bool recallable(const RideAlongValues& values) {
	for (size_t i = 0; i < kRideAlongWireValues; ++i) {
		if (!std::isfinite(rideAlongWireValue(values, i))) {
			return false;
		}
	}
	return true;
}

struct SoundVariations {
	/// One sound's remembered stances.
	struct List {
		/// What each stance IS since V20: a stored set of the ride-along values — the sound's
		/// exploration. The place array that used to sit beside this is GONE from memory (place left
		/// the stance) and survives only as dead zeros in the wire record.
		std::array<RideAlongValues, kMaxVariations> values {};
		/// How many of them mean anything. ZERO IS THE ORDINARY CASE and the migration guarantee: a
		/// sound that has never stored a stance has the one implicit record its live state holds.
		uint8_t count = 0;
		/// Which one was last recalled. Meaningless while `count` is zero, and never past it.
		uint8_t active = 0;

		friend bool operator==(const List&, const List&) = default;

		bool empty() const noexcept { return count == 0; }
		bool full() const noexcept { return count >= kMaxVariations; }
	};

	std::array<List, kMaxSounds> sound {};

	friend bool operator==(const SoundVariations&, const SoundVariations&) = default;
};

/// Store `values` as a new variation of `slot`, made the active one, and say whether there was
/// room. `values` are the sound's ride-alongs at the moment of storing — its exploration, V20.
///
/// THE FIRST ONE STORES TWO. A sound with no variations already has a stance — the implicit record
/// its live state holds — so materialising the list with a single entry would silently throw away
/// nothing, while materialising it with the current stance plus its copy is what "clone this
/// variation" means on a sound that has only the implicit one. The mock's own rule, from the other
/// end: a sound is never unplaced.
inline bool storeVariation(SoundVariations& list, size_t slot, RideAlongValues values) {
	if (slot >= kMaxSounds) {
		return false;
	}
	SoundVariations::List& sound = list.sound[slot];
	if (sound.empty()) {
		sound.values[0] = values;
		sound.count = 1;
		sound.active = 0;
	}
	if (sound.full()) {
		return false;
	}
	sound.values[sound.count] = values;
	sound.active = sound.count;
	sound.count++;
	return true;
}

/// Forget one, and say whether it went.
///
/// THE LAST ONE CANNOT BE REMOVED — the mock says so outright ("a sound is never unplaced") and it is
/// the same rule from the storage side: removing the last stance would leave a list that claims a
/// sound has no place while its parameters plainly hold one. Removing the ACTIVE one leaves the
/// neighbour before it active, which is where the eye already is.
inline bool forgetVariation(SoundVariations& list, size_t slot, size_t index) {
	if (slot >= kMaxSounds) {
		return false;
	}
	SoundVariations::List& sound = list.sound[slot];
	if (index >= sound.count || sound.count <= 1) {
		return false;
	}
	for (size_t at = index; at + 1 < sound.count; ++at) {
		sound.values[at] = sound.values[at + 1];
	}
	sound.count--;
	sound.values[sound.count] = {};
	if (sound.active > index) {
		sound.active--;
	}
	else if (sound.active == index) {
		sound.active = static_cast<uint8_t>(index > 0 ? index - 1 : 0);
	}
	return true;
}

} // namespace feathers
