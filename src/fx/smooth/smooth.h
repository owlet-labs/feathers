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

#include "device/registry.h"
#include "device/transport_lock.h"
#include "fx/dyn/meter_ramp.h"
#include "fx/smooth/smooth_types.h"

#include <array>
#include <cstdint>

namespace feathers::fx::smooth {

/// **ONE SMOOTH SHAPER: ONE VOICE'S, OR THE BUS'S.** What the device IS — the eight zones, Twist, the
/// blend weights — is `smooth_types.h`, each piece a pure function checked against the firmware's; this
/// is the firmware's `sineShapeBuffer` around them, statement for statement, with the per-buffer setup
/// on a grid of its own (`kBlockFrames`) and its state per instance. The state is the firmware's
/// `SineShaperVoiceState` plus the half of `SineTableShaperParams` that smooths.
class SmoothShaper {
public:
	/// NO ALLOCATION: the rate-derived constants, then `reset`.
	void prepare(double sampleRate) noexcept;

	/// AS IF JUST SWITCHED ON — which, per voice, is a new note: the DC and feedback states, the stereo
	/// LFO, the drive glide's "not yet placed" (`voice.cpp` resets `SineShaperVoiceState` there), and the
	/// coefficient smoother's too, so the note's first block starts AT its targets. The grid restarts.
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept { target_ = settings; }
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	/// `sineShapeBuffer`'s stereo overload.
	void processStereo(float* left, float* right, int frames) noexcept;
	/// THE STEREO ROAD'S LEFT CHANNEL — not the firmware's mono overload, which
	/// has no stereo LFO; see the .cpp.
	void processMono(float* mono, int frames) noexcept;

	/// THE VOICE'S NOTE AND STACK (`device::NoteContext`), per chunk on the per-voice road. Only the stack is read: it
	/// sets the stack trim (`stackTrimOf`), applied while Poly is on and never on the bus, which sums whole chords.
	void setNoteContext(const device::NoteContext& note) noexcept { note_ = note; }
	/// THE TRIM THE CURRENT BLOCK APPLIED — 1 at a stack of one and on the bus.
	[[nodiscard]] float stackTrimForTest() const noexcept { return block_.stackTrim; }

	/// WHERE THE READINGS GO (`AudioOps::setReadingsSink`). Installed on the BUS instance only; a voice has none.
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }
	/// THE HOST'S TRANSPORT, per block (`AudioOps::setTransportClock`): while it plays, the BUS instance's stereo LFO is
	/// the song's (backlog NM — derived, so a reload or a play mid-song lands on the same phase).
	void setTransportClock(const device::TransportClock& clock) noexcept { clock_ = clock; }
	/// A VOICE INSTANCE (`AudioOps::setVoice`, never called on the bus). **ITS LFO IS NOT THE SONG'S, AND WHY**: it
	/// starts at every note, at a phase the Twist chooses (the firmware's per-note variation), so it is anchored to the
	/// note — reproducible by construction — and locking it to the song would undo the variation.
	void setVoice() noexcept { voice_ = true; }
	[[nodiscard]] float lfoPhaseNowForTest() const noexcept { return lfoPhase_; }
	/// THE HELD INPUT PEAK, as published.
	[[nodiscard]] float inputPeakForTest() const noexcept { return inPeak_.reading(); }

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — a claim about a value cannot be inferred from a rendered buffer.                  //
	//----------------------------------------------------------------------------------------------//

	/// THE NINE SMOOTHED COEFFICIENTS: c1, c3..c9 left, c3..c9 right — `SineTableShaperParams`' order.
	using CoefficientSet = std::array<float, 9>;
	[[nodiscard]] CoefficientSet coefficientsForTest() const noexcept;
	/// START THE SMOOTHER FROM THESE rather than at the first block's targets. The firmware's power-on values
	/// are {1, 0, ...}; a comparison with the reference starts both there, so the glide is compared too.
	void adoptCoefficientsForTest(const CoefficientSet& c) noexcept;
	/// The drive gain the next frame will use; negative before the first block.
	[[nodiscard]] float driveGainForTest() const noexcept { return driveGainPlaced_ ? driveGain_ : -1.0f; }
	[[nodiscard]] float lfoPhaseForTest() const noexcept { return lfoPhase_; }
	[[nodiscard]] int32_t feedbackStateForTest(int channel) const noexcept { return fbState_[channel & 1]; }
	[[nodiscard]] int32_t dcStateForTest(int channel) const noexcept { return dcState_[channel & 1]; }
	/// WHAT THE LAST FRAME'S SHAPER SAW, per channel.
	[[nodiscard]] const ShapeProbe& probeForTest(int channel) const noexcept { return probe_[channel & 1]; }
	/// The current block's zone, as the setup decided it.
	[[nodiscard]] int zoneForTest() const noexcept { return block_.zone; }
	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }
	/// THE RATE-DERIVED CONSTANTS, which must be the firmware's to the bit at 44.1 kHz.
	[[nodiscard]] float perSampleAlphaForTest() const noexcept { return perSampleAlpha_; }
	[[nodiscard]] int32_t hpfAlphaForTest() const noexcept { return hpfAlphaQ_; }
	[[nodiscard]] int32_t feedbackPoleForTest() const noexcept { return fbAlphaQ_; }

