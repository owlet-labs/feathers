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
#include "device/transport_lock.h"
#include "fx/disperser/disperser_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feathers::fx::disperser {

/// **THE DISPERSER'S RUNNING STATE AND ITS SAMPLE LOOP.** Follows `processDisperser`, `processBuffer`
/// and `processSample` statement for statement, in the firmware's units (full scale in is 1/128), with
/// the firmware's saturations as clamps and its one non-saturating shift as a wrap.
class Disperser {
public:
	/// ALLOCATE. Activate-time only: the 200 ms line at the running rate. The firmware allocates it
	/// from SDRAM when Stages leaves 0 and frees it at 0; here it is always there.
	void prepare(double sampleRate);

	/// AS IF JUST SWITCHED ON. The heard note is NOT forgotten — it is the host's, as `lastNoteCode`
	/// is the Sound's and not the effect's.
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept;
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	/// THE LAST NOTE, from the bus. A gated note makes the note HEARD from then on; the pitch is kept
	/// through releases, as `getLastNoteCode()` keeps it.
	void setNoteContext(const device::NoteContext& note) noexcept;
	/// THE HOST'S TRANSPORT, per block (`AudioOps::setTransportClock`): while it plays, the two wobble LFOs are the
	/// song's (backlog NM — derived, so a reload mid-song lands on the same phase), and free-run when it stops.
	void setTransportClock(const device::TransportClock& clock) noexcept { clock_ = clock; }

