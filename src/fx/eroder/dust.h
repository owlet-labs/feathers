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

#include "fx/eroder/drive_stage.h"
#include "fx/eroder/eroder_types.h"
#include "fx/sklitter/ring.h"

#include <cstdint>

namespace feathers::fx::eroder {

/// **DUST: THE SIGNAL ITSELF THROUGH ONE VERY SHORT DELAY WHOSE READ HEAD IS SHAKEN.** Where the Comb is a resonant
/// line added to the dry, this is the dry REPLACED by a copy of itself read a few hundred microseconds late, at a
/// distance that a band of noise, a sine, or a value held between the input's zero crossings keeps moving. One
/// quantity per control (`deriveDust` in `eroder_types.h`): how far the head travels, where the shaking sits in the
/// spectrum, how wide that band is.
///
/// **THE HEAD ONLY EVER MOVES LATER**: the delay runs from `kDustMarginFrames` to that plus twice the Amount, so the
/// dry needs no delay of its own and the device has no latency to report. Below 100% Mix the dry and the shaken copy
/// are blended a moving fraction of a millisecond apart.
///
/// Everything here is stated in seconds and hertz and the noise is normalised to its band at the running rate, so the
/// head's excursion and the band it moves in do not depend on the host's sample rate.
class Dust {
public:
	/// ALLOCATE. Activate-time only.
	void prepare(double sampleRate);
	/// AS IF JUST SWITCHED ON: the line, the filters, the oscillator, the held value, the noise stream, the smoothers.
	void reset() noexcept;

	void setPitch(double semitones) noexcept { pitchSemitones_ = semitones; }
	/// THE INPUT'S LEVEL AS THE DEVICE FOLLOWS IT, for the frames about to be processed (`Param::Follow`).
	void setEnvelope(float envelope) noexcept { envelope_ = envelope; }

	void process(const Settings& settings, float* left, float* right, int frames) noexcept;

	/// WHETHER THE LINE STILL HOLDS SOUND THE HEAD WILL READ.
	[[nodiscard]] bool holdsSound() const noexcept { return line_.holdsSound(); }

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — a claim about a value cannot be inferred from a rendered buffer.                  //
	//----------------------------------------------------------------------------------------------//

	[[nodiscard]] const DustDerived& derivedForTest() const noexcept { return derived_; }
	/// WHERE EACH CHANNEL'S READ HEAD WAS ON THE LAST SAMPLE, in frames behind the newest write.
	[[nodiscard]] double lastDelayFramesForTest(int channel) const noexcept { return lastDelayFrames_[channel & 1]; }
	/// THE MODULATOR'S LAST VALUE PER CHANNEL, after the blend and the clamp: -1..1.
	[[nodiscard]] float lastModForTest(int channel) const noexcept { return lastMod_[channel & 1]; }
	/// THE BAND NOISE ALONE, normalised, before the blend with the sine and the hold.
	[[nodiscard]] float lastNoiseForTest(int channel) const noexcept { return lastNoise_[channel & 1]; }
	[[nodiscard]] uint64_t holdsForTest() const noexcept { return holds_; }
	[[nodiscard]] float smoothedAmountFramesForTest() const noexcept { return amountFrames_; }
	[[nodiscard]] float followGainForTest() const noexcept { return followGain_; }
	[[nodiscard]] float lastRingForTest(int channel) const noexcept { return lastRing_[channel & 1]; }
	/// THE LEFT SIDE'S LAST FRAME AT THREE POINTS: what the head read, what the shaper made of it, and what was fed
	/// back round the line.
	[[nodiscard]] float lastReadForTest() const noexcept { return lastRead_; }
	[[nodiscard]] float lastShapedForTest() const noexcept { return lastShaped_; }
	[[nodiscard]] float lastTapForTest() const noexcept { return lastTap_; }

private:
	void tick(const Settings& settings) noexcept;
	void renderChunk(float* left, float* right, int frames) noexcept;
	[[nodiscard]] float nextNoise() noexcept;

	fx::sklitter::Ring line_ {};
	double sampleRate_ = 48000.0;
	uint64_t writeCounter_ = 0;
	double pitchSemitones_ = kReferenceNote;

	DustDerived derived_ {};
	/// FRAMES LEFT IN THE CURRENT CONTROL BLOCK, carried across calls: the grid is the device's, not the host's.
	int untilTick_ = 0;
	/// False until the first block, which SNAPS the smoothers to the controls.
	bool placed_ = false;

	// PER-SAMPLE SMOOTHERS, one pole each by a TIME (`kDustSmoothingSeconds`), so a partition cannot move them.
	float smoothing_ = 1.0f;
	float amountFrames_ = 0.0f;
	float feedback_ = 0.0f;
	float wetGain_ = 0.0f;
	float dryGain_ = 1.0f;

	/// THE NOISE BAND: two state-variable bandpasses in series a channel (left's pair, then right's), trapezoidal,
	/// so they are stable to the top of the range.
	struct Band {
		float ic1 = 0.0f;
		float ic2 = 0.0f;
	};
	Band bands_[4] = {};
	double sinePhase_ = 0.0;

	/// THE HELD VALUE: re-drawn whenever the input's mid (left plus right) changes sign.
	float held_ = 0.0f;
	float prevMid_ = 0.0f;

	float feedbackL_ = 0.0f;
	float feedbackR_ = 0.0f;

	/// THE SHAPER (`drive_stage.h`).
	DriveStage shaper_ {};
	/// HP WET, and the same filter on the dry for Sub Thru (`drive_stage.h`).
	WetHighpass highpass_ {};
	/// THE SHAKE'S EXTRAS: the right sine's own phase while it is detuned, and Step's hold.
	double sinePhaseRight_ = 0.0;
	double stepPhase_ = 0.0;
	float stepL_ = 0.0f;
	float stepR_ = 0.0f;
	/// RUST: what each side was multiplied by on the last frame.
	float lastRing_[2] = { 1.0f, 1.0f };
	/// FOLLOW: the level handed in, and the gain made of it, glided.
	float envelope_ = 0.0f;
	float followGain_ = 1.0f;
	float followSmoothing_ = 1.0f;

	/// **NOT SAVED WITH THE PATCH, AND WHY**: a per-sample texture from a constant seed; the value it holds is
	/// re-drawn at every zero crossing of the input, so nothing reached by it outlives a reload's first cycle.
	uint32_t rng_ = kRngSeed;
	static constexpr uint32_t kRngSeed = 0x9e3779b9u;

	double lastDelayFrames_[2] = {};
	float lastMod_[2] = {};
	float lastNoise_[2] = {};
	float lastRead_ = 0.0f;
	float lastShaped_ = 0.0f;
	float lastTap_ = 0.0f;
	uint64_t holds_ = 0;
};

} // namespace feathers::fx::eroder
