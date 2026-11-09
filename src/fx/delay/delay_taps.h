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

#include "fx/delay/delay_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

//--------------------------------------------------------------------------------------------------//
// THE TAPS STRIP'S DATA — what the engine publishes, and what an impulse arriving NOW would become.   //
//--------------------------------------------------------------------------------------------------//
//
// **LIVE, NOT RESTATED FROM THE CONTROLS** (backlog LF: the engine publishes the live tap times
// through readings, so a modulated Time, the tape's wow or the granular spray moves the strip). The tap
// layout is MOSTLY a pure function of the settings, and "mostly" is the trouble: the head the device
// reads at is not the head the controls ask for while it glides, wobbles or crossfades, and a card that
// restated it from the controls would draw a still picture of a moving delay. So the device publishes
// the heads it actually READ AT on its last sample, and the strip expands those.
//
// **THE STRIP IS AN IMPULSE RESPONSE, CHANNEL BY CHANNEL.** Each tick is one arrival on one OUTPUT, at
// the gain it arrives with: left output above the centre line, right output below. The dry path is the
// tick at zero. Each tick also says which INPUT it came from, which is the only way Ping-Pong can be
// drawn honestly: with a centred input and no Spread, a crossed loop arrives at exactly the times an
// uncrossed one does (both channels carry the same thing), so a picture by output alone would show
// Ping-Pong doing nothing. Coloured by origin, the crossing is what alternates.
//
// THE LOOP IS EXPANDED LINEARLY, from the device's own topology (`delay.cpp`):
//
//     wet_c(t)  = Σ_k g_c[k] · line_c(t − h_c·f[k])        (one tap at f = 1, g = 1 outside Multi-Tap)
//     loop_c(t) = line_c(t − h_c)                          (the BASE tap's raw read, in every mode)
//     line_c(t) = send · in_c(t) + feedback · loop_c'(t)   (c' = c, or the other channel under Ping-Pong)
//                                                          (Classic: line_L takes send · ½(in_L + in_R), line_R none)
//
// REVERSE replaces the heard taps with two mirrored reads of the line (`delay_types.h`), and its loop is the
// same forward base-tap read — so the line, and the recursion above, are unchanged.
//
// WHAT THE EXPANSION LEAVES OUT, named so nobody reads it as drawn: Tone and the tape's HF Loss are
// filters, so they colour a repeat rather than scale it, and the saturation and the safety clip are
// unity below full scale. The heights are the loop's broadband gain, `feedback` per pass.

