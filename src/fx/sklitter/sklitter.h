/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#pragma once

#include "device/device.h"
#include "fx/sklitter/engage.h"
#include "fx/sklitter/ring.h"
#include "fx/sklitter/slice_plan.h"
#include "fx/sklitter/sklitter_types.h"
#include "fx/sklitter/tripwire.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// **FORWARD ONLY, AND THE INCLUDE GRAPH IS THE REASON.** `rack::InnerChain` lives above this device
// (see `feathers-fx-sklitter` in CMakeLists.txt): `feathers-rack` links `feathers-fx`, so a header
// here that reached into `rack/` would put the host inside its own tenant. `sklitter.cpp` is in the
// target that can see both; this header carries a pointer and nothing else, which is also what keeps
// `sklitter.h` cheap for the five suites and the cards that include it.
namespace feathers::rack {
class InnerChain;
}

// **AND THE COUNTER THE CHAINS BORROW, FORWARD-DECLARED FOR THE SAME REASON** (gap G3). It is held by
// `unique_ptr` and never touched in this header, so a declaration is enough — `Sklitter`'s destructor
// is out of line in `sklitter.cpp`, which is what makes an incomplete type legal here. Including
// `backplane/graph.h` instead would pull the graph into the five suites and the cards that include
// this file, for a member none of them name.
namespace feathers::backplane {
class InstanceCounter;
}

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// SKLITTER, THE DEVICE. CLASSIC ONLY SO FAR.                                                          //
//--------------------------------------------------------------------------------------------------//
//
// Task 6 of the port plan is a VERTICAL SLICE: the whole host road -- ring, Engage, transport, the
// rate mapping, registration -- proved end to end through the one mode that needs no phi and no hash.
// The seven remaining modes land in task 7 against a pinned slice schedule.
//
// The surface mirrors `fx::owltt::Owltt` exactly, plus automod's `setTransportClock`. Both are
// existing roads; neither is new for this device.

class Sklitter {
public:
	/// **ONE GRAIN'S TWO FADE LENGTHS** — the firmware's `attackFadeLen` and `decayFadeLen`, with
	/// `Min Env` raising the floor. Public because the test seams below return one. See the .cpp.
	struct GrainFade {
		size_t attack = 0;
		size_t decay = 0;
	};

	/// **DECLARED, NOT DEFAULTED, BECAUSE THIS DEVICE HOLDS A CHAIN.** `postChain_` is a pointer to an
	/// incomplete type here, so the special members have to be defined where it is complete — in
	/// `sklitter.cpp`, which is the only translation unit that can see `rack/inner_chain.h`.
	Sklitter();
	~Sklitter();
	/// **MOVABLE, NOT COPYABLE, AND THIS IS A NARROWING OF WHAT IT WAS.** It used to be implicitly
	/// copyable; a `unique_ptr` member ends that, and a user-declared destructor would have suppressed
	/// the implicit MOVE as well — which would break the test fixtures that build a driven device in a
	/// helper and `return s;`. The move is declared for them and defined out of line, where
	/// `rack::InnerChain` is complete. A moved-from device has no chain; `runPostChain` reads the
	/// pointer rather than assuming one, for exactly that reason.
	Sklitter(const Sklitter&) = delete;
	Sklitter& operator=(const Sklitter&) = delete;
	Sklitter(Sklitter&&) noexcept;
	Sklitter& operator=(Sklitter&&) noexcept;

	/// The rate and the host's longest block (`device::AudioOps::prepare`); sizes every scratch buffer below.
	void prepare(double sampleRate, int maxFrames);
	void reset();
	void setTarget(const Settings& settings);
	void setTransportClock(const device::TransportClock& clock);
	/// **THE DEVICE'S BLOCK: the render, then the post chain over what it left.** Two lines, and the
	/// split is the whole of trap T3 for this device — `renderStereo` has FIVE early returns (bad
	/// frames, bypassed, idle, and two inside the recording road) and every one of them is a return
	/// INTO this function rather than past it. Putting the chain at the bottom of the render instead
	/// would have skipped it on four of the five, which is the inner editor frozen in exactly the
	/// states a player reaches.
	void processStereo(float* left, float* right, int frames);

	//----------------------------------------------------------------------------------------------//
	// THE POST CHAIN — backlog EW, the inner effects lane on the summed output.                     //
	//----------------------------------------------------------------------------------------------//
	//
	// **AFTER THE BLEND, WHICH IS DECISION D2 AND NOT A SHORTCUT.** This device blends its dry and wet
	// PER SAMPLE and IN PLACE (`blendAt`, called from all four renderers' write-backs), so a chain on
	// the WET path alone — musically the more useful answer — would need a wet scratch buffer and a
	// rewrite of every one of those write-backs. "After the blend" is what "post" honestly means, it is
	// a six-line insertion, and the chain carrying its OWN mix (`rack::InnerChain::setMix`) costs
	// nothing structural and does not foreclose the wet-only road later.
	//
	// **AND IT RUNS EVEN WHILE THIS DEVICE'S OWN `Enabled` IS DOWN.** `RackEngine::processBus` calls
	// `processStereo` unconditionally — `traits.enableControl` gates the modulation OUTPUTS and not the
	// audio call — so a reverb in the chain keeps its tail when the stutter is switched off. That is
	// trap T6 avoided by construction rather than by a fade.
	//
	// **ONE CHAIN.** The grain lane EW also asks for is NOT here and no framing anticipates it: two
	// bodies inside one `ContainerOps::saveState` (gap G2) and a lane argument on the route verbs
	// (gap G3) are both design decisions nobody has made.
	//
	// **NO ON/OFF AND NO MIX CONTROL, AND THAT IS A DEFERRAL RATHER THAN A DESIGN.**
	// On/off and mix are skipped for now, as in Feathers. So the chain runs
	// enabled at unity mix and no parameter drives either — the capability is on `rack::InnerChain`
	// (`setEnabled`, `setMix`, `setGain`) and nothing here calls it. It is safe as a default because a
	// chain holding nothing is a true pass bit for bit, not because "on" was judged the right value;
	// the value has not been judged at all yet.
	//
	// **AND WHEN THEY ARRIVE THEY MUST BE WIRED, NOT DRAWN.** CLAUDE.md's honesty rule: a control is
	// enabled iff it changes sound through a real param. A default with no parameter is fine and is
	// what this is; a KNOB with no parameter is the one thing that must never appear here to fill the
	// gap this paragraph describes.

	/// THE CHAIN — always present on a live device, and null only on a MOVED-FROM one. Message thread;
	/// what the container ops and the chain's editor reach through.
	[[nodiscard]] rack::InnerChain* postChain() noexcept { return postChain_.get(); }
	[[nodiscard]] const rack::InnerChain* postChain() const noexcept { return postChain_.get(); }
	/// **THE GRAIN LANE** — the per-grain one. See `grainChain_`.
	[[nodiscard]] rack::InnerChain* grainChain() noexcept { return grainChain_.get(); }
	[[nodiscard]] const rack::InnerChain* grainChain() const noexcept { return grainChain_.get(); }
	/// **WHETHER ANYTHING HELD WILL STILL COME OUT** — `AudioOps::holdsTail` (backlog MC). Engaged, the device plays its
	/// capture with no new input: a dry or gated slice is exactly silent with the bar still to come, and Recording and
	/// Standby turn into Playing on their own. So it holds while the gesture does AND the capture holds sound (a held
	/// loop sustains by design; one pWrite has filled with silence holds nothing). Its two inner chains answer for
	/// themselves. Off, the device has no effect at all.
	[[nodiscard]] bool holdsTail() const noexcept;

	/// TEST ONLY — how much of the playing slice is going down the grain lane, and which Grain voices
	/// are. **A claim about which grains took the detour is a claim about a VALUE** (CLAUDE.md, paid
	/// for four times): it cannot be recovered from a rendered buffer, because a buffer carries every
	/// quantity at once and a lane at unity gain sounds like no lane at all.
	[[nodiscard]] float grainSendForTest() const noexcept { return grainSend_; }
	[[nodiscard]] bool grainLaneVoiceForTest(bool voiceB) const noexcept {
		return voiceB ? grainLaneB_ : grainLaneA_;
	}

