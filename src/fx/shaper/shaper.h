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
#include "fx/shaper/shaper_curve.h"
#include "fx/shaper/shaper_types.h"

#include <array>
#include <memory>
#include <cstddef>

// THE SHAPER, RUNNING — the curve from `shaper_curve.h`, played.
//
// THE WHOLE DESIGN OF THIS CLASS IS ABOUT ONE PROBLEM: A TABLE REBUILD IS EXPENSIVE AND
// `setFields` IS ON THE AUDIO THREAD.
//
// `FxRunner::sync` runs before every slice and pushes a device's row into it EVERY BLOCK — its own
// comment says so, and it is right to: a row is where a swallowed device's controls live and they
// move whenever a hand does. So a `setTarget` that rebuilt the table synchronously would put the
// whole build inside one audio callback, on any block in which the hand had moved.
//
// THE FIRMWARE DOES NOT HAVE THIS PROBLEM AND SOLVES IT ANYWAY. On the reference the rebuild is
// scheduled onto the UI thread (`regenerateIfDirty`, called from a routine), and X/Y are deliberately
// NOT patched params — `shaper.h:44` says in as many words that changing them "triggers expensive
// table regeneration" and that this "makes them unsuitable for real-time modulation". That decision is
// carried over: Knee, Colour and Gamma declare `modulatable = false`. **But it is not sufficient
// here**, because a hand on a knob still moves a value once per block, and there is no UI thread in
// this device's world to hand the work to — `AudioOps` has a `setFields` and a `processStereo` and
// nowhere else to stand.
//
// SO THE BUILD IS AMORTISED INSTEAD OF DEFERRED. A slice of the back table is filled per block, the
// extremes accumulate across the slices, and the table is normalised and swapped in when the last
// slice lands. **The back table is never read while it is being written** — that is what makes
// partial writes invisible rather than a source of clicks — and the fade below is what makes the
// swap itself inaudible.
//
// THE THREE PROPERTIES THAT MAKE THIS SAFE ON THE AUDIO THREAD, each one a rule this repo already
// has:
//
//   1. NO ALLOCATION, EVER. Both tables are `std::array` members. The firmware allocates lazily —
//      `ensureBuffersAllocated`, `std::vector::resize`, and a deallocation when the effect goes
//      linear — because the hardware has 8 MB and cannot afford 8 KB it is not using. This has no such
//      constraint and CLAUDE.md has a hard rule, so the two tables are simply always there.
//   2. BOUNDED WORK PER BLOCK. `kBuildChunk` entries, whatever the block size and whatever the hand
//      is doing. The cost of a block is a function of the block, not of the gesture.
//   3. NO UNBOUNDED RESTART. A drag changes the target every block; latching the parameters AT THE
//      START of a build and only re-checking when it completes is what stops a continuous drag from
//      restarting the build forever and freezing the sound at the old curve. The curve then follows
//      the hand at one table per `kTableEntries / kBuildChunk` blocks, which is a timbre update rate
//      and not a parameter update rate — and a waveshaper's timbre is exactly the thing a player
//      hears as continuous anyway.
//
// AND THE SWAP IS CROSSFADED, which is the other half of "click-free". The firmware double-buffers
// with an IIR blend and then — `table_shaper.h:288`, "TEMPORARY: Crossfade disabled for
// benchmarking" — reads the target table directly, so the mechanism is present and switched off. It
// is on here, over a fixed span, which is cheaper than an IIR to reason about: two lookups per sample
// while a fade is running and one at every other time.

namespace feathers::fx::shaper {

class Shaper {
public:
	Shaper();

	void prepare(double sampleRate);

	/// FORGET THE SIGNAL, KEEP THE CURVE. `AudioOps::reset` means "as if just switched on", and the
	/// distinction that entry point's own comment draws is between forgetting a delay line and
	/// forgetting that you were running. The TABLE is neither: it is a pure function of the settings,
	/// which a reset does not change, so rebuilding it here would spend a rebuild to arrive at the
	/// table already in hand. What is cleared is everything the SIGNAL wrote — the comb's history, the
	/// slew state, the zero-crossing counters and the smoothed sweep.
	void reset();

