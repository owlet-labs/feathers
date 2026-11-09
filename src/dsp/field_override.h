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

namespace feathers {

// ------------------------------------------------------------------------------------------------
// THE FIELD MODIFIER CURVE. Lives in dsp/field_override.h (2026-10-30, so a note device can read it without the
// character headers), included by slice_build.h, because BOTH tiers of the engine
// have a knob in front of a field now: the flagship's operators (flagship.h) and, since 2026-08-25,
// the morph table's own amp noise, built in slice_build.h. flagship.h
// includes that header, so putting the curve under it is what lets one definition serve both — the
// alternative was a second copy in slice_build, which is this repo's most-repeated bug shape.
// ------------------------------------------------------------------------------------------------

/// THE KNOB POSITION THAT CHANGES NOTHING. Written once and read by every default, because it is not
/// a taste: it is the fixed point of fieldWithOverride, and a default that disagrees with it is
/// a caller silently asking for the field lifted to FULL. That is not hypothetical — moving the
/// semantics from "multiply" to "bias" without moving these defaults turned every unspecified sound
/// fully smooth, and `operated` went true on a character with no operators in it at all. A case
/// caught it.
///
/// RENAMED FROM kFieldNeutral (2026-08-25) when the curve below stopped being smoothness's own.
/// Six per-sound scales rest here now, and a constant named after one of them is a name that would
/// have to be read past every time somebody set a quantize default.
inline constexpr float kFieldNeutral = 0.5f;

/// A FIELD MODIFIER KNOB, applied to the field it modifies. Named fieldWithOverride until
/// 2026-08-25, when the same control went in front of quantize, sync, feedback, the point
/// count and the morph table's amp noise.
///
/// ONE FUNCTION FOR ALL SIX, deliberately. The comment above records what it cost to move these
/// semantics once already; a second function with this job is how that gets paid for twice. Every
/// caller passes a field in [0,1] and a knob in [0,1], and both are clamped here rather than at the
/// call sites, which is what makes six callers safe to add without six clamps to keep in step.
///
///   knob 0.0 -> 0            knob 0.5 -> field           knob 1.0 -> 1
inline float fieldWithOverride(float field, float knob) {
	const float k = knob < 0.0f ? 0.0f : (knob > 1.0f ? 1.0f : knob);
	const float f = field < 0.0f ? 0.0f : (field > 1.0f ? 1.0f : field);
	if (k <= kFieldNeutral) {
		return f * (k * 2.0f);
	}
	const float lift = (k - kFieldNeutral) * 2.0f;
	return f + (1.0f - f) * lift;
}

} // namespace feathers