	/// **WHERE THE CARD'S PICTURE LEAVES THE AUDIO THREAD.** `device::ReadingsSink`, installed by the
	/// engine for every host; published once per block while there is anything to say. See
	/// `sklitter_types.h`'s `Reading` for what the numbers mean.
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }

	/// PUBLISH THE PICTURE — called once per block by the ops table, after the audio.
	void publishReadings();

	/// **THE TWO HELD LEVELS, AS `Reading::WetLevel` AND `Reading::GrainLevel` WILL PUBLISH THEM.**
	/// VALUES, not a picture: the claim "the circle sparks at this rate in this colour" is a claim about
	/// two numbers, and CLAUDE.md's rule is that a number is asserted rather than inferred from a
	/// buffer. Maintained by the render itself, so a caller that drives `processStereo` directly — which
	/// every one of this device's own suites does — reads the same figures the host's sink would carry.
	///
	/// **THEY PARTITION: `wetLevel()` is what did not take the grain lane, `grainLevel()` is what did.**
	/// See `Reading::WetLevel` for why that is exact and why it has to be.
	[[nodiscard]] float wetLevel() const noexcept { return wetLevelPeak_; }
	[[nodiscard]] float grainLevel() const noexcept { return grainLevelPeak_; }
	/// **HOW MANY GRAINS HAVE FIRED IN GATE CLASS `cls`, mod `kGrainOnsetWrap`** — what
	/// `Reading::GrainOnsets` publishes, maintained at the onset itself (`playGrain`'s voice wraps), so a
	/// direct caller reads the number the sink would carry. See `Reading::GrainOnsets`.
	[[nodiscard]] uint32_t grainOnsetCount(size_t cls) const noexcept {
		return cls < kGrainOnsetClasses ? grainOnsets_[cls] : 0u;
	}
	/// HOW MANY TIMES REPEAT HAS RESTARTED ITS SLICE, and the period of the last one, seconds — what
	/// `Reading::RepeatCount` / `RepeatPeriod` publish.
	[[nodiscard]] uint32_t repeatCount() const noexcept { return repeats_; }
	[[nodiscard]] float repeatPeriod() const noexcept { return repeatPeriod_; }

	//----------------------------------------------------------------------------------------------//
	// THE MOD ROAD — `kModOps`, the second op table over this same object. Design §4.               //
	//----------------------------------------------------------------------------------------------//
	//
	// SKLITTER is the registry's first device that is BOTH an audio device and a modulation source. The
	// engine builds and prepares it through `kAudioOps` (one storage, taken from the first pointer in a
	// fixed order) and then, every block, hands the SAME resolved row to both tables: `kAudioOps.setFields`
	// -> `setTarget`, and `kModOps.setFields` -> `setPerformFields`.
	//
	// **THE TWO ROADS DO NOT SHARE A WRITER, AND THAT IS DELIBERATE.** The audio road owns `target_`; the
	// mod road decodes the puck into `puckX_`/`puckY_` and nothing else. A second writer of `target_` is a
	// second writer however equal the values look — and the values are made to agree instead by the one
	// sanitiser in `performWeightsOf`, which a case pins.

	/// **THE PUCK ONLY**, from the device's own row, and this must not call `setTarget`. Publishes
	/// `Perf X`, `Perf Y` and the four weights at once, from ONE `PerformWeights` — see `performOutput`.
	void setPerformFields(const float* fields, size_t fieldsPresent) noexcept;

	/// ADVANCE `Perf Speed` BY ONE `kPerformTickSamples` BLOCK. Design §4.5: an exponential moving average
	/// of |Δpuck|/Δt over `kSpeedTauSeconds`, NOT a peak-follower.
	void tickPerform() noexcept;

	/// WHAT AUX OUTPUT `n` IS PUBLISHING. Out of range reads 0, which is `ModOps::outputAt`'s contract for
	/// an index past `Descriptor::outputs`.
	[[nodiscard]] float performOutput(size_t output) const noexcept;

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS. **A CLAIM ABOUT A VALUE CANNOT BE INFERRED FROM A RENDERED BUFFER.**              //
	//----------------------------------------------------------------------------------------------//
	//
	// A comparison between two renders proves that SOMETHING changed and never that the right thing
	// did -- and every instance of that failure in this repo has been a PARTIAL effect surviving
	// where a total one would have been caught. "Which slice is playing, at what position, in which
	// direction" is exactly such a claim, and it is not recoverable from audio that carries every
	// quantity at once. Five lines each, per the rule that the remedy is cheap and the cleverness is
	// not.

	[[nodiscard]] Status statusForTest() const noexcept { return engage_.status(); }
	/// TEST ONLY — how much of the buffer the gesture has captured (a warm press starts with what the
	/// pre-warm held; a re-press during a capture keeps it).
	[[nodiscard]] size_t capturedFramesForTest() const noexcept { return engage_.capturedFrames(); }
	/// The slice length the rate and the tempo resolved to. Not recoverable from the output: two
	/// different lengths both produce "some audio".
	[[nodiscard]] size_t sliceFramesForTest() const noexcept { return sliceFrames_; }
	/// THE CAPTURED LOOP, which is a different number from the bar since `Param::BufferLength`. The
	/// picture normalises its offsets against THIS, so a case restating the arrangement needs it.
	[[nodiscard]] size_t bufferFramesForTest() const noexcept { return bufferFrames_; }
	/// **THE LENGTHS THE LAST PICTURE WALK DIVIDED BY** — the key's, which is what the band was drawn
	/// against. At rest these are what the settings imply and may differ from the two above (backlog IB),
	/// and a band's sources alone cannot say which bar they were normalised by.
	[[nodiscard]] size_t pictureBarFramesForTest() const noexcept { return restKey_.barFrames; }
	[[nodiscard]] size_t pictureBufferFramesForTest() const noexcept { return restKey_.bufferFrames; }
	/// **THE FOUR FLOORS THAT WERE 44.1 kHz SAMPLE COUNTS**, in the host's frames. Read the VALUES: a
	/// floor that bites only at extremes is not recoverable from ordinary output, and "half the intended
	/// duration at 96 kHz" is a claim about a number.
	[[nodiscard]] size_t minBurstCycleForTest() const noexcept { return minBurstCycle_; }
	[[nodiscard]] size_t minBurstGrainForTest() const noexcept { return minBurstGrain_; }
	[[nodiscard]] size_t minSliceFramesForTest() const noexcept { return minSliceFrames_; }
	[[nodiscard]] size_t minSubSliceFramesForTest() const noexcept { return minSubSliceFrames_; }
	[[nodiscard]] size_t subSliceLengthForTest(size_t audible, size_t subdivisions) const {
		return subSliceLength(audible, subdivisions);
	}
	[[nodiscard]] size_t minGrainFramesForTest() const noexcept { return minGrainFrames_; }
	/// **THE ENVELOPE FLOOR, EVALUATED DIRECTLY.** Its shape is a claim about a GAIN at a position, and a
	/// rendered grain carries the signal and the envelope multiplied together — the one thing a buffer
	/// cannot be asked to separate. `pos` is frames since the grain began, `remaining` frames until its
	/// audible end.
	/// The next call's Mix and Stereo Width windows, or null for a control not moving — see `mixNow_`.
	void setWindowRuns(const float* mix, const float* width) noexcept {
		pendingMixRun_ = mix;
		pendingWidthRun_ = width;
	}
	[[nodiscard]] float envGainForTest(size_t pos, size_t remaining) {
		envPos_ = pos;
		// **THE REAL FADE LENGTHS FOR THIS GRAIN**, so the seam exercises `grainFadeLengths` — the
		// firmware's envelope with `Min Env` as its floor — rather than a ramp invented for the test.
		const GrainFade f = grainFadeLengths(pos + remaining);
		// **A WET OF 1 IS THE GAIN ITSELF, and it no longer has to borrow `mixNow_` to say so.** Until
		// the blend split, the only road to the envelope ran through a function that also applied Mix
		// and the engage fade, so this seam forced the mix to 1 and put it back. `wetOf` IS the
		// envelope, which is what this accessor was always asking for.
		return wetOf(1.0f, remaining, f.attack, f.decay);
	}
	/// THE TWO LENGTHS THEMSELVES, for a case about the envShape asymmetry and the floors.
	[[nodiscard]] GrainFade grainFadeForTest(size_t gated) const { return grainFadeLengths(gated); }
	/// The plan is what carries `envShape`; a case asserting the asymmetry has to be able to set it.
	void setPlanEnvShapeForTest(float shape) { plan_.envShape = shape; }
	[[nodiscard]] size_t envFramesForTest() const noexcept { return envFrames_; }
	/// Whether the ring already holds a beat-aligned bar, so a gesture need not wait for one.
	[[nodiscard]] bool prewarmReadyForTest() const noexcept { return prewarmReady(); }
	[[nodiscard]] size_t prewarmedFramesForTest() const noexcept { return prewarmed_; }
	/// The idle head's last measured disagreement with the song (frames) and how often it re-seated.
	[[nodiscard]] long long prewarmDriftForTest() const noexcept { return prewarmDrift_; }
	[[nodiscard]] size_t prewarmReseatsForTest() const noexcept { return prewarmReseats_; }

	/// THE PITCH RATIO THE CURRENT SLICE IS BEING READ AT. 1.0 outside Pitch mode.
	[[nodiscard]] double pitchRatioForTest() const noexcept { return pitchRatio_; }

	/// The anti-click state, for a venue that must assert the MECHANISM rather than listen for a click.
	[[nodiscard]] bool waitingForZeroCrossForTest() const noexcept { return waitingForZeroCrossL_; }
	[[nodiscard]] bool releaseMutedForTest() const noexcept { return releaseMutedL_; }
	[[nodiscard]] bool consecutiveForTest() const noexcept { return consecutive_; }
	[[nodiscard]] bool nextConsecutiveForTest() const noexcept { return nextConsecutive_; }
	/// Whether the anti-click gate is running for the grain now playing.
	[[nodiscard]] bool zcActiveForTest() const noexcept { return zcActive_; }
	/// Whether the output is still crossfading in from the dry after a trigger.
	[[nodiscard]] bool engageFadingForTest() const noexcept { return engageFadePos_ < engageFadeFrames_; }
	/// Which bar of the loop is playing — the multi-bar evolution counter.
	[[nodiscard]] int32_t barIndexForTest() const noexcept { return barIndex_; }
	/// Whether the slice now playing is dry — density's decision is per slice, not a mix.
	[[nodiscard]] bool sliceIsDryForTest() const noexcept { return sliceIsDry_; }
	/// The per-slice pan, as the render sees it: 0 when the slice does not pan.
	/// Burst's cycle and grain, for a venue that must assert the gate rather than listen for it.
	[[nodiscard]] size_t burstGrainForTest() const noexcept { return burstGrain_; }
	/// TEST ONLY — where Burst's slice begins in the ring, and how long it is.
	[[nodiscard]] size_t sliceOriginForTest() const noexcept { return sliceOrigin_; }
	[[nodiscard]] size_t capturedSliceFramesForTest() const noexcept { return capturedSliceFrames_; }
	/// Whether the slice now playing writes back, and where the linear write head is.
	[[nodiscard]] bool pWriteThisSliceForTest() const noexcept { return pWriteThisSlice_; }
	/// Repeat's own 0..7 plan counter.
	[[nodiscard]] int32_t repeatLoopIndexForTest() const noexcept { return repeatLoopIndex_; }
	[[nodiscard]] size_t linearBarPosForTest() const noexcept { return linearBarPos_; }
	/// The song beat the pre-warm anchored ring frame 0 to, or `INT64_MIN` before one did (backlog GU).
	[[nodiscard]] long long prewarmBeatForTest() const noexcept { return prewarmBeat_; }
	[[nodiscard]] size_t burstCycleForTest() const { return burstCycleLength(); }

	//----------------------------------------------------------------------------------------------//
	// BACKLOG HN — THE DEFERRED-WRITE FEASIBILITY INSTRUMENT. TEST-ONLY, DEFAULT OFF.               //
	//                                                                                               //
	// Backlog GV needs pWrite's write-back to carry post-LANE material, and the lanes run a whole    //
	// block after the per-sample write (`renderChunk`: `renderStereo` → `grainChain_->process` →     //
	// `runPostChain`). The proposed fix is to RECORD each write's position, source and mix during    //
	// the render and PERFORM the writes after the lanes have returned. This switch implements only   //
	// the deferral — the source is unchanged — so that the deferral's exactness can be measured on   //
	// its own, before any feedback circuit is built on the assumption that it is exact.              //
	//                                                                                               //
	// AND THE AUDIT IS THE OTHER HALF, because the deferral is exact IF AND ONLY IF no playback read //
	// during a chunk lands on a position written earlier in that same chunk. `auditReads_` counts    //
	// exactly those. It is a separate instrument from the switch on purpose: the switch can be       //
	// proved bit-identical on material that never exercises the hazard, and then the identity means  //
	// nothing. Turn the audit on to find out whether the material could have expressed a difference. //
	//----------------------------------------------------------------------------------------------//

	/// Defer pWrite's ring writes to the end of the chunk instead of performing them per sample.
	void setDeferWritesForTest(bool on) noexcept { deferWrites_ = on; }
	/// Count playback reads that land on a position written earlier in the same chunk.
	void setReadAuditForTest(bool on) noexcept { auditReads_ = on; }
	/// How many such reads have been seen since `resetReadAuditForTest`.
	[[nodiscard]] size_t auditReadHitsForTest() const noexcept { return auditHits_; }
	/// How many playback reads were examined at all — the denominator, and the vacuity check.
	[[nodiscard]] size_t auditReadsForTest() const noexcept { return auditReads1_; }
	/// How many ring writes were recorded — the second vacuity check: no writes, no hazard.
	[[nodiscard]] size_t auditWritesForTest() const noexcept { return auditWrites_; }
	void resetReadAuditForTest() noexcept {
		auditHits_ = 0;
		auditReads1_ = 0;
		auditWrites_ = 0;
	}

	/// **RIBBON TRUTH — WHAT THE LOOPER READ, SAMPLE BY SAMPLE.** The band claims where a slot reads, which
	/// way, and how often it restarts; none of those is recoverable from an output that carries envelopes
	/// and pans (CLAUDE.md: assert the value). One entry per sample of the block: the ring frame of the
	/// sample's MAIN read (the last primary read, so a wrap crossfade's tail is not it; −1 where nothing was
	/// read — a dry slot), and the slot and sub-piece the looper was in. The caller owns the array, sized to
	/// its largest block, and refills it before each block; null (the default) costs one branch a sample.
	struct ReadTraceEntry {
		long long at = -1;
		int32_t slice = -1;
		size_t position = 0; ///< `positionInSlice_` at the sample
		size_t sub = 0;      ///< the ratchet piece the looper was in
		bool dry = false;
		/// **WHAT THE RING HELD AT `at` WHEN IT WAS READ, AND WHERE THE WRITE HEAD WAS** (backlog JC) —
		/// the frame's left value before interpolation, and `linearBarPos_` before this sample's write.
		/// Together they say whether a read behind the head heard what the head wrote there.
		float heardL = 0.0f;
		size_t head = 0;
		/// The wet gain the crossing duck applied at this sample: 1 away from the head. See `playLooper`.
		float duck = 1.0f;
	};
	void setReadTraceForTest(ReadTraceEntry* trace, size_t length) noexcept {
		readTrace_ = trace;
		readTraceLength_ = length;
	}
	/// **THE SAME, FOR GRAIN'S TWO VOICES** (backlog JC) — the ring frame each voice read, its grain's
	/// position and flags, and its window. A held read is invisible in the output because the window
	/// rescales it every sample; the frames are not.
	struct GrainTraceEntry {
		long long atA = -1;
		long long atB = -1;
		size_t posA = 0; ///< `grainPosA_`: where voice A's grain sits in the buffer
		int32_t indexA = 0; ///< `grainIndexA_` while the sample played
		bool upA = false;
		bool upB = false;
		bool revA = false;
		float envA = 0.0f;
		float envB = 0.0f;
		size_t head = 0;    ///< `linearBarPos_` before this sample's write
		float duckA = 1.0f; ///< each voice's crossing-duck gain — see `playGrain`
		float duckB = 1.0f;
		bool dryA = false;
		bool dryB = false;
	};
	void setGrainTraceForTest(GrainTraceEntry* trace, size_t length) noexcept {
		grainTrace_ = trace;
		grainTraceLength_ = length;
	}

	//----------------------------------------------------------------------------------------------//
	// BACKLOG GU/GV — THE SEAM THAT MAKES THE FEEDBACK CLAIMS VALUE ASSERTIONS RATHER THAN          //
	// DIFFERENCE CHECKS. CLAUDE.md: *"A CLAIM ABOUT A VALUE CANNOT BE INFERRED FROM A RENDERED       //
	// BUFFER — ASSERT THE VALUE."* A feedback level is exactly the quantity a rendered buffer cannot //
	// be asked about: every pass carries it and so does everything else, and a comparison between    //
	// two renders goes green whenever ANYTHING differs. Five lines of accessor buys the number.      //
	//----------------------------------------------------------------------------------------------//

	/// The wet sample the feedback path last took, BEFORE damping — i.e. what the tap switch chose.
	[[nodiscard]] float feedbackTapForTest() const noexcept { return lastFbTapL_; }
	/// And the same sample AFTER the damping pair, which is what was actually blended into the ring.
	[[nodiscard]] float feedbackWrittenForTest() const noexcept { return lastFbDampedL_; }
	/// The write-head position the last flush wrote at, so a case can read the frame it landed in.
	[[nodiscard]] size_t lastWriteHeadForTest() const noexcept { return lastWriteHead_; }
	/// How many samples the last flush performed a DRY blend for, and a FEEDBACK blend for.
	/// **A COUNT PRODUCED BY THE WRITE ITSELF**, which a counter standing beside the call cannot be:
	/// delete either blend and the count it returns goes to zero with it.
	[[nodiscard]] size_t dryWritesForTest() const noexcept { return lastDryWrites_; }
	[[nodiscard]] size_t feedbackWritesForTest() const noexcept { return lastFbWrites_; }
	/// `P Write`'s recorded source and amount at the last flushed sample — the third blend's two
	/// unknowns, without which the composition claim can only be made about the first two.
	[[nodiscard]] float pWriteSrcForTest() const noexcept { return lastPwSrcL_; }
	[[nodiscard]] float pWriteMixForTest() const noexcept { return lastPwMix_; }
	/// The damping coefficients, so a case can restate them from the declared FREQUENCIES and the
	/// host's rate — the 44.1-kHz-constant-used-at-96 defect this file has already paid for four times.
	void dampCoeffsForTest(float* lowpass, float* dcPole) const noexcept {
		*lowpass = dampA_;
		*dcPole = dcR_;
	}
	/// Run the real damper, on the real state, one sample. Lets a case restate the filter itself
	/// rather than infer it from a buffer.
	void dampOneForTest(float inL, float inR, float* outL, float* outR) noexcept {
		dampFeedback(inL, inR, outL, outR);
	}

	[[nodiscard]] float panForTest() const noexcept {
		return panActive_ ? (panRight_ ? 1.0f - panFade_ : panFade_ - 1.0f) : 0.0f;
	}

	/// **THE CAPTURED LENGTH OVER THE LENGTH RATE ASKS FOR NOW** — 1.0 while Rate sits where it was
	/// pressed, 2.0 one division faster. Burst's cycle is the capture divided by it. Read the VALUE rather
	/// than inferring it from two buffers.
	[[nodiscard]] double rateStepForTest() const noexcept { return rateStep(); }

	/// WHERE RATE EFFECTIVELY SITS, after the press-time snap and any movement since.
	[[nodiscard]] double effectiveRateForTest() const noexcept { return effectiveRatePosition(); }
	[[nodiscard]] const Ring& ringForTest() const noexcept { return ring_; }
	/// backlog KX — the write gain the last chunk ended on: 1 unfrozen, 0 frozen, between them on the ramp.
	[[nodiscard]] float freezeWriteGainForTest() const noexcept { return freezeWriteGain_; }
	/// backlog MJ — whether the Record Gate let the last sample through (meaningless while the gate is off), and the
	/// record light as `Reading::Recording` publishes it: whether a write landed in the ring during the last call.
	[[nodiscard]] bool recordGateOpenForTest() const noexcept { return recordGateOpen_; }
	[[nodiscard]] bool recordingForTest() const noexcept { return recording_; }
	/// THE LONGEST PIECE `processStereo` RENDERS AT ONCE (backlog GO) — and 0 if any per-sample buffer disagrees
	/// with it, so a case reading this cannot pass on the length while one buffer was left at another size.
	[[nodiscard]] size_t scratchFramesForTest() const noexcept {
		const size_t n = scratchFrames_;
		for (const size_t len : { wetL_.size(), wetR_.size(), mixCoeff_.size(), sendL_.size(), sendR_.size(),
		                          captureScratchL_.size(), captureScratchR_.size(), deferPos_.size(), deferSrcL_.size(),
		                          deferSrcR_.size(), deferMix_.size(), auditWritten_.size(), rwPos_.size(),
		                          rwWhat_.size(), rwPwL_.size(), rwPwR_.size(), rwPwMix_.size(), fbTapL_.size(),
		                          fbTapR_.size(), freezeGain_.size() }) {
			if (len != n) {
				return 0;
			}
		}
		return n;
	}
	/// THE STEP OF THE BAR, THE PLAN IT PRODUCED, AND THE SCHEDULE IT RESOLVED TO. None of the three
	/// is recoverable from the output: two different slices both render "some audio".
	[[nodiscard]] int32_t sliceIndexForTest() const noexcept { return sliceIndex_; }
	[[nodiscard]] size_t positionInSliceForTest() const noexcept { return positionInSlice_; }
	/// **WHERE IN THE BAR THE PLAYING SLOT BEGAN (backlog IG)** — the swung start a case asserts by value;
	/// a start frame is not recoverable from the output any more than a slice index is.
	[[nodiscard]] size_t slotStartFrameForTest() const noexcept { return slotStartFrame_; }
	/// **THE TRANSPORT LOCK'S OWN ARITHMETIC, EXPOSED (backlog GT).** CLAUDE.md: *"a claim about a
	/// value cannot be inferred from a rendered buffer"*, and this feature is that rule's own example
	/// — the two mutations that gave it teeth were a transport lock's retrigger road deleted outright
	/// and a note's frequency replaced by a fixed one, both of which survived render comparisons.
	/// So the cursor the lock DERIVES is readable directly, beside the cursor it INSTALLS.
	[[nodiscard]] int32_t lockedSliceForTest(double atFrame) const noexcept {
		int32_t slice = -1;
		size_t within = 0;
		return lockedCursor(atFrame, &slice, &within) ? slice : -1;
	}
	/// The offset INSIDE that slice, the other half of the pair — under groove the two are the warp's
	/// inverse, and the slice alone cannot say whether the frame was placed by the warped boundary.
	[[nodiscard]] size_t lockedWithinForTest(double atFrame) const noexcept {
		int32_t slice = -1;
		size_t within = 0;
		return lockedCursor(atFrame, &slice, &within) ? within : static_cast<size_t>(-1);
	}
	[[nodiscard]] double lockedBarPhaseForTest(double atFrame) const noexcept {
		return lockedBarPhase(atFrame);
	}
	[[nodiscard]] int32_t lockedBarIndexForTest(double atFrame) const noexcept {
		return lockedBarIndex(atFrame);
	}
	[[nodiscard]] bool rejoinPendingForTest() const noexcept { return rejoinPending_; }
	/// **THE DUCK IS A VALUE, NOT A SHAPE IN A BUFFER.** Whether the wet is on its way to the dry ahead
	/// of an unlocked rejoin cannot be recovered from the output — every level in it is also a level the
	/// device reaches for other reasons — so the case that claims the duck ran reads it here.
	[[nodiscard]] bool rejoinDuckingForTest() const noexcept { return rejoinDucked_; }
	/// Whether a locked stop holds the device, and how many frames of its 5 ms ramp are still owed (backlog GT).
	[[nodiscard]] bool parkedForTest() const noexcept { return parked_; }
	[[nodiscard]] size_t parkLeftForTest() const noexcept { return parkLeft_; }
	[[nodiscard]] size_t wrapFadeForTest() const noexcept { return wrapFade_; }
	[[nodiscard]] int32_t sliceCountForTest() const noexcept { return numSlices_; }
	[[nodiscard]] const SlicePlan& planForTest() const noexcept { return plan_; }
	/// **THE SETTINGS THE DEVICE IS ACTUALLY HOLDING** — for the cases that ask what a HOST's row decoded
	/// to. `AudioOps::setFields` fills a row from `rowOf(Settings{})` and copies only the fields the host
	/// sent, so a short row's tail arrives as its DECLARED default; restating that arithmetic in a test
	/// would assert the restatement rather than the seam, and the perform tail cannot be heard from the
	/// outside at all while the puck is centred (the circle is neutral there by design).
	[[nodiscard]] const Settings& targetForTest() const noexcept { return target_; }
	/// **THE PERFORM CIRCLE'S ANSWER AT THE MOMENT THE PLAYING SLICE WAS DRAWN.** The blend is eight
	/// numbers and the plan keeps only thresholded decisions taken from them, so neither the plan nor the
	/// audio can be asked what the push WAS — a puck that reached the sound by the wrong road, or by half
	/// a road, renders a different buffer either way. CLAUDE.md: *"if the claim is 'this value is X at this
	/// instant', read the value."*
	///
	/// **`slice`, NOT `macroPush`, AND THE NAME IS THE WARNING.** Only `advanceSlice` stamps it, so in
	/// Grain — which draws its plans in `grainPlanFor` and never advances a slice — this returns a
	/// default-constructed push for ever. A name that said "the macro's push" invited exactly the reading
	/// that would make that a silent false negative; Grain's road is asked through `grainPlanForTest`.
	[[nodiscard]] const MacroPush& slicePushForTest() const noexcept { return lastPush_; }
	/// THE PUSH THE NEXT PLAN WOULD TAKE, from `target_` — anchored push and puck together (backlog HT).
	[[nodiscard]] MacroPush targetPushForTest() const { return targetPush(); }
	/// **THE ONE-AHEAD PEEK, ITS INDEX, AND WHETHER IT CROSSED A BAR** — the second plan `advanceSlice`
	/// draws, which nothing else publishes. It decides `nextConsecutive_` (a fade taken or skipped at the
	/// join), so a peek drawn from the wrong push is audible only as a click at a boundary, which is the
	/// least testable symptom this device has.
	///
	/// **`nextSlice`, LIKE `slicePushForTest`, AND FOR THE SAME REASON.** Only `advanceSlice` stamps these,
	/// so in Grain they stay at their defaults for ever — and `nextSliceIndexForTest`'s default of **0 is
	/// the worse of the two, because 0 is a PLAUSIBLE index rather than an obviously empty one**: a Grain
	/// case reading these would compare a default-constructed plan against a real one at index 0 and read
	/// the red as a perform bug. Grain's road is asked through `grainPlanForTest`.
	///
	/// **THE WRAP IS PUBLISHED RATHER THAN INFERRED, AND THAT IS NOT A CONVENIENCE.** A case must know
	/// whether the peek crossed a bar, because `advanceSlice` hands a wrapped peek a different
	/// `barIndex` and a restatement carrying the current one then disagrees for a reason that has nothing
	/// to do with the push. The obvious inference — compare the next index against `sliceIndexForTest` —
	/// **is false in Repeat**, where the peek's index is `(repeatLoopIndex_ + 1) & 0x7`, a counter
	/// unrelated to `sliceIndex_`, while the wrap is still decided by `sliceIndex_ + step >= numSlices_`.
	/// It would skip real peeks when that counter is low and admit wrapped ones when it is high. So the
	/// flag itself is published; and for the same reason the index below is the index the plan was DRAWN
	/// at, which is the next slice in every mode but Repeat, where it is that repeat counter.
	[[nodiscard]] const SlicePlan& nextSlicePlanForTest() const noexcept { return nextPlan_; }
	[[nodiscard]] int32_t nextSliceIndexForTest() const noexcept { return nextGrainIndex_; }
	[[nodiscard]] bool nextWrapsForTest() const noexcept { return nextWraps_; }
	[[nodiscard]] const SliceSchedule& scheduleForTest() const noexcept { return schedule_; }
	/// **HOW MANY MORE SLOTS REPLAY THE PLAYING SLOT'S PLAN** — the reference's `scatterRepeatCounter`
	/// after this slot's setup. A held plan is equal to a fresh one whenever the hash happens to agree, so
	/// the hold cannot be read off the plan alone.
	[[nodiscard]] int32_t holdLeftForTest() const noexcept { return hold_.left; }
	[[nodiscard]] float delaySendForTest() const noexcept { return delaySend_; }
	[[nodiscard]] size_t delayTimeForTest() const noexcept { return delayTime_; }
	/// The echo return's slewed gain — 0 once a fade-out has finished and the line is frozen.
	[[nodiscard]] float delayEchoGainForTest() const noexcept { return echoGain_; }
	/// **THE ECHO LINE'S WRITE HEAD, BECAUSE ITS MOTION IS THE CLAIM AND NO BUFFER CARRIES IT.**
	/// `delayTime_` is a lag in WRITES, not in frames, and the only thing that advances the head is
	/// the echo block itself — so whether an echo's real lag is the quarter slice it is written as
	/// depends on how many frames pass per write, which is a quantity that exists nowhere in the
	/// output. CLAUDE.md's value rule: read the number rather than infer it from a render.
	[[nodiscard]] size_t delayWritePosForTest() const noexcept { return delay_.writePos(); }
	[[nodiscard]] size_t delayLengthForTest() const noexcept { return delay_.length(); }
	/// **THE TWO GRAIN PHASES, because their DIFFERENCE is the design claim.** A crossfade that has
	/// collapsed still renders audio -- it just pulses to silence once per grain, which a finiteness
	/// or a silence-length check is too coarse to see. The number is the only honest witness.
	[[nodiscard]] double grainPhaseAForTest() const noexcept { return grainPhaseA_; }
	[[nodiscard]] double grainPhaseBForTest() const noexcept { return grainPhaseB_; }
	/// ONE GRAIN'S PLAN, for a case that must separate "reaches nothing" from "flipped no decision in
	/// this window" — two states a rendered buffer cannot tell apart.
	[[nodiscard]] SlicePlan grainPlanForTest(int32_t grainIndex) { return grainPlanFor(grainIndex); }
	/// **GRAIN.S OWN GEOMETRY, which is armed at the gesture and was never re-armed on a mode change.**
	/// The spread is the whole two-voice design: at zero both voices read the same place and the cloud
	/// is one voice. Not recoverable from the output — a collapsed cloud still renders audio.
	[[nodiscard]] size_t grainLengthForTest() const noexcept { return grainLength_; }
	[[nodiscard]] size_t grainSpreadForTest() const noexcept { return grainSpread_; }
	[[nodiscard]] size_t grainPosAForTest() const noexcept { return grainPosA_; }
	[[nodiscard]] size_t grainPosBForTest() const noexcept { return grainPosB_; }
	/// Whether voice A writes its current grain under pWrite — the per-grain draw.
	[[nodiscard]] bool grainWritesAForTest() const noexcept { return grainWritesA_; }
	/// The gate class a voice's CURRENT grain plays under — the dry and reverse gates as `playGrain`
	/// settled them at its onset. What an onset counter's class is asserted against (backlog HR round 2).
	[[nodiscard]] size_t grainGateClassForTest(bool voiceB) const noexcept {
		return voiceB ? grainOnsetClass(grainDryB_, grainRevB_) : grainOnsetClass(grainDryA_, grainRevA_);
	}

