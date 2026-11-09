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
#include "fx/pitch/pitch_types.h"
#include "fx/sklitter/ring.h"

#include <cstddef>
#include <cstdint>

namespace feathers::fx::pitch {

/// **THE TWO-HEAD DELAY-LINE SHIFTER.** The topology and what is deliberately missing are in
/// `pitch_types.h`; this is how it runs.
///
/// One phase, advancing a cycle per grain. Two heads per channel, half a cycle apart. A head's
/// DELAY behind the write position is the running quantity: it changes by `1 - ratio` every sample,
/// which is the same statement as "the head reads the line `ratio` times as fast as it was written".
/// When a head's phase wraps its window is at zero, and that is the instant its delay is re-seated.
///
/// **THE DELAY IS INTEGRATED, NOT COMPUTED FROM THE PHASE**, and that is the decision the rest leans
/// on. The textbook form is `delay = (1 - phase) * span`, which is a function of the CURRENT ratio
/// and grain length — so moving Shift or Size mid-grain teleports both heads, a click per control
/// tick for as long as a modulator is on the knob. Integrated, a moved Shift changes only the SLOPE
/// from here on, and the sounding grain carries on from where it is.
class PitchShifter {
public:
	/// ALLOCATE. Activate-time only.
	void prepare(double sampleRate);

	/// AS IF JUST SWITCHED ON — the line, the heads, the phase and the random stream.
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept;
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	void processStereo(float* left, float* right, int frames) noexcept;

	/// DECLARED, for the delay's reason: the stereo road over one buffer, which yields the LEFT
	/// channel because `processStereo` stores right-then-left. With Width up the two channels are
	/// different pitches, so which one survives is audible and is asserted.
	void processMono(float* mono, int frames) noexcept;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — CLAUDE.md: a claim about a value cannot be inferred from a rendered buffer.       //
	//----------------------------------------------------------------------------------------------//

	/// THE RATIO EACH CHANNEL RAN AT IN THE LAST BLOCK — what came out the far end of Scale, Shift,
	/// Fine and Width, as opposed to what the knobs say.
	[[nodiscard]] double ratioLeftForTest() const noexcept { return lastRatioL_; }
	[[nodiscard]] double ratioRightForTest() const noexcept { return lastRatioR_; }

	/// A HEAD'S DELAY BEHIND THE WRITE POSITION, in frames. `channel` 0 is left; `head` is 0 or 1.
	[[nodiscard]] double headDelayForTest(int channel, int head) const noexcept {
		return delay_[channel & 1][head & 1];
	}

	/// HOW MANY TIMES A HEAD HAS BEEN RE-SEATED since the last reset — the grain count.
	[[nodiscard]] uint64_t reseatsForTest() const noexcept { return reseats_; }
	/// WHETHER THE LINE STILL HOLDS SOUND A HEAD WILL READ — `AudioOps::holdsTail` (backlog MC): a head seated behind
	/// the newest sound reads exact zeroes until it reaches it. Off, nothing is read.
	[[nodiscard]] bool holdsTail() const noexcept { return target_.enabled && line_.holdsSound(); }

	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }

private:
	/// SEAT ONE HEAD FOR ITS NEXT GRAIN, both channels at once. Forward, it starts far enough back
	/// that a head reading faster than the line is written cannot catch the write position before its
	/// window closes — `(ratio - 1)` grain lengths, and nothing at all when the ratio is one or less,
	/// which is what makes a shift of zero the input exactly. Reversed, it starts at the newest sample
	/// and falls back from there.
	///
	/// **ONE JITTER DRAW PER HEAD, SHARED BY THE TWO CHANNELS**, so Jitter scatters the grain in time
	/// and leaves the stereo image to Width, whose job that is.
	///
	/// `loopFrames` is Feedback's grain of loop delay — `kFeedbackMax` says why it exists — and is
	/// zero whenever Feedback is.
	void seatHead(int head, double ratioLeft, double ratioRight, double grainFrames, bool reverse,
	              double jitter, double loopFrames) noexcept;

	/// XORSHIFT32, re-seeded by `reset` — the registry's partition sweep needs the same samples in to
	/// be the same samples out, and a jittered grain drawing from an unreset stream fails that for a
	/// reason unrelated to anything the sweep hunts.
	[[nodiscard]] float nextUnit() noexcept;

	fx::sklitter::Ring line_ {};
	double sampleRate_ = 48000.0;
	/// THE ABSOLUTE WRITE POSITION — `ring_lerp.h` says why `Ring::writePos()` will not do.
	uint64_t writeCounter_ = 0;

	Settings target_ {};

	/// 0..1 through a grain cycle. Head 1 runs half a cycle ahead of head 0.
	double phase_ = 0.0;
	/// [channel][head], in frames behind the newest written sample.
	double delay_[2][2] = {};
	/// FALSE UNTIL THE FIRST BLOCK, so the heads are seated from the first block's controls rather
	/// than from a constructor's guess at them.
	bool seated_ = false;

	/// THE LAST SAMPLE'S WET, waiting to be written back into the line under Feedback.
	float loopL_ = 0.0f;
	float loopR_ = 0.0f;

	/// **NOT SAVED WITH THE PATCH, AND WHY** (backlog NM): the grain draws (constant seed, reset with the device) and the
	/// 2 s line are a tail — Feedback stops short of 1, so it decays — and audio is not persisted.
	uint32_t rng_ = kRngSeed;
	static constexpr uint32_t kRngSeed = 0x85ebca6bu;

	double lastRatioL_ = 1.0;
	double lastRatioR_ = 1.0;
	uint64_t reseats_ = 0;
};

} // namespace feathers::fx::pitch
