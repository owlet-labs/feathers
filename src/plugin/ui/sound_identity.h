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

#include "plugin/gui_bridge.h"
#include "plugin/rotate_sound.h"
#include "plugin/sound_gamma.h"
#include "plugin/sound_residue.h"

#include <cstddef>

// WHAT ONE SOUND IS, and how to read or write the whole of it.
//
// This lived inside the Explore surface, which is where it was needed first: a breadcrumb records an
// identity, an undo restores one. It moved here when the sound cards grew a CLONE verb, because a
// clone is exactly "read this identity, write it into that slot" — and a second reading of what
// belongs to a sound is how one surface comes to copy less of a sound than another surface restores.
//
// The list has grown three times for reasons worth remembering, and each time something that was NOT
// copied turned out to be part of the sound: drift when the second identity axis landed (A5), the
// plane and its residue when a rotation began banking what it would have moved (A26), and gamma's
// unbounded base when the vernier arrangement gave the corner a coarse half to carry. Anything added
// to this struct has to be added to both functions below and to nothing else.

namespace feathers::ui {

/// Everything that decides what one sound SOUNDS like, which is what a breadcrumb records.
///
/// The full identity rather than a chart position — the design says so, and the reason survives every
/// change to the chart: a position is relative to a view, and a view is a thing you can lose.
///
/// WHAT IS DELIBERATELY NOT IN HERE: `PlaceX`/`PlaceY`. Where a sound sits in the blend circle is the
/// constellation's geometry — where you play it FROM — and the whole of this session's arc is that
/// identity and performance are different things (A32 for the chart, A24 for gamma, A26 for what a
/// rotation banks). A clone therefore copies what a sound sounds like and leaves where it is played
/// from to the slot it lands in.
struct SoundIdentity {
	double zone = 0.0;
	double offset = 0.0;
	/// The VERNIER: the per-sound Gamma parameter, ±4, automatable by the host.
	double gamma = 0.0;
	/// And the COARSE half beside it, unbounded and carrying no parameter id — where the Explore
	/// corner now accumulates. Home is the two added (sound_gamma.h). A double, because that is the
	/// whole point of the arrangement: folded into the float offset it would quantise at about eight
	/// zones per ULP by the time it was worth having.
	double gammaBase = 0.0;
	/// Where the sound has drifted out into its own plane. Part of the identity rather than of the
	/// performance — see sound_travel.h — so undo and the breadcrumbs have to carry it or they would
	/// restore half a sound.
	double driftX = 0.0;
	double driftY = 0.0;
	/// The plane the sound is on, when it is on one of its own. Part of the identity for the same
	/// reason drift is — Stage 3 says breadcrumbs record full identity, and a rewind that restored
	/// where a sound was but not which way its plane pointed would restore a different sound.
	SliceOrientationPin plane;
	/// AND WHAT ITS ROTATIONS BANKED. Since the A10 reversal a rotation preserves the standing point by
	/// folding the difference into a residue, so home, drift and plane no longer determine the sound on
	/// their own: restore those three without this and you land on the right ORIENTATION with the wrong
	/// sound standing on it, which is a worse failure than landing somewhere obviously else. 88 numbers
	/// a crumb, which is the price of a breadcrumb meaning what Stage 3 says it means.
	PlaneResidue residue;
	/// The Radius and Theta corners — how far from its place the sound is played, and which way
	/// (`PolarOffset`, sound_travel.h). Identity for the reason drift is: an undo that put the star back
	/// and left the sound orbiting out at the old radius would restore half a sound. LAST, because
	/// `readIdentity` builds this positionally.
	PolarOffset orbit {};

	friend bool operator==(const SoundIdentity&, const SoundIdentity&) = default;
};

/// Read a slot's identity, whole.
SoundIdentity readIdentity(const ParamBridge& bridge, size_t slot);

/// Write one into a slot: the three parameters through their own bracketed gestures, the drift, the
/// plane and the residue through theirs. Values that already match are not written, so restoring an
/// identity a sound already has spends no undo step and marks nothing dirty.
///
/// The surface's own business — selecting the slot, re-centring a view, saying so in the tray — is
/// deliberately NOT here: this writes a sound and nothing else.
void writeIdentity(ParamBridge& bridge, size_t slot, const SoundIdentity& identity);

} // namespace feathers::ui