	/// **THE SHARED TABLE (backlog OM).** A voice instance: frees its own bank (message thread, `AudioOps::setVoice`).
	void becomeVoice() noexcept;
	/// A voice instance: reads `bus`'s bank and baked settings from now on (audio thread, `AudioOps::pullHandBlocks`).
	void shareFrom(const Shaper& bus) noexcept;
	/// The bus instance while its voices run: take the collapsed row's settings and keep the shared bank built and
	/// fading for `frames` samples (audio thread, `AudioOps::beginSharedBlock`).
	void beginSharedBlock(int frames);
	/// TEST ONLY: the bank this instance plays, so a case can ask whether two voices play ONE table.
	[[nodiscard]] const void* bankForTest() const noexcept { return bank_; }
	/// The bank's bytes: what a bus instance holds besides itself, and what a voice no longer does.
	[[nodiscard]] static constexpr size_t bankBytesForTest() noexcept { return sizeof(TableBank); }

	void setTarget(const Settings& settings);
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	/// WHICH NOTE IS SOUNDING. Used by ONE thing — the Lowpass extra's cutoff, which the firmware
	/// tracks off the note so the filter sits a fixed interval above the fundamental rather than at an
	/// absolute frequency that means something different on every pitch.
	///
	/// A DEVICE ON THE BUS THAT STILL WANTS A NOTE is not a contradiction: HARM is the precedent and
	/// the same shape — mono-tracking, one pitch, used to place a frequency rather than to trigger
	/// anything. With no note the reference is 110 Hz, which is the firmware's own default.
	void setNoteContext(const device::NoteContext& note);

	/// The next call's Trim window, or null — see `pendingTrimRun_`.
	void setTrimRun(const float* run) noexcept { pendingTrimRun_ = run; }
	void processStereo(float* left, float* right, int frames);
	void processMono(float* mono, int frames);

	//---------------------------------------------------------------------------------------//
	// TEST SEAMS. Five lines each, and CLAUDE.md's reason: a claim about a value cannot be    //
	// recovered from a rendered buffer. Every one of these exists because the thing it        //
	// reports is invisible from outside and a difference check would pass on the wrong cause. //
	//---------------------------------------------------------------------------------------//

	/// WHAT THE FIELD SAID AT THE CURRENT POSITION. The whole device is "position becomes curve", and
	/// this is the only place the intermediate is visible. A case asserting a weight here is asserting
	/// the mapper; a case comparing two renders would pass whenever anything at all differed.
	[[nodiscard]] const CurveParams& curveForTest() const noexcept { return curve_; }

	/// THE SETTINGS AS LAST TOLD, so a case can see what `setFields` actually decoded.
	[[nodiscard]] const Settings& targetForTest() const noexcept { return target_; }

	/// THE TABLE THE AUDIO PATH IS ACTUALLY READING. Not the one that would be built from the current
	/// settings — the one in front, which is the whole point: it is how a case can tell an amortised
	/// build that has LANDED from one still in flight.
	[[nodiscard]] const std::array<float, kTableEntries>& tableForTest() const noexcept {
		return bank_->table[bank_->front];
	}

	/// HOW MANY TABLES HAVE BEEN SWAPPED IN since construction.
	///
	/// IT IS THE ONLY WAY TO SEE THE AMORTISATION AT ALL. The class's central claim is that a build
	/// costs bounded work per block and lands after a known number of them; a render cannot express
	/// that, and a timing measurement cannot distinguish "amortised correctly" from "fast enough that
	/// nobody noticed". This counts the events.
	[[nodiscard]] int rebuildsForTest() const noexcept { return bank_->rebuilds; }

