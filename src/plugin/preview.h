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

#include "dsp/constellation.h"

#include <cstddef>

// PREVIEW: LISTENING TO ONE SOUND ON ITS OWN, while the puck is somewhere else.
//
// THE PROBLEM IT SOLVES, a preview button/mode for selecting individual sounds
// even when the puck is off of an individual sound. What you hear is the blend at the puck, and the
// blend falls off with distance, so the sound you are editing on the Explore chart is the one you can
// least hear — you tune it deaf and then walk over to find out what you did.
//
// WHAT IT DOES — "STAND ON IT", chosen over the alternative. The character collapses to that
// one sound WITH THE PUCK PLACED AT ITS ANCHOR, which is not the same as muting the others: the
// travel term is `Weather x (puck - anchor)`, so standing on the sound makes it exactly zero and what
// you hear is `home + drift` — the sound at its own identity. That is precisely what the Explore
// chart's star claims to be, so the ear and the picture agree by construction rather than by anyone
// remembering to keep them in step. The alternative (drop the others, leave the puck where it is)
// would hand you the sound coloured by standing somewhere else, which is not the sound being edited.
// At Weather 0 the two are the same thing, which is most patches, and the difference only opens up
// where the terrain is being let through — exactly where it matters.
//
// WHAT IT IS NOT, and every one of these is load-bearing:
//   - NOT A PARAMETER. It spends no id, no host can automate it, and no patch carries it. It is where
//     your attention is, like the selection and the surface (ui_state.h), not what the patch is.
//   - NOT AN EDIT. It writes nothing to any identity, marks no state dirty, and opens no gesture. A
//     preview that changed the patch would be an audition you had to undo.
//   - NOT AN AUDITION HANDLE. It does not belong in AuditionTuning, whose contract is settings nobody
//     has committed to keeping; this is a shipped feature.
//   - NOT SAVED, and therefore NOT ORPHANABLE: it is cleared when the editor closes and when the
//     plugin is deactivated. A latched preview that outlived its only control would leave a plugin
//     playing one sound with nothing on screen to say why.
//
// It IGNORES the Enable switch on purpose — auditioning a sound that is not in the patch is exactly
// what you want before switching it on, and since preview writes nothing, the sound stays out of the
// patch while you listen to it.

namespace feathers {

/// Which sound is being listened to on its own, or none.
struct Preview {
	/// The value that means "nobody" — one past the last slot, so `active()` is a single comparison
	/// and there is no sentinel to get wrong in a signed conversion.
	static constexpr size_t kNone = kMaxSounds;

	size_t slot = kNone;

	bool active() const noexcept { return slot < kMaxSounds; }

	friend bool operator==(const Preview&, const Preview&) = default;
};

} // namespace feathers