	void processStereo(float* left, float* right, int frames) noexcept;
	/// WHERE THE READINGS GO — see `Reading`. Installed once by the host that adopts the instance.
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }
	/// DECLARED. The stereo road over one buffer, yielding the LEFT channel.
	void processMono(float* mono, int frames) noexcept;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS — a claim about a value cannot be inferred from a rendered buffer.                  //
	//----------------------------------------------------------------------------------------------//

	struct BlockForTest {
		TopoParams topo {};
		TwistParams twist {};
		Routing routing {};
		double freqIndex = 0.0;   ///< the smoothed centre index the coefficients last used
		double spreadIndex = 0.0; ///< the smoothed spread, 0..1
		double centerHz = 0.0;
		double halfSpreadOctaves = 0.0; ///< the L/R half-offset after the LFOs, octaves
		double foldedDelaySamples = 0.0;
		double transient = 0.0;
		double punchGain = 0.0;
		double chirpGainF = 0.0;
		double chirpGain2F = 0.0;
		double chirpGainFRaw = 0.0;  ///< before the loop-gain cap
		double chirpGain2FRaw = 0.0; ///< before the loop-gain cap
		double loopGainRaw = 0.0;    ///< the chirp loop's bound before the cap
		bool fullPath = false; ///< the delay road ran this block
		int coefficientUpdates = 0;
	};
	[[nodiscard]] const BlockForTest& blockForTest() const noexcept { return block_; }
	[[nodiscard]] double stageHzForTest(int stage, int channel) const noexcept {
		return stageHz_[static_cast<size_t>(stage & 31)][channel & 1];
	}
	[[nodiscard]] double stageQForTest(int stage) const noexcept { return stageQ_[static_cast<size_t>(stage & 31)]; }
	[[nodiscard]] double stageGainForTest(int stage) const noexcept { return stageGains_[static_cast<size_t>(stage & 31)]; }
	[[nodiscard]] size_t stageOffsetForTest(int stage) const noexcept {
		return stageOffsets_[static_cast<size_t>(stage & 31)];
	}
	[[nodiscard]] AllpassCoeffs coeffsForTest(int stage, int channel) const noexcept {
		return channel == 0 ? coeffsL_[static_cast<size_t>(stage & 31)] : coeffsR_[static_cast<size_t>(stage & 31)];
	}
	[[nodiscard]] double smoothedTopoForTest() const noexcept { return smoothedTopo_; }
	[[nodiscard]] double smoothedTwistForTest() const noexcept { return smoothedTwist_; }
	[[nodiscard]] bool heardNoteForTest() const noexcept { return heardNote_; }
	/// The note it latched, in semitones (A4 = 69) — what KeyTrack centres on. TEST ONLY.
	[[nodiscard]] double noteSemitonesForTest() const noexcept { return noteSemitones_; }
	[[nodiscard]] int blockFramesForTest() const noexcept { return blockFrames_; }
	[[nodiscard]] size_t lineLengthForTest() const noexcept { return lineL_.size(); }
	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }
	[[nodiscard]] uint64_t currentDelayQ16ForTest() const noexcept { return currentDelayQ16_; }
	/// HOW MANY TIMES A SAMPLE WAS PINNED AT THE LINE'S `signed_saturate<30>` CEILING since reset — the
	/// runaway's signature.
	[[nodiscard]] uint64_t linePinsForTest() const noexcept { return linePins_; }
	/// THE REST OFF — so a test can hold the output with it against the output without it.
	void setRestEnabledForTest(bool on) noexcept { restEnabled_ = on; }
	[[nodiscard]] uint64_t restClearsForTest() const noexcept { return restClears_; }
	/// **WHETHER AN ECHO IS STILL WAITING IN THE LINE** — `AudioOps::holdsTail` (backlog MC): up to 200 ms ahead of the
	/// head, behind an output the stages may already have brought to exact zero.
	[[nodiscard]] bool holdsTail() const noexcept;
	[[nodiscard]] size_t lineSizeForTest() const noexcept { return lineL_.size(); }
	/// THE TWO STEREO LFOs HELD AT THEIR CENTRE — so an impulse response is time-invariant and can be held
	/// against the card's plot, which draws them there.
	void setLfoFrozenForTest(bool on) noexcept { lfoFrozen_ = on; }
	/// The two wobbles' phases and the rate scale the last coefficient update moved them by (backlog NM's song lock).
	[[nodiscard]] float widthLfoPhaseForTest() const noexcept { return widthLfoPhase_; }
	[[nodiscard]] float detuneLfoPhaseForTest() const noexcept { return detuneLfoPhase_; }
	[[nodiscard]] double lfoRateScaleForTest() const noexcept { return lastLfoRateScale_; }
	/// **THE CHAIN ALONE** — every delay write zeroed, so nothing is fed back: what the allpasses do by themselves.
	void setWritesOffForTest(bool on) noexcept { writesOff_ = on; }
	/// **THE FIRMWARE'S ROAD** — its Direct Form II Transposed stages (stepped), its delay snap under a tenth, its
	/// stepped delay-road gains and its routing cut with an eighth kept: everything the port changed about how
	/// the device MOVES. Held still none of it differs; the transcription comparisons run on it.
	void setDf2tForTest(bool on) noexcept { df2tForTest_ = on; }
	/// **THE LATTICE STEPPED, AS THE FIRMWARE STEPS ITS COEFFICIENTS** — the zipper the ramp removes, for its A/B.
	void setLatticeStepsForTest(bool stepped) noexcept { latticeRampForTest_ = !stepped; }
	/// Samples the loop rider has been below 1 — zero at every normal level.
	[[nodiscard]] uint64_t riderEngagedForTest() const noexcept { return riderEngaged_; }
	void setRiderOffForTest(bool off) noexcept { riderOff_ = off; }
	/// A stage's state energy, both channels — zero once it has left the chain.
	[[nodiscard]] double stageStateEnergyForTest(int stage) const noexcept {
		const auto i = static_cast<size_t>(stage & 31);
		return s1_[i][0] * s1_[i][0] + s1_[i][1] * s1_[i][1] + s2_[i][0] * s2_[i][0] + s2_[i][1] * s2_[i][1];
	}