namespace feathers::fx::delay {

/// AT MOST THIS MANY GRAINS ARE PUBLISHED — the first of the pool's ACTIVE prefix. Enough to see the
/// spray move; the pool itself is `kMaxGrains`, which is far more than a readings slot should carry.
inline constexpr size_t kGrainReadings = 16;

/// **WHAT THE STRIP DRAWS FOR A MODE** — not `Mode` itself, because three modes are one picture: Clean,
/// Xfade and Tape all read ONE head per channel (Tape's wobble and Xfade's second head are folded into the
/// published heads), and Granular with Grain Size at its OFF stop is Clean (`delay_types.h`).
///
/// `Reverse` (backlog MB) is its own picture: two windowed heads per channel, each playing a chunk backwards,
/// so an arrival's time depends on WHERE in its chunk the line is when the impulse goes in.
enum class TapLayout : int { Single, Granular, MultiTap, Reverse };

/// THE READINGS, BY INDEX — the order `Delay::processStereo` publishes and `decodeTapPicture` reads.
namespace reading {
inline constexpr size_t kLayout = 0;     ///< `TapLayout`
inline constexpr size_t kHeadL = 1;      ///< seconds — the head the last sample READ at, wobble included
inline constexpr size_t kHeadR = 2;      ///< the right channel's
inline constexpr size_t kFadeFromL = 3;  ///< seconds — the OLD head while an Xfade runs
inline constexpr size_t kFadeFromR = 4;
inline constexpr size_t kFadeNew = 5;    ///< the new head's weight (1 when no fade is running)
inline constexpr size_t kFadeOld = 6;    ///< the old head's weight (0 when no fade is running)
inline constexpr size_t kFeedback = 7;   ///< loop gain per pass
inline constexpr size_t kPingPong = 8;   ///< `PingPong`: 0 off, 1 Stereo, 2 Classic — the loop crosses at 1 and 2
inline constexpr size_t kWetGain = 9;    ///< input to first pass: `send * wetGain`, i.e. Mix with or without Dub
inline constexpr size_t kDryGain = 10;   ///< the dry path's gain
inline constexpr size_t kAxisUnit = 11;  ///< seconds — Sync's base before the Time multiplier, for the axis
inline constexpr size_t kTapCount = 12;  ///< how many heard taps per channel (1 outside Multi-Tap)
inline constexpr size_t kPassEarly = 13; ///< Granular: how much earlier than the tap one pass can land, seconds
inline constexpr size_t kPassLate = 14;  ///< Granular: how much later
inline constexpr size_t kGrainCount = 15;   ///< how many grains follow, at most `kGrainReadings`
inline constexpr size_t kGrainSpread = 16;  ///< seconds the right channel's grains read behind the left's
inline constexpr size_t kTapFrac = 17;                       ///< `kMaxTaps` fractions of the head
inline constexpr size_t kTapGainL = kTapFrac + kMaxTaps;     ///< `kMaxTaps` heard gains, left
inline constexpr size_t kTapGainR = kTapGainL + kMaxTaps;    ///< `kMaxTaps` heard gains, right
inline constexpr size_t kGrainDelay = kTapGainR + kMaxTaps;  ///< `kGrainReadings` left-channel delays, seconds
inline constexpr size_t kGrainWeight = kGrainDelay + kGrainReadings; ///< each grain's weight as heard
/// Reverse: the leading head's phase in its chunk, 0..1, for the next sample (the chunk is `kHeadL`/`kHeadR`).
inline constexpr size_t kRevPhaseL = kGrainWeight + kGrainReadings;
inline constexpr size_t kRevPhaseR = kRevPhaseL + 1;
/// THE FEEDBACK LANE (delay-love S3): its measured gain per pass (1 without a lane; `kFeedback` already carries it),
/// the shortest tap the lane allows, seconds (0 without one), and the loop limiter's gain (1 = not limiting).
inline constexpr size_t kLanePass = kRevPhaseR + 1;
inline constexpr size_t kTapFloor = kLanePass + 1;
inline constexpr size_t kLoopLimit = kTapFloor + 1;
inline constexpr size_t kCount = kLoopLimit + 1;
} // namespace reading

inline constexpr size_t kReadingCount = reading::kCount;

/// THE READINGS, DECODED. Seconds and linear gains throughout.
struct TapPicture {
	TapLayout layout = TapLayout::Single;
	std::array<double, 2> head {};      ///< [L, R]
	std::array<double, 2> fadeFrom {};  ///< [L, R]
	float fadeNew = 1.0f;
	float fadeOld = 0.0f;
	float feedback = 0.0f;
	bool pingPong = false; ///< the loop crosses channels (Stereo or Classic)
	bool classic = false;  ///< and the input enters the LEFT line only, as mono (Classic)
	float wetGain = 0.0f;
	float dryGain = 1.0f;
	double axisUnit = 0.0;
	int tapCount = 1;
	std::array<double, kMaxTaps> tapFrac {};
	std::array<std::array<float, kMaxTaps>, 2> tapGain {}; ///< [channel][tap]
	double passEarly = 0.0;
	double passLate = 0.0;
	size_t grainCount = 0;
	double grainSpread = 0.0;
	std::array<double, kGrainReadings> grainDelay {};
	std::array<float, kGrainReadings> grainWeight {};
	std::array<double, 2> revPhase {}; ///< Reverse: [L, R] the leading head's phase in its chunk
	float lanePass = 1.0f;  ///< the FEEDBACK FX lane's measured gain per pass — already in `feedback`
	double tapFloor = 0.0;  ///< seconds: the shortest tap the lane allows; 0 without a lane
	float loopLimit = 1.0f; ///< the loop limiter's gain, 1 when it is not limiting
};

/// **NOTHING PUBLISHED IS NOTHING DRAWN** — a short row (a device that has not run yet, a host that never
/// installed a sink) decodes to `false`, and the card draws its bed and says so rather than a picture of
/// zeros.
[[nodiscard]] inline bool decodeTapPicture(const float* values, size_t count, TapPicture* out) {
	if (values == nullptr || out == nullptr || count < kReadingCount) {
		return false;
	}
	namespace r = reading;
	TapPicture p {};
	p.layout = static_cast<TapLayout>(std::clamp(static_cast<int>(std::lround(values[r::kLayout])), 0, 3));
	p.head = { static_cast<double>(values[r::kHeadL]), static_cast<double>(values[r::kHeadR]) };
	p.fadeFrom = { static_cast<double>(values[r::kFadeFromL]), static_cast<double>(values[r::kFadeFromR]) };
	p.fadeNew = values[r::kFadeNew];
	p.fadeOld = values[r::kFadeOld];
	p.feedback = values[r::kFeedback];
	p.pingPong = values[r::kPingPong] >= 0.5f;
	p.classic = values[r::kPingPong] >= 1.5f;
	p.wetGain = values[r::kWetGain];
	p.dryGain = values[r::kDryGain];
	p.axisUnit = static_cast<double>(values[r::kAxisUnit]);
	p.tapCount = std::clamp(static_cast<int>(std::lround(values[r::kTapCount])), 1, kMaxTaps);
	for (size_t k = 0; k < static_cast<size_t>(kMaxTaps); ++k) {
		p.tapFrac[k] = static_cast<double>(values[r::kTapFrac + k]);
		p.tapGain[0][k] = values[r::kTapGainL + k];
		p.tapGain[1][k] = values[r::kTapGainR + k];
	}
	p.passEarly = static_cast<double>(values[r::kPassEarly]);
	p.passLate = static_cast<double>(values[r::kPassLate]);
	p.grainCount = std::min(kGrainReadings,
	                        static_cast<size_t>(std::max(0L, std::lround(values[r::kGrainCount]))));
	p.grainSpread = static_cast<double>(values[r::kGrainSpread]);
	for (size_t i = 0; i < kGrainReadings; ++i) {
		p.grainDelay[i] = static_cast<double>(values[r::kGrainDelay + i]);
		p.grainWeight[i] = values[r::kGrainWeight + i];
	}
	p.revPhase = { static_cast<double>(values[r::kRevPhaseL]), static_cast<double>(values[r::kRevPhaseR]) };
	p.lanePass = values[r::kLanePass];
	p.tapFloor = static_cast<double>(values[r::kTapFloor]);
	p.loopLimit = values[r::kLoopLimit];
	*out = p;
	return true;
}

/// ONE ARRIVAL ON THE STRIP.
struct StripTick {
	double seconds = 0.0; ///< after an impulse arriving now
	float gain = 0.0f;    ///< linear, as heard
	int channel = 0;      ///< the OUTPUT it is heard on: 0 left (drawn above), 1 right (below)
	int origin = 0;       ///< the INPUT it came from — differs from `channel` only through a crossed loop
	int pass = 0;         ///< 0 the first pass through the line, n the n-th repeat; -1 the dry path
	double early = 0.0;   ///< Granular: how far before `seconds` this pass's energy can land
	double late = 0.0;    ///< Granular: how far after