	/// WHETHER A BUILD IS IN FLIGHT, and how far in. Reported so a case can drive exactly as many
	/// blocks as a build needs and assert that it took that many and not fewer.
	[[nodiscard]] bool buildingForTest() const noexcept { return bank_->building; }
	[[nodiscard]] size_t buildCursorForTest() const noexcept { return bank_->cursor; }

	/// THE THRESHOLD AND SLOPE THE BLEND IS RUNNING AT. Mix is not a crossfade here — it lowers an
	/// amplitude threshold — and that is a claim about two numbers that no rendered buffer states.
	/// The threshold is in INPUT units: what `|x|` is compared against, and what a shaped sample is
	/// referenced to.
	[[nodiscard]] float thresholdForTest() const noexcept { return threshold_; }
	[[nodiscard]] float blendSlopeForTest() const noexcept { return blendSlope_; }

	/// THE SWEEP THE READ IS RUNNING AT — Drive's whole effect, smoothed. 1 at drive 0; see
	/// `kSweepOctaves` in shaper.cpp for the law. A case restating the read needs the number the device
	/// is actually using, which lags the setting by the smoother's time constant.
	[[nodiscard]] float sweepForTest() const noexcept { return sweep_; }
	/// THE TWO EMPHASIS SECTIONS THE ENGINE IS RUNNING — the coefficients, not the sound.
	[[nodiscard]] TiltSection emphasisInForTest() const noexcept { return emphasisIn_; }
	[[nodiscard]] TiltSection emphasisOutForTest() const noexcept { return emphasisOut_; }

	/// HOW FAR PAST THE CEILING THE OUTPUT STAGE HAS HAD TO TRIM, at most, since the last reset.
	///
	/// THE READ IS INSIDE THE CEILING BY CONSTRUCTION AND THE DC BLOCKER IS NOT. A blocker is a
	/// highpass: removing the offset an asymmetric shaped wave carries shifts its other peak outward
	/// by that offset, so a wave whose shaped peaks sit at exactly ±1.0 comes out of the blocker at
	/// 1.0 plus its DC — MEASURED 1.009 on a +6 dB sine at colour 0.38. The output stage clamps that,
	/// and this reports the most it ever clamped, so a case can hold the clamp to being a trim of the
	/// blocker's shift and not the mechanism by which the ceiling is met.
	[[nodiscard]] float ceilingExcessForTest() const noexcept { return ceilingExcess_; }

	/// **HOW HARD THE TABLE IS BEING DRIVEN — the decaying peak input.** Not a test seam: the card and
	/// the popout draw it behind the transfer, which is what makes "am I past the threshold" a thing
	/// you can see rather than infer. See `meterInput` for why peak and not RMS.
	[[nodiscard]] float drivePeak() const noexcept { return drivePeak_; }
	/// **WHAT THE TRANSFER PRODUCED, BEFORE THE DC BLOCKER.** The drawn curve is a static function of
	/// one sample; `blockDc` is a filter with memory. Comparing the plot against the post-blocker
	/// output measures the blocker, not the agreement the certificate is about.
	[[nodiscard]] float preBlockForTest() const noexcept { return preBlock_; }
	/// The memory stages' envelope on one channel — Bloom and Sag scale by it, so a case asserting what
	/// they did reads what they were told.
	/// Loop's residue on one channel — so a restatement can take ONE step from the engine's own state
	/// rather than integrating its own copy, which a steep curve and a feedback loop drive apart.
	[[nodiscard]] float loopResidueForTest(int channel) const noexcept {
		return channel_[static_cast<size_t>(channel)].loopResidue;
	}
	[[nodiscard]] float envelopeForTest(int channel) const noexcept {
		return channel_[static_cast<size_t>(channel)].envelope;
	}
	/// The integrator's accumulator for one channel — bounded at +-1 (Shaper parity sweep, 2026-09-16).
	[[nodiscard]] float integratorForTest(int channel) const noexcept { return channel_[channel].integrator; }

