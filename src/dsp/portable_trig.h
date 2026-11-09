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

// SINE AND COSINE THAT ROUND THE SAME ON EVERY PLATFORM, for state that feeds back on itself.
//
// std::cos(float) is the C library's cosf, and C libraries do not promise the correctly rounded result, so two of
// them can round the same input differently. -ffp-contract=off pins our arithmetic, not theirs. MEASURED 2026-10-06:
// glibc's cosf is 1 ulp off the correctly rounded float on 446,486 of the 1.09e9 floats in [0, 2 pi] (0.04%); a
// 1-ulp nudge to one of the Weave ring's window values (weave::windows, through an LD_PRELOAD shim) moved Resonate's
// orbit buzz share from 0.0049 to 0.0059 and a released note's contacts at +1 s from 230 to 30 per second. On macOS
// arm64 (CI run 37412368692) three test_fx_resonate cases went red the same way, while the run with the ring's
// orbit off its bridge matched Linux to every printed digit. The ring is a feedback system and its buzz is
// chaotic: an ulp in, a different render out.
//
// These are pure double arithmetic — a Cody-Waite reduction by pi/2 and Taylor polynomials on [-pi/4, pi/4]
// (truncation below 5e-17 there) — in their own translation unit under -ffp-contract=off, so every IEEE-754 host
// computes the same bits. MEASURED: the correctly rounded float on every float in [0, 2 pi] (against long double).
// The reduction is exact while |x| / (pi/2) fits 20 bits (|x| below about 1.6e6; derived, not measured).
// Use them where a float sine or cosine reaches state that a later sample reads; a value that is only heard
// (a pan, a gain) does not need them. Cost, MEASURED: bench_resonate's Ring 256 unchanged within the host's noise
// (619 / 629 / 622 us a block before, 624 / 620 / 628 after, alternated).

namespace feathers::dsp {

float portableSin(float x);
float portableCos(float x);

} // namespace feathers::dsp