	friend bool operator==(const StripTick&, const StripTick&) = default;
};

/// **THE STRIP'S HEIGHTS ARE DECIBELS, FROM THIS FLOOR TO UNITY.** A feedback tail is geometric, so on a dB
/// scale its repeats fall along a STRAIGHT line whose slope is the Feedback — the tail reads as one shape
/// rather than as a first tick and a scatter of specks. Linear heights put the default (Mix 35%, Feedback
/// 35%) at a third of the strip and its second repeat at a pixel; photographed, on the first draft.
inline constexpr float kStripFloorDb = -48.0f;
/// BELOW THE FLOOR AN ARRIVAL IS NOT DRAWN — it would be a tick of no height.
inline constexpr float kStripGainFloor = 0.0039810717f; // 10^(-48/20)

/// HOW MUCH OF A HALF-STRIP A GAIN REACHES, 0..1 — the dB scale above, clamped at unity.
[[nodiscard]] inline float stripHeightOf(float gain) {
	if (!(gain > kStripGainFloor)) {
		return 0.0f;
	}
	const float db = 20.0f * std::log10(gain);
	return std::clamp((db - kStripFloorDb) / -kStripFloorDb, 0.0f, 1.0f);
}
/// AT MOST THIS MANY PASSES ARE EXPANDED PER INPUT, so Feedback past unity terminates on the span alone.
inline constexpr int kStripMaxPasses = 64;

/// **EVERY ARRIVAL OF AN IMPULSE ON BOTH INPUTS, UP TO `spanSeconds`** — the dry path first, then each
/// input's chain of passes in order: input L's arrivals, then input R's. One chain per input, because the
/// loop feeds ONE tap back (the base tap's raw read), so each pass has exactly one successor.
///
/// **AN XFADE IS DRAWN ON THE FIRST PASS ONLY** — the old head at its fading weight beside the new one. A
/// fade lasts `kXfadeSeconds`, so by the time a repeat is due the old head has gone and the repeats run
/// from the new one.
[[nodiscard]] inline std::vector<StripTick> stripTicks(const TapPicture& p, double spanSeconds) {
	std::vector<StripTick> out;
	for (int c = 0; c < 2; ++c) {
		if (p.dryGain > kStripGainFloor) {
			out.push_back({ 0.0, p.dryGain, c, c, -1, 0.0, 0.0 });
		}
	}
	const bool granular = p.layout == TapLayout::Granular;
	const bool fading = p.layout == TapLayout::Single && p.fadeOld > 0.0f;
	// **CLASSIC PING-PONG IS ONE CHAIN, NOT TWO.** Both inputs are summed to mono into the LEFT line, so an impulse on
	// both inputs — a centred one — is ONE arrival chain at unit level, starting on the left and crossing every pass.
	// Drawn as two half-height chains it would stack two ticks at every arrival and show neither the level heard nor
	// the bounce. It is drawn as origin 0, the left, where it enters.
	const int origins = p.classic ? 1 : 2;
	for (int origin = 0; origin < origins; ++origin) {
		int c = origin;
		double tau = 0.0;
		float a = 1.0f;
		for (int pass = 0; pass < kStripMaxPasses; ++pass) {
			const auto ci = static_cast<size_t>(c);
			const double h = p.head[ci];
			if (!(h > 0.0) || tau > spanSeconds) {
				break;
			}
			// EACH PASS THROUGH THE GRANULATOR THROWS AGAIN, so the window a repeat can land in is the
			// single pass's window times the passes it has made — its support, not a guess at its spread.
			const double passes = static_cast<double>(pass + 1);
			const double early = granular ? p.passEarly * passes : 0.0;
			const double late = granular ? p.passLate * passes : 0.0;
			const float newWeight = (fading && pass == 0) ? p.fadeNew : 1.0f;
			if (p.layout == TapLayout::Reverse) {
				// **TWO RETURNS A PASS, MIRRORED.** The heads' mirrors fall every half chunk from where the leading
				// head is (`revPhase`); the two inside one chunk after the copy enters the line each read it back,
				// at `2·mirror − entry`, weighted `sin^2` of how far into that head's chunk the copy sits — the
				// two weights sum to one. The whisker is that head's playback window, the chunk it is heard in.
				const double half = 0.5 * h;
				const double age = std::clamp(p.revPhase[ci], 0.0, 1.0) * h;
				const double first = -age + std::ceil((tau + age) / half) * half;
				for (const double mirror : { first, first + half }) {
					const double into = mirror - tau;
					const double sn = std::sin(3.14159265358979323846 * into / h);
					const auto g = static_cast<float>(static_cast<double>(a * p.wetGain) * sn * sn);
					const double t = 2.0 * mirror - tau;
					if (into >= 0.0 && into < h && t <= spanSeconds && g > kStripGainFloor) {
						out.push_back({ t, g, c, origin, pass, into, h - into });
					}
				}
			}
			for (int k = 0; k < p.tapCount && p.layout != TapLayout::Reverse; ++k) {
				const auto ki = static_cast<size_t>(k);
				const double t = tau + h * p.tapFrac[ki];
				const float g = a * p.wetGain * p.tapGain[ci][ki] * newWeight;
				if (t <= spanSeconds && g > kStripGainFloor) {
					out.push_back({ t, g, c, origin, pass, early, late });
				}
			}
			if (fading && pass == 0) {
				const double t = tau + p.fadeFrom[ci];
				const float g = a * p.wetGain * p.fadeOld;
				if (p.fadeFrom[ci] > 0.0 && t <= spanSeconds && g > kStripGainFloor) {
					out.push_back({ t, g, c, origin, pass, 0.0, 0.0 });
				}
			}
			// THE LOOP: the base tap, one feedback's worth, into this channel's line or the other's.
			tau += h;
			a *= p.feedback;
			if (!(std::abs(a) * p.wetGain > kStripGainFloor)) {
				break;
			}
			if (p.pingPong) {
				c = 1 - c;
			}
		}
	}
	return out;
}

/// ONE GRAIN IN FLIGHT, on one channel — where it is reading now and how much of it is heard.
struct StripGrain {
	double seconds = 0.0;
	float weight = 0.0f;
	int channel = 0;