	/// THE FRONT TABLE'S STEEPEST SECANTS, per half — what the read rate and output scale are derived
	/// from. Reported so a case can check the engine's number against one it computes itself from
	/// `tableForTest`, which is the restatement CLAUDE.md asks for.
	[[nodiscard]] const TableBound& boundForTest() const noexcept { return bank_->bound[bank_->front]; }

	/// WHAT THE DC BLOCKER IS HOLDING, per channel — the running output offset it has removed.
	///
	/// IT EXISTS BECAUSE THE DEFECT IT GUARDS IS INVISIBLE IN A SPECTRUM AND OBVIOUS IN A VALUE. DC is
	/// one bin nobody plots, and the way it presents to a player is a click when the effect switches
	/// in and a mix that will not null — neither of which a rendered buffer comparison would flag.
	/// **THE HYSTERESIS OFFSET THIS BLOCK, in excess units and signed by the field.**
	///
	/// TEST-ONLY, AND IT EXISTS BECAUSE A PROPORTION STOPPED BEING ABLE TO SEE IT. The case that owns
	/// this knob used to assert that turning it down removed most of the device's distortion, which
	/// was true while hysteresis was the only ungated effect. Under the relaxation of the
	/// dry/wet peak relationship the table's own low-level gain became the dominant source —
	/// MEASURED, a 0.251 sine at colour 0 reads 18.9 % THD at the neutral knob and 16.5 % with the
	/// knob at zero — so a THD RATIO now measures the table and reports it as this knob.
	///
	/// The claim worth keeping is about the OFFSET, and this is the five lines that let it be asserted
	/// as a value instead of inferred from a spectrum. Same reason `voiceFilterTargetForTest` and
	/// `voicePhaseForTest` exist: a quantity a signal carries among many cannot be recovered from the
	/// signal.
	[[nodiscard]] float hystOffsetForTest() const noexcept { return hystOffset_; }

	[[nodiscard]] float dcOffsetForTest(int channel) const noexcept {
		return channel_[static_cast<size_t>(channel)].dcOut;
	}

	/// THE NOTE-TRACKED LOWPASS CORNER, in Hz. Zero when the extra is off. It exists because the
	/// cutoff is derived from three things — the note, the field's slew intensity and the sample rate
	/// — and a filter's corner is the canonical example in this repo of a value not recoverable from
	/// its own output.
	[[nodiscard]] float lowpassCutoffForTest() const noexcept { return lowpassCutoff_; }
	[[nodiscard]] float trimGainForTest() const noexcept { return trimGain_; }

	/// THE INPUT FILTERS' CORNERS AS THE ENGINE RUNS THEM, in Hz — ZERO WHEN A FILTER IS OFF (backlog FA). A
	/// corner is not recoverable from the output, which is why this exists; the coefficient beside it is the
	/// number the filter actually multiplies by.
	[[nodiscard]] float inputHpfHzForTest() const noexcept { return inputHpfHz_; }
	[[nodiscard]] float inputLpfHzForTest() const noexcept { return inputLpfHz_; }
	[[nodiscard]] float inputHpfAlphaForTest() const noexcept { return inputHpfAlpha_; }
	[[nodiscard]] float inputLpfAlphaForTest() const noexcept { return inputLpfAlpha_; }
	/// THE CROSSFADE AS THIS FRAME HEARS IT — the glided value, not the knob (backlog FA).
	[[nodiscard]] float dryWetForTest() const noexcept { return dryWet_; }
	/// THE MEMORY AMOUNTS AND THE CHEBY CROSSFADE AS THIS FRAME HEARS THEM — glided, not the knobs. Where a glide
	/// comes to rest is a number no rendered buffer states, and "comes to rest exactly on the knob" is the claim.
	[[nodiscard]] float bloomForTest() const noexcept { return bloom_; }
	[[nodiscard]] float sagForTest() const noexcept { return sag_; }
	[[nodiscard]] float loopForTest() const noexcept { return loop_; }
	[[nodiscard]] float chebyMixForTest() const noexcept { return shapes_.chebyMix; }

