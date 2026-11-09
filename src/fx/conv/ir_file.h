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

#include <cstddef>
#include <string>
#include <vector>

namespace feathers::fx::conv {

/// **AN IMPULSE RESPONSE READ FROM DISK** (backlog JD, piece 2), in two stages so the markers are cheap:
///
/// 1. **THE FILE, WHOLE** (`loadIrFile`): decoded and resampled to the host rate, once per file and rate.
///    - FORMATS: WAV and AIFF (dr_wav), FLAC (dr_flac). `libs/dr_libs`, public domain or MIT-0.
///    - CHANNELS: mono is copied to both; stereo is kept; more than two keeps the first two.
///    - RATE: resampled offline to `hostRate` (`resampleOffline`), so a room is the same room in seconds
///      at any rate.
///    - LENGTH: at most `kIrMaxFileSeconds` of it is read (`truncated` says a file was longer).
/// 2. **THE SPAN BETWEEN THE MARKERS** (`trimIr`), ready for a kernel — cut again every time a marker
///    moves, from the whole file held in memory, so a drag does not go back to the disk.
///    - BEGIN/END are fractions of the whole file (markers in the impulse view choose the begin
///      and end points at which to truncate).
///    - A span longer than `maxSeconds` (the convolver's capacity) is cut there.
///    - FADES where a cut lands mid-sound: `kIrBeginFadeSeconds` in after a moved begin, `kIrCutFadeSeconds`
///      out before a moved end or the capacity's cut, so no cut clicks.
///    - LEVEL: one gain for both channels (the file's stereo balance kept) making the mean channel energy
///      1 — the level contract `generateTestIr` keeps, so every room returns at the input's level, and
///      a span that drops the loud direct sound does not come back quieter.
///
/// ALLOCATES AND READS FILES: message thread, never the audio thread.
struct LoadedIr {
	bool ok = false;
	std::string error;
	double sampleRate = 0.0;     // the rate of `left`/`right`, i.e. the host rate asked for
	double fileSampleRate = 0.0; // the rate the file was recorded at
	unsigned fileChannels = 0;
	bool truncated = false;      // stage 1: the file was longer than read; stage 2: the span was cut
	std::vector<float> left;
	std::vector<float> right;
};

/// WHERE THE MARKERS ARE, as fractions of the whole file. `end <= begin` is read as the whole file.
struct IrTrim {
	float begin = 0.0f;
	float end = 1.0f;
	bool operator==(const IrTrim&) const = default;
};

inline constexpr double kIrMaxFileSeconds = 30.0;
inline constexpr double kIrCutFadeSeconds = 0.05;
inline constexpr double kIrBeginFadeSeconds = 0.001;

[[nodiscard]] LoadedIr loadIrFile(const std::string& path, double hostRate);

/// STAGE 1 FROM DECODED FRAMES — the seam the file road and the tests share. `interleaved` holds
/// `frames * channels` samples.
[[nodiscard]] LoadedIr resampleIr(const float* interleaved, size_t frames, unsigned channels, double fileRate,
                                  double hostRate);

/// STAGE 2: the markers, then the envelope over the span (`envelope`: gains at evenly spaced points from
/// the span's first sample to its last; empty for none), then the stretch, then the capacity's cut, the fades
/// and the level.
[[nodiscard]] LoadedIr shapeIr(const LoadedIr& whole, float begin, float end, float stretch,
                               const std::vector<float>& envelope, double maxSeconds);
/// The markers alone.
[[nodiscard]] LoadedIr trimIr(const LoadedIr& whole, IrTrim trim, double maxSeconds);

/// **OFFLINE RESAMPLING**, a Kaiser-windowed sinc with `kResampleZeros` zero crossings each side,
/// cut off below the lower of the two Nyquists. Not for the audio thread: it allocates, and it is
/// O(frames x zeros). Our own rather than sst-basic-blocks' Lanczos, which is GPL3, so the IR road
/// adds nothing to the licence list.
inline constexpr int kResampleZeros = 32;
[[nodiscard]] std::vector<float> resampleOffline(const float* in, size_t frames, double fromRate, double toRate);

} // namespace feathers::fx::conv
