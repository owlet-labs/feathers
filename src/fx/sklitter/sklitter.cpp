/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#include "fx/sklitter/sklitter.h"

// FOR `performPushOf` — the perform circle's one builder, which every plan site below calls and none
// duplicates. `sklitter.h` deliberately does not pull this in: the header's `RestKey` needs `MacroPush`
// (from `slice_plan.h`) and not the blend that produces one.
#include "fx/sklitter/perform.h"
// **THE ONE TRANSLATION UNIT THAT SEES BOTH SIDES** — this file is compiled into
// `feathers-fx-sklitter`, which sits above `feathers-rack`. `sklitter.h` carries only a forward
// declaration; see the note beside it for why the header cannot reach here.
//
// **AND THE INCLUDE ORDER IS LOAD-BEARING, in the mild way `sklitter_types.h`'s own tail marker is
// about.** `rack/inner_chain.h` pulls `rack/rack_engine.h`, which no longer includes this device
// (the one static_assert it wanted moved to `src/plugin/device_registry.h`) — so there is no cycle
// to survive. It is written down because the day someone puts a device include back into
// `rack_engine.h`, this is where it would resurface.
#include "backplane/axes.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <new>

#ifdef FEATHERS_DIAGNOSTIC
#include <cstdarg>
#include <cstdio>
#include <ctime>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#include <xmmintrin.h>
#define FEATHERS_TRIPWIRE_MXCSR 1
#endif
#if !defined(_WIN32)
#include <unistd.h>
#endif
#endif

namespace feathers::fx::sklitter {

namespace {

//--------------------------------------------------------------------------------------------------//
// THE RATE MAPPING.                                                                                  //
//--------------------------------------------------------------------------------------------------//
//
// Rate spans four halvings, from a quarter note at 0 through a sixteenth at 0.5 to a sixty-fourth at
// 1, and it is always tempo-relative. `kSlowestDivisionBeats` (a quarter) and `kHalvings` (four) live
// in `slice_plan.h`, where the Rate readout reads the same pair (backlog GS).

/// WHEN THE HOST SAYS NOTHING. A device that produced no slice at all without a tempo would be
/// silent in a host that offers none; 120 BPM is the convention the rest of this repo uses.
constexpr double kAssumedSecondsPerBeat = 0.5;

/// Burst's Quantized press: the 0..1 Rate position snapped to a musical division.
double snapToDivision(double rate) {
	// The nearest of the five divisions 0, 1/4, 1/2, 3/4 and 1. Candidates are tried slowest first and
	// only a strictly closer one replaces the current pick, so an exact tie keeps the slower division.
	// A NaN is closer to nothing and so keeps the first candidate, 0.
	constexpr double kDivisions[] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
	double best = kDivisions[0];
	double bestDistance = std::abs(rate - best);
	for (const double division : kDivisions) {
		const double distance = std::abs(rate - division);
		if (distance < bestDistance) {
			best = division;
			bestDistance = distance;
		}
	}
	return best;
}

/// **THERE IS NO KNOB CONFIGURATION ANY MORE, AND THIS IS THE ZERO THAT SAYS SO.**
///
/// `planFor` and `offsetsFor` still take a `Macro Cfg` coordinate, because they are a faithful port of a
/// reference that HAS one (`slice_plan.h`'s REFERENCE/PORT overloads, and the twenty-one pinned plan
/// values in `test_fx_sklitter_modes.cpp` that are the whole of the parity story). **THIS DEVICE HAS
/// STOPPED HAVING ONE** — `Param::MacroConfig` was deleted on 2026-09-19 with `Param::Macro`, and each
/// waypoint carries its own configuration INSIDE the push `performPushOf` builds.
///
/// **WHAT THE TWO HALVES OF IT REACH, SO THIS IS CHECKABLE RATHER THAN A HOPE.** The `lap` becomes
/// `PhaseOffsets::macroConfig`, and the `position` becomes `planFor`'s third argument; both are read by
/// exactly one expression, the `macroConfigNorm` sum the plan publishes for the tests and reads nowhere
/// (`slice_plan.h` says so at the sum itself). So a zero here changes what that published number IS —
/// it is gamma alone for this device now — and changes NO decision the plan makes. Every decision comes
/// through `push`.
constexpr Coordinate kNoKnobConfig { 0.0f, 0.0f };

} // namespace

/// Where Rate is right now, as a 0..1 position: the live knob, or during a quantized Burst gesture
/// the press's snapped division moved by the knob's travel since the press.
double Sklitter::effectiveRatePosition() const {
	// The live knob clamped to 0..1. Inside a quantized gesture: the snapped division plus how far the
	// (clamped) knob has moved from its unclamped value at the press, clamped again.
	const double knob = std::clamp(static_cast<double>(target_.rate), 0.0, 1.0);
	if (!quantizedGesture_) {
		return knob;
	}
	const double travel = knob - static_cast<double>(rateAtPress_);
	return std::clamp(quantizedBase_ + travel, 0.0, 1.0);
}

size_t Sklitter::resolveSliceFrames() const {
	const double position = effectiveRatePosition();
	const double beats = kSlowestDivisionBeats * std::exp2(-kHalvings * position);

	const double secondsPerBeat = clock_.haveTempo() ? clock_.secondsPerBeat : kAssumedSecondsPerBeat;
	const double frames = beats * secondsPerBeat * sampleRate_;

	// Never zero -- a slice of no length divides by zero on its first wrap -- and never past what was
	// allocated at activate.
	const size_t capacity = ring_.capacity();
	if (capacity == 0) {
		return 0;
	}
	const auto rounded = static_cast<long long>(std::llround(frames));
	if (rounded < 1) {
		return 1;
	}
	return std::min(static_cast<size_t>(rounded), capacity);
}

/// ONE BAR, which is what a looper mode captures. Four beats of the host's tempo; without one, the
/// same 120 BPM assumption the division mapping makes.
size_t Sklitter::resolveBarFrames() const {
	const double secondsPerBeat = clock_.haveTempo() ? clock_.secondsPerBeat : kAssumedSecondsPerBeat;
	const double frames = static_cast<double>(kBeatsPerBar) * secondsPerBeat * sampleRate_;
	const size_t capacity = ring_.capacity();
	if (capacity == 0) {
		return 0;
	}
	const auto rounded = static_cast<long long>(std::llround(frames));
	if (rounded < 1) {
		return 1;
	}
	return std::min(static_cast<size_t>(rounded), capacity);
}

/// **ARM GRAIN'S OWN GEOMETRY — the two-voice cloud's length, spread, phases and read positions.**
///
/// **EXTRACTED BECAUSE IT HAD EXACTLY ONE CALLER AND NEEDED TWO** (backlog EQ; there is some
/// kind of glitch when moving from (tested) shuffle to grain. the grain is frozen until you
/// disable/reenable). It lived inline in the branch that runs when the GESTURE begins, gated on the
/// mode armed at that instant — so arriving in Grain by turning the Mode dropdown on a device already
/// playing never ran it, and the cloud inherited the construction defaults: spread ZERO, which puts
/// both voices on the same sample, and both read positions at the top of the buffer.
///
/// **IT DELIBERATELY TOUCHES NOTHING BUT GRAIN.** The obvious fix — `reset()` on a mode change — is
/// what the BYPASS road does, and it discards the captured loop: silencing a looper mid-performance,
/// which is a worse bug than the one being fixed. The buffer is the one thing a looper must keep
/// across a mode change.
void Sklitter::armGrain(float zoneA) {
	// RATE IS THE GRAIN LENGTH HERE, squared, with a ~23 ms floor; Zone A is the spread.
	const size_t kMinGrain = minGrainFrames_;
	const double small = std::clamp(1.0 - static_cast<double>(target_.rate), 0.0, 1.0);
	grainLength_ = std::clamp(static_cast<size_t>(static_cast<double>(sliceFrames_) * small * small),
	                          kMinGrain, std::max<size_t>(sliceFrames_, kMinGrain));
	const Coordinate az = splitCoordinate(zoneA);
	// ACROSS THE WHOLE CAPTURED LOOP: a spread is a read offset, and offsets reach the buffer.
	grainSpread_ = static_cast<size_t>(az.position * static_cast<double>(resolveBufferFrames()));
	grainPhaseA_ = 0.0;
	grainPhaseB_ = 0.5;
	grainPosA_ = 0;
	grainPosB_ = grainSpread_ / 2;
	grainOffsetA_ = 0;
	grainOffsetB_ = grainLength_ / 2; // the firmware's: B starts half way through, with its window
	grainRepeatLeftA_ = 0;
	grainRng_ = 0x9E3779B9u;
}

/// **CLASSIC AND BURST ENTERED FROM A LOOPER GESTURE — armed FROM THE CURSOR .**
/// Their press captures one Rate-sized slice into a reset ring; a looper gesture leaves a whole buffer
/// there instead and no slice at all (`capturedSliceFrames_` 0). So the slice is the live Rate's division,
/// clamped to the ring, starting at the frame the looper was reading when the Mode changed: the material
/// under the playhead goes on stuttering rather than the stutter jumping to the bar's top.
///
/// **`sliceFrames_` IS LEFT ALONE** — it is the captured bar, and turning back to a looper mode needs it.
void Sklitter::armSliceFromCursor() {
	const size_t ring = ring_.length();
	if (ring == 0) {
		return;
	}
	size_t cursor = 0;
	if (armedMode_ == Mode::Grain) {
		cursor = grainPosA_ + std::min(grainOffsetA_, grainLength_ > 0 ? grainLength_ - 1 : 0);
	}
	else {
		cursor = schedule_.startOffset + positionInSlice_;
	}
	sliceOrigin_ = cursor % ring;
	capturedSliceFrames_ = std::clamp<size_t>(resolveSliceFrames(), 1, ring);
}

/// **THE GRAIN'S FADE LENGTHS, PORTED FROM `prepareGrainEnvelopeQ31`.**
///
/// **THIS WAS MISSING ENTIRELY AND IT IS THE CLICK REPORTED SINCE THE PORT LANDED.**
/// The firmware ramps every grain linearly at BOTH ends, always; `attackFadeLen`, `decayFadeLen`,
/// `fadeLen` and `kAntiClickSamples` appeared NOWHERE in this port. What came across was the
/// zero-crossing gate and the two CONSECUTIVE guards — the flags that SUPPRESS the fade between
/// adjacent grains — but not the fade they guard. So our attack was silent until the first crossing
/// and then FULL LEVEL IMMEDIATELY, which is abrupt exactly when a crossing arrives late or never.
///
/// **THE TWO MECHANISMS ARE COMPLEMENTARY, WHICH IS WHY THE FIRMWARE HAS BOTH** (checked, rather than
/// assumed: `stutterer.cpp` runs `waitingForZeroCrossL` AND this fade). The gate picks a good place to
/// start; the ramp covers the case where the good place is late, or is not actually at zero.
///
/// THE ARITHMETIC IS THE REFERENCE'S, restated:
///   - a base fade of `kAntiClickSamples`, or HALF the gated length when the grain is shorter than two
///     of them — which is the same half-grain cap asked for on `Min Env`, arrived at from the
///     other direction;
///   - asymmetric by `envShape`: `min(envShape * 2, 1)` on the attack and `min((1 - envShape) * 2, 1)`
///     on the decay, so 0 is a percussive instant attack with a full decay and 1 is its mirror;
///   - a floor of `kMinAntiClickBase` on each end WHENEVER THE BASE FADE CAN AFFORD IT, which is what
///     keeps a fade on the end `envShape` has scaled to nothing.
///
/// **AND `Min Env` IS THE PLAYER'S FLOOR ON TOP OF IT**, by design. The control no longer IS the fade — it raises it. At its
/// default of off the grain gets exactly the firmware's envelope; above that it gets whichever is
/// longer, still capped at half the grain so a short one cannot be flattened.
///
///  gated the audible length of this grain, after `lengthMult`
Sklitter::GrainFade Sklitter::grainFadeLengths(size_t gated) const {
	GrainFade f;
	if (gated == 0) {
		return f;
	}
	const size_t base = gated > antiClickFrames_ * 2 ? antiClickFrames_ : gated / 2;
	const float shape = std::clamp(plan_.envShape, 0.0f, 1.0f);
	const float attackScale = std::min(shape * 2.0f, 1.0f);
	const float decayScale = std::min((1.0f - shape) * 2.0f, 1.0f);
	f.attack = static_cast<size_t>(static_cast<float>(base) * attackScale);
	f.decay = static_cast<size_t>(static_cast<float>(base) * decayScale);
	if (base >= minAntiClickFrames_) {
		f.attack = std::max(f.attack, minAntiClickFrames_);
		f.decay = std::max(f.decay, minAntiClickFrames_);
	}
	// **THE PLAYER'S FLOOR, THEN THE HALF-GRAIN CAP OVER BOTH.** Raising the floor must not be able to
	// flatten a short grain, which is the whole reason the cap exists.
	f.attack = std::max(f.attack, envFrames_);
	f.decay = std::max(f.decay, envFrames_);
	f.attack = std::min(f.attack, gated / 2);
	f.decay = std::min(f.decay, gated / 2);
	return f;
}

/// **ONE RATCHET SUB-SLICE, FLOORED AT 24 MS** — the reference's:
///
///     scatterSubSliceLength = currentSliceLength / scatterSubdivisions;
///     if (scatterSubSliceLength < kMinSubSliceSamples) {
///         size_t effectiveFloor = std::min(kMinSubSliceSamples, currentSliceLength);
///
/// A duration here, like the device's other floors, so it means 24 ms at every host rate.
size_t Sklitter::subSliceLength(size_t audible, size_t subdivisions) const {
	const size_t divided = audible / std::max<size_t>(subdivisions, 1);
	return divided < minSubSliceFrames_ ? std::min(minSubSliceFrames_, audible) : divided;
}

/// **THE RATCHET AS IT IS HEARD** (ribbon truth) — how many of `p`'s pieces START where the slot still sounds,
/// which is not the plan's count. `playLooper` cuts the SOUNDING length (the material when a swing lengthened
/// the slot past it, else the slot) times `lengthMult` into the count, floored at 24 ms; the last 15 ms of that
/// is the release zone a ratchet always has (its next slot is never a continuation), muted at the first
/// crossing; and with a one-bar buffer the bar's last 50 ms is muted the same way (`barEndSilence_`). A piece
/// that would start in any of those never sounds. `*piece` is one piece as a fraction of the slot, 0 unless
/// more than one sounds. The band's walk and its playing cell both ask here.
int32_t Sklitter::heardRatchet(const SlicePlan& p, size_t slot, size_t material, size_t slotStart, size_t buffer,
                               bool barEndSilent, float* piece) const {
	*piece = 0.0f;
	const int32_t count = std::clamp(p.subdivisions, 1, 12);
	if (count <= 1 || slot == 0) {
		return 1;
	}
	const size_t sounding = std::min(material, slot);
	const auto audible = static_cast<size_t>(static_cast<double>(sounding) * p.lengthMult);
	const size_t length = subSliceLength(audible, static_cast<size_t>(count));
	size_t sounds = audible > releaseZone_ ? audible - releaseZone_ : audible;
	if (barEndSilent && buffer > barEndZone_ && slotStart + sounds > buffer - barEndZone_) {
		sounds = buffer - barEndZone_ > slotStart ? buffer - barEndZone_ - slotStart : 0;
	}
	const size_t heard = length > 0 ? std::min<size_t>(static_cast<size_t>(count), (sounds + length - 1) / length) : 1u;
	if (heard > 1) {
		*piece = static_cast<float>(static_cast<double>(length) / static_cast<double>(slot));
	}
	return static_cast<int32_t>(std::max<size_t>(heard, 1));
}

/// **THE FADE'S GAIN AT A POSITION — one function, because the OUTPUT and the WRITE both need it.**
///
/// The firmware weights its `pWriteEnvQ31` with the same attack/decay ramp it applies to the output,
/// then takes a `min` against a separate write fade. Ours computed the ramp inside `blend` where only
/// the output could reach it, so the write got the gate envelope and no ramp at all — which is why
/// `pWrite` was the worst setting in five of six modes in the click sweep.
float Sklitter::fadeGainAt(size_t pos, size_t remaining, size_t attackLen, size_t decayLen) {
	float g = 1.0f;
	if (attackLen > 0 && pos < attackLen) {
		g = std::min(g, static_cast<float>(pos) / static_cast<float>(attackLen));
	}
	if (decayLen > 0 && remaining < decayLen) {
		g = std::min(g, static_cast<float>(remaining) / static_cast<float>(decayLen));
	}
	return g;
}

/// **DRY AGAINST WET, AND THE ATTACK FLOOR, AT EVERY RENDERER'S WRITE.**
///
/// **ONE FUNCTION WITH THREE CALLERS, DELIBERATELY.** `playBurst`, `playLooper` and
/// `playGrain` each own their output line, and this device has now produced the "a fix reached one
/// road and stopped" failure FIVE times — density to the slice modes and not Grain, the hard cut to
/// the looper renderer and not the grain one, the repaint to the card and not the panel, the mode
/// re-arm to `advanceSlice` which Grain never calls, and the ribbon to `playLooper` alone. A new
/// per-sample stage written inline at one site would be the sixth. The case that guards it sweeps
/// EVERY mode rather than the one being worked on.
///
///  dry the input sample, before this device touched it
///  wet what the renderer produced
float Sklitter::wetOf(float wet, size_t remaining, size_t attackLen, size_t decayLen) const {
	return wetAt(wet, envPos_, remaining, attackLen, decayLen);
}

/// **THE SAME, AT A FADE POSITION THE CALLER NAMES.** The looper fades each RATCHET sub-slice and each
/// GATED end, so its position is inside the sub-slice rather than `envPos_` from the slice start — see
/// `playLooper`.
float Sklitter::wetAt(float wet, size_t pos, size_t remaining, size_t attackLen,
                      size_t decayLen) const {
	// **THE ENVELOPE FLOOR FIRST, because it belongs to the GRAIN and the mix belongs to the DEVICE.**
	// Folding the dry in first and ramping afterwards would fade the INPUT up and down too, which is a
	// mute rather than a click guard — the dry signal is continuous and has no edge to protect.
	//
	// **BOTH ENDS, WHICH IS WHY THE CONTROL IS `Min Env` AND NOT `Min Attack`.** The first version
	// floored the attack only; the control became Min Env, shaping both attack and release. A floor under the fade-IN and none under the fade-OUT guards one edge of a
	// grain and leaves the other bare, and the end of a grain is where a slot is hard-zeroed — which is
	// as sharp a discontinuity as any start.
	//
	// **THE SMALLER OF THE TWO**, so a grain SHORTER than two ramps fades in and straight back out
	// rather than the two ramps fighting: at any sample the gain is whichever end is nearer.
	// **THROUGH `fadeGainAt`, which the WRITE path calls too** — see there for why one function.
	return wet * fadeGainAt(pos, remaining, attackLen, decayLen);
}

/// **THE DEVICE'S OWN SHARE OF THE OUTPUT AT THIS SAMPLE — `Mix`, times the engage crossfade.**
///
/// **AND AT THE GESTURE'S START THE WET CROSSFADES IN FROM THE DRY.** Until the trigger the device
/// passes the input; on the trigger frame the renderer takes over, and its first grain is silent until
/// a zero crossing — so the output fell from the live input to nothing in one sample. MEASURED: about
/// a fifth of the click sweep's remaining jumps were that one frame, one per engage. The reference
/// switches hard. Over the grain fade's base length the mix rises from zero instead.
///
/// **THE ENGAGE FADE TRAVELS WITH THE MIX AND NOT WITH THE ENVELOPE, which is the same division
/// `wetAt`'s header draws.** The envelope belongs to the GRAIN; this belongs to the GESTURE, and a
/// gesture is a thing the DEVICE is doing — so it applies after the lanes have had the wet, exactly as
/// `Mix` does. A lane therefore hears a full-strength grain from the first sample of an engage, and it
/// is the device's output that fades in.
/// **AND THE SAME RAMP RUNS BACKWARDS FOR THE UNLOCKED REJOIN — backlog HE.** A rejoin is a SEEK in
/// the middle of audio the player is hearing (locked, the device was parked and there is nothing to
/// interrupt), so the wet has to reach the dry before the cursor moves and come back from it after.
/// One shape, two directions: the same `engageFadeFrames_`, counted down instead of up. MEASURED
/// without it, on a 311.13 Hz probe whose own largest step is 0.0407: the step across the restart was
/// **0.511**, 12.5x the input, against 0.040 for the locked resume on the identical script.
float Sklitter::mixAt() const {
	float engage = 1.0f;
	if (engageFadeFrames_ > 0) {
		// **ON `rejoinDucked_`, NOT ON THE COUNTER REACHING ZERO, AND THE DIFFERENCE IS A CLICK.** A
		// duck that ran out while the host was still stopped would fall through to the branch below,
		// find the engage fade long finished, and snap the wet back to full at the dry. The flag holds
		// the device at the dry until the seek it is waiting for actually lands.
		if (rejoinDucked_) {
			engage = static_cast<float>(rejoinDuckLeft_) / static_cast<float>(engageFadeFrames_);
		}
		else if (engageFadePos_ < engageFadeFrames_) {
			engage = static_cast<float>(engageFadePos_) / static_cast<float>(engageFadeFrames_);
		}
	}
	return std::clamp(mixNow_, 0.0f, 1.0f) * engage;
}

/// THE CAPTURED LOOP, which is the bar scaled by the setting's rung.
///
/// **CLAMPED TO THE RING, AND THE CLAMP IS REACHABLE.** `kLooperSeconds` is 16, which holds four bars
/// down to 60 BPM and no lower — at 40 BPM a four-bar rung wants 24 seconds and gets 16. Silently
/// capturing less than the setting says is the honesty rule failing quietly, so `bufferBarsForTest`
/// exists for a case to read what was ACTUALLY taken, and the UI draws the buffer it was given rather
/// than the buffer that was asked for.
size_t Sklitter::resolveBufferFrames() const {
	const size_t bar = resolveBarFrames();
	if (bar == 0) {
		return 0;
	}
	// **CLASSIC AND BURST DO NOT HAVE A BUFFER IN THIS SENSE.** They capture one slice at Rate's
	// length and read it back; the setting is gated to the looper modes in the descriptor, and this
	// agrees rather than relying on the gate to be the only guard.
	if (!target_.isLooperMode()) {
		return bar;
	}
	const double want = static_cast<double>(bar) * target_.bufferBars;
	const auto rounded = static_cast<long long>(std::llround(want));
	if (rounded < 1) {
		return 1;
	}
	return std::min(static_cast<size_t>(rounded), ring_.capacity());
}

void Sklitter::prepare(double sampleRate, int maxFrames) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// THE ANTI-CLICK WINDOWS, IN TIME RATHER THAN IN THE REFERENCE'S 44.1 kHz SAMPLE COUNTS — see the
	// members. 15 ms covers a full cycle down to 33 Hz, which is what makes the crossing search find
	// one at all on bass material; 50 ms is the bar seam's slop.
	// **AND THE FOUR THAT WERE LEFT IN THE REFERENCE'S SAMPLES** — see `kMinBurstCycleSeconds` and its
	// neighbours. Converted HERE, beside the three that always were, because the whole point is that
	// every length in this device is a DURATION and only one place knows the host's rate.
	const auto frames = [this](double seconds) {
		const auto n = static_cast<long long>(std::llround(seconds * sampleRate_));
		return n < 1 ? size_t { 1 } : static_cast<size_t>(n);
	};
	minBurstCycle_ = frames(kMinBurstCycleSeconds);
	minBurstGrain_ = frames(kMinBurstGrainSeconds);
	minSliceFrames_ = frames(kMinSliceSeconds);
	minSubSliceFrames_ = frames(kMinSubSliceSeconds);
	minGrainFrames_ = frames(kMinGrainSeconds);
	antiClickFrames_ = frames(kAntiClickSeconds);
	minAntiClickFrames_ = frames(kMinAntiClickSeconds);
	engageFadeFrames_ = antiClickFrames_;
	pWriteFadeFrames_ = frames(kPWriteFadeSeconds);
	crossingDuckFrames_ = frames(kCrossingDuckSeconds);
	// The ramp's length follows the control, not the rate alone — recomputed in `setTarget`.
	envFrames_ = 0;
	releaseZone_ = static_cast<size_t>(std::lround(0.015 * sampleRate_));
	barEndZone_ = static_cast<size_t>(std::lround(0.050 * sampleRate_));
	// ~5 ms, the reference's `kBufferWrapFadeLen` of 220 at 44.1 kHz.
	wrapFade_ = static_cast<size_t>(std::lround(0.005 * sampleRate_));
	// backlog MJ: the Record Gate's hold, the hardcoded 500 ms.
	recordGateHoldFrames_ = static_cast<size_t>(std::lround(kRecordGateHoldSeconds * sampleRate_));
	recordGateHoldLeft_ = 0;

	// **THE PERFORM SPEED'S EMA COEFFICIENT, DERIVED WHERE THE RATE ARRIVES** — `1 - exp(-Δt/τ)` from
	// two constants and `sampleRate_`, none of which changes between ticks. It lives here for the same
	// reason the anti-click windows above do: this is the one place that knows the host's rate.
	speedCoeff_ = 1.0 - std::exp(-(static_cast<double>(kPerformTickSamples) / sampleRate_) / kSpeedTauSeconds);

	ring_.prepare(sampleRate_, kLooperSeconds);
	delay_.prepare(sampleRate_, kDelaySeconds);
	// **THE WET PATH'S SCRATCH, SIZED HERE AND NOWHERE ELSE** — trap T2, the same rule the post chain's
	// buffers below follow, and sized to the host's longest block (backlog GO). `processStereo` cuts a longer call to
	// this length, so these never need to grow and nothing after this line allocates.
	scratchFrames_ = static_cast<size_t>(device::scratchFrames(maxFrames));
	wetL_.assign(scratchFrames_, 0.0f);
	wetR_.assign(scratchFrames_, 0.0f);
	mixCoeff_.assign(scratchFrames_, 0.0f);
	sendL_.assign(scratchFrames_, 0.0f);
	sendR_.assign(scratchFrames_, 0.0f);
	captureScratchL_.assign(scratchFrames_, 0.0f); // backlog KZ: the input as RECORDED, when Input Gain is not 0 dB
	captureScratchR_.assign(scratchFrames_, 0.0f);
	freezeGain_.assign(scratchFrames_, 1.0f); // backlog KX: one write gain per sample of the longest chunk
	// **BACKLOG HN'S SCRATCH, ON THE SAME LINE OF ARGUMENT AND FOR THE SAME REASON.** One entry per
	// sample of the longest chunk is the worst case by construction: `linearBarPos_` advances once
	// per sample, so a chunk cannot record more writes than it has samples.
	deferPos_.assign(scratchFrames_, 0);
	deferSrcL_.assign(scratchFrames_, 0.0f);
	deferSrcR_.assign(scratchFrames_, 0.0f);
	deferMix_.assign(scratchFrames_, 0.0f);
	deferCount_ = 0;
	auditWritten_.assign(scratchFrames_, 0u);
	// **BACKLOG GU/GV'S SCRATCH, SIZED HERE FOR THE SAME REASON AND BY THE SAME WORST CASE.** One
	// entry per sample of the longest chunk: the write head advances once per sample, so a chunk
	// cannot record more heads than it has samples. Two more buffers than HN needed, because the
	// Grain FX tap has to be snapshotted before `runPostChain` rewrites the wet in place.
	rwPos_.assign(scratchFrames_, 0);
	rwWhat_.assign(scratchFrames_, 0u);
	rwPwL_.assign(scratchFrames_, 0.0f);
	rwPwR_.assign(scratchFrames_, 0.0f);
	rwPwMix_.assign(scratchFrames_, 0.0f);
	fbTapL_.assign(scratchFrames_, 0.0f);
	fbTapR_.assign(scratchFrames_, 0.0f);

	// **THE DAMPING COEFFICIENTS ARE RATE-DEPENDENT AND ARE COMPUTED WHERE THE RATE IS KNOWN**, which
	// is the defect the parity sweep found four times over in this file: a constant that means 44.1
	// kHz and is used at 96. `kFeedbackDampHz` and `kFeedbackDcHz` are FREQUENCIES for exactly that
	// reason, so the conversion happens once, here, against the real rate.
	const double sr = sampleRate_ > 0.0 ? sampleRate_ : kReferenceRate;
	constexpr double kTwoPi = 6.283185307179586476925286766559;
	dampA_ = static_cast<float>(1.0 - std::exp(-kTwoPi * kFeedbackDampHz / sr));
	dcR_ = static_cast<float>(std::exp(-kTwoPi * kFeedbackDcHz / sr));
	dampLpL_ = dampLpR_ = 0.0f;
	dcX1L_ = dcX1R_ = dcY1L_ = dcY1R_ = 0.0f;
	// **THE POST CHAIN'S SCRATCH IS SIZED HERE TOO, and this is the whole of trap T2 for it**: a chain
	// that sized its buffers on first use would allocate on the audio thread. `InnerChain::prepare` is
	// safe before or after `adopt` and carries the rate across to the bridge either way.
	if (postChain_ != nullptr) {
		postChain_->prepare(sampleRate_, maxFrames);
	}
	if (grainChain_ != nullptr) {
		grainChain_->prepare(sampleRate_, maxFrames);
	}
	reset();
}

void Sklitter::reset() {
	ring_.reset();
	captureGainPrimed_ = false; // the next chunk takes Input Gain where it stands rather than ramping from 0 dB
	// AND FREEZE WHERE IT STANDS (backlog KX). The ring was just zeroed — a host reset clears every buffer, frozen
	// or not, as a deactivate does — so there is no boundary to crossfade.
	freezePrimed_ = false;
	recordGateHoldLeft_ = 0; // backlog MJ: and the Record Gate shut, until the input next reaches it
	recordGateOpen_ = false;
	delay_.reset();
	delayTime_ = 0;
	delaySend_ = 0.0f;
	echoSend_ = 0.0f;
	echoGain_ = 0.0f;
	echoTime_ = 0;
	grainSend_ = 0.0f;
	grainLaneA_ = false;
	grainLaneB_ = false;
	engage_.reset();
	sliceFrames_ = 0;
	bufferFrames_ = 0;
	// NOTHING IS ARMED AFTER A RESET — or the next mode change would compare against a mode this
	// device is no longer playing and skip the arming it needs.
	armedMode_ = Mode::kCount;
	capturedBufferBars_ = 0.0;
	capturedSliceFrames_ = 0;
	sliceOrigin_ = 0;
	prewarming_ = false;
	prewarmBeat_ = INT64_MIN;
	prewarmed_ = 0;
	prewarmSeated_ = false;
	prewarmLastDrift_ = kNoDrift;
	// **THE REJOIN OWES NOTHING AFTER A RESET.** The debt it carries is "the cursor is somewhere a
	// transport stop left it"; a reset has just put the cursor at the start on purpose, and a seek
	// afterwards would move it off the position the reset just gave it.
	// **AND THE DUCK GOES WITH IT, OR THE WET STAYS AT THE DRY** — `mixAt` holds the device there until
	// the seek that will now never come.
	rejoinPending_ = false;
	rejoinDucked_ = false;
	rejoinDuckLeft_ = 0;
	parked_ = false; // backlog GT: the next stopped block arms its own ramp
	parkLeft_ = 0;
	pitchRatio_ = 1.0;
	quantizedGesture_ = false;
	quantizedBase_ = 0.0;
	rateAtPress_ = 0.0f;
	waitingForZeroCrossL_ = waitingForZeroCrossR_ = true;
	releaseMutedL_ = releaseMutedR_ = false;
	crossingDuck_ = 1.0f;
	crossingDuckA_ = 1.0f;
	crossingDuckB_ = 1.0f;
	prevOutputL_ = prevOutputR_ = 0.0f;
	consecutive_ = false;
	nextConsecutive_ = false;
	zcActive_ = false;
	barIndex_ = 0;
	barEndSilence_ = true;
	sliceIsDry_ = false;
	prevSliceDry_ = false;
	prevSliceCut_ = false;
	tailSilenced_ = false;
	prevReadEnd_ = kNoReadEnd;
	panFade_ = 1.0f;
	panCross_ = 0.0f;
	panRight_ = false;
	panActive_ = false;
	panCounter_ = 0;
	burstGrain_ = 0;
	burstCyclePos_ = 0;
	burstReadPos_ = 0;
	linearBarPos_ = 0;
	pWriteThisSlice_ = false;
	writeGain_ = 0.0f;
	repeatLoopIndex_ = 1;
	nextPlanCache_ = PlanCache {};
	hold_ = PlanHold {};
	holdNextSlice_ = -1;

	// **THE PERFORM SPEED FORGETS WHERE THE PUCK WAS** — design §4.5's "no spike on arrival". A patch load
	// or a transport reset can move the puck any distance at all, and without this the next tick would
	// read that jump as one tick's travel and publish a speed nothing did.
	//
	// **THIS IS THE ROAD A DUAL DEVICE ACTUALLY TAKES, which is why it is here and not in `kModOps.reset`:**
	// `RackEngine::reset` asks `if (d->audio != nullptr && d->audio->reset != nullptr) ... else if (d->mod
	// ...)`, so the AUDIO table's reset is the one that runs for this device and `kModOps.reset` is never
	// called. `Sklitter::prepare` reaches this by calling `reset()` at its end.
	//
	// **THE PUCK ITSELF IS NOT CLEARED, NOR ARE THE SIX GEOMETRY OUTPUTS.** The puck is a SETTING —
	// redelivered by `setPerformFields` every block — and the six are a pure function of it, so zeroing
	// either here would report a centred puck for one block after every reset and then jump back. Only
	// the SPEED is state, and only state is what a reset is for. (What a downstream input reads in the
	// meantime is not this device's cell anyway: `RackEngine::reset` stores 0 into every slot output of
	// its own accord, for exactly the window this paragraph is about.)
	havePuck_ = false;
	speed_ = 0.0;
	performOut_[outIndex(Out::Speed)] = 0.0f;

	// **AND THE HELD LEVEL, BECAUSE A HOLD IS STATE AND A RESET IS FOR STATE.** Left standing, a
	// transport stopped mid-bar would keep the circle sparking at the last bar's peak for as long as
	// nothing called the render again — the window only turns over on frames, deliberately (see the
	// members), so the thing that would normally retire it is the thing that has stopped.
	wetLevelPeak_ = 0.0f;
	wetLevelCandidate_ = 0.0f;
	grainLevelPeak_ = 0.0f;
	grainLevelCandidate_ = 0.0f;
	outLevelHeld_ = 0;

	// **AND THE CHAIN INSIDE DROPS ITS TAILS.** A reset is the host saying "nothing is playing"; a
	// delay in the post chain that kept its buffer would sound the previous take's audio at the next
	// note. `InnerChain::reset` settles its own ramp where it was aimed and does not clear the record.
	if (postChain_ != nullptr) {
		postChain_->reset();
	}
	if (grainChain_ != nullptr) {
		grainChain_->reset();
	}
}

//--------------------------------------------------------------------------------------------------//
// THE MOD ROAD — `kModOps` over the same object. Design §4.                                          //
//--------------------------------------------------------------------------------------------------//

void Sklitter::setPerformFields(const float* fields, size_t fieldsPresent) noexcept {
	// **THE PUCK ONLY, AND THIS MUST NOT CALL `setTarget`.** The audio road owns `target_`, and a second
	// writer is a second writer however equal the values look today.
	//
	// A FIELD THE HOST DID NOT SEND READS 0, which for both puck axes is the centre — the same answer
	// `Settings`' declared default gives, so a short row decodes to a centred puck on either road.
	const auto at = [&](Param p) {
		const size_t i = static_cast<size_t>(p);
		return i < fieldsPresent && fields != nullptr ? fields[i] : 0.0f;
	};
	// **ONE SANITISER, AND IT IS `performWeightsOf`'S.** It already de-NaNs and clamps; a second clamp
	// here would be a second road, and one that could publish a `Perf X` the weights beside it were not
	// built from. So the axes are read back OFF the weights (`PerformWeights::x`/`y`) rather than
	// computed again — the published axis and the published weight are then one value's two halves.
	const PerformWeights w = performWeightsOf(at(Param::PerfX), at(Param::PerfY));
	puckX_ = w.x;
	puckY_ = w.y;
	// **BY `Out`, NOT BY LITERAL.** `kOutputs` declares each ordinal's NAME and its declared range, and
	// the only thing that ever tied "index 3" to `Weight 2` was the order two lists were written in — a
	// reorder of the table would have published the puck under a weight's name and a weight's
	// `bipolar = false`, with the count assert that used to sit here still passing. `sklitter_types.h`
	// pins the ordinal-to-name mapping outright now; these reads say which one they mean.
	performOut_[outIndex(Out::X)] = w.x;
	performOut_[outIndex(Out::Y)] = w.y;
	performOut_[outIndex(Out::Weight1)] = w.corner[kCfg1];
	performOut_[outIndex(Out::Weight2)] = w.corner[kCfg2];
	performOut_[outIndex(Out::Weight3)] = w.corner[kCfg3];
	performOut_[outIndex(Out::Weight4)] = w.corner[kCfg4];
}