	/// HOW MANY TABLE ENTRIES ONE SAMPLE OF AUDIO PAYS FOR.
	///
	/// PER SAMPLE, NOT PER BLOCK, AND THAT IS THE WHOLE CONTRACT. An earlier version spent a chunk per
	/// PROCESS CALL, floored at 128 and scaled by the frame count. It was wrong in a way only the
	/// registry's own sweeps could see: **the table then landed at a different point in the audio
	/// depending on how the host happened to partition it**, so the same samples split differently
	/// came out different. `test_fx_devices.cpp`'s "the same samples in a different partition are the
	/// same samples out" and `test_synth.cpp`'s block-size sweep both caught it — MEASURED at 0.183
	/// worst |diff| through the runner and 0.022 through the plugin.
	///
	/// A BUDGET SPENT INSIDE THE SAMPLE LOOP HAS NO SUCH FREEDOM: after N samples of audio exactly
	/// 2N entries have been built, whoever chopped the buffer up. Determinism here is not a nicety —
	/// a bounced mix and a live playback go through different block sizes.
	///
	/// TWO, MEASURED. An entry costs 0.125 us on this machine (16.0 us for 128); a frame at 48 kHz is
	/// 20.8 us of budget. Two entries a frame is 1.2% of it, at EVERY block size — which is the
	/// property the old per-block floor destroyed. A full 1025-entry table therefore lands 513 samples
	/// after it is asked for, which is 10.7 ms at 48 kHz.
	static constexpr size_t kBuildEntriesPerFrame = 2;

	/// HOW LONG A TABLE SWAP TAKES TO CROSS, in samples. Long enough that a discontinuity between two
	/// curves is a slide rather than a step, short enough that a drag does not feel lagged: at 48 kHz
	/// this is 10.7 ms, and a build takes 9 blocks anyway.
	static constexpr int kFadeSamples = 512;

private:
	/// PER-CHANNEL SIGNAL STATE. Two of everything, because the comb, the slew stage and the
	/// sub-harmonic all carry history and L and R must not share it — a shared zero-crossing counter
	/// would make the sub-harmonic's sign depend on which channel was processed last, which is a
	/// stereo image that moves with the buffer layout.
	struct ChannelState {
		float lowpass = 0.0f;    ///< the lowpass's output
		float integrator = 0.0f; ///< the integrator's accumulator, cleared at zero crossings
		float prevInput = 0.0f; ///< the sample the comb echoes and the zero-crossing test compares
		float prevAbs = 0.0f;   ///< hysteresis: the previous |x|, so the direction is "louder or quieter"
		/// PERSISTS ACROSS BLOCKS, where the firmware's restarts at zero every buffer (Shaper parity
		/// sweep, 2026-09-16): there the comb and zero-crossing rate are phase-locked to the buffer,
		/// here they run free at fs/stride whatever the host's block is — the repo's block-size rule,
		/// kept on purpose and said here rather than left as a silent divergence.
		int32_t strideCounter = 0;
		int32_t zcCount = 0;
		float subSign = 1.0f;
		float dcIn = 0.0f;  ///< DC blocker: last input
		float dcOut = 0.0f; ///< DC blocker: last output
		float emphasisIn = 0.0f;  ///< the pre-emphasis section's state
		float emphasisOut = 0.0f; ///< the de-emphasis section's state
		float envelope = 0.0f;     ///< the memory stages' peak follower — Bloom and Sag read it
		float loopResidue = 0.0f;  ///< Loop: what the curve took away, one-pole lowpassed
		float inputHpfLow = 0.0f; ///< the input highpass: the low part it subtracts. Zero while the filter is off
		float inputLpf = 0.0f;    ///< the input lowpass's output. Follows the input while the filter is off
	};