private:
	/// THE CONTROL PATH, once per block, reading the block's first input sample for the transient
	/// detector as `processBuffer` reads `buffer[0]`.
	void tick(const Settings& s, float firstL, float firstR) noexcept;
	void updateCoefficients(double freqIndex, double spread, double lrOffset, double q, int activeStages,
	                        double spreadCurve, double qTilt, double bimodalSeparation, double detuning,
	                        double emphasis, double lfoRateScale) noexcept;
	/// THE SAMPLE LOOP, over a run of frames inside one block.
	void renderChunk(float* left, float* right, int frames) noexcept;
	void processSample(float inL, float inR, float& outL, float& outR) noexcept;
	void route(float inL, float inR, float& outL, float& outR) noexcept;
	/// One routing on the state set `runOld_` names — `route` runs two of them through a crossfade.
	void routeOnce(Topology t, float inL, float inR, float& outL, float& outR) noexcept;
	/// One sample along the ramp from the last update's lattice to this one's.
	void advanceLattice() noexcept;
	/// THE MOST `route` CAN AMPLIFY, at any frequency, for the live stage gains and routing — see `tick`'s cap.
	[[nodiscard]] float routingBound() const noexcept;
	void runStage(size_t i, float& l, float& r) noexcept;
	void writeFractional(float inL, float inR, uint64_t offsetQ16) noexcept;
	[[nodiscard]] bool hasEnergy() const noexcept;
	void clearAudioState() noexcept;
	void publishReadings() noexcept;

	/// **THE REST — NOT THE FIRMWARE'S; ITS q31 NEVER FALLS SILENT EITHER.** MEASURED on the transcription:
	/// after a burst the firmware's output settles into a limit cycle near -90 dBFS and stays there. This
	/// port's does not — but it decays SLOWLY where stages sit at the 14 Hz angle floor: one such stage
	/// falls ~33 dB/s, a cascade of 32 peaks seconds late, and with Chirp recirculating the tail measured
	/// ~2 dB/s all the way down (a meta-zone setting at the default Freq: -89 dBFS at 10 s, -160 at 50 s).
	/// So once the input has been EXACTLY zero and the output under -140 dBFS (the Eroder's 1e-7; 30 dB
	/// under a 24-bit floor, cutting nothing anyone hears) for longer than the line is long, every state and
	/// the line are cleared: nothing written before the quiet can still be waiting in a 200 ms line after
	/// 250 ms of it. The filter's and CLIP's precedent, with the window sized to this device's memory.
public:
	static constexpr float kRestLevel = 1.0e-7f;
	static constexpr double kRestSeconds = 0.25;
	/// THE LOOP RIDER'S THRESHOLD, in the firmware's units: 1/8 is +24.1 dBFS here, 6 dB under the line's pin (1/4).
	static constexpr float kRiderLevel = 0.125f;
	/// Its release to -60 dB, in seconds.
	static constexpr double kRiderReleaseSeconds = 0.5;
	/// How long a routing change crossfades the old routing into the new — see `route`.
	static constexpr double kRouteFadeSeconds = 0.010;
	/// The delay road's gains glide to each block's value with this time constant — see `processSample`.
	static constexpr double kGainGlideSeconds = 0.002;
	/// The rider's GAIN smoothing: how fast it comes down, and how fast it goes back up (one-pole time constants).
	static constexpr double kRiderAttackSeconds = 0.002;
	static constexpr double kRiderReleaseGainSeconds = 0.005;

