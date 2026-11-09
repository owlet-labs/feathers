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

#include "device/device.h"
#include "fx/eroder/eroder_types.h"
#include "fx/sklitter/ring.h"

#include <cstddef>
#include <cstdint>

namespace feathers::fx::eroder {

/// **THE ERODER'S RUNNING STATE AND ITS SAMPLE LOOP.** What the device IS — the mapping from four
/// controls to eleven quantities — is `derive()` in `eroder_types.h`; this is what runs on the result.
/// The loop follows `processEroder`'s inner loop statement for statement, and says so where it does.
class Eroder {
public:
	/// ALLOCATE. Activate-time only: the comb's line, sized from `kLineSeconds` at the running rate.
	void prepare(double sampleRate);

	/// AS IF JUST SWITCHED ON — the line, the filter, the held sample, the triangle, the noise stream,
	/// and the smoothing's "not yet placed".
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept;
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	/// THE LAST NOTE, from the bus. **GATE IGNORED, PITCH KEPT** — the shaper's rule and the
	/// firmware's behaviour: `getLastNoteCode()` does not forget a note because its key came up, so
	/// the comb stays tuned through a release tail instead of snapping to middle C on every note-off.
	void setNoteContext(const device::NoteContext& note) noexcept;

	void processStereo(float* left, float* right, int frames) noexcept;

	/// DECLARED. The stereo road over one buffer, yielding the LEFT channel because `processStereo`
	/// stores right-then-left — and here it matters more than usual: the right channel's noise is
	/// decorrelated from the left's by `width`, so the two are different signals at any setting.
	void processMono(float* mono, int frames) noexcept;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — a claim about a value cannot be inferred from a rendered buffer.                  //
	//----------------------------------------------------------------------------------------------//

	/// WHAT THE LAST BLOCK ACTUALLY RAN ON — after the smoothing, the note and the rate.
	[[nodiscard]] const Derived& derivedForTest() const noexcept { return derived_; }
	/// THE SMOOTHED COORDINATES, as opposed to the ones asked for.
	[[nodiscard]] double smoothedToneForTest() const noexcept { return smoothedTone_; }
	[[nodiscard]] double smoothedCharacterForTest() const noexcept { return smoothedCharacter_; }
	/// WHERE EACH CHANNEL'S READ HEAD WAS ON THE LAST SAMPLE, in seconds behind the write.
	[[nodiscard]] double lastDelaySecondsForTest(int channel) const noexcept {
		return lastDelayFrames_[channel & 1] / sampleRate_;
	}
	/// HOW MANY TIMES THE SAMPLE-AND-HOLD HAS RE-DRAWN since the last reset.
	[[nodiscard]] uint64_t holdsForTest() const noexcept { return holds_; }
	/// WHETHER THE LINE STILL HOLDS SOUND THE HEAD WILL READ — `AudioOps::holdsTail` (backlog MC): up to 5.8 ms behind a
	/// click, longer than a small host block. Off or at Mix 0 the line is not read (`processStereo`).
	[[nodiscard]] bool holdsTail() const noexcept { return target_.enabled && target_.mix > 0.0f && line_.holdsSound(); }
	[[nodiscard]] float triangleForTest() const noexcept { return triValue_; }
	[[nodiscard]] double pitchSemitonesForTest() const noexcept { return pitchSemitones_; }
	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }
	/// HOW MANY TICKS THE MODULATION CLOCK HAS RUN since the last reset — one per 1/44100 s at any host rate.
	[[nodiscard]] uint64_t modTicksForTest() const noexcept { return modTicks_; }

private:
	/// THE CONTROL PATH: smooth the two coordinates one control block further and re-derive.
	void tick(const Settings& settings) noexcept;
	/// THE SAMPLE LOOP, over a run of frames that lies inside one control block.
	void renderChunk(float* left, float* right, int frames) noexcept;

	/// XORSHIFT32 → [-1, 1). The firmware's `getNoise()` is a full-range q31.
	[[nodiscard]] float nextNoise() noexcept;

	fx::sklitter::Ring line_ {};
	double sampleRate_ = 48000.0;
	uint64_t writeCounter_ = 0;

	Settings target_ {};
	double pitchSemitones_ = kReferenceNote;

	/// NEGATIVE-INFINITY-FREE "NOT YET PLACED": false until the first block, which SNAPS the smoothed
	/// coordinates to the controls. The firmware glides up from zero at power-on, which is a sweep
	/// through the bank every time the device is switched on — and would make the registry's
	/// partition sweep a statement about block lengths rather than about this device.
	bool placed_ = false;
	/// FRAMES LEFT IN THE CURRENT CONTROL BLOCK — carried across calls, which is the whole point: the
	/// grid is the device's and not the host's.
	int untilTick_ = 0;
	double smoothedTone_ = 0.0;
	double smoothedCharacter_ = 0.0;

	Derived derived_ {};

	// THE SVF — `EroderNoiseState`.
	float svfLowL_ = 0.0f;
	float svfLowR_ = 0.0f;
	float svfBandL_ = 0.0f;
	float svfBandR_ = 0.0f;
	// THE SAMPLE-AND-HOLD. **MONO, AND IT LISTENS TO THE LEFT CHANNEL ONLY** — the firmware's
	// `heldL`, `prevInputL`, `prevFilteredL`, with no right-hand counterparts. Kept.
	float held_ = 0.0f;
	float prevInputL_ = 0.0f;
	float prevFilteredL_ = 0.0f;

	float feedbackL_ = 0.0f;
	float feedbackR_ = 0.0f;
	float wetHpfL_ = 0.0f;
	float wetHpfR_ = 0.0f;

	/// THE PITCHED TRIANGLE: a value and a signed step, the sign being its direction, which persists
	/// across blocks exactly as `triStep`'s does.
	float triValue_ = 0.0f;
	float triStep_ = 0.0f;

	/// **NOT SAVED WITH THE PATCH, AND WHY** (backlog NM): a per-sample texture from a constant seed, and the value it
	/// holds is redrawn at every zero crossing — milliseconds. Nothing reached by it outlives a reload's first cycle.
	uint32_t rng_ = kRngSeed;
	static constexpr uint32_t kRngSeed = 0xc2b2ae35u;

	/// **THE MODULATION CLOCK — 44.1 kHz AT EVERY HOST RATE (backlog NX-D1).** The noise, the hold, the SVF and the
	/// triangle are per-SAMPLE quantities in the firmware — a white draw's spectral density, a wrap, a step — so run at
	/// the host's rate they are a different instrument (at 96 kHz the wraps vanish and the shake falls 3–5 dB). They run
	/// here on a phase that advances `kReferenceRate / rate` a host frame and ticks at each whole turn; the read head
	/// HOLDS the last tick's value between ticks. At 44.1 kHz the step is exactly one, so there is one tick a frame and
	/// the loop is the firmware's, to the bit. The comb, the shapers and the wet highpass stay at the host rate: their
	/// constants are seconds and hertz.
	double tickPhase_ = 1.0;
	double tickStep_ = 1.0;
	/// A ZERO CROSSING OF THE DRY LEFT INPUT, seen at the host rate and waiting for the next tick to fire the hold.
	bool pendingInputCross_ = false;
	float modL_ = 0.0f;
	float modR_ = 0.0f;
	uint64_t modTicks_ = 0;

	double lastDelayFrames_[2] = {};
	uint64_t holds_ = 0;
};

} // namespace feathers::fx::eroder
