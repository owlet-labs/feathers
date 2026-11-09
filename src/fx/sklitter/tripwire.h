/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#pragma once

#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// THE CRUSH TRIPWIRE (2026-09-24). It was heard SKLITTER's wet go "bit crushed" mid-session — fresh //
// instances at defaults crushed too, and only a Bitwig restart cleared it. Nothing headless          //
// reproduced it: not the DSP, the lanes, device reuse, process statics, song position (double to     //
// 10 days), the sequence, an MXCSR rounding mode, or any single setting. So the next occurrence has  //
// to explain itself: what the host was feeding the device, and the moment the wet went wrong.        //
//--------------------------------------------------------------------------------------------------//
//
// **THE VALUES ARE COMPILED EVERYWHERE; THE LOGGING ONLY UNDER `FEATHERS_DIAGNOSTIC`.** These structs
// sit in `Sklitter` unconditionally so the class has one layout in every translation unit that
// includes it — the diagnostic define reaches `sklitter.cpp` and not the plugin or the tests, and a
// member that existed on one side of that line only would be two classes under one name. The calls
// that fill them are behind the define in `sklitter.cpp`, so a release build pays nothing.

/// **ONE BLOCK OF THE WET, SCANNED FOR THE TWO SIGNATURES A CRUSH CAN LEAVE IN VALUES.**
///
/// `held` counts samples where the wet repeated its previous value exactly, nonzero, WHILE THE INPUT
/// MOVED — a sample-and-hold or a decimated read. `moving` is the denominator: wet samples that were
/// live (nonzero) while the input moved. A frozen or silent input says nothing, so it counts in
/// neither. `nonFinite` is NaN or Inf in the wet, counted whatever the input did.
struct WetScan {
	size_t moving = 0;
	size_t held = 0;
	size_t nonFinite = 0;
};

/// `prevIn` / `prevWet` carry the last sample across calls so a held pair straddling a block
/// boundary is still seen. Reads `n` samples of each; writes nothing but the two carries.
inline WetScan scanWet(const float* in, const float* wet, size_t n, float& prevIn, float& prevWet) noexcept {
	WetScan s;
	for (size_t i = 0; i < n; ++i) {
		const float x = in[i];
		const float w = wet[i];
		if (!std::isfinite(w)) {
			++s.nonFinite;
		}
		else if (w != 0.0f && x != prevIn) {
			++s.moving;
			s.held += (w == prevWet) ? 1u : 0u;
		}
		prevIn = x;
		prevWet = w;
	}
	return s;
}

/// **A WINDOW TRIPS ON ANY NON-FINITE SAMPLE, OR ON A QUARTER OF ITS LIVE SAMPLES HELD.** A 2x
/// decimation holds half; clean audio holds essentially none (MEASURED on the crush sweep: at most
/// 7e-6 over every mode, puck and setting). `kMinMoving` keeps a window that barely played from
/// tripping on two coincidences.
inline constexpr size_t kTripMinMoving = 256;
[[nodiscard]] inline bool wetTripped(const WetScan& s) noexcept {
	return s.nonFinite > 0 || (s.moving >= kTripMinMoving && s.held * 4 > s.moving);
}

/// **WHAT THE HOST AND THE DEVICE LOOK LIKE, LOGGED WHEN IT CHANGES.** Compared as a whole; the
/// first difference prints one line with all of it.
struct TripSnapshot {
	double sampleRate = 0.0;
	double secondsPerBeat = 0.0;
	bool playing = false;
	uint32_t mxcsr = 0;
	size_t sliceFrames = 0;
	int32_t numSlices = 0;
	size_t bufferFrames = 0;
	int status = -1;
	int mode = -1;
	bool engage = false;
	bool enabled = false;

	friend bool operator==(const TripSnapshot&, const TripSnapshot&) = default;
};

/// **THE RUNNING STATE BEHIND THE LOG.** Block sizes as a range (a new extreme is a change); the song
/// position checked against the frames that elapsed (a mismatch is a jump or a loop); the scan's
/// window accumulated over about a second; and a line budget, so a pathological host cannot fill the
/// engine log.
struct Tripwire {
	TripSnapshot last {};
	bool haveLast = false;
	int minBlock = INT_MAX;
	int maxBlock = 0;
	double expectBeats = std::numeric_limits<double>::quiet_NaN();
	uint64_t jumps = 0;
	WetScan window {};
	size_t windowFrames = 0;
	float prevIn = 0.0f;
	float prevWet = 0.0f;
	bool tripped = false;
	uint64_t trips = 0;
	int linesLeft = 4000;
};

} // namespace feathers::fx::sklitter