private:
	[[nodiscard]] float detectTransient(float inL, float inR, double delaySamples) noexcept;

	double sampleRate_ = 48000.0;
	int blockFrames_ = 128;
	int crossfadeFrames_ = 256;
	uint64_t minHarmonicQ16_ = 441u << 16;

	Settings target_ {};
	bool heardNote_ = false;
	double noteSemitones_ = 60.0;

	// THE BLOCK GRID.
	bool placed_ = false;
	int untilTick_ = 0;
	int activeStages_ = kDefaultStages;
	Topology topology_ = Topology::Cascade;
	int lastTopoZone_ = -1;
	double smoothedTopo_ = 0.0;
	double smoothedTopoLap_ = 0.0; ///< Topo's phase past its zone (`Settings::topoLap`), glided
	double smoothedQOffset_ = 0.0; ///< `Settings::qOffsetOctaves`, glided

	double smoothedTwist_ = 0.0;
	double smoothedFreq_ = 0.0;
	double smoothedSpread_ = 0.0;

	// `Disperser`'s coefficient state.
	std::array<std::array<double, 2>, kMaxStages> s1_ {};
	std::array<std::array<double, 2>, kMaxStages> s2_ {};
	/// **THE ROUTING CROSSFADE** — see `route`. The old routing's states, moved out at a change, and which set
	/// `runStage` works on; both preallocated, nothing moves on the audio thread but values.
	std::array<std::array<double, 2>, kMaxStages> s1Old_ {};
	std::array<std::array<double, 2>, kMaxStages> s2Old_ {};
	bool runOld_ = false; ///< `runStage` on the old set (not a pointer: the object may be copied)
	Topology oldTopology_ = Topology::Cascade;
	int routeFadeRemaining_ = 0;
	int routeFadeFrames_ = 441;
	std::array<AllpassCoeffs, kMaxStages> coeffsL_ {};
	std::array<AllpassCoeffs, kMaxStages> coeffsR_ {};
	/// **THE LATTICE AS IT RUNS, RAMPED TO EACH UPDATE'S TARGET SAMPLE BY SAMPLE** — see `advanceLattice`.
	struct Lattice {
		double k1 = 0.0;
		double c1 = 1.0;
		double k2 = 0.0;
		double c2 = 1.0;
	};
	std::array<std::array<Lattice, 2>, kMaxStages> lattice_ {};
	std::array<std::array<Lattice, 2>, kMaxStages> latticeStep_ {};
	std::array<std::array<Lattice, 2>, kMaxStages> latticeTarget_ {};
	int latticeRemaining_ = 0;
	int latticeCount_ = 0; ///< the stages the last update set — a stage past it snaps when it arrives
	bool latticeSnap_ = true;
	bool latticeRampForTest_ = true;
	std::array<std::array<double, 2>, kMaxStages> stageHz_ {};
	std::array<double, kMaxStages> stageQ_ {};
	std::array<size_t, kMaxStages> stageOffsets_ {};
	std::array<float, kMaxStages> ladderCrossGains_ {};
	/// 1.0 is unity — the firmware stores it halved (`ONE_Q31 / 2`) and doubles with a shift.
	std::array<float, kMaxStages> stageGains_ {};
	int coeffUpdateCounter_ = kCoeffUpdateStride;
	double lastFreq_ = -1.0;
	double lastSpread_ = -1.0;
	int lastActiveStages_ = -1;
	float lastLrOffset_ = 0.0f;
	float lastQ_ = -1.0f;
	float lastSpreadCurve_ = -1.0f;
	float lastQTilt_ = 999.0f;
	float lastBimodal_ = -1.0f;
	float lastDetuning_ = -1.0f;
	float lastEmphasis_ = 999.0f;
	float widthLfoPhase_ = 0.0f;
	float detuneLfoPhase_ = 0.0f;

	// THE DELAY — `DisperserDelayState`, a write-ahead ring read at its head.
	std::vector<float> lineL_ {};
	std::vector<float> lineR_ {};
	size_t headPos_ = 0;
	int activityCounter_ = 0;
	uint64_t currentDelayQ16_ = 0;
	uint64_t prevDelayQ16_ = 0;
	uint64_t pendingDelayQ16_ = 0;
	int crossfadeRemaining_ = 0;
	float fastEnv_ = 0.0f;
	float slowEnv_ = 0.0f;

	// THIS BLOCK'S LATCHED QUANTITIES — `processBuffer`'s locals.
	bool fullPath_ = false;
	float punch_ = 0.0f;
	float chirp_ = 0.0f;
	float crossGain_ = 0.4f;
	float punchGain_ = 0.0f;
	float chirpGainF_ = 0.0f;
	float chirpGain2F_ = 0.0f;
	bool useHarmonic2F_ = false;

	BlockForTest block_ {};
	uint64_t linePins_ = 0;
	int restFrames_ = 11025;
	int quietFrames_ = 0;
	bool restEnabled_ = true;
	/// How many times the rest has cleared the audio state — once per silence, never per sample.
	uint64_t restClears_ = 0;
	bool lfoFrozen_ = false;
	/// THE TRANSPORT, and the two wobbles' locks onto the song — see `setTransportClock`. `tickOffset_` is how far into
	/// the host's block the setup now running starts, so the song position is the block's plus that.
	device::TransportClock clock_ {};
	device::TransportLock widthLock_ {};
	device::TransportLock detuneLock_ {};
	int tickOffset_ = 0;
	double lastLfoRateScale_ = 0.0;
	bool writesOff_ = false;
	bool df2tForTest_ = false;
	float riderEnv_ = 0.0f;
	float riderRelease_ = 0.9997f;
	uint64_t riderEngaged_ = 0;
	float riderGain_ = 1.0f;
	float riderAttack_ = 0.01f;
	float riderGainRelease_ = 0.004f;
	bool riderOff_ = false;
	/// THE DELAY ROAD'S GAINS AS HEARD — gliding to the block's values; see `processSample`.
	float punchNow_ = 0.0f;
	float chirpFNow_ = 0.0f;
	float chirp2FNow_ = 0.0f;
	float readNow_ = 0.0f;
	float gainGlide_ = 0.01f;
	device::ReadingsSink sink_ {};
};

} // namespace feathers::fx::disperser
