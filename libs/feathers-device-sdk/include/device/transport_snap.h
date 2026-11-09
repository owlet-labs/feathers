/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <cmath>

// **A HOST'S SONG POSITION, PUT BACK ON THE SAMPLE IT STANDS ON**. CLAP carries the position in fixed point
// (2^31 per beat, and per second), so a block that starts exactly on a sample arrives up to ~1e-5 of a sample past it. A
// transport-locked reader that asks "is my event on this sample" (the first sample at or after it) then lands one
// sample late: MEASURED, an Arp's step at 6001 for 6000 at 120 bpm, in the rack plugin and then in Feathers. Every such
// reader takes its clock from the two plugins' transport conversions, so both convert through this, and nothing
// downstream needs to know.

namespace feathers::device {

/// `position` (in seconds or beats) put on the nearest sample when it lies within a thousandth of one; anything further
/// off is the host's own and left alone. `samplesPerUnit` is the sample rate (seconds) or samples per beat (beats); a
/// unit the host gave no rate for (≤ 0), or a position that is not one, passes through.
[[nodiscard]] inline double onSample(double position, double samplesPerUnit) noexcept {
	if (!(samplesPerUnit > 0.0) || !std::isfinite(samplesPerUnit) || !std::isfinite(position)) {
		return position;
	}
	const double samples = position * samplesPerUnit;
	const double nearest = std::round(samples);
	return std::abs(samples - nearest) < 1e-3 ? nearest / samplesPerUnit : position;
}

} // namespace feathers::device
