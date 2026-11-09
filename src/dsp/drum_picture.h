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

#include "dsp/drum_voice.h"
#include "dsp/partial_spectrum.h"

// THE DRUM AS HEARD AT A PLACE: a hit's components as a spectrum the
// terrain's lens reads (partials::cycleOf), each weighted by how long it rings — the energy a component leaves over the hit is
// ∝ a²·T60, so its heard amplitude is a·√(T60 / T60_body). Computed from the same mapping the voice plays (drum::modeRatio,
// drum::metalRatio, drum::modeT60, the band centres), never by rendering a hit per texel.
//
// Ratios are over the kind's own resting pitch (drum::kindBaseHz): a picture has no note.

namespace feathers::drum {

/// The heard spectrum of a `kind` hit with fields `f` at its resting pitch, into the first `count` partials of `out` (envAtRatio
/// the amplitude, fine 1, ratio the component's frequency over the resting pitch). A component at or above `maxRatio` is left
/// silent (a picture of N points resolves ratios below N / 2). Noise is drawn as a band of evenly spaced partials.
void heardSpectrum(Kind kind, const Fields& f, int count, partials::PartialSpectrum& out, double maxRatio = 1.0e9);

} // namespace feathers::drum
