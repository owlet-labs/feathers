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

#include "plugin/ui/sound_identity.h"

#include "plugin/params.h"

#include <algorithm>

namespace feathers::ui {

SoundIdentity readIdentity(const ParamBridge& bridge, size_t slot) {
	const size_t at = std::min(slot, kMaxSounds - 1);
	const AnchorPoint drift = bridge.soundTravel().offset[at];
	return { bridge.value(static_cast<clap_id>(soundParam(slot, SoundField::Zone))),
		     bridge.value(static_cast<clap_id>(soundParam(slot, SoundField::Offset))),
		     bridge.value(static_cast<clap_id>(soundParam(slot, SoundField::Gamma))),
		     bridge.soundGamma().base[at],
		     drift.x,      drift.y,
		     bridge.soundOrientations().pin[at],
		     bridge.soundResidue().banked[at],
		     bridge.soundTravel().orbit[at] };
}

void writeIdentity(ParamBridge& bridge, size_t slot, const SoundIdentity& identity) {
	const size_t at = std::min(slot, kMaxSounds - 1);
	const auto write = [&](SoundField field, double value) {
		const auto param = static_cast<clap_id>(soundParam(slot, field));
		if (bridge.value(param) == value) {
			return;
		}
		bridge.beginGesture(param);
		bridge.setValue(param, value);
		bridge.endGesture(param);
	};
	write(SoundField::Zone, identity.zone);
	write(SoundField::Offset, identity.offset);
	write(SoundField::Gamma, identity.gamma);
	// The coarse half, which is state rather than a parameter and so has a bracket of its own — the
	// same shape drift has, and for the same reason: there is no id for a gesture to be about, so the
	// bracket's job is to tell the host the project changed.
	if (bridge.soundGamma().base[at] != identity.gammaBase) {
		bridge.beginSoundGammaGesture();
		bridge.setSoundGamma(slot, identity.gammaBase);
		bridge.endSoundGammaGesture();
	}
	// The drift half, restored with the rest: an undo that put a sound's home back and left it out in
	// the plane would be restoring half a sound.
	const AnchorPoint drift = bridge.soundTravel().offset[at];
	if (drift.x != identity.driftX || drift.y != identity.driftY) {
		bridge.beginSoundTravelGesture();
		bridge.setSoundTravel(slot, { identity.driftX, identity.driftY });
		bridge.endSoundTravelGesture();
	}
	if (bridge.soundTravel().orbit[at] != identity.orbit) {
		bridge.beginSoundTravelGesture();
		bridge.setSoundOrbit(slot, identity.orbit);
		bridge.endSoundTravelGesture();
	}
	// And the plane, restored as a STORED NUMBER rather than as the turns subtracted back off — which
	// is the whole reason the generator is the state of record and the basis is derived from it.
	if (!(bridge.soundOrientations().pin[at] == identity.plane)) {
		bridge.beginSoundOrientationGesture();
		bridge.setSoundOrientation(slot, identity.plane);
		bridge.endSoundOrientationGesture();
	}
	// AND THE RESIDUE WITH IT, in the same breath and for the same reason. Since the overrule these
	// three are one thing: a rotation moves the plane AND banks what the turn would have moved, so
	// putting back two of the three lands on the right orientation with the wrong sound standing on it.
	if (!(bridge.soundResidue().banked[at] == identity.residue)) {
		bridge.beginSoundOrientationGesture();
		bridge.setSoundResidue(slot, identity.residue);
		bridge.endSoundOrientationGesture();
	}
}

} // namespace feathers::ui