	/// One block's worth of housekeeping: derive the field, advance the build, recompute the blend.
	void beginBlock(int frames);
	/// Decide whether a build is owed and latch its parameters. Called once per block, because that
	/// is when settings arrive — the SPENDING is per sample, below.
	/// THE DRIVE METER'S ONE WRITER — see the definition.
	void meterInput(float x);
	/// THE CURVE STAGE — threshold split, extras, read — on one sample that the memory stages have
	/// already offset and scaled. Returns the pre-output value; `processSample` wraps it.
	float stage(float x, ChannelState& state);
	/// The same stage with no state and no extras: what the curve alone does to one value. Bloom reads
	/// it to take its offset back out through the curve.
	[[nodiscard]] float staticStage(float x) const;

	void maybeStartBuild();
	/// Spend one sample's worth of build budget; normalise and swap when the last entry lands.
	void advanceBuild();
	/// Advance everything that belongs to a FRAME rather than to a channel — the build budget, the
	/// crossfade, and the smoothed sweep.
	///
	/// IT IS A SEPARATE STEP BECAUSE STEREO CALLS `processSample` TWICE PER FRAME. The drive smoothing
	/// used to live inside `processSample`, which advanced it at twice the rate on the stereo road —
	/// so the mono and stereo roads drifted apart, AND the left and right channels of the same frame
	/// were driven by two different values. Caught by `test_fx_devices.cpp`'s "EVERY device with a MONO
	/// road: it is the stereo road's left channel, sample for sample", at 0.418 worst |diff|.
	void advanceFrame();
	/// The per-sample path, shared by the mono and stereo roads so there is one of it.
	[[nodiscard]] float processSample(float x, int channel);
	/// THE SHAPED EXCESS: `a·T(r·e)`, read from the front table, or crossed from the back one
	/// while a swap is fading. `excess` is signed — its sign picks the table's half — and is how far
	/// past the threshold the sample is, in input units. Each table carries its own `r` and `a`,
	/// derived from its own bound, so a fade between two tables is a fade between two reads that
	/// each satisfy the ceiling on their own.
	[[nodiscard]] float shape(float excess) const;
	/// One table's read of one excess. `bound` is that table's own.
	[[nodiscard]] float readTable(size_t which, float excess) const;
	/// Remove the offset a normalised table leaves behind. See shaper.cpp — it fixes a measured
	/// -19 dBFS of DC on silence, not a theoretical concern.
	/// THE INPUT FILTERS (backlog FA): the highpass, then the lowpass, each passing the sample untouched while off.
	float inputFilters(float x, ChannelState& state) const;
	float blockDc(float x, ChannelState& state);
	/// THE OUTPUT STAGE: the blocker, then the ceiling. Every sample leaves through here.
	float toOutput(float y, ChannelState& state);


	Settings target_ {};
	/// Whether the last block ran — so the first bypassed block clears the running state, once
	/// (backlog EH, shape 6; the multimode filter's `active_`).
	bool running_ = false;
	/// Whether the smoothed threshold and blend slope have been snapped to their first target — the
	/// firmware's "snap on first use", so a device does not start silent and glide in. False after a
	/// reset, so "as if just switched on" holds for the mix staging too.
	bool mixPrimed_ = false;
	/// Where the smoothed threshold and blend slope are heading, and the per-frame step toward it.
	float thresholdTarget_ = 1.0f;
	float blendSlopeTarget_ = 0.0f;
	float mixCoeff_ = 1.0f;
	CurveParams curve_ {};