private:
	/// THE PER-BUFFER SETUP, once per `kBlockFrames`: read the settings, decide the zone and the targets,
	/// the convergence test, the stereo LFO and the feedback amount.
	void beginBlock() noexcept;
	/// The stereo loop over frames inside one block; `left` and `right` may be the same buffer (the mono road).
	void renderFrames(float* left, float* right, int frames) noexcept;
	void advanceCoefficients() noexcept;

	double sampleRate_ = kReferenceRate;
	float perSampleAlpha_ = static_cast<float>(kPerSampleAlphaAtReference);
	float stridedAlpha_ = static_cast<float>(kPerSampleAlphaAtReference) * kSmoothingStride;
	int32_t hpfAlphaQ_ = static_cast<int32_t>(kOutputHpfAlphaAtReference * kOneQ31);
	/// The feedback tap's one-pole as a q31 step; `1 << 30` (the firmware's `>> 1`) at 44.1 kHz.
	int32_t fbAlphaQ_ = 1 << 30;
	/// THE DC BLOCKER'S DEAD BAND: below `2^32 / alpha` its update `mulhi(-dc, alpha)` floors to 0 and stops.
	int32_t deadBand_ = 142;
	/// THE REST: consecutive frames of exactly-zero input, and how many make `kRestSilentSeconds` at this rate.
	int silentRun_ = 0;
	int restFrames_ = 2205;

	Settings target_ {};

	/// THE BLOCK THE GRID IS IN — everything the firmware hoists out of its sample loop.
	struct Block {
		bool bypass = true;
		int zone = 0;
		float posInZone = 0.0f;
		TwistParams twist {};
		int32_t mix = 0;
		int32_t invMix = kOneQ31;
		int32_t fbAmount = 0;
		float targetDriveGain = 1.0f;
		float driveRScale = 1.0f;
		/// THE STACK TRIM, folded into the drive gain's TARGET so it rides the per-frame drive smoothing.
		float stackTrim = 1.0f;
		bool converged = true;
		bool fmZone = false;
		/// c1 shared, then c3..c9 left, then c3..c9 right — the smoother's targets.
		CoefficientSet targets {};
		int framesLeft = 0;
		int stride = 0;
	};
	Block block_ {};

	/// THE SMOOTHER'S STATE, per instance (see the header's departures) — and "not yet placed" until the
	/// first block snaps it to its targets.
	CoefficientSet smoothed_ { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	bool coefficientsPlaced_ = false;
	Coefficients weights_[2] {};

	/// `params->smoothedDrive`: the q31 drive the last block asked for, INT32_MIN before the first.
	int32_t smoothedDriveQ_ = INT32_MIN;
	/// `voiceState->smoothedDriveGain`, with its negative sentinel as a flag.
	float driveGain_ = 1.0f;
	bool driveGainPlaced_ = false;
	float lfoPhase_ = 0.0f;
	int32_t dcState_[2] = {};
	int32_t fbState_[2] = {};
	ShapeProbe probe_[2] {};
	/// THE INPUT PEAK, held 50 ms (`fx::dyn::HeldPeak`, CLIP's), and the sink it is published to once per call.
	fx::dyn::HeldPeak inPeak_ {};
	device::NoteContext note_ {};
	device::ReadingsSink sink_ {};
	/// THE TRANSPORT, the bus LFO's lock onto the song, and how far into the host's block the setup now running starts.
	device::TransportClock clock_ {};
	device::TransportLock lfoLock_ {};
	int blockOffset_ = 0;
	bool voice_ = false;
};

} // namespace feathers::fx::smooth
