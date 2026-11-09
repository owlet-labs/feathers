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

#include <cstdint>
#include <vector>

namespace feathers::fx::conv {

/// **THE FIRST IMPULSE RESPONSE: GENERATED, NOT LOADED** (generate the test ir).
/// Nothing in Feathers reads an audio file yet (backlog JD, scope `2026-09-24-featherverb-convolution-scope.md`),
/// so the convolution's first pass runs on a room made from a seed:
/// - silence for `kPredelaySeconds`;
/// - early reflections: `kEarlyTaps` taps between 6 and 60 ms, with different gains, signs and times on the
///   two channels, so the field is stereo from the first reflection;
/// - a diffuse tail: decorrelated noise in two bands split near 3 kHz, the lows decaying to -60 dB in
///   `kRt60Low` and the highs in `kRt60High`, so it darkens as it dies as a room does. It fades in over
///   `kTailFadeSeconds`, so it does not start with a step.
///
/// **UNIT ENERGY PER CHANNEL**, so noise convolved with it comes back at the input's level. Made at the
/// rate it will run at, so it is the same room in seconds at any rate. DETERMINISTIC in `seed`: the same
/// seed is the same bits. This allocates, so it belongs on the message thread, never the audio thread.
struct GeneratedIr {
	double sampleRate = 0.0;
	std::vector<float> left;
	std::vector<float> right;
};

inline constexpr double kPredelaySeconds = 0.007;
inline constexpr int kEarlyTaps = 14;
inline constexpr double kRt60Low = 2.2;
inline constexpr double kRt60High = 0.9;
inline constexpr double kBandSplitHz = 3000.0;
inline constexpr double kTailFadeSeconds = 0.004;
inline constexpr double kTailStartSeconds = 0.012;

[[nodiscard]] GeneratedIr generateTestIr(double sampleRate, double seconds = 2.5, uint32_t seed = 0x0f1e2d3cu);

/// **THE GENERATED ROOMS** (add generated plate and hall): four rooms made from a seed,
/// browsable beside a folder's files so the browser has somewhere to go before any file is loaded. ROOM is the
/// first IR above (`kRoomSpec`, its bits unchanged); PLATE is dense from the first millisecond, no discrete
/// reflections, bright and even; HALL waits 20 ms, scatters 24 reflections over the next 100 and builds slowly into
/// a long dark tail.
struct RoomSpec {
	double predelay;   // seconds of silence first
	int earlyTaps;     // discrete reflections
	double earlyFrom;  // where they start, seconds
	double earlySpan;  // over how long
	double earlyDecay; // their fall, as an RT60
	double earlyGain;  // their level against the tail
	double rt60Low;    // the tail below `splitHz`
	double rt60High;   // and above it
	double splitHz;
	double tailStart;  // where the tail begins
	double tailFade;   // its build
	double seconds;    // the IR's length
};
inline constexpr RoomSpec kRoomSpec { kPredelaySeconds, kEarlyTaps, 0.006, 0.054, 0.25, 4.0, kRt60Low, kRt60High,
	                                  kBandSplitHz, kTailStartSeconds, kTailFadeSeconds, 2.5 };
inline constexpr RoomSpec kPlateSpec { 0.001, 0, 0.0, 0.0, 1.0, 0.0, 2.6, 1.8, 3000.0, 0.001, 0.002, 3.0 };
inline constexpr RoomSpec kHallSpec { 0.020, 24, 0.020, 0.100, 0.6, 3.0, 3.8, 1.9, 2500.0, 0.030, 0.080, 5.0 };
/// AMBIENCE (also generated ambiance): the air around a sound rather than a space — dense early reflections
/// over the first 25 ms and a tail gone in well under a second.
inline constexpr RoomSpec kAmbienceSpec { 0.002, 18, 0.003, 0.022, 0.12, 2.5, 0.6, 0.35, 3000.0, 0.004, 0.010, 1.0 };
inline constexpr int kGeneratedRoom = 0;
inline constexpr int kGeneratedPlate = 1;
inline constexpr int kGeneratedHall = 2;
inline constexpr int kGeneratedAmbience = 3;
inline constexpr int kGeneratedRooms = 4;
[[nodiscard]] const RoomSpec& roomSpec(int which) noexcept;
[[nodiscard]] const char* generatedRoomName(int which) noexcept;
/// One of the three, at its own length. Allocates: message thread.
[[nodiscard]] GeneratedIr generateRoom(int which, double sampleRate, uint32_t seed = 0x0f1e2d3cu);

} // namespace feathers::fx::conv