	/// **THE TABLES AND THEIR BUILD, AS ONE BANK (backlog OM: force one table for all voices).**
	/// Owned by the instance that builds it — the bus instance, or any instance off the per-voice road. A VOICE
	/// instance owns none (`setVoice` frees it, on the message thread) and reads its bus instance's, which the host
	/// hands it every block (`AudioOps::pullHandBlocks`) and which the bus maintains while its voices run
	/// (`AudioOps::beginSharedBlock`). So under Poly every voice plays ONE table, built from the device's own row.
	struct TableBank {
		/// TWO TABLES AND AN INDEX. `front` is what the audio path reads.
		std::array<std::array<float, kTableEntries>, 2> table {};
		/// EACH TABLE'S OWN BOUND, computed when it lands and read beside it. The identity's is {1, 1},
		/// which is what a default-constructed one holds, so the never-built tables are read correctly.
		std::array<TableBound, 2> bound {};
		size_t front = 0;
		TableExtent extent {};
		size_t cursor = 0;
		/// FRACTIONAL ENTRIES CARRIED BETWEEN SAMPLES, so a non-integer rate would still be exact. At two
		/// per frame it is always zero; it exists so the rate is a number rather than a special case.
		float budget = 0.0f;
		bool building = false;
		/// HAS A REAL TABLE EVER BEEN BUILT? The tables start as the IDENTITY and `built` starts as a
		/// default `Settings`, which together tell a lie: that the identity table is the curve those
		/// settings produce. Without this flag a device whose settings happen to equal the defaults would
		/// decide it had nothing to build and would play the identity forever.
		bool everBuilt = false;
		int rebuilds = 0;
		Settings built {};   ///< the settings the FRONT table was built from
		Settings latched {}; ///< the settings the IN-FLIGHT build is being built from
		/// FADE POSITION, 1.0 WHEN SETTLED. While below 1 the audio path reads both tables.
		float fade = 1.0f;
		size_t fadeFrom = 0;
	};
	std::unique_ptr<TableBank> own_ = std::make_unique<TableBank>();
	/// The bank this instance reads: its own, or (a voice) its bus instance's. Null on a voice not yet handed one.
	TableBank* bank_ = own_.get();
	/// THE BUS INSTANCE A VOICE READS ITS TABLE AND ITS BAKED SETTINGS FROM, or null off the shared road.
	const Shaper* shared_ = nullptr;
	/// A VOICE'S OWN FADE THROUGH THE SHARED BANK'S CROSSFADE: taken from the bank at each block's start and stepped per
	/// sample here, never written back — only the owner moves the bank.
	float voiceFade_ = 1.0f;
	[[nodiscard]] float& fadeRef() noexcept { return shared_ != nullptr ? voiceFade_ : bank_->fade; }
	[[nodiscard]] float fadeRef() const noexcept { return shared_ != nullptr ? voiceFade_ : bank_->fade; }
	/// Build the whole table now, no amortising and no crossfade — what a shared bank does when its voices would
	/// otherwise play the identity (a fresh or reset bus instance under Poly). Allocation-free; ~one rebuild's work.
	void buildNow();

	/// SNAP THE SMOOTHED GAIN ON THE NEXT BLOCK — set by `reset()` and by construction. See
	/// `beginBlock`: it is what makes "as if just switched on" true of the drive ramp as well as of
	/// the tables, and it cannot be done in `reset()` itself because the settings that decide the
	/// target usually arrive AFTER the reset.
	bool snapNext_ = true;
	bool linear_ = true;
	/// HOW MUCH OF THE TABLE REACHES THE OUTPUT — 0 at knee 0, 1 from `kKneeFadeTop` up.
	float kneeFade_ = 1.0f;
	float ceilingExcess_ = 0.0f; ///< see ceilingExcessForTest
	/// PEAK INPUT, DECAYING — how hard the table is being driven. See `meterInput`.
	float drivePeak_ = 0.0f;
	/// PER-SAMPLE FALL, computed in `prepare` from `kDriveFallDbPerSecond` — never a literal, because a
	/// per-sample constant hides a sample-rate dependency.
	float driveDecay_ = 0.99995f;
	/// THE LAST MIXED SAMPLE BEFORE `blockDc` — the value `staticTransferAt` can be compared against.
	float preBlock_ = 0.0f;

	float fadeStep_ = 0.0f;

	double sampleRate_ = 48000.0;
	/// THE CORNER'S REFERENCE WITH NO NOTE SOUNDING — the firmware's `kLpfRefFreq`, A2, which puts
	/// the corner in 220..440 Hz. DECLARED ONLY HERE: `shaper.cpp` carried a `kLpfRefFreq` constant
	/// holding this same 110 and used it nowhere, so the fact had two declarations and one reader.
	/// clang caught it as an unused constant — and macOS builds these with `-Werror`, so it would
	/// have failed the mac build rather than merely being untidy.
	float noteHz_ = 110.0f;