private:
	/// **ONE CHUNK OF THE BLOCK, WHOLE: render, lanes, blend.** `processStereo` cuts the host's block
	/// into pieces no longer than the scratch and calls this for each.
	///
	/// `left`/`right` arrive holding the DRY and leave holding the output; the wet never touches them
	/// until the blend at the end, which is what lets the lanes have the wet to themselves.
	void renderChunk(float* left, float* right, int frames);

	/// THE RENDER ITSELF — what `processStereo` was before it gained a post chain. Private
	/// because the chain is not optional from outside: a caller that could reach this would be a
	/// caller that silently skips the chain, and the test suites are full of such callers.
	///
	/// **IT NO LONGER WRITES `left`/`right`.** Every renderer now writes the WET into `wetL_`/`wetR_`
	/// and its blend coefficient into `mixCoeff_`, leaving the input intact for the blend that happens
	/// after the lanes. `left`/`right` are still passed because three renderers READ the live input —
	/// Grain's dry voices and its bare-dry slew, and the capture that walks the ring.
	void renderStereo(float* left, float* right, int frames);

	/// **RUN THE POST CHAIN OVER THIS BLOCK'S OUTPUT** — the second line of `processStereo`, and its
	/// only caller, for the reason above: these two are a pair and either alone is a wrong answer.
	///
	/// It publishes the seven aux outputs into the chain's core sources first (decision D3(a)), so an
	/// inner modulator reading `Perf X` reads the same value an outer one does.
	void runPostChain(float* left, float* right, int frames);

	/// **FOLD ONE CHUNK'S TWO PEAKS INTO THE HELD LEVELS.** Called by the render, not by
	/// `publishReadings`, so the numbers exist whether or not a sink was ever installed — see
	/// `wetLevel()`. Both together, always both, over the one shared window (see the members): folding
	/// them separately is what would let the two halves of a ratio come from different intervals.
	void foldOutLevels(float wetPeak, float grainPeak, size_t frames) noexcept;

	/// HOW LONG A SLICE IS, from the rate control and the host's tempo.
	[[nodiscard]] size_t resolveSliceFrames() const;
	[[nodiscard]] double rateStep() const;
	[[nodiscard]] size_t burstCycleLength() const;
	void playBurst(int from, int frames);
	[[nodiscard]] double effectiveRatePosition() const;
	/// **WHICH BAR OF THE SEQUENCE IS PLAYING — the free-running counter folded by `Settings::seqBars`.**
	///
	/// `planFor` reads this as two bits, so four is the longest sequence those bits can express and
	/// `Param::SeqLength`'s ladder stops there. The fold is HERE and not at `barIndex_` itself because
	/// two other readers want the real bar: Time's phrase (`% kTimePhraseLength`) and the pan's
	/// `barIndex_ * 1.3`. One counter, two periods.
	[[nodiscard]] int32_t seqBarIndex() const noexcept {
		const int32_t bars = target_.seqBars > 0 ? target_.seqBars : 1;
		return ((barIndex_ % bars) + bars) % bars;
	}

	/// ONE BAR, which is the grid Rate divides.
	[[nodiscard]] size_t resolveBarFrames() const;
	/// **THE CAPTURED LOOP — the bar scaled by `Settings::bufferBars`, clamped to the ring.** Separate
	/// from the bar since `Param::BufferLength`; see `bufferFrames_`.
	[[nodiscard]] size_t resolveBufferFrames() const;
	/// Dry against wet, plus the minimum-attack ramp — one road for all four renderers. See the .cpp.
	/// Dry against wet, plus the minimum-envelope ramp at BOTH ends of a grain — one road for all four
	/// renderers. `remaining` is frames until this grain's audible end; `kNoGrainEnd` where the caller
	/// has no such edge. See the .cpp.
	[[nodiscard]] GrainFade grainFadeLengths(size_t gated) const;
	/// One ratchet sub-slice's length, floored at `kMinSubSliceSeconds`. See the .cpp.
	[[nodiscard]] size_t subSliceLength(size_t audible, size_t subdivisions) const;
	/// How many of a ratchet's pieces are HEARD in a slot, and one piece as a fraction of it — see the .cpp.
	[[nodiscard]] int32_t heardRatchet(const SlicePlan& p, size_t slot, size_t material, size_t slotStart,
	                                   size_t buffer, bool barEndSilent, float* piece) const;
	/// The fade's gain at a position — shared by the output and the pWrite write. See the .cpp.
	[[nodiscard]] static float fadeGainAt(size_t pos, size_t remaining, size_t attackLen,
	                                      size_t decayLen);
	/// **THE WET, AT THIS SAMPLE — the GRAIN's own envelope and nothing else.** What a renderer writes
	/// into `wetL_`/`wetR_`, and what the two inner lanes see.
	///
	/// **THIS AND `mixAt` ARE THE TWO HALVES `blendAt` USED TO BE, AND THE SPLIT IS NOT A REFACTOR.**
	/// That function folded three terms owned by three different things — the grain's envelope, the
	/// gesture's engage crossfade, and the device's Mix — and its own header said so. Once the master
	/// lane moved to the WET path (master dry wet should be post fx) the envelope
	/// had to stay in the renderer, because the lane must see grains that have been enveloped, while the
	/// mix had to move past the lane. So the blend SPLITS; it does not relocate.
	[[nodiscard]] float wetOf(float wet, size_t remaining, size_t attackLen, size_t decayLen) const;
	/// The same at a fade position the caller names — the looper's ratchet sub-slices and gated ends.
	[[nodiscard]] float wetAt(float wet, size_t pos, size_t remaining, size_t attackLen,
	                          size_t decayLen) const;
	/// **HOW MUCH OF THE WET THIS SAMPLE TAKES: `Mix` times the engage crossfade.** Written into
	/// `mixCoeff_` by every renderer at every sample it produces, and applied AFTER both lanes.
	[[nodiscard]] float mixAt() const;
	/// **"THIS RENDERER HAS NO GRAIN END TO FADE OUT OF."** Grain has a triangular window of its own and
	/// Burst gates a slice of its own length; neither has an edge for the release half to guard.
	static constexpr size_t kNoGrainEnd = static_cast<size_t>(-1);
	/// Arm Grain's two-voice geometry. See the .cpp — it has two callers and used to have one.
	void armGrain(float zoneA);
	/// Arm Burst's one slice from where the looper cursor is reading — Mode turned to it during a
	/// looper gesture. See the .cpp.
	void armSliceFromCursor();
	/// One voice's dry draw at its grain boundary — the firmware's per-grain density. See the .cpp.
	[[nodiscard]] SlicePlan grainPlanFor(int32_t grainIndex);
	[[nodiscard]] bool grainDrawIsDry(int32_t grainIndex);
	/// Whether one grain WRITES under pWrite — a per-grain draw, as the firmware makes it. See the .cpp.
	[[nodiscard]] bool grainDrawWrites(int32_t grainIndex) const;
	/// Rate and Zone A, re-read at a grain boundary so they act while playing. See the .cpp.
	void refreshGrainGeometry();

	/// ONE BLOCK OF A LOOPER MODE: schedule a slice, shape it, write back.
	void playLooper(float* left, float* right, int from, int frames);
	/// TAKE THE NEXT SLICE OF THE BAR — recomputes the plan and the schedule.
	void advanceSlice();

	/// **THE PERFORM PUSH FOR THE SETTINGS THIS DEVICE IS HOLDING — the one call the three plan sites
	/// make.** `performPushOf` takes the weights and the depth as arguments (see `perform.h` for why),
	/// which is right for the seam and wrong to spell out three times: the wide form does not tie the
	/// weights or the depth to the `Settings` beside them, so `performPushOf(target_, someOtherWeights,
	/// 1.0f)` compiles and plays. This file's own comments are largely about a term going missing from
	/// one of three hand-built callers; three verbatim copies of a three-argument call is that shape
	/// again, one layer up. The wide signature stays available for whoever needs it — a picture walk
	/// over a hypothetical puck, say — and nothing in the DEVICE uses it directly.
	[[nodiscard]] MacroPush targetPush() const;

	Settings target_ {};

	//----------------------------------------------------------------------------------------------//
	// THE MOD ROAD'S STATE. Written only by `setPerformFields`, `tickPerform`, `prepare` and `reset`. //
	//----------------------------------------------------------------------------------------------//

	/// THE SEVEN AUX OUTPUTS, in `kOutputs` order: x, y, N, E, S, W, speed.
	std::array<float, kPerformOutputs> performOut_ {};
	/// THE SANITISED PUCK the last row decoded to — the mod road's own copy, never `target_`'s.
	float puckX_ = 0.0f;
	float puckY_ = 0.0f;
	/// WHERE THE PUCK WAS AT THE PREVIOUS TICK, which is the only thing a speed can be measured against.
	float lastPuckX_ = 0.0f;
	float lastPuckY_ = 0.0f;
	/// **FALSE AFTER CONSTRUCT, `prepare` AND `reset` — the next tick measures nothing and publishes 0.**
	/// Design §4.5: a patch load that teleports the puck is not a gesture, and without this the first
	/// tick after a load would read the whole distance from the origin as one tick's travel.
	bool havePuck_ = false;
	/// UNITS PER SECOND, averaged over `kSpeedTauSeconds`. Double because the EMA accumulates.
	double speed_ = 0.0;
	/// **THE EMA'S COEFFICIENT, `1 - exp(-Δt/τ)`, DERIVED IN `prepare`.** Δt is `kPerformTickSamples`
	/// over the host's rate and τ is a constant, so nothing about it changes between ticks and an `exp`
	/// per tick on the audio thread would carry no information.
	///
	/// **THE DEFAULT IS THE 48 kHz VALUE, NOT ZERO**, so an instance ticked before `prepare` averages at
	/// the rate `sampleRate_` also defaults to rather than freezing the speed at 0 — the two defaults are
	/// one assumption and they agree. It is a literal because `std::exp` is not constexpr; MEASURED to
	/// fifteen places at the stated inputs, and `prepare` overwrites it from the host's real rate before
	/// any tick the engine makes.
	double speedCoeff_ = 0.016528546178383; // 1 - exp(-(64/48000)/0.080)

	/// **MIX AND STEREO WIDTH, PER SAMPLE, WHILE A ROUTED SOURCE MOVES THEM** (backlog FK). `setWindowRuns` hands the next
	/// call's tick windows; `renderStereo` takes them, and each renderer's loop reads its sample's value into `mixNow_` and
	/// `widthNow_` (`sampleControls`) — the two numbers every read of Mix and Width uses. Null runs read the settings.
	const float* pendingMixRun_ = nullptr;
	const float* pendingWidthRun_ = nullptr;
	const float* mixRun_ = nullptr;
	const float* widthRun_ = nullptr;
	float mixNow_ = 0.0f;
	float widthNow_ = 0.0f;
	/// `i` is CHUNK-relative; the windows are indexed from the start of the host's call, which is what
	/// `windowBase_` adds back. They were the same number until the block began being cut into chunks.
	void sampleControls(int i) noexcept {
		const int w = i + windowBase_;
		mixNow_ = mixRun_ != nullptr ? std::clamp(mixRun_[w], 0.0f, 1.0f) : target_.mix;
		widthNow_ = widthRun_ != nullptr ? widthRun_[w] : target_.stereoWidth;
	}
	/// Where this chunk starts inside the host's block — see `sampleControls`.
	int windowBase_ = 0;

	//----------------------------------------------------------------------------------------------//
	// THE WET PATH — what the two inner lanes see, and the coefficient that folds it back in.        //
	//----------------------------------------------------------------------------------------------//
	//
	// **WHY THE WET LIVES IN A BUFFER AT ALL.** A lane is a CHAIN, and a chain processes a block rather
	// than a sample — so the wet is not finished inside the renderer's per-sample loop and the blend
	// cannot happen there any more. `mixCoeff_` carries what the blend needs out of the loop with it.
	//
	// **TWO PROPERTIES THESE BUFFERS MUST HAVE, AND BOTH ARE DESIGN CONSTRAINTS RATHER THAN
	// PREFERENCES.** `tests/test_sklitter_chain.cpp`'s *"a device whose chain holds NOTHING is bit for
	// bit the device that has no chain at all"* is a merged promise, and it survives this change only
	// because of them:
	//
	//   1. **ASSIGN, NEVER ACCUMULATE.** `0.0f + (−0.0f)` is `+0.0f`, so a renderer that added its wet
	//      into a zeroed buffer would change the sign of every negative zero it produced — for a device
	//      with nothing in its chain. `rack::InnerChain`'s own empty-chain fast path exists for exactly
	//      this reason and names the sign explicitly. A renderer WRITES its sample; only a SEND
	//      accumulates, and a send is a different buffer.
	//
	//   2. **AN UNWRITTEN SAMPLE CARRIES `m = 0`, WHICH IS WHY THESE ARE CLEARED EVERY CHUNK.** There
	//      are regions of a block no renderer writes: the capture phase in `renderStereo` walks the
	//      input into the ring without producing output, and `playLooper` returns at once when
	//      `sliceFrames_ == 0`. Today that means "the dry is already in `left[i]` and passes for free".
	//      At `m = 0` the blend is skipped and the sample is the dry EXACTLY, for every finite value
	//      and for both zeros — so the coefficient is the mix and the written-mask at once.
	//
	//      **AND THERE ARE TWO INDEPENDENT REASONS TO CLEAR, NOT ONE — STATED SEPARATELY SO THAT
	//      DISPROVING EITHER DOES NOT RETIRE THE OTHER.** (a) a stale WET would sound last chunk's
	//      audio through a silent region, and (b) a stale COEFFICIENT would replay last chunk's BLEND
	//      over the dry, which is the worse of the two because it corrupts a region the device is
	//      supposed to be passing through untouched. A one-reason justification is the kind somebody
	//      optimises away after satisfying themselves about the one reason.
	//
	// **AND THE COEFFICIENT IS WRITTEN BY THE RENDERER RATHER THAN RECONSTRUCTED AFTERWARDS.** It could
	// be re-derived from `mixRun_` and the engage counter, and that would be one buffer cheaper and
	// wrong in this codebase's characteristic way: it would compute in a second place a number four
	// renderers already produce, and it would assume every sample is written, which item 2 says is
	// false. `blendAt` was ONE function with four callers precisely to stop that (its header lists five
	// occasions a fix reached one renderer and not the others); re-deriving here would reintroduce the
	// shape inside the change that split it.
	/// **THE LONGEST PIECE RENDERED AT ONCE**, and the length of each buffer below: the host's longest block, as
	/// `prepare` was told it (backlog GO; `device::scratchFrames`), so a host keeping its promise is rendered in one
	/// piece. `processStereo` still cuts a longer call to it — the guard a promise needs. In the rack the chunk loop
	/// runs exactly once however big the host's buffer is, because `RackEngine::processStereo` already cuts every
	/// device's block at `device::kTickWindow` (64). Zero until `prepare`, and `processStereo` renders nothing then.
	size_t scratchFrames_ = 0;
	std::vector<float> wetL_;
	std::vector<float> wetR_;
	std::vector<float> mixCoeff_;
	device::TransportClock clock_ {};
	EngageMachine engage_;
	/// **THIS RING'S LIFETIME IS TIED TO AN ASSUMPTION ABOUT CONTAINER BODIES, and the assumption is
	/// not written anywhere this ring can see it** (bolt-on-chains trap T9). Since this device became a
	/// container, `Converger::commit` treats a body ARRIVING under a live container's own id as "new
	/// contents" (`converger.cpp:369`, `newContents`) and REPLACES the whole device instance — this
	/// ring with it. Four seconds of captured audio, gone mid-gesture.
	///
	/// **IT IS NOT A DEFECT TODAY, and the reason is a single caller.** `ContainerBodies::store` is
	/// called from exactly one place — `rack_state.cpp`'s `applyPatchToRecord`, a PATCH LOAD — where a
	/// fresh ring is the correct answer: the whole plugin state is being replaced. The rack device,
	/// the only other container, has no audio state of its own and so never noticed the rule.
	///
	/// **THE DAY A SECOND CALLER BUMPS A BODY GENERATION, THIS BECOMES A DEFECT**, and it will present
	/// as "the looper drops its bar when I do X" with nothing in this file to explain it. Whoever adds
	/// that caller meets this comment here rather than in a backlog entry they have not read.
	Ring ring_;

	double sampleRate_ = 48000.0;
	size_t sliceFrames_ = 0;

	/// **HOW MUCH AUDIO IS CAPTURED, WHICH STOPPED BEING THE SAME NUMBER AS THE BAR.**
	///
	/// `sliceFrames_` is the BAR in a looper mode — the grid Rate divides — and it was also the ring's
	/// length, the capture target, the write wrap and the modulo every read offset came back through,
	/// because a looper could only ever capture one bar. `Param::BufferLength` separates them: the bar
	/// still sets the grid, and this sets the reach.
	///
	/// EQUAL TO `sliceFrames_` AT THE DEFAULT ONE-BAR BUFFER, which is what makes the setting inert
	/// until it is moved and is the property the whole suite is the test of.
	size_t bufferFrames_ = 0;

	/// THE BUFFER LENGTH THE CURRENT CAPTURE WAS MADE AT. A change to the setting must DISCARD and
	/// re-record (discarding and restarting the buffer when the setting switches is acceptable), and
	/// noticing the change needs the old value — there is nowhere else it is written down once
	/// `target_` has moved.
	double capturedBufferBars_ = 0.0;
	/// THE LENGTH RATE ASKED FOR AT THE PRESS. `sliceFrames_` is what was RECORDED and does not
	/// move; this is what it was recorded AT, so the live rate can be stated as a ratio against it.
	size_t capturedSliceFrames_ = 0;
	/// **WHERE IN THE RING BURST'S SLICE BEGINS.** 0 after its own press, which resets the ring and
	/// records from its top; the looper cursor's frame when Mode is turned to Burst during a looper
	/// gesture (`armSliceFromCursor`), whose ring holds a whole buffer.
	size_t sliceOrigin_ = 0;

	/// **QUANTIZE: A SNAP AT THE PRESS AND A FREE KNOB AFTERWARDS.** See
	/// `Sklitter::effectiveRatePosition`. `quantizedBase_` is the division the gesture began on and
	/// `rateAtPress_` is where the control was when it did, so later movement is a delta against it.
	bool quantizedGesture_ = false;
	double quantizedBase_ = 0.0;
	float rateAtPress_ = 0.0f;

	/// **PITCH MODE'S RESAMPLING RATIO, PER SLICE.** `shouldPitchUp` is a 2x decimation the reference
	/// applies in EVERY mode; this is the separate thing Pitch mode is actually for — a degree drawn
	/// from `kScaleSemitones` and turned into a ratio by `kPitchRatioFP`. Both tables were ported and
	/// neither was read, so Pitch only ever jumped an octave. pitch mode: I think im only
	/// hearing octave jumps.
	double pitchRatio_ = 1.0;

	//----------------------------------------------------------------------------------------------//
	// **THE ANTI-CLICK SYSTEM, WHICH DID NOT COME ACROSS AT ALL.**                                   //
	//                                                                                                //
	// normal modes: clickier grains than i remember from [the hardware]--did //
	// you forget to apply the clever transient handling/zero crossing business? I did. The port has //
	// `grainEnvelope` and nothing else; the reference has THREE separate mechanisms and none of the   //
	// four constants behind them (`kGrainReleaseZone`, `kBarEndZone`, `kBufferWrapFadeLen`,           //
	// `kMinGrainSize`) appeared anywhere in this directory.                                          //
	//                                                                                                //
	//   1. ATTACK: at a non-consecutive grain start, each channel is MUTED UNTIL ITS FIRST ZERO       //
	//      CROSSING. An envelope fades in from wherever the waveform happens to be; this starts it    //
	//      from where the waveform is already zero, which is a different and better thing.            //
	//   2. RELEASE: inside the last ~15 ms, each channel mutes at its NEXT zero crossing rather than  //
	//      at the end of its slot.                                                                   //
	//   3. CONSECUTIVE PREDICTION: both are SKIPPED when the audio flows naturally — the grain has no //
	//      offset, no reverse and no pitch-up — which needs the NEXT grain's plan, not just this      //
	//      one's. Without the skip the protection itself becomes the artefact, chopping a continuous  //
	//      passage at every slot edge.                                                                //
	//----------------------------------------------------------------------------------------------//

	/// Mute this channel until its first zero crossing — the attack half.
	bool waitingForZeroCrossL_ = true;
	bool waitingForZeroCrossR_ = true;
	/// Muted for the rest of the grain, having found a zero crossing in the release zone.
	bool releaseMutedL_ = false;
	bool releaseMutedR_ = false;
	/// The previous sample, per channel — a crossing is a SIGN CHANGE, so it cannot be seen from one.
	float prevOutputL_ = 0.0f;
	float prevOutputR_ = 0.0f;
	/// Whether THIS grain flows on from the last, and whether the NEXT flows on from this one.
	bool consecutive_ = false;
	bool nextConsecutive_ = false;
	/// WHETHER THE GATE RUNS FOR THIS GRAIN AT ALL — false when the audio flows naturally and the
	/// protection would be the artefact. Settled with the plan, once a slice.
	bool zcActive_ = false;

	/// **WHICH BAR OF THE LOOP THIS IS, 0..63.** Two bits of it weight a pair of phi triangles into
	/// Zone A's phase (`planFor`'s `ofs.barIndex != 0` branch), which is what makes a four-bar loop
	/// evolve instead of repeating exactly. It was declared, read, and never written.
	int32_t barIndex_ = 0;

	/// THE SINK, and the picture it is handed. The picture is written one step at a time as the bar
	/// plays — see `sklitter_types.h` — so it costs two stores a slice rather than N plans a block.
	device::ReadingsSink sink_ {};
	std::array<float, kReadingCount> readings_ {};

	/// **THE TWO LEVELS' PIECES: a published peak and a running candidate each, over ONE SHARED
	/// WINDOW.** Kept in FRAMES rather than in seconds because the only clock this device is sure of is
	/// `sampleRate_`, and a host that stops calling us stops the window too — which is right: a held
	/// peak that aged on wall time would fall to zero while the transport was parked on a loud bar.
	///
	/// **ONE WINDOW FOR BOTH, AND THAT IS NOT A SAVING — IT IS THE CLAIM.** The picture's whole value is
	/// "how much of the sound went each way", which is a RATIO; two independently-turning windows would
	/// make that a comparison between two numbers measured over different intervals, and it would be
	/// wrong exactly when the material was changing, which is when anyone is looking.
	float wetLevelPeak_ = 0.0f;
	float wetLevelCandidate_ = 0.0f;
	float grainLevelPeak_ = 0.0f;
	float grainLevelCandidate_ = 0.0f;
	size_t outLevelHeld_ = 0;
	/// **THE GRAIN ONSET COUNTERS, one per gate class** — `Reading::GrainOnsets`. Audio thread only;
	/// they cross the boundary as copies in `readings_`. Deliberately NOT cleared by `reset()`: see there.
	std::array<uint32_t, kGrainOnsetClasses> grainOnsets_ {};
	/// Counts one onset of a grain whose gates are these. `playGrain`'s two voice wraps and nothing else.
	void countGrainOnset(bool dry, bool reversed) noexcept {
		uint32_t& n = grainOnsets_[grainOnsetClass(dry, reversed)];
		n = (n + 1u) % kGrainOnsetWrap;
	}
	/// **THE REPEAT COUNTER AND THE PERIOD OF THE SLOT THAT LAST RESTARTED** — `Reading::RepeatCount` and
	/// `Reading::RepeatPeriod`. Audio thread only; never cleared, for the onset counters' reason.
	uint32_t repeats_ = 0;
	float repeatPeriod_ = 0.0f;
	/// Counts one restart of Repeat's slice, `slotFrames` long. `playLooper`'s slot end and nothing else.
	void countRepeat(size_t slotFrames) noexcept {
		repeats_ = (repeats_ + 1u) % kGrainOnsetWrap;
		repeatPeriod_ = sampleRate_ > 0.0 ? static_cast<float>(static_cast<double>(slotFrames) / sampleRate_)
		                                  : 0.0f;
	}


	/// Whether THIS bar takes the ~50 ms silence at its seam — every bar except in Time, which takes
	/// it only on the last bar of its four-bar phrase.
	bool barEndSilence_ = true;

	//----------------------------------------------------------------------------------------------//
	// **THE LOOPER PRE-WARMS.** the looper is supposed to pre warm.                        //
	//                                                                                                //
	// The port captured from the moment Engage went high: a whole bar, then a wait for the next beat //
	// boundary — MEASURED at four to five beats, over two seconds at 120 BPM, before a note. The      //
	// reference does not do that. `Stutterer::recordStandby` writes the incoming audio into the      //
	// looper buffer CONTINUOUSLY while the device is armed and not playing, so the gesture triggers   //
	// against a buffer that is already full and the sound starts at the next grid line.               //
	//                                                                                                //
	// **IT IS BEAT-ALIGNED, and that is not a detail.** The reference waits for a boundary before it  //
	// starts writing (`waitingForRecordBeat`), so frame 0 of the ring is a beat. A free-running ring  //
	// would hold a whole bar of audio starting at an arbitrary phase, and every slice the device then //
	// played would be offset from the grid by however long ago the plugin happened to load.           //
	//----------------------------------------------------------------------------------------------//

	/// Whether the pre-warm has passed its beat boundary and is writing.
	bool prewarming_ = false;
	/// The beat it is waiting for, or `kNoBeat` before the first look.
	long long prewarmBeat_ = INT64_MIN;
	/// The beat the last waiting block saw — a song that goes BACK is a DAW loop's wrap, and the wait
	/// ends there rather than at a downbeat past the loop's end. See `prewarm`.
	long long prewarmLastBeat_ = INT64_MIN;
	/// How much of the bar it has written since that boundary — a whole one means ready.
	size_t prewarmed_ = 0;
	void prewarm(const float* left, const float* right, int frames);
	/// **THE IDLE HEAD IS SEATED ONCE, THEN APPENDS — and re-seated only on a CONFIRMED discontinuity**
	/// (the 2026-09-24 crush's guard). See `seatPrewarmHead`.
	bool prewarmSeated_ = false;
	/// The last call's disagreement, song head minus append head, in frames (signed, wrapped to half a
	/// ring); `kNoDrift` when nothing has been compared since the last seat.
	static constexpr long long kNoDrift = INT64_MIN;
	long long prewarmLastDrift_ = kNoDrift;
	/// The most recent disagreement, for the tolerance measurement and the tripwire.
	long long prewarmDrift_ = 0;
	/// How many times a confirmed discontinuity re-seated the idle head.
	size_t prewarmReseats_ = 0;
	void seatPrewarmHead(int frames) noexcept;
	void tripwireReseat(long long drift, int frames) noexcept;
	/// **THE APPEND HEAD ON THE SONG'S PHASE, ONCE A BLOCK (backlog IN).** The idle and recording roads
	/// append with `Ring::write`, which keeps whatever phase the head started at: a block-granular
	/// anchor, or every frame heard before the transport rolled. Re-seated at each block's first frame
	/// from `lockedWriteHead`, ring index i is song phase i whenever there is a rolling song to say so.
	void seatWriteHeadOnSong() noexcept;

	//----------------------------------------------------------------------------------------------//
	// **THE TRANSPORT LOCK — backlog GT.** An option to stop playback with the transport, the buffer  //
	// frozen, staying in phase on restart — and the phase is the transport's: a gesture triggered on //
	// the downbeat resumes with the same buffer as if it had been re-triggered on the downbeat.       //
	//                                                                                                 //
	// **THE READ CURSOR IS THE WHOLE OF IT.** `sliceIndex_` is zeroed at engage and advanced by        //
	// `sliceIndex_ += schedule_.grainLength`; nothing has ever re-derived it from the song. Under the  //
	// lock that increment is replaced by a derivation, so the position becomes a FUNCTION of the song  //
	// position rather than a counter that happens to have started in the right place.                 //
	//                                                                                                 //
	// **AND THE ANCHOR CANCELS, which is why nothing new is stored for the phase.** `prewarm` only     //
	// ever begins on a beat index divisible by four (`((beat / 4) + 1) * 4`), so ring frame 0 sits on  //
	// a song DOWNBEAT and the bar grid the cursor walks is the song's own bars. The bar COUNTER is     //
	// the one thing that does need the anchor, and `prewarmBeat_` above is it.                         //
	//----------------------------------------------------------------------------------------------//

	/// Whether the lock is on AND this mode is one it means anything in. See `Param::TransportLocked`.
	[[nodiscard]] bool transportLockActive() const noexcept;
	/// **WHERE IN THE BAR THE SONG IS**, as a fraction of a bar, or a NEGATIVE number when the host
	/// offers no beat position. `atFrame` is frames past this block's transport reading.
	[[nodiscard]] double lockedBarPhase(double atFrame) const noexcept;
	/// The bar the song is in, counted from the ring's frame 0 and folded at `kBarIndexWrap` — or −1
	/// when `prewarmBeat_` never got stamped, which is the one part of the lock that needs an anchor.
	[[nodiscard]] int32_t lockedBarIndex(double atFrame) const noexcept;
	/// The transport's own `(slice, offset-in-slice)` on the live grid. False means "cannot be
	/// derived", and every caller falls back to the free-running counter when it says so.
	[[nodiscard]] bool lockedCursor(double atFrame, int32_t* slice, size_t* within) const noexcept;
	/// The transport's own position for the ring's WRITE head, in frames of the captured buffer (backlog
	/// GU). False means "cannot be derived", and the head keeps walking as it was.
	[[nodiscard]] bool lockedWriteHead(double atFrame, size_t* head) const noexcept;
	/// SEEK. Moves the cursor to the transport's phase and rebuilds the slice's plan at the new index.
	bool seekToTransport(double atFrame);

	/// **SET WHENEVER THE HOST'S TRANSPORT IS STOPPED, IN BOTH SETTINGS, AND CLEARED BY THE SEEK IT
	/// CAUSES ON THE RESUME (backlog GT/HE).** A restart cannot wait for the next slot end — the first
	/// sample after it is already supposed to be in phase, and under the lock the cursor is parked
	/// wherever the stop left it rather than anywhere the grid would find.
	///
	/// **NAMED FOR THE REJOIN AND NOT FOR THE LOCK, because it is no longer the lock's.** It was
	/// `lockResync_` while the re-phasing was the locked setting's alone; the refinement made the
	/// toggle gate only whether audio keeps being produced while the transport is parked.
	bool rejoinPending_ = false;

	/// **THE LOCKED STOP'S RAMP — backlog GT.** A locked stop used to cut to the dry in one sample and stop
	/// every writer mid-stroke: MEASURED on HE's 311.13 Hz probe, a step of 0.381 (9.3x the input's own) at
	/// the stop, and a step of 1 left in the ring where Dry Write stopped. So the stop now owes ONE
	/// `wrapFade_` (5 ms) before it holds still: the wet falls to the dry over it, and the write gain falls
	/// to 0 over it through KX's own gate (`seatFreeze` targets 0 while `parked_`), which is also what
	/// ramps the writers back in on the resume. `processStereo` cuts its chunk where the ramp ends, so the
	/// heads walk exactly the ramp's frames and are parked where it leaves them.
	///
	/// `parked_` is set from the first stopped block under the lock to the first rolling one (or the lock
	/// going off), so the ramp is armed once per stop. Neither is ever set unlocked.
	bool parked_ = false;
	size_t parkLeft_ = 0;

	/// **FRAMES OF WET-OUT RAMP STILL OWED BEFORE THE SEEK MAY LAND — the unlocked rejoin's half of the
	/// engage fade (backlog HE).** Non-zero only while a sounding device is on its way to the dry; the
	/// seek happens once it reaches zero, and `engageFadePos_` brings the wet back afterwards.
	size_t rejoinDuckLeft_ = 0;
	/// Whether THIS rejoin has already run its duck — a duck that re-armed every block would hold the
	/// device at the dry for ever.
	bool rejoinDucked_ = false;

	/// One sample of the gesture's crossfade, in whichever direction it is running. **BOTH COUNTERS IN
	/// ONE PLACE** because there are four render loops and a decrement missing from one of them is a
	/// device stuck at the dry in exactly one mode.
	void advanceEngageFade() noexcept {
		++engageFadePos_;
		if (rejoinDuckLeft_ > 0) {
			--rejoinDuckLeft_;
		}
	}

	/// **WHAT THE RESTING PATTERN WAS COMPUTED FROM.** See `publishReadings`: the arrangement is a pure
	/// function of these, so it is recomputed only when one of them moves rather than every block.
	struct RestKey {
		float zoneA = -1.0f;
		float zoneB = -1.0f;
		// `macroConfig` stood here and is GONE WITH ITS CONTROL (2026-09-19). It was in the key because
		// the walk's `planFor` reads it; that argument now has nothing behind it — the device hands
		// `planFor` a constant zero config (see `sklitter.cpp`'s `kNoKnobConfig`), and a constant cannot
		// make a key stale. The waypoints' four configurations reach the walk through `push`, below.
		/// **THE PUSH, NOT THE MACRO, AND THAT IS THE WHOLE OF TASK 6 IN THIS STRUCT.** The walk below
		/// draws its plans from `performPushOf(target_)`, so the macro knob is no longer what decides
		/// them: the four waypoints and the puck decide them too. A key that still held `macro` alone
		/// would compare equal while the puck moved, and the resting picture would FREEZE — the "key
		/// short by one" shape `PlanCache`'s own note already paid for once, arriving here by a second
		/// road. Holding the push rather than the eleven controls it is built from is also narrower than
		/// it looks: two different puck positions that produce the same push produce the same picture,
		/// which is the thing the key is actually about.
		///
		/// **AND IT IS THE ONE MEMBER HERE WHOSE DEFAULT IS REACHABLE.** `{}` is the push of a macro at
		/// 0 — a patch a user can actually be sitting on — where the `macro = -1.0f` it replaced was an
		/// impossible sentinel doing double duty as "no key yet". **So "the first publish always
		/// redraws" now rests entirely on `mode = Mode::kCount` and the -1 integers below.** Do not
		/// "tidy" the mode out of this key: without it, a device whose first published arrangement
		/// happens to match every other defaulted member would skip its first walk and publish a band of
		/// zeroes.
		MacroPush push {};
		float rate = -1.0f;
		/// **GAMMA STAYS, THOUGH THE PUSH ALREADY CARRIES ITS EFFECT ON THE PUSH.** Checked by reading
		/// rather than assumed, and the first draft of this comment cited the WRONG mechanism — it named
		/// `planFor`'s `macroConfigNorm` sum, which is the one quantity that function never reads (its own
		/// note there: it "computes it here only to publish it in `p.macroConfigNorm`"). The real
		/// independent readers are two, both in `planFor`: **`phRawA`/`phRawB`**, the `zoneLap +
		/// kResolution · ofs.gamma` pair every zone phase is derived from, and the **`PhiTriContext` the
		/// timbral bank is evaluated in**, which takes gamma as its fourth member. Neither comes through
		/// the push. So the push subsumes `macro`, which reached the plan ONLY as a push, and does not
		/// subsume `gamma`, which has readers of its own.
		float gamma = -1.0f;
		Mode mode = Mode::kCount;
		/// **THE BAR IS GONE FROM THIS KEY AND THAT IS THE FIX, NOT AN OVERSIGHT.**
		///
		/// It was here because the picture was ONE BAR and therefore changed every time the bar
		/// counter moved. The picture is the whole SEQUENCE now, so the current bar no longer selects
		/// what is drawn — it only selects where the head is, which is a header field and not part of
		/// the arrangement. Leaving `bar` in would have recomputed all sixty-four steps every bar line
		/// to arrive at the same answer.
		int32_t seqBars = -1;
		int32_t slicesPerBar = -1;
		int32_t steps = -1;
		/// **THE RESOLVED LENGTHS, NOT `bufferBars`.** The walk divides by these, and they catch up
		/// with the setting only when `prewarm` next runs — see `publishReadings`.
		size_t barFrames = 0;
		size_t bufferFrames = 0;
		/// WHICH WINDOW OF A SEQUENCE TOO LONG TO CRAM IS ON SCREEN. Zero whenever it fits.
		int32_t windowBase = -1;
		/// **REPEAT'S PLAN COUNTER AT THE BAND'S FIRST STEP** (backlog IX) — Repeat draws its plans at
		/// `repeatLoopIndex_`, not at the step, so the walk does too; 0 in every other mode.
		int32_t repeatBase = -1;
		/// **RIBBON TRUTH: THE THREE CONTROLS THE WALK READS THAT THIS KEY DID NOT HOLD.** Density thins slots
		/// to dry, the Send Override reaches the plan through `offsetsFor`, and the groove sets how long each
		/// slot really is — which is how many restarts of a ratchet fit in it. A key without them compared
		/// equal while any of them moved, and the band kept the picture from before (IB's shape).
		float density = -1.0f;
		float sendOverride = -2.0f;
		GrooveFeel groove { GrooveMode::kCount, -1.0 };
		/// Time's phrase position of the sequence's first bar — see `publishReadings`; 0 in every other mode.
		int32_t phraseBase = -1;
		bool operator==(const RestKey&) const = default;
	};
	/// **ONE KEY FOR BOTH STATES NOW.** It guarded the resting walk only, because playing published a
	/// step at a time; the walk is the same walk in both states since the picture became coherent, so
	/// the name is the only thing left of the distinction.
	RestKey restKey_ {};
	/// The picture walk's own cache — sharing the playing grain's would evict its entry every block.
	PlanCache restCache_ {};
	/// **WARM MEANS THE WHOLE BUFFER, NOT THE FIRST BAR OF IT.** These were one number until
	/// `Param::BufferLength` split them; see `bufferFrames_`.
	[[nodiscard]] bool prewarmReady() const noexcept {
		return bufferFrames_ > 0 && prewarmed_ >= bufferFrames_;
	}

	/// **WHETHER THIS SLICE PLAYS THE INPUT INSTEAD OF THE GRAIN — one answer for the whole slice.**
	/// Density, the macro and the plan's own hash all vote; the reference calls the result
	/// `scatterDryMix` and compares it against a fixed 0.5, which is a boolean wearing a float.
	bool sliceIsDry_ = false;
	/// The previous slice's answer, for the attack-skip rule (*"Previous was dry, current is dry"*).
	bool prevSliceDry_ = false;
	/// Whether the previous slice was cut short by `lengthMult` — its tail is silence, so a join after
	/// it is never consecutive. See `advanceSlice`.
	bool prevSliceCut_ = false;
	/// Whether THIS slice silences the rest of its slot past `lengthMult`'s cut — false for a dry slice
	/// running into a dry one. See `advanceSlice`.
	bool tailSilenced_ = false;
	/// Where the previous slice's read ENDED, for the join test — `kNoReadEnd` when it did not read forwards
	/// at unison (reversed, pitched, ratcheted), so no read can continue from it. See `advanceSlice`.
	static constexpr size_t kNoReadEnd = static_cast<size_t>(-1);
	size_t prevReadEnd_ = kNoReadEnd;

	/// **THE PER-SLICE CROSSFEED PAN, which `SlicePlan::panAmount` was computed for and nothing
	/// applied.** The plan produced it two different ways and the render never read it. Direction is
	/// the bar index in Repeat and a free-running counter otherwise, and a grain spanning more than
	/// one slot does not pan at all — a slow pan across a long grain reads as a mix error rather than
	/// as movement.
	float panFade_ = 1.0f;  ///< what the leading channel keeps
	float panCross_ = 0.0f; ///< what it feeds to the other
	bool panRight_ = false;
	bool panActive_ = false;
	uint32_t panCounter_ = 0;

	//----------------------------------------------------------------------------------------------//
	// **BURST IS A GATE.** At the trigger it takes half the captured slice as a grain, then plays     //
	// that grain at NATIVE SPEED followed by silence until the cycle completes, over and over. Rate    //
	// moves the CYCLE, not the grain, so turning it changes how often the burst fires rather than     //
	// what pitch it is — which is the entire character of the mode.                                   //
	//----------------------------------------------------------------------------------------------//

	/// Half the captured slice, fixed at the trigger.
	size_t burstGrain_ = 0;
	/// Where in the cycle we are, and where in the grain. Both wrap on their own lengths.
	size_t burstCyclePos_ = 0;
	size_t burstReadPos_ = 0;

	//----------------------------------------------------------------------------------------------//
	// **pWRITE WAS A DIFFERENT FEATURE WEARING THE NAME.** It blended the input into the buffer at    //
	// the SHUFFLED read position, on every sample, in every looper mode, always. The reference:      //
	//   - a per-SLICE probability, `Bits(sliceIndex ^ (barIndex << 16) ^ 0xDEADBEEF).threshold4`;    //
	//   - written at the LINEAR bar position — *"read from shuffled position, write to linear        //
	//     position"*, which is what makes it a tape loop rather than a feedback path;                //
	//   - crossfaded by the GRAIN ENVELOPE, not by the control's value;                              //
	//   - never in Repeat;                                                                           //
	//   - and DUCKED when the read and write regions overlap, unless the slice is dry.               //
	//----------------------------------------------------------------------------------------------//

	//---- BACKLOG HN: the deferred-write instrument's state. See the setters above. ----------------//
	bool deferWrites_ = false;
	bool auditReads_ = false;
	/// The recorded writes for this chunk. SIZED AT `prepare`, never resized after — the
	/// no-allocation-after-activate rule is why these are three parallel arrays rather than a vector
	/// of a struct that somebody would later `push_back` into.
	std::vector<size_t> deferPos_;
	std::vector<float> deferSrcL_;
	std::vector<float> deferSrcR_;
	std::vector<float> deferMix_;
	size_t deferCount_ = 0;
	/// The audit's per-chunk map: `auditWritten_[k]` is set when the chunk's k-th sample wrote.
	std::vector<uint8_t> auditWritten_;
	size_t auditLo_ = 0;  ///< `linearBarPos_` at the start of the chunk
	size_t auditHits_ = 0;
	size_t auditReads1_ = 0;
	size_t auditWrites_ = 0;
	ReadTraceEntry* readTrace_ = nullptr; ///< `setReadTraceForTest`
	size_t readTraceLength_ = 0;
	GrainTraceEntry* grainTrace_ = nullptr; ///< `setGrainTraceForTest`
	size_t grainTraceLength_ = 0;
	/// **THE CRUSH TRIPWIRE** — see `tripwire.h`. Filled and logged only in a `FEATHERS_DIAGNOSTIC`
	/// build (`tripwireBlock`, `tripwireWet`); present in every build so the layout is one layout.
	Tripwire tripwire_ {};
	void tripwireBlock(int frames) noexcept;
	void tripwireWet(const float* dry, size_t n) noexcept;

	/// Record a write instead of performing it. Returns true when it was recorded.
	bool recordDeferredWrite(size_t pos, float srcL, float srcR, float mix) noexcept;
	/// Perform every recorded write, in the order they were recorded, and empty the record.
	void flushDeferredWrites() noexcept;
	/// Mark that this chunk wrote at `pos`.
	void auditWrite(size_t pos) noexcept;
	/// Count `pos` if this chunk has already written there.
	void auditRead(size_t pos) noexcept;

	//----------------------------------------------------------------------------------------------//
	// BACKLOG GU/GV — THE THREE SEQUENTIAL BLENDS. A DRAFT (2026-09-21).                            //
	//                                                                                               //
	// THE DESIGN: the writers BLEND rather than interact directly with pWrite. Every write to the    //
	// buffer is a blend that reduces the buffer's content and adds the signal, in the order dry      //
	// write, then feedback, then pWrite.                                                             //
	//                                                                                               //
	// **SO THE THREE WRITERS DO NOT GATE OR CONSULT ONE ANOTHER; THEY COMPOSE.** Each is             //
	// `old + (src − old)·amount`, in that order, at the SAME write head — `linearBarPos_`, which     //
	// walks the bar once per sample whatever the read head is doing.                                 //
	//                                                                                               //
	// **AND THE ORDER IS THE REASON ALL THREE HAVE TO BE DEFERRED TOGETHER.** The feedback source is //
	// the post-lane WET, produced a whole block after the per-sample write (`renderChunk`:           //
	// `renderStereo` -> `grainChain_->process` -> `runPostChain`). `P Write` is currently performed  //
	// per sample, which would put it FIRST rather than last. So when the circuit is live the whole   //
	// trio is recorded per sample and performed at the flush point, in the order.                 //
	//                                                                                               //
	// **AND ONLY WHEN IT IS LIVE.** `Settings::writersActive` is false for every patch that has not  //
	// touched the two new knobs, and the existing immediate write runs untouched — so the deferral   //
	// (exact in these modes by backlog HN's measurement, and only in these modes) is never a claim   //
	// anybody has to take on trust to get the old behaviour.                                         //
	//----------------------------------------------------------------------------------------------//

	/// What the chunk's k-th sample wants done at its write head.
	enum RingWriteFlag : uint8_t {
		kHeadLive = 1u << 0, ///< the write head is at `rwPos_[k]`; DRY and FEEDBACK may write here
		kHasPWrite = 1u << 1 ///< `P Write` recorded a source and a mix for this sample
	};

	/// True for the whole of a chunk in which the two continuous writers act. Sampled ONCE per chunk
	/// rather than per sample, so a control moving mid-chunk cannot leave half a block recorded and
	/// half performed — which would double-write the samples on the wrong side of the change.
	bool writersLive_ = false;
	std::vector<size_t> rwPos_;    ///< the write head at each sample of the chunk
	std::vector<uint8_t> rwWhat_;  ///< `RingWriteFlag`s, cleared at the top of every chunk
	std::vector<float> rwPwL_;     ///< `P Write`'s recorded source, left
	std::vector<float> rwPwR_;
	std::vector<float> rwPwMix_;   ///< and its recorded amount: the grain ramp times the slewed decision
	/// **THE POST-GRAIN-LANE WET, KEPT ONLY WHEN THE TAP ASKS FOR IT.** `wetL_`/`wetR_` are still the
	/// post-MASTER signal at the flush point, so the Post FX tap needs no copy at all; the Grain FX
	/// tap is the one that has to be snapshotted before `runPostChain` overwrites it in place.
	std::vector<float> fbTapL_;
	std::vector<float> fbTapR_;

	/// The damping one-poles' state. **PERSISTENT ACROSS BLOCKS, which is what makes them filters
	/// rather than per-block shapes** — a filter re-zeroed every block is a click generator at the
	/// block rate. Reset with the device, not with the chunk.
	float dampLpL_ = 0.0f;
	float dampLpR_ = 0.0f;
	float dcX1L_ = 0.0f;
	float dcX1R_ = 0.0f;
	float dcY1L_ = 0.0f;
	float dcY1R_ = 0.0f;
	float dampA_ = 1.0f;  ///< lowpass coefficient, from `kFeedbackDampHz` at `prepare`
	float dcR_ = 0.0f;    ///< DC blocker pole, from `kFeedbackDcHz` at `prepare`

	/// The test seam's readings, written by `flushRingWrites` alone. See the accessors above.
	float lastFbTapL_ = 0.0f;
	float lastFbDampedL_ = 0.0f;
	size_t lastWriteHead_ = 0;
	size_t lastDryWrites_ = 0;
	size_t lastFbWrites_ = 0;
	float lastPwSrcL_ = 0.0f;
	float lastPwMix_ = 0.0f;

	/// Record what this sample's write head is, and optionally what `P Write` wants written there.
	void recordRingWrite(int i, size_t pos) noexcept;
	void recordRingPWrite(int i, float srcL, float srcR, float mix) noexcept;
	/// **THE DRY BLEND, AT THE HEAD, IN THE SAMPLE** — the first of the three, performed where it is
	/// recorded rather than at the flush. See the definition for why (backlog JC).
	void writeDryNow(size_t pos, float dryL, float dryR) noexcept;
	/// **EVERY WRITE INTO THE LOOP — P Write, Feedback, Dry Write and the deferred ones — WITH A FLOOR** (backlog MC):
	/// a sample under `kLoopFloor` lands as an exact zero. Each of them blends `old + (new − old)·a`, so a loop rewritten
	/// with silence decays geometrically and never ARRIVES at zero; MEASURED on the factory preset
	/// machine_elf_book_of_dreams, 72,500 frames still decaying from 1e-22 to 1e-30 a minute after the output was
	/// exactly silent, with `holdsTail` keeping a host awake for it. The capture (`Ring::write`) is not floored: it
	/// records the input. A frozen ring takes no writes (`Ring::setWriteGain`), so nothing in it is floored either.
	void writeLoop(size_t pos, float left, float right) noexcept;
	/// -140 dBFS, the house `kSilenceFloor` (the Delay's, the Pitch shifter's, the Eroder's).
	static constexpr float kLoopFloor = 1.0e-7f;

	/// **INPUT GAIN — backlog KZ.** Points `captureL_`/`captureR_` at what this chunk RECORDS: the host's own buffers
	/// at 0 dB (nothing is multiplied, so the default is bit-identical by construction), otherwise the scratch below
	/// filled with the input times the gain — ramped linearly across the chunk from the last chunk's gain, so a
	/// modulated knob does not step the recording once a tick. Every road that writes the INPUT into the ring reads
	/// these (the capture, the pre-warm, `Dry Write`); every road that HEARS the input reads `left`/`right`.
	void seatCapture(const float* left, const float* right, size_t n) noexcept;
	const float* captureL_ = nullptr;
	const float* captureR_ = nullptr;
	std::vector<float> captureScratchL_;
	std::vector<float> captureScratchR_;
	/// The linear gain the last chunk ended on; `captureGainPrimed_` false means none has, and the next takes its own.
	float captureGain_ = 1.0f;
	bool captureGainPrimed_ = false;

	/// **FREEZE — backlog KX.** Fills `freezeGain_` with this chunk's per-sample write gain: walking toward 0 while
	/// `Settings::freeze` is on and toward 1 while it is off, by one over `wrapFade_` a sample — the loop seam's
	/// own length, so the boundary where writing stopped is a crossfade no shorter than an ordinary loop point's
	/// fade. Every road that writes the ring hands `ring_` the gain of the sample it writes for
	/// (`Ring::setWriteGain`); the roads that would zero or fade the ring wholesale (a press's reset, the
	/// pre-warm's restart, the capture's seam fade) are skipped outright while frozen.
	/// **AND THE RECORD GATE — backlog MJ** shuts the same gain, per sample, while `left`/`right` (the input as it
	/// arrives) has stayed under `recordGateThreshold(Settings::recordGate)` for longer than the 500 ms hold. Off (0),
	/// the target is exactly what it was without it.
	void seatFreeze(const float* left, const float* right, size_t n) noexcept;
	/// `ring_.reset()` for every road but the host's own reset — skipped while frozen.
	void resetRingUnlessFrozen() noexcept;
	/// The gain sample `i` of this chunk writes at; the chunk's last gain once `i` is past it (`flushDeferredWrites`).
	[[nodiscard]] float freezeGainAt(size_t i) const noexcept {
		return i < freezeGain_.size() ? freezeGain_[i] : freezeWriteGain_;
	}
	std::vector<float> freezeGain_;
	/// The gain the last chunk ended on; `freezePrimed_` false means none has, so a device prepared (or reset)
	/// frozen starts frozen rather than ramping into it over a buffer that is empty anyway.
	float freezeWriteGain_ = 1.0f;
	bool freezePrimed_ = false;
	/// **THE RECORD GATE — backlog MJ.** Frames of hold left: re-armed to `recordGateHoldFrames_` by any input sample at
	/// or over the threshold, counted down by each under it; the gate is open while it is non-zero. Zero after
	/// `prepare` and `reset`, so a gated device starts shut until something arrives.
	size_t recordGateHoldLeft_ = 0;
	size_t recordGateHoldFrames_ = 0; ///< `kRecordGateHoldSeconds` in the host's frames, from `prepare`
	/// Whether the last sample `seatFreeze` saw left the gate open. Meaningless (and unread) while the gate is off.
	bool recordGateOpen_ = false;
	/// Whether the gate is ON and SHUT right now — what the wholesale roads (a press's reset, the capture's seam fade)
	/// ask, as they ask `Settings::freeze`: a zeroed ring is a recording of silence too.
	[[nodiscard]] bool recordGateShut() const noexcept {
		return recordGateThreshold(target_.recordGate) > 0.0f && !recordGateOpen_;
	}
	/// **THE RECORD LIGHT — `Reading::Recording`.** Whether `ring_.landed()` moved over the last `processStereo`;
	/// `landedAtBlock_` is where it stood when that call began.
	bool recording_ = false;
	size_t landedAtBlock_ = 0;
	/// **THE OTHER TWO BLENDS, IN THE DESIGNED ORDER, AFTER BOTH LANES HAVE RUN.** `fbL`/`fbR` are whichever
	/// tap the switch selected. The dry blend already landed, per sample, by `writeDryNow`.
	void flushRingWrites(const float* fbL, const float* fbR, size_t n) noexcept;
	/// The gentle fixed damping, one sample, both channels. See `kFeedbackDampHz`.
	void dampFeedback(float inL, float inR, float* outL, float* outR) noexcept;

	/// Where in the bar the write head is — linear, independent of which slice is being read.
	size_t linearBarPos_ = 0;
	/// Whether THIS slice writes at all: the per-slice draw, after the overlap duck.
	bool pWriteThisSlice_ = false;

	/// **REPEAT DRAWS ITS PLANS FROM A COUNTER OF ITS OWN, 0..7.** The reference:
	/// `grainIndex = isRepeat ? scatterRepeatLoopIndex : scatterSliceIndex`, stepped `& 0x7` at each
	/// repeat and started at 1 *"for non-zero hash seed"*. Repeat has one slice by construction, so
	/// following `sliceIndex` would hand the same index to `planFor` for ever and every repetition
	/// would be identical — which is what this port did.
	int32_t repeatLoopIndex_ = 1;

	/// THE PEEK'S OWN CACHE. `planFor` caches one plan; asking it for slice N and slice N+1 through
	/// the same one evicts on every call, which turns a dead-zone hit into a guaranteed miss.
	PlanCache nextPlanCache_ {};

	/// **THE TWO WINDOWS, IN SAMPLES AT THE HOST'S RATE.** The reference declares them as constants —
	/// `kGrainReleaseZone = 662` and `kBarEndZone = 2205` — and both are 44.1 kHz counts whose own
	/// comments give the intent in TIME: *"~15ms release window before grain end (ZC search, 33Hz
	/// min)"* and *"~50ms silent window before bar/phrase end (slop)"*. A plugin runs at whatever the
	/// host says, so copying the integers would make the release window 11 ms at 96k and the crossing
	/// search miss the low end it was sized for. They are computed in `prepare`.
	size_t releaseZone_ = 662;
	size_t barEndZone_ = 2205;
	/// ~5 ms — the reference's `kBufferWrapFadeLen` of 220, in time rather than in samples.
	size_t wrapFade_ = 220;
	/// **THE FOUR LENGTHS THE PORT LEFT AS 44.1 kHz SAMPLE COUNTS**, in the host's frames — see
	/// `kMinBurstCycleSeconds` in `sklitter_types.h`. At 96 kHz the raw numbers were half their intended
	/// duration, which is the floor doing half its job exactly where a short grain clicks most. The
	/// initialisers are the reference's own counts, so a device that is used before `prepare` behaves as
	/// it did rather than as nothing.
	size_t minBurstCycle_ = 64;
	size_t minBurstGrain_ = 32;
	size_t minSliceFrames_ = 256;
	/// See `kMinSubSliceSeconds`.
	size_t minSubSliceFrames_ = 1058;
	size_t minGrainFrames_ = 1024;
	/// **THE FIRMWARE'S GRAIN FADE** — its 440-sample base and its 64-sample floor, in the host's frames.
	/// See `grainFadeLengths`.
	size_t antiClickFrames_ = 440;
	size_t minAntiClickFrames_ = 64;
	/// **THE ENGAGE CROSSFADE** — how far into the gesture the output is, and over how many frames the
	/// wet rises from the dry. See `blend`.
	size_t engageFadePos_ = size_t { 1 } << 30; // finished: only a trigger starts it
	size_t engageFadeFrames_ = 440;
	/// The write's own 64-sample fade — see `kPWriteFadeSeconds`.
	size_t pWriteFadeFrames_ = 64;
	/// Either side of a read crossing the live write head — see `kCrossingDuckSeconds`.
	size_t crossingDuckFrames_ = 96;
	/// The crossing duck's gain now, slewed — see `playLooper`. 1 is no duck. One per Grain voice too.
	float crossingDuck_ = 1.0f;
	float crossingDuckA_ = 1.0f;
	float crossingDuckB_ = 1.0f;
	[[nodiscard]] float crossingDuckTarget(long long at, double v) const noexcept;
	float slewCrossingDuck(float* gain, float target) const noexcept;
	/// **THE WRITE DECISION, SLEWED** — 0 to 1 over `pWriteFadeFrames_`, toward `pWriteThisSlice_`. See
	/// the looper's write block for why this and not a per-slice fade.
	float writeGain_ = 0.0f;
	/// **THE MINIMUM-ATTACK RAMP** — its length in frames, and how far into it the current grain is.
	/// Zero length is off, which is the control's default. See `Param::MinEnv`.
	size_t envFrames_ = 0;
	size_t envPos_ = 0;

	//---- THE LOOPER MODES ------------------------------------------------------------------------//
	PlanCache planCache_;   ///< a MEMBER, not a static: see `slice_plan.h`
	/// **THE SECOND RING, and it is a separate one on purpose.** The per-grain echo has to keep
	/// sounding after its grain has been replaced, so it cannot live in the buffer the grains are
	/// read from -- a send that shared the loop would be overwritten by the next slice.
	Ring delay_;
	size_t delayTime_ = 0;   ///< frames behind the write head: a quarter bar
	float delaySend_ = 0.0f; ///< 0 off, else 0.25 / 0.5 / 1.0
	/// **THE SEND'S TWO RAMPS AND ITS LATCHED LAG** (backlog JC): the send level slewed toward
	/// `delaySend_`, the return's gain slewed toward on/off, and the lag the line runs at through a
	/// fade-out after `delayTime_` has already been zeroed. See the send in `playLooper`.
	float echoSend_ = 0.0f;
	float echoGain_ = 0.0f;
	size_t echoTime_ = 0;

	//---- GRAIN MODE: two voices, half a turn apart --------------------------------------------------//
	double grainPhaseA_ = 0.0; ///< [0,1); the triangle window's position
	double grainPhaseB_ = 0.5; ///< **half a turn behind A, which is the whole trick**: as one window
	                           ///< closes the other is wide open, so the cloud never gaps
	/// **WHICH MODE THE PLAYING STATE WAS ARMED FOR**, so a Mode change under a live gesture can re-arm
	/// what that mode owns. `Mode::kCount` means "nothing armed yet". See `advanceSlice` and backlog EQ.
	Mode armedMode_ = Mode::kCount;
	size_t grainPosA_ = 0;
	size_t grainPosB_ = 0;
	/// **EACH VOICE'S READ OFFSET INTO ITS GRAIN** — the firmware's `grainOffsetA/B`: zeroed at the
	/// voice's wrap, stepped by 1, or by 2 when the voice is pitched up. See `playGrain`.
	size_t grainOffsetA_ = 0;
	size_t grainOffsetB_ = 512;
	size_t grainLength_ = 1024;
	size_t grainSpread_ = 0;
	/// Whether each voice is playing the INPUT rather than the buffer this turn — density, drawn per
	/// voice at its own wrap. See `grainDrawIsDry`.
	bool grainDryA_ = false;
	bool grainDryB_ = false;
	/// How far the Grain output has moved to the BARE input — slewed toward "both voices dry". See
	/// `playGrain`.
	float grainBareDry_ = 0.0f;
	/// **WHETHER VOICE A WRITES THIS GRAIN** — the firmware's `grainAWritesWet`, drawn at A's wrap against
	/// pWrite. True until the first wrap decides, which is the firmware's own default.
	bool grainWritesA_ = true;
	/// Whether each voice plays its grain backwards, and whether it plays it an octave up by
	/// decimation. Decided per grain from the PLAN — see `grainPlanFor`.
	bool grainRevA_ = false;
	bool grainRevB_ = false;
	bool grainUpA_ = false;
	bool grainUpB_ = false;
	/// **EACH VOICE'S OWN GRAIN COUNTER**, which is what makes the decisions deterministic and what
	/// keeps the two voices from deciding identically. Free-running; the plan hash folds it.
	int32_t grainIndexA_ = 0;
	int32_t grainIndexB_ = 1;
	/// **HOW MANY MORE GRAINS VOICE A REPLAYS ITS POSITION FOR** — the firmware's
	/// `loopGrainRepeatCounter`, kept across blocks. See the wrap in `playGrain`.
	int32_t grainRepeatLeftA_ = 0;
	/// The grain walk's own plan cache — sharing the playing slice's would evict its entry at every
	/// wrap, which is the same reason `nextPlanCache_` and `restCache_` exist.
	PlanCache grainPlanCache_ {};
	uint32_t grainRng_ = 0x9E3779B9u;

	/// ONE BLOCK OF GRAIN: a cloud of two crossfaded voices rather than a sequence of slices.
	void playGrain(float* left, float* right, int from, int frames);
	/// XORSHIFT, so a grain cloud is deterministic per gesture and allocates nothing.
	uint32_t nextRandom() noexcept;
	SlicePlan plan_ {};
	/// **THE PUSH THE PLAYING SLICE WAS DRAWN FROM — STAMPED FOR THE TEST SEAM AND READ BY NOTHING ELSE.**
	/// Eight numbers is what the perform circle produces, and no render can be asked what they were: a plan
	/// is a handful of thresholded decisions and a buffer is the sound of them. `slicePushForTest`.
	MacroPush lastPush_ {};
	/// **THE ONE-AHEAD PEEK'S PLAN AND THE INDEX IT WAS DRAWN AT — the other road out of `advanceSlice`.**
	/// The peek decides `nextConsecutive_`, which decides whether a join fades; nothing else publishes it,
	/// and a plan reached by the wrong push there is a fade taken or skipped rather than a sound. See
	/// `nextSlicePlanForTest`.
	SlicePlan nextPlan_ {};
	int32_t nextGrainIndex_ = 0;
	/// Whether that peek crossed a bar line — the thing a case cannot infer from the index in Repeat.
	bool nextWraps_ = false;
	SliceSchedule schedule_ {};
	/// **THE REFERENCE'S REPEAT HOLD (`scatterCachedGrain`, `scatterRepeatCounter`)** — see `PlanHold`. A
	/// hold never crosses a bar, so its state is a function of the bar's walk from step 0; `holdNextSlice_`
	/// is the step the natural advance lands on, and ANY other arrival (a seek, the lock re-deriving the
	/// cursor, a press, a rate change) rebuilds the hold by that walk rather than carrying a count that
	/// belongs to a different path through the bar. -1 means "nothing to carry": rebuild.
	PlanHold hold_ {};
	int32_t holdNextSlice_ = -1;
	/// **AND A TURNED CONTROL IS ANOTHER ARRIVAL (ribbon truth, decision (a), 2026-09-23).** The hold
	/// caches a plan drawn under the controls of the slot that began it; carried across a turn, the next slots
	/// replayed a plan the CURRENT controls would never draw, until the bar's end — and the band, which walks
	/// the bar under the current controls, drew something else (MEASURED: Time, 4 slots after a mid-bar turn).
	/// So the hold is keyed on everything the plan and the walk's step read, compared once a slot in
	/// `advanceSlice`, and a change rebuilds it as a seek does: the plan a step plays is a function of the bar
	/// and the controls NOW.
	struct HoldKey {
		float zoneA = -1.0f;
		float zoneB = -1.0f;
		MacroPush push {};
		float gamma = -1.0f;
		float sendOverride = -2.0f;
		Mode mode = Mode::kCount;
		int32_t numSlices = -1;
		int32_t seqBar = -1;
		size_t sliceFrames = 0;
		size_t bufferFrames = 0;
		bool operator==(const HoldKey&) const = default;
	};
	HoldKey holdKey_ {};
	/// Frames into the slot an arrival that JOINED a long grain enters it at; 0 for every ordinary slot.
	size_t joinEntry_ = 0;
	int32_t sliceIndex_ = 0;
	int32_t numSlices_ = 1;
	size_t positionInSlice_ = 0;

	/// **WHERE IN THE BAR THE PLAYING SLOT ACTUALLY BEGAN, IN FRAMES — the groove's anchor (backlog IG).**
	///
	/// A swung slot ends on the CURRENT warp's absolute boundary, `grooveBoundary(s + g)`, and starts
	/// wherever the last one really ended — so its length is the difference. Measuring from the real start
	/// rather than from `grooveBoundary(s)` is what keeps the bar a bar while Groove is being turned or
	/// modulated: slots before the change and after it add up to exactly the bar, because every end is an
	/// absolute boundary. A walk that summed per-slot lengths under two different warps would slip the
	/// whole loop against the transport by the difference, for ever, in the free-running setting.
	size_t slotStartFrame_ = 0;
	/// **WHETHER THIS BAR HAS PLAYED A SWUNG SLOT.** While it has not, the slot length is `scheduleFor`'s
	/// own, untouched — the zero-change guarantee is then the original arithmetic rather than an equal one.
	/// Set by a swung slot; cleared where the bar restarts (the wrap, the press, a seek), so a groove turned
	/// back to Straight mid-bar re-anchors on its absolute boundaries until the bar is over.
	bool grooveAnchored_ = false;
	/// THE ONE PLACE A SLOT'S LENGTH IS SETTLED UNDER GROOVE — see `slotStartFrame_`.
	void applyGrooveToSlot();
	/// **HOW MUCH MATERIAL THE SLOT'S SLICE HOLDS — `scheduleFor`'s length, before the groove moved the
	/// slot (backlog IN addendum).** Swing moves WHEN a slice plays, not what it is: a slot the groove
	/// LENGTHENED reads its own slice and then falls silent until the delayed one starts, and it never
	/// reads on into the next slice's hit. Equal to `schedule_.sliceLength` whenever the groove is Straight.
	size_t materialLength_ = 0;
	/// Whether this slot is a wet one the groove lengthened past its material, so the rest of it is silent.
	bool grooveGated_ = false;
	/// **WHETHER THE SLOT BEFORE THIS ONE WAS `grooveGated_` — so this one starts out of that silence.** The
	/// join is consecutive in the READ and not in the SOUND: the material ends, the gap is zero, and the
	/// delayed hit begins at whatever its first sample is. Both edges take the short ramp (backlog IN,
	/// groove-click) — see `playLooper`.
	bool grooveGapBefore_ = false;

	/// **ONE INSTANCE COUNTER FOR EVERY CHAIN THIS DEVICE HOLDS** — gap G3. It is inert while there is
	/// exactly one chain (a lone chain's ids are unambiguous however they are counted) and it is here
	/// now because the day a SECOND lane arrives is the day a missing one is a silent collision rather
	/// than a compile error: `ContainerOps`' route verbs carry a `device::Destination{deviceId, index}`
	/// with no lane argument, so the holder dispatches on the id alone.
	///
	/// **ON THE HEAP, AND THE MOVE CONSTRUCTOR IS THE WHOLE REASON.** `InnerChain` BORROWS this counter
	/// by pointer. A plain member would move with the device while the chain — which lives on the heap
	/// and does not move — went on pointing into the moved-from `Sklitter`. A `unique_ptr` moves the
	/// POINTEE's address unchanged, so the chain's borrow stays valid. Declared before `postChain_` so
	/// it is constructed first and destroyed last.
	std::unique_ptr<backplane::InstanceCounter> laneIds_;

	/// **THE INNER EFFECTS CHAIN ON THIS DEVICE'S OWN OUTPUT** (backlog EW). Built in the constructor —
	/// on the message thread, where a device is built — and never lazily: a chain built inside
	/// `processStereo` allocates on the audio thread, and the failure is a click under load rather than
	/// a crash (trap T2). Its BRIDGE is the part that waits for `adopt`, which is the only moment a
	/// container may build its record (trap T5); `rack::InnerChain` owns that distinction so this
	/// device does not have to know about it.
	std::unique_ptr<rack::InnerChain> postChain_;

	/// **THE GRAIN LANE** — backlog EW's other half, and the one that is different in KIND from the
	/// master lane above. The master sees the summed output; this one is fed grain by grain, by a
	/// per-grain decision (`SlicePlan::shouldSendToLane`), so what comes out of it has been CHOSEN
	/// rather than merely processed.
	///
	/// **ITS OUTPUT REJOINS BEFORE THE MASTER LANE** — before master lane. So the
	/// master's input, and therefore its dry path, INCLUDES whatever the grain lane produced: a grain
	/// that took the detour is processed by the master like everything else.
	///
	/// It borrows `laneIds_` exactly as `postChain_` does, which is what gap G3 bought and what makes a
	/// `device::Destination{deviceId, index}` name one device across the two of them.
	std::unique_ptr<rack::InnerChain> grainChain_;

	/// **THE SEND BUS — the chosen grains, and nothing else.** Cleared every chunk and ACCUMULATED
	/// into, which is the one place in this device that accumulates: a send sums the grains that won
	/// their draw, where a renderer ASSIGNS its sample (see `mixCoeff_` for why that distinction is
	/// load-bearing rather than stylistic). A send buffer has no bit-exactness promise to keep — it is
	/// silent unless something was deliberately put in it.
	std::vector<float> sendL_;
	std::vector<float> sendR_;

	/// **THE LOOPER ROAD'S SEND LEVEL FOR THE PLAYING SLICE**, armed in `advanceSlice` beside
	/// `delaySend_` and for the same reasons. One scalar, because the looper plays one slice at a time.
	float grainSend_ = 0.0f;
	/// **THE GRAIN ROAD'S, PER VOICE** — Grain mode runs exactly two crossfaded readers
	/// (`grainPhaseA_`/`grainPhaseB_`), so two flags rather than a scalar: each voice wins or loses its
	/// own draw at its own wrap, from its own grain index, exactly as `grainRevA_`/`grainDryA_` do.
	bool grainLaneA_ = false;
	bool grainLaneB_ = false;
};