void Sklitter::tickPerform() noexcept {
	// DESIGN §4.5: the MEAN speed over τ, not a peak. The puck arrives in STEPS — a UI drag event every
	// ~16 ms, host automation once a block — so the per-tick distance is a spike followed by zeros; a
	// peak-hold would turn that granularity into loudness and read a coarser host as a FASTER hand.
	//
	// **NO `sampleRate_ <= 0` GUARD, and its absence is the honest statement.** One stood here; it was
	// unreachable — the member is 48000.0 from its declaration and `prepare` clamps anything non-positive
	// to the same value — and an unexplained guard tells the next reader that the value CAN be
	// non-positive, which would make `speedCoeff_` below worth re-deriving per tick. It cannot be.
	if (!havePuck_) {
		// THE FIRST TICK AFTER construct/prepare/reset MEASURES NOTHING. It only remembers where the puck
		// is, so the tick after it has a previous position that is a tick old rather than a patch old.
		lastPuckX_ = puckX_;
		lastPuckY_ = puckY_;
		havePuck_ = true;
		speed_ = 0.0;
		performOut_[outIndex(Out::Speed)] = 0.0f;
		return;
	}
	// **`dt` IS RECOMPUTED HERE WHILE `speedCoeff_` IS DERIVED IN `prepare`, AND THE TWO CANNOT DISAGREE.**
	// This file argues against exactly that shape everywhere else, so it is worth one clause: `prepare` is
	// the ONLY writer of `sampleRate_`, and both derivations read it — so they are one fact computed in two
	// places rather than two copies that can drift. What `prepare` buys is the `exp`, which is expensive
	// and constant; a divide is neither, and hoisting it would put a second rate-derived member beside the
	// first for no gain.
	const double dt = static_cast<double>(kPerformTickSamples) / sampleRate_;
	const double moved =
	    std::hypot(static_cast<double>(puckX_ - lastPuckX_), static_cast<double>(puckY_ - lastPuckY_));
	lastPuckX_ = puckX_;
	lastPuckY_ = puckY_;
	// **SAMPLE-RATE INDEPENDENT BY CONSTRUCTION**: the instantaneous reading is a distance over the time it
	// took, and the coefficient is `1 - exp(-dt/τ)` rather than a fixed fraction — so 44.1 k and 96 k agree
	// after the same ELAPSED time rather than after the same number of ticks. The coefficient is two
	// constants and the host's rate, so it is derived once in `prepare` (`speedCoeff_`) rather than by an
	// `exp` per tick that carries no information.
	speed_ += (moved / dt - speed_) * speedCoeff_;
	// **AND IT REACHES EXACT ZERO RATHER THAN PARKING IN THE DENORMALS.** A still puck decays by
	// `exp(-dt/τ)` per tick — 0.98347 at 64 samples and 48 kHz — so from 2 units/s it crosses the double
	// denormal boundary after about 42,500 ticks, under a minute of stillness, and then STICKS: the delta
	// `(0 - speed_) * speedCoeff_` underflows to exactly 0 and the value sits there for the session.
	// `multimode_filter.h` documents the same arithmetic and `puck_motion.h` paid for it first
	// ("multiplying by 0.982 each tick walks the value down through the denormals and then STICKS"); this
	// tree sets flush-to-zero nowhere, and automod, harm, shaper and filter each carry their own flush.
	// The cost here is two denormal operations per 64 samples rather than per sample, so this line is
	// bought for consistency with the house rather than for a measured saving — said plainly because the
	// magnitude is small and a comment implying otherwise would be the thing being guarded against.
	if (speed_ < 1e-12) {
		speed_ = 0.0;
	}
	// NORMALISED BY SATURATION, `v / (v + vHalf)` — 0 at rest, 0.5 at `kSpeedHalfUnitsPerSecond`,
	// approaching 1 and never reaching it. No clamp corner for a fast drag to sit against.
	performOut_[outIndex(Out::Speed)] = static_cast<float>(speed_ / (speed_ + kSpeedHalfUnitsPerSecond));
}

float Sklitter::performOutput(size_t output) const noexcept {
	return output < performOut_.size() ? performOut_[output] : 0.0f;
}

MacroPush Sklitter::targetPush() const {
	// **THE WEIGHTS AND THE DEPTH COME FROM THE SAME `Settings` AS THE WAYPOINTS, AND THIS IS THE ONE
	// PLACE THAT SAYS SO.** See the declaration for why the wide `performPushOf` is right for the seam
	// and wrong to spell out at every plan site.
	//
	// **NOTE WHICH PUCK THIS IS: `target_`'s, the AUDIO road's** — not `puckX_`/`puckY_`, which is the
	// mod road's copy and exists to be PUBLISHED. The sound must not depend on `advanceModulators`
	// having run: a host that only renders would freeze the puck at the centre. `setPerformFields`'
	// comment is the other half of this sentence.
	// AND THE ANCHORED PUSH UNDER IT (backlog HT), which `totalPushOf` leaves out exactly when there is none.
	return totalPushOf(target_,
	                   performPushOf(target_, performWeightsOf(target_.perfX, target_.perfY), target_.perfDepth));
}

void Sklitter::setTarget(const Settings& settings) {
	target_ = settings;
	// **THE RAMP'S LENGTH IN FRAMES, RESOLVED WHERE THE CONTROL ARRIVES.** Seconds times the host's
	// rate, exactly as the three anti-click windows are resolved in `prepare` — the difference is that
	// this one follows a control rather than a constant, so it cannot be settled once at activate.
	envFrames_ = target_.minEnvSeconds > 0.0f
	                    ? static_cast<size_t>(std::lround(static_cast<double>(target_.minEnvSeconds)
	                                                      * sampleRate_))
	                    : 0;
}

void Sklitter::setTransportClock(const device::TransportClock& clock) {
	clock_ = clock;
	// THE TEMPO REACHES INSIDE: a delay in either lane set to a note value needs the host's clock, and
	// this device is the only thing between it and the host. **BOTH LANES** — a tempo-synced device in
	// the grain lane would otherwise run at whatever a clock of zero means.
	if (postChain_ != nullptr) {
		postChain_->setTransportClock(clock);
	}
	if (grainChain_ != nullptr) {
		grainChain_->setTransportClock(clock);
	}
}

/// **RECORD WHILE IDLE, SO THE GESTURE HAS SOMETHING TO PLAY.** See `sklitter.h` for why this exists at
/// all. Three rules, all the reference's:
///
///   - ONLY WHILE THE DEVICE IS ARMED AND NOT PLAYING. `recordStandby` returns unless the status is
///     STANDBY, because once it is playing `pWrite` owns the buffer and two writers would fight.
///   - ONLY IN A LOOPER MODE. Burst captures a DIVISION at the press — a few thousand frames, no
///     perceptible wait — and pre-warming a bar for it would write over the slice it is about to
///     record.
///   - STARTING ON A BEAT. Frame 0 of the ring is a boundary, so the bar the device plays is aligned
///     to the grid rather than to whenever the plugin happened to load.
void Sklitter::prewarm(const float* left, const float* right, int frames) {
	const size_t bar = resolveBarFrames();
	if (bar == 0) {
		return;
	}
	const size_t buffer = resolveBufferFrames();
	// **THE BAR CAN CHANGE UNDER IT** — the host's tempo is automatable — and a ring holding half of
	// one bar and half of another is not a loop. Start again rather than keep a mixture.
	//
	// **AND THE BUFFER SETTING IS THE SECOND WAY IT CHANGES**, decided the same way in advance:
	// discarding and restarting the buffer when the setting switches is acceptable. So the test is on
	// BOTH lengths — a tree that watched only the bar would keep a one-bar recording and call it four.
	if (sliceFrames_ != bar || bufferFrames_ != buffer) {
		sliceFrames_ = bar;
		bufferFrames_ = buffer;
		capturedBufferBars_ = target_.bufferBars;
		ring_.setLength(buffer);
		resetRingUnlessFrozen();
		prewarming_ = false;
		prewarmBeat_ = INT64_MIN;
		prewarmed_ = 0;
	}

	// **A RING FILLED BEFORE THE TRANSPORT ROLLED HAS NO ANCHOR, SO THE FIRST ROLLING BLOCK TAKES ONE
	// (backlog IN).** A plugin loaded on a stopped DAW starts listening below with nothing to align to,
	// and nothing used to look again: ring frame 0 stayed wherever loading happened, so every loop played
	// back late by the frames heard before Play, modulo the buffer — MEASURED on the rack's own CLAP road,
	// exactly the pre-roll every time (1024, 2048, 4096 frames), at every block size and in both lock
	// settings. That audio is at no song phase, so it is discarded exactly as a first anchor discards
	// what came before it.
	if (prewarming_ && prewarmBeat_ == INT64_MIN && clock_.havePositionBeats() && clock_.playing) {
		prewarming_ = false;
	}

	if (!prewarming_) {
		// **WITHOUT A PLAYHEAD THERE IS NO BOUNDARY TO WAIT FOR**, and waiting for one that cannot
		// arrive would leave the device exactly as unwarmed as it was. A stopped host still reports a
		// tempo, so this asks the position — the same test `EngageMachine::advance` makes.
		if (!clock_.havePositionBeats() || !clock_.playing) {
			prewarming_ = true;
		}
		else {
			// **THE NEXT DOWNBEAT, NOT THE NEXT BEAT — and the difference is audible.** does it
			// begin in phase or from the start? MEASURED on the first version, which waited for any
			// boundary: ring index `i` held song phase `i + 24000`, so the captured bar sat ONE BEAT
			// off the musical one. Every slice the device then played was a beat displaced from the
			// grid, and a shuffle reordered material that was already in the wrong place — audible as
			// a loop that will not lock however carefully the zones are set.
			//
			// A BAR IS FOUR BEATS HERE (`resolveBarFrames`), so the boundary to start on is one whose
			// index divides by four. Frame 0 of the ring is then the song's downbeat, and the bar the
			// device plays is the bar the player heard.
			constexpr long long kBeats = kBeatsPerBar;
			const long long beat = static_cast<long long>(std::floor(clock_.songPositionBeats));
			const long long lastBeat = prewarmLastBeat_;
			prewarmLastBeat_ = beat;
			if (prewarmBeat_ == INT64_MIN) {
				prewarmBeat_ = ((beat / kBeats) + 1) * kBeats;
				return;
			}
			// **OR THE SONG WENT BACKWARDS — a DAW loop's wrap.** The next downbeat of a ONE-BAR loop is the
			// loop's end, which the song never reaches, so this waited for ever and every press there
			// recorded a whole bar before it could even wait for a grid line (the shape `EngageMachine::
			// advance` had, 2026-09-24). The wrap is where the loop starts again, so the pre-warm starts
			// there, anchored on the downbeat of the bar it landed in: `seatWriteHeadOnSong` below places
			// the head from that anchor, so a loop that starts mid-bar is written at its song phase too.
			if (beat < lastBeat) {
				prewarmBeat_ = static_cast<long long>(std::floor(clock_.songPositionBeats / static_cast<double>(kBeats)))
				               * kBeats;
			}
			else if (beat < prewarmBeat_) {
				return;
			}
			prewarming_ = true;
		}
		resetRingUnlessFrozen();
		prewarmed_ = 0;
		prewarmSeated_ = false; // a fresh warm is seated on its downbeat by `seatPrewarmHead`
	}

	// **THE ANCHOR ABOVE IS A BLOCK'S, THE DOWNBEAT IS A FRAME'S.** Writing begins at the first block
	// whose start is past the downbeat, so appending from 0 put ring frame 0 up to a block late and the
	// loop played EARLY by that much for ever — MEASURED at the device, 256 frames at 512-frame blocks,
	// 888 at 1024 and 44.1 kHz; our cases drove 480, which divides the 120 BPM bar and hid it.
	seatPrewarmHead(frames);
	for (int n = 0; n < frames; ++n) {
		ring_.setWriteGain(freezeGainAt(static_cast<size_t>(n))); // backlog KX: frozen, the head walks and nothing lands
		ring_.write(left[n], right[n]);
	}
	// **SATURATING, NOT WRAPPING.** The ring itself wraps and goes on holding the most recent bar;
	// this count only has to answer "is there a whole one", and letting it grow without bound would
	// make a device left running for an hour carry a number nobody reads.
	// **AGAINST THE BUFFER, NOT THE BAR.** A four-bar buffer is not warm after one bar: it would
	// engage on a ring three quarters full of whatever was there before, which is silence on a device
	// just loaded and last hour's audio on one that was not.
	prewarmed_ = std::min(prewarmed_ + static_cast<size_t>(frames), buffer);
}

//--------------------------------------------------------------------------------------------------//
// THE TRANSPORT LOCK — backlog GT..                                               //
//--------------------------------------------------------------------------------------------------//

bool Sklitter::transportLockActive() const noexcept {
	// **THE MODE GATE IS THE DESCRIPTOR'S, RESTATED HERE BECAUSE THE ENGINE IS WHAT DECIDES.** The
	// honesty rule says a control is enabled iff it changes sound, and it says to verify that by
	// reading the engine — so the engine holds the same condition the card draws from `kLooperModes`,
	// rather than trusting that a hidden control is also an inert one.
	return target_.transportLocked && target_.isLooperMode();
}

/// **THE BAR GRID IS THE SONG'S, AND THE ANCHOR CANCELS OUT OF IT.** `prewarm` waits for a beat index
/// divisible by four before it writes a sample (`sklitter.cpp`'s `((beat / kBeatsPerBar) + 1) *
/// kBeatsPerBar`), so ring frame 0 sits on a song downbeat and the bar the read cursor walks is a song
/// bar. That makes the phase `songPositionBeats mod 4` with nothing subtracted — which matters for more
/// than tidiness: `prewarmBeat_` is cleared on a tempo change and on a buffer-length change, and a phase
/// that needed it would go blind at exactly the two moments the device is already re-capturing.
///
/// **IN BARS RATHER THAN IN FRAMES, BECAUSE THE TWO BARS ARE NOT THE SAME NUMBER.** `sliceFrames_` is
/// the bar as it was at the press; the transport's bar is the bar as it is now. Returning a FRACTION
/// lets the caller multiply by whichever one it means, and the caller means the captured one.
double Sklitter::lockedBarPhase(double atFrame) const noexcept {
	if (!clock_.havePositionBeats() || !clock_.haveTempo() || sampleRate_ <= 0.0) {
		return -1.0;
	}
	const double framesPerBeat = clock_.secondsPerBeat * sampleRate_;
	if (!(framesPerBeat > 0.0)) {
		return -1.0;
	}
	const double beats = clock_.songPositionBeats + atFrame / framesPerBeat;
	const double bars = beats / static_cast<double>(kBeatsPerBar);
	return bars - std::floor(bars);
}

/// **THE ONE PART OF THE LOCK THAT DOES NEED THE ANCHOR.** `barIndex_` is not a position in the bar, it
/// is WHICH bar — the number `planFor` folds to `Param::SeqLength` and reads as the arrangement's
/// evolution. Counted from the ring's frame 0, a DAW loop over the same bars therefore plays the same
/// arrangement every pass, which is the property that makes this repeatable rather than merely restartable.
///
/// **−1 MEANS "NO ANCHOR", AND IT IS A REAL STATE RATHER THAN AN ERROR.** `prewarmBeat_` is `INT64_MIN`
/// until a beat-aligned pre-warm has stamped it, and it is cleared again whenever the tempo or the
/// buffer length moves. The caller then leaves `barIndex_` free-running: the phase within the bar is
/// still the transport's, and only the arrangement's bar keeps its own count.
int32_t Sklitter::lockedBarIndex(double atFrame) const noexcept {
	if (prewarmBeat_ == INT64_MIN || !clock_.havePositionBeats() || !clock_.haveTempo()
	    || sampleRate_ <= 0.0) {
		return -1;
	}
	const double framesPerBeat = clock_.secondsPerBeat * sampleRate_;
	if (!(framesPerBeat > 0.0)) {
		return -1;
	}
	const double beats =
	    clock_.songPositionBeats + atFrame / framesPerBeat - static_cast<double>(prewarmBeat_);
	const auto bars = static_cast<long long>(std::floor(beats / static_cast<double>(kBeatsPerBar)));
	// **EUCLIDEAN, NOT `%`.** A restart BEFORE the engage point is a legal state — the point is to wrap the
	// engage point against the sequence length to keep it in phase, not to trigger at the exact same
	// time in the track — so `bars` is genuinely negative
	// there, and C's `%` would hand back a negative bar index for `planFor` to index with.
	const auto wrapped = static_cast<int32_t>(((bars % kBarIndexWrap) + kBarIndexWrap) % kBarIndexWrap);
	return wrapped;
}

/// **THE POSITION ↔ (SLICE, OFFSET) RELATION, READ IN THE OTHER DIRECTION.** `playLooper` computes
/// `barPos = sliceIndex_ * slotLength + positionInSlice_`; this inverts it against the BASE slot,
/// which is the grid the schedule is laid out on (`scheduleFor`: `sliceLength = baseSliceLength *
/// grainLength`, and the last grain of the bar swallows the remainder).
///
/// **THE LAST SLICE ABSORBS THE REMAINDER HERE TOO**, by clamping the index rather than by dividing
/// again: an integer `baseSliceLength` loses up to `numSlices - 1` frames per bar, so the final frames
/// of the bar divide out to an index one past the end. Clamping puts them in the slice that actually
/// plays them, which is the same slice `scheduleFor` lengthened to hold them.
bool Sklitter::lockedCursor(double atFrame, int32_t* slice, size_t* within) const noexcept {
	const double phase = lockedBarPhase(atFrame);
	if (phase < 0.0 || sliceFrames_ == 0 || numSlices_ <= 0) {
		return false;
	}
	const size_t base = sliceFrames_ / static_cast<size_t>(numSlices_);
	if (base == 0) {
		return false;
	}
	// **ROUNDED TO THE NEAREST FRAME, NOT TRUNCATED — and on a steady transport this is the whole of
	// what the lock did to the sound.** A slot end asks about the frame that sits EXACTLY on a base-grid
	// boundary, and a song position one ulp low (a host's accumulated position, CLAP's fixed-point beat
	// time, or this function's own `atFrame / framesPerBeat`) made the product 11999.9999… where 12000
	// was meant, which truncation floors into the slice that just FINISHED. The lock then installed it
	// again and ran a whole slice late until a boundary happened to land high — MEASURED on the honesty
	// matrix's fixture, Shuffle: 91 of 300 blocks differing from the unlocked setting on a transport
	// that never stops, the read cursor off the song's slice in 222 of them. The position is
	// sample-quantised, so the nearest sample is the answer; the wrap takes a bar's last half frame to
	// the top of the next.
	const auto framesIn = static_cast<size_t>(std::llround(phase * static_cast<double>(sliceFrames_)))
	                      % sliceFrames_;
	// **UNDER GROOVE THE GRID IS THE WARPED ONE, SO THE LOCK AND THE FREE WALK NAME THE SAME SLICE (backlog
	// IG).** The walk ends each swung slot on `grooveBoundary(s + g)`; this inverts exactly that function,
	// on the same rounded frame, so a slot end under a steady transport lands on the boundary the walk
	// just reached and the derivation names the slice the walk would have taken. The straight grid keeps
	// its own two lines below — the zero-change guarantee is the original arithmetic, not an equal one.
	if (const GrooveFeel feel = target_.grooveFeel(); grooveActive(feel)) {
		*slice = grooveSliceAt(feel, framesIn, numSlices_, sliceFrames_, within);
		return true;
	}
	const size_t index = std::min(framesIn / base, static_cast<size_t>(numSlices_ - 1));
	*slice = static_cast<int32_t>(index);
	*within = framesIn - index * base;
	return true;
}

/// **THE WRITE HEAD'S PHASE, WHICH IS THE BUFFER'S RATHER THAN THE BAR'S — backlog GU.** The goal
/// is a continuous write that stays in phase. `linearBarPos_` walks once per sample
/// and nothing re-derived it, so it held phase only while nothing disturbed it. MEASURED before this
/// existed, pre-warmed from beat 0 (ring frame 0 on beat 4) at 120 BPM: a press at 10.5 put fresh input
/// from beat 11 into ring frame 0 — a quarter-buffer off the recording it overwrites, in STEADY play —
/// and a restart, a loop jump or a tempo change each moved it again (the table is in GU ADDENDUM 4).
///
/// **CALLED ONLY WHERE THE READ CURSOR IS RE-DERIVED** — `seekToTransport` and the locked slot end — so
/// the two heads are re-phased by one road and at the same instants. A second decision about WHEN
/// would be two answers to one question; in particular the unlocked setting's free run leaves both
/// heads alone together.
///
/// **THE BUFFER, NOT THE BAR, IS WHAT THE HEAD WALKS**, so a multi-bar buffer needs to know WHICH bar
/// the song is in, and that is what the anchor is for: ring frame 0 sits on song beat `prewarmBeat_`,
/// so the head is `(songBeats − prewarmBeat_) mod bufferBeats`. **WITHOUT AN ANCHOR** the lock's own
/// premise still holds — frame 0 is a downbeat (`lockedBarPhase`) — which is enough for a buffer of a
/// bar or less, since those lengths divide the bar; a longer one keeps the bar its head is in and
/// takes only the phase inside it, exactly as `lockedBarIndex` leaves `barIndex_` counting.
///
/// **THE BUFFER'S LENGTH IN BEATS IS THE CAPTURED ONE**, `capturedBufferBars_`: a tempo change moves
/// how long a beat is, not how many beats the recording holds.
bool Sklitter::lockedWriteHead(double atFrame, size_t* head) const noexcept {
	const double phase = lockedBarPhase(atFrame);
	if (phase < 0.0 || bufferFrames_ == 0 || !(capturedBufferBars_ > 0.0)) {
		return false;
	}
	const auto buffer = static_cast<double>(bufferFrames_);
	double at = 0.0;
	if (prewarmBeat_ != INT64_MIN || capturedBufferBars_ <= 1.0) {
		const double framesPerBeat = clock_.secondsPerBeat * sampleRate_;
		const double anchor = prewarmBeat_ != INT64_MIN ? static_cast<double>(prewarmBeat_) : 0.0;
		const double through = (clock_.songPositionBeats + atFrame / framesPerBeat - anchor)
		                       / (capturedBufferBars_ * static_cast<double>(kBeatsPerBar));
		at = (through - std::floor(through)) * buffer;
	}
	else {
		const auto bar = static_cast<double>(sliceFrames_);
		if (!(bar > 0.0)) {
			return false;
		}
		at = std::floor(static_cast<double>(linearBarPos_) / bar) * bar + phase * bar;
	}
	// **ROUNDED AND WRAPPED, for the reason `lockedCursor` gives** — truncating put the head one frame
	// behind the read cursor's boundary whenever the song position arrived an ulp low.
	*head = static_cast<size_t>(std::llround(at)) % bufferFrames_;
	return true;
}

/// **ONLY ON A ROLLING SONG.** A parked transport reports one position for every block, so seating on
/// it would write each stopped block over the last; with no song to follow the head keeps appending,
/// which is what it did before. And only when the ring IS the buffer the derivation measures.
void Sklitter::seatWriteHeadOnSong() noexcept {
	if (!clock_.playing || bufferFrames_ == 0 || ring_.length() != bufferFrames_) {
		return;
	}
	size_t head = 0;
	// **AT THIS CHUNK'S FIRST SAMPLE, NOT THE CALL'S** — `processStereo` cuts a long call at
	// its scratch, and `clock_` is the call's. At 0 here, every chunk of a call longer than the scratch
	// was seated back on the call's first sample and wrote over the chunk before it (MEASURED: after a tone
	// and then silence in 8192-frame calls the ring still peaked at 0.5). The same shape as the 2026-09-24
	// crush, one level down; the other seats in this file already add `windowBase_`.
	if (lockedWriteHead(static_cast<double>(windowBase_), &head)) {
		ring_.seekWrite(head);
	}
}

/// **THE IDLE HEAD: SEATED ONCE, THEN APPENDED, RE-SEATED ONLY ON A CONFIRMED DISCONTINUITY** (the
/// 2026-09-24 crush's guard). This used to seat on the song at EVERY call, which is right only while the
/// host's clock is right at every call: a host that hands several calls one position (the Feathers lane
/// before de54acb1, a nested host before 950ce9af) had every one of them write the SAME frames, and the
/// ring kept whatever the rest held. The capture below already seats once and appends; now the idle road
/// does too.
///
/// **A DISCONTINUITY IS A DISAGREEMENT THAT HOLDS STILL.** Each call compares where the song says the head
/// is with where appending put it. Within `kPrewarmSeatToleranceFrames` it is noise and the head appends.
/// Past it, the call's disagreement is compared with the last call's: a locate or a loop wrap moves the
/// song once and then advances with the audio, so the disagreement is the same on the next call and the
/// head is re-seated there; a stale or repeated position grows the disagreement by the call's length every
/// call, never holds still, and is never followed. The cost is one call written at the old phase after a
/// genuine jump — a call's worth of frames, overwritten on the next pass round the ring.
void Sklitter::seatPrewarmHead(int frames) noexcept {
	if (!clock_.playing || bufferFrames_ == 0 || ring_.length() != bufferFrames_) {
		// A parked song has no phase to follow (see `seatWriteHeadOnSong`); when it rolls again the head
		// is seated afresh rather than compared with where the parked blocks appended it.
		prewarmSeated_ = false;
		return;
	}
	size_t head = 0;
	if (!lockedWriteHead(static_cast<double>(windowBase_), &head)) {
		return;
	}
	if (!prewarmSeated_) {
		ring_.seekWrite(head);
		prewarmSeated_ = true;
		prewarmLastDrift_ = kNoDrift;
		prewarmDrift_ = 0;
		return;
	}
	const auto len = static_cast<long long>(bufferFrames_);
	long long drift = (static_cast<long long>(head) - static_cast<long long>(ring_.writePos())) % len;
	drift = drift > len / 2 ? drift - len : (drift < -len / 2 ? drift + len : drift);
	prewarmDrift_ = drift;
	if (std::llabs(drift) <= kPrewarmSeatToleranceFrames) {
		prewarmLastDrift_ = drift;
		return;
	}
	if (prewarmLastDrift_ != kNoDrift && std::llabs(drift - prewarmLastDrift_) <= kPrewarmSeatToleranceFrames) {
		ring_.seekWrite(head);
		++prewarmReseats_;
		tripwireReseat(drift, frames);
		prewarmLastDrift_ = kNoDrift;
		return;
	}
	prewarmLastDrift_ = drift;
}

/// **THE SEEK — used on a RESUME, where the cursor cannot wait for the next slot end.** A stop parks
/// the cursor wherever the block ended; the first sample after the restart is already meant to be in
/// phase, so the offset INSIDE the slice is installed too rather than rounded down to a slice boundary.
bool Sklitter::seekToTransport(double atFrame) {
	int32_t slice = 0;
	size_t within = 0;
	if (!lockedCursor(atFrame, &slice, &within)) {
		return false;
	}
	sliceIndex_ = slice;
	// **THE SLOT BEGAN ON THE BOUNDARY THE CURSOR WAS DERIVED FROM (backlog IG)**, straight or swung — an
	// absolute position, so a seek is where a groove's anchor restarts along with the bar's.
	slotStartFrame_ = grooveBoundary(target_.grooveFeel(), slice, numSlices_, sliceFrames_);
	grooveAnchored_ = false;
	const int32_t bar = lockedBarIndex(atFrame);
	if (bar >= 0) {
		barIndex_ = bar;
	}
	// **THE WRITE HEAD REJOINS WITH THE READ CURSOR (backlog GU)** — see `lockedWriteHead`. Before
	// `advanceSlice`, because the overlap duck it runs compares the read region against this head.
	size_t head = 0;
	if (lockedWriteHead(atFrame, &head)) {
		linearBarPos_ = head;
	}
	// The plan and the schedule are a pure function of the index and the bar, and both just moved —
	// `advanceSlice` is the one builder of them, so the seek goes through it rather than around it.
	advanceSlice();
	// **AFTER `advanceSlice`, WHICH ZEROES IT.** And clamped, because the schedule it just built may be
	// a long grain whose slot is shorter than the offset the base grid implies at the bar's last slice.
	// `within` is into the LOCKED slice; a seek that joined a long grain (`advanceSlice`) is that far further in.
	positionInSlice_ = schedule_.sliceLength > 0 ? std::min(within + joinEntry_, schedule_.sliceLength - 1) : 0;
	// **AND THE OUTPUT CROSSFADES BACK IN FROM THE DRY**, exactly as it does when a gesture begins
	// (`engageFadePos_`). A resume is a start as far as the wet is concerned: the device was silent for
	// however long the host was stopped, and stepping straight to a mid-grain sample is a click.
	engageFadePos_ = 0;
	return true;
}

void Sklitter::processStereo(float* left, float* right, int frames) {
	// **THIS CALL'S WINDOWS, TAKEN ONCE HERE RATHER THAN ONCE PER CHUNK.** They were consumed in
	// `renderStereo` while that was called exactly once per host block; now that the block is cut, a
	// second chunk would find `pendingMixRun_` already nulled and silently fall back to the settings —
	// a routed Mix that worked for the first 4096 samples and stopped.
	mixRun_ = pendingMixRun_;
	widthRun_ = pendingWidthRun_;
	pendingMixRun_ = nullptr;
	pendingWidthRun_ = nullptr;

	if (frames <= 0 || scratchFrames_ == 0) { // and nothing before `prepare` has sized the scratch
		return;
	}
	tripwireBlock(frames); // the crush tripwire; empty outside a diagnostic build (`tripwire.h`)
	landedAtBlock_ = ring_.landed(); // backlog MJ: the record light is whether this call lands a write
	// **A LOCKED STOP OWES ONE 5 ms RAMP, ARMED ON ITS FIRST STOPPED BLOCK — backlog GT.** See `parked_`. Armed
	// here rather than in `renderStereo` because the chunk below is cut where the ramp ends; in the rack a
	// block is 64 frames, so the ramp spans several calls and `parkLeft_` carries it between them.
	if (!clock_.playing && transportLockActive()) {
		if (!parked_) {
			parked_ = true;
			parkLeft_ = wrapFade_;
		}
	}
	else {
		parked_ = false;
		parkLeft_ = 0;
	}
	// **CUT TO THE SCRATCH**, which `prepare` sized to the host's longest block (backlog GO) — so a host keeping
	// its promise is rendered in ONE piece, and this loop is the guard for a call that breaks it (a host out of
	// contract, a case driving the device directly). In the rack it runs once whatever the host's buffer:
	// `RackEngine::processStereo` already cuts every device's block at `device::kTickWindow`, which is 64.
	const auto total = static_cast<size_t>(frames);
	size_t done = 0;
	while (done < total) {
		size_t n = std::min(scratchFrames_, total - done);
		if (parkLeft_ > 0) {
			n = std::min(n, parkLeft_); // the stop's ramp ends on a chunk boundary, and the rest is parked
		}
		windowBase_ = static_cast<int>(done);
		renderChunk(left + done, right + done, static_cast<int>(n));
		done += n;
	}
	windowBase_ = 0;
	recording_ = ring_.landed() != landedAtBlock_; // backlog MJ: `Reading::Recording`, from where the writes land
}

/// **RENDER, LANES, BLEND — in that order, by design.**
///
/// The master dry/wet is post fx (2026-09-20), reversing the placement the master lane shipped
/// with. So the lane sees the WET and the blend happens after it, against a dry that was never
/// touched.
///
/// The wet and the coefficient are cleared first, and an unwritten sample is therefore a bit-exact
/// pass of the dry — see where they are declared for why that is load-bearing rather than tidy.
//--------------------------------------------------------------------------------------------------//
// BACKLOG HN — THE DEFERRED WRITE AND ITS AUDIT. Test-only; see `sklitter.h` for the argument.     //
//--------------------------------------------------------------------------------------------------//

bool Sklitter::recordDeferredWrite(size_t pos, float srcL, float srcR, float mix) noexcept {
	if (deferCount_ >= deferPos_.size()) {
		return false; // cannot happen with a chunk of `scratchFrames_`; the guard is the proof, not a hope
	}
	deferPos_[deferCount_] = pos;
	deferSrcL_[deferCount_] = srcL;
	deferSrcR_[deferCount_] = srcR;
	deferMix_[deferCount_] = mix;
	++deferCount_;
	return true;
}

/// **THE `old` IS READ HERE, NOT AT RECORD TIME, AND THAT IS THE DEFERRAL BEING FAITHFUL.** The write
/// is a read-modify-write (`old + (src − old)·mix`); recording `old` during the render would make the
/// deferred path insensitive to anything that touched the ring in between, which is exactly the class
/// of difference this instrument exists to find.
void Sklitter::flushDeferredWrites() noexcept {
	// BACKLOG KX: a deferred write does not know its sample, so it lands at the gain the chunk ended on.
	ring_.setWriteGain(freezeWriteGain_);
	for (size_t k = 0; k < deferCount_; ++k) {
		float oldL = 0.0f;
		float oldR = 0.0f;
		ring_.readAt(deferPos_[k], &oldL, &oldR);
		const float mix = deferMix_[k];
		writeLoop(deferPos_[k], oldL + (deferSrcL_[k] - oldL) * mix,
		              oldR + (deferSrcR_[k] - oldR) * mix);
	}
	deferCount_ = 0;
}

void Sklitter::auditWrite(size_t pos) noexcept {
	if (!auditReads_ || bufferFrames_ == 0) {
		return;
	}
	++auditWrites_;
	const size_t idx = (pos + bufferFrames_ - (auditLo_ % bufferFrames_)) % bufferFrames_;
	if (idx < auditWritten_.size()) {
		auditWritten_[idx] = 1u;
	}
}