	//-- per-block derived ---------------------------------------------------------------------//
	float sweep_ = 1.0f; ///< how many times faster than the unit-slope minimum the table is read
	float sweepTarget_ = 1.0f;
	float sweepAlpha_ = 0.001f;
	float threshold_ = 1.0f; ///< input units; `|x|` above it is shaped
	float headroom_ = 0.0f;  ///< 1 - threshold_: what a shaped sample may add to the threshold
	float blendSlope_ = 0.0f;
	float hystOffset_ = 0.0f; ///< in EXCESS units, signed by the field; see beginBlock
	float subBoost_ = 0.0f;
	float lowpassAlpha_ = 0.0f;
	/// The input trim as a linear gain — `Param::Trim` in dB, resolved once a block.
	float trimGain_ = 1.0f;
	/// THE INPUT FILTERS (backlog FA), at the block rate: each one's corner in Hz (0 off) and its one-pole
	/// coefficient. See `Param::InputHpf`.
	float inputHpfHz_ = 0.0f;
	float inputLpfHz_ = 0.0f;
	float inputHpfAlpha_ = 0.0f;
	float inputLpfAlpha_ = 0.0f;
	/// THE CROSSFADE, glided per frame on the threshold's half-life and primed with it. See `Param::DryWet`.
	float dryWet_ = 1.0f;
	/// EMPHASIS: the tilt into the curve and its exact inverse out of it, from `target_.emphasisDb` at
	/// the block rate. See `Param::Emphasis` and `tiltSection`.
	TiltSection emphasisIn_ {};
	TiltSection emphasisOut_ {};
	/// THE MEMORY STAGES (backlog IE): per-frame smoothed amounts, the envelope's two coefficients and
	/// the loop's darkening pole, the last three set in `prepare` from the sample rate.
	float bloom_ = 0.0f;
	float sag_ = 0.0f;
	float loop_ = 0.0f;
	/// THE READ SHAPES — Stair and Cheby, applied at the read (backlog IM). `shapes_` is what the read
	/// uses; `shapesTarget_` is this block's, which the Cheby amount glides toward per frame.
	ReadShapes shapes_ {};
	ReadShapes shapesTarget_ {};
	float envAttack_ = 1.0f;
	float envRelease_ = 1.0f;
	float loopAlpha_ = 1.0f;
	/// THE CURVE'S SLOPE AT THE ORIGIN, floored at one and re-read each block — Loop's residue divides by it.
	float loopSlope_ = 1.0f;
	/// THE STAGE'S OUTPUT JUST ABOVE ZERO, re-read each block. 0 since backlog IF took the Mix-1 step out;
	/// the memory stages still measure from it, so a curve that steps at zero cannot move under them.
	float silence_ = 0.0f;
	/// **TRIM, PER FRAME, WHILE A ROUTED SOURCE MOVES IT** (backlog FK): the next call's window (`setTrimRun`), taken by that
	/// call; each frame then sets `trimGain_` from its own value by the one law, `10^(dB/20)`.
	const float* pendingTrimRun_ = nullptr;
	float lowpassCutoff_ = 0.0f;
	float integratorBlend_ = 0.0f;
	float feedbackAmount_ = 0.0f;
	float dcPole_ = 0.9987f;
	int32_t stride_ = 64;
	int32_t subRatio_ = 2;
	int32_t rotation_ = 0;
	bool lowpassActive_ = false;
	bool integratorActive_ = false;
	bool subActive_ = false;
	bool feedbackActive_ = false;

	std::array<ChannelState, 2> channel_ {};
};

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in shaper.cpp.
extern const device::AudioOps kAudioOps;

} // namespace feathers::fx::shaper
