/*
 * Copyright © 2024-2025 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fx/owltt/owltt.h"

#include "character/phi_triangle.hpp"
#include "character/util.hpp"
#include "device/registry.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <atomic>
#include <cmath>
#include <cstring>

using owlet::dsp::phi::wrapPhase;
using owlet::dsp::triangleFloat;

namespace feathers::fx::owltt {

namespace {

constexpr float kThresholdRefdB = 21.49f; // log(2^31), matches firmware
constexpr int32_t kNumCharacterZones = 8;
constexpr int32_t kNumVibeZones = 8;
// Per-band peak detection stride (bass=4, mid=2, high=1), matching firmware's kPeakStride.
constexpr std::array<int32_t, 3> kPeakStride = {4, 2, 1};
// Soft clip knee points: bands at +6dB (2x), output at 0dBFS (1x).
constexpr float kBandClipKnee = 2.0f;
constexpr float kOutputClipKnee = 1.0f;
/// The firmware's `kGainQ22MaxBand`: 256x, +48 dB, the most its Q9.22 band gain could hold.
constexpr float kMaxBandGain = 256.0f;
/// The firmware's `kMinGap` on both crossover menu items: the high corner sits at least this far
/// above the low one, so the split cannot invert.
constexpr float kMinCrossoverGapHz = 100.0f;

/// Soft clipping: halves the excess above/below knee. Ported from firmware's softClip (util.hpp:144).
[[nodiscard]] float softClip(float x, float knee) {
	if (x > knee) {
		return knee + (x - knee) * 0.5f;
	}
	if (x < -knee) {
		return -knee + (x + knee) * 0.5f;
	}
	return x;
}

} // namespace

// ========================================================================
// State serialization
// ========================================================================

Settings stateOf(const Row& row) {
	Settings s;
	size_t i = 0;
	s.mode = row[i++];
	s.enabled = row[i++] > 0.5f;
	s.threshold = row[i++];
	s.ratio = row[i++];
	s.bandwidth = row[i++];
	s.upDownSkew = row[i++];
	s.attack = row[i++];
	s.release = row[i++];
	s.character = row[i++];
	s.vibe = row[i++];

	s.low.threshold = row[i++];
	s.low.ratio = row[i++];
	s.low.bandwidth = row[i++];
	s.low.level = row[i++];

	s.mid.threshold = row[i++];
	s.mid.ratio = row[i++];
	s.mid.bandwidth = row[i++];
	s.mid.level = row[i++];

	s.high.threshold = row[i++];
	s.high.ratio = row[i++];
	s.high.bandwidth = row[i++];
	s.high.level = row[i++];

	s.lowCrossover = row[i++];
	s.highCrossover = row[i++];
	s.outputGain = row[i++];
	s.blend = row[i++];
	// THE CLIP CHOICE: 0 Off, 1 Master (what On was), 2 Bands.
	{
		const float clip = row[i++];
		s.softClip = clip > 0.5f;
		s.clipOnly = clip > 2.5f ? 0 : (clip > 1.5f ? 1 : 2); // 3 Both, 2 Bands, 1 Master
	}
	s.characterMode = static_cast<uint32_t>(std::clamp(std::lround(row[i++]), 0L, 8L));
	s.vibeMode = static_cast<uint32_t>(std::clamp(std::lround(row[i++]), 0L, 8L));
	s.stereoLink = std::clamp(row[i++], 0.0f, 1.0f);
	s.midSide = row[i++] > 0.5f;
	s.width = std::clamp(row[i++], 0.0f, 2.0f);
	s.low.width = std::clamp(row[i++], 0.0f, 2.0f);
	s.mid.width = std::clamp(row[i++], 0.0f, 2.0f);
	s.high.width = std::clamp(row[i++], 0.0f, 2.0f);
	s.driveDb = std::clamp(row[i++], kDriveMinDb, kDriveMaxDb);
	s.lookahead = static_cast<uint32_t>(std::clamp(std::lround(row[i++]), 0L, static_cast<long>(kLookaheadChoices - 1)));
	s.keyTrack = row[i++] > 0.5f;
	for (BandParams* band : { &s.low, &s.mid, &s.high }) {
		band->upRatio = std::clamp(row[i++], 0.0f, 1.0f);
		band->downRatio = std::clamp(row[i++], 0.0f, 1.0f);
	}
	for (BandParams* band : { &s.low, &s.mid, &s.high }) {
		band->attack = std::clamp(row[i++], 0.0f, 1.0f);
		band->release = std::clamp(row[i++], 0.0f, 1.0f);
	}
	for (BandParams* band : { &s.low, &s.mid, &s.high }) {
		band->inputDb = std::clamp(row[i++], kBandInputMinDb, kBandInputMaxDb);
	}

	// **THE WALK ENDS WHERE THE ENUM ENDS, AND THAT IS THE WHOLE DISCIPLINE.** This walk is positional
	// — `i++` down the row — so an entry read where it makes sense rather than where its `Param` sits
	// shifts every field after it. Two phase params were once appended after `vibe`, which is where
	// they belonged by MEANING and two dozen slots from where their ids were, and every band's
	// threshold, ratio and level then read one slot late; the device diverged to 7e3 out of a 4.0
	// input on its own defaults and the registry sweep caught it. Those two are DELETED now (Feedback,
	// 2026-09-09) — from the enum and from here in one edit, which is the other half of the same
	// hazard, since matching counts prove nothing about matching positions. The reading order is the
	// ENUM's, not the idea's, and `tests/test_fx_owltt.cpp` states that ordering a second time by hand.

	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	size_t i = 0;
	row[i++] = s.mode;
	row[i++] = s.enabled ? 1.0f : 0.0f;
	row[i++] = s.threshold;
	row[i++] = s.ratio;
	row[i++] = s.bandwidth;
	row[i++] = s.upDownSkew;
	row[i++] = s.attack;
	row[i++] = s.release;
	row[i++] = s.character;
	row[i++] = s.vibe;

	row[i++] = s.low.threshold;
	row[i++] = s.low.ratio;
	row[i++] = s.low.bandwidth;
	row[i++] = s.low.level;

	row[i++] = s.mid.threshold;
	row[i++] = s.mid.ratio;
	row[i++] = s.mid.bandwidth;
	row[i++] = s.mid.level;

	row[i++] = s.high.threshold;
	row[i++] = s.high.ratio;
	row[i++] = s.high.bandwidth;
	row[i++] = s.high.level;

	row[i++] = s.lowCrossover;
	row[i++] = s.highCrossover;
	row[i++] = s.outputGain;
	row[i++] = s.blend;
	row[i++] = !s.softClip ? 0.0f : (s.clipOnly == 0 ? 3.0f : (s.clipOnly == 1 ? 2.0f : 1.0f));
	row[i++] = static_cast<float>(s.characterMode);
	row[i++] = static_cast<float>(s.vibeMode);
	row[i++] = s.stereoLink;
	row[i++] = s.midSide ? 1.0f : 0.0f;
	row[i++] = s.width;
	row[i++] = s.low.width;
	row[i++] = s.mid.width;
	row[i++] = s.high.width;
	row[i++] = s.driveDb;
	row[i++] = static_cast<float>(s.lookahead);
	row[i++] = s.keyTrack ? 1.0f : 0.0f;
	for (const BandParams* band : { &s.low, &s.mid, &s.high }) {
		row[i++] = band->upRatio;
		row[i++] = band->downRatio;
	}
	for (const BandParams* band : { &s.low, &s.mid, &s.high }) {
		row[i++] = band->attack;
		row[i++] = band->release;
	}
	for (const BandParams* band : { &s.low, &s.mid, &s.high }) {
		row[i++] = band->inputDb;
	}

	return row;
}

// ========================================================================
// OWLTT DSP class
// ========================================================================

Owltt::Owltt() {
	for (auto& band : bands_) {
		band.setAttack(0.25f);
		band.setRelease(0.25f);
		band.setThresholdDown(0.5f);
		band.setRatioDown(0.0f);
		band.setRatioUp(0.0f);
		band.setBandwidth(0.5f);
		band.setOutputLevel(0.5f);
	}
}

void Owltt::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// THE GAIN SMOOTHER'S COEFFICIENT, from a time constant in SECONDS — see `kGainSmoothingSeconds`.
	// Computed here and read nowhere else, which is the whole of what makes it independent of both the
	// block length and the host's rate.
	{
		const double tau = kGainSmoothingSeconds * sampleRate_;
		gainSmoothAlpha_ = tau > 1.0 ? static_cast<float>(1.0 - std::exp(-1.0 / tau)) : 1.0f;
	}
	restSilenceNeeded_ = static_cast<int>(kRestSilentSeconds * sampleRate_);
	// THE METER REFRESH IN SAMPLES — see the note on `meterRefreshPeriodSamples_`. Scaled by the
	// host's rate as well, so the indicator holds the same TIME at 44.1 kHz as at 48.
	{
		const double atReference = kMeterRefreshBuffers * kMeterReferenceFrames;
		meterRefreshPeriodSamples_ =
		    std::max(1, static_cast<int>(atReference * sampleRate_ / 48000.0));
	}
	restSilence_ = 0;
	// THE LOOKAHEAD'S LINE (backlog ND), sized here for the longest choice so the render never allocates. One more slot
	// than the longest delay, so the sample read and the sample written are never the same slot.
	lookCap_ = lookaheadSamplesFor(kLookaheadChoices - 1, sampleRate_) + 1;
	look_.assign(static_cast<size_t>(lookCap_) * kLookaheadLanes, 0.0f);
	clearLookahead();
	for (auto& band : bands_) {
		band.prepare(sampleRate);
	}
	for (auto& pair : channels_) {
		for (auto& channel : pair) {
			channel.prepare(sampleRate);
		}
	}
}

void Owltt::resetCrossovers() {
	for (auto& x : ap1_) {
		x.reset();
	}
	for (auto& x : ap2_) {
		x.reset();
	}
	for (auto& x : twisted_) {
		x.reset();
	}
	for (auto& x : ap3_) {
		x.reset();
	}
	for (auto& x : lr2fast_) {
		x.reset();
	}
	for (auto& x : lr2full_) {
		x.reset();
	}
	for (auto& x : lr4fast_) {
		x.reset();
	}
	for (auto& x : lr4full_) {
		x.reset();
	}
	for (auto& x : twist3_) {
		x.reset();
	}
}

void Owltt::reset() {
	for (auto& band : bands_) {
		band.reset();
	}
	for (auto& pair : channels_) {
		for (auto& channel : pair) {
			channel.reset();
		}
	}
	resetCrossovers();
	lastCrossoverType_ = 255;
	for (auto& pair : smoothedGain_) {
		pair.fill(1.0f);
	}
	smoothedOutputGain_ = 1.0f;
	inputGain_.fill(1.0f);
	frameCount_ = 0;
}

void Owltt::clearLookahead() {
	std::fill(look_.begin(), look_.end(), 0.0f);
	lookPos_ = 0;
}

void Owltt::lookaheadStep(std::array<float*, kLookaheadLanes> lanes, int frames) {
	if (lookLen_ == 0) {
		return;
	}
	for (int i = 0; i < frames; ++i) {
		const uint32_t w = lookPos_;
		const uint32_t r = (w + lookCap_ - lookLen_) % lookCap_;
		for (size_t k = 0; k < kLookaheadLanes; ++k) {
			float& x = lanes[k][i];
			const float in = x;
			x = look_[static_cast<size_t>(r) * kLookaheadLanes + k];
			look_[static_cast<size_t>(w) * kLookaheadLanes + k] = in;
		}
		lookPos_ = w + 1 == lookCap_ ? 0 : w + 1;
	}
}

void Owltt::setTarget(const Settings& settings) {
	target_ = settings;
}

void Owltt::updateCrossoverFrequencies() {
	const float sr = static_cast<float>(sampleRate_);
	// THE HIGH CORNER KEEPS THE FIRMWARE'S 100 Hz GAP ABOVE THE LOW ONE. Its two menu items clamp
	// each other so `low > high` cannot be dialled; this device's descriptor ranges overlap (20..2000
	// against 200..20000) and nothing clamped, so a low corner dragged past the high one INVERTED the
	// split — the "low" band above the "high" (OWLTT parity sweep, 2026-09-16). Clamped where the
	// number is used, so every road a row can arrive by is covered; the card shows the row's values.
	// KEY TRACK: the corners the note and the knobs' offsets put them on, glided in 5 ms (`trackedCrossoverHz`); off, or
	// with no note yet, the knobs exactly as before — the same floats, so Off is the device before Key Track to the bit.
	float low = target_.lowCrossover;
	float highKnob = target_.highCrossover;
	if (target_.keyTrack && heardNote_) {
		const double noteHz = keyHzOf(noteSemitones_);
		const double targets[2] { std::log2(trackedCrossoverHz(target_.lowCrossover, true, noteHz)),
			                      std::log2(trackedCrossoverHz(target_.highCrossover, true, noteHz)) };
		const double alpha = 1.0 - std::exp(-static_cast<double>(std::max(sliceFrames_, 1)) / (kKeySmoothSeconds * std::max(sampleRate_, 1.0)));
		for (size_t k = 0; k < 2; ++k) {
			if (!keyPrimed_) {
				keyLog2Now_[k] = targets[k];
			}
			keyLog2Now_[k] += alpha * (targets[k] - keyLog2Now_[k]);
			if (std::fabs(targets[k] - keyLog2Now_[k]) < kGlideSnap) {
				keyLog2Now_[k] = targets[k];
			}
		}
		keyPrimed_ = true;
		low = static_cast<float>(std::exp2(keyLog2Now_[0]));
		highKnob = static_cast<float>(std::exp2(keyLog2Now_[1]));
	}
	else {
		keyPrimed_ = false;
	}
	effectiveLowCrossoverHz_ = low;
	const float high = std::max(highKnob, low + kMinCrossoverGapHz);
	effectiveHighCrossoverHz_ = high;
	for (auto& x : ap1_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : ap2_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : twisted_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
		x.setTwist(vibeTwist_);
	}
	for (auto& x : ap3_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : lr2fast_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : lr2full_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : lr4fast_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : lr4full_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
	}
	for (auto& x : twist3_) {
		x.setLowCrossover(low, sr);
		x.setHighCrossover(high, sr);
		x.setTwist(vibeTwist_);
	}
}

/// Port of the firmware's setVibe + setCharacter. Computes all character/vibe-derived parameters.
/// Matches the firmware's zone math exactly (phi triangles, zone multipliers, OWLTT oscillation).
void Owltt::computeCharacterAndVibe() {
	// **THE TWO PHASE OFFSETS ENTER HERE, AND THESE ARE THE ONLY LINES THAT EVER WRITE THEM.** Both
	// members were declared and read — six `wrapPhase` calls below, plus the twist and the OWLTT
	// gate — and assigned nowhere, so every φ triangle in this function ran at phase zero from the
	// port until 2026-08-28. some effects with zone knobs are missing gamma functions.
	//
	// Taken from `target_` here rather than in `setTarget` so the value used and the values derived
	// from it are read in the same pass. `setTarget` is called from the row bridge and this from
	// `beginBlock`; keeping the read adjacent to its uses is what stops the two drifting.
	//
	// **THE LAP IS THE PHASE, WHICH IS WHAT LETS THE COORDINATE BE UNBOUNDED — AND IS NOW THE WHOLE
	// OF IT.**
	//
	// BY DESIGN, none of these is bounded. `Character` and `Vibe` declared
	// 0..1 and their lapping was a UI gesture — `ZoneCoordBox` wrapped the box and carried +1 into a
	// separate phase param on the way past the eighth zone. **That made the bound load-bearing: the
	// coordinate could not leave 0..1, because leaving it was implemented by not leaving it.**
	//
	// Derived here instead. The whole-number part of the coordinate IS how many laps have been turned,
	// so this says "keep turning past the 8th zone and the phase keeps advancing" as arithmetic
	// rather than as a gesture. The zone below then reads the FRACTION.
	//
	// **AND THE PHASE PARAMS ARE GONE, AND WITH THEM THE DUAL-GANG VALUE BOXES.**
	// Once the lap arrived here, every (zone, lap) pair was reachable from the coordinate alone; what
	// the separate counter still added was a FRACTIONAL offset BETWEEN laps, which is finer than the
	// quantum the consumption below is built on. That is what made the second control redundant
	// rather than merely crowded, and `owltt_types.h` carries the arithmetic in full.
	feelPhaseOffset_ = std::floor(characterCoordinate());
	vibePhaseOffset_ = std::floor(vibeCoordinate());

	// --- setVibe (firmware multiband.h:998) ---
	const float v = vibeCoordinate();
	characterComputed_ = false;

	auto vibeZoneInfo = computeZone(v - std::floor(v), kNumVibeZones);
	int32_t vibeZone = vibeZoneInfo.index;
	float vibeZonePos = vibeZoneInfo.position;

	// Twist modulation from global vibe position.
	// THE POSITION INSIDE THE LAP, NOT A CLAMP (backlog EH, shape 2). Vibe is unbounded since 2026-09-09
	// and its whole part is the lap (`vibePhaseOffset_` above); this was `clamp(v, 0, 1)`, the identity
	// while Vibe was 0..1 and a saturation after — and the phi block below runs ONLY past the first lap,
	// exactly where the clamp was a constant, so all eight vibe phases sat flat across every lap's
	// interior. The fraction is what the else branch already reads through `vibeZonePos`.
	const float globalVibePos = v - std::floor(v);
	if (globalVibePos < 0.5f) {
		vibeTwist_ = 1.0f;
	}
	else {
		const float rampPos = (globalVibePos - 0.5f) * 2.0f;
		const float twistPhase = std::fmod(rampPos * 10.0f + vibePhaseOffset_, 1.0f);
		const float triangleVal = triangleFloat(twistPhase);
		const float blend = rampPos;
		vibeTwist_ = 1.0f - blend * 0.5f * (1.0f - triangleVal);
	}

	// Full phi-triangle evolution when vibePhaseOffset is engaged.
	if (vibePhaseOffset_ != 0.0f) {
		const double phRaw = static_cast<double>(vibePhaseOffset_);
		const float ph225 = wrapPhase(phRaw * owlet::dsp::phi::kPhi225);
		const float ph300 = wrapPhase(phRaw * owlet::dsp::phi::kPhi300);
		const float ph350 = wrapPhase(phRaw * owlet::dsp::phi::kPhi350);
		const float ph375 = wrapPhase(phRaw * owlet::dsp::phi::kPhi375);
		const float ph400 = wrapPhase(phRaw * owlet::dsp::phi::kPhi400);
		const float ph325 = wrapPhase(phRaw * owlet::dsp::phi::kPhi325);
		const float ph360 = wrapPhase(phRaw * owlet::dsp::phi::kPhi360);
		const float ph385 = wrapPhase(phRaw * owlet::dsp::phi::kPhi385);

		const float wpVal = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi225 - 0.25f + ph225);
		vibePhaseWidth_ = {wpVal, wpVal, wpVal};
		vibePhaseKnee_ = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi300 + ph300);
		vibePhaseTiming_[0] = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi350 - 0.25f + ph350);
		vibePhaseTiming_[1] = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi375 + 0.083f + ph375);
		vibePhaseTiming_[2] = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi400 + 0.417f + ph400);
		vibePhaseSkew_[0] = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi325 + 0.25f + ph325);
		vibePhaseSkew_[1] = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi360 - 0.083f + ph360);
		vibePhaseSkew_[2] = 0.5f * triangleFloat(globalVibePos * owlet::dsp::phi::kPhi385 + 0.583f + ph385);
	}
	else {
		switch (vibeZone) {
		case 0: // Sync
			vibePhaseWidth_ = {vibeZonePos * 0.1f, vibeZonePos * 0.1f, vibeZonePos * 0.1f};
			vibePhaseKnee_ = vibeZonePos * 0.1f;
			vibePhaseTiming_ = {vibeZonePos * 0.1f, vibeZonePos * 0.1f, vibeZonePos * 0.1f};
			vibePhaseSkew_ = {vibeZonePos * 0.1f, vibeZonePos * 0.1f, vibeZonePos * 0.1f};
			break;
		case 1: // Spread
			vibePhaseWidth_ = {0.0f, 0.0f, 0.0f};
			vibePhaseKnee_ = 0.167f * vibeZonePos;
			vibePhaseTiming_ = {0.0f, 0.333f * vibeZonePos, 0.667f * vibeZonePos};
			vibePhaseSkew_ = {0.0f, 0.333f * vibeZonePos, 0.667f * vibeZonePos};
			break;
		case 2: // Pairs
			vibePhaseWidth_ = {0.0f, 0.0f, 0.0f};
			vibePhaseKnee_ = 0.5f * vibeZonePos;
			vibePhaseTiming_ = {0.0f, 0.5f * vibeZonePos, 0.0f};
			vibePhaseSkew_ = {0.0f, 0.5f * vibeZonePos, 0.0f};
			break;
		case 3: // Cascade
			vibePhaseWidth_ = {0.25f * vibeZonePos, 0.25f * vibeZonePos, 0.25f * vibeZonePos};
			vibePhaseKnee_ = 0.5f * vibeZonePos;
			vibePhaseTiming_ = {0.0f, 0.25f * vibeZonePos, 0.5f * vibeZonePos};
			vibePhaseSkew_ = {0.0f, 0.333f * vibeZonePos, 0.667f * vibeZonePos};
			break;
		case 4: // Invert
			vibePhaseWidth_ = {0.0f, 0.0f, 0.0f};
			vibePhaseKnee_ = 0.0f;
			vibePhaseTiming_ = {0.5f * vibeZonePos, 0.5f * vibeZonePos, 0.5f * vibeZonePos};
			vibePhaseSkew_ = {0.5f * vibeZonePos, 0.5f * vibeZonePos, 0.5f * vibeZonePos};
			break;
		case 5: // Pulse
			vibePhaseWidth_ = {0.0f, 0.0f, 0.0f};
			vibePhaseKnee_ = 0.1f * vibeZonePos;
			vibePhaseTiming_ = {0.0f, 0.05f * vibeZonePos, 0.1f * vibeZonePos};
			vibePhaseSkew_ = {0.5f, 0.55f * vibeZonePos, 0.6f * vibeZonePos};
			break;
		case 6: // Drift
			vibePhaseWidth_ = {0.2f * vibeZonePos, 0.2f * vibeZonePos, 0.2f * vibeZonePos};
			vibePhaseKnee_ = 0.3f * vibeZonePos;
			vibePhaseTiming_ = {0.1f * vibeZonePos, 0.2f * vibeZonePos, 0.4f * vibeZonePos};
			vibePhaseSkew_ = {0.15f * vibeZonePos, 0.35f * vibeZonePos, 0.25f * vibeZonePos};
			break;
		case 7: { // Twist
			const float wpVal = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi225 - 0.25f);
			vibePhaseWidth_ = {wpVal, wpVal, wpVal};
			vibePhaseKnee_ = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi300);
			vibePhaseTiming_[0] = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi350 - 0.25f);
			vibePhaseTiming_[1] = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi375 + 0.083f);
			vibePhaseTiming_[2] = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi400 + 0.417f);
			vibePhaseSkew_[0] = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi325 + 0.25f);
			vibePhaseSkew_[1] = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi360 - 0.083f);
			vibePhaseSkew_[2] = 0.5f * triangleFloat(vibeZonePos * owlet::dsp::phi::kPhi385 + 0.583f);
			break;
		}
		default:
			vibePhaseWidth_ = {0.0f, 0.0f, 0.0f};
			vibePhaseKnee_ = 0.0f;
			vibePhaseTiming_ = {0.0f, 0.0f, 0.0f};
			vibePhaseSkew_ = {0.0f, 0.0f, 0.0f};
		}
	}

	// --- setCharacter (firmware multiband.h:683) ---
	const float c = characterCoordinate();
	// NOT A LIVE MEMO, AND SAYING SO BECAUSE IT LOOKS LIKE ONE. `characterComputed_` is set false at
	// the top of this same function, so this test is always false and the character half recomputes
	// every block. That is why the phase offset above needs no entry here — but if the reset is ever
	// removed, this guard must gain `feelPhaseOffset_` or moving the phase alone will do nothing.
	if (c == characterKnob_ && characterComputed_) {
		return;
	}
	characterKnob_ = c;
	characterComputed_ = true;

	// THE FRACTION, BECAUSE THE WHOLE PART IS NOW THE LAP — see `feelPhaseOffset_` above. `computeZone`
	// clamps to 0..1 and is right to: what reaches it is a position inside one lap.
	auto zoneInfo = computeZone(c - std::floor(c), kNumCharacterZones);
	int32_t zone = zoneInfo.index;
	float zonePos = zoneInfo.position;

	// IN DOUBLE BEFORE THE MULTIPLY (backlog EH, shape 2), as the vibe half of this function does a
	// hundred lines up: `feelPhaseOffset_` is a float lap count to 2^23, and a float product with a phi
	// constant loses the fraction the wrap needs — all six phases collapsed to 0 past ~5.2 million laps
	// and quantised long before.
	const double feelRaw = static_cast<double>(feelPhaseOffset_);
	const float ph025 = wrapPhase(feelRaw * owlet::dsp::phi::kPhi025);
	const float ph033 = wrapPhase(feelRaw * owlet::dsp::phi::kPhi033);
	feelPhase033_ = ph033;
	const float ph050 = wrapPhase(feelRaw * owlet::dsp::phi::kPhi050);
	const float ph067 = wrapPhase(feelRaw * owlet::dsp::phi::kPhi067);
	const float ph075 = wrapPhase(feelRaw * owlet::dsp::phi::kPhi075);
	const float ph100 = wrapPhase(feelRaw * owlet::dsp::phi::kPhi100);

	const uint8_t crossoverType = static_cast<uint8_t>(std::round(target_.mode));
	// **A TYPE THAT COMES IN STARTS CLEAN** (backlog EH, shape 6). Nine crossover objects sit behind
	// the switch and each kept its allpass state from whenever its type last ran, so a mode change
	// resumed a filter mid-tail — the multimode filter guards the identical case ("coming back from
	// silence with whatever it held when it was last needed would ring a stale tail into the signal").
	if (crossoverType != lastCrossoverType_) {
		resetCrossovers();
		lastCrossoverType_ = crossoverType;
	}
	// THE FOUR "WEIRD" CROSSOVERS ARE QUIRKY, TWISTED, WEIRD AND TWIST3 — the allpass creative set,
	// whose bass the Width/Air/OTT zones let run wider because those splitters collapse it less. The
	// firmware tests `1 || 2 || 3 || 9` against a type numbered FROM ZERO (`multiband.h`, `setCrossoverType`
	// caps at 9); this device's Mode is the menu's 1..10, so the same literals here meant AP 6dB, Quirky,
	// Twisted and INVERTED, and never Weird or Twist3 — off by one for the port's whole life (OWLTT parity
	// sweep, 2026-09-16). `kModeNames` is the numbering: 2 Quirky, 3 Twisted, 4 Weird, 10 Twist3.
	const bool weirdXover =
	    (crossoverType == 2 || crossoverType == 3 || crossoverType == 4 || crossoverType == 10);

	// Response: 0=smooth, 1=punchy.
	switch (zone) {
	case 3: // Punch
		response_ = 0.9f + zonePos * 0.1f;
		break;
	case 6: // OTT
		response_ = 0.85f + zonePos * 0.15f;
		break;
	case 4: // Air
		response_ = 0.1f - zonePos * 0.1f;
		break;
	case 5: // Rich
		response_ = 0.15f - zonePos * 0.15f;
		break;
	case 7: { // OWLTT
		response_ = 0.5f + 0.5f * triangleFloat(zonePos * owlet::dsp::phi::kPhi150 * 0.85f +
		                                       vibePhaseWidth_[0] + ph033 - 0.3f);
		break;
	}
	default:
		response_ = 0.5f;
	}

	// Stereo width [bass, mid, high].
	switch (zone) {
	case 0: // Width
		bandWidth_[0] = weirdXover ? (-0.2f + zonePos * 1.5f) : (0.7f + zonePos * 0.5f);
		bandWidth_[1] = 0.85f + zonePos * 0.85f;
		bandWidth_[2] = 0.9f + zonePos * 1.1f;
		break;
	case 4: // Air
		bandWidth_[0] = weirdXover ? (0.6f + zonePos * 0.4f) : 0.7f;
		bandWidth_[1] = 1.0f + zonePos * 0.5f;
		bandWidth_[2] = 1.1f + zonePos * 0.7f;
		break;
	case 6: // OTT
		bandWidth_[0] = weirdXover ? (0.6f + zonePos * 0.4f) : 0.7f;
		bandWidth_[1] = 1.0f + zonePos * 0.5f;
		bandWidth_[2] = 1.0f + zonePos * 0.7f;
		break;
	case 3: // Punch
		bandWidth_[0] = weirdXover ? (0.5f + zonePos * 0.3f) : 0.7f;
		bandWidth_[1] = 0.85f + zonePos * 0.1f;
		bandWidth_[2] = 0.95f + zonePos * 0.15f;
		break;
	case 7: { // OWLTT
		bandWidth_[0] =
		    weirdXover
		        ? (-0.3f + 1.2f * triangleFloat(zonePos * owlet::dsp::phi::kPhi100 * 0.85f +
		                                        vibePhaseWidth_[0] + ph025 - 0.3f))
		        : 0.5f;
		bandWidth_[1] =
		    0.8f + 0.7f * triangleFloat(zonePos * owlet::dsp::phi::kPhi150 * 0.85f +
		                                vibePhaseWidth_[1] + ph033 - 0.3f);
		bandWidth_[2] =
		    0.9f + 0.7f * triangleFloat(zonePos * owlet::dsp::phi::kPhi225 * 0.85f +
		                                vibePhaseWidth_[2] + ph050 - 0.3f);
		break;
	}
	default:
		bandWidth_[0] = weirdXover ? (0.5f + zonePos * 0.5f) : 0.7f;
		bandWidth_[1] = 1.0f;
		bandWidth_[2] = 1.0f;
	}

	// Knee: 0=hard, 1=soft.
	switch (zone) {
	case 0: // Width
		knee_ = 0.2f + zonePos * 0.6f;
		break;
	case 4: // Air
		knee_ = 0.6f + zonePos * 0.35f;
		break;
	case 5: // Rich
		knee_ = 0.6f + zonePos * 0.3f;
		break;
	case 3: // Punch
		knee_ = 0.05f + zonePos * 0.15f;
		break;
	case 6: // OTT
		knee_ = 0.1f + zonePos * 0.2f;
		break;
	case 7: { // OWLTT
		knee_ = 0.5f + 0.45f * triangleFloat(zonePos * owlet::dsp::phi::kPhi225 * 0.85f +
		                                     vibePhaseKnee_ + ph050 - 0.3f);
		break;
	}
	default:
		knee_ = 0.4f;
	}

	// Per-band timing offsets — A DELIBERATE DIVERGENCE FROM THE FIRMWARE (backlog EL 2026-09-16:
	// "leave timing as is for now", then "document the divergence").
	//
	// The reference (`dsp_multiband.h`) computes THIS table, zone for zone and number for number, and
	// then never reads it: nothing in its envelope path multiplies by a timing offset, so on the hardware
	// the Timing zone is inert and so is the timing half of Punch, Air, OTT and OWLTT, and of the
	// Vibe/Feel modulation below. Here the table is APPLIED — see the `timingMult` line in the gain
	// loop — so five of the eight zones change how fast each band's envelope moves. This is the
	// working version of what the firmware's table describes, kept on purpose; a parity build would
	// have to make five zones and two modulators inert to match, and nobody has asked for that.
	switch (zone) {
	case 1: // Timing
		timingOffset_[0] = -0.7f * zonePos;
		timingOffset_[1] = 0.0f;
		timingOffset_[2] = 0.7f * zonePos;
		break;
	case 3: // Punch
		timingOffset_[0] = -0.5f - zonePos * 0.3f;
		timingOffset_[1] = -0.4f - zonePos * 0.3f;
		timingOffset_[2] = -0.3f - zonePos * 0.3f;
		break;
	case 4: // Air
		timingOffset_[0] = 0.4f + zonePos * 0.3f;
		timingOffset_[1] = 0.0f;
		timingOffset_[2] = -0.6f - zonePos * 0.3f;
		break;
	case 6: // OTT
		timingOffset_[0] = -0.4f - zonePos * 0.15f;
		timingOffset_[1] = -0.5f - zonePos * 0.15f;
		timingOffset_[2] = -0.6f - zonePos * 0.15f;
		break;
	case 7: { // OWLTT
		timingOffset_[0] = 0.6f * triangleFloat(zonePos * owlet::dsp::phi::kPhi300 * 0.85f +
		                                        vibePhaseTiming_[0] + ph067 - 0.3f);
		timingOffset_[1] = 0.6f * triangleFloat(zonePos * owlet::dsp::phi::kPhi350 * 0.85f + 0.333f +
		                                        vibePhaseTiming_[1] + ph075 - 0.3f);
		timingOffset_[2] = 0.6f * triangleFloat(zonePos * owlet::dsp::phi::kPhi375 * 0.85f + 0.667f +
		                                        vibePhaseTiming_[2] + ph100 - 0.3f);
		break;
	}
	default:
		timingOffset_[0] = timingOffset_[1] = timingOffset_[2] = 0.0f;
	}

	// Per-band skew.
	switch (zone) {
	case 2: // Skew
		skewOffset_[0] = -0.85f + zonePos * 1.5f;
		skewOffset_[1] = 0.0f;
		skewOffset_[2] = 0.85f - zonePos * 1.5f;
		break;
	case 4: // Air
		skewOffset_[0] = 0.1f * zonePos;
		skewOffset_[1] = -0.4f * zonePos;
		skewOffset_[2] = -0.7f - zonePos * 0.25f;
		break;
	case 5: // Rich
		skewOffset_[0] = -0.4f - zonePos * 0.35f;
		skewOffset_[1] = -0.5f - zonePos * 0.35f;
		skewOffset_[2] = -0.3f - zonePos * 0.25f;
		break;
	case 6: // OTT
		skewOffset_[0] = 0.2f + zonePos * 0.1f;
		skewOffset_[1] = 0.0f;
		skewOffset_[2] = -0.2f - zonePos * 0.1f;
		break;
	case 7: { // OWLTT
		skewOffset_[0] = 0.9f * triangleFloat(zonePos * owlet::dsp::phi::kPhi350 * 0.85f +
		                                      vibePhaseSkew_[0] + ph075 - 0.3f);
		skewOffset_[1] = 0.9f * triangleFloat(zonePos * owlet::dsp::phi::kPhi300 * 0.85f + 0.167f +
		                                      vibePhaseSkew_[1] + ph067 - 0.3f);
		skewOffset_[2] = 0.9f * triangleFloat(zonePos * owlet::dsp::phi::kPhi375 * 0.85f + 0.333f +
		                                      vibePhaseSkew_[2] + ph100 - 0.3f);
		break;
	}
	default:
		skewOffset_[0] = skewOffset_[1] = skewOffset_[2] = 0.0f;
	}

	// Zone multipliers for vibe/feel modulation: [zone][param].
	// Params: 0-2=width, 3=knee, 4-6=timing, 7-9=skew.
	// clang-format off
	static constexpr float kVibeZoneMult[7][10] = {
		{0.45f, 0.55f, 0.65f, 0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f}, // Width
		{0.0f, 0.0f, 0.0f, 0.0f, 0.55f, 0.55f, 0.55f, 0.0f, 0.0f, 0.0f}, // Timing
		{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.65f, 0.65f, 0.65f}, // Skew
		{0.3f, 0.3f, 0.3f, 0.3f, 0.4f, 0.4f, 0.4f, 0.0f, 0.0f, 0.0f},   // Punch
		{0.3f, 0.4f, 0.55f, 0.3f, 0.4f, 0.0f, 0.4f, 0.0f, 0.4f, 0.55f}, // Air
		{0.0f, 0.0f, 0.0f, 0.4f, 0.0f, 0.0f, 0.0f, 0.55f, 0.55f, 0.4f}, // Rich
		{0.0f, 0.4f, 0.55f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.0f, 0.3f}, // OTT
	};
	static constexpr float kFeelZoneMult[7][10] = {
		{0.22f, 0.28f, 0.35f, 0.22f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f}, // Width
		{0.00f, 0.00f, 0.00f, 0.00f, 0.28f, 0.28f, 0.28f, 0.00f, 0.00f, 0.00f}, // Timing
		{0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f, 0.35f, 0.35f, 0.35f}, // Skew
		{0.15f, 0.15f, 0.15f, 0.15f, 0.22f, 0.22f, 0.22f, 0.00f, 0.00f, 0.00f}, // Punch
		{0.15f, 0.22f, 0.28f, 0.15f, 0.22f, 0.00f, 0.22f, 0.00f, 0.22f, 0.28f}, // Air
		{0.00f, 0.00f, 0.00f, 0.22f, 0.00f, 0.00f, 0.00f, 0.28f, 0.28f, 0.22f}, // Rich
		{0.00f, 0.22f, 0.28f, 0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.00f, 0.15f}, // OTT
	};
	// clang-format on

	// Apply vibe and feelPhaseOffset_ modulation to non-OWLTT zones.
	if (zone != 7) {
		// THE DEPTH READS THE LAP'S FRACTION, AS THE ZONE, THE POSITION AND THE PHASES DO. The firmware's
		// depth is `0.55 * knob` with a q31 knob that cannot pass 1, and its secret offset feeds the
		// triangle PHASES only; this device folds knob and offset into one unbounded coordinate, so the
		// raw coordinate here grew the depth without bound past lap 1 and switched it off below 0
		// (OWLTT parity sweep, 2026-09-16; Requirement: the depth cycles 0..0.55 every lap, no clamp).
		vibeModAmount_ = 0.55f * (vibeCoordinate() - std::floor(vibeCoordinate()));
		const float vibeModAmount = vibeModAmount_;
		const bool applyVibe = vibeModAmount > 0.01f;
		const bool applyFeel = std::abs(feelPhaseOffset_) > 0.01f;

		if (applyVibe || applyFeel) {
			const float* vm = kVibeZoneMult[zone];
			const float* fm = kFeelZoneMult[zone];

			const float phiTri[6] = {
			    triangleFloat(zonePos * owlet::dsp::phi::kPhi025 + ph025 - 0.3f),
			    triangleFloat(zonePos * owlet::dsp::phi::kPhi033 + ph033 - 0.3f),
			    triangleFloat(zonePos * owlet::dsp::phi::kPhi050 + ph050 - 0.3f),
			    triangleFloat(zonePos * owlet::dsp::phi::kPhi067 + ph067 - 0.3f),
			    triangleFloat(zonePos * owlet::dsp::phi::kPhi075 + ph075 - 0.3f),
			    triangleFloat(zonePos * owlet::dsp::phi::kPhi100 + ph100 - 0.3f),
			};
			const float vibeTri[10] = {
			    triangleFloat(vibePhaseWidth_[0]),          triangleFloat(vibePhaseWidth_[1] + 0.33f),
			    triangleFloat(vibePhaseWidth_[2] + 0.67f),  triangleFloat(vibePhaseKnee_),
			    triangleFloat(vibePhaseTiming_[0]),         triangleFloat(vibePhaseTiming_[1] + 0.33f),
			    triangleFloat(vibePhaseTiming_[2] + 0.67f), triangleFloat(vibePhaseSkew_[0]),
			    triangleFloat(vibePhaseSkew_[1] + 0.33f),   triangleFloat(vibePhaseSkew_[2] + 0.67f),
			};
			static constexpr int8_t kPhiIdx[10] = {0, 1, 2, 2, 3, 4, 5, 4, 3, 5};

			for (int i = 0; i < 3; i++) {
				if (applyVibe && vm[i] != 0.0f)
					bandWidth_[i] += vibeModAmount * vm[i] * vibeTri[i];
				if (applyFeel && fm[i] != 0.0f)
					bandWidth_[i] += fm[i] * phiTri[kPhiIdx[i]];
			}
			if (applyVibe && vm[3] != 0.0f)
				knee_ += vibeModAmount * vm[3] * vibeTri[3];
			if (applyFeel && fm[3] != 0.0f)
				knee_ += fm[3] * phiTri[kPhiIdx[3]];
			knee_ = std::clamp(knee_, 0.0f, 1.0f);
			for (int i = 0; i < 3; i++) {
				if (applyVibe && vm[4 + i] != 0.0f)
					timingOffset_[i] += vibeModAmount * vm[4 + i] * vibeTri[4 + i];
				if (applyFeel && fm[4 + i] != 0.0f)
					timingOffset_[i] += fm[4 + i] * phiTri[kPhiIdx[4 + i]];
			}
			for (int i = 0; i < 3; i++) {
				if (applyVibe && vm[7 + i] != 0.0f)
					skewOffset_[i] += vibeModAmount * vm[7 + i] * vibeTri[7 + i];
				if (applyFeel && fm[7 + i] != 0.0f)
					skewOffset_[i] += fm[7 + i] * phiTri[kPhiIdx[7 + i]];
			}
		}
	}

	// Update pre-computed envelope alpha from response_.
	alpha_ = 0.60f - response_ * 0.58f;
	oneMinusAlpha_ = 1.0f - alpha_;

	// Up/down skew from knob.
	upDownSkew_ = target_.upDownSkew * 2.0f - 1.0f;
}

void Owltt::processStereo(float* left, float* right, int frames) {
	// SLICED, BECAUSE THE ENGINE BELOW HAS A FIXED-SIZE STACK BUFFER. See `kSliceFrames`: the band
	// splitting `memcpy`s `frames` samples into `std::array<float, kSliceFrames>`, so a longer call
	// wrote past the end of the stack rather than returning a wrong number.
	//
	// A LOOP RATHER THAN A CLAMP, so a long call renders all of its samples instead of the first 512
	// and silence after. This device's compression is per-CALL by construction — one gain computed
	// from the whole block's envelope, ramped across it — so a sliced long call is not identical to
	// the unsliceable one it replaces; it is the same thing the shipping path already does, where
	// `Synth::render` never hands over more than 32 frames at a time.
	if (left == nullptr || right == nullptr || frames <= 0) {
		return;
	}
	// THE INPUT, READ BEFORE THE SLICES OVERWRITE IT IN PLACE — see `kRestSilentSeconds`. Only while
	// the device is enabled: a disabled OWLTT returns without touching the buffer, and reading one it
	// declined to write would be measuring the caller's audio.
	// A BYPASS CLEARS THE RUNNING STATE, ONCE (backlog EH, shape 6): nine crossovers' filters and
	// three bands' gain memories otherwise waited through the bypass and resumed mid-tail — and the
	// rest-flush below is gated on `enabled`, so it could never fire while bypassed to do it instead.
	// THE LOOKAHEAD IN FORCE (backlog ND), from the stored choice — the same function `latencySamples` answers from, so
	// the delay applied is the latency reported. A change applies at once; the host restarts after it (assumption L6).
	lookLen_ = lookCap_ > 0 ? std::min(lookaheadSamplesFor(target_.lookahead, sampleRate_), lookCap_ - 1) : 0;
	// A MODE OUTSIDE 1-10 TAKES THE BYPASS ROAD (backlog OY's aside): the slice returns on one untouched, so without
	// this it reported the lookahead's latency and delayed nothing. The descriptor cannot reach it; a raw row can.
	const long crossoverType = std::lround(target_.mode);
	if (!target_.enabled || crossoverType < 1 || crossoverType > 10) {
		if (running_) {
			running_ = false;
			reset();
		}
		// **A BYPASSED OWLTT IS STILL LATE BY ITS LOOKAHEAD** — COMP's rule (backlog LE): the host was told a latency and
		// pads everything else by it, so a bypass that stopped delaying would arrive early. The dry lanes carry the input
		// through; the low band's lanes take it too (as Mid and Side in M/S) and the other bands' silence, so a
		// re-enable reads the input back out of the line rather than up to 5 ms of whatever was there before.
		if (lookLen_ > 0) {
			for (int i = 0; i < frames; ++i) {
				const uint32_t w = lookPos_;
				const uint32_t r = (w + lookCap_ - lookLen_) % lookCap_;
				const float inL = left[i];
				const float inR = right[i];
				float* in = &look_[static_cast<size_t>(w) * kLookaheadLanes];
				const float* out = &look_[static_cast<size_t>(r) * kLookaheadLanes];
				left[i] = out[6];
				right[i] = out[7];
				in[0] = target_.midSide ? (inL + inR) * 0.5f : inL;
				in[3] = target_.midSide ? (inL - inR) * 0.5f : inR;
				in[1] = in[2] = in[4] = in[5] = 0.0f;
				in[6] = inL;
				in[7] = inR;
				lookPos_ = w + 1 == lookCap_ ? 0 : w + 1;
			}
		}
		return;
	}
	running_ = true;
	bool silentInput = true;
	// A NON-FINITE INPUT SAMPLE IS TREATED AS 0, before any detector or crossover sees it (as Comp
	// and EQ do): one NaN in a crossover or an envelope is a NaN in every later sample. Finite
	// input is untouched.
	for (int i = 0; i < frames; ++i) {
		left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
		right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
		silentInput = silentInput && left[i] == 0.0f && right[i] == 0.0f;
	}

	int done = 0;
	while (done < frames) {
		const int take = std::min(frames - done, kSliceFrames);
		blendRunAt_ = done;
		processStereoSlice(left + done, right + done, take);
		done += take;
	}
	blendRun_ = nullptr; // one call's window
	driveRun_ = nullptr;

	if (!target_.enabled) {
		return;
	}
	// **THE TAIL REACHES EXACT SILENCE INSTEAD OF PARKING IN THE DENORMALS.** See `kRestLevel`.
	if (!silentInput) {
		restSilence_ = 0;
		return;
	}
	if (restSilence_ < restSilenceNeeded_) {
		restSilence_ += frames;
		return;
	}
	float peak = 0.0f;
	bool poisoned = false;
	for (int i = 0; i < frames; ++i) {
		// `std::max` ignores a NaN, so a poisoned tail would read as silence and never flush.
		if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
			left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
			right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
			poisoned = true;
		}
		peak = std::max(peak, std::fabs(left[i]));
		peak = std::max(peak, std::fabs(right[i]));
	}
	if (!poisoned && (peak == 0.0f || peak >= kRestLevel)) {
		return;
	}
	// `reset()` HERE IS THE WHOLE STATE AND NOTHING ELSE — this device keeps no "which stage is live"
	// flags, which is exactly what made `MultimodeFilter::reset()` unusable for the same job there.
	// Checked rather than assumed: `Owltt::reset` clears the bands, the crossovers, the smoothed gains
	// and `frameCount_`, and touches no setting.
	reset();
}

void Owltt::processStereoSlice(float* left, float* right, int frames) {
	if (!target_.enabled || frames <= 0) {
		return;
	}
	sliceFrames_ = frames;

	const uint8_t crossoverType = static_cast<uint8_t>(std::round(target_.mode));
	// Mode is 1-10 (crossover type selector). Mode 0 is not valid (Enabled handles bypass).
	if (crossoverType < 1 || crossoverType > 10) {
		return;
	}

	// Character/vibe state FIRST, then the crossovers — `updateCrossoverFrequencies` hands the Twisted
	// and Twist3 splitters `vibeTwist_`, which `computeCharacterAndVibe` is what sets. The other way
	// round (as it stood until the OWLTT parity sweep, 2026-09-16) ran both on the PREVIOUS slice's
	// twist; the firmware sets the twist inside render after its vibe pass, and this is that order.
	computeCharacterAndVibe();
	updateCrossoverFrequencies();
	// BACKLOG LR: THE WIDTH EACH BAND SENDS ON — the zone's, scaled by the global Width and the band's own. At
	// 100% both scalers are exactly 1, so this is the zone's width to the bit (pinned by "LR: at Width 100%
	// everywhere ..."). The high band's averaging detector below still keys on the ZONE's width: it is a
	// character the zone chose, and the Width controls are a side gain, not a detector setting.
	{
		const std::array<float, 3> bandScale { target_.low.width, target_.mid.width, target_.high.width };
		for (size_t b = 0; b < kNumBands; ++b) {
			effectiveBandWidth_[b] = effectiveBandWidthOf(bandWidth_[b], target_.width, bandScale[b]);
		}
	}

	// THE LINKED SHIFT, FROM `owltt_types.h` RATHER THAN FROM HERE. It was four lines and a lambda in
	// this function, which meant the engine knew what a threshold IS and the picture did not -- so
	// turning the global moved the sound and left the graph's bars where they were. The
	// thresholds in the graph must match the thresholds the device uses, so they can be set by eye.
	//
	// MOVED RATHER THAN COPIED. The alternative was the UI restating this arithmetic, and a pair of
	// numbers that agree on the day they are written is what this codebase has spent a week paying
	// for. `effectiveBandOf` is the only statement of it now, and `OwlttGraph` draws from the same
	// call the engine renders from.
	const auto effLow = effectiveBandOf(target_.low, target_);
	const auto effMid = effectiveBandOf(target_.mid, target_);
	const auto effHigh = effectiveBandOf(target_.high, target_);

	// EVERY COMPRESSOR OF A BAND GETS THE BAND'S SETTINGS — the linked detector and its two channels
	// (backlog LQ), set in one order by one function, so the three cannot drift apart and Link 100% can be
	// the linked device to the bit.
	const auto configure = [this](BandCompressor& c, const EffectiveBand& eff) {
		c.setAttack(eff.attack);
		c.setRelease(eff.release);
		c.setThresholdDown(eff.threshold);
		c.setRatioDown(eff.ratioDown);
		c.setRatioUp(eff.ratioUp);
		c.setBandwidth(eff.bandwidth);
		c.setOutputLevel(eff.level);
		c.setLookaheadWindow(static_cast<float>(lookLen_)); // ND: 0 without one
	};
	const std::array<EffectiveBand, kNumBands> effective { effLow, effMid, effHigh };
	for (size_t b = 0; b < kNumBands; ++b) {
		configure(bands_[b], effective[b]);
		for (BandCompressor& channel : channels_[b]) {
			configure(channel, effective[b]);
		}
	}

	const float outputGain = outputGainFromKnob(target_.outputGain);

	// Dry buffer for blend — and whenever Blend moves inside the call, which may leave 1.
	const float* blendRun = blendRun_ != nullptr ? blendRun_ + blendRunAt_ : nullptr;
	std::array<float, static_cast<size_t>(Owltt::kSliceFrames)> dryL {}, dryR {};
	if (target_.blend < 1.0f || blendRun != nullptr || lookLen_ > 0) { // ND: the line is fed every call while on
		std::memcpy(dryL.data(), left, static_cast<size_t>(frames) * sizeof(float));
		std::memcpy(dryR.data(), right, static_cast<size_t>(frames) * sizeof(float));
	}

	// Band buffers.
	std::array<float, static_cast<size_t>(Owltt::kSliceFrames)> lowL {}, midL {}, highL {};
	std::array<float, static_cast<size_t>(Owltt::kSliceFrames)> lowR {}, midR {}, highR {};

	// **THE DRIVE (backlog NB), IN FRONT OF THE SPLIT** — the crossovers are linear, so driving the input drives every
	// band, and every detector and every gain after it hears the driven signal. THE DRY IS NOT DRIVEN: the copy above
	// and `alignDry` below both read the undriven `left`/`right`, so Blend mixes back what came in (COMP's MI rule).
	// At 0 dB the gain is `exp(0)`, exactly 1, and `x * 1.0f` is x — the device before NB to the bit. Per sample from
	// the host's window while a route moves it, like Blend; `exp` only when the value changes.
	const float* driveRun = driveRun_ != nullptr ? driveRun_ + blendRunAt_ : nullptr;
	float driveDb = target_.driveDb;
	float drive = driveGainOf(driveDb);

	// THE LR MODES' DRY, FADED TO THE RAW INPUT AT BLEND 0 (backlog OY): `alignedDryShare` of the Blend this sample
	// mixes at — the host's window while it moves, else the stored value. At share 0 it is `raw`, to the bit, so Blend 0
	// renders the input exactly as the other modes do; at 1 it is the aligned dry as before. `aligned` is computed by
	// the caller every sample regardless, so the allpass stays warm.
	const auto alignedDryAt = [&](float raw, float aligned, int i) {
		const float share = alignedDryShare(blendRun != nullptr ? std::clamp(blendRun[i], 0.0f, 1.0f) : target_.blend);
		if (share <= 0.0f) {
			return raw;
		}
		if (share >= 1.0f) {
			return aligned;
		}
		return raw + (aligned - raw) * share;
	};

	// Split into bands using the selected crossover type.
	for (int i = 0; i < frames; ++i) {
		if (driveRun != nullptr && driveRun[i] != driveDb) {
			driveDb = driveRun[i];
			drive = driveGainOf(std::clamp(driveDb, kDriveMinDb, kDriveMaxDb));
		}
		const float inL = left[i] * drive;
		const float inR = right[i] * drive;
		CrossoverBands bandsL {}, bandsR {};
		switch (crossoverType) {
		case 1: // AP 6dB
			ap1_[0].process(inL, bandsL);
			ap1_[1].process(inR, bandsR);
			break;
		case 2: // Quirky (AP2)
			ap2_[0].process(inL, bandsL);
			ap2_[1].process(inR, bandsR);
			break;
		case 3: // Twisted
			twisted_[0].process(inL, bandsL);
			twisted_[1].process(inR, bandsR);
			break;
		case 4: // Weird (AP3)
			ap3_[0].process(inL, bandsL);
			ap3_[1].process(inR, bandsR);
			break;
		// THE LINKWITZ-RILEY MODES HAND THE BLEND A DRY SIGNAL IN THEIR OWN PHASE (backlog ER). Their
		// bands sum to an ALLPASSED input, not the input — that is the price of real slopes — so a
		// wet/dry mix against the raw input cancels where the allpass reaches 180°: at each corner
		// for LR4, across the mid band for LR2. `alignDry` is the same allpass, so the mix is flat.
		// Written every sample, not only while blending, so the filter is warm when Blend leaves 1.
		case 5: // LR2 Fast
			lr2fast_[0].process(inL, bandsL);
			lr2fast_[1].process(inR, bandsR);
			dryL[i] = alignedDryAt(left[i], lr2fast_[0].alignDry(left[i]), i);
			dryR[i] = alignedDryAt(right[i], lr2fast_[1].alignDry(right[i]), i);
			break;
		case 6: // LR2 Full
			lr2full_[0].process(inL, bandsL);
			lr2full_[1].process(inR, bandsR);
			dryL[i] = alignedDryAt(left[i], lr2full_[0].alignDry(left[i]), i);
			dryR[i] = alignedDryAt(right[i], lr2full_[1].alignDry(right[i]), i);
			break;
		case 7: // LR4 Fast
			lr4fast_[0].process(inL, bandsL);
			lr4fast_[1].process(inR, bandsR);
			dryL[i] = alignedDryAt(left[i], lr4fast_[0].alignDry(left[i]), i);
			dryR[i] = alignedDryAt(right[i], lr4fast_[1].alignDry(right[i]), i);
			break;
		case 8: // LR4 Full
			lr4full_[0].process(inL, bandsL);
			lr4full_[1].process(inR, bandsR);
			dryL[i] = alignedDryAt(left[i], lr4full_[0].alignDry(left[i]), i);
			dryR[i] = alignedDryAt(right[i], lr4full_[1].alignDry(right[i]), i);
			break;
		case 9: // Inverted (AP1 with swapped low/high)
			ap1_[0].process(inL, bandsL);
			ap1_[1].process(inR, bandsR);
			std::swap(bandsL.low, bandsL.high);
			std::swap(bandsR.low, bandsR.high);
			break;
		case 10: // Twist3
			twist3_[0].process(inL, bandsL);
			twist3_[1].process(inR, bandsR);
			break;
		default:
			// Unknown type — treat as bypass.
			bandsL.low = bandsR.low = inL;
			bandsL.mid = bandsR.mid = 0.0f;
			bandsL.high = bandsR.high = 0.0f;
			break;
		}
		lowL[i] = bandsL.low;
		midL[i] = bandsL.mid;
		highL[i] = bandsL.high;
		lowR[i] = bandsR.low;
		midR[i] = bandsR.mid;
		highR[i] = bandsR.high;
	}

	// Per-band envelope detection and gain calculation.
	++frameCount_;
	const bool useAvgEnvelope = bandWidth_[2] > 1.0f;

	// Band buffer pointers for level update.
	const std::array<float*, 3> bandL = {lowL.data(), midL.data(), highL.data()};
	const std::array<float*, 3> bandR = {lowR.data(), midR.data(), highR.data()};

	// **EACH BAND'S INPUT LEVEL (the per-band rebuild)**, AFTER THE SPLIT AND BEFORE THE DETECTOR: the band's audio is
	// scaled, so its detector, its gain computer and the signal the gain is applied to all hear it. A one-pole per
	// sample toward the target, so a modulated level does not step. At 0 dB the target is exactly 1 and the gain
	// starts there, so the band is left alone and no multiply runs: the device before it, to the bit.
	for (size_t b = 0; b < 3; ++b) {
		const float target = driveGainOf(target_.band(b).inputDb);
		inputGainTarget_[b] = target;
		if (inputGain_[b] == 1.0f && target == 1.0f) {
			continue;
		}
		float g = inputGain_[b];
		for (int i = 0; i < frames; ++i) {
			g += (target - g) * gainSmoothAlpha_;
			bandL[b][i] *= g;
			bandR[b][i] *= g;
		}
		if (std::abs(target - g) < 1.0e-4f * target) { // the one-pole stalls a few ulps short; snap
			g = target;
		}
		inputGain_[b] = g;
	}

	// **IN M/S (backlog LQ) EACH BAND IS ENCODED HERE, IN PLACE, AFTER ITS WIDTH**: `bandL` becomes the band's
	// Mid and `bandR` its Side scaled by the band's width — the same `mid` and `sideScaled` the L/R road below
	// computes per sample, moved ahead of detection. So Side's detector hears the side the band will actually
	// send on (the Character zone's narrowing included), and the apply loop gains Mid and Side and decodes.
	// THE ORDER, STATED: split, width, detect, gain, decode. In L/R the detectors keep reading the band BEFORE
	// its width, as they always have — moving them would change the default sound.
	const bool midSide = target_.midSide;
	if (midSide) {
		for (size_t b = 0; b < 3; ++b) {
			const float width = effectiveBandWidth_[b];
			for (int i = 0; i < frames; ++i) {
				const float mid = (bandL[b][i] + bandR[b][i]) * 0.5f;
				const float side = (bandL[b][i] - bandR[b][i]) * 0.5f;
				bandL[b][i] = mid;
				bandR[b][i] = side * width;
			}
		}
	}
	const float link = std::clamp(target_.stereoLink, 0.0f, 1.0f);

	std::array<std::array<float, 2>, 3> channelGains {};
	for (size_t b = 0; b < 3; ++b) {
		// THE DETECTOR'S SMOOTHING, ADVANCED BY THE BLOCK'S LENGTH rather than by one step per call.
		//
		// `alpha_` is a per-CALL coefficient — `level_ = level_ * (1 - a) + peak * a`, once per
		// `processStereo`. So at a 32-sample chunk the detector smoothed EIGHT TIMES faster than at
		// 256, and the compressor's whole response tracked the host's buffer size. This is the same
		// correction `runEnvelope` already makes for the attack and release: a one-pole advanced by n
		// samples is `1 - (1 - a)^n`, and dividing n by `kReferenceFrames` is what makes a 256-sample
		// call reproduce exactly what it did before.
		//
		// ONCE PER CALL, NOT PER SAMPLE. `std::pow` here is one call per band per block against a
		// block of crossover filtering; it allocates nothing, which is the rule that matters on this
		// thread.
		const float levelAlpha =
		    1.0f
		    - std::pow(oneMinusAlpha_,
		               static_cast<float>(frames) / static_cast<float>(kReferenceFrames));
		// THE BAND'S LINKED DETECTOR — the louder channel (or the two's mean), exactly as before LQ. It is
		// the `shared` level Link blends toward, and its own envelope still runs for the meters' seams.
		// **IN M/S IT IS ALWAYS THE LOUDER OF MID AND SIDE, never their mean.** The mean is an L/R answer to a
		// wide high band (`useAvgEnvelope`); applied to Mid and Side it would read a mono signal (Side 0) at
		// half its level, 6 dB under what L/R reads it at, so switching to M/S would move every threshold.
		bands_[b].updateLevel(bandL[b], bandR[b], static_cast<size_t>(frames), levelAlpha,
		                      1.0f - levelAlpha, useAvgEnvelope && !midSide, kPeakStride[b]);
		const float bandSkew = std::clamp(upDownSkew_ + skewOffset_[b], -1.0f, 1.0f);
		// Apply per-band timing offset as a multiplier on the envelope time (numSamples): −0.7 is about
		// 3x faster, +0.7 is 1.7x slower. THE FIRMWARE DOES NOT HAVE THIS LINE — it fills the same table
		// and leaves it unread (see the zone switch in `updateCharacter`). Kept live deliberately.
		const float timingMult = 1.0f + timingOffset_[b];
		const float effectiveNumSamples = static_cast<float>(frames) * timingMult;
		(void)bands_[b].calculateGain(effectiveNumSamples, kThresholdRefdB, knee_, bandSkew, frameCount_);
		const float shared = bands_[b].getInputLevelLog();
		// **EACH CHANNEL'S OWN DETECTOR, THEN LINK'S BLEND, THEN ITS OWN GAIN COMPUTER.** A channel's detector
		// is handed its one buffer twice, so `max(|x|, |x|)` and `(|x| + |x|) / 2` are both `|x|` exactly. The
		// blend is in the detector's log units (nepers, so dB), in the two-ended form that is EXACT at both
		// ends: at Link 1 it is `0 * own + shared`, the linked level to the bit, so both channels run the
		// linked compressor's trajectory and the device is the one it was before LQ (pinned by "LQ: at the
		// default stereo settings OWLTT renders bit-identically"); at Link 0 it is the channel's own.
		const std::array<const float*, 2> buffers { bandL[b], bandR[b] };
		for (size_t c = 0; c < 2; ++c) {
			BandCompressor& channel = channels_[b][c];
			channel.updateLevel(buffers[c], buffers[c], static_cast<size_t>(frames), levelAlpha, 1.0f - levelAlpha,
			                    useAvgEnvelope, kPeakStride[b]);
			channel.setInputLevelLog((1.0f - link) * channel.getInputLevelLog() + link * shared);
			channelGains[b][c] =
			    channel.calculateGain(effectiveNumSamples, kThresholdRefdB, knee_, bandSkew, frameCount_);
		}
	}

	// **THE LOOKAHEAD (backlog ND): THE DETECTORS HAVE HEARD THIS CALL; THE AUDIO THEY GAIN IS NOW HELD BACK.** Every band
	// and the dry go through one delay at one position, so the bands stay aligned with each other and with Blend's dry.
	// The gains computed above are applied to audio `lookLen_` samples older, so an onset meets a gain already falling.
	lookaheadStep({ bandL[0], bandL[1], bandL[2], bandR[0], bandR[1], bandR[2], dryL.data(), dryR.data() }, frames);

	// Compute target gains (band gain * per-band output level), per channel.
	std::array<std::array<float, 2>, 3> targetBandGain {};
	for (size_t b = 0; b < 3; ++b) {
		// CEILINGED AT 256x (+48 dB), the firmware's `kGainQ22MaxBand`: its Q9.22 gain could hold no
		// more, and a float that can hold 31.6 x 10 = 316x was quietly louder than the reference at
		// the extremes of Ratio and Level (OWLTT parity sweep, 2026-09-16).
		for (size_t c = 0; c < 2; ++c) {
			targetBandGain[b][c] = std::min(channelGains[b][c] * bands_[b].getOutputLevelLinear(), kMaxBandGain);
		}
	}
	const float targetOutputGain = outputGain;

	// RECORDED FOR THE TEST SEAM, and for nothing else — see `lastTargetBandGainForTest`.
	lastTargetBandGain_ = targetBandGain;

	// A ONE-POLE PER SAMPLE AT A TIME CONSTANT IN SECONDS, not a linear ramp across the call.
	//
	// THE OLD FORM WAS `(target - smoothed) / frames` PER SAMPLE, which reached the target exactly at
	// the end of whatever block it was handed — so the smoothing time was the host's chunk length and
	// the device rendered audibly differently at different buffer sizes. See `kGainSmoothingSeconds`
	// for the measurement and for why 5.333 ms is the number.
	const float alpha = gainSmoothAlpha_;
	// **UNDER A LOOKAHEAD A FALLING GAIN SETTLES INSIDE THE WINDOW (backlog ND, option 3)**: the same one-pole, at no
	// slower than `lookLen_ / kLookaheadSettle` while it falls; a rising gain keeps the 5.333 ms. Without one,
	// `alphaDown` IS `alpha` and the line below is the line it always was.
	const float alphaDown = lookLen_ > 0
	                            ? std::max(alpha, 1.0f - std::exp(-BandCompressor::kLookaheadSettle / static_cast<float>(lookLen_)))
	                            : alpha;

	// Fused gain apply + M/S width + soft clip + recombine loop.
	for (int i = 0; i < frames; ++i) {
		for (size_t b = 0; b < 3; ++b) {
			for (size_t c = 0; c < 2; ++c) {
				const float step = targetBandGain[b][c] < smoothedGain_[b][c] ? alphaDown : alpha;
				smoothedGain_[b][c] += (targetBandGain[b][c] - smoothedGain_[b][c]) * step;
			}
		}
		smoothedOutputGain_ += (targetOutputGain - smoothedOutputGain_) * alpha;

		float sumL = 0.0f, sumR = 0.0f;

		for (size_t b = 0; b < 3; ++b) {
			float L = bandL[b][i];
			float R = bandR[b][i];

			if (!midSide) {
				// M/S width processing with per-band width.
				const float width = effectiveBandWidth_[b];
				const float mid = (L + R) * 0.5f;
				const float side = (L - R) * 0.5f;
				const float sideScaled = side * width;
				L = mid + sideScaled;
				R = mid - sideScaled;

				// Apply smoothed gain + track post-gain peak (firmware: bandPeakThisBuffer). Each channel its
				// own (backlog LQ); at Link 100% the two are one number.
				L *= smoothedGain_[b][0];
				R *= smoothedGain_[b][1];
			}
			else {
				// THE BAND IS ALREADY MID AND WIDTH-SCALED SIDE (encoded above): gain each, then decode.
				const float mid = L * smoothedGain_[b][0];
				const float side = R * smoothedGain_[b][1];
				L = mid + side;
				R = mid - side;
			}

			// Soft clip per band — under the switch (backlog EL): the firmware's `softClipEnabled_`.
			if (target_.softClip && target_.clipOnly != 2) { // the band stage: Bands and Both, not Master
				L = softClip(L, kBandClipKnee);
				R = softClip(R, kBandClipKnee);
			}
			// METERED AFTER THE CLIPPER, as the firmware meters it (`multiband.h`, the band peak is taken
			// off the clipped `scaledL0`): the meter shows what the band SENDS ON, and the clip indicator
			// downstream cannot fire on a peak the clipper already removed. It was read before the clip
			// until the OWLTT parity sweep, 2026-09-16.
			bands_[b].updatePostGainPeak(std::max(std::abs(L), std::abs(R)));

			sumL += L;
			sumR += R;
		}

		// Output gain.
		sumL *= smoothedOutputGain_;
		sumR *= smoothedOutputGain_;

		// Soft clip at output — under the same switch as the bands.
		if (target_.softClip && target_.clipOnly != 1) { // the sum stage: Master and Both, not Bands
			sumL = softClip(sumL, kOutputClipKnee);
			sumR = softClip(sumR, kOutputClipKnee);
		}

		// Track output peak (firmware: peakThisBuffer) — AFTER the clipper, where the firmware reads
		// `outLVec` (OWLTT parity sweep, 2026-09-16). The peak is what leaves the device.
		outputPeakThisBuffer_ = std::max(outputPeakThisBuffer_, std::max(std::abs(sumL), std::abs(sumR)));

		// Wet/dry blend — per sample while it moves (`setBlendRun`).
		if (blendRun != nullptr) {
			const float b = std::clamp(blendRun[i], 0.0f, 1.0f);
			left[i] = sumL * b + dryL[i] * (1.0f - b);
			right[i] = sumR * b + dryR[i] * (1.0f - b);
		}
		else if (target_.blend < 1.0f) {
			left[i] = sumL * target_.blend + dryL[i] * (1.0f - target_.blend);
			right[i] = sumR * target_.blend + dryR[i] * (1.0f - target_.blend);
		}
		else {
			left[i] = sumL;
			right[i] = sumR;
		}
	}

	// **NO SNAP AT THE END OF THE CALL.** This used to read "snap smoothed gains to target after the
	// ramp completes", which was correct for a ramp that ARRIVED — and it is what kept the smoothing
	// tied to the block after the ramp itself was replaced by a one-pole. Leaving it in would have
	// made `kGainSmoothingSeconds` a lie: the gain would still have landed on its target at every
	// call boundary, so the time constant would still have been the chunk length and only the shape
	// of the approach would have changed. Found by measuring the fix and finding it had barely moved
	// the number.

	// Decay per-band output peaks (firmware: bandOutputPeak_ = max(bandPeakThisBuffer, bandOutputPeak_ * 0.95)).
	for (auto& band : bands_) {
		band.decayBandOutputPeak(static_cast<size_t>(frames));
	}

	// **THE MASTER PEAK DECAYS HERE, NOT INSIDE `updateMeters`, AND THAT IS A FIX RATHER THAN A MOVE.**
	//
	// `updateMeters` opens with `if (!meterCallback_) return;`, and this decay — with the reset of
	// `outputPeakThisBuffer_` that goes with it — used to sit after that gate. But
	// `outputPeakThisBuffer_` is written in the SAMPLE LOOP unconditionally, so with no editor open it
	// is a running maximum that is never cleared: it accumulates for the life of the plugin, and the
	// first meter update after somebody opens the editor reports the loudest thing since load rather
	// than the current level. The band peaks were never exposed to this, because
	// `decayBandOutputPeak` was already called from here.
	//
	// AND THE RATE IS IN SAMPLES, which is the other half. `0.95` per CALL is a half-life of 9 ms at a
	// 32-sample block and 148 ms at 512 — sixteen times slower for the same signal, decided by the
	// host's buffer size. Same reference block as the gain smoother and the detector, so all three of
	// this device's ported per-block rates agree about what "today" means.
	{
		const double keep = std::pow(0.95, static_cast<double>(frames) / 256.0);
		outputPeak_ = std::max(outputPeakThisBuffer_, outputPeak_ * static_cast<float>(keep));
	}
	outputPeakThisBuffer_ = 0.0f;

	updateMeters(frames);
}

float Owltt::channelGainReductionForTest(size_t band, size_t channel) const {
	return channels_[band][std::min<size_t>(channel, 1)].getGainReductionDB();
}

float Owltt::channelInputLevelLogForTest(size_t band, size_t channel) const {
	return channels_[band][std::min<size_t>(channel, 1)].getInputLevelLog();
}

void Owltt::setSlot(size_t slot) {
	slot_ = slot;
}

void Owltt::setMeterCallback(MeterCallback callback) {
	meterCallback_ = std::move(callback);
}

void Owltt::updateMeters(int frames) {
	if (!meterCallback_) {
		return;
	}
	// FIRMWARE'S METER SCALING (multiband.h:1864-1910). Ported exactly — these are not arbitrary numbers.
	//
	// Per-band GR: envelope * 8.686 converts natural log to dB. Noise floor at |envelope| < 0.012 (~0.1dB).
	//   Scale ±12dB to ±127, preserve sign (negative = downward = bars down).
	// Per-band input level: dBFS = 8.686 * (rms_ - 21.5). Noise floor at -48dBFS.
	//   Linear map [-48, 0] dBFS to [0, 127].
	// Per-band output level: peak after band output level, normalized to EFFECTIVE_0DBFS.
	//   Same dBFS scale as input.
	// Output peak: max(peakThisBuffer, outputPeak_ * 0.95) decay.
	// Clipping: hold timer at kIndicatorHoldBuffers=170, refreshed every kMeterRefreshBuffers=35.

	constexpr float kRefLevel = 21.5f; // log(2^31)
	constexpr float kNoiseFloorEnvelope = 0.012f; // ~0.1dB in natural log units
	constexpr float kNoiseFloorDBFS = -48.0f;
	constexpr float kScaleGR = 127.0f / 12.0f; // ±12dB → ±127
	constexpr float kScaleLevel = 127.0f / 48.0f; // [-48, 0] dBFS → [0, 127]

	std::array<float, 3> grDB {};
	std::array<float, 3> grSecond {};
	std::array<float, 3> inputLevelDB {};
	std::array<float, 3> outputLevelDB {};
	for (size_t b = 0; b < 3; ++b) {
		// Per-band GR with noise floor and ±12dB scaling.
		// EACH CHANNEL'S OWN (backlog LQ): the first is what readings 0..2 always carried, the second goes out
		// beside it as 14..16. At Link 100% the two are the linked envelope, so nothing a reader saw moved.
		const auto scaledGr = [&](float envelope) {
			if (std::abs(envelope) < kNoiseFloorEnvelope) {
				return 0.0f;
			}
			const float gr = envelope * 8.686f; // natural log → dB
			return std::clamp(gr * kScaleGR, -127.0f, 127.0f) / 127.0f; // Normalize to ±1.0
		};
		grDB[b] = scaledGr(channels_[b][0].getGainReductionDB());
		grSecond[b] = scaledGr(channels_[b][1].getGainReductionDB());

		// Per-band input level: dBFS-linear scale.
		float dBFS = 8.686f * (bands_[b].getInputLevelLog() - kRefLevel);
		if (dBFS < kNoiseFloorDBFS) {
			inputLevelDB[b] = 0.0f;
		}
		else {
			inputLevelDB[b] = std::clamp((dBFS - kNoiseFloorDBFS) * kScaleLevel / 127.0f, 0.0f, 1.0f);
		}

		// Per-band output level: post-level peak, same dBFS scale.
		float peak = bands_[b].getBandOutputPeak();
		if (peak < 1000.0f / 2147483648.0f) { // Noise floor
			outputLevelDB[b] = 0.0f;
		}
		else {
			float peakDB = 8.686f * std::log(peak + 1e-10f);
			outputLevelDB[b] = std::clamp((peakDB - kNoiseFloorDBFS) * kScaleLevel / 127.0f, 0.0f, 1.0f);
		}
	}


	// Clipping detection with hold timer.
	// COUNTED IN SAMPLES, NOT IN CALLS. See `meterRefreshPeriodSamples_`.
	meterRefreshSamples_ += frames;
	if (meterRefreshSamples_ >= meterRefreshPeriodSamples_) {
		meterRefreshSamples_ = 0;
		constexpr float kClipThreshold = 1.0f; // Normalized to full scale
		if (outputPeak_ > kClipThreshold) {
			clippingHoldCounter_ = kIndicatorHoldBuffers / kMeterRefreshBuffers;
		}
		else if (clippingHoldCounter_ > 0) {
			clippingHoldCounter_--;
		}
		clipping_ = (clippingHoldCounter_ > 0);
		// AND EACH BAND'S OWN LIGHT (backlog EL), on the same grid with the same hold: the firmware's
		// `bandSaturating_` reads the band's post-clip peak against its 0 dBFS reference, which is
		// half the band knee here — so the light says "this band's clipper is working".
		for (size_t b = 0; b < 3; ++b) {
			if (bands_[b].getBandOutputPeak() > kBandSaturationThreshold) {
				bandSaturationHold_[b] = kIndicatorHoldBuffers / kMeterRefreshBuffers;
			}
			else if (bandSaturationHold_[b] > 0) {
				bandSaturationHold_[b]--;
			}
			bandSaturating_[b] = bandSaturationHold_[b] > 0;
		}
	}

	// THE COMBINED GAIN REDUCTION, every block: the mean |gain change| of the three bands in dB — the
	// firmware's `getGainReduction()` before its 0..127 scaling. Read off the same envelopes the bars
	// draw, so the readout and the bars cannot disagree.
	{
		float total = 0.0f;
		for (size_t b = 0; b < 3; ++b) {
			// THE DEEPER CHANNEL OF EACH BAND — the linked envelope at Link 100%, as before LQ.
			total += std::max(std::abs(channels_[b][0].getGainReductionDB() * 8.686f),
			                  std::abs(channels_[b][1].getGainReductionDB() * 8.686f));
		}
		combinedGainReductionDb_ = total / 3.0f;
	}
	const MeterExtras extras { bandSaturating_, combinedGainReductionDb_, grSecond, static_cast<float>(noteHzReading()) };
	meterCallback_(slot_, grDB, inputLevelDB, outputLevelDB, clipping_, extras);
}

// ========================================================================
// AudioOps: how the host runs this device
// ========================================================================

/// THE CONTROLS READ PER SAMPLE WHEN MODULATED — see `AudioOps::windowedControls`.
constexpr std::array<uint8_t, 2> kWindowedControls { static_cast<uint8_t>(Param::Blend), static_cast<uint8_t>(Param::Drive) };

const device::AudioOps kAudioOps {
	.stateBytes = sizeof(Owltt),
	.stateAlign = alignof(Owltt),
	.construct = [](void* storage) { new (storage) Owltt(); },
	.destruct = [](void* storage) { static_cast<Owltt*>(storage)->~Owltt(); },
	.prepare = [](void* storage, double sampleRate, int) { static_cast<Owltt*>(storage)->prepare(sampleRate); },
	// THE HOST'S RESET EMPTIES THE LOOKAHEAD TOO (backlog ND); the device's own resets keep it — see `clearLookahead`.
	.reset =
	    [](void* storage) {
		    static_cast<Owltt*>(storage)->reset();
		    static_cast<Owltt*>(storage)->clearLookahead();
		    static_cast<Owltt*>(storage)->forgetNote(); // KEY TRACK: "as if just switched on" — no note yet
	    },
	.setSlot = [](void* storage, size_t slot) { static_cast<Owltt*>(storage)->setSlot(slot); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    Row row {};
		    const size_t take = std::min(fieldsPresent, row.size());
		    for (size_t i = 0; i < take; ++i) {
			    row[i] = fields[i];
		    }
		    // A ROW FROM BEFORE LQ STOPS SHORT OF THE STEREO PAIR, and a zero there would be Link 0% — two
		    // independent compressors where the patch had one linked one. They take their DECLARED defaults
		    // instead (device_row.h's rule), which is the linked device. Only these two: every field before
		    // them has always read an absent value as zero here, and changing that is not LQ's business.
		    // BACKLOG LR'S FOUR WIDTHS, LIKEWISE: a zero there is MONO, so a short row takes 100%, the zone's own width.
		    // THE PER-BAND REBUILD'S TWELVE TRIMS AND OFFSETS, TOO: a zero is a step away from neutral, so a short row
		    // takes the declared (neutral) default.
		    for (const Param p : { Param::StereoLink, Param::StereoMode, Param::Width, Param::LowWidth, Param::MidWidth,
		                           Param::HighWidth, Param::LowUpRatio, Param::LowDownRatio, Param::MidUpRatio,
		                           Param::MidDownRatio, Param::HighUpRatio, Param::HighDownRatio, Param::LowAttack,
		                           Param::LowRelease, Param::MidAttack, Param::MidRelease, Param::HighAttack,
		                           Param::HighRelease }) {
			    const auto at = static_cast<size_t>(p);
			    if (at >= take) {
				    row[at] = static_cast<float>(kParamDescs[at].def);
			    }
		    }
		    static_cast<Owltt*>(storage)->setTarget(stateOf(row));
	    },
	.setNoteContext = [](void* storage, const device::NoteContext& note) { static_cast<Owltt*>(storage)->setNoteContext(note); },
	.processStereo = [](void* storage, float* left, float* right, int frames) {
		static_cast<Owltt*>(storage)->processStereo(left, right, frames);
	},
	.processMono = nullptr, // Stereo only
	// THE HOST'S EAR FOR THE METERS (`device::ReadingsSink`): the ten readings a OWLTT surface reads back —
	// three bands of gain reduction, input level and output level, then clipping — packed and handed on
	// every block. The callback captures the sink by value (two words) so it never allocates.
	.setReadingsSink = [](void* storage, device::ReadingsSink sink) {
		// SEVENTEEN READINGS SINCE BACKLOG LQ: the ten above, then a light per band (10..12), the combined gain
		// reduction in dB (13) — backlog EL — and each band's SECOND channel's gain reduction (14..16: R, or S
		// in M/S), readings 0..2 being the first's. `OwlttGraph::meters` reads them back in this order.
		// AND THE EIGHTEENTH, KEY TRACK'S NOTE (17): the note's Hz while it places the crossovers, 0 while it does not —
		// the Phaser's `NoteHz`, what the knobs' readout and the graph ask. It rides in the extras, as every road does.
		static_cast<Owltt*>(storage)->setMeterCallback([sink](size_t, const std::array<float, 3>& grDB,
		                                                     const std::array<float, 3>& inputLevelDB,
		                                                     const std::array<float, 3>& outputLevelDB, bool clipping,
		                                                     const Owltt::MeterExtras& extras) {
			float values[Owltt::kReadingCount];
			for (size_t b = 0; b < 3; ++b) {
				values[b] = grDB[b];
				values[3 + b] = inputLevelDB[b];
				values[6 + b] = outputLevelDB[b];
				values[10 + b] = extras.bandSaturating[b] ? 1.0f : 0.0f;
				values[14 + b] = extras.secondChannelGr[b];
			}
			values[9] = clipping ? 1.0f : 0.0f;
			values[13] = extras.combinedGrDb;
			values[17] = extras.noteHz;
			if (sink.publish != nullptr) {
				sink.publish(sink.context, values, Owltt::kReadingCount);
			}
		});
	},
	// BLEND IS READ PER SAMPLE WHILE IT MOVES (backlog FK) — the wet/dry line is per sample already, so the window costs
	// nothing extra. The crossovers and the width are coefficients, left on the tick. DRIVE TOO (backlog NB): a gain in
	// front of the split, so a routed Drive stepping once per tick would be a zipper on the whole input.
	.windowedControls = kWindowedControls,
	.setFieldWindows =
	    [](void* storage, const float* const* runs, size_t count, int) {
		    const auto blend = static_cast<size_t>(Param::Blend);
		    static_cast<Owltt*>(storage)->setBlendRun(blend < count ? runs[blend] : nullptr);
		    const auto drive = static_cast<size_t>(Param::Drive);
		    static_cast<Owltt*>(storage)->setDriveRun(drive < count ? runs[drive] : nullptr);
	    },
	// **THE LOOKAHEAD IS THE LATENCY (backlog ND)**, from the STORED choice (L1) through the function the render delays by,
	// enabled or not (a bypass still delays). Off, or a row from before ND, is 0 — the device before ND.
	.latencySamples = [](const void*, const float* fields, size_t fieldsPresent, double sampleRate) -> uint32_t {
		const auto at = static_cast<size_t>(Param::Lookahead);
		if (fields == nullptr || at >= fieldsPresent) {
			return 0;
		}
		const long choice = std::clamp(std::lround(fields[at]), 0L, static_cast<long>(kLookaheadChoices - 1));
		return lookaheadSamplesFor(static_cast<uint32_t>(choice), sampleRate);
	},
};

//--------------------------------------------------------------------------------------------------//
// KEY TRACK                                                                                         //
//--------------------------------------------------------------------------------------------------//

void Owltt::setNoteContext(const device::NoteContext& note) noexcept {
	// LATCHED ONLY WHILE A KEY IS HELD, so the crossovers hold the last note after the release — the Phaser's rule and
	// its reason (`fx::phaser::Phaser::setNoteContext`: frozen at the release; a bend in a release tail is ignored).
	if (!note.gate || !std::isfinite(note.pitchSemitones)) {
		return;
	}
	heardNote_ = true;
	noteSemitones_ = static_cast<double>(note.pitchSemitones);
}

void Owltt::forgetNote() noexcept {
	heardNote_ = false;
	keyPrimed_ = false;
}

double Owltt::noteHzReading() const noexcept { return target_.keyTrack && heardNote_ ? keyHzOf(noteSemitones_) : 0.0; }

namespace {

bool viewTracking(const device::TextView& view) noexcept {
	constexpr size_t kNoteHz = 17;
	return view.readingCount != nullptr && view.reading != nullptr && view.readingCount(view.host) > kNoteHz
	       && view.reading(view.host, kNoteHz) != 0.0f;
}

bool isCrossover(size_t control) noexcept {
	return control == static_cast<size_t>(Param::LowCrossover) || control == static_cast<size_t>(Param::HighCrossover);
}

} // namespace

/// THE PER-BAND TRIMS AND OFFSETS, stored 0..1 with 0.5 meaning none: they read and type as signed percentages from the
/// centre ("0", "+25%", "-40%"), and the stored value and key are unchanged.
bool isCentred(size_t control) noexcept {
	return (control >= static_cast<size_t>(Param::LowUpRatio) && control <= static_cast<size_t>(Param::HighRelease));
}

const device::TextOps kTextOps {
	.format = [](size_t control, double value, const device::TextView& view, char* out, size_t size) {
		if (isCentred(control)) {
			const double pct = (value - 0.5) * 100.0;
			if (std::fabs(pct) < 0.5) {
				std::snprintf(out, size, "0");
			}
			else {
				std::snprintf(out, size, "%+.0f%%", pct);
			}
			return true;
		}
		if (!isCrossover(control) || !viewTracking(view)) {
			return false;
		}
		const double raw = crossoverKeyOffsetSemitones(value);
		const double st = std::fabs(raw) < 0.05 ? 0.0 : raw;
		std::snprintf(out, size, st == 0.0 ? "%.1f st" : "%+.1f st", st);
		return true;
	},
	.parse = [](size_t control, const char* text, const device::TextView& view, double* value) {
		if (isCentred(control)) {
			if (text == nullptr) {
				return false;
			}
			char* end = nullptr;
			const double pct = std::strtod(text, &end);
			if (end == text || !std::isfinite(pct)) {
				return false;
			}
			while (*end == ' ') {
				++end;
			}
			if (*end == '%') {
				++end;
			}
			if (*end != '\0') {
				return false;
			}
			*value = std::clamp(0.5 + pct / 100.0, 0.0, 1.0);
			return true;
		}
		if (!isCrossover(control) || !viewTracking(view) || text == nullptr) {
			return false;
		}
		const char* s = text;
		while (*s == ' ') {
			++s;
		}
		char* end = nullptr;
		const double st = std::strtod(s, &end);
		if (end == s || !std::isfinite(st)) {
			return false;
		}
		s = end;
		while (*s == ' ') {
			++s;
		}
		if (std::strncmp(s, "st", 2) == 0) {
			s += 2;
		}
		while (*s == ' ') {
			++s;
		}
		if (*s != '\0') {
			return false;
		}
		*value = kCrossoverKeyRefHz * std::exp2(st / 12.0);
		return true;
	},
};

} // namespace feathers::fx::owltt