	friend bool operator==(const StripGrain&, const StripGrain&) = default;
};

/// THE PUBLISHED GRAINS, both channels: the left at its delay, the right `grainSpread` behind it — the two
/// reads `Delay::renderGrains` makes of every grain.
[[nodiscard]] inline std::vector<StripGrain> stripGrains(const TapPicture& p) {
	std::vector<StripGrain> out;
	if (p.layout != TapLayout::Granular) {
		return out;
	}
	for (size_t i = 0; i < p.grainCount; ++i) {
		const float w = p.grainWeight[i] * p.wetGain;
		out.push_back({ p.grainDelay[i], w, 0 });
		out.push_back({ p.grainDelay[i] + p.grainSpread, w, 1 });
	}
	return out;
}

/// **THE AXIS FOLLOWS THE HAND, NOT THE MODULATION.** A span derived from the live head moves WITH the
/// head, so every tick would sit still while the delay moved under it — the one thing the live strip
/// exists to show. So the span is taken from `unitSeconds`, the tap the UNMODULATED controls ask for, and
/// snapped up a 1-2-5 ladder to at least three of them: a modulated Time, a glide or a wobble moves the
/// ticks across a still axis, and the axis itself steps only when a hand takes the tap past a rung.
[[nodiscard]] inline double stripSpanSeconds(double unitSeconds) {
	constexpr double kPeriods = 3.0;
	const double want = std::clamp(unitSeconds, kMinDelaySeconds, kMaxDelaySeconds) * kPeriods;
	double decade = 0.001;
	for (int i = 0; i < 8; ++i) {
		for (const double step : { 1.0, 2.0, 5.0 }) {
			if (decade * step >= want) {
				return decade * step;
			}
		}
		decade *= 10.0;
	}
	return want;
}

} // namespace feathers::fx::delay