/// **`auditWritten_` IS CLEARED AT EVERY CHUNK, WHICH IS WHAT MAKES A SET FLAG MEAN "EARLIER IN THIS
/// CHUNK" RATHER THAN "AT SOME POINT".** The index is the read position's distance ahead of the
/// chunk's starting write head, and a flag can only be set for a sample already rendered, so a hit is
/// a read of material this chunk wrote.
void Sklitter::auditRead(size_t pos) noexcept {
	if (!auditReads_ || bufferFrames_ == 0) {
		return;
	}
	++auditReads1_;
	const size_t idx = (pos % bufferFrames_ + bufferFrames_ - (auditLo_ % bufferFrames_)) % bufferFrames_;
	if (idx < auditWritten_.size() && auditWritten_[idx] != 0u) {
		++auditHits_;
	}
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG GU/GV — THE THREE SEQUENTIAL BLENDS. See `sklitter.h` for the decisions behind each line. //
//--------------------------------------------------------------------------------------------------//

void Sklitter::recordRingWrite(int i, size_t pos) noexcept {
	const auto k = static_cast<size_t>(i);
	if (k >= rwWhat_.size()) {
		return; // cannot happen at `scratchFrames_`; the guard is the proof rather than a hope
	}
	rwPos_[k] = pos;
	rwWhat_[k] |= kHeadLive;
}

/// **THE DRY WRITER IS NOT DEFERRED, BECAUSE NOTHING IT NEEDS IS LATE (2026-09-24, by design,
/// backlog JC).** The deferral exists for FEEDBACK, whose source is the wet after both lanes — material
/// that does not exist until the chunk has rendered. The dry writer's source is the host's input, which
/// is in hand at the sample. Deferring it anyway meant a read of a frame the head had passed earlier in
/// the same chunk got the frame's PREVIOUS contents, a bar old, and then the new ones once the chunk
/// flushed — so a read trailing the head by less than a block flipped between two recordings at every
/// block boundary. At 1x no read trails the head (the looper reads its own slot at the head, before the
/// write), which is why HN's sweep measured 0 hazard reads; a PITCHED read does, and does it routinely.
/// MEASURED on a 113.3 Hz sine, Dry Write 1, puck at rest: pitched-slice clicks 77/117/66/128 (Time,
/// Shuffle, Pattern, Pitch) before, 17/8/4/40 with this, and what is left is the head itself — see the
/// crossing duck in `playLooper`.
///
/// **THE ORDER IS UNCHANGED.** Dry lands in the sample; feedback and `P Write` land at the flush, after
/// it; so each frame still sees dry, then feedback, then `P Write`, and the ring after the flush is the
/// ring the three sequential blends always made. Only reads WITHIN the chunk see anything different.
/// **THE CROSSING DUCK'S TARGET for a read at ring frame `at` moving at `v` frames a sample relative to
/// the write head** — 0 at the head, 1 at `crossingDuckFrames_ · |v|` frames from it and beyond, which is
/// `crossingDuckFrames_` OUTPUT samples either side of the crossing at any velocity. 1 when no continuous
/// writer is on (a frozen ring has no join) or when the read moves with the head (it never meets it).
/// `playLooper` carries the measurement.
float Sklitter::crossingDuckTarget(long long at, double v) const noexcept {
	if (!writersLive_ || at < 0 || bufferFrames_ == 0 || crossingDuckFrames_ == 0 || std::abs(v) < 1e-9) {
		return 1.0f;
	}
	const auto len = static_cast<long long>(bufferFrames_);
	long long delta = ((at - static_cast<long long>(linearBarPos_)) % len + len) % len;
	delta = delta > len / 2 ? len - delta : delta;
	const double reach = static_cast<double>(crossingDuckFrames_) * std::abs(v);
	return static_cast<float>(std::min(1.0, static_cast<double>(delta) / reach));
}

/// One sample of a crossing duck's gain toward `target`, slewed at one over the width a sample.
float Sklitter::slewCrossingDuck(float* gain, float target) const noexcept {
	const float slew = crossingDuckFrames_ > 0 ? 1.0f / static_cast<float>(crossingDuckFrames_) : 1.0f;
	*gain = std::clamp(target, *gain - slew, *gain + slew);
	return *gain;
}

void Sklitter::writeLoop(size_t pos, float left, float right) noexcept {
	const auto floored = [](float v) { return std::fabs(v) < kLoopFloor ? 0.0f : v; };
	ring_.writeAt(pos, floored(left), floored(right));
}

void Sklitter::writeDryNow(size_t pos, float dryL, float dryR) noexcept {
	const float dryAmt = std::clamp(target_.dryWrite, 0.0f, 1.0f);
	if (dryAmt <= 0.0f) {
		return;
	}
	float oldL = 0.0f;
	float oldR = 0.0f;
	ring_.readAt(pos, &oldL, &oldR);
	writeLoop(pos, oldL + (dryL - oldL) * dryAmt, oldR + (dryR - oldR) * dryAmt);
}

void Sklitter::seatCapture(const float* left, const float* right, size_t n) noexcept {
	const float db = target_.inputGainDb;
	const float gain = db == 0.0f ? 1.0f : static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
	const float from = captureGainPrimed_ ? captureGain_ : gain;
	captureGain_ = gain;
	captureGainPrimed_ = true;
	if (from == 1.0f && gain == 1.0f) {
		captureL_ = left;
		captureR_ = right;
		return;
	}
	// RAMPED ONLY WHEN IT MOVED: a held gain multiplies every sample by the same float, which is what makes the
	// recording exactly `input x 10^(dB/20)` frame for frame.
	const float step = n > 0 ? (gain - from) / static_cast<float>(n) : 0.0f;
	for (size_t i = 0; i < n && i < captureScratchL_.size(); ++i) {
		const float g = from == gain ? gain : from + step * static_cast<float>(i + 1);
		captureScratchL_[i] = left[i] * g;
		captureScratchR_[i] = right[i] * g;
	}
	captureL_ = captureScratchL_.data();
	captureR_ = captureScratchR_.data();
}

void Sklitter::resetRingUnlessFrozen() noexcept {
	// **A ZEROED RING IS THE LARGEST WRITE THERE IS**, so a frozen device skips it: the pre-warm's restart on a new
	// bar or buffer length, and a cold press, leave the buffer as it stood. Their counters still restart, so
	// unfreezing resumes the recording exactly where an unfrozen device would be. The host's `reset` is not
	// routed here — it clears every buffer, frozen or not.
	// **AND NOT WHILE THE RECORD GATE IS SHUT (backlog MJ)** — a zeroed ring is silence recorded over the buffer, the
	// one thing the gate exists to stop.
	if (!target_.freeze && !recordGateShut()) {
		ring_.reset();
	}
}

void Sklitter::seatFreeze(const float* left, const float* right, size_t n) noexcept {
	// **THE FROZEN LOOP IS NOT SAVED WITH THE PATCH, AND WHY** (backlog NM): it is audio — 384 KB a second at 48 kHz —
	// and audio is not persisted unless it is added later (2026-10-18). A reload with Freeze on holds the write gain at 0
	// over an empty ring: frozen silence until Freeze is let go.
	// backlog GT: a LOCKED STOP is a freeze for as long as it holds — the writers ramp out at the stop and back in
	// at the resume by this same road.
	const float held = (target_.freeze || parked_) ? 0.0f : 1.0f;
	// **THE RECORD GATE — backlog MJ.** Off (a threshold of exactly 0), every sample's target is `held`, which is
	// the whole of what this function did before it, so an ungated device is bit-identical by construction.
	const float gate = recordGateThreshold(target_.recordGate);
	const auto targetAt = [&](size_t i) noexcept {
		if (gate <= 0.0f) {
			return held;
		}
		// THE LARGER EAR, AS IT ARRIVES — before Input Gain, so turning the recording up does not also lower the gate.
		const float level = std::max(std::fabs(left[i]), std::fabs(right[i]));
		if (level >= gate) {
			recordGateHoldLeft_ = recordGateHoldFrames_;
			recordGateOpen_ = true;
		}
		else if (recordGateHoldLeft_ > 0) {
			// THE HOLD: open for `recordGateHoldFrames_` samples after the last one that reached the threshold, then
			// shut — and the ramp below takes the writers out over the seam's 5 ms from there.
			--recordGateHoldLeft_;
			recordGateOpen_ = true;
		}
		else {
			recordGateOpen_ = false;
		}
		return recordGateOpen_ ? held : 0.0f;
	};
	const float step = wrapFade_ > 0 ? 1.0f / static_cast<float>(wrapFade_) : 1.0f;
	const size_t m = std::min(n, freezeGain_.size());
	float g = freezeWriteGain_;
	if (!freezePrimed_) {
		g = (gate <= 0.0f || recordGateOpen_) ? held : 0.0f; // unprimed: where the gate stands, as KX starts frozen
	}
	freezePrimed_ = true;
	for (size_t i = 0; i < m; ++i) {
		const float target = targetAt(i);
		// TOWARD THE TARGET AND CLAMPED ONTO IT, so a settled gain is exactly 1 or exactly 0 — the two values
		// `Ring::store` treats as "store it" and "store nothing" rather than as a blend.
		g = g < target ? std::min(target, g + step) : std::max(target, g - step);
		freezeGain_[i] = g;
	}
	freezeWriteGain_ = g;
	ring_.setWriteGain(g);
}

void Sklitter::recordRingPWrite(int i, float srcL, float srcR, float mix) noexcept {
	const auto k = static_cast<size_t>(i);
	if (k >= rwWhat_.size()) {
		return;
	}
	rwPwL_[k] = srcL;
	rwPwR_[k] = srcR;
	rwPwMix_[k] = mix;
	rwWhat_[k] |= kHasPWrite;
}

/// **THE GENTLE FIXED DAMPING — a lowpass and a DC blocker, in that order.** By design damping exists
/// and the lane stays in the loop; these two are what makes an EMPTY lane usable, and
/// `kFeedbackDampHz`/`kFeedbackDcHz` carry why each is here.
///
/// **THE STATE IS PER DEVICE AND NOT PER BLOCK**, which is the whole of what makes them filters.
void Sklitter::dampFeedback(float inL, float inR, float* outL, float* outR) noexcept {
	// **THE CEILING IS APPLIED BEFORE THE FILTERS, NOT AFTER**, so a diverging lane cannot push an
	// unbounded number through the one-poles' state and leave them holding it after the clamp would
	// have caught the output. See `kFeedbackCeiling`.
	inL = std::clamp(inL, -kFeedbackCeiling, kFeedbackCeiling);
	inR = std::clamp(inR, -kFeedbackCeiling, kFeedbackCeiling);
	dampLpL_ += (inL - dampLpL_) * dampA_;
	dampLpR_ += (inR - dampLpR_) * dampA_;
	const float yL = dampLpL_ - dcX1L_ + dcR_ * dcY1L_;
	const float yR = dampLpR_ - dcX1R_ + dcR_ * dcY1R_;
	dcX1L_ = dampLpL_;
	dcX1R_ = dampLpR_;
	dcY1L_ = yL;
	dcY1R_ = yR;
	// **AND THE OUTPUT IS CLAMPED TOO, WHICH IS NOT BELT AND BRACES — IT IS A MEASUREMENT.** The DC
	// blocker `(1 − z⁻¹)/(1 − R z⁻¹)` has an impulse response whose absolute sum is about 2, so a
	// source clamped to ±2 can leave it at ±4. MEASURED, four passes through a x4 master lane on the
	// `Post FX` tap: the ring peaked at **3.51** with only the input clamped. Clamping the input
	// bounds the STATE; clamping the output is what bounds the number that reaches the ring, and the
	// ceiling is a claim about the ring.
	*outL = std::clamp(yL, -kFeedbackCeiling, kFeedbackCeiling);
	*outR = std::clamp(yR, -kFeedbackCeiling, kFeedbackCeiling);
}

/// **DRY, THEN FEEDBACK, THEN `P Write` — the order, and the order is audible rather than
/// bookkeeping.** Because the blends compose, what survives of the buffer's history after all three
/// is `(1 − a_dry)(1 − a_fb)(1 − a_pw)`, and a writer later in the chain is attenuated less by the
/// ones after it: `P Write` dominates and DRY is the most written over.
///
/// **THE `old` IS RE-READ BETWEEN EACH BLEND, WHICH IS WHAT MAKES THEM SEQUENTIAL RATHER THAN
/// PARALLEL.** Reading it once and blending three sources into that snapshot would be a three-way
/// mix landing in one write — a different instrument, and not the one designed. Each blend sees what
/// the one before it left.
void Sklitter::flushRingWrites(const float* fbL, const float* fbR, size_t n) noexcept {
	const float dryAmt = std::clamp(target_.dryWrite, 0.0f, 1.0f);
	const float fbAmt = std::clamp(target_.feedback, 0.0f, 1.0f);
	lastDryWrites_ = 0;
	lastFbWrites_ = 0;
	for (size_t k = 0; k < n && k < rwWhat_.size(); ++k) {
		const uint8_t what = rwWhat_[k];
		if (what == 0u) {
			continue;
		}
		const size_t pos = rwPos_[k];
		lastWriteHead_ = pos;
		float oldL = 0.0f;
		float oldR = 0.0f;
		// BACKLOG KX: the flush writes for sample `k`, so it writes at sample `k`'s freeze gain — both blends below.
		ring_.setWriteGain(freezeGainAt(k));

		// 1. DRY — the fresh input. GU's continuous in-phase write, arriving as a control. ALREADY
		// WRITTEN, in the sample, by `writeDryNow`; counted here so the seam still reports the chunk.
		if ((what & kHeadLive) != 0u && dryAmt > 0.0f) {
			++lastDryWrites_;
		}

		// 2. FEEDBACK — the WET, post-effects, damped. **THE DAMPER RUNS ON EVERY LIVE SAMPLE AND NOT
		// ONLY ON THE ONES THAT WRITE**, because a one-pole whose input is decimated is a different
		// filter: its corner would then depend on how much of the block the looper happened to play.
		if ((what & kHeadLive) != 0u && fbAmt > 0.0f) {
			float dL = 0.0f;
			float dR = 0.0f;
			dampFeedback(fbL[k], fbR[k], &dL, &dR);
			lastFbTapL_ = fbL[k];
			lastFbDampedL_ = dL;
			ring_.readAt(pos, &oldL, &oldR);
			writeLoop(pos, oldL + (dL - oldL) * fbAmt, oldR + (dR - oldR) * fbAmt);
			++lastFbWrites_;
		}

		// 3. `P Write` — the pre-envelope grain, untouched by any of this. pwrite is already
		// as much feedback as we would need; it is LAST, so it is the least written over.
		if ((what & kHasPWrite) != 0u) {
			ring_.readAt(pos, &oldL, &oldR);
			const float mix = rwPwMix_[k];
			lastPwSrcL_ = rwPwL_[k];
			lastPwMix_ = mix;
			writeLoop(pos, oldL + (rwPwL_[k] - oldL) * mix, oldR + (rwPwR_[k] - oldR) * mix);
		}
		else {
			// **ZEROED RATHER THAN LEFT, so a case reading this after a sample that did not write is
			// told "nothing", not the last sample that did.** A stale reading is the shape that makes
			// a value assertion pass for the wrong reason.
			lastPwMix_ = 0.0f;
		}
	}
}

void Sklitter::renderChunk(float* left, float* right, int frames) {
	const auto n = static_cast<size_t>(frames);
	if (wetL_.size() < n || mixCoeff_.size() < n) {
		return; // not prepared; rendering into a short buffer is the one thing worse than silence
	}
	// **SAMPLED ONCE PER CHUNK, BEFORE ANYTHING RENDERS.** A control that moved mid-chunk would
	// otherwise leave the first half of the block recorded and the second half already written, and
	// the flush would then write the first half a second time.
	writersLive_ = target_.writersActive();
	if (writersLive_) {
		std::fill_n(rwWhat_.begin(), n, static_cast<uint8_t>(0u));
	}
	deferCount_ = 0;
	if (auditReads_) {
		auditLo_ = linearBarPos_;
		std::fill(auditWritten_.begin(), auditWritten_.end(), 0u);
	}
	std::fill_n(wetL_.begin(), n, 0.0f);
	std::fill_n(wetR_.begin(), n, 0.0f);
	std::fill_n(mixCoeff_.begin(), n, 0.0f);
	std::fill_n(sendL_.begin(), n, 0.0f);
	std::fill_n(sendR_.begin(), n, 0.0f);
	seatCapture(left, right, n);
	seatFreeze(left, right, n);

	renderStereo(left, right, frames);

	// **THE LOCKED STOP'S WET-OUT — backlog GT.** Applied to the coefficient before anything reads it, so the
	// sparks' levels and the blend agree, in every mode at once. The same slope the write gain falls on in
	// `seatFreeze`, reaching 0 on the ramp's last frame.
	if (parkLeft_ > 0) {
		const size_t m = std::min(n, parkLeft_);
		for (size_t i = 0; i < m; ++i) {
			mixCoeff_[i] *= static_cast<float>(parkLeft_ - i - 1) / static_cast<float>(wrapFade_);
		}
		std::fill(mixCoeff_.begin() + static_cast<std::ptrdiff_t>(m), mixCoeff_.begin() + static_cast<std::ptrdiff_t>(n),
		          0.0f);
		parkLeft_ -= m;
	}

	// **THE GRAIN LANE, ON THE GRAINS THAT WON THEIR DRAW — AND ITS OUTPUT REJOINS THE WET BEFORE THE
	// MASTER SEES IT.** before master lane. The consequence is worth stating
	// where it happens rather than leaving to be inferred: **the master lane's input — and therefore
	// its DRY path — includes whatever this lane produced.** A grain that took the detour is processed
	// by the master like everything else, because a lane that rejoins upstream is simply part of what
	// the downstream lane is handed.
	//
	// **AND IT IS A DIVERSION RATHER THAN A PARALLEL SEND, BY DESIGN. THIS PARAGRAPH USED TO SAY THE
	// OPPOSITE AND WAS WRONG.**
	//
	// **BACKLOG EW POINTS THE OTHER WAY, AND THAT IS WHY THIS IS WRITTEN OUT RATHER THAN ASSUMED.** The
	// entry says the per-slice decision picks which grains go into the lane *"exactly as `shouldDelay`
	// picks which go to the internal delay"* — and the internal delay IS a parallel send: the grain
	// stays in the wet and the echo is added on top. So a reader with the live document in one hand and
	// this code in the other finds them disagreeing.
	//
	// **THE SAME MECHANISM PRODUCES A DIFFERENT RESULT HERE, AND THAT IS THE WHOLE OF IT.** The echo
	// returns through a DELAY LINE — a quieter, later copy, which is what makes adding it musical. An
	// inner CHAIN at unity returns what it was given, unchanged and in time. So a grain that stayed in
	// the wet AND came back from the lane would be heard twice: **merely attaching an EMPTY lane would
	// make every chosen grain 6 dB louder**, which is the master lane's own bit-for-bit promise failing
	// in the first place a player would notice.
	//
	// So a chosen grain LEAVES the wet (see the two divert sites in `playLooper` and `playGrain`) and
	// what is added here is the lane's output in its place. `tests/test_sklitter_chain.cpp`'s *"a grain
	// lane holding NOTHING is inaudible however many grains are diverted into it"* is the guard on that
	// decision, and known-bad 6 — building this as a parallel send — is what proves the guard works.
	// **Do not "fix" this back to EW's wording without re-reading that case.**
	//
	// **AND THE SPARKS' TWO LEVELS ARE TAKEN HERE, IN THE ONE INSTANT THE PARTITION EXISTS** — backlog
	// HR, and it is the diversion above that makes it a partition rather than an overlap. For the
	// length of the loop below, `wetL_` is everything that did NOT take the lane and `sendL_` is the
	// lane's OUTPUT, which is everything that did. One line later they are the same buffer.
	//
	// **SO "WET MINUS GRAIN FX" IS THE WET READ ONE LINE EARLY, NOT A SUBTRACTION.** The grain fx is
	// taken out of the master before the sparks; doing it by arithmetic after the fact would leak a
	// float's worth of error into a partition whose entire value is that it does not. `Reading::WetLevel`
	// carries the rest of the argument, including what this deliberately does not account for.
	float wetPeak = 0.0f;
	float grainPeak = 0.0f;
	if (grainChain_ != nullptr) {
		for (size_t g = 0; g < kPerformOutputs; ++g) {
			grainChain_->publishCoreSource(g, performOutput(g));
		}
		grainChain_->process(sendL_.data(), sendR_.data(), frames);
		for (size_t i = 0; i < n; ++i) {
			// `mixCoeff_` is clamped to 0..1 by `mixAt`, so scaling a magnitude by it is a magnitude.
			const float m = mixCoeff_[i];
			wetPeak = std::max(wetPeak, std::max(std::fabs(wetL_[i]), std::fabs(wetR_[i])) * m);
			grainPeak = std::max(grainPeak, std::max(std::fabs(sendL_[i]), std::fabs(sendR_[i])) * m);
			wetL_[i] += sendL_[i];
			wetR_[i] += sendR_[i];
		}
	}
	else {
		// **NO LANE MEANS NO SECOND COLOUR, AND THE DIVERTED GRAINS ARE GONE RATHER THAN QUIET.**
		// `grainChain_` is built in the constructor and this branch is defensive, but it must not
		// silently start adding `sendL_` back: without a lane those samples are the RAW diverted
		// grains, and folding them in would make an absent chain audible.
		for (size_t i = 0; i < n; ++i) {
			const float m = mixCoeff_[i];
			wetPeak = std::max(wetPeak, std::max(std::fabs(wetL_[i]), std::fabs(wetR_[i])) * m);
		}
	}
	foldOutLevels(wetPeak, grainPeak, n);

	// **BACKLOG GV: THE `Grain FX` TAP IS TAKEN HERE, AND IT IS A COPY FOR ONE REASON** — `runPostChain`
	// rewrites the wet IN PLACE, so after the next line there is no post-grain-lane signal left to
	// read. The `Post FX` tap costs nothing and takes no copy: it IS `wetL_`/`wetR_` at the flush.
	//
	// **WHICH OF THE TWO IS THE ONE THING STILL UNDECIDED** (post grain fx or post post fx), which
	// is why this is a switch that can be flipped while it plays
	// rather than a constant somebody chose in advance.
	if (writersLive_ && target_.feedback > 0.0f && !target_.feedbackPostMaster) {
		std::copy_n(wetL_.begin(), n, fbTapL_.begin());
		std::copy_n(wetR_.begin(), n, fbTapR_.begin());
	}

	// THE MASTER LANE, ON THE WET ALONE. Reached even when `Enabled` is down — and what it is handed
	// then is SILENCE, because a bypassed device writes no wet., asked for exactly
	// that: a reverb inside decays as a tail should instead of reverberating the untouched dry.
	runPostChain(wetL_.data(), wetR_.data(), frames);

	// **BACKLOG GU/GV: FEEDBACK AND `P Write` LAND HERE, after both lanes and before the output blend** —
	// after the lanes because that is the earliest point at which post-lane material exists at all. The
	// DRY blend, first of the three, already landed in the sample (`writeDryNow`, backlog JC): its source
	// is the host's input, which was never late.
	if (writersLive_) {
		const float* fbL = target_.feedbackPostMaster ? wetL_.data() : fbTapL_.data();
		const float* fbR = target_.feedbackPostMaster ? wetR_.data() : fbTapR_.data();
		flushRingWrites(fbL, fbR, n);
	}

	// **BACKLOG HN: THE DEFERRED WRITES LAND HERE, after both lanes** — which is the earliest point at
	// which a post-lane source would exist, and therefore the point the real feature would use.
	flushDeferredWrites();

	// **AND THE BLEND, LAST.** `dry + (wet − dry)·m`, the same arithmetic in the same order `blendAt`
	// ended with — which is what keeps a device whose chain holds nothing bit for bit the device with
	// no chain at all, since an empty chain is a true pass and `m` is unchanged by any of this.
	//
	// **THE SPARKS' LEVELS ARE NOT TAKEN HERE, AND THAT MOVED DELIBERATELY** (backlog HR). They were,
	// while there was one of them; two that PARTITION can only be read where the two roads meet, which
	// is at the grain lane's rejoin above. By this line the lane is inside the wet and the split is
	// gone.
	tripwireWet(left, n); // `left` is still the host's dry here; the wet is `wetL_`, lanes included
	for (size_t i = 0; i < n; ++i) {
		const float m = mixCoeff_[i];
		// **`m == 0` LEAVES THE SAMPLE ALONE RATHER THAN BLENDING IT WITH ZERO, AND THE SIGN OF A ZERO
		// IS THE WHOLE REASON.** `dry + (wet − dry)·0` is `dry + 0.0f`, which is `+0.0f` when `dry` is
		// `−0.0f` — so the arithmetically-identical form would flip the sign of every negative zero in
		// the regions no renderer wrote, for a device doing nothing. Before the wet moved into a buffer
		// these samples were never assigned at all, and this branch is what preserves that.
		// `rack::InnerChain`'s empty-chain fast path is the same guard for the same reason.
		//
		// **THE FIRST DRAFT OF THIS LINE WAS THE `+=` FORM, WRITTEN MINUTES AFTER I HAD WARNED SOMEBODY
		// ELSE ABOUT EXACTLY THIS.** Recorded because of who made it rather than despite it: knowing a
		// trap is not protection against it, and the only thing that was ever going to catch this is
		// the case below that reads the sign bit. `-0.0f == +0.0f` is true in IEEE, so no equality
		// check anywhere could have found it either.
		if (m == 0.0f) {
			continue;
		}
		left[i] += (wetL_[i] - left[i]) * m;
		right[i] += (wetR_[i] - right[i]) * m;
	}
}

#ifdef FEATHERS_DIAGNOSTIC
namespace {

/// One line to stderr — which is Bitwig's `~/.BitwigStudio/log/engine.log` for a plugin. Formatted
/// into a stack buffer and handed to `write(2)` whole: no allocation, no stdio lock, one syscall.
/// Called only on a CHANGE (never per sample), and each device has a line budget.
[[gnu::format(printf, 2, 3)]] void tripLine(Tripwire& t, const char* fmt, ...) noexcept {
	if (t.linesLeft <= 0) {
		return;
	}
	--t.linesLeft;
	char buf[768];
	va_list args;
	va_start(args, fmt);
	const int len = std::vsnprintf(buf, sizeof buf, fmt, args);
	va_end(args);
	if (len <= 0) {
		return;
	}
	const auto n = std::min(static_cast<size_t>(len), sizeof buf - 1);
#if !defined(_WIN32)
	const ssize_t written = ::write(2, buf, n);
	(void)written;
#else
	std::fwrite(buf, 1, n, stderr);
#endif
}

long long tripMilliseconds() noexcept {
	struct timespec ts {};
	clock_gettime(CLOCK_REALTIME, &ts);
	return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

} // namespace

/// Everything the tripwire knows, on one line, tagged with why it was written.
#define FEATHERS_TRIP_LOG(t, why)                                                                        \
	tripLine((t),                                                                                        \
	         "[FEATHERS-SKLITTER] %s dev=%p sr=%.1f block=[%d,%d] bpm=%.4f playing=%d beats=%.6f "         \
	         "jumps=%llu mxcsr=0x%04x(rc=%u ftz=%u daz=%u) slice=%zu slices=%d ring=%zu status=%d "        \
	         "mode=%d engage=%d enabled=%d wet[moving=%zu held=%zu nonfinite=%zu] trips=%llu at %lld\n",   \
	         (why), static_cast<const void*>(this), (t).last.sampleRate, (t).minBlock, (t).maxBlock,      \
	         (t).last.secondsPerBeat > 0.0 ? 60.0 / (t).last.secondsPerBeat : 0.0, (t).last.playing ? 1 : 0, \
	         clock_.songPositionBeats, static_cast<unsigned long long>((t).jumps), (t).last.mxcsr,          \
	         ((t).last.mxcsr >> 13) & 3u, ((t).last.mxcsr >> 15) & 1u, ((t).last.mxcsr >> 6) & 1u,        \
	         (t).last.sliceFrames, (t).last.numSlices, (t).last.bufferFrames, (t).last.status,             \
	         (t).last.mode, (t).last.engage ? 1 : 0, (t).last.enabled ? 1 : 0, (t).window.moving,         \
	         (t).window.held, (t).window.nonFinite, static_cast<unsigned long long>((t).trips),            \
	         tripMilliseconds())

void Sklitter::tripwireBlock(int frames) noexcept {
	Tripwire& t = tripwire_;
	TripSnapshot now;
	now.sampleRate = sampleRate_;
	now.secondsPerBeat = clock_.secondsPerBeat;
	now.playing = clock_.playing;
#ifdef FEATHERS_TRIPWIRE_MXCSR
	now.mxcsr = _mm_getcsr() & 0xFFC0u; // the control bits; the sticky exception flags would churn
#endif
	now.sliceFrames = sliceFrames_;
	now.numSlices = numSlices_;
	now.bufferFrames = bufferFrames_;
	now.status = static_cast<int>(engage_.status());
	now.mode = static_cast<int>(target_.mode);
	now.engage = target_.engage;
	now.enabled = target_.enabled;

	bool changed = !t.haveLast || !(now == t.last);
	if (frames < t.minBlock || frames > t.maxBlock) {
		t.minBlock = std::min(t.minBlock, frames);
		t.maxBlock = std::max(t.maxBlock, frames);
		changed = true;
	}
	// **THE SONG POSITION AGAINST THE FRAMES THAT ELAPSED** — a loop, a locate, or a host feeding a
	// position that does not advance with the audio. Logged with where it was expected to be.
	const double framesPerBeat = now.secondsPerBeat * now.sampleRate;
	if (now.playing && clock_.havePositionBeats() && framesPerBeat > 0.0) {
		if (std::isfinite(t.expectBeats) && std::abs(clock_.songPositionBeats - t.expectBeats) > 1e-3) {
			++t.jumps;
			tripLine(t, "[FEATHERS-SKLITTER] jump dev=%p expected=%.6f got=%.6f at %lld\n",
			         static_cast<const void*>(this), t.expectBeats, clock_.songPositionBeats, tripMilliseconds());
		}
		t.expectBeats = clock_.songPositionBeats + static_cast<double>(frames) / framesPerBeat;
	}
	else {
		t.expectBeats = std::numeric_limits<double>::quiet_NaN();
	}
	if (changed) {
		t.last = now;
		t.haveLast = true;
		FEATHERS_TRIP_LOG(t, "state");
	}
}

void Sklitter::tripwireWet(const float* dry, size_t n) noexcept {
	Tripwire& t = tripwire_;
	const WetScan s = scanWet(dry, wetL_.data(), n, t.prevIn, t.prevWet);
	t.window.moving += s.moving;
	t.window.held += s.held;
	t.window.nonFinite += s.nonFinite;
	t.windowFrames += n;
	// ABOUT A SECOND A WINDOW: long enough that a clean window's held count is its true zero, short
	// enough that the trip lands within a second of the sound going wrong.
	if (static_cast<double>(t.windowFrames) < sampleRate_) {
		return;
	}
	const bool now = wetTripped(t.window);
	if (now && !t.tripped) {
		++t.trips;
		FEATHERS_TRIP_LOG(t, "TRIP");
	}
	else if (!now && t.tripped) {
		FEATHERS_TRIP_LOG(t, "clear");
	}
	t.tripped = now;
	t.window = {};
	t.windowFrames = 0;
}

void Sklitter::tripwireReseat(long long drift, int frames) noexcept {
	tripLine(tripwire_,
	         "[FEATHERS-SKLITTER] jump reseat dev=%p drift=%lld frames call=%d beats=%.6f reseats=%zu at %lld\n",
	         static_cast<const void*>(this), drift, frames, clock_.songPositionBeats, prewarmReseats_,
	         tripMilliseconds());
}

#undef FEATHERS_TRIP_LOG
#else
void Sklitter::tripwireBlock(int) noexcept {}
void Sklitter::tripwireWet(const float*, size_t) noexcept {}
void Sklitter::tripwireReseat(long long, int) noexcept {}
#endif

void Sklitter::foldOutLevels(float wetPeak, float grainPeak, size_t frames) noexcept {
	// **A NON-FINITE SAMPLE READS AS SILENCE RATHER THAN AS A PEAK.** `GuiBridge` scrubs non-finites at
	// the publish already, but the UI reads these in tests without going through it, and a NaN that
	// reached an emission rate would make the spark count NaN and the pad's own arithmetic undefined —
	// a picture is a worse place to discover a NaN than a meter is.
	const auto clean = [](float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; };
	wetPeak = clean(wetPeak);
	grainPeak = clean(grainPeak);

	// **BOTH RISE INSTANTLY, AND BOTH FALL ONLY AT THE SAME WINDOW BOUNDARY.** The rise is what makes a
	// single-sample transient visible to a surface that draws three frames later; the shared window is
	// what makes the two numbers a RATIO rather than two readings from different moments (see the
	// members). No smoothing and no decay: the ballistics belong to whatever draws it, and
	// `busutil_meter.h` argues the same split for the same reading.
	wetLevelPeak_ = std::max(wetLevelPeak_, wetPeak);
	grainLevelPeak_ = std::max(grainLevelPeak_, grainPeak);
	wetLevelCandidate_ = std::max(wetLevelCandidate_, wetPeak);
	grainLevelCandidate_ = std::max(grainLevelCandidate_, grainPeak);

	outLevelHeld_ += frames;
	const auto hold = static_cast<size_t>(std::max(1.0, sampleRate_ * kOutLevelHoldSeconds));
	if (outLevelHeld_ >= hold) {
		// **THE TURNOVER TAKES BOTH OR NEITHER.** A window that retired one level and not the other
		// would hand the surface a fresh number beside a stale one, which is the ratio being wrong in
		// exactly the frames where the material moved.
		wetLevelPeak_ = wetLevelCandidate_;
		grainLevelPeak_ = grainLevelCandidate_;
		wetLevelCandidate_ = 0.0f;
		grainLevelCandidate_ = 0.0f;
		outLevelHeld_ = 0;
	}
}

bool Sklitter::holdsTail() const noexcept {
	if (!target_.enabled) {
		return false;
	}
	// ENGAGED, THE CAPTURE PLAYS ON WITH NO INPUT — but only what is in it: a loop that pWrite has filled with silence
	// plays silence for ever, and holds nothing. The echo returns a quarter bar later while it is heard. PARKED on a
	// locked stop (backlog GT) it plays nothing until the transport moves, and a host restarting it wakes the plugin.
	const bool engaged = engage_.status() != Status::Off && !parked_;
	if (engaged && (ring_.holdsSound() || (echoGain_ > 0.0f && delay_.holdsSound()))) {
		return true;
	}
	return (postChain_ != nullptr && postChain_->holdsTail()) || (grainChain_ != nullptr && grainChain_->holdsTail());
}

void Sklitter::renderStereo(float* left, float* right, int frames) {
	// THE SCALARS FOR ANY READ BEFORE A LOOP. **The window POINTERS are taken in `processStereo` now**,
	// because this function is called once per chunk and they belong to the whole call.
	mixNow_ = target_.mix;
	widthNow_ = target_.stereoWidth;
	if (frames <= 0 || ring_.capacity() == 0) {
		return;
	}

	// BYPASS IS NOT THE GESTURE. `Enabled` off silences the device's effect entirely and leaves the
	// signal alone; it does not merely release the stutter.
	if (!target_.enabled) {
		engage_.reset();
		return;
	}

	// **THE STOP IS SEEN IN BOTH SETTINGS; ONLY THE FREEZE IS GATED — backlog GT/HE.** The design
	// came in two halves and the second collapsed the first: the device can be both free-running and
	// transport-locked, resetting to the proper phase when the transport restarts; and with the lock off
	// the effect slips past a stopped transport and rejoins when it resumes, which is a feature in the
	// right mode. So the control gates **whether audio keeps being produced while
	// the transport is parked, and nothing else** — the REJOIN belongs to both settings, which is why
	// the debt is recorded above the gate and the `return` below it.
	//
	// **THE RETURN IS A RETURN RATHER THAN A RESET.** `reset()` is the opposite of frozen: its own
	// comment says a reset is *"the host saying nothing is playing"* and it drops the inner chain's
	// tails. So the locked device simply does nothing for the duration — no wet is written, so the
	// blend leaves the dry bit for bit (see `renderChunk`'s `m == 0` branch).
	//
	// **ABOVE `setEngaged`, NOT BELOW IT, AND THAT IS THE FREEZE.** The gesture's own branch below
	// re-lengths and may RESET the ring; the pre-warm below that RECORDS into it. Both are writes to
	// the buffer, and both are reached by a device that is supposed to be holding still. Stopping
	// before the engage machine also means a press made while the host is parked is seen on the
	// restart instead of half-applied to a device that cannot act on it.
	//
	// **AND THE DEBT IS ARMED WHILE STOPPED AND PAID WHILE PLAYING, WHICH IS NOT THE SAME AS "PAID
	// WHENEVER SET".** Under the lock the difference is invisible — the `return` means `playLooper`
	// never runs while parked. UNLOCKED it is the whole of the free run: a host that publishes a
	// playhead while stopped (Bitwig does) would otherwise have every block seek to the SAME song
	// position, pinning the cursor and turning free-running stutter over a parked DAW into a
	// device that stands still. `playLooper` consumes it only when `clock_.playing`.
	//
	// **AND NOT UNTIL THE STOP'S RAMP HAS RUN (backlog GT).** The first 5 ms of a locked stop render as
	// usual while `renderChunk` takes the wet to the dry and `seatFreeze` takes the writers to 0; see
	// `parked_`. `processStereo` cut the chunk there, so this returns from the next one.
	if (!clock_.playing) {
		rejoinPending_ = true;
		if (transportLockActive() && parkLeft_ == 0) {
			return;
		}
	}

	const Status before = engage_.status();
	engage_.setEngaged(target_.engage, target_);
	if (engage_.status() != before && engage_.status() != Status::Off) {
		// A FRESH GESTURE. What gets captured is resolved ONCE, at the press, from the rate and the
		// tempo as they were then, not re-evaluated per block.
		//
		// **BURST CAPTURES A DIVISION; A LOOPER MODE CAPTURES A BAR.** That is the same knob asking
		// two different questions, and it is why `Rate` means a LENGTH in Burst and a slice COUNT
		// in the looper modes. Repeat is the exception on both counts: it cuts the bar by a
		// continuous curve rather than a ladder, so it has one slice of a computed length.
		if (engage_.armed().isLooperMode()) {
			sliceFrames_ = resolveBarFrames();
			// QUANTIZE IS BURST'S ONLY: a looper mode never asks `resolveSliceFrames` anything, so
			// leaving the snap disarmed keeps the two facts one.
			quantizedGesture_ = false;
			// A LOOPER MODE READS AT ONE. Its bar is not a division of the Rate knob — Rate buys it
			// SLICES there, live, in `advanceSlice` — so there is no ratio to state and the rate
			// step must not be computed against a bar it did not set.
			capturedSliceFrames_ = 0;
			if (engage_.armed().mode == Mode::Repeat) {
				numSlices_ = static_cast<int32_t>(
				    std::max<size_t>(1, sliceFrames_ / repeatSliceLength(target_.rate, sliceFrames_)));
			}
			else {
				numSlices_ = sliceCountFor(engage_.armed().mode, target_.rate);
			}
			if (engage_.armed().mode == Mode::Grain) {
				armGrain(engage_.armed().zoneA);
			}
			armedMode_ = engage_.armed().mode;
		}
		else {
			// **THE SNAP HAPPENS HERE AND NOWHERE ELSE**, and it has to precede the resolve below:
			// the captured length is the snapped rate's.
			quantizedGesture_ = engage_.armed().quantized;
			rateAtPress_ = engage_.armed().rate;
			quantizedBase_ = quantizedGesture_
			                     ? snapToDivision(std::clamp(static_cast<double>(rateAtPress_), 0.0, 1.0))
			                     : 0.0;
			sliceFrames_ = resolveSliceFrames();
			capturedSliceFrames_ = sliceFrames_;
			numSlices_ = 1;
			// THE SLICE IS THE RING'S START — a press resets the ring below and records from frame 0.
			sliceOrigin_ = 0;
			// AND THE MODE IT WAS PRESSED IN IS ARMED, so the dispatch below does not take the first playing
			// block for a switch into it and re-arm over what the press just captured.
			armedMode_ = engage_.armed().mode;
		}
		// **KEEP THE PRE-WARM RATHER THAN CLEARING IT.** A looper that has been listening already holds
		// a beat-aligned bar; resetting here would throw it away and make the gesture record another,
		// which is exactly the wait this exists to remove. The capture count is handed the frames the
		// ring already has, so `EngageMachine::advance` finds `haveAudio` true at once and triggers on
		// the next boundary.
		// **THE RING HOLDS THE BUFFER, NOT THE BAR** — one number until `Param::BufferLength`; see
		// `bufferFrames_`. A looper mode takes its buffer from the setting; Burst captures one slice
		// and `resolveBufferFrames` hands back the bar for it rather than scaling it.
		bufferFrames_ = engage_.armed().isLooperMode() ? resolveBufferFrames() : sliceFrames_;
		capturedBufferBars_ = target_.bufferBars;
		// **AND WHAT IT HAS SO FAR, NOT ONLY A WHOLE BUFFER (2026-09-24).** This was all or nothing: a press
		// one frame short of warm reset the ring and recorded the whole buffer again — and so did every
		// re-press during a capture, since the capture count starts at zero. The log: after Burst
		// re-lengthed the ring, a Pattern press at 7.65 (80 BPM, two-bar buffer) needed until 16; a
		// release at 14.47 and a re-press at 15.87 threw 6.8 beats away and moved the start to 24, and a
		// hand toggling every beat or two never got there at all. The pre-warm's frames are the song's own
		// phase (seated every block), which is exactly what a capture writes, so they count as captured.
		const size_t held =
		    engage_.armed().isLooperMode() && prewarming_ && ring_.length() == bufferFrames_
		        ? std::min(prewarmed_, bufferFrames_)
		        : 0;
		ring_.setLength(bufferFrames_);
		if (held == 0) {
			resetRingUnlessFrozen();
		}
		else {
			engage_.captured(held);
		}
		sliceIndex_ = 0;
		// **REPEAT'S PLAN COUNTER STARTS EACH GESTURE AT 1**, as the reference's `triggerPlaybackNow` sets
		// `scatterRepeatLoopIndex = 1`. It was set only by `reset()`, so it carried on from the last gesture
		// and the arrangement a press played depended on how many repeats the previous one had run — which
		// no picture drawn from the controls could show (backlog IX).
		repeatLoopIndex_ = 1;
		// AND THE HOLD STARTS FRESH, as the reference's `triggerPlaybackNow` zeroes `scatterRepeatCounter`.
		holdNextSlice_ = -1;
		positionInSlice_ = 0;
		slotStartFrame_ = 0; // backlog IG: the bar's top, and the groove's anchor with it
		grooveAnchored_ = false;
		// **THE GESTURE IS A RESUME TOO, UNDER THE LOCK (backlog GT), AND LEAVING IT OUT WAS A REAL
		// GAP.** `sliceIndex_ = 0` above puts the cursor at the top of the captured bar whatever the
		// song is doing, so a press away from a downbeat plays the bar from its start while the music
		// is somewhere else. MEASURED before this line existed: the locked device and the free one both
		// began at slice 0 from a press at beat 1.5, and the locked one then JUMPED to the song's slice
		// at its first slot end — an arbitrary leap mid-gesture instead of a start in phase.
		//
		// the design reads on the press as directly as it reads on a restart: a gesture triggered on
		// the downbeat resumes with the same buffer as if it had been re-triggered on the downbeat. The seam is the same one, so it goes through the same flag.
		//
		// **AND IT IS NO LONGER GATED ON THE LOCK — by design, 2026-09-22 (backlog GU).** It was:
		// the press was read as free run's to leave alone, `sliceIndex_ = 0` above being its answer.
		// Then GU phased the WRITE head on every press (with Transport Locked off, every press starts
		// recording at the song's phase), and an unlocked read cursor left at slice 0 put
		// the two heads a song phase apart — MEASURED on the feedback suite's fixture, a 256-frame gap
		// that turned 288,360 of 582,720 reads into reads of what had just been written and slowed the
		// loop's six-pass decay from 0.350 to 0.724. The decision: both heads start at the song's phase
		// on every press, as under the lock. What still separates the settings is GT's: unlocked slips
		// past a stopped transport and rejoins when it resumes. **ONE ROAD**: the press owes the same
		// seek a resume does, so `seekToTransport` places both heads from one derivation.
		rejoinPending_ = engage_.armed().isLooperMode();
		schedule_ = SliceSchedule {};
		planCache_ = PlanCache {};
	}

	if (engage_.status() == Status::Off) {
		// **IDLE IS WHERE THE LOOPER FILLS ITS RING.** Dry either way — the host's buffer is already
		// the input and nothing is written to it — but the ring is listening, so the next press has a
		// bar to play instead of two seconds of silence to record one.
		if (target_.enabled && target_.isLooperMode()) {
			prewarm(captureL_, captureR_, frames);
		}
		return;
	}
	// ANYTHING BUT IDLE writes or reads the ring on its own terms (a capture seats itself once, below), so
	// the idle head it hands back is seated afresh on the next idle call rather than trusted.
	prewarmSeated_ = false;

	// RECORDING: the input goes into the ring and passes through untouched; Burst is inaudible until
	// its buffer is full.
	//
	// **THE THRESHOLD IS CROSSED AT A FRAME, NOT AT A BLOCK BOUNDARY, AND THAT DISTINCTION IS THE
	// WHOLE OF BLOCK-SIZE INDEPENDENCE.** The first draft recorded the whole block, then asked once
	// whether it was time to play -- so the same audio began stuttering at frame 1536 under 256-frame
	// blocks and at 8192 under one long call, which is the host changing the sound. The capture
	// threshold is counted per sample for exactly this reason.
	//
	// It is still not a per-frame call: within one block the transport does not move, so the only
	// thing that can change mid-block is the capture count. Record exactly up to the threshold, ask
	// once, and either start playing at that frame or record the remainder and ask again.
	int i = 0;
	if (engage_.status() != Status::Playing) {
		// **A LOOPER CAPTURE IS WRITTEN AT THE SONG'S PHASE TOO (backlog IN)** — the press seeks both
		// heads to the song (GU), so a capture appended from a reset ring's 0 would sit a press-phase
		// away from the cursor that reads it. Burst plays its slice from its own start.
		//
		// **ONCE, WHERE THE CAPTURE BEGINS, THEN APPENDED.** A warm press hands over a head the idle
		// road already seated (`captured(bufferFrames_)` above, so this is skipped). Seating every block
		// instead turned two cases red whose clock says "playing at beat 0" for ever: each block went
		// back over the same frames, and a capture should not be destroyed by a clock that stands still.
		if (engage_.armed().isLooperMode() && engage_.capturedFrames() == 0) {
			seatWriteHeadOnSong();
		}
		const size_t have = engage_.capturedFrames();
		// **THE CAPTURE IS DONE WHEN THE BUFFER IS FULL, NOT WHEN THE BAR IS.** These were one number
		// before `Param::BufferLength`; a four-bar buffer that started playing after one bar would read
		// three bars of whatever the ring held before the gesture.
		const size_t toThreshold = bufferFrames_ > have ? bufferFrames_ - have : 0;
		const int take = static_cast<int>(std::min<size_t>(static_cast<size_t>(frames), toThreshold));
		for (int k = 0; k < take; ++k, ++i) {
			ring_.setWriteGain(freezeGainAt(static_cast<size_t>(i))); // backlog KX: a frozen capture counts its frames and records nothing
			ring_.write(captureL_[i], captureR_[i]);
		}
		if (take > 0) {
			engage_.captured(static_cast<size_t>(take));
		}
		engage_.advance(clock_, bufferFrames_);

		if (engage_.status() != Status::Playing) {
			// Either the block ran out before the threshold, or the threshold is met and the machine
			// is waiting for a grid line. Both keep recording what is left of the block.
			const int rest = frames - i;
			for (; i < frames; ++i) {
				ring_.setWriteGain(freezeGainAt(static_cast<size_t>(i))); // backlog KX: a frozen capture counts its frames and records nothing
				ring_.write(captureL_[i], captureR_[i]);
			}
			if (rest > 0) {
				engage_.captured(static_cast<size_t>(rest));
				engage_.advance(clock_, bufferFrames_);
			}
			if (engage_.status() == Status::Off && engage_.armed().isLooperMode() && prewarmBeat_ != INT64_MIN
			    && ring_.length() == bufferFrames_) {
				// **A GESTURE RELEASED BEFORE ITS TRIGGER ENDS HERE, AND HANDS ITS CAPTURE TO THE PRE-WARM
				// (2026-09-24, decision B).** It recorded the song's phase into the buffer-long ring,
				// which is what the pre-warm writes, so the pre-warm carries on from it rather than starting
				// over, and the next press finds it warm. Only with an anchor: without one the pre-warm's
				// own first rolling block would discard it anyway (`prewarm`, backlog IN).
				prewarming_ = true;
				prewarmed_ = std::min(engage_.capturedFrames(), bufferFrames_);
			}
			if (engage_.status() != Status::Playing) {
				return;
			}
		}
		// **THE SEAM IS FADED ONCE, HERE, AND FOR THE LOOPER MODES ONLY.** See `Ring::fadeSeamAt`.
		// The reference does it at the end of its capture, on the buffer rather than on the output,
		// because the discontinuity is in the MATERIAL: the first frame and the last were recorded a
		// whole loop apart and meet every time round. Not for Burst: its slice can be ~1500 frames, and
		// the same 5 ms faded at each end of that is a tremolo, not a declick.
		// **AT THE APPEND HEAD, WHICH IS WHERE THE CAPTURE ENDED** — see `Ring::fadeSeamAt`: since backlog IN
		// index 0 is the song's downbeat, continuous material, and fading it there took the downbeat's attack.
		// **NOT WHILE FROZEN (backlog KX)**: a fade is a write, and a frozen capture recorded nothing to seam.
		// **NOR WHILE THE RECORD GATE IS ON AND SHUT (backlog MJ)** where the capture ended: the writers already crossfaded
		// into the old material there over the gate's 5 ms, so the head is not a seam, and fading it would write into the
		// buffer the gate is keeping. Ungated, the condition is exactly the one above it.
		const bool gateShutHere = recordGateThreshold(target_.recordGate) > 0.0f
		                         && freezeGainAt(static_cast<size_t>(std::max(0, i - 1))) <= 0.0f;
		if (engage_.armed().isLooperMode() && !target_.freeze && !gateShutHere) {
			ring_.fadeSeamAt(ring_.writePos(), wrapFade_);
		}

		// **BURST TAKES ITS GRAIN AT THE TRIGGER**, exactly here: half the captured slice, fixed for the
		// life of the gesture.
		if (engage_.armed().mode == Mode::Burst) {
			burstGrain_ = sliceFrames_ / 2;
			burstCyclePos_ = 0;
			burstReadPos_ = 0;
		}

		// THE OUTPUT CROSSFADES IN FROM THE DRY — see `engageFadePos_`.
		// LOOPER MODES ONLY: Burst plays captured frames back by value from the trigger, and its cases
		// pin that.
		engageFadePos_ = engage_.armed().isLooperMode() ? 0 : engageFadeFrames_;
		// **A TRIGGER OUTRANKS A DUCK IN FLIGHT.** The rejoin's wet-out exists to hide a seek under
		// continuing audio; a trigger has just restarted the gesture from the dry through the line
		// above, so the duck has nothing left to hide and `mixAt` would hold the device at the dry
		// waiting for a seek that the trigger has already made unnecessary.
		rejoinDucked_ = false;
		rejoinDuckLeft_ = 0;
	}

	// **IF THE MODE MOVED UNDER THE PLAYING DEVICE, THE MODE BEING ENTERED IS ARMED HERE** (backlog EQ).
	// there is some kind of glitch when moving from (tested) shuffle to grain. the grain is
	// frozen until you disable/reenable.
	//
	// Everything mode-private is computed ONCE, in the branch that runs when the gesture begins, gated
	// on the mode armed at that instant. Turning the Mode dropdown on a device already playing never
	// ran it, so Grain inherited the construction defaults — spread ZERO, both voices on the same
	// sample. The rule the zones already follow applies here too: a control the hand moves is expected
	// to act, and Mode is such a control.
	//
	// **AT THE DISPATCH, NOT IN `advanceSlice`, AND THE FIRST ATTEMPT PUT IT THERE.** `playGrain` is its
	// own renderer and never calls `advanceSlice` — so the re-arm sat on a road the entering mode does
	// not travel, and the case stayed red with the fix applied. **THIS DEVICE HAS NOW PAID FOR THAT
	// SHAPE FOUR TIMES** (density reached the slice modes and not Grain; the hard cut reached the looper
	// renderer and not the grain renderer; the repaint reached the card and not the panel; and now
	// this). The dispatch below is the one place every mode passes through.
	//
	// **THE BUFFER IS NOT TOUCHED.** `reset()` would clear it — the bypass road's job, and a worse bug
	// than this one: a looper silenced mid-performance.
	//
	// **ONLY ON A CHANGE.** Arming Grain every block would re-zero its two phases at the block rate and
	// the cloud would buzz rather than drift.
	if (target_.mode != armedMode_) {
		if (target_.mode == Mode::Grain) {
			armGrain(target_.zoneA);
		}
		// **AND BURST, WHICH WAS ARMED ONLY AT THE PRESS (2026-09-24).** Entered from a looper
		// gesture it had nothing: `capturedSliceFrames_` is 0 after a looper press, so Burst's cycle was 0
		// and `playBurst` returned on its first line — the device passed the dry and its playhead stood
		// still (found by the liveness fuzz, every failure in 37 of 40 seeds). The decision: **arm from the
		// cursor** — the live Rate's division, read from where the looper was reading when the hand turned
		// the dropdown, so the stutter picks up the material under the playhead rather than the bar's top.
		if (target_.mode == Mode::Burst && (kLooperModes & kModeBit(armedMode_)) != 0u) {
			armSliceFromCursor();
		}
		// BURST'S GRAIN IS HALF WHATEVER SLICE IS HELD, whichever road armed it.
		if (target_.mode == Mode::Burst) {
			burstGrain_ = capturedSliceFrames_ / 2;
			burstCyclePos_ = 0;
			burstReadPos_ = 0;
		}
		armedMode_ = target_.mode;
	}

	if (target_.mode == Mode::Burst) {
		playBurst(i, frames);
	}
	else if (target_.mode == Mode::Grain) {
		playGrain(left, right, i, frames);
	}
	else {
		playLooper(left, right, i, frames);
	}
}

//--------------------------------------------------------------------------------------------------//
// BURST: one captured slice, gated on a cycle the Rate knob sets.                                    //
//--------------------------------------------------------------------------------------------------//

/// **THE RATE AGAINST THE CAPTURE, LIVE — THE ONE THING IN THIS DEVICE THAT IS NOT A PRESS SNAPSHOT.**
///
/// The captured LENGTH is fixed at the press; this is that length over the length the Rate knob asks
/// for now, `captured / live`, read every block. Turn Rate to a faster division and `live` shrinks and
/// the step rises above one.
///
/// BOUNDED, because `live` is the player's and a division four halvings away from a bar is not the
/// limit of what automation can ask for between two blocks. Sixteen either way is past anything the
/// control itself can reach.
double Sklitter::rateStep() const {
	if (capturedSliceFrames_ == 0) {
		return 1.0;
	}
	const size_t live = resolveSliceFrames();
	if (live == 0) {
		return 1.0;
	}
	const double step = static_cast<double>(capturedSliceFrames_) / static_cast<double>(live);
	return std::clamp(step, 1.0 / 16.0, 16.0);
}

/// **HOW LONG ONE BURST CYCLE IS, RIGHT NOW.** The captured slice divided by `rateStep()`, floored
/// at a minimum cycle so a fast Rate cannot reach audio-rate gating. Rate up, cycle shorter, bursts more often; the grain itself never changes length.
size_t Sklitter::burstCycleLength() const {
	if (capturedSliceFrames_ == 0) {
		return 0;
	}
	const double step = rateStep();
	const double cycle = static_cast<double>(capturedSliceFrames_) / (step > 0.0 ? step : 1.0);
	const auto rounded = static_cast<long long>(std::llround(cycle));
	return rounded < static_cast<long long>(minBurstCycle_) ? minBurstCycle_ : static_cast<size_t>(rounded);
}

/// **THE GATE.** In the grain, read at native speed, with no pitch change. After the grain, silence
/// until the cycle completes.
void Sklitter::playBurst(int from, int frames) {
	const size_t cycle = burstCycleLength();
	if (cycle == 0 || burstGrain_ == 0) {
		return;
	}
	// **CLAMPED TO NINE TENTHS OF THE CYCLE, AND NEVER BELOW 32.** Both are the reference's: a grain
	// filling its cycle is not a burst, it is a loop, and one of a handful of samples is a click with
	// a period.
	size_t grain = burstGrain_;
	const size_t duty = cycle * 9 / 10;
	if (grain > duty) {
		grain = duty;
	}
	if (grain < minBurstGrain_) {
		grain = minBurstGrain_;
	}

	for (int i = from; i < frames; ++i) {
		sampleControls(i); // Mix and Width at this sample (backlog FK)
		float l = 0.0f;
		float r = 0.0f;
		if (burstCyclePos_ < grain) {
			ring_.readAt(sliceOrigin_ + burstReadPos_, &l, &r);
			if (++burstReadPos_ >= grain) {
				burstReadPos_ = 0;
			}
		}
		wetL_[static_cast<size_t>(i)] = wetOf(l, kNoGrainEnd, 0, 0);
		wetR_[static_cast<size_t>(i)] = wetOf(r, kNoGrainEnd, 0, 0);
		mixCoeff_[static_cast<size_t>(i)] = mixAt();
		++envPos_;
		advanceEngageFade();

		if (++burstCyclePos_ >= cycle) {
			burstCyclePos_ = 0;
			burstReadPos_ = 0;
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE SIX LOOPER MODES: a bar cut into slices, each one planned and scheduled.                       //
//--------------------------------------------------------------------------------------------------//

/// **THE SLOT ENDS ON THE WARP'S ABSOLUTE BOUNDARY AND STARTS WHERE THE LAST ONE REALLY ENDED (backlog IG).**
/// See `slotStartFrame_` for why the difference, rather than `boundary(s + g) − boundary(s)`, is the length.
///
/// **A BAR THAT HAS NOT BEEN SWUNG KEEPS `scheduleFor`'s LENGTH, UNTOUCHED** — so Straight is not merely
/// equal to what shipped but IS it. Once a bar has swung a slot, it finishes on absolute boundaries even
/// if the feel returns to Straight mid-bar: those are the straight grid's own `k · base`, reached from
/// wherever the swung slot ended, which is what re-anchors the walk instead of slipping it.
void Sklitter::applyGrooveToSlot() {
	const GrooveFeel feel = target_.grooveFeel();
	const bool active = grooveActive(feel);
	if (!active && !grooveAnchored_) {
		return;
	}
	const size_t end = grooveBoundary(feel, sliceIndex_ + schedule_.grainLength, numSlices_, sliceFrames_);
	if (slotStartFrame_ >= end) {
		// **THE GRID MOVED UNDER THE WALK** — Rate re-cut the bar mid-slot, so the old start is on a
		// different grid. Re-anchor on this slice's own boundary rather than produce a slot of no length.
		slotStartFrame_ = grooveBoundary(feel, sliceIndex_, numSlices_, sliceFrames_);
	}
	if (end > slotStartFrame_) {
		schedule_.sliceLength = std::max(end - slotStartFrame_, minSliceFrames_);
	}
	grooveAnchored_ = grooveAnchored_ || active;
}

void Sklitter::advanceSlice() {
	// THE COORDINATES SPLIT INTO A POSITION AND A LAP, and the lap joins the declared phase offset --
	// which is what "each lap is the same eight somewhere else in the field" means. `owltt.cpp` joins
	// its Character to the same DSP the same way.
	// **LIVE, NOT THE PRESS SNAPSHOT — and reading the snapshot here made the device inert.** On an
	// installed build no control changed the pattern after it had started. That was a defect, and the
	// reference says so: `updateLiveParams` exists precisely to allow
	// *"real-time adjustment of phase offsets while scatter is playing"*, and the zones are re-read
	// from the ParamManager at every slice setup rather than captured at the press.
	//
	// WHAT THE SNAPSHOT IS ACTUALLY FOR is the LATCH decision — whether a release ends the gesture —
	// and `EngageMachine` keeps that one on its own. Everything else is a control the hand expects to
	// move under it, which is the whole point of a performance effect.
	const Coordinate a = splitCoordinate(target_.zoneA);
	const Coordinate b = splitCoordinate(target_.zoneB);

	// AND THE RATE BUYS ITS SLICES LIVE TOO. The reference recomputes `scatterNumSlices` from the rate
	// knob inside every slice setup, not once at the press — so turning it while the loop runs changes
	// how the bar is cut, which is most of what playing this device IS. The captured BAR does not
	// change; only how many pieces it is read in.
	if (target_.isLooperMode() && sliceFrames_ > 0) {
		numSlices_ = (target_.mode == Mode::Repeat)
		                 ? static_cast<int32_t>(std::max<size_t>(
		                       1, sliceFrames_ / repeatSliceLength(target_.rate, sliceFrames_)))
		                 : sliceCountFor(target_.mode, target_.rate);
		sliceIndex_ = numSlices_ > 0 ? sliceIndex_ % numSlices_ : 0;
	}

	// **THE SEQUENCE FOLDS THE BAR COUNTER BEFORE THE PLAN READS IT.** `planFor` consumes this as two
	// bits, which is a four-bar cycle and was hard-wired; `Param::SeqLength` chooses how many of those
	// four the arrangement actually uses. At 4 the modulo is the identity and the device evolves as it
	// shipped; at 1 every bar is bar zero and the arrangement simply repeats.
	//
	// **FOLDED HERE RATHER THAN AT THE COUNTER**, because `barIndex_` has two other readers that must
	// keep counting freely: Time's phrase (`barIndex_ % kTimePhraseLength`) and the pan's
	// `barIndex_ * 1.3` both want the real bar, and folding at the source would have silently shortened
	// those too. Two consumers, two periods, one counter.
	//
	// **THE LAP IS THE PHASE.** There is no separate phase control any more: an unbounded coordinate's
	// whole part IS how far past the eighth zone it has turned, which is the same number a
	// push-and-twist offset used to supply. `zone_coord_cell.h` carries the decision.
	//
	// **MACRO CFG USED TO SPLIT LIKE THE OTHER TWO AND THERE IS NOTHING LEFT TO SPLIT.** Its fraction was
	// the position inside the eight named regions and its whole part the offset the deleted phase param
	// supplied; the control itself is deleted now (2026-09-19) and `kNoKnobConfig` is what the port's
	// signature is fed instead. The four waypoints' configurations reach the plan inside `push`.
	const Coordinate& mc = kNoKnobConfig;
	// **AND THE MACRO'S PHASE PUSH RIDES IN WITH THEM** — see `offsetsFor`, which is the one builder.
	//
	// **THE PUSH IS THE PERFORM CIRCLE'S NOW, AND IT IS BUILT ONCE FOR THE WHOLE SLICE.** `performPushOf`
	// blends the Macro/Macro Cfg knobs with the four waypoints at the puck's weights (`perform.h`); a
	// centred puck returns the knobs' own push to the bit, so this is today's sound until the puck moves.
	// **ONE BUILD, HANDED TO BOTH CALLS BY REFERENCE** — `offsetsFor` reads two of the eight terms and
	// `planFor` the other six, and building it twice would be six phi triangles paid twice as well as two
	// values free to differ the day one call site is edited.
	const MacroPush push = targetPush();
	lastPush_ = push; // the test seam; see `slicePushForTest`
	// **AND THE SEND OVERRIDE RIDES IN WITH THEM.** It is not a push — it REPLACES the delay
	// probability rather than joining the bias — so it travels in `PhaseOffsets` beside
	// `macroPushesDelay`; `slice_plan.h` states why at the field.
	const PhaseOffsets ofs = offsetsFor(a, b, mc, push, target_.gamma, seqBarIndex(),
	                                    target_.mode == Mode::Repeat, target_.sendOverride);

	// **WHICH INDEX THE PLAN IS DRAWN AT** — Repeat's own counter, everything else's slice. See
	// `repeatLoopIndex_`.
	const int32_t grainIndex = (target_.mode == Mode::Repeat) ? repeatLoopIndex_ : sliceIndex_;

	// **THE HOLD, AND TIME'S OVERRIDE (ported 2026-09-23)** — see `PlanHold`. A slot that arrives by the
	// natural advance carries the hold on; any other arrival rebuilds it by walking this bar from step 0
	// exactly as the head would have, so the plan a step plays does not depend on how the cursor got there
	// (and the band, which walks the bar the same way, draws it). A TURNED CONTROL is such an arrival too —
	// see `HoldKey`: once a slot, a comparison of a few numbers, nothing allocated.
	const HoldKey holdKey { target_.zoneA, target_.zoneB,       push,     target_.gamma, target_.sendOverride,
		                    target_.mode,  numSlices_,          seqBarIndex(), sliceFrames_, bufferFrames_ };
	const bool controlsTurned = !(holdKey == holdKey_);
	holdKey_ = holdKey;
	// **AN ARRIVAL INSIDE A LONG GRAIN JOINS IT (ribbon truth, decision (a)).** The walk from step 0 can step
	// OVER the cursor: after a mid-bar turn the old slot ends where the new controls' walk has a grain
	// running (and a seek can land in one). A fresh slot there plays a step the walk — and the band — has
	// under that grain (MEASURED: 3-5 slots per turned run, in Shuffle, Pattern and Time). So the cursor
	// takes the covering grain's step and plan and enters it at its offset, as a seek enters a slot.
	joinEntry_ = 0;
	bool joined = false;
	if (sliceIndex_ != holdNextSlice_ || controlsTurned) {
		hold_ = PlanHold {};
		if (holdsPlans(target_.mode)) {
			for (int32_t s = 0; s < sliceIndex_;) {
				const SlicePlan p = planUnderHold(hold_, target_.mode, a.position, b.position, numSlices_, [&] {
					return planFor(a.position, b.position, mc.position, push, s, ofs, &planCache_);
				});
				const SliceSchedule sch = scheduleFor(target_.mode, a.position, s, numSlices_, sliceFrames_, p,
				                                      bufferFrames_, minSliceFrames_);
				const int32_t next = s + std::max<int32_t>(1, sch.grainLength);
				if (next > sliceIndex_) {
					const size_t from = grooveBoundary(target_.grooveFeel(), s, numSlices_, sliceFrames_);
					joinEntry_ = slotStartFrame_ > from ? slotStartFrame_ - from : 0;
					slotStartFrame_ = from;
					sliceIndex_ = s;
					plan_ = p;
					joined = true;
					break;
				}
				s = next;
			}
		}
	}
	if (!joined) {
		plan_ = planUnderHold(hold_, target_.mode, a.position, b.position, numSlices_, [&] {
			return planFor(a.position, b.position, mc.position, push, grainIndex, ofs, &planCache_);
		});
	}
	// **THE BAR IS THE GRID, THE BUFFER IS THE REACH** — see `scheduleFor`. Rate still buys slices
	// per BAR (chosen over slices-per-buffer); the extra argument is what lets an offset land
	// in a bar other than this one.
	schedule_ = scheduleFor(target_.mode, a.position, sliceIndex_, numSlices_, sliceFrames_, plan_,
	                        bufferFrames_, minSliceFrames_);
	// THE BAR'S END CANCELS THE HOLD — the reference's `if (nextSliceIndex >= scatterNumSlices)
	// scatterRepeatCounter = 0;` *"compute fresh params for new bar"*, taken at the same moment: after this
	// slot's plan, before the next one's.
	if (const int32_t natural = sliceIndex_ + std::max<int32_t>(1, schedule_.grainLength); natural >= numSlices_) {
		hold_.left = 0;
		holdNextSlice_ = 0;
	}
	else {
		holdNextSlice_ = natural;
	}
	// **AND THE GROOVE DECIDES HOW LONG THE SLOT LASTS — before anything below reads its length** (the
	// overlap duck, the read end the consecutive test compares). Where the slot READS from is untouched.
	// **WHAT THE SLICE HOLDS IS TAKEN FIRST (backlog IN addendum)** — see `materialLength_`.
	materialLength_ = schedule_.sliceLength;
	applyGrooveToSlot();

	// **PITCH MODE PICKS A SCALE DEGREE PER SLICE, AND THIS IS WHERE THE TWO TABLES FINALLY GET READ.**
	//
	// `kScaleSemitones` and `kPitchRatioFP` came across verbatim in `decisions.h` and NOTHING READ
	// EITHER — so the only pitch movement in the device was `shouldPitchUp`, a 2x decimation the
	// reference applies in every mode. pitch mode: I think im only hearing octave jumps.
	// That is exactly what a device with the ladder missing sounds like.
	//
	// THE REFERENCE'S OWN ARITHMETIC (`stutterer.cpp`, the `isPitch` block): hash Zone A with the
	// slice index, take three bits as a degree, index the chosen scale's eight degrees, and look the
	// semitone up as a 16.16 ratio. It lives in the stutterer rather than in `computeGrainParams`,
	// which is why it belongs here and not in `planFor`.
	//
	// **THE SEED IS A TRANSLATION AND IS SAID SO.** The reference takes `zoneAParam >> 16` off a q31,
	// which is the top fifteen bits of a 0..1 control. Ours is an unbounded coordinate, so the same
	// bits are its POSITION inside the zone scaled by 32768 — the lap is deliberately not in it, or
	// every lap would be a different set of degrees for a control that is meant to name a region.
	// **THE ANTI-CLICK ARMING, AND THE PREDICTION IT NEEDS.** The reference does exactly this at its
	// slice boundary: a grain is CONSECUTIVE when the audio flows on from the last one — no slice
	// offset, not reversed, not decimated — and the attack protection is armed only when the grain
	// about to start is NOT. Knowing that requires the NEXT grain's plan, so the reference peeks one
	// ahead and so does this. Without the peek the protection fires at every slot edge and becomes
	// the artefact it exists to remove.
	// **ARMED FROM THE PREVIOUS GRAIN'S PREDICTION, AND ONLY THEN RECOMPUTED — the order is the
	// mechanism.** At this instant `nextConsecutive_` still holds what the LAST grain predicted about
	// this one, which is the only thing that can be known before this grain's plan is read. The
	// reference does the same and it reads as a bug until you notice the assignment is further down
	// its own block.
	// **A TAIL THE RELEASE GATE MUTED IS AN EDGE, whatever the flags said.** The bar-end window mutes the
	// last slice of a bar at a crossing even when the next flows on, so the bar's first slice then
	// started at full level out of silence. MEASURED: Pattern's and Pitch's worst jump in the click
	// sweep, 0.59 at slice zero of the next bar.
	const bool tailEndedMuted = releaseMutedL_ || releaseMutedR_;
	if (!nextConsecutive_ || tailEndedMuted) {
		waitingForZeroCrossL_ = waitingForZeroCrossR_ = true;
		// **AND THE MINIMUM-ATTACK RAMP RESTARTS WITH THE GRAIN** — the same instant the crossing gate
		// arms, because they guard the same edge by two different means: the gate waits for a good place
		// to start, the ramp covers the case where there is not one.
		envPos_ = 0;
		prevOutputL_ = prevOutputR_ = 0.0f;
	}
	releaseMutedL_ = releaseMutedR_ = false;

	// **DENSITY IS A PER-SLICE DECISION, NOT A MIX — AND I HAD REGRESSED IT TO ONE.** Did you
	// regress density to a simple mix? Yes. The output read
	// `l * density + input * (1 - density)`, a continuous crossfade on every sample of every slice,
	// which at 0.5 is every grain at half volume over the input at half volume — mush. The reference
	// makes the decision ONCE PER SLICE, by a hash, and its own comment rules out what I wrote:
	// *"Density threshold: hard cut between grain and dry (not a blend)."* Density thins the grain
	// STREAM; it does not fade it.
	//
	// `0xBADC0FFE` is the reference's seed for exactly this and appeared nowhere in the port.
	const float density = std::clamp(target_.density, 0.0f, 1.0f);
	const auto densityWantsDry = [&](int32_t slice) {
		if (density >= 1.0f) {
			return false;
		}
		const uint32_t h = hash::mix(static_cast<uint32_t>(slice) ^ 0xBADC0FFEu);
		const float draw = static_cast<float>(h & 0xFFFFu) / 65535.0f;
		return draw >= density;
	};

	consecutive_ = (plan_.sliceOffset == 0) && !plan_.shouldReverse && !plan_.shouldPitchUp;
	// **ONCE PER SLICE, NOT PER SAMPLE.** The envelope's depth and gate are the PLAN's and the plan
	// does not change inside a slice, so asking this per sample was arithmetic repeated 48,000 times
	// a second for an answer that moves eight times a bar.
	// **AND THE LENGTH GATE COUNTS AS AN ENVELOPE, WHICH IS THE EDGE STILL HEARD.**
	// On the build that already had the crossing gate, segment edges still sounded rough and clicky.
	// `lengthMult` cuts the slice short and the rest of the slot is hard-zeroed — a
	// discontinuity as sharp as any — but this predicate did not mention it. So a slice that was
	// otherwise CONSECUTIVE (no offset, no reverse, no pitch-up) with `envDepth == 0` had the whole
	// protection switched off at exactly the edge that needed it. The reference folds the length into
	// `gatedLength` and measures its release window against that; the equivalent here is to say that
	// a shortened slice has an active envelope.
	//
	// THE THRESHOLDS ARE THE REFERENCE'S, not exact comparisons: `scatterEnvActive = (envDepth >
	// 0.001f)` and `scatterGateActive = (gateRatio < 0.999f)`. A float that lands at 0.9999 through a
	// triangle is not a gate and should not arm one.
	const bool envActive =
	    plan_.envDepth > 0.001f || plan_.gateRatio < 0.999f || plan_.lengthMult < 0.999f;
	zcActive_ = !(consecutive_ && !envActive);

	// **pWRITE IS A PER-SLICE DRAW, NOT A PER-SAMPLE AMOUNT.** The reference:
	// `Bits(sliceIndex ^ (barIndex << 16) ^ 0xDEADBEEF).threshold4(0, pWrite * 16)` — so the control
	// is the PROBABILITY that a given slice writes back at all, and a slice either writes or does
	// not. Never in Repeat (`hasPWrite = isLooperMode && mode != Repeat`), because Repeat is reading
	// one figure over and over and writing into it would erase the figure.
	pWriteThisSlice_ = false;
	if (target_.isLooperMode() && target_.mode != Mode::Repeat) {
		const auto threshold = static_cast<uint8_t>(std::clamp(target_.pWrite, 0.0f, 1.0f) * 16.0f);
		const hash::Bits sliceBits(static_cast<uint32_t>(sliceIndex_)
		                           ^ (static_cast<uint32_t>(barIndex_) << 16) ^ 0xDEADBEEFu);
		pWriteThisSlice_ = sliceBits.threshold4(0, threshold);

		// **THE OVERLAP DUCK, which is the feedback guard.** If the region being written intersects
		// the region being read, this slice does not write — otherwise the loop reads what it just
		// wrote and runs away. The reference exempts a DRY slice: *"Fresh input has no feedback
		// concern — we're not reading what we just wrote."*
		if (pWriteThisSlice_ && bufferFrames_ > 0 && !sliceIsDry_) {
			const size_t playback = bufferFrames_;
			const size_t readStart = schedule_.startOffset % playback;
			const size_t writeStart = linearBarPos_ % playback;
			const size_t len = schedule_.sliceLength;
			const size_t writeInRead = (writeStart + playback - readStart) % playback;
			const size_t readInWrite = (readStart + playback - writeStart) % playback;
			if (writeInRead < len || readInWrite < len) {
				pWriteThisSlice_ = false;
			}
		}
	}

	// **THE PICTURE USED TO BE WRITTEN HERE, ONE STEP AT A TIME, AND THAT WAS THE BUG .**
	//
	// The displayed bar was overwritten as the playhead passed, because the playhead spans one bar
	// while the arrangement can repeat over four. A slot was indexed by position within a BAR and written as the
	// head reached it, while `planFor` evolves the arrangement over four — so slot k held bar 0's
	// answer until the head passed it in bar 1, then bar 1's until bar 2. The band was never a
	// pattern; it was a seam walking along with the playhead.
	//
	// **MEASURED BEFORE IT WAS REMOVED** (`test_fx_sklitter.cpp`, "MEASURE: how much of the published
	// picture is a DIFFERENT bar's pattern"): at 32 slices to the bar, TWENTY of the 32 slots change
	// from one bar to the next — and the head only reaches SEVENTEEN of them, because
	// `sliceIndex_ += schedule_.grainLength` steps over the slots a long grain spans, leaving those
	// showing whatever bar last wrote them rather than the previous one. Two independent ways of not
	// being a picture, and writing it here could not fix either.
	//
	// It is published as ONE COHERENT WALK over the whole sequence in `publishReadings` now. Nothing
	// replaces these lines: the arrangement is a pure function of the controls, so the playing device
	// has no information about it that the walk does not already have.
	//
	// **THE ONE THING THAT IS LOST IS DENSITY'S VOTE**, and it is deliberate. `sliceIsDry_` includes
	// `densityWantsDry`, which draws per slice from a hash that only advances as the bar plays — so it
	// is knowable HERE and nowhere else. Drawing it meant the picture could only ever be assembled
	// under the head, which is the bug. The two roads to dry that the controls settle (the plan's and
	// the macro's) are still drawn; density thins the stream without announcing which slices it took.

	// **THE PAN, WHICH THE PLAN HAD BEEN COMPUTING FOR NOBODY.** Three cases, the reference's:
	// Repeat takes its direction from the BAR so a repeated figure alternates sides bar by bar;
	// anything longer than one slot does not pan, because a pan spread across a long grain reads as a
	// mix error rather than as movement; everything else alternates on a free-running counter. The
	// multipliers 1.3 and 5.3 are the reference's, wrapped, so the sides do not fall into a period.
	{
		float pan = 0.0f;
		if (target_.mode == Mode::Repeat) {
			const float dir =
			    phi::wrapPhase(static_cast<float>(barIndex_) * 1.3f) < 0.5f ? -1.0f : 1.0f;
			pan = dir * plan_.panAmount;
		}
		else if (schedule_.grainLength > 1) {
			pan = 0.0f;
		}
		else {
			const float dir =
			    phi::wrapPhase(static_cast<float>(panCounter_++) * 5.3f) < 0.5f ? -1.0f : 1.0f;
			pan = dir * plan_.panAmount;
		}
		const float panAbs = std::abs(pan);
		panActive_ = panAbs > 0.001f;
		panFade_ = 1.0f - panAbs;
		panCross_ = panAbs * 0.5f;
		panRight_ = pan > 0.0f;
	}

	// **TIME MODE TAKES THE BAR-END SILENCE ONCE A PHRASE, NOT ONCE A BAR.** The reference's line is
	// `!isTime || ((scatterBarIndex % kTimePhraseLength) == kTimePhraseLength - 1)`: Time builds a
	// four-bar phrase, and silencing the seam of every bar inside it would cut the phrase into four.
	barEndSilence_ = (target_.mode != Mode::Time)
	                 || ((barIndex_ % kTimePhraseLength) == kTimePhraseLength - 1);

	// PITCH MODE'S RATIO FOR A SLICE — one function, because the join below asks it of the NEXT slice as
	// well as this one.
	const auto ratioFor = [&](int32_t slice) {
		if (target_.mode != Mode::Pitch) {
			return 1.0;
		}
		const auto zoneASeed = static_cast<uint32_t>(a.position * 32768.0f);
		const uint32_t hashInput = zoneASeed ^ (static_cast<uint32_t>(slice) * 2654435761u);
		const uint32_t hashVal = hash::mix(hashInput);
		const auto degreeIdx = static_cast<size_t>(hashVal & 0x7u);
		const auto scaleIdx = static_cast<size_t>(
		    std::clamp(static_cast<int>(std::lround(target_.pitchScale)), 0, 24));
		const int semitones = std::clamp(static_cast<int>(kScaleSemitones[scaleIdx][degreeIdx]), 0, 17);
		return static_cast<double>(kPitchRatioFP[semitones]) / 65536.0;
	};
	{
		// ONE SLICE AHEAD, THROUGH ITS OWN CACHE. Sharing the playing grain's would evict the entry
		// the next block needs on every single call — the reference peeks through its single static
		// cache and pays exactly that.
		//
		// **THE SLICE THAT WILL ACTUALLY PLAY NEXT, NOT `grainIndex + 1`.** The advance steps by
		// `schedule_.grainLength` and wraps to zero at the bar with the bar counter moving on; the peek
		// asked for the neighbouring INDEX, which is a slice that never plays whenever a grain spans more
		// than one slot. MEASURED on Shuffle: indices ran 0, 1, 5, 6, 10… and the peek disagreed with
		// the next slice's own `consecutive_` at joins in every bar — so one side of the join faded and
		// the other did not, a full-height step in the output's envelope and in the pWrite buffer. The
		// reference has the same `scatterSliceIndex + 1` against a `+= grainLength` advance; its inline
		// path papers over one direction of it by forcing `scatterConsecutive = true`.
		const int32_t step = std::max<int32_t>(1, static_cast<int32_t>(schedule_.grainLength));
		const bool nextWraps = sliceIndex_ + step >= numSlices_;
		const int32_t nextSlice = nextWraps ? 0 : sliceIndex_ + step;
		const int32_t nextGrainIndex =
		    (target_.mode == Mode::Repeat) ? ((repeatLoopIndex_ + 1) & 0x7) : nextSlice;
		PhaseOffsets nextOfs = ofs;
		if (nextWraps) {
			const int32_t bars = target_.seqBars > 0 ? target_.seqBars : 1;
			const int32_t nextBar = (barIndex_ + 1) % kBarIndexWrap;
			nextOfs.barIndex = ((nextBar % bars) + bars) % bars;
		}
		// THE SAME PUSH THE PLAYING SLICE WAS DRAWN FROM — this block is inside `advanceSlice`, so `push`
		// is the one built above. The peek is a PREDICTION of the next slice under the controls as they
		// stand; rebuilding the push here would say nothing different and would cost a second blend.
		// AND THROUGH A COPY OF THE HOLD, so the peek predicts the plan that will actually play: the cached
		// one while the hold has slots left, a fresh draw (Time's override included) once it has not. The
		// bar's end has already zeroed it above when the next slot wraps.
		PlanHold peekHold = hold_;
		const SlicePlan next =
		    planUnderHold(peekHold, target_.mode, a.position, b.position, numSlices_, [&] {
			    return planFor(a.position, b.position, mc.position, push, nextGrainIndex, nextOfs, &nextPlanCache_);
		    });
		// THE SEAM FOR THIS ROAD, and it exists because reverting the argument above to `target_.macro`
		// left every other perform case green — the "a fix reached one road of two" shape this device has
		// paid for four times, appearing inside the commit that fixed the other three. See
		// `nextSlicePlanForTest`. The wrap flag rides with them because it is not recoverable from the
		// index in Repeat, where that index is the repeat counter rather than a slice.
		nextPlan_ = next;
		nextGrainIndex_ = nextGrainIndex;
		nextWraps_ = nextWraps;

		// **CONSECUTIVE MEANS THE READ CONTINUES — ASKED OF THE READ POSITIONS, NOT OF THE PLAN.** The
		// reference's test is `sliceOffset == 0 && !reversed && !pitchUp`, which is right in Shuffle, where
		// a slice with no offset reads its own slot. It is not right where the SCHEDULE moves the read:
		// Pattern re-orders the slots, Repeat reads one figure again, and a ratchet or a Pitch ratio
		// changes where a slot's read ends. There the flag said "flows on" across a jump in the material
		// and both sides skipped their fade. MEASURED: Pattern at 10x the input's step on a join flagged
		// consecutive, material 0.28 apart. So a join is consecutive exactly when the next read starts
		// where this one ends, both reading forwards at unison with no ratchet.
		const auto readsOn = [&](const SlicePlan& p, int32_t slice) {
			return !p.shouldReverse && !p.shouldPitchUp && p.subdivisions <= 1 && ratioFor(slice) == 1.0;
		};
		const size_t loopLen = std::max<size_t>(bufferFrames_, 1);
		const bool thisReadsOn = readsOn(plan_, sliceIndex_);
		// **THE READ ENDS WHERE THE MATERIAL DOES, NOT WHERE A LENGTHENED SLOT DOES (backlog IN addendum).**
		// A swung on-beat slot reads its own slice and is then silent (`grooveGated_`), so its read ends at
		// the next slice's start — which is exactly where the delayed slice begins reading. Measured against
		// the swung length instead, every swing seam looked like a jump: MEASURED at Swing 16 66%, 720
		// frames of release mute before each off-beat and a 478-frame fade over its attack. A slot the
		// groove SHORTENED still ends early, and the join after it is the real jump it is (the choke).
		const size_t readLength = std::min(schedule_.sliceLength, materialLength_);
		const size_t thisReadEnd = (schedule_.startOffset + readLength) % loopLen;
		const SliceSchedule nextSchedule = scheduleFor(target_.mode, a.position, nextSlice, numSlices_,
		                                               sliceFrames_, next, bufferFrames_, minSliceFrames_);
		nextConsecutive_ = thisReadsOn && readsOn(next, nextSlice)
		                   && (nextSchedule.startOffset % loopLen) == thisReadEnd;
		const bool readContinuedFromPrev =
		    thisReadsOn && prevReadEnd_ < loopLen && (schedule_.startOffset % loopLen) == prevReadEnd_;
		prevReadEnd_ = thisReadsOn ? thisReadEnd : kNoReadEnd;

		// **THE WHOLE DRY DECISION, WHICH IS THREE ROADS AND NOT ONE.** The plan's own hash, the
		// macro's vote, and density's draw — any of them makes the slice dry. The previous version of
		// this line consulted `plan_.useDry` alone, so a slice that was dry BECAUSE OF DENSITY was
		// treated as a grain by the anti-click system and got an attack mute it did not need.
		const bool wantsDry = plan_.useDry || plan_.macroWantsDry || densityWantsDry(sliceIndex_);
		// THE MACRO'S VOTE IS THE SAME FOR BOTH — it depends on the controls, not on the slice, and
		// the reference reuses the one it computed rather than asking the next grain's plan.
		const bool nextWantsDry =
		    next.useDry || plan_.macroWantsDry || densityWantsDry(nextSlice);

		// A DRY GRAIN FOLLOWED BY A DRY GRAIN IS THE INPUT, UNBROKEN — the reference says so in as
		// many words (*"Current is dry, next is dry -> skip decay"*), and it matters because dry is
		// where a spurious mute is most audible: the listener is hearing the track itself.
		//
		// **AND A JOIN WHERE THE SOURCE CHANGES IS NEVER CONSECUTIVE — a divergence from the reference,
		// which only ever forces these flags TRUE.** Its plan-based test (no offset, no reverse, no
		// pitch-up) says the BUFFER READ continues; it says nothing about a dry slice handing over to a
		// wet one, where the source jumps from the live input to the buffer. Both sides then skip their
		// fade and the join is a hard step — in the output, and in the pWrite buffer. MEASURED: the one
		// seam left in the Shuffle buffer after every write fix was exactly this, a dry slice's 1.0
		// meeting a wet slice's 0.0 at a slice boundary.
		nextConsecutive_ = (wantsDry && nextWantsDry) || (nextConsecutive_ && !wantsDry && !nextWantsDry);
		// AND THE ATTACK SIDE NEEDS THE PREVIOUS SLICE'S ANSWER, which is why it is remembered:
		// *"Previous was dry, current is dry -> skip attack"*.
		consecutive_ = (wantsDry && prevSliceDry_) || (readContinuedFromPrev && !wantsDry && !prevSliceDry_);
		//
		// **AND NOR IS A JOIN WITH SILENCE IN IT.** A slice cut short by `lengthMult` hard-zeroes the rest
		// of its slot, so whatever follows starts out of silence however continuous the READ is. With both
		// flags set, the cut slice skipped its decay and the next skipped its attack: a full-height onset.
		// MEASURED: about seventy of the click sweep's 123 jumps were exactly this — output 0.00, then
		// -0.95 on the first sample of a "consecutive" slice.
		//
		// **ONLY WHERE THE TAIL IS ACTUALLY SILENCED — AND A DRY SLICE RUNNING INTO A DRY ONE DOES NOT
		// SILENCE IT.** The input is continuous across that join, so cutting it would put a gap in the
		// track itself, and marking the join non-consecutive cascades: the next dry slice then loses its
		// own exemption, and so on down the run. MEASURED both ways: gating every cut made the density
		// probe case read 127 where it measures 150+. The reference passes a dry run unbroken
		// (`skipEnvForDry`, *"input audio is continuous"*); this keeps that without its hard edge where
		// a cut slice is followed by a flag that says the audio flows on.
		if (prevSliceCut_ || tailEndedMuted) {
			consecutive_ = false;
		}
		// A GATE BELOW ONE SILENCES THE TAIL TOO — the last sub-slice ends at its gated end.
		const bool silencesTail =
		    (plan_.lengthMult < 0.999f || plan_.gateRatio < 0.999f) && !(wantsDry && nextWantsDry);
		if (silencesTail) {
			nextConsecutive_ = false;
		}
		prevSliceCut_ = silencesTail;
		tailSilenced_ = silencesTail;
		// The crossing gate was armed from the plan-based flag above; re-derive it from the final one.
		zcActive_ = !(consecutive_ && !envActive);
		prevSliceDry_ = wantsDry;
		sliceIsDry_ = wantsDry;
		// A DRY SLOT IS THE LIVE INPUT, WHICH HAS NO "NEXT SLICE'S HIT" TO REACH INTO — gating it would put
		// a hole in the track itself, the thing `tailSilenced_`'s dry exemption exists to prevent.
		// The outgoing slot's answer is kept first: it is the silence this slot's attack starts out of.
		grooveGapBefore_ = grooveGated_;
		grooveGated_ = !wantsDry && materialLength_ < schedule_.sliceLength;
	}

	pitchRatio_ = ratioFor(sliceIndex_);
	// A JOINED GRAIN IS ENTERED WHERE THE CURSOR REALLY IS — see the hold above.
	positionInSlice_ = schedule_.sliceLength > 0 ? std::min(joinEntry_, schedule_.sliceLength - 1) : 0;

	// **THE ECHO IS A QUARTER BAR, ALWAYS** -- the reference calls it "classic rhythmic delay" and
	// does not make it a control. `shouldDelay` says whether this grain sends at all; `delaySendBits`
	// says how much, as a bit shift: 1,2,3 -> 25%, 50%, 100%.
	if (plan_.shouldDelay && plan_.delaySendBits > 0 && delay_.capacity() > 1) {
		delayTime_ = std::min(sliceFrames_ / 4, delay_.capacity() - 1);
		delaySend_ = 1.0f / static_cast<float>(1u << (3u - plan_.delaySendBits));
	}
	else {
		delayTime_ = 0;
		delaySend_ = 0.0f;
	}

	// **THE GRAIN LANE'S SEND FOR THIS SLICE** (backlog EW). Armed here, beside the echo's, because
	// this is where the looper's grain BEGINS — the same place the plan's other per-grain decisions are
	// latched.
	//
	// **BINARY, NOT A LEVEL, AND THAT IS A CHOICE WORTH KNOWING ABOUT.** The echo carries a drawn
	// AMOUNT as well as a decision (`delaySendBits`: 25/50/100%); the lane does not, because it has a
	// chain with its own Mix and gain on the other end, so a second attenuation drawn per grain would
	// be two answers to one question. Whether the lane should ALSO inherit a per-grain level is
	// backlog EW's open question and is open; this is the simplest thing that is honest.
	grainSend_ = plan_.shouldSendToLane ? 1.0f : 0.0f;
}

void Sklitter::playLooper(float* left, float* right, int from, int frames) {
	if (sliceFrames_ == 0 || numSlices_ <= 0) {
		return;
	}
	// **THE REJOIN — backlog GT/HE, AND IT IS UNGATED FROM THE LOCK.** Both settings re-phase when the
	// transport restarts; the toggle decides only whether the device kept sounding through the gap.
	// Locked, the cursor was parked and this is a resume from silence. Unlocked, the cursor free-ran
	// and this is the rejoin named as the feature: with the lock off the effect slips past a stopped
	// transport and rejoins when it resumes.
	//
	// **ONLY WHILE THE TRANSPORT ROLLS**, which is what keeps the unlocked free run free — see the arm
	// site in `renderStereo`.
	//
	// **AND THE UNLOCKED ONE DUCKS FIRST, WHICH IS THE WHOLE OF WHAT THE TWO SETTINGS COST
	// DIFFERENTLY.** Locked, the device produced nothing through the gap and `renderChunk` left the dry
	// bit for bit, so the seek lands on a signal that is ALREADY the dry and the step across it is the
	// input's own. Unlocked, the device was sounding: the same seek moves the read cursor under a grain
	// the player is hearing, and the wet it jumps to has no relation to the wet it jumped from.
	// MEASURED (`backlog HE: MEASURE`) on a 311.13 Hz probe whose own largest step is 0.0407 — the step
	// across the restart was **0.511** without the duck and **0.0392** with it, against **0.0399** for
	// the locked resume on the identical script.
	//
	// **ONE FADE, NOT TWO — `mixAt` runs `engageFadeFrames_` backwards.** The device already fades the
	// wet up from the dry at a press; a rejoin needs the same ramp in the other direction first, and a
	// second shape with its own length would be two answers to one question.
	//
	// **THE SEEK WAITS FOR THE BLOCK TOP AFTER THE RAMP RUNS OUT** rather than splitting the block at
	// the exact frame. It costs one block of dry at the bottom of the duck — where the wet is already
	// at zero gain, so nothing can be heard to happen there — and it keeps the seek at the one place a
	// seek happens.
	//
	// **AND THE DEBT IS CLEARED WHETHER OR NOT THE SEEK LANDS**, because the answer to GT's one open
	// question is that a host offering no beat position falls back to the free-running counter.
	// Retrying it every block would be a different answer — the device would keep pretending it owed a
	// seek it can never make, and would call `advanceSlice` on every block for as long as the host
	// stayed positionless.
	//
	// **AND NOTHING TO DUCK AT A PRESS.** Since GU the press owes this seek in both settings, and the
	// trigger has just put the wet at the dry (`engageFadePos_ = 0`, not yet advanced): there is no
	// grain under the cursor for a duck to hide, and ducking from full would START by playing the wet
	// at full — a click the duck exists to prevent. So an unlocked debt ducks only once the wet has
	// come up at all, which a rejoin after a stop always has.
	if (rejoinPending_ && clock_.playing) {
		if (!rejoinDucked_ && !transportLockActive() && engageFadeFrames_ > 0 && engageFadePos_ > 0) {
			rejoinDucked_ = true;
			rejoinDuckLeft_ = engageFadeFrames_;
		}
		else if (rejoinDuckLeft_ == 0) {
			rejoinPending_ = false;
			const bool ducked = rejoinDucked_;
			rejoinDucked_ = false;
			// **AND IF THE SEEK CANNOT LAND, THE WET STILL HAS TO COME BACK.** A host that says it is
			// rolling but publishes no playhead leaves `lockedCursor` with nothing to derive from, and
			// a duck without the `engageFadePos_` reset `seekToTransport` performs would snap the wet
			// to full from the dry — the click the duck was run to avoid, at the other end of it.
			if (!seekToTransport(static_cast<double>(windowBase_ + from)) && ducked) {
				engageFadePos_ = 0;
			}
		}
	}
	if (schedule_.sliceLength == 0) {
		advanceSlice();
	}

	// THE SLICE'S AUDIBLE LENGTH IS THE SCHEDULED ONE TIMES THE PLAN'S MULTIPLIER; what is left of the
	// slot plays silence, which is what makes `lengthMult` a rhythm rather than a volume.
	// THE BAR AS ONE LINE, for the bar-end window. The playback step times the slot plus where we are
	// inside it -- the position in what comes OUT, which is where the seam is heard, rather than in
	// what was recorded.
	// THE CAPTURED LOOP, which is the buffer and not the bar since `Param::BufferLength`.
	const size_t barLength = bufferFrames_;

	for (int i = from; i < frames; ++i) {
		sampleControls(i); // Mix and Width at this sample (backlog FK)
		const size_t slotLength = schedule_.sliceLength;
		// **A SLOT THE GROOVE LENGTHENED SOUNDS ONLY ITS OWN SLICE (backlog IN addendum).** Past the material
		// the read would reach the NEXT slice's hit and play it on the straight grid, before the delayed
		// slice plays it again late — MEASURED at Swing 16 66%: every off-beat heard at full level for
		// ~1200 frames at its unswung time, then again at its swung one. MPC/REX play a swung hit once,
		// late, with the gap before it silent; so does this.
		const size_t soundingLength = grooveGated_ ? materialLength_ : slotLength;
		const auto audible = static_cast<size_t>(static_cast<double>(soundingLength) * plan_.lengthMult);
		// ONE PAIR PER SAMPLE-BLOCK ITERATION, from the grain's audible length — cheap, and it follows a
		// live `lengthMult` or `Min Env` change without waiting for the next slice.
		//
		// **PER RATCHET SUB-SLICE, TO ITS GATED END — the reference's envelope geometry.** It prepares the
		// envelope on `scatterSubSliceLength` and cuts at `gatedLength = subSliceLength * gateRatio`, so
		// every ratchet restart and every gate cut is an edge with its own fade. Ours faded once, at the
		// slice's audible end: a ratchet jumped back to the slice start at full level and a gate chopped
		// to silence mid-slice. MEASURED once the click guard was given arrangements that produce them —
		// it had none: 4,800 slices at its Macro Cfg without a single gate or ratchet — 35x the input's
		// own step in every slice mode.
		const auto subdivisions = static_cast<size_t>(std::max(1, plan_.subdivisions));
		const size_t subLength = subSliceLength(audible, subdivisions);
		size_t whichSub = 0;
		size_t inSub = positionInSlice_;
		if (subLength > 0) {
			whichSub = std::min(positionInSlice_ / subLength, subdivisions - 1);
			inSub = positionInSlice_ - whichSub * subLength;
		}
		const bool lastSub = whichSub + 1 == subdivisions;
		// THE LAST SUB-SLICE ABSORBS THE REMAINDER, as the read below does — and EVERY sub-slice ends at the
		// audible end at the latest, because a floored ratchet runs out before its last piece.
		const size_t toAudibleEnd = audible > whichSub * subLength ? audible - whichSub * subLength : 0;
		const size_t subEnd = lastSub ? toAudibleEnd : std::min(subLength, toAudibleEnd);
		// **A DRY SLICE RUNNING INTO A DRY ONE IS NOT GATED** — the input passes on, as its tail does
		// (`tailSilenced_`).
		const float gate = plan_.gateRatio < 0.999f && (tailSilenced_ || !sliceIsDry_)
		                       ? plan_.gateRatio
		                       : 1.0f;
		const size_t fadeEdge = gate < 0.999f
		                            ? std::min(subEnd, static_cast<size_t>(static_cast<float>(subLength) * gate))
		                            : subEnd;
		const GrainFade grainFade = grainFadeLengths(fadeEdge);
		// **HOISTED ABOVE THE WRITE, because the write needs them too.** They were computed beside the
		// output write at the bottom of the loop, which is AFTER the pWrite block — so the write could
		// not see the ramp that was about to be applied to the sample it was storing.
		const size_t leftInGrain = fadeEdge > inSub ? fadeEdge - inSub : 0;
		// The consecutive guards belong to the SLICE's two ends: the first sub-slice's attack and an
		// ungated last sub-slice's decay. Every other edge inside the slice is a jump or a cut.
		//
		// **EXCEPT ACROSS A SWING GAP, WHERE THE READ FLOWS ON AND THE SOUND DOES NOT (groove-click).** A
		// lengthened slot's material ends, its gap is zero, and the delayed hit starts from that zero at its
		// own first sample — consecutive to the READ test, a step both ways to the ear. MEASURED on a 220 Hz
		// sine at 0.5, 44.1k: 0.13 at the material's end and 0.11 at the delayed hit's start, and on real
		// material the full height of whatever sample the slice starts on. So both edges take the
		// reference's floor ramp, `kMinAntiClickSeconds` (64 samples at 44.1 kHz, 1.45 ms), and not the full
		// grain fade: the addendum's point stands, that a swung hit keeps its attack (a 10 ms fade over it
		// was one of the things that made swing sound "weird"), and 1.45 ms removes the step without
		// taking the transient.
		const size_t gapRamp = std::min(minAntiClickFrames_, fadeEdge / 2);
		const size_t fadeIn =
		    (whichSub == 0 && consecutive_) ? (grooveGapBefore_ ? gapRamp : 0) : grainFade.attack;
		const size_t fadeOut = (lastSub && gate >= 0.999f && nextConsecutive_) ? (grooveGated_ ? gapRamp : 0)
		                                                                       : grainFade.decay;
		// **THE SLOT'S REAL START, ALWAYS — not `sliceIndex_ * slotLength`** (ribbon truth, 2026-09-23). That
		// product is the bar position only while every slot is one step long: a LONG GRAIN's slot is `span`
		// steps, so slice 4 of a 4-step grain read as frame 96,000 of a 96,000-frame bar — inside the bar-end
		// release zone from its first sample, muted at the first crossing, and SILENT for its whole length
		// while the band drew it. MEASURED by the audit: every long grain whose index times its length passed
		// the bar's last 50 ms, in every slicing mode. `slotStartFrame_` is kept on every road (the step,
		// the wrap, the lock, the seek, the press), which is why the groove already used it (backlog IG).
		const size_t barPos = slotStartFrame_ + positionInSlice_;

		// **THE SOURCE IS CHOSEN FIRST, WHICH IS THE ORDER THE REFERENCE USES AND THE ORDER THIS HAD
		// BACKWARDS.** It read the grain, enveloped it, panned it, ran the anti-click gate over it —
		// and THEN threw all of that away and substituted the dry input. So a dry slice got no
		// envelope and no crossing protection at all, which is exactly where the reference is most
		// careful (*"dry stops -> wet envs in from 0 = click without this"*).
		const bool skipEnvForDry = sliceIsDry_ && consecutive_ && nextConsecutive_;
		ReadTraceEntry* const traced =
		    readTrace_ != nullptr && static_cast<size_t>(i) < readTraceLength_ ? &readTrace_[i] : nullptr;
		if (traced != nullptr) { // ribbon truth: `setReadTraceForTest`
			*traced = ReadTraceEntry { -1, sliceIndex_, positionInSlice_, whichSub, sliceIsDry_, 0.0f, linearBarPos_, 1.0f };
		}
		// **THE LAST RATCHET ABSORBS THE REMAINDER.** An integer divide leaves `audible % subdivisions`
		// samples over, and a ratchet that restarts on the quotient drops them — so the slot ends with
		// a sliver of silence, once per slice, which is a rhythmic artefact rather than a rounding
		// one. The reference carries `scatterLastSubSliceLength` for exactly this and uses it when
		// `subdivIndex == subdivisions - 1`.
		// (The sub-slice geometry is computed above, where the fades need it.)

		float l = 0.0f;
		float r = 0.0f;
		long long mainAt = -1; // the ring frame of this sample's main read; −1 when nothing was read
		if (sliceIsDry_) {
			l = left[i];
			r = right[i];
		}
		// **READ THROUGH THE WHOLE SLOT, EVEN PAST THE AUDIBLE END** — the output is silenced there
		// below, but `srcL` is what pWrite records, and the write's release runs a few samples past
		// that end. Reading only inside `audible` left `srcL` at silence there, so every writing slice
		// with a `lengthMult` cut stamped a notch into the buffer, with a full-height step at its start.
		// MEASURED in the click sweep: material at 0.740 followed by 0.012 in the ring, replayed by
		// every later pass.
		else if (positionInSlice_ < slotLength) {
			// **THE RATCHET RESTARTS THE SLICE N TIMES INSIDE ITS OWN SLOT.** `subdivisions` is a
			// repeat of the slice START, not a faster read, so the pitch does not move.

			// PITCH UP IS 2x DECIMATION, as the reference does it -- read twice as fast. It is not
			// Pitch mode's ladder: `pitchRatio_` is, and the two multiply exactly as the reference's
			// `loopPitchIncrement` and `loopPitchRatioFP` do.
			const size_t step = plan_.shouldPitchUp ? inSub * 2 : inSub;

			// **INTERPOLATED, BECAUSE A SCALE RATIO IS NOT A WHOLE NUMBER OF SAMPLES.** The reference
			// says why in as many words: *"linear interpolation for non-integer pitch ratios -- this
			// avoids aliasing/bitcrushing artifacts from truncation."* At a ratio of 1 the fraction is
			// zero and this is the old read, sample for sample, which is what keeps every other mode
			// untouched.
			const double scaled = static_cast<double>(step) * pitchRatio_;
			const auto whole = static_cast<size_t>(scaled);
			const double frac = scaled - static_cast<double>(whole);
			// **WRAPPED AT THE SUB-SLICE, NOT CLAMPED AT THE SLOT.** A read faster than one — pitch-up's
			// 2x, or a Pitch-mode ratio above unison — runs off the end of its material before the slot
			// is over. This clamped it to the last frame, so the rest of the grain HELD ONE SAMPLE: a DC
			// step with no crossing for the release gate to mute on, and then a hard edge at the audible
			// end. MEASURED in the click sweep: a Repeat grain ending on -0.5311 held for its final
			// ~1200 samples. The reference wraps: `playbackPos += loopPitchIncrement; if (playbackPos >=
			// loopEffectiveSubLen) playbackPos = 0;` — pitch-up plays the slice twice ("internal loop").
			//
			// **AND THE WRAP IS CROSSFADED, which the reference's is not.** A bare wrap jumps from the end
			// of the material back to its start mid-grain — MEASURED worse than the clamp, up to 66x the
			// input's own step. So the loop's period is `readLen - xfade`, and over the first `xfade`
			// frames of each later pass the read blends from the frame the previous pass would have
			// reached next into the frame this pass starts on. The first pass reads exactly as before.
			const size_t readLen = std::max<size_t>(1, subLength > 0 ? std::min(subLength, slotLength) : slotLength);
			const auto readOne = [&](double pos, float* outL, float* outR) {
				const auto w = static_cast<size_t>(pos);
				const size_t within = std::min(w, readLen - 1);
				const size_t nextFrame = (within + 1) < readLen ? within + 1 : within;
				const size_t at = plan_.shouldReverse ? (schedule_.startOffset + slotLength - 1 - within)
				                                      : (schedule_.startOffset + within);
				const size_t atNext = plan_.shouldReverse
				                          ? (schedule_.startOffset + slotLength - 1 - nextFrame)
				                          : (schedule_.startOffset + nextFrame);
				auditRead(at); // backlog HN
				if (traced != nullptr) {
					traced->at = static_cast<long long>(at); // the last primary read of the sample is its main one
				}
				ring_.readAt(at, outL, outR);
				mainAt = static_cast<long long>(at); // the last primary read is the main one, as for the trace
				if (traced != nullptr) {
					traced->heardL = *outL;
				}
				const double fr = pos - static_cast<double>(w);
				if (fr > 0.0) {
					float l1 = 0.0f;
					float r1 = 0.0f;
					auditRead(atNext); // backlog HN
					ring_.readAt(atNext, &l1, &r1);
					*outL += (l1 - *outL) * static_cast<float>(fr);
					*outR += (r1 - *outR) * static_cast<float>(fr);
				}
			};
			// **THE CROSSFADE IS SO MANY OUTPUT FRAMES, NOT SO MANY READ FRAMES (2026-09-23).** It is counted in
			// read positions, and a read at 4x (Pitch's octave with pitch-up) crossed 64 of them in 16 output
			// samples — a blend between two unrelated phases that fast is a step of its own. MEASURED on a
			// 220 Hz sine in Pitch at 16 slices: 6.08x the input's step, at exactly the wrap, once the repeat
			// hold made that plan meet that ratio. Scaled by the read's speed it lasts the same time at any.
			const double readSpeed = pitchRatio_ * (plan_.shouldPitchUp ? 2.0 : 1.0);
			const size_t xfade = std::min(
			    static_cast<size_t>(static_cast<double>(pWriteFadeFrames_) * std::max(1.0, readSpeed)), readLen / 4);
			const auto period = static_cast<double>(readLen - xfade);
			(void)whole;
			(void)frac;
			// **AND ONLY A READ THAT DOES WRAP CROSSFADES (groove-click, 2026-09-23).** The crossfade lives
			// in the last `xfade` frames of the FIRST pass, blending toward the start of the second — so a
			// read that ends before `readLen` blended toward a pass that never plays. At the defaults (unison,
			// no ratchet, the whole slot) that is every slot: its last 64 frames faded into a copy of its
			// own first 64, and the next slot then started from where the read really was. MEASURED on a
			// 220 Hz sine at 0.5: a 0.45 step at every slot start, both hosts, both lock settings, Straight
			// included. some glitch in the audio, very subtle on attack — a faint pre-echo of
			// each slice's attack, just before the next one's.
			//
			// THE REGION IS WHAT THIS SUB-SLICE READS: its own length, the last one through the slot's end
			// (the read runs on past `audible` for `P Write`), except in a slot the groove lengthened, whose
			// read past its material is silence and is written at a zero ramp.
			const size_t regionEnd =
			    subLength == 0 ? slotLength
			                   : (grooveGated_ ? subEnd
			                                   : (lastSub ? slotLength - std::min(slotLength, whichSub * subLength)
			                                              : subLength));
			const double lastStep =
			    static_cast<double>(regionEnd > 0 ? regionEnd - 1 : 0) * (plan_.shouldPitchUp ? 2.0 : 1.0);
			const bool wraps = lastStep * pitchRatio_ > static_cast<double>(readLen - 1);
			if (!wraps || scaled < period || xfade == 0) {
				readOne(scaled < static_cast<double>(readLen) ? scaled : std::fmod(scaled, static_cast<double>(readLen)), &l, &r);
			}
			else {
				const double q = std::fmod(scaled, period);
				if (q < static_cast<double>(xfade)) {
					float tailL = 0.0f;
					float tailR = 0.0f;
					readOne(q + period, &tailL, &tailR);
					readOne(q, &l, &r);
					const auto t = static_cast<float>(q / static_cast<double>(xfade));
					l = tailL + (l - tailL) * t;
					r = tailR + (r - tailR) * t;
				}
				else {
					readOne(q, &l, &r);
				}
			}
		}

		// **THE SOURCE BEFORE THE ENVELOPE, which is what pWrite writes.** The reference keeps
		// `srcL`/`srcR` for exactly this and calls them *"Pre-envelope source for crossfaded pWrite"*:
		// writing the enveloped sample would fade the material a little more on every pass, so a loop
		// left running would quietly sink towards silence.
		const float srcL = l;
		const float srcR = r;

		// **THE CROSSING DUCK (backlog JC, 2026-09-24).** With a continuous writer on, the
		// ring is two recordings joined at the write head: behind it the input of moments ago, ahead of it
		// the input of a bar ago. A read at the head's own speed never meets the join — the looper reads its
		// slot AT the head, before the write — but a read at any other velocity passes through it, and the
		// join is a cut between two unrelated signals. MEASURED on a 113.3 Hz sine at Dry Write 1, after the
		// dry write stopped being deferred: 17/8/4/40 clicks (Time, Shuffle, Pattern, Pitch) at the puck's
		// rest, each one a read passing `linearBarPos_`, up to 129x the input's own step. So the wet dips to
		// nothing AT the join and is back to unity `crossingDuckFrames_` output samples either side of it.
		//
		// **THE WIDTH IS IN OUTPUT TIME, SO IT IS COUNTED IN READ FRAMES AT THE READ'S SPEED RELATIVE TO THE
		// HEAD** — `|v|` frames closer per sample, where `v` is the read's velocity minus the head's 1. A
		// reversed read counts too: at 1x it closes on the head at 2 frames a sample, and crosses it once a
		// pass. A frozen ring has no join, so nothing here runs without a writer.
		//
		// **AND THE GAIN IS SLEWED, AT THE SAME ONE-OVER-WIDTH A SAMPLE THE APPROACH ITSELF MOVES AT.** A
		// read that closes on the head descends the ramp exactly that fast, so the slew costs a crossing
		// nothing; what it catches is the read LEAVING the head's neighbourhood by a jump — a wrap back to
		// the start of its material a few frames short of the head. MEASURED with the gain a pure function
		// of distance: the wet sprang from 0.02 to 1 in one sample there, 13-38x the input's step, in the
		// clicks that were left.
		{
			const double speed = pitchRatio_ * (plan_.shouldPitchUp ? 2.0 : 1.0);
			const double v = (plan_.shouldReverse ? -speed : speed) - 1.0;
			const float g = slewCrossingDuck(&crossingDuck_, mainAt >= 0 ? crossingDuckTarget(mainAt, v) : 1.0f);
			if (mainAt >= 0) {
				l *= g;
				r *= g;
			}
			if (traced != nullptr) {
				traced->duck = g;
			}
		}

		// **THE ENVELOPE APPLIES TO A DRY SLICE TOO** -- `skipEnvForDry` is the only exemption and it
		// requires the audio to flow BOTH ways, which is the reference's rule to the letter. A dry
		// slice between two grains is a transition and needs the fade as much as a grain does.
		if (!skipEnvForDry) {
			if (positionInSlice_ >= audible) {
				// PAST THE AUDIBLE WINDOW: `lengthMult` is a rhythm, so the rest of the slot is silent
				// -- for the dry source as well, or a thinned stream would have its holes filled in by
				// the very input it is supposed to be interrupting.
				//
				// **EXCEPT A DRY SLICE RUNNING INTO A DRY ONE**, which passes the input on (see
				// `tailSilenced_`). It must not fall through to the gate envelope either: that envelope is
				// `subLength` long and reads zero past it, which silenced the very tail this exemption keeps
				// — MEASURED as the largest class left in the click sweep, 49 jumps.
				//
				// AND THE GAP BEFORE A SWUNG HIT (`grooveGated_`), which is silence and not the next hit.
				if (tailSilenced_ || grooveGated_) {
					l = 0.0f;
					r = 0.0f;
				}
			}
			else {
				// **AND A DRY SLICE THAT JOINS DRY ON EITHER SIDE IS NOT SHAPED AT ALL.** `skipEnvForDry` covers a
				// dry slice with dry on BOTH sides; with dry on one side only, the shape's value at that edge met
				// the unshaped input across the join. Two steps, MEASURED once the repeat hold made dry runs
				// common (2026-09-23), on a 220 Hz sine in Shuffle and Pattern:
				//   - the tail: the shape decays toward the audible end, and past it the branch above passes the
				//     input on, so the output stepped from the shape's tail back to the full input
				//     (0.666 → 0.981, 10.9x the input's step);
				//   - the head: after a dry slice, the shape starts at `1 − envDepth`, so the output stepped
				//     from the full input down to it (−0.861 → −0.591 at depth 0.32, 9.4x).
				// The side that meets a grain is still guarded, by the crossing gate and the grain fade, which is
				// the edge a dry slice between two grains needs. A dry slice between two grains keeps its shape.
				const bool dryJoinsDry = sliceIsDry_ && (consecutive_ || (!tailSilenced_ && nextConsecutive_));
				const float env = dryJoinsDry ? 1.0f
				                            : grainEnvelope(static_cast<int32_t>(inSub),
				                                            static_cast<int32_t>(subLength > 0 ? subLength : 1),
				                                            gate, plan_.envDepth, plan_.envShape);
				l *= env;
				r *= env;

				// **CROSSFEED PAN, the reference's algebra exactly**: at pan=1, L is silent and R
				// carries (L+R)/2; the simplification is `R + (L-R)*cross` rather than two
				// multiplies. Not on a dry slice — *"input audio should pass through unchanged"*.
				if (panActive_ && !sliceIsDry_) {
					if (panRight_) {
						const float cross = (l - r) * panCross_;
						l = l * panFade_;
						r = r + cross;
					}
					else {
						const float cross = (r - l) * panCross_;
						r = r * panFade_;
						l = l + cross;
					}
				}

				// STEREO SPREAD: the plan's width leans the two channels apart. At zero this is
				// identity. NOT ON A DRY SLICE -- the reference gates its pan on `!useDry` for the
				// same reason: widening the track itself is not what this control is for.
				if (!sliceIsDry_ && plan_.stereoWidth != 0.0f) {
					const float w = plan_.stereoWidth * widthNow_;
					const float mid = 0.5f * (l + r);
					l = mid + (l - mid) * (1.0f + w);
					r = mid + (r - mid) * (1.0f - w);
				}
			}
		}

		if (zcActive_ && !skipEnvForDry) {
			// A CROSSING IS A SIGN CHANGE BETWEEN TWO SAMPLES, so it cannot be read off one. The
			// reference spells it `(prev != 0) && ((out ^ prev) < 0)` on q31 — a sign-bit xor. In
			// float that is a product below zero, with the same "and the previous was not zero"
			// guard, because silence either side is not a crossing to start from.
			const bool zcL = (prevOutputL_ != 0.0f) && (l * prevOutputL_ < 0.0f);
			const bool zcR = (prevOutputR_ != 0.0f) && (r * prevOutputR_ < 0.0f);
			prevOutputL_ = l;
			prevOutputR_ = r;

			// ATTACK: silent until this channel's first crossing.
			if (waitingForZeroCrossL_) {
				if (zcL) {
					waitingForZeroCrossL_ = false;
				}
				else {
					l = 0.0f;
				}
			}
			if (waitingForZeroCrossR_) {
				if (zcR) {
					waitingForZeroCrossR_ = false;
				}
				else {
					r = 0.0f;
				}
			}

			// RELEASE: inside the last ~15 ms of the grain, mute at the next crossing rather than at
			// the slot's edge. **AND AT THE BAR'S END TOO**, which is a separate window in the
			// reference (`kBarEndZone`, ~50 ms) covering the seam where the whole loop wraps — the
			// one click that is there even when every grain inside the bar is consecutive.
			const size_t inGrain = positionInSlice_ < audible ? positionInSlice_ : audible;
			const bool inRelease =
			    ((tailSilenced_ || !nextConsecutive_) && audible > releaseZone_ && inGrain > audible - releaseZone_)
			    || (barEndSilence_ && barLength > barEndZone_ && barPos > barLength - barEndZone_);
			if (inRelease) {
				if (!releaseMutedL_ && zcL) {
					releaseMutedL_ = true;
				}
				if (!releaseMutedR_ && zcR) {
					releaseMutedR_ = true;
				}
			}
			if (releaseMutedL_) {
				l = 0.0f;
			}
			if (releaseMutedR_) {
				r = 0.0f;
			}
		}
		else {
			// **THE PREVIOUS SAMPLE IS TRACKED WHEN THE GATE IS OFF TOO.** A crossing is read against it, and
			// it was only written inside the gate — so the first gated sample after a run without the gate
			// compared against a value from long ago, saw a sign change that was not there, and muted a
			// live sample. MEASURED: seven release mutes landing on 0.56 at a slice start in the sweep.
			prevOutputL_ = l;
			prevOutputR_ = r;
		}


		// **pWRITE IS WHAT LETS THE LOOP EVOLVE, AND IT IS A TAPE LOOP.** At 0 the buffer is frozen
		// and the bar repeats for ever; at 1 every slice writes and the incoming audio replaces it.
		// The reference's default is 0, and that is a real choice: a looper that overwrites as fast
		// as it reads has nothing left to scatter.
		//
		// **READ FROM THE SHUFFLED POSITION, WRITE TO THE LINEAR ONE** — the reference's own sentence,
		// and the thing this port had wrong. Writing back where it read makes a feedback path; writing
		// along the bar makes a tape that the shuffle then reads out of order.
		//
		// CROSSFADED BY THE GRAIN'S RAMP, not by the control: the control already decided WHETHER this
		// slice writes, and the ramp is what keeps the written edge from being a step.
		//
		// **THE RAMP, NOT THE GATE ENVELOPE — the reference's `pWriteEnvQ31` in full:** it starts at
		// full, is overwritten by the attack or decay ramp inside those regions, and is ZERO past
		// `loopGatedLen`. The gate envelope's depth and shape shape the OUTPUT only. Ours wrote with the
		// gate envelope alone, which is wrong twice: it has no ramp, so written regions butted older
		// material with hard seams (`pWrite under read` topped the click sweep in five of six modes);
		// and it starts each slice at a different level from where the last one ended, so even a
		// consecutive join stepped — MEASURED at 0.696 against 1.0 across one join.
		//
		// **THE DECISION ITSELF IS SLEWED, over the firmware's `kPWriteFade` (64 samples at 44.1 kHz)**,
		// and "past the audible end" is part of the decision, so the reference's hard zero at the gate
		// becomes a 64-sample release rather than a seam where `lengthMult` cuts a grain short.
		//
		// **WHY A SLEW AND NOT THE REFERENCE'S FIXED FADE, measured both ways.** The reference skips its
		// fade between CONSECUTIVE slices, but consecutive is a fact about the READ, and whether a slice
		// WRITES is a separate draw (and a duck). With the guards copied, a writing slice after a
		// non-writing consecutive one starts at full weight against old material: the seam case read
		// steps of exactly 1.0 in four modes. Without the guards, two consecutive slices that BOTH write
		// dip to zero at their join — a probe case that measured 150+ changed frames read 127. A slew on
		// the decision does neither: it holds at one across a join where both write, and ramps wherever
		// the decision changes, which is the only place a consecutive join has an edge.
		// `skipEnvForDry` is the reference's whole-envelope exemption: dry flowing into dry writes the
		// input unbroken, past the gate included.
		const bool writing = pWriteThisSlice_ && (!tailSilenced_ || positionInSlice_ < audible);
		const float writeTarget = writing ? 1.0f : 0.0f;
		const float writeStep = pWriteFadeFrames_ > 0 ? 1.0f / static_cast<float>(pWriteFadeFrames_) : 1.0f;
		writeGain_ = writeGain_ < writeTarget ? std::min(writeTarget, writeGain_ + writeStep)
		                                      : std::max(writeTarget, writeGain_ - writeStep);
		if (writeGain_ > 0.0f) {
			// The RAMP is the grain's own attack and decay, under the firmware's consecutive guards,
			// through the same function the output uses so the two cannot drift.
			const float ramp = fadeGainAt(inSub, leftInGrain, fadeIn, fadeOut);
			const float mix = std::clamp(ramp * writeGain_, 0.0f, 1.0f);
			auditWrite(linearBarPos_);
			// **BACKLOG GU/GV: WHEN THE CONTINUOUS WRITERS ARE LIVE, `P Write` IS RECORDED RATHER THAN
			// PERFORMED — because it has to be LAST.** The order is dry, feedback, `P Write`, and
			// the feedback source does not exist until the lanes have run; performing this one here
			// would make it first. Recording it costs the rest of the trio nothing and keeps the
			// composition exactly as designed.
			if (writersLive_) {
				recordRingPWrite(i, srcL, srcR, mix);
			}
			else if (deferWrites_) { // backlog HN, test-only
				recordDeferredWrite(linearBarPos_, srcL, srcR, mix);
			}
			else {
				float oldL = 0.0f;
				float oldR = 0.0f;
				ring_.readAt(linearBarPos_, &oldL, &oldR);
				ring_.setWriteGain(freezeGainAt(static_cast<size_t>(i))); // backlog KX
				writeLoop(linearBarPos_, oldL + (srcL - oldL) * mix, oldR + (srcR - oldR) * mix);
			}
		}

		// **AND THE HEAD ITSELF IS RECORDED FOR EVERY SAMPLE, not only the writing ones** — the two
		// continuous writers write wherever the head is, which is the whole of what "continuous"
		// means here, and `P Write`'s slice draw has nothing to do with them (backlog GU/GV: they
		// blend, they do not interact).
		if (writersLive_) {
			// **AUDITED HERE TOO, AND ADDING THIS LINE IS WHAT MADE THE LOOPER'S HAZARD FIGURE MEAN
			// ANYTHING (backlog HU).** The audit's only `auditWrite` on this path used to sit inside
			// the `P Write` branch — so a run with `P Write` at 0 marked NO writes, and its hazard
			// count came back 0 of 582,720 for the arithmetic reason that nothing could be a hit.
			// A zero from an instrument that cannot register anything is the vacuous form of exactly
			// the claim this counter exists to make.
			auditWrite(linearBarPos_);
			recordRingWrite(i, linearBarPos_);
			ring_.setWriteGain(freezeGainAt(static_cast<size_t>(i))); // backlog KX
			writeDryNow(linearBarPos_, captureL_[i], captureR_[i]);
		}

		// THE WRITE HEAD WALKS THE BAR, once per sample, whatever the read head is doing.
		if (bufferFrames_ > 0 && ++linearBarPos_ >= bufferFrames_) {
			linearBarPos_ = 0;
		}

		// **THE GRAIN LANE'S TAP, AND IT IS THE PURE GRAIN — before the echo send and before the echo
		// return** (backlog EW; tap (a), chosen 2026-09-20 on parity grounds). This is the
		// same `l`/`r` the echo send below takes, which is the point: the two destinations are fed the
		// same thing and differ only in where it goes. A tap after the return would put the echo inside
		// the lane as well, which is a different effect and not the one the precedent describes.
		//
		// **A DIVERSION, NOT A PARALLEL SEND — the grain LEAVES the wet.** Taken here and subtracted at
		// the write below, so an EMPTY lane at unity is exactly transparent: the grain goes out, passes
		// through nothing, and comes back at the same level.
		//
		// **A DELIBERATE DECISION, AGAINST BACKLOG EW's OWN WORDING** — the entry says "exactly as `shouldDelay` picks which go to the internal delay",
		// and that delay is a parallel send. `renderChunk` carries the full account of why the same
		// mechanism gives a different answer here; the short form is that an echo returns quieter and
		// later while a chain at unity returns what it was given, so adding it back would make every
		// chosen grain 6 dB louder for nothing.
		const float sentL = grainSend_ > 0.0f ? l : 0.0f;
		const float sentR = grainSend_ > 0.0f ? r : 0.0f;

		// **AND A DIVERTED GRAIN STILL FEEDS THE ECHO AT FULL LEVEL — AND IT IS DELIBERATE RATHER
		// THAN AN OVERSIGHT IN THE SUBTRACTION.** `delay_.write` below is
		// handed `l`, which is still the WHOLE grain; `sentL` is subtracted from the WET alone, three
		// statements further down. So a grain that took the lane is heard by two routes — processed,
		// through the lane, and unprocessed, as an echo — and the echo of a diverted grain returns to
		// the wet rather than to the lane.
		//
		// The question was whether the send to grain fx sits after the integrated effects such as the
		// built-in delay; parallel is right, because those are sends with tails. **THE TAIL IS THE WHOLE
		// ARGUMENT.** The echo is a SEND, and a
		// send has a tail that outlives the decision that fed it: the per-slice draw which diverted
		// this grain is over long before its echo has finished sounding. Subtracting `sentL` here
		// would make a tail already in flight vanish because of a coin flip in its past — the echo
		// would thin and thicken with the grain lane's draw, which is not what a send does.
		//
		// **THE COST IS REAL AND IS THE PRICE OF THAT, so it is written down rather than discovered:**
		// a filter in the grain lane does NOT shape what the echo carries, because the echo was fed
		// upstream of the tap. A player who puts a filter there and listens to the tail hears the
		// unfiltered grain. That is the same shape as this repo's "partial effect survives where a
		// total one would be caught" family, and it is accepted here rather than missed.
		//
		// **THE SEND IS THE DRY GRAIN AND THE FEEDBACK IS HALF THE RETURN, WRITTEN BEFORE THE RETURN
		// IS MIXED IN.** Order matters: mixing first and then writing would feed the echo its own
		// output at full gain and run away. The reference is explicit about it.
		//
		// **AND IT IS RAMPED ON AND OFF, NOT SWITCHED (2026-09-24, backlog JC).** The
		// send turned on at a slice start and off at the next, and both edges were bare: the send's own
		// input stepped from nothing to a live signal (so its echo arrived a quarter bar later with a
		// step in it), and the return stopped mid-tail. A wet slice hides both under its grain fades; a
		// DRY slice has none, and there it was the loudest click left in the device. MEASURED on a
		// 113.3 Hz sine, puck on E at 0.1: an output of exactly 2.0 — the input's 1.0 plus an echo of
		// 1.0 — where it had been 0.997 a sample before, 69x the input's own step. So the send level and
		// the return are both slewed over `minAntiClickFrames_`. The line runs on through the fade-out
		// and freezes when the return reaches zero, which is the reference's freeze a ramp later.
		{
			const bool sending = delaySend_ > 0.0f && delayTime_ > 0;
			if (sending) {
				echoTime_ = delayTime_;
			}
			const float step = minAntiClickFrames_ > 0 ? 1.0f / static_cast<float>(minAntiClickFrames_) : 1.0f;
			const float targetGain = sending ? 1.0f : 0.0f;
			echoGain_ = std::clamp(targetGain, echoGain_ - step, echoGain_ + step);
			echoSend_ = std::clamp(delaySend_, echoSend_ - step, echoSend_ + step);
			if (echoGain_ > 0.0f && echoTime_ > 0) {
				const size_t readAt = delay_.writePos() + delay_.length() - echoTime_;
				float echoL = 0.0f;
				float echoR = 0.0f;
				delay_.readAt(readAt, &echoL, &echoR);
				delay_.write(l * echoSend_ + echoL * 0.5f, r * echoSend_ + echoR * 0.5f);
				l += echoL * echoGain_;
				r += echoR * echoGain_;
			}
		}

		// **THE AUDIBLE END, NOT THE SLOT'S.** `lengthMult` can cut a grain short and hard-zero the rest
		// of the slot; fading toward the SLOT boundary would ramp down through silence and leave the real
		// edge — the one the length gate makes — bare. `audible` is where the sound actually stops.
		// **THE CONSECUTIVE GUARDS ARE THE FIRMWARE'S** — `!scatterConsecutive` on the attack and
		// `!loopNextConsecutive` on the decay: a grain that flows on from the last one has no edge, and
		// fading it is an audible dip in continuous material rather than a click guard. Computed above,
		// where the write can reach them.
		// **THE GRAIN'S ENVELOPE IS APPLIED TO BOTH HALVES, AND `wetAt` BEING LINEAR IN ITS INPUT IS
		// WHAT MAKES THE SPLIT EXACT**: `wetAt(l − sent) + wetAt(sent)` is `wetAt(l)`, so diverting a
		// grain moves it between the two buses without changing what the device produces in total. The
		// lane therefore receives grains that have been enveloped, which is the same reason the
		// envelope stayed in the renderer when the blend was split.
		wetL_[static_cast<size_t>(i)] = wetAt(l - sentL, inSub, leftInGrain, fadeIn, fadeOut);
		wetR_[static_cast<size_t>(i)] = wetAt(r - sentR, inSub, leftInGrain, fadeIn, fadeOut);
		if (grainSend_ > 0.0f) {
			sendL_[static_cast<size_t>(i)] += wetAt(sentL, inSub, leftInGrain, fadeIn, fadeOut);
			sendR_[static_cast<size_t>(i)] += wetAt(sentR, inSub, leftInGrain, fadeIn, fadeOut);
		}
		mixCoeff_[static_cast<size_t>(i)] = mixAt();
		++envPos_;
		advanceEngageFade();

		if (++positionInSlice_ >= slotLength) {
			if (target_.mode == Mode::Repeat) {
				repeatLoopIndex_ = (repeatLoopIndex_ + 1) & 0x7;
				// **A REPEAT, COUNTED WHERE IT HAPPENS** (backlog HR round 2, repeat slices get
				// the split spark) — the slot just ended and the repeated slice starts again. The
				// period published with it is this slot's own length.
				countRepeat(slotLength);
			}
			// **THE TRANSPORT LOCK REPLACES THE ADVANCE, WHICH IS THE WHOLE OF THE OPTION (backlog
			// GT).** The counter below is what makes the read cursor free-running: it starts at 0 at
			// engage and nothing has ever re-derived it from the song, so it holds phase only until
			// something disturbs it. Under the lock the next slice is DERIVED from the song position at
			// the frame the slot ends on, so the position is a function of the transport rather than a
			// count of how many slots have gone by.
			//
			// **THE ADVANCE AND THE DERIVATION AGREE ON THE SAME GRID, AND THAT IS WHY THIS IS A
			// RE-PHASING RATHER THAN A RHYTHM CHANGE.** `scheduleFor` tiles the bar exactly —
			// `sliceLength = baseSliceLength * grainLength`, with the last grain of the bar swallowing
			// the integer division's remainder — so a slot end under a steadily-advancing transport
			// falls on the base-grid boundary the derivation names.
			//
			// **BUT DO NOT READ THAT AS "TURNING IT ON CHANGES NOTHING UNTIL THE HOST DOES SOMETHING",
			// WHICH IS WHAT THIS COMMENT SAID UNTIL IT WAS MEASURED.** `MATRIX: every control in every
			// mode` reported the control MOVING in all five slice modes with a transport that never
			// stops. What it was hearing is the ENGAGE POINT: the gesture branch sets `sliceIndex_ = 0`,
			// and under the lock the press is re-phased to the song instead. That is the feature, and
			// `backlog GT: MEASURE — what the lock changes in an UNDISTURBED pass` is where the two
			// halves are separated by value rather than argued here.
			//
			// **SINCE GU (2026-09-22) THE PRESS IS PHASED IN BOTH SETTINGS**, so the engage point no longer
			// separates them. The matrix went on reading the control MOVING in the five slice modes on a
			// transport that never stops, and that was a DEFECT, found by value on 2026-09-22: this
			// derivation truncated, so a song position one ulp low at the slot end named the outgoing
			// slice and the lock ran a whole slice late (see `lockedCursor`, which now rounds). With it
			// fixed an unbroken transport is bit-identical in both settings — the matrix carries that as
			// a by-name exception, and `backlog GT: a hair either side of a slot end` holds it by value.
			//
			// **THE BAR COUNTER GOES WITH IT WHEN THERE IS AN ANCHOR TO COUNT FROM**, and keeps its own
			// count when there is not — see `lockedBarIndex`. The phase inside the bar is the
			// transport's either way.
			int32_t lockedSlice = 0;
			size_t lockedWithin = 0;
			const bool locked = transportLockActive()
			                    && lockedCursor(static_cast<double>(windowBase_ + i + 1), &lockedSlice,
			                                    &lockedWithin);
			if (locked) {
				const int32_t lockedBar = lockedBarIndex(static_cast<double>(windowBase_ + i + 1));
				if (lockedBar >= 0) {
					barIndex_ = lockedBar;
				}
				else if (lockedSlice < sliceIndex_) {
					// **NO ANCHOR, SO THE BAR COUNTER IS STILL THE FREE-RUNNING ONE — and it still has to
					// step somewhere.** A derived slice that went BACKWARDS is the bar coming round, which
					// is the same event the counter below steps on. Read off the two indices rather than
					// off the schedule, because `schedule_` here is still the OUTGOING slice's.
					barIndex_ = (barIndex_ + 1) % kBarIndexWrap;
				}
				sliceIndex_ = lockedSlice;
				// THE SLOT BEGINS ON THE BOUNDARY THE LOCK DERIVED (backlog IG) — absolute, so the bar's top
				// is also where a groove's anchor restarts.
				slotStartFrame_ = grooveBoundary(target_.grooveFeel(), lockedSlice, numSlices_, sliceFrames_);
				if (lockedSlice == 0) {
					grooveAnchored_ = false;
				}
				// **AND THE WRITE HEAD WITH IT (backlog GU)** — one road, the same instant; see
				// `lockedWriteHead`. It has already stepped past this sample, so it asks about `i + 1`.
				size_t head = 0;
				if (lockedWriteHead(static_cast<double>(windowBase_ + i + 1), &head)) {
					linearBarPos_ = head;
				}
			}
			else {
				sliceIndex_ += schedule_.grainLength;
				slotStartFrame_ += slotLength; // backlog IG: where the next slot really begins
			}
			if (!locked && sliceIndex_ >= numSlices_) {
				sliceIndex_ = 0;
				slotStartFrame_ = 0;
				grooveAnchored_ = false;
				// **THE BAR COUNTER, WHICH NOTHING ADVANCED.** `PhaseOffsets::barIndex` was declared,
				// read in two places and NEVER WRITTEN — it sat at 0 for the life of the device, so
				// `planFor`'s whole `if (ofs.barIndex != 0)` branch was unreachable and multi-bar
				// evolution did not exist. The reference steps it here, at the wrap, modulo
				// `kBarIndexWrap`.
				barIndex_ = (barIndex_ + 1) % kBarIndexWrap;
			}
			advanceSlice();
		}
	}
}

uint32_t Sklitter::nextRandom() noexcept {
	grainRng_ ^= grainRng_ << 13;
	grainRng_ ^= grainRng_ >> 17;
	grainRng_ ^= grainRng_ << 5;
	return grainRng_;
}

//--------------------------------------------------------------------------------------------------//
// GRAIN: a cloud, not a sequence.                                                                    //
//--------------------------------------------------------------------------------------------------//
//
// **THE OTHER FIVE LOOPER MODES WALK THE BAR; THIS ONE SCATTERS OVER IT.** Two voices read short
// grains from positions chosen at random inside `spread`, each windowed by a triangle, and they run
// HALF A TURN APART -- so as one window closes the other is wide open and the cloud never gaps. That
// phase offset is the whole of the crossfade; there is no separate fade.
//
// Rate means a third thing here: the GRAIN LENGTH, squared so the short end has the resolution, with
// a ~23 ms floor because micro-grains below it are harsh rather than granular. Zone A is the spread:
// at zero both voices sit at the top of the bar, at one they range over all of it.

/// **ONE GRAIN'S DECISIONS, DRAWN THE WAY EVERY OTHER MODE DRAWS THEIRS.**
///
/// **THIS IS A DELIBERATE DIVERGENCE FROM THE FIRMWARE AND IT WAS CALLED FOR.** Its grain path draws
/// reverse and pitch from a free-running LFSR against a bare scalar —
/// `grainAReversed = (fastRandom() & 0xFFFF) / 65535.0f < zoneBNorm * 0.4f` — with no phi triangle,
/// no macro, and no reproducibility. The grain path should support the macro and compare deterministic
/// draws against a phi-triangle threshold. That is exactly what the SLICE path does (`planFor`: a phi triangle for each
/// probability, macro pushing it up or down through macro config's own bipolar triangles, then
/// `hashCtx.evalBool` keyed on the index) and the grain path is the one place it was not done.
///
/// **SO A GRAIN ASKS `planFor` WITH ITS OWN INDEX, exactly as a slice asks with its slice index.**
/// Three things follow that the LFSR could not give: the draws are DETERMINISTIC, so the same grain
/// under the same controls decides the same way; MACRO, MACRO CONFIG and GAMMA reach Grain by
/// construction rather than by a second road anyone has to remember to add; and there is one
/// implementation of every threshold instead of two to drift apart.
///
/// The project's policy sanctions this: firmware parity is a per-feature choice, and plugin-first
/// character evolution is legitimate.
///
/// **CHEAP ENOUGH TO DO PER GRAIN.** A grain is `kMinGrain` samples at the shortest — 1024, about 47 a
/// second per voice at 48 kHz — against `advanceSlice` calling the same function eight or more times a
/// bar. It has its own `PlanCache` so it cannot evict the playing slice's entry.
SlicePlan Sklitter::grainPlanFor(int32_t grainIndex) {
	const Coordinate a = splitCoordinate(target_.zoneA);
	const Coordinate b = splitCoordinate(target_.zoneB);
	const Coordinate& mc = kNoKnobConfig; // no knob configuration since 2026-09-19; see the constant
	// The one builder — the macro's phase push included. See `offsetsFor`.
	//
	// **THE PERFORM CIRCLE REACHES GRAIN BY THE SAME ROAD AS EVERY OTHER MODE, AND THAT IS THE POINT OF
	// THE FUNCTION ABOVE.** Grain draws its own plan per grain rather than per slice, which is exactly the
	// second road this device has paid for four times — a fix that reached the slice path and not this
	// one. `performPushOf` is the only builder of a push in this file, so there is no version of that
	// mistake left to make here.
	const MacroPush push = targetPush();
	// **THE OVERRIDE IS HANDED IN HERE TOO, THOUGH GRAIN HAS NO DELAY SEND TO OVERRIDE.** `playGrain`
	// never calls `advanceSlice`, which is the only reader of `shouldDelay`, so this changes no sound in
	// this mode — and the control is gated out of Grain on the card for exactly that reason. It is
	// passed anyway because a plan built by a different road from the playing slice's is the second-road
	// defect this device has paid for four times; withholding one argument here would be a fifth.
	const PhaseOffsets ofs =
	    offsetsFor(a, b, mc, push, target_.gamma, seqBarIndex(), false, target_.sendOverride);
	return planFor(a.position, b.position, mc.position, push, grainIndex, ofs, &grainPlanCache_);
}

/// **DENSITY FOR ONE GRAIN, DETERMINISTIC ON THE GRAIN INDEX** — the same shape as
/// `densityWantsDry`'s per-slice hash, for the same reason given about the other draws. The
/// firmware draws this one freely too; a free draw means two runs of the same material thin the cloud
/// differently, which is not something a performance effect should do to a take.
bool Sklitter::grainDrawIsDry(int32_t grainIndex) {
	const float density = std::clamp(target_.density, 0.0f, 1.0f);
	if (density >= 1.0f) {
		return false; // full density is every grain, with no draw to make
	}
	const uint32_t h = hash::mix(static_cast<uint32_t>(grainIndex) ^ 0xBADC0FFEu);
	const float draw = static_cast<float>(h & 0xFFFFu) / 65535.0f;
	return draw >= density;
}

/// **pWRITE IN GRAIN IS A PER-GRAIN DRAW, NOT A LEVEL.** The firmware, at voice A's wrap:
///
///     grainAWritesWet = (static_cast<float>(fastRandom() & 0xFFFF) / 65535.0f) < pWriteProbGrain;
///
/// and the write itself then mixes at `envA` alone — pWrite never scales the sample. Ours scaled it,
/// which made pWrite a continuous overdub depth: a different control. Hashed on the grain index for
/// the same reason as `grainDrawIsDry`, with its own salt so the two draws do not move together.
bool Sklitter::grainDrawWrites(int32_t grainIndex) const {
	const float p = std::clamp(target_.pWrite, 0.0f, 1.0f);
	const uint32_t h = hash::mix(static_cast<uint32_t>(grainIndex) ^ 0x5EEDF00Du);
	const float draw = static_cast<float>(h & 0xFFFFu) / 65535.0f;
	return draw < p;
}

/// **RATE AND ZONE A, RE-READ AT A GRAIN BOUNDARY.** In grain mode, none of the params seem to
/// change the sound if they are changed while in enabled mode.
///
/// They were captured when the mode was ARMED and never read again, so the cloud's length and spread
/// froze at whatever they were then. The firmware recomputes both at slice setup and refreshes its
/// zone params on a throttle, so they move under the hand there.
///
/// **AT THE BOUNDARY RATHER THAN PER SAMPLE, and that is the whole reason this is a function.**
/// `offsetA` is `phase * grainLength_`, so changing the length mid-grain moves the read head
/// discontinuously — a click, in the mode whose whole job is to be smooth. Latching it where the
/// phase wraps means a turn of the knob lands between grains and is inaudible as an edit.
///
/// **PHASES AND POSITIONS ARE NOT TOUCHED** — this is not `armGrain`. Re-arming here would re-zero the
/// cloud every grain, which is the opposite of a cloud.
void Sklitter::refreshGrainGeometry() {
	if (sliceFrames_ == 0) {
		return;
	}
	constexpr size_t kMinGrain = 1024;
	const double small = std::clamp(1.0 - static_cast<double>(target_.rate), 0.0, 1.0);
	grainLength_ = std::clamp(static_cast<size_t>(static_cast<double>(sliceFrames_) * small * small),
	                          kMinGrain, std::max<size_t>(sliceFrames_, kMinGrain));
	const Coordinate az = splitCoordinate(target_.zoneA);
	grainSpread_ = static_cast<size_t>(az.position * static_cast<double>(resolveBufferFrames()));
}

void Sklitter::playGrain(float* left, float* right, int from, int frames) {
	if (sliceFrames_ == 0) {
		return;
	}
	// **RE-READ EACH TURN, NOT ONCE PER BLOCK** — `grainLength_` is refreshed at voice A's wrap below,
	// so this has to be inside the loop or a change would wait for the next block boundary.
	double inc = 1.0 / static_cast<double>(std::max<size_t>(grainLength_, 1));

	for (int i = from; i < frames; ++i) {
		sampleControls(i); // Mix and Width at this sample (backlog FK)
		// A TRIANGLE WINDOW, rising for the first half turn and falling for the second.
		const auto window = [](double phase) {
			return phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
		};
		inc = 1.0 / static_cast<double>(std::max<size_t>(grainLength_, 1));
		const auto envA = static_cast<float>(window(grainPhaseA_));
		const auto envB = static_cast<float>(window(grainPhaseB_));

		// **REVERSE AND PITCH, PER VOICE, ON THE READ OFFSET** — the firmware's lines, in its own
		// arithmetic (stutterer.cpp:1209-1211, 1311, 1319-1323):
		//
		//     size_t effectiveOffsetA = grainAReversed
		//         ? (grainLength > grainOffsetA ? grainLength - 1 - grainOffsetA : 0)
		//         : grainOffsetA;
		//     grainOffsetA += grainAPitchUp ? 2 : 1;
		//     // Advance envelope phases (per-voice: double for pitch up to maintain grain length)
		//     grainPhaseA += grainAPitchUp ? (basePhaseInc * 2) : basePhaseInc;
		//
		// Pitch-up is DECIMATION — every other sample, an octave — which is the same mechanism the slice
		// renderer uses for `shouldPitchUp`, so the two modes agree about what an octave is. Reverse
		// mirrors within the grain rather than reading backwards from the position, so a reversed grain
		// occupies exactly the same span of buffer as a forward one.
		//
		// **THE OFFSET IS THE PHASE, SO A PITCHED VOICE'S PHASE RUNS TWICE AS FAST (2026-09-24, backlog
		// JC).** The port took the offset line and dropped the phase line: the offset doubled while the
		// window ran at 1x, so the read reached the grain's last frame at phase 0.5 — the window's PEAK —
		// and HELD it for the second half. MEASURED: exactly 50% of every pitched-up voice-sample was one
		// repeated frame, a DC ramp down the window's falling side. funny artifacts on repitch
		// grains. With the phase doubled the grain covers its `grainLength_` frames in half the samples,
		// reaching its end as the window closes.
		//
		// **AND THE OFFSET IS THE FIRMWARE'S INTEGER COUNTER, NOT DERIVED FROM THE PHASE.** It was
		// `phase * grainLength_`, which starts a grain one or two frames in whenever the phase carries a
		// remainder over the wrap, and jumps under voice B when A's wrap re-reads the length mid-B-grain.
		// Counted, it starts every grain at frame 0 and steps by exactly 1 or 2. The clamp to the grain's
		// last frame stays for the one case the firmware does not have: a length that shrank under B.
		const size_t spanA = std::min(grainOffsetA_, grainLength_ > 0 ? grainLength_ - 1 : 0);
		const size_t spanB = std::min(grainOffsetB_, grainLength_ > 0 ? grainLength_ - 1 : 0);
		const size_t offsetA = grainRevA_ ? (grainLength_ > spanA ? grainLength_ - 1 - spanA : 0) : spanA;
		const size_t offsetB = grainRevB_ ? (grainLength_ > spanB ? grainLength_ - 1 - spanB : 0) : spanB;

		float al = 0.0f;
		float ar = 0.0f;
		float bl = 0.0f;
		float br = 0.0f;
		auditRead(grainPosA_ + offsetA); // backlog HN
		auditRead(grainPosB_ + offsetB);
		ring_.readAt(grainPosA_ + offsetA, &al, &ar);
		ring_.readAt(grainPosB_ + offsetB, &bl, &br);
		if (grainTrace_ != nullptr && static_cast<size_t>(i) < grainTraceLength_) { // `setGrainTraceForTest`
			grainTrace_[i] = GrainTraceEntry { static_cast<long long>(grainPosA_ + offsetA),
				                               static_cast<long long>(grainPosB_ + offsetB),
				                               grainPosA_,
				                               grainIndexA_,
				                               grainUpA_,
				                               grainUpB_,
				                               grainRevA_,
				                               envA,
				                               envB,
				                               linearBarPos_,
				                               1.0f,
				                               1.0f,
				                               grainDryA_,
				                               grainDryB_ };
		}

		// **A DRY VOICE PLAYS THE INPUT THROUGH ITS OWN ENVELOPE — the firmware's exact rule, and not
		// what I would have guessed.** `stutterer.cpp`:
		//
		//     q31_t srcAL = grainAIsDry ? dryL : bufAL;
		//
		// So the voice is not muted and does not bypass the window: it SWAPS ITS SOURCE, from the
		// captured buffer to the live input, and is still scaled by `envA`. Silencing it instead would
		// have thinned the cloud by amplitude rather than by content, which is a different effect.
		if (grainDryA_) {
			al = left[i];
			ar = right[i];
		}
		if (grainDryB_) {
			bl = left[i];
			br = right[i];
		}

		// **THE STEREO WIDTH SENDS THE TWO VOICES APART, which is what it is FOR in this mode** --
		// `Traits` calls it "Grain mode only" and this is why: with one voice there is nothing to
		// spread. At zero both sit centred and the cloud is mono.
		// **IT RAN BACKWARDS, AND IT JUMPED.** The comment above has always said what this control
		// does; the arithmetic under it did the opposite. At w = 0 the old formula gave voice A the
		// left channel and voice B the right — FULL separation — and at w = 1 it put both at half in
		// both, which is mono. The `if (w == 0.0f)` special case then forced mono at exactly zero, so
		// the control read mono at 0, near-hard-apart a hair above it, and back to mono at 1. The
		// default is 0.5. Found by the EH sweep, by reading the comment against the code.
		//
		// **ZERO IS MONO AND ONE IS APART**, which is the reference's sense too — its
		// `grainAntiWidth` is `1 - |width|`, the CROSS-feed, so no width means full cross. Each voice
		// keeps `0.5 + w/2` of its own side and sends `0.5 - w/2` across: at 0 both voices are half in
		// each channel, at 1 A is wholly left and B wholly right, and nothing jumps in between.
		//
		// **THE MONO CASE IS A SUM AT HALF, NOT A SUM AT FULL.** The old special case added the two
		// voices outright, so crossing zero also changed the level by 6 dB. Two voices summed to mono
		// belong at half each.
		const float w = std::clamp(widthNow_, 0.0f, 1.0f);
		const float own = 0.5f + w * 0.5f;
		const float cross = 0.5f - w * 0.5f;
		// **THE GRAIN LANE IN GRAIN MODE, PER VOICE — and a DIVERSION, exactly as on the looper road**
		// (backlog EW chose the diversion over EW's parallel-send wording on 2026-09-20, and
		// `renderChunk` carries why). A voice that won its draw contributes to the lane INSTEAD of to
		// the output, so an empty lane at unity is transparent rather than 6 dB loud.
		//
		// **THE TWO VOICES ARE GATED SEPARATELY, WHICH IS THE WHOLE POINT.** Each drew its own decision
		// at its own wrap from its own grain index, so a block can have voice A in the lane and voice B
		// out of it — and that is the difference between a lane fed grain by grain and one fed a sum.
		// The four terms are the same four the output is built from, so a diverted voice arrives at the
		// lane with its own envelope, its own width placement, and its own reverse and pitch already
		// applied.
		//
		// **GRAIN MODE HAS NO ECHO**, so the looper's "before or after the return" choice does not
		// arise here. That asymmetry is structural, not a decision, and is not something to make match.
		//
		// **AND EACH VOICE IS DUCKED WHERE IT CROSSES THE LIVE WRITE HEAD** — the looper's crossing duck
		// (`playLooper` carries why), per voice, on the voice's own velocity: 1 or 2 frames a sample, negative
		// reversed, against the head's 1. A forward voice at 1x never crosses; a pitched or reversed one
		// does. MEASURED, Dry Write 1, 113.3 Hz sine, the sweep's zones and rates: 23 / 103 / 124 clicks at
		// the puck's rest / on E at 0.4 / half way to E at 0.1 without it, 0 / 1 / 0 with it.
		const auto voiceVelocity = [](bool up, bool rev) {
			const double s = up ? 2.0 : 1.0;
			return (rev ? -s : s) - 1.0;
		};
		const float duckA = slewCrossingDuck(
		    &crossingDuckA_, grainDryA_ ? 1.0f
		                                : crossingDuckTarget(static_cast<long long>(grainPosA_ + offsetA),
		                                                     voiceVelocity(grainUpA_, grainRevA_)));
		const float duckB = slewCrossingDuck(
		    &crossingDuckB_, grainDryB_ ? 1.0f
		                                : crossingDuckTarget(static_cast<long long>(grainPosB_ + offsetB),
		                                                     voiceVelocity(grainUpB_, grainRevB_)));
		if (grainTrace_ != nullptr && static_cast<size_t>(i) < grainTraceLength_) {
			grainTrace_[i].duckA = duckA;
			grainTrace_[i].duckB = duckB;
		}
		const float aOwn = al * envA * duckA * own;
		const float aCross = ar * envA * duckA * cross;
		const float bCross = bl * envB * duckB * cross;
		const float bOwn = br * envB * duckB * own;
		if (grainLaneA_) {
			sendL_[static_cast<size_t>(i)] += aOwn;
			sendR_[static_cast<size_t>(i)] += aCross;
		}
		if (grainLaneB_) {
			sendL_[static_cast<size_t>(i)] += bCross;
			sendR_[static_cast<size_t>(i)] += bOwn;
		}
		float l = (grainLaneA_ ? 0.0f : aOwn) + (grainLaneB_ ? 0.0f : bCross);
		float r = (grainLaneA_ ? 0.0f : aCross) + (grainLaneB_ ? 0.0f : bOwn);

		// **BOTH VOICES DRY IS THE BARE INPUT** — the firmware's `outputL = dryL` case, reached only when
		// neither voice drew a grain this turn.
		//
		// **THIS LINE USED TO READ `sliceIsDry_`, AND THAT IS WHY DENSITY WAS DEAD IN GRAIN.** On the
		// 80e81d76 install, no control changed the sound in Grain mode once the device was engaged. MEASURED: seven of the nine controls the descriptor declares
		// live in Grain changed the output by EXACTLY ZERO.
		//
		// `sliceIsDry_` is settled in `advanceSlice` — **which Grain never calls**, because `playGrain` is
		// its own renderer. So the EH sweep's hard-cut fix landed on this line and the VALUE it reads was
		// never updated on this path: correct-looking code consulting a variable nobody writes here. The
		// decision is Grain's own now, drawn per VOICE at each phase wrap, which is where the firmware
		// draws it.
		//
		// **SLEWED, NOT SUBSTITUTED.** The two windowed voices do not sum to the input's level at the
		// instant the second one goes dry — each is scaled by its own window and by the width's own and
		// cross weights — so swapping the bare input in stepped the level by up to three quarters of the
		// signal. MEASURED: Grain's worst jump in the click sweep, 0.74 with density thinning. The
		// reference swaps hard. Over the grain fade's base length the output moves between the two.
		const float dryTarget = (grainDryA_ && grainDryB_) ? 1.0f : 0.0f;
		const float dryStep = antiClickFrames_ > 0 ? 1.0f / static_cast<float>(antiClickFrames_) : 1.0f;
		grainBareDry_ = grainBareDry_ < dryTarget ? std::min(dryTarget, grainBareDry_ + dryStep)
		                                          : std::max(dryTarget, grainBareDry_ - dryStep);
		if (grainBareDry_ > 0.0f) {
			l += (left[i] - l) * grainBareDry_;
			r += (right[i] - r) * grainBareDry_;
		}

		if (target_.pWrite > 0.0f && grainWritesA_) {
			// **THE LINEAR BAR POSITION, NOT THE GRAIN'S READ POSITION — the same bug the looper already had
			// fixed, which Grain never got.** `playLooper`'s own comment states the rule: *"READ FROM THE
			// SHUFFLED POSITION, WRITE TO THE LINEAR ONE — the reference's own sentence, and the thing this
			// port had wrong. Writing back where it read makes a feedback path."* Grain kept the feedback
			// path: it read at `grainPosA_ + offsetA` and wrote to the very same sample.
			//
			// The firmware is explicit — `size_t writePos = (loopPlaybackStartPos + loopLinearBarPos) %
			// kLooperBufferSize;` inside its own grain branch. **SIXTH INSTANCE in this device of a fix
			// reaching one renderer and not the other**, and the one that cost the most: the click sweep
			// still had Grain at 39.9x after the write was enveloped, because enveloping a write that lands
			// in the wrong place only smooths the wrong place.
			// **THE FIRMWARE'S GRAIN WRITE, VERBATIM IN SHAPE** — and three things ours had differently:
			//
			//     if (hasPWrite && grainAWritesWet) {
			//         ... existL * (1 - envA) + srcAL * envA ...
			//
			// 1. **THE MIX IS `envA`**, voice A's own triangle — zero at both ends of every grain, so the
			//    written region fades in and out with the grain and leaves no seam. Ours wrote at a flat
			//    `target_.pWrite`, a hard-edged overdub. (There is no `kPWriteFade` in this branch; that
			//    guard is the looper's, for envelopes that can be off. A triangle cannot be.)
			// 2. **pWRITE IS WHETHER, NOT HOW MUCH** — a per-grain draw, `grainWritesA_`.
			// 3. **THE SOURCE IS VOICE A's**, `srcAL = grainAIsDry ? dryL : bufAL` — the grain it is
			//    playing, which is the input only when the voice is dry. Ours always wrote the input, so
			//    a wet cloud recorded over its own buffer with material it was not playing.
			const float wMix = std::clamp(envA, 0.0f, 1.0f);
			auditWrite(linearBarPos_);
			// **THE AUDITION SWITCH, AND IT IS THE WHOLE OF ORDER B** (backlog HU, `Param::GrainOrder`).
			// Recording `P Write` instead of performing it puts it at the flush BEHIND Dry and
			// Feedback, which is the designed order; `flushRingWrites` already runs the three in that
			// sequence, so nothing else has to change.
			//
			// **GUARDED ON `writersLive_` AS WELL AS ON THE SWITCH**, which is what keeps the two
			// states bit-identical with both new knobs at zero: with no continuous writer asking for
			// anything there is no flush to be behind, and B would otherwise defer this write for no
			// reason and change Grain's sound on its own.
			if (writersLive_ && target_.grainPWriteLast) {
				recordRingPWrite(i, al, ar, wMix);
			}
			else if (deferWrites_) { // backlog HN, test-only
				recordDeferredWrite(linearBarPos_, al, ar, wMix);
			}
			else {
				float oldL = 0.0f;
				float oldR = 0.0f;
				ring_.readAt(linearBarPos_, &oldL, &oldR);
				ring_.setWriteGain(freezeGainAt(static_cast<size_t>(i))); // backlog KX
				writeLoop(linearBarPos_, oldL + (al - oldL) * wMix, oldR + (ar - oldR) * wMix);
			}
		}

		// **BACKLOG HU: THE TWO CONTINUOUS WRITERS IN GRAIN.** The head is recorded for every sample,
		// exactly as in `playLooper` — they write wherever the head is, which is what "continuous"
		// means.
		//
		// **AND WHETHER `P Write` ABOVE IS DEFERRED IS NOW THE AUDITION SWITCH'S ANSWER — `Param::
		// GrainOrder`, added 2026-09-21 because the question cannot be settled from a description:
		// A and B have to be heard. The paragraphs below
		// describe the DEFAULT, `P Write First`, which is what shipped and what this comment used to
		// state unconditionally.**
		//
		// The designed order is DRY, then FEEDBACK, then `P Write`. Getting `P Write` last requires deferring
		// it to the flush — and in Grain that is exactly the change that is forbidden: backlog HN
		// measured a deferred `P Write` moving **55,843 of 96,000 ring frames** here, because
		// `playGrain` consults no overlap duck. That is the sound that has been in use for days, so
		// it stays immediate and the sequence in Grain is **`P Write` -> DRY -> FEEDBACK**.
		//
		// **WHAT THAT COSTS IS EXACTLY ONE THING, AND IT IS NOT THE OBVIOUS ONE.** The surviving
		// history is `(1-envA)(1-a_dry)(1-a_fb)` in either order — multiplication commutes — so the
		// buffer decays identically. What differs is which writer lands LAST, unattenuated by the
		// ones after it: FEEDBACK in Grain, `P Write` in the looper modes.
		//
		// **THE ALTERNATIVE WAS REJECTED ON ITS SHAPE:** deferring `P Write` here only while the new
		// writers are active would restore the designed order and is bit-identical at zero knobs, but it
		// puts a DISCONTINUITY in `P Write`'s own sound at the instant `Feedback` leaves its stop. An
		// order that differs by mode is better than a control that changes character when a different
		// knob moves off zero.
		//
		// **AND A PLAUSIBLE CONSEQUENCE OF THE INVERSION IS FALSE, measured** (backlog HU): it does
		// NOT follow that `Feedback` at 100% erases `P Write` here. A blend at amount 1 replaces the
		// frame, so the direct road is indeed closed — but the feedback SOURCE is the wet, and in
		// Grain the wet is what the two voices read out of this buffer, which is material `P Write`
		// wrote. It moves 76,790 ring frames at feedback 1.0 against 83,399 at 0.5.
		if (writersLive_) {
			auditWrite(linearBarPos_); // the continuous writers' own span, for the hazard measurement
			recordRingWrite(i, linearBarPos_);
			ring_.setWriteGain(freezeGainAt(static_cast<size_t>(i))); // backlog KX
			writeDryNow(linearBarPos_, captureL_[i], captureR_[i]);
		}

		wetL_[static_cast<size_t>(i)] = wetOf(l, kNoGrainEnd, 0, 0);
		wetR_[static_cast<size_t>(i)] = wetOf(r, kNoGrainEnd, 0, 0);
		mixCoeff_[static_cast<size_t>(i)] = mixAt();
		++envPos_;
		advanceEngageFade();

		// ADVANCE, AND RE-SEED ON THE WRAP. A voice that kept its position would be a loop; picking a
		// new one each turn is what makes it a cloud.
		// **THE BAR ADVANCES IN GRAIN TOO, AND IT DID NOT.** the player ribbon should respond in
		// grain mode.
		//
		// `linearBarPos_` was stepped only in `playLooper`, so in Grain the bar's position never moved —
		// and THREE things hang off it, not just the ribbon. The playhead had nothing to sweep; the BAR
		// COUNTER is stepped at the wrap, so multi-bar evolution and `Param::SeqLength` were frozen at
		// bar zero in Grain; and pWrite's linear write head never moved either.
		//
		// Grain still HAS a bar — it is the captured buffer — it simply does not cut it into slices. The
		// same wrap, in the same place in the loop, is all that was missing.
		if (bufferFrames_ > 0 && ++linearBarPos_ >= bufferFrames_) {
			linearBarPos_ = 0;
			barIndex_ = (barIndex_ + 1) % kBarIndexWrap;
		}

		// **A PITCHED VOICE'S OFFSET STEPS BY 2 AND ITS PHASE RUNS AT 2x** — the firmware's lines; see the
		// read above.
		grainOffsetA_ += grainUpA_ ? 2u : 1u;
		grainOffsetB_ += grainUpB_ ? 2u : 1u;
		grainPhaseA_ += grainUpA_ ? 2.0 * inc : inc;
		grainPhaseB_ += grainUpB_ ? 2.0 * inc : inc;
		if (grainPhaseA_ >= 1.0) {
			grainPhaseA_ -= 1.0;
			grainOffsetA_ = 0;
			// **THE PLAN DECIDES, NOT A FREE DRAW** — see `grainPlanFor`. The POSITION below stays random:
			// where in the buffer a grain lands is what makes a cloud a cloud, and the firmware picks it
			// freely too. What is deterministic is what is DONE to it.
			const SlicePlan pa = grainPlanFor(grainIndexA_);
			// **VOICE A'S POSITION REPEAT — the firmware's, ported 2026-09-24 (backlog JC).**
			// stutterer.cpp:1342-1348, at voice A's wrap:
			//
			//     if (loopGrainRepeatCounter > 0) {
			//         loopGrainRepeatCounter--;
			//     } else {
			//         grainPosA = fastRandom() % spread;
			//         loopGrainRepeatCounter = loopGrainRepeatSlices - 1;
			//     }
			//
			// so voice A replays one spot for `repeatSlices` grains running, and draws no position while
			// it does. Voice B always moves, as there. TWO DEPARTURES, both forced by this port's shape:
			// the firmware's `loopGrainRepeatSlices` is the playing SLICE's plan, and Grain here has no
			// slices — it draws a plan per grain (`grainPlanFor`) — so a run takes its length from the
			// plan of the grain that starts it. And **A DELIBERATE DIVERGENCE FROM A FIRMWARE BUG**
			// (2026-09-24): the firmware's counter is a LOCAL — `int32_t loopGrainRepeatCounter =
			// 0;` at stutterer.cpp:1172, declared at the top of every render call and never written back — so
			// on the hardware a run is cut at every 128-sample block, and a grain of 1024 or more almost never
			// gets its repeat, against its own comment ("hold position for N grain cycles"). Here it is a
			// member and a run lasts the grains it names; `test_fx_sklitter.cpp`'s *"a repeat run lasts its N
			// grains ACROSS host blocks"* holds that at a 64-frame block.
			if (grainRepeatLeftA_ > 0) {
				--grainRepeatLeftA_;
			}
			else {
				grainPosA_ = grainSpread_ > 0 ? (nextRandom() % grainSpread_) : 0;
				grainRepeatLeftA_ = std::max<int32_t>(pa.repeatSlices, 1) - 1;
			}
			grainRevA_ = pa.shouldReverse;
			grainUpA_ = pa.shouldPitchUp;
			grainDryA_ = pa.useDry || pa.macroWantsDry || grainDrawIsDry(grainIndexA_);
			grainWritesA_ = grainDrawWrites(grainIndexA_);
			// **THE GRAIN LANE, FROM THE SAME PLAN** (backlog EW) — one decision mechanism for both
			// roads: `planFor` drew this for the looper's slices and draws it here for this voice's
			// grain, from this voice's own index.
			grainLaneA_ = pa.shouldSendToLane;
			// **THE ONSET, COUNTED WITH THE GATES IT WILL PLAY UNDER** (backlog HR round 2) — the
			// effective dry, not the plan's alone, so the spark's colour is the grain the ear hears.
			countGrainOnset(grainDryA_, grainRevA_);
			++grainIndexA_;
			refreshGrainGeometry();
		}
		if (grainPhaseB_ >= 1.0) {
			grainPhaseB_ -= 1.0;
			grainOffsetB_ = 0;
			grainPosB_ = grainSpread_ > 0 ? (nextRandom() % grainSpread_) : 0;
			// **VOICE B IS OFFSET IN THE INDEX, NOT SHARING VOICE A'S.** Asking the same index would give
			// the two voices identical decisions — one grain wearing two envelopes rather than a cloud.
			const SlicePlan pb = grainPlanFor(grainIndexB_);
			grainRevB_ = pb.shouldReverse;
			grainUpB_ = pb.shouldPitchUp;
			grainDryB_ = pb.useDry || pb.macroWantsDry || grainDrawIsDry(grainIndexB_);
			grainLaneB_ = pb.shouldSendToLane; // voice B's own draw, from voice B's own index
			countGrainOnset(grainDryB_, grainRevB_);
			++grainIndexB_;
		}
	}
}

/// **PUBLISHED ONCE A BLOCK, AND ONLY THE HEADER MOVES MOST BLOCKS.** The per-step entries are
/// written when a slice begins; this fills in the four numbers that change continuously and hands the
/// whole array over. A sink that is not installed costs one branch.
void Sklitter::publishReadings() {
	if (sink_.publish == nullptr) {
		return;
	}
	readings_[static_cast<size_t>(Reading::Status)] = static_cast<float>(engage_.status());
	readings_[static_cast<size_t>(Reading::Recording)] = recording_ ? 1.0f : 0.0f; // backlog MJ: the record light
	// **STEPS, CLAMPED TO WHAT THE PICTURE HOLDS.** A bar cut finer than the picture publishes the
	// steps it can and the card draws what it is given; it must never claim more entries than exist.
	// **THE CUT IS PUBLISHED AT REST, NOT ONLY WHILE PLAYING.** nothing is populating until i
	// engage the effect. `numSlices_` is settled in the engage branch, so before the gesture there
	// was nothing to draw and the band was an empty bed — the player could not see what the current
	// Mode and Rate were about to do to the bar until after committing to it.
	//
	// AT REST THE COUNT IS WHAT THE SETTINGS IMPLY, by the same two functions the press uses, so the
	// resting picture and the playing one cannot disagree about the cut. The steps are published
	// WITHOUT their `Known` bit — nothing has played, so there is nothing true to say about where each
	// reads from — and the band draws them as empty slots. Engaging then fills them in as they play.
	// **HOW THE BAR IS CUT — the live count while playing, what the settings imply at rest.**
	//
	// **THE CUT IS PUBLISHED AT REST, NOT ONLY WHILE PLAYING.** nothing is populating until i
	// engage the effect. `numSlices_` is settled in the engage branch, so before the gesture there
	// was nothing to draw and the band was an empty bed — the player could not see what the current
	// Mode and Rate were about to do to the bar until after committing to it. At rest the count comes
	// from the same two functions the press uses, so the resting picture and the playing one cannot
	// disagree about the cut.
	//
	// **NOT GATED ON `sliceFrames_ == 0`.** That was how "has the gesture settled a bar yet" was
	// asked, and the pre-warm sets `sliceFrames_` while idle — so the test quietly stopped meaning
	// what it said and the band showed ONE step for a device sitting in Shuffle. MEASURED in the
	// instrumented run as `N=1` where it should read 16. The question is only ever about the STATUS.
	//
	// **AND IN GRAIN, WHAT THE SETTINGS IMPLY WHILE PLAYING TOO.** The live count is `numSlices_`, and
	// the one line that refreshes it from Rate is in `advanceSlice` — which Grain never calls. So in
	// Grain the band took Rate at rest and froze at the press: a control that re-cut the picture
	// until the moment it was being played.
	int32_t slicesPerBar = numSlices_;
	if (engage_.status() != Status::Playing || target_.mode == Mode::Grain) {
		if (target_.isLooperMode()) {
			slicesPerBar = loopSlicesFor(target_.mode, target_.rate, resolveBarFrames());
		}
		else {
			slicesPerBar = 1; // Burst plays one captured slice
		}
	}
	slicesPerBar = std::max(slicesPerBar, 0);

	// **THE PICTURE IS THE WHOLE SEQUENCE, NOT ONE BAR — which is the whole of the fix.**
	//
	// The playhead spans one bar while the arrangement can repeat over four. So the band spans
	// `seqBars` of them, and a step of the picture is a step of the SEQUENCE. Cram was chosen over page
	// for this, because with varying sequence sizes cramming makes the length naturally visible — a
	// two-bar sequence is then visibly half the chunks of a four-bar one.
	//
	// **CLAMPED TO WHAT THE PICTURE HOLDS**, exactly as the one-bar version was: a sequence cut finer
	// than `kPictureSteps` publishes the steps it can and the card draws what it is given. It must
	// never claim more entries than exist.
	const int32_t seqBars = target_.isLooperMode() && target_.seqBars > 0 ? target_.seqBars : 1;

	// **HOW MANY OF THOSE BARS ACTUALLY FIT, WHICH IS NOT ALWAYS ALL OF THEM — and getting this wrong
	// put the playhead off the end of the band.**
	//
	// A four-bar sequence at 32 slices to the bar wants 128 steps and the picture holds 64. The first
	// version simply CLAMPED the step count, which leaves the picture holding two bars while the head
	// is still addressed in a four-bar scheme: `seqBarIndex() * slicesPerBar` reaches 96 for a band
	// with 64 slots. CAUGHT BY ITS OWN CASE, reading `32 < 32` — and it was my code rather than the
	// mutation under test, which is the whole argument for asserting the head's bar rather than
	// trusting the arithmetic.
	//
	// **SO THE BAND DROPS WHOLE BARS RATHER THAN TRUNCATING THE LAST ONE.** A partial bar at the right
	// edge would be a lie about the cut in exactly the place the design claims to be precise, and
	// `SeqBars` is published from THIS rather than from the setting — so the rules, the steps and the
	// head all come from one number and cannot disagree. The setting still governs the SOUND; this
	// governs only how much of it the band can show.
	int32_t drawnBars = seqBars;
	if (slicesPerBar > 0) {
		const auto fits = static_cast<int32_t>(kPictureSteps / static_cast<size_t>(slicesPerBar));
		drawnBars = std::clamp(fits, 1, seqBars);
	}
	const auto steps = static_cast<size_t>(std::clamp<int64_t>(
	    static_cast<int64_t>(slicesPerBar) * drawnBars, 0, static_cast<int64_t>(kPictureSteps)));

	// **ONE COHERENT WALK, WHETHER PLAYING OR AT REST, AND THAT IS THE FIX.**
	//
	// The arrangement was published a step at a time as each step played, because that is where the
	// device already computes it for nothing — and it is why the band assembled itself from up to four
	// different bars under the playhead. `advanceSlice` carries the full account of what that cost.
	//
	// **IT IS A PURE FUNCTION OF THE CONTROLS.** `planFor` and `scheduleFor` take the zones, the macro,
	// the mode and a step index and nothing else — no captured audio, no playhead. So the whole
	// sequence can be computed at any moment, and the playing device knows nothing about the
	// arrangement that this walk does not. Turning Zone A redraws the pattern under the hand, engaged
	// or not.
	//
	// **RECOMPUTED ONLY WHEN THE INPUTS MOVE.** This runs on the audio thread, once a block; sixty-four
	// plans and schedules every block for a picture that changes when a knob does would be paying
	// continuously for something usually still.
	//
	// **AND WHICH WINDOW OF THE SEQUENCE IS ON SCREEN, WHEN THE WHOLE OF IT WILL NOT FIT.** Cram was
	// chosen over page, since it makes varying sequence lengths naturally visible, and
	// cram is what happens whenever the sequence fits, which is every rung of the ladder except the
	// finest cut. At 32 slices to the bar a four-bar sequence needs 128 slots against 64, so SOMETHING
	// has to give.
	//
	// **THE ONE THING THAT MUST NOT GIVE IS THE TRUTH OF THE PICTURE.** The first fix wrapped the head
	// into the two bars that fit, which draws bar 0 while the device plays bar 2 — the same class of
	// lie as the seam this whole change removes, just slower. So the band shows the WINDOW the current
	// bar is in, and pages only when it physically cannot cram. The key carries the window, or a page
	// turn would leave the old page on screen.
	const int32_t windowBase = drawnBars > 0 ? (seqBarIndex() / drawnBars) * drawnBars : 0;
	if (steps > 0 && slicesPerBar > 0) {
		// **THE KEY HOLDS THE LENGTHS THE WALK DIVIDES BY, NOT THE SETTING THEY COME FROM.** It held
		// `bufferBars`, and the setting and the frames move at different moments: the setting when the
		// hand turns it, `bufferFrames_` when `prewarm` next runs — which is not while bypassed and not
		// while engaged. The key moved first, the walk ran against the old frames, and when the frames
		// caught up nothing moved the key. A device loaded bypassed drew a FLAT band (every source 0,
		// from a bar of one frame) until a zone was touched.
		//
		// **AND AT REST THE LENGTHS ARE THE ONES THE SETTINGS IMPLY, NOT THE RING'S (backlog IB).** Holding
		// the frames in the key made the band follow them — but "not while bypassed" and "not while the
		// locked transport is parked" are exactly the states where the frames NEVER catch up while the
		// hand is still turning things, so the key was right about a stale number. MEASURED: Buffer Length
		// never redrew in any looper mode while bypassed or parked under the lock, and a Mode change out
		// of Burst drew every source at 0 (a bar of one frame, as `sliceFrames_` was never
		// set). At rest nothing is playing the ring, and the next thing that will is `prewarm` or the
		// press, both of which re-length it from these same two functions — so the resting picture is
		// the one those will play, as `slicesPerBar` above already is. Once armed, the gesture owns
		// the lengths and the band draws what it captured.
		size_t bar = sliceFrames_;
		size_t buffer = bufferFrames_;
		if (engage_.status() == Status::Off && target_.isLooperMode()) {
			if (const size_t implied = resolveBarFrames(); implied > 0) {
				bar = implied;
				buffer = resolveBufferFrames();
			}
		}
		bar = bar > 0 ? bar : 1;
		buffer = buffer > 0 ? buffer : bar;
		// **THE PICTURE'S PUSH, BUILT ONCE FOR THE WHOLE WALK AND HELD IN THE KEY.** It is the same
		// `performPushOf` the playing slice uses, which is what makes the band the arrangement the device
		// would play; and it is in the KEY because it is what the walk varies over — a key still holding
		// the deleted `target_.macro` would compare equal while the puck moved and freeze the picture (that
		// control is gone now, and the point it made survives it: the push is what varies). Built before the
		// comparison rather than inside the `if`, because the comparison is what needs it.
		const MacroPush push = targetPush();
		// **REPEAT PLANS AT ITS OWN COUNTER, SO THE WALK DOES (backlog IX).** `advanceSlice` hands `planFor`
		// `repeatLoopIndex_` in Repeat, a counter stepped once a slot and wrapped at 8 — not the step. At rest
		// the next press starts it at 1 on step 0; while playing it is read off the device, counted back from
		// the step under the head, so the steps this pass has played and the ones it will play both get the
		// counter they meet.
		int32_t repeatBase = 0;
		if (target_.mode == Mode::Repeat) {
			repeatBase = 1;
			if (engage_.status() == Status::Playing) {
				const int32_t head = (seqBarIndex() - windowBase) * slicesPerBar + std::max(0, sliceIndex_);
				repeatBase = ((repeatLoopIndex_ - head) % 8 + 8) % 8;
			}
		}
		const GrooveFeel feel = target_.grooveFeel();
		// **TIME'S PHRASE, WHICH THE SEQUENCE DOES NOT CARRY.** Time silences the bar's last 50 ms only on the
		// last bar of its four-bar phrase (`barEndSilence_`), counted on `barIndex_` — so a sequence shorter
		// than the phrase plays its steps differently from pass to pass. The walk plans each bar of THIS pass:
		// the phrase position of the sequence's first bar, from the counter the device plays on (at rest, the
		// one it would carry into the press). In the key, so the band follows the phrase bar by bar.
		const int32_t phraseBase = target_.mode == Mode::Time
		                               ? (((barIndex_ - seqBarIndex()) % kTimePhraseLength) + kTimePhraseLength) % kTimePhraseLength
		                               : 0;
		const RestKey key { target_.zoneA,  target_.zoneB,     push,
			                target_.rate,   target_.gamma,     target_.mode,
			                drawnBars,      slicesPerBar,
			                static_cast<int32_t>(steps),       bar,
			                buffer,         windowBase,        repeatBase,
			                target_.density, target_.sendOverride, feel, phraseBase };
		if (!(key == restKey_)) {
			restKey_ = key;
			const Coordinate ra = splitCoordinate(target_.zoneA);
			const Coordinate rb = splitCoordinate(target_.zoneB);
			const Coordinate& rm = kNoKnobConfig; // see the constant: the knob configuration is deleted
			// **THE LONG GRAIN AND THE RATCHET (backlog IX — the grain-repeat effect applies in other modes
			// as well, a long grain draws a larger grain, and the two can stack).** Both are the plan's, both act only where `advanceSlice` cuts the bar
			// (`kGrooveModes`: Repeat, Time, Shuffle, Pattern, Pitch — Grain has its own renderer and Burst
			// never asks), and they DO stack: the ratchet divides the slot, and a long grain's slot is
			// `grainLength` steps long, so it ratchets across the whole of it (the reference's
			// `scatterSubSliceLength = currentSliceLength / scatterSubdivisions`, taken after the long grain
			// set `currentSliceLength`).
			//
			// **A LONG GRAIN IS ONE CELL, AND THE STEPS UNDER IT DO NOT PLAY.** The head steps
			// `sliceIndex_ += grainLength`, so the walk does too: it publishes the grain's span on its first
			// step and marks the rest `Covered`. It drew a plan for every step before, including the ones a
			// long grain skips — cells for slices the device never started.
			const bool slicing = (kGrooveModes & kModeBit(target_.mode)) != 0u;
			size_t coveredUntil = 0;
			size_t coveringStep = 0;
			// **THE HOLD, WALKED AS THE HEAD WALKS IT** — reset at each bar's first step (the bar's end cancels
			// it) and carried slot to slot, with Time's override on every fresh draw. See `PlanHold`.
			PlanHold walkHold {};
			for (size_t k = 0; k < steps && k < kPictureSteps; ++k) {
				const size_t base = stepReading(k);
				if (k < coveredUntil) {
					readings_[base] = readings_[stepReading(coveringStep)];
					readings_[base + 1u] =
					    static_cast<float>(static_cast<uint32_t>(StepFlag::Known) | static_cast<uint32_t>(StepFlag::Covered));
					readings_[base + 2u] = 0.0f;
					continue;
				}
				// **WHICH BAR OF THE SEQUENCE THIS STEP IS IN, AND WHERE IN THAT BAR.** The step index
				// the plan and the schedule are asked about is the position within the BAR — that is
				// the grid both of them divide — while the bar number is what drives the multi-bar
				// evolution. Handing `k` to `planFor` directly would have drawn a sixty-four-slice bar
				// that the device never plays.
				const auto whichBar = static_cast<int32_t>(k / static_cast<size_t>(slicesPerBar));
				const auto at = static_cast<int32_t>(k % static_cast<size_t>(slicesPerBar));
				// The one builder — the macro's phase push included, as the playing slice has it.
				// ALREADY FOLDED: `whichBar` runs 0..seqBars-1 by construction, which is exactly what
				// `seqBarIndex()` computes for the playing device.
				// AND THE SEND OVERRIDE, so the picture draws the sends the device will actually make:
				// a band that keeps showing phi's scattered sends while the knob is at -100% would be
				// describing a device that is not playing.
				const PhaseOffsets ro = offsetsFor(ra, rb, rm, push, target_.gamma, windowBase + whichBar,
				                                   target_.mode == Mode::Repeat, target_.sendOverride);
				const int32_t planIndex =
				    target_.mode == Mode::Repeat ? (repeatBase + static_cast<int32_t>(k)) & 0x7 : at;
				if (at == 0) {
					walkHold = PlanHold {};
				}
				const SlicePlan p = planUnderHold(walkHold, target_.mode, ra.position, rb.position, slicesPerBar, [&] {
					return planFor(ra.position, rb.position, rm.position, push, planIndex, ro, &restCache_);
				});
				const SliceSchedule sch = scheduleFor(target_.mode, ra.position, at, slicesPerBar, bar, p,
				                                      buffer, minSliceFrames_);
				readings_[base] = static_cast<float>(sch.startOffset) / static_cast<float>(buffer);
				uint32_t flags = static_cast<uint32_t>(StepFlag::Known);
				float ratchetPiece = 0.0f;
				// **DRY IS THE PLAN'S, THE MACRO'S AND DENSITY'S — all three, as `advanceSlice` asks.** This said
				// density "draws per slice from a hash that advances as the bar plays", and it does not: the draw
				// is a hash of the SLOT and the control (`grainDrawIsDry`, the same arithmetic), so the walk can
				// ask it. The ribbon-truth audit measured every density-thinned slot drawn as a grain.
				const bool dry = p.useDry || p.macroWantsDry || grainDrawIsDry(at);
				if (slicing) {
					// THE SPAN IS THE SCHEDULE'S, clamped as the head's is — to the bar and to the buffer.
					const auto span = static_cast<size_t>(std::clamp<int32_t>(sch.grainLength, 1, 32));
					flags |= static_cast<uint32_t>(span - 1u) << kStepSpanShift;
					coveringStep = k;
					coveredUntil = k + span;
					// **THE RATCHET AS IT IS HEARD, which is not the plan's count** (ribbon truth). `playLooper` cuts
					// the slot's SOUNDING length (the material, when a swing lengthened the slot past it; else the
					// slot as the groove moved it) times `lengthMult` into the count, floored at 24 ms — and a piece
					// that would start past that audible end is silence. So the count published is the pieces that
					// start inside it, and the piece is a fraction of the REAL slot. MEASURED before: a count of 9
					// published where 4 were heard (the floor), and every swung slot's pieces off by its swing.
					if (!dry && sch.sliceLength > 0) {
						size_t slot = sch.sliceLength;
						if (grooveActive(feel)) {
							const size_t from = grooveBoundary(feel, at, slicesPerBar, bar);
							const size_t to = grooveBoundary(feel, at + static_cast<int32_t>(span), slicesPerBar, bar);
							slot = std::max(to > from ? to - from : 0, minSliceFrames_);
						}
						// Time's bar-end silence falls on its phrase's last bar — read off the sequence's bar, which
						// is the phrase's whenever the sequence is a whole number of phrases.
						const bool barEndSilent =
						    target_.mode != Mode::Time
						    || (phraseBase + windowBase + whichBar) % kTimePhraseLength == kTimePhraseLength - 1;
						const int32_t heard = heardRatchet(p, slot, sch.sliceLength, grooveBoundary(feel, at, slicesPerBar, bar),
						                                   buffer, barEndSilent, &ratchetPiece);
						if (heard > 1) {
							flags |= static_cast<uint32_t>(heard - 1) << kStepRatchetShift;
						}
					}
				}
				readings_[base + 2u] = ratchetPiece;
				// **A DRY SLOT READS NOTHING**, so it is not reversed, not pitched and does not restart — the
				// band drew all three on the hollow cell, as marks about a read that never happened.
				if (dry) {
					flags |= static_cast<uint32_t>(StepFlag::Dry);
				}
				else {
					if (p.shouldReverse) {
						flags |= static_cast<uint32_t>(StepFlag::Reversed);
					}
					if (p.shouldPitchUp) {
						flags |= static_cast<uint32_t>(StepFlag::PitchedUp);
					}
				}
				readings_[base + 1u] = static_cast<float>(flags);
			}
			for (size_t k = steps; k < kPictureSteps; ++k) {
				const size_t base = stepReading(k);
				readings_[base] = 0.0f;
				readings_[base + 1u] = 0.0f;
				readings_[base + 2u] = 0.0f;
			}
		}
	}
	readings_[static_cast<size_t>(Reading::Steps)] = static_cast<float>(steps);
	// **HOW MANY BARS THE BAND IS SHOWING, so it can rule them.** Without this the card would have to
	// infer the bar lines from `Steps` and a slice count it does not have, which is a second opinion
	// about the cut; the device knows and says.
	readings_[static_cast<size_t>(Reading::SeqBars)] = static_cast<float>(drawnBars);
	// **THE HEAD ADDRESSES THE SEQUENCE, NOT THE BAR — the other half of the report.**
	//
	// `sliceIndex_` is a position within the bar, so a head published from it alone swept the first
	// bar's worth of the band over and over while the picture showed four. The bar it is IN is
	// `seqBarIndex()`, which is the same fold the walk above uses for `whichBar` — so head and
	// arrangement are addressed by one scheme and cannot point at different things.
	// **FOLDED BY WHAT IS DRAWN, NOT BY THE SETTING.** When the band shows fewer bars than the
	// sequence has, the head wraps within the band rather than running off its right edge — which is
	// what "32 < 32" was.
	// **RELATIVE TO THE WINDOW ON SCREEN**, so the head marks the bar the device is really in. It is
	// the identity whenever the sequence fits, which is every case but the finest cut.
	const int32_t headBar =
	    target_.isLooperMode() && drawnBars > 0 ? (seqBarIndex() - windowBase) : 0;
	const int32_t perBar = std::max(1, slicesPerBar);

	// **GRAIN HAS NO SLICE INDEX, SO ITS HEAD COMES FROM THE BAR POSITION.** the player ribbon
	// should respond in grain mode.
	//
	// `sliceIndex_` and `positionInSlice_` are `advanceSlice`'s, and Grain never calls it — so both sat
	// at whatever the gesture left them and the ribbon did not move. Grain reads the buffer as a CLOUD
	// rather than as a sequence of slices, but it still walks the bar, and `linearBarPos_` is exactly
	// how far through it is. Divided into the same steps the band draws, that is the same ribbon
	// sweeping the same picture — which is what makes the band mean one thing in every mode.
	if (target_.mode == Mode::Grain && bufferFrames_ > 0) {
		const double through = static_cast<double>(linearBarPos_) / static_cast<double>(bufferFrames_);
		const double atStep = std::clamp(through, 0.0, 0.999999) * static_cast<double>(perBar);
		const auto whole = static_cast<int32_t>(atStep);
		readings_[static_cast<size_t>(Reading::Step)] = static_cast<float>(headBar * perBar + whole);
		readings_[static_cast<size_t>(Reading::StepPhase)] =
		    static_cast<float>(std::clamp(atStep - static_cast<double>(whole), 0.0, 1.0));
	}
	// **BURST HAS NO SLICE INDEX EITHER, SO ITS HEAD IS WHERE IT IS IN ITS CYCLE** (2026-09-24), which
	// is what the gate is timed by. It is one step, and the phase across it read `positionInSlice_` —
	// the looper's, which it does not advance — so the ribbon stood wherever the last looper gesture left
	// it: the playhead stopped on every switch into it.
	else if (target_.mode == Mode::Burst) {
		double phase = 0.0;
		if (engage_.status() == Status::Playing) {
			const size_t cycle = burstCycleLength();
			phase = cycle > 0 ? static_cast<double>(burstCyclePos_) / static_cast<double>(cycle) : 0.0;
		}
		readings_[static_cast<size_t>(Reading::Step)] = 0.0f;
		readings_[static_cast<size_t>(Reading::StepPhase)] = static_cast<float>(std::clamp(phase, 0.0, 1.0));
	}
	else {
		readings_[static_cast<size_t>(Reading::Step)] =
		    static_cast<float>(headBar * perBar + std::max(0, sliceIndex_));
		const float slot = schedule_.sliceLength > 0 ? static_cast<float>(schedule_.sliceLength) : 1.0f;
		readings_[static_cast<size_t>(Reading::StepPhase)] =
		    std::clamp(static_cast<float>(positionInSlice_) / slot, 0.0f, 1.0f);
		// **THE PLAYING CELL'S RATCHET IS THE PLAYING SLOT'S** (ribbon truth). The walk sizes each slot from the
		// grid; the slot under the head is sized by the ENGINE, and the two differ exactly where the groove
		// re-anchors a bar turned mid-way (backlog IG: the first slot after the turn runs to the new grid from
		// where the old one left off — MEASURED 4,080 frames against the walk's 6,000, so three pieces heard
		// and four drawn). So the head's cell takes its count and piece from the slot itself.
		const auto head = static_cast<size_t>(headBar * perBar + std::max(0, sliceIndex_));
		if (engage_.status() == Status::Playing && (kGrooveModes & kModeBit(target_.mode)) != 0u && head < steps
		    && !sliceIsDry_) {
			const size_t base = stepReading(head);
			auto flags = static_cast<uint32_t>(readings_[base + 1u]);
			if ((flags & (static_cast<uint32_t>(StepFlag::Covered) | static_cast<uint32_t>(StepFlag::Dry))) == 0u) {
				float piece = 0.0f;
				const int32_t heard = heardRatchet(plan_, schedule_.sliceLength, materialLength_, slotStartFrame_,
				                                   bufferFrames_, barEndSilence_, &piece);
				flags &= ~(kStepCountMask << kStepRatchetShift);
				flags |= heard > 1 ? static_cast<uint32_t>(heard - 1) << kStepRatchetShift : 0u;
				readings_[base + 1u] = static_cast<float>(flags);
				readings_[base + 2u] = piece;
			}
		}
	}
	// THE CAPTURE BAR FILLS TOWARD THE BUFFER, which is what is being recorded.
	const float want = bufferFrames_ > 0 ? static_cast<float>(bufferFrames_) : 1.0f;
	readings_[static_cast<size_t>(Reading::Fill)] =
	    std::clamp(static_cast<float>(engage_.capturedFrames()) / want, 0.0f, 1.0f);
	// **READ, NOT COMPUTED.** The render maintains it (`foldOutLevel`), so this publish and a direct
	// caller's `outputLevel()` are one number rather than two that agree by habit.
	readings_[static_cast<size_t>(Reading::WetLevel)] = wetLevelPeak_;
	readings_[static_cast<size_t>(Reading::GrainLevel)] = grainLevelPeak_;
	// THE ONSET COUNTERS (backlog HR round 2) — exact as floats, since they wrap at 2^24.
	for (size_t c = 0; c < kGrainOnsetClasses; ++c) {
		readings_[static_cast<size_t>(Reading::GrainOnsets) + c] = static_cast<float>(grainOnsets_[c]);
	}
	readings_[static_cast<size_t>(Reading::RepeatCount)] = static_cast<float>(repeats_);
	readings_[static_cast<size_t>(Reading::RepeatPeriod)] = repeatPeriod_;
	// **RATE AS WHAT IS PLAYING (backlog GS).** Burst reads the EFFECTIVE position while a
	// gesture is live — a quantized press moves it off the knob — and the knob at rest, where the next
	// press starts from it. A looper mode reads the count the band above was cut by.
	const double ratePosition = engage_.status() != Status::Off
	                                ? effectiveRatePosition()
	                                : std::clamp(static_cast<double>(target_.rate), 0.0, 1.0);
	readings_[static_cast<size_t>(Reading::RateDivision)] =
	    static_cast<float>(rateDivisionFor(target_.mode, ratePosition, slicesPerBar));
	// THE GRID THE BAND IS RULED ON (backlog IG) — the feel where it acts, Straight where it does not; the
	// mode and the amount as two numbers since backlog IO.
	const GrooveFeel groove = target_.grooveFeel();
	const bool grooving = grooveActive(groove);
	readings_[static_cast<size_t>(Reading::Groove)] = grooving ? static_cast<float>(groove.mode) : 0.0f;
	readings_[static_cast<size_t>(Reading::GrooveAmount)] = grooving ? static_cast<float>(groove.amount) : 0.0f;
	// **REPEAT'S FACE — HOW LONG ONE REPEAT READS, on the steps' scale (the buffer).** Zero elsewhere, which
	// is what keeps every other mode on the chunk face. Engaged it is the playing slot's MATERIAL, before
	// the groove moved the slot (`materialLength_`); at rest it is what the settings imply, by the same
	// function the slot will be cut with (backlog IB).
	float repeatLength = 0.0f;
	if (target_.mode == Mode::Repeat) {
		const bool live = engage_.status() == Status::Playing && materialLength_ > 0;
		const bool atRest = engage_.status() == Status::Off || sliceFrames_ == 0;
		const size_t bar = atRest ? resolveBarFrames() : sliceFrames_;
		const size_t held = atRest ? resolveBufferFrames() : bufferFrames_;
		const size_t buffer = held > 0 ? held : bar;
		const size_t frames = live ? materialLength_ : repeatReadFrames(target_.rate, bar, slicesPerBar);
		if (buffer > 0) {
			repeatLength = std::clamp(static_cast<float>(frames) / static_cast<float>(buffer), 0.0f, 1.0f);
		}
	}
	readings_[static_cast<size_t>(Reading::RepeatLength)] = repeatLength;
	sink_.publish(sink_.context, readings_.data(), readings_.size());
}

//--------------------------------------------------------------------------------------------------//
// THE POST CHAIN — backlog EW. See the declarations in `sklitter.h` for the decisions (D1/D2/D3).    //
// This is the first caller of the bolt-on chain support.                                             //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **THE CHAIN'S SHAPE, AND ITS CORE SOURCES ARE THIS DEVICE'S SEVEN AUX OUTPUTS** — decision D3(a).
///
/// **IT IS NOT `rack::kRackAxes`, AND THE DIFFERENCE IS THE ONE NUMBER THAT MATTERS.** That shape
/// declares eight core sources because a rack's are its eight MACROS — a hand's knobs. This chain has
/// no macros and no hand; what it publishes inward is the puck, the four weights and the speed, which
/// is seven. Sixteen slots and the two strides are the same because they are properties of the
/// backplane rather than of the host.
///
/// **WHY NOT `Source::outerOutput` FROM THIS DEVICE INSTEAD, WHICH READS AS THE PROPER ROAD.** Trap
/// T1, measured (`tests/test_rack_nesting.cpp`): `RackEngine::upstreamOf(self, output(self, n))`
/// answers FALSE for all seven, because `Graph::reaches(from, to)` returns true when `from == to` —
/// *"a device is not upstream of itself"* — and `wouldCycle` uses that bare form. A SIBLING earlier in
/// the chain answers true; only the self-case is refused. So the outer road needs a carve-out in a
/// rule that is currently uniform, and this one works today.
///
/// (Since backlog HS `upstreamOf` refuses `s.device == self` OUTRIGHT, one line above the cycle test:
/// this device now declares some of its own CONTROLS downstream of its outputs (`kSelfRouting`), and
/// that declaration must not answer for the chains in here. The answer T1 pins is unchanged.)
constexpr backplane::Axes kPostChainAxes {
	.coreSources = kPerformOutputs,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = device::kMaxDeviceOutputs,
};

Sklitter& deviceAt(void* storage) { return *static_cast<Sklitter*>(storage); }
const Sklitter& deviceAt(const void* storage) { return *static_cast<const Sklitter*>(storage); }

/// The chain inside an instance, or null — every container op is guarded by this and nothing else.
///
/// **ONE BODY, TWO NAMES.** `postChainOf` is the same cast published for the editor (see
/// `sklitter.h`); this is the name the ops table below reads, kept because every one of those lambdas
/// says `chainAt(storage)` and renaming them would be twelve edits for no fact.

/// **WHICH LANE IS WHICH IN THE SAVED BODY** — stable ids, never positions (`rack::BundleLane`).
enum : uint16_t { kLaneMaster = 0, kLaneGrain = 1 };

/// THE TWO LANES, AS THE BODY CODEC SEES THEM. One place; every container verb that spans both reads
/// it rather than listing them again.
std::array<rack::BundleLane, 2> lanesOf(void* storage) {
	auto& d = deviceAt(storage);
	return { rack::BundleLane { kLaneMaster, d.postChain() }, rack::BundleLane { kLaneGrain, d.grainChain() } };
}

/// **EVERY LANE, FOR THE VERBS THAT SIMPLY FAN OUT.** `adopt`, `prepare`, `setVoices`, `setOuterGate`
/// and `reset` mean the same thing to both chains, so they are written once here.
template <class Fn>
void forEachLane(void* storage, Fn&& fn) {
	auto& d = deviceAt(storage);
	if (auto* c = d.postChain(); c != nullptr) {
		fn(*c);
	}
	if (auto* c = d.grainChain(); c != nullptr) {
		fn(*c);
	}
}

/// **THE LANE THAT HOLDS A GIVEN INNER DEVICE — the whole reason gap G3 had to land first.**
///
/// `device::ContainerOps`' four route verbs take a `device::Destination{deviceId, index}` with no lane
/// argument, so the holder must answer "which chain?" from the id alone. That is sound ONLY because
/// the two chains issue ids from ONE counter (`Sklitter::laneIds_`): before that fix both chains named
/// their first device 1 and both answered `descriptorOf(1)`, so this function would have had two right
/// answers and no way to choose. Null for an id neither holds.
rack::InnerChain* laneHolding(void* storage, DeviceInstanceId id) {
	auto& d = deviceAt(storage);
	if (auto* c = d.postChain(); c != nullptr && c->bridge() != nullptr && c->bridge()->descriptorOf(id) != nullptr) {
		return c;
	}
	if (auto* c = d.grainChain(); c != nullptr && c->bridge() != nullptr && c->bridge()->descriptorOf(id) != nullptr) {
		return c;
	}
	return nullptr;
}

const rack::InnerChain* laneHolding(const void* storage, DeviceInstanceId id) {
	return laneHolding(const_cast<void*>(storage), id);
}

} // namespace

rack::InnerChain* postChainOf(void* surface) noexcept {
	return surface != nullptr ? deviceAt(surface).postChain() : nullptr;
}

const rack::InnerChain* postChainOf(const void* surface) noexcept {
	return surface != nullptr ? deviceAt(surface).postChain() : nullptr;
}

rack::InnerChain* grainChainOf(void* surface) noexcept {
	return surface != nullptr ? deviceAt(surface).grainChain() : nullptr;
}

const rack::InnerChain* grainChainOf(const void* surface) noexcept {
	return surface != nullptr ? deviceAt(surface).grainChain() : nullptr;
}

Sklitter::Sklitter()
    : laneIds_(std::make_unique<backplane::InstanceCounter>()),
      // **THE COUNTER GOES IN BY POINTER** (gap G3) — with ONE chain this changes nothing observable,
      // and with two it is the whole of what makes a `device::Destination{deviceId, index}` name one
      // device rather than one per lane. Passing it now means the second lane cannot forget it.
      postChain_(std::make_unique<rack::InnerChain>(kPostChainAxes, laneIds_.get())),
      // **THE GRAIN LANE, ON THE SAME AXES AND THE SAME COUNTER.** Same shape because it publishes the
      // same seven aux outputs inward; same counter because gap G3 is what stops the two chains naming
      // one another's devices.
      grainChain_(std::make_unique<rack::InnerChain>(kPostChainAxes, laneIds_.get())) {
	// THE SPIKE RECORDER NAMES A DEVICE IN EITHER LANE BY ITS PATH: `SKLT/post/DELA` (backlog SR).
	postChain_->engine().setCostLane("post");
	grainChain_->engine().setCostLane("grain");
	// **AND THESE SEVEN CELLS RIDE IN THE SAVED BODY, WHICH IS NOT A DEFECT BUT WILL READ AS ONE.**
	// `RackBridge::patch()` writes `engine.macro(i)` for every core source, and this chain's core
	// sources are the live puck — so **two saves of the same patch at different puck positions produce
	// DIFFERENT BODY BYTES.** A byte-compare round-trip case would call that a defect; it is not. The
	// values are overwritten by `runPostChain` on the first tick after load, before anything reads
	// them, so nothing downstream can tell. A save/load/save comparison is still deterministic and
	// still valid; it is only "same patch, moved puck" that differs, and the next person to write a
	// round-trip case needs that sentence before they debug a phantom.
	//
	// The alternative — a flag telling the codec these cells are live rather than stored — is format
	// framing, which is gap G2's territory and nobody's decision to make mid-implementation.
	//
	// **THE POLARITY OF EACH CORE SOURCE, DECLARED ONCE, FROM THE ONE TABLE THAT ALREADY SAYS IT.**
	// `kOutputs[n].bipolar` is what every OUTER reader of these seven is sized by; an inner reader that
	// disagreed would be the same source meaning two different things depending on which side of this
	// device you stood. `InnerChain::publishCoreSource` then takes the value as the source publishes it.
	for (size_t n = 0; n < kPerformOutputs; ++n) {
		postChain_->setCoreBipolar(n, kOutputs[n].bipolar);
		grainChain_->setCoreBipolar(n, kOutputs[n].bipolar);
	}
}

Sklitter::~Sklitter() = default;
Sklitter::Sklitter(Sklitter&&) noexcept = default;
Sklitter& Sklitter::operator=(Sklitter&&) noexcept = default;

void Sklitter::runPostChain(float* left, float* right, int frames) {
	if (postChain_ == nullptr) {
		return;
	}
	// **THE SEVEN, PUBLISHED INWARD BEFORE THE CHAIN RUNS.** `performOut_` is the MOD road's copy —
	// what `advanceModulators` published this block and what every outer route from this device reads.
	// Taking it (rather than `target_`'s puck, which is the audio road's) is what makes `Perf X` mean
	// the same thing inside the chain as outside it.
	for (size_t n = 0; n < kPerformOutputs; ++n) {
		postChain_->publishCoreSource(n, performOutput(n));
	}
	// AFTER THE BLEND: `renderStereo` has already written this block's dry/wet mixture into these two
	// buffers, through `blendAt`, on every road it can have taken. See the header for why "after" and
	// not "on the wet" — and for the fact that this call is reached even when `Enabled` is down.
	postChain_->process(left, right, frames);
}

//--------------------------------------------------------------------------------------------------//
// `device::ContainerOps` — ONE-LINE FORWARDS, which is the whole point of `rack::InnerChain`.         //
// Nothing below decides anything; every decision is in that unit, where a second holder inherits it. //
//--------------------------------------------------------------------------------------------------//

const device::ContainerOps kContainerOps {
	.adopt =
	    [](void* storage, const device::Catalogue& catalogue) {
		    forEachLane(storage, [&](rack::InnerChain& c) { c.adopt(catalogue); });
	    },
	// **BOTH LANES IN ONE BODY** — gap G2's framing, `rack::writeChainBundle`. The lanes are named by
	// stable id rather than by position, so this device's body does not care which order they are in
	// or whether a later build has a third.
	.saveState =
	    [](const void* storage, const device::ByteSink& sink) {
		    const auto lanes = lanesOf(const_cast<void*>(storage));
		    return rack::writeChainBundle(lanes, sink);
	    },
	// ALL-OR-NOTHING ACROSS BOTH, which is what `ContainerOps::loadState` demands and what a device
	// with two chains has to work for: see `rack::readChainBundle`.
	.loadState =
	    [](void* storage, const uint8_t* data, size_t bytes) {
		    const auto lanes = lanesOf(storage);
		    return rack::readChainBundle(lanes, data, bytes);
	    },
	// TWO RACK BODIES PLUS THE FRAME. Large, and a ceiling rather than an expectation — it bounds how
	// far a reader will walk a length out of a file, and nothing allocates it.
	.maxBodyBytes = rack::bundleMaxBytes(2),
	.setOuterGate =
	    [](void* storage, const device::OuterGate& gate) {
		    forEachLane(storage, [&](rack::InnerChain& c) { c.setOuterGate(gate); });
	    },
	// **THE POOL OUTSIDE, TO BOTH LANES** (host slots T11: "SKLITTER's inner chains are containers too"). Both
	// lanes report under this device's one path, so each is stamped with its own `lane`; their devices' ids
	// come from one counter (`laneIds_`), so `[sklitter, id]` names one device in one lane.
	.setHostSlots =
	    [](void* storage, const device::HostSlotLink& link) {
		    uint8_t lane = 0;
		    forEachLane(storage, [&](rack::InnerChain& c) {
			    device::HostSlotLink stamped = link;
			    stamped.lane = lane++;
			    c.setHostSlots(stamped);
		    });
	    },
	// **TWO `RouteDrop`s MERGED INTO ONE, which is gap G7 and is now real rather than hypothetical.**
	// The vocabulary carries `count + firstSource + firstDestination`, and with two lanes the "first"
	// pair has lost which chain it came from. Counts add — that is the number a host SAYS — and the
	// first pair is the first lane's if it dropped anything, the second's otherwise. What is lost is
	// only which lane a named destination belonged to, and the host prefixes the chain's name anyway.
	.service =
	    [](void* storage) {
		    auto& d = deviceAt(storage);
		    device::RouteDrop merged {};
		    forEachLane(storage, [&](rack::InnerChain& c) {
			    const device::RouteDrop one = c.service();
			    if (one.count == 0) {
				    return;
			    }
			    if (merged.count == 0) {
				    merged.firstSource = one.firstSource;
				    merged.firstDestination = one.firstDestination;
			    }
			    merged.count += one.count;
		    });
		    (void)d;
		    return merged;
	    },
	// **THE PASS-THROUGH MODEL, EVEN THOUGH THIS DEVICE IS NOT POLY** (`kTraits.perVoiceControl` is
	// -1). The chain is not Poly either; its DEVICES are, each for itself. A host that runs voices
	// tells this device so, and a Poly device dropped into the chain then gets one instance per outer
	// voice — which is the same answer the rack gives, and costs one forward.
	.setVoices =
	    [](void* storage, size_t voices) {
		    forEachLane(storage, [&](rack::InnerChain& c) { c.setVoices(voices); });
	    },
	//--------------------------------------------------------------------------------------------//
	// THE FOUR ROUTE VERBS — dispatched on the inner device's id, which names ONE lane since G3.   //
	//--------------------------------------------------------------------------------------------//
	//
	// `forEachRouteFrom` asks BOTH, because the question is "everything this outer source reaches"
	// and that spans the lanes. The other three carry a `Destination` and so address one device; they
	// resolve it through `laneHolding`. Before the shared instance counter landed, both chains
	// answered for the same id and these three would have picked whichever was asked first.
	.forEachRouteFrom =
	    [](const void* storage, device::Source outer, void* ctx,
	       void (*fn)(void* ctx, device::Destination inner, float amount)) {
		    forEachLane(const_cast<void*>(storage),
		                [&](rack::InnerChain& c) { c.forEachRouteFrom(outer, ctx, fn); });
	    },
	.routeAmountFrom =
	    [](const void* storage, device::Source outer, device::Destination inner) {
		    const auto* c = laneHolding(storage, inner.device);
		    return c != nullptr ? c->routeAmountFrom(outer, inner) : 0.0f;
	    },
	.setRouteFrom =
	    [](void* storage, device::Source outer, device::Destination inner, float amount) {
		    auto* c = laneHolding(storage, inner.device);
		    return c != nullptr && c->setRouteFrom(outer, inner, amount);
	    },
	.routeTransferFrom =
	    [](const void* storage, device::Source outer, device::Destination inner) {
		    const auto* c = laneHolding(storage, inner.device);
		    return c != nullptr ? c->routeTransferFrom(outer, inner) : device::Transfer::Lin;
	    },
	.setRouteTransferFrom =
	    [](void* storage, device::Source outer, device::Destination inner, device::Transfer t) {
		    auto* c = laneHolding(storage, inner.device);
		    return c != nullptr && c->setRouteTransferFrom(outer, inner, t);
	    },
	.nameDestination =
	    [](const void* storage, device::Destination inner, char* out, size_t capacity) {
		    const auto* c = laneHolding(storage, inner.device);
		    return c != nullptr ? c->nameDestination(inner, out, capacity) : size_t { 0 };
	    },
	// A PRESET'S ROAD (backlog FO): both lanes, all or nothing, through their records while running.
	.applyState =
	    [](void* storage, const uint8_t* data, size_t bytes) {
		    const auto lanes = lanesOf(storage);
		    return rack::applyChainBundle(lanes, data, bytes);
	    },
};

/// THE CONTROLS READ PER SAMPLE WHEN MODULATED — see `AudioOps::windowedControls`.
constexpr std::array<uint8_t, 2> kWindowedControls { static_cast<uint8_t>(Param::Mix), static_cast<uint8_t>(Param::StereoWidth) };

const device::AudioOps kAudioOps {
	.stateBytes = sizeof(Sklitter),
	.stateAlign = alignof(Sklitter),
	.construct = [](void* storage) { new (storage) Sklitter(); },
	.destruct = [](void* storage) { static_cast<Sklitter*>(storage)->~Sklitter(); },
	.prepare = [](void* storage, double sampleRate, int maxFrames) {
		static_cast<Sklitter*>(storage)->prepare(sampleRate, maxFrames);
	},
	.reset = [](void* storage) { static_cast<Sklitter*>(storage)->reset(); },
	.setSlot = nullptr,
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    Row row = rowOf(Settings {}); // anything the host omits keeps its declared default
		    const size_t take = std::min(fieldsPresent, row.size());
		    for (size_t i = 0; i < take; ++i) {
			    row[i] = fields[i];
		    }
		    Settings s = stateOf(row);
		    // THE ANCHORED PUSH (backlog HT), read past the params; a row that predates it leaves no anchor.
		    for (size_t i = 0; i < kAnchorFloats; ++i) {
			    const size_t at = kAnchorFirst + i;
			    const float v = at < fieldsPresent ? fields[at] : 0.0f;
			    s.anchor[i] = std::isfinite(v) ? v : 0.0f;
		    }
		    static_cast<Sklitter*>(storage)->setTarget(s);
	    },
	// **THE NOTE REACHES THE LANES AND NOTHING ELSE** — this device has never read one. It is declared
	// because a keytracking filter inside a chain needs the pitch, and the whole note is forwarded
	// rather than only the gate, for exactly the reason `rack_device.cpp` gives.
	//
	// **BOTH LANES, and this one reached only the master until the grain lane's first full read.** A
	// seam that serves one chain of two is the shape this device's own `blendAt` header lists five
	// instances of; the grain lane would have held a keytracking filter that never heard a note.
	.setNoteContext =
	    [](void* storage, const device::NoteContext& note) {
		    forEachLane(storage, [&](rack::InnerChain& c) { c.setNoteContext(note); });
	    },
	// **ORDER MATTERS HERE AND THE COMPILER IS THE ONLY THING THAT SAYS SO.** C++20 designated
	// initialisers must follow declaration order, and `setTransportClock` sits BEFORE `processStereo`
	// in `device::AudioOps`. `automod.cpp` carries a note about tripping on the same edge.
	.setTransportClock =
	    [](void* storage, const device::TransportClock& clock) {
		    static_cast<Sklitter*>(storage)->setTransportClock(clock);
	    },
	.processStereo =
	    [](void* storage, float* left, float* right, int frames) {
		    auto* s = static_cast<Sklitter*>(storage);
		    // THE POST CHAIN IS INSIDE `processStereo`, not here (backlog EW). It was here first, and
		    // that was a footgun: every one of this device's own suites calls the method directly, so
		    // the chain would have run for the host and for nobody else — a road tested by nothing.
		    s->processStereo(left, right, frames);
		    s->publishReadings();
	    },
	.processMono = nullptr, // stereo only: a Sklitter buffer is two channels by construction
	.setReadingsSink =
	    [](void* storage, device::ReadingsSink sink) {
		    static_cast<Sklitter*>(storage)->setReadingsSink(sink);
	    },
	// **THE OTHER HALF OF THE OUTER GATE, AND WITHOUT IT THE FIRST HALF IS A LIE.**
	// `ContainerOps::setOuterGate` is the MESSAGE-THREAD half: it lets the chain's editor admit a route
	// from a source outside this device. This is the AUDIO half — the reader that finds that source's
	// cell. Declaring one without the other admits a route and then reads nothing from it, which is a
	// silent zero rather than an error.
	// BOTH LANES, for the reason `setNoteContext` above gives: `ContainerOps::setOuterGate` fans out,
	// so a reader that did not would admit a route into the grain lane and then read nothing from it.
	.setOuterSources =
	    [](void* storage, const device::OuterSources& outer) {
		    forEachLane(storage, [&](rack::InnerChain& c) { c.setOuterSources(outer); });
	    },
	// MIX AND STEREO WIDTH ARE READ PER SAMPLE WHILE THEY MOVE (backlog FK) — see `Sklitter::mixNow_`.
	.windowedControls = kWindowedControls,
	.setFieldWindows =
	    [](void* storage, const float* const* runs, size_t count, int) {
		    const auto mix = static_cast<size_t>(Param::Mix);
		    const auto width = static_cast<size_t>(Param::StereoWidth);
		    static_cast<Sklitter*>(storage)->setWindowRuns(mix < count ? runs[mix] : nullptr,
		                                                   width < count ? runs[width] : nullptr);
	    },
	// **THE POST CHAIN'S LATENCY, AND ONLY ITS** (backlog JM, assumption L8): it runs in series after the
	// blend (`runPostChain`), so whatever it delays, this device's output is late by. The GRAIN lane's is
	// not reported: it delays grains, which have no dry partner to align with — only a place in the bar.
	.latencySamples =
	    [](const void* storage, const float*, size_t, double sampleRate) -> uint32_t {
		    const rack::InnerChain* post = static_cast<const Sklitter*>(storage)->postChain();
		    return post != nullptr ? post->latencySamples(sampleRate) : 0;
	    },
	.holdsTail = [](const void* storage) { return static_cast<const Sklitter*>(storage)->holdsTail(); },
};

/// **THE SECOND OP TABLE OVER THE SAME OBJECT — design §4.1, and the storage is `kAudioOps`'.**
///
/// `rack_engine.cpp` takes a device's bytes from the FIRST pointer in the fixed order audio, mod, notes:
/// `buildSlot` reads `desc->audio->stateBytes` when `audio` is set and never looks at `mod`'s, and
/// `destructWith` destroys through `audio` and `else if mod` — so a dual device is constructed, prepared,
/// reset and destroyed EXACTLY ONCE, through the audio table. `construct`/`destruct`/`prepare`/`reset`
/// below are therefore never called for SKLITTER.
///
/// **THEY ARE STILL FILLED IN, AND NOT WITH NULLS, BECAUSE A TABLE MUST DESCRIBE THE CLASS IT SHARES
/// STORAGE WITH.** The sizes and the lifetime hooks are a statement about `Sklitter`, and one that
/// disagreed with `kAudioOps`' would be a lie that happens not to be read today — exactly the kind of
/// second copy this file's other notes are about. Anything that ever built through this table would get
/// the same object.
const device::ModOps kModOps {
	.stateBytes = sizeof(Sklitter),
	.stateAlign = alignof(Sklitter),
	.construct = [](void* storage) { new (storage) Sklitter(); },
	.destruct = [](void* storage) { static_cast<Sklitter*>(storage)->~Sklitter(); },
	// The mod table carries no longest block (`ModOps::prepare`), and this one is never called (above).
	.prepare = [](void* storage, double sampleRate) {
		static_cast<Sklitter*>(storage)->prepare(sampleRate, device::kDefaultMaxFrames);
	},
	.reset = [](void* storage) { static_cast<Sklitter*>(storage)->reset(); },
	// **THE PUCK ONLY** — only the audio road writes `target_`. See `Sklitter::setPerformFields`.
	//
	// **THE TWO ROADS GET THE SAME BUFFER, NOT THE SAME RESOLVE, AND THE DIFFERENCE HAS A SUBJECT.** An
	// earlier draft of this comment said "the SAME resolved row `kAudioOps.setFields` gets", which is
	// stronger than the engine promises. `RackEngine::beginPiece` runs `advanceModulators`, which calls
	// `resolveInto(slot, *d)` and then `mod->setFields`; `processBus` LATER calls `resolveInto` AGAIN on
	// the same `slot.resolved` before `audio->setFields`. Two resolves of one buffer at two instants in
	// one piece.
	//
	// For a puck at its BASE VALUE the two agree by construction — nothing writes the field between
	// them. For a ROUTED `Perf X`, which the design calls out by name (an LFO routed to Perf X reads
	// as speed), they agree only while every source feeding it published BEFORE this device's mod
	// resolve, and the bound when one does not is ONE control tick — 64 samples of that source's
	// movement.
	//
	// **WITHIN ONE CHAIN IT CANNOT HAPPEN, AND THAT IS MEASURED RATHER THAN ARGUED.** The walk is per
	// chain in PATH ORDER, so the only arrangement that could skew is a source standing AFTER this
	// device — and `forEachPrecedence` gives consecutive mod-carrying devices a Modulation edge, so that
	// route would close a cycle and `mayRoute` refuses it. Probed on a `RackEngine` holding SKLITTER and
	// a DAHDSR: with the envelope AFTER, `mayRoute(DHDS.out0 -> SKLITTER.PerfX)` is **false**; with it
	// BEFORE, **true** — and then it publishes before this device's mod resolve, so both roads read the
	// same number. The ordering that would produce the skew is exactly the ordering the routing rule
	// forbids.
	//
	// **SO IT TAKES A HOST WITH MORE THAN ONE CHAIN** — Feathers, whose fx lane and modulator strip are
	// two paths with no adjacency between them, where a source in a later-walked chain may legally feed
	// a device in an earlier one. A `RackEngine` has a single chain (`chainCount() == 1`), which is why
	// no case here demonstrates it.
	//
	// **WHAT THAT MEANS FOR A PATCH, said plainly rather than left to be discovered:** the published
	// weights are what the MOD road saw, and the sound uses what the AUDIO road saw. **It is not fixable
	// by making the audio road read `puckX_`** — that would make the sound depend on
	// `advanceModulators` having run at all, and a host that only renders would freeze the puck at the
	// centre. `targetPush()` carries the other half of this note.
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    static_cast<Sklitter*>(storage)->setPerformFields(fields, fieldsPresent);
	    },
	.blockSamples = kPerformTickSamples,
	// NO `ModTickContext` IS READ: the retrigger edge is a note-shaped fact and the puck's speed has
	// nothing to retrigger. A device that ignores the context is why the parameter is unnamed here.
	.tick = [](void* storage, const device::ModTickContext&) { static_cast<Sklitter*>(storage)->tickPerform(); },
	.outputAt = [](const void* storage, size_t output) {
		return static_cast<const Sklitter*>(storage)->performOutput(output);
	},
	// PER-NOTE AND PLAYHEAD ARE NULL, WHICH IS THE CAPABILITY SAYING NO: the circle is one gesture, not
	// one per voice, and it has no shape to be part-way through.
	.outputForNote = nullptr,
	.playhead = nullptr,
};

} // namespace feathers::fx::sklitter
