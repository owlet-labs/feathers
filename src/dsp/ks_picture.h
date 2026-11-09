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

#include "dsp/ks_string.h"
#include "dsp/partial_spectrum.h"
#include "dsp/slice_build.h"

// THE STRING AS HEARD, AS A SPECTRUM (KS engine design §2.3): what a place on the map sounds
// like on the Karplus-Strong engine, for the pictures (the terrain, the wave display, the likes' spectra) — which have no
// note of their own, so it is read at a reference pitch. Each harmonic's amplitude is the pluck's (the tone's at k, and the
// noise's, shared as Noise says), through the pick and the pickup combs, weighted by how long the loop lets it ring
// (√(T60_k / T60_1): the energy a note gives it over its life, amplitude-equivalent); its frequency is the loop's own
// partial k (Stretch pulls the high ones sharp). Then normalised to the Additive target and dimmed by how long the
// fundamental rings, so a short, thudding string reads darker than one that sings. A PICTURE, not the sound: the sound is
// the string itself.

namespace feathers::ks {

/// The pictures' reference pitch and rate: C3 at 48 kHz.
inline constexpr double kPictureHz = kDecayReferenceHz;
inline constexpr double kPictureRate = 48000.0;

/// The first `count` harmonics (≤ partials::kMaxPartials) of the string a place gives, as a spectrum `cycleOf` reads:
/// envAtRatio the heard amplitude, fine 1, ratio the loop's partial over f0. The rest are zero.
/// A partial whose ratio reaches `maxRatio` is left silent: a picture sampled at N points resolves ratios below N / 2, and Stretch
/// pulls the high harmonics sharp past it (they would fold back as an alias, a colour no place has).
void heardSpectrum(const partials::PartialSpectrum& tone, const StringFields& material, int count,
                   partials::PartialSpectrum& out, const StringControls& controls = {}, double maxRatio = 1.0e9);

} // namespace feathers::ks