/// **THE CHAIN INSIDE A RUNNING INSTANCE, FROM THE OPAQUE HANDLE THE HOST FILES** — SKLITTER's
/// `rack::bridgeOf`, and deliberately the same shape.
///
/// A host files a container's live storage under its device id at provisioning
/// (`ContainerBodies::attach`) and hands it back as a `void*` through `DeviceHost::containerSurfaceOf`.
/// The cast that turns that pointer into a device is only safe in the module that BUILT the device —
/// `chain_expanded.h` states the same rule for the rack — so it is written here once rather than at
/// every surface that wants the chain.
///
/// **THE POINTER IS A CONTEMPORANEOUS HANDLE AND NOT AN IDENTITY** (CLAUDE.md's ABA rule): the
/// converger replaces a container's whole instance when a body arrives (trap T9), so a caller
/// re-asks `containerSurfaceOf` and drops what it built on a different answer rather than keeping
/// this pointer across ticks.
///
/// Null for a null handle, and for a moved-from device, which has no chain.
[[nodiscard]] rack::InnerChain* postChainOf(void* surface) noexcept;
[[nodiscard]] const rack::InnerChain* postChainOf(const void* surface) noexcept;

/// **THE OTHER LANE, THROUGH THE SAME HANDLE** — everything said above holds word for word, because
/// there is one handle per INSTANCE and not one per lane. An editor asks the host once and casts
/// twice.
///
/// **AND IT IS A SEPARATE FUNCTION RATHER THAN A LANE ARGUMENT, DELIBERATELY.** A `chainOf(surface,
/// lane)` would put a lane index in every caller and make "which lane" a runtime value on a road where
/// it is always a compile-time fact — the rail knows which column it is building. The four route verbs
/// are the place that genuinely cannot know (they are handed an id and must ASK, which is what
/// `laneHolding` is for); nothing on the editor side has that problem.
[[nodiscard]] rack::InnerChain* grainChainOf(void* surface) noexcept;
[[nodiscard]] const rack::InnerChain* grainChainOf(const void* surface) noexcept;

} // namespace feathers::fx::sklitter
