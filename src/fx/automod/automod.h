/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/registry.h"
#include "device/transport_lock.h"
#include "fx/automod/automod_comb.h"
#include "fx/automod/automod_derived.h"
#include "fx/automod/automod_envelope.h"
#include "fx/automod/automod_filter.h"
#include "fx/automod/automod_lfo.h"
#include "fx/automod/automod_spring.h"
#include "fx/automod/automod_types.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

// THE ASSEMBLY — six components, one coordinate, one signal path.
//
// THIS IS THE FILE THAT GIVES THE OTHER SIX A CALLER, and until it existed every one of them was
// half-installed: libraries with tests and nothing outside `tests/` reaching them. It is one commit
// on purpose. A half-wired effect is wired without being right, and nothing says so.
//
// ---------------------------------------------------------------------------------------------
// THE SIGNAL PATH, IN ORDER, AND WHY THAT ORDER.
// ---------------------------------------------------------------------------------------------
//
//   input -> envelope follower (reads, does not touch)
//         -> SVF, corner swept by the spring-smoothed LFO
//         -> comb, delay swept by its own spring off a phase-offset tap
//         -> wet/dry against the untouched input
//
// THE ENVELOPE READS THE INPUT, NOT THE FILTERED SIGNAL, which is the firmware's and is the only
// arrangement that does not feed back: an envelope reading the filter's output would open the filter,
// which would raise the output, which would open it further. The firmware's comment says "Envelope
// tracking on INPUT (before processing)" and the parenthesis is the whole reason.
//
// THE COMB COMES AFTER THE FILTER because it is an effect on the filtered signal rather than a
// parallel voice — its wet is ADDED, so putting it first would comb the dry signal and then filter
// the result, which is a different device.
//
// ---------------------------------------------------------------------------------------------
// A KNOWN CHARACTERISTIC: THE BOTTOM OF A WIDE COMB SWEEP FLATTENS AGAINST THE FLOOR.
// ---------------------------------------------------------------------------------------------
//
// Recorded HERE, and not only in `automod_derived.h` where it was found, because it is something a
// player MEETS: at some coordinates the comb's pitch stops falling partway down the LFO's swing and
// sits at the floor for part of the cycle. It is not a bug and it is not a clamp misfiring.
//
// The comb's base delay is placed in what is left after the modulation range is reserved AT THE TOP
// ONLY. Measured over the coordinate cube, `base - range` reaches **-194 samples** at the worst
// corner, so the clamp in `Comb::delayFor` is LOAD-BEARING at the bottom rather than defensive. That
// is the firmware's arrangement — `kMinDelay` 4 against a 400-sample range — so it is ported rather
// than introduced. Reserving the range at the bottom too would remove the flattening and move every
// short comb delay upward, which is a different instrument.
//
// ---------------------------------------------------------------------------------------------
// THE DERIVE RUNS WHEN THE COORDINATES MOVE, AND THE COORDINATES ARE THE ONLY KEY.
// ---------------------------------------------------------------------------------------------
//
// `deriveAutomod` is ~40 numbers out of nine inputs and is not free, so it is gated. But the gate is
// an EQUALITY ON THE INPUTS rather than a set of `prev` fields beside a mutable cache: the whole
// argument for a pure function is that its answer cannot be stale, and a gate keyed on anything
// other than the arguments would reintroduce exactly the staleness it removes.
//
// A `float` COMPARISON IS THE RIGHT GATE HERE AND NOT A TOLERANCE. The question is not "did this
// change audibly" but "is the value I hold the answer to the arguments I have" — and for that,
// bitwise equality is the only correct test. A tolerance would let the held answer drift away from
// its own inputs, which is the bug this design exists to make impossible.
//
// ---------------------------------------------------------------------------------------------
// THE MODULATION IS RE-READ ON A GRID ANCHORED TO THE STREAM, NOT TO THE CALLER'S BLOCK.
// ---------------------------------------------------------------------------------------------
//
// The first draft read the LFO once per `processStereo` call, which would have made the device's
// sound a function of the host's buffer size — eight times as many modulation updates at 64 frames
// as at 512. `multimode_filter.h` states the rule as a correctness constraint, and `test_synth.cpp`
// pins it: a performance must render identically in blocks of 1 and 37. See `beginTick`.

namespace feathers::fx::automod {

/// HOW OFTEN THE MODULATION IS RE-READ, in samples. 64 at 48 kHz is 750 Hz — the same grid
/// `mod::kBlockSamples` and `PuckMotion::kTickSamples` use, and the same one `multimode_filter`
/// steps its coefficients on. One number, anchored to the stream rather than to the caller's block,
/// which is what makes the device's sound independent of the host's buffer size.
inline constexpr int kTickSamples = 64;

/// **HOW LONG THE INPUT MUST HAVE BEEN EXACTLY SILENT BEFORE THE STATE IS CLEARED**, in seconds.
///
/// A COMB WITH FEEDBACK DOES NOT REACH EXACT ZERO ON ITS OWN, which is what this is for. At the
/// feedback the Type field can ask for, an echo decays geometrically — it becomes inaudible quickly
/// and becomes ZERO only when the last denormal flushes, and `test_fx_devices`'s tail case asks for
/// exact silence because a device still writing 1e-30 into a chain is still costing a downstream
/// device its denormal path.
///
/// THE CONDITION IS ABOUT THE INPUT, NOT THE OUTPUT, and `multimode_filter.h` states why in the same
/// words this borrows: "a delay line holds a full-scale sample while the output between taps is
/// nothing... So the condition is about the INPUT, which is the only thing that can promise there is
/// nothing left to come out." Our line is 2048 samples — 43 ms at 48 kHz — and one second is
/// twenty-three times that.
inline constexpr double kRestSilentSeconds = 1.0;

/// HOW FAR THE FREQ CONTROL MOVES THE DEVICE, in octaves either way. The firmware's own figure for
/// the comb side (`automodulator.cpp:1038`, "Full range (±0x80000000) = ±2 octaves"); the filter side
/// takes the same number rather than the firmware's linear half-range, because everything here is
/// logarithmic and one figure serving both is what keeps the two sides symmetric.
inline constexpr float kFreqOctaves = 2.0f;

/// WHAT `Freq` DOES TO THE FILTER'S CORNER, as a ratio — and it is a free function because TWO
/// callers need it and one of them is a picture.
///
/// **THE STRIP WAS PASSING A LITERAL 1.0 WHERE THE DSP PASSES THIS, AND THE CURVE THEREFORE DID NOT
/// MOVE WITH THE KNOB.** It showed twice: moving the Cutoff knob did not affect the drawn curve
/// (though both moved with the XY pad and the actual modulation), and then, after a fix that
/// addressed the REDRAW rather than the CONTENT, the curve still did not move with the Cutoff
/// knob. The first fix made the strip redraw when settings changed; it redrew the same
/// curve, because `coordsOf` does not carry `freq` and nothing downstream of `deriveAutomod` knew
/// about it. The second report is what said the gate was never the problem.
///
/// AND THE INTENT WAS ALREADY WRITTEN DOWN. `automod.cpp`'s call site says it goes "THROUGH
/// `cornerHzFor` RATHER THAN INLINE, so the card's strip draws THIS number rather than one it
/// computed itself" — and then the two call sites passed different fourth arguments, which is the
/// one way that arrangement can still fail. A shared function is what makes the agreement structural
/// instead of aspirational.
///
/// CLAMPED AS THE DSP CLAMPS IT. Two octaves either way, bounded to [0.25, 4] — the picture must
/// flatten where the sound flattens or it promises travel the corner does not have.
[[nodiscard]] inline float filterFreqRatioFor(float freq) {
	const float safe = std::isfinite(freq) ? std::clamp(freq, -1.0f, 1.0f) : 0.0f;
	return std::clamp(std::exp2(safe * kFreqOctaves), 0.25f, 4.0f);
}

//------------------------------------------------------------------------------------------------//
// THE TWO READINGS A SURFACE SHARES WITH THE SOUND.                                                //
//                                                                                                  //
// Both were written INLINE inside `Automod` and both are now free functions, for one reason: the    //
// card draws the waveform the device will play, and a picture assembled from its own copy of these  //
// is a second implementation of the instrument. CLAUDE.md's rule pointed at a picture.              //
//------------------------------------------------------------------------------------------------//

/// THE COORDINATES A SETTINGS BLOCK NAMES — the seven numbers `deriveAutomod` reads.
///
/// @param lfoHz the rate the device will run at (`Automod::effectiveRateHz()`, which needs the
///        transport clock). **IT REACHES ONLY THE SPRINGS.** `springHz`, `springZeta` and
///        `combSpringHz` scale with it; the waveform, the start phase, the filter base and the mod
///        depth do not depend on it at all. That is what lets a surface with no tempo draw the shape
///        faithfully by passing the un-synced rate, and it is asserted rather than asserted-about —
///        see the case that derives at two rates three decades apart and compares those four fields.
[[nodiscard]] inline AutomodCoords coordsOf(const Settings& s, float lfoHz) {
	AutomodCoords coords;
	coords.type = s.type;
	coords.flavor = s.flavor;
	coords.mod = s.mod;
	// **THE ROTATION IS NOT APPLIED HERE, AND THAT IS THE DESIGN RATHER THAN AN OMISSION.**
	//
	// It was, once: an earlier version turned the three stored offsets through `rotatePhases` on the
	// way into the derive, so that the honesty sweep would stop reporting Rotate as a dead control.
	// The consequence was audible — rotate changed the sound with the other controls stationary,
	// which it must not — and it is the same decision Explore's rotate already lives under
	// (`dsp/slice_basis.h`: a rotation that changed the sound would be redundant with gamma, so it
	// keeps the sound until you move in xy/gamma). A re-aiming is
	// where the NEXT gesture goes, never a change to the sound that is standing there.
	//
	// EXPLORE NEEDS A BANKED RESIDUE TO KEEP THAT PROMISE AND THIS DEVICE DOES NOT, and the reason is
	// what decided the shape here. Explore's state of record is a travel vector in a basis, so the
	// sound is `basis(rotate) * travel` and holding it still under a turn takes a third quantity
	// (A26's residue). Automod's state of record IS the standing sound: the three phase params are
	// the offsets the derive reads, and `automod_types.h` says in so many words that the puck is a
	// VIEW of them. So the invariant is free — the derive reads the phases as stored, and Rotate
	// reaches the sound only through what the pad WRITES next: `phaseOffsetsFor(x, y, rotate)`
	// decodes the next drag in the turned basis, and `puckForOffsets(o, rotate)` draws the standing
	// phases where that basis puts them, which is turning with the spokes.
	//
	// THE HONESTY SWEEP LISTS ROTATE AS SILENT UNDER ITS FIXTURE, AND SAYS WHY — the same entry
	// Drawn's band types have: a constraint on a future gesture, not a value in the signal path.
	// `test_fx_automod.cpp` pins both halves as values: the standing phases bit-identical across a
	// Rotate change off the centre, and the next decode landing somewhere else.
	// **THE THREE PER-COORDINATE PHASES ARE GONE — collapsed into the coordinates themselves (BJ
	// piece 3).** They were never a separate input to the derive: it summed each with its coordinate
	// on the way in. `coords.type` above now carries what that sum carried.
	//
	// **THE ROTATE INVARIANT IS UNTOUCHED, AND IT IS WORTH CHECKING RATHER THAN ASSUMING.** The
	// paragraph above turns on the standing phases not moving when Rotate turns — and the collapse
	// changes which PARAM holds them, not whether anything writes them during a turn. The pad writes
	// only the folded travel (`AutomodPad::commitAndReanchor` calls `blob_`, never the coordinate
	// params), so a turn still reaches the sound through nothing at all.
	// THE DEVICE GAMMA RIDES IN AS A PHASE, which is the firmware's arrangement and the design.
	coords.gammaPhase = s.gamma;
	coords.foldedTravel = s.foldedTravel;
	coords.lfoHz = lfoHz;
	return coords;
}

/// WHAT AN LFO TAP BECOMES BEFORE THE SPRINGS SEE IT: depth scales it and Manual is added after, so
/// Manual reaches the sound even at zero depth.
///
/// **STOP RETURNS MANUAL UNSCALED**, and that is not a shortcut — see `beginTick`, which used to
/// express it as a branch around the same arithmetic. In Stop the taps ARE Manual, so scaling them
/// by depth would make Manual a product of two knobs; folding the branch in here means the surface
/// and the sound cannot disagree about what Stop shows.
///
/// CLAMPED HERE RATHER THAN AT THE CALL, because a row is what a patch load hands over and both
/// readers take one — the playbook's 8a, that the guard belongs on the road the DATA takes.
[[nodiscard]] inline float modulationFor(const Settings& s, float tap) {
	const float depth = std::isfinite(s.depth) ? std::clamp(s.depth, 0.0f, 1.0f) : 0.0f;
	const float manual = std::isfinite(s.manual) ? std::clamp(s.manual, -1.0f, 1.0f) : 0.0f;
	if (s.lfoMode == LfoModeChoice::Stop) {
		return manual;
	}
	const float t = std::isfinite(tap) ? tap : 0.0f;
	return t * depth + manual;
}

/// **WHAT THE ENVELOPE DOES TO THE LFO — THE FIRMWARE'S THREE ROADS, RESTATED IN FLOAT.** Parity
/// port of `automodulator.cpp`'s per-buffer block (shoot for parity), which for
/// a month this device had carried as five derived numbers and ONE read (backlog EH). The reference:
///
///     absScale    = |depth|
///     envScale    = absScale * envDepthInfluence      * env
///     derivScale  = absScale * envDerivDepthInfluence * derivNorm
///     targetScale = saturate(depth + envScale + derivScale)
///     target      = (lfo + env * envValueInfluence) * targetScale  +  manual
///
/// THREE THINGS THE OLD ROAD GOT WRONG, each a real difference in the sound:
///   - it multiplied the spring's OUTPUT by `1 + env * envDepthInfluence`, so with Depth at zero the
///     envelope steered MANUAL — the firmware adds Manual after the scale, and gates every envelope
///     term by |depth|, so at Depth 0 the envelope does nothing at all;
///   - the derivative road was missing: an attack swells the sweep beyond what the sustained level
///     does, and a decay shrinks it — the part that makes a pick sound different from a pad;
///   - the value road was missing: the envelope is ADDED to the LFO before the scale, so the corner
///     follows loudness even where the LFO sits still — the auto-wah half of the effect.
/// The two PHASE roads (`envPhaseInfluence`, `envDerivPhaseInfluence`) are computed and unread here
/// exactly as in the firmware, whose `AUTOMOD_ENABLE_PHASE_PUSH` defaults to 0 — not parity's to add.
///
/// THE DERIVATIVE'S UNIT. The firmware's derivative is "envelope change over this buffer", smoothed,
/// clamped at ±1/64 and scaled by 64 to a ±1 norm — full scale at one sixty-fourth of full level per
/// 128-sample buffer at 44.1 kHz. Ours is per SECOND (see `EnvelopeFollower::derivative`), so the
/// same norm is the per-second rate over that buffer's worth of change per second:
inline constexpr float kFirmwareBufferSeconds = 128.0f / 44100.0f;
inline constexpr float kDerivFullScalePerSecond = (1.0f / 64.0f) / kFirmwareBufferSeconds; // ≈ 5.38 /s

struct EnvelopeSteer {
	float derivNorm = 0.0f; ///< the clamped, normalised derivative, ±1
	float envValue = 0.0f;  ///< what the envelope adds to the LFO's value
	float scale = 0.0f;     ///< the depth after the envelope and its derivative steered it
};

/// The block's steering numbers for one channel. `env` is the follower's level BOUNDED AT ONE —
/// `Automod::processFrame` explains why the bound is not optional in float — and `derivPerSecond` is
/// the follower's derivative. Pure, so the strip and a case can ask it the same question.
[[nodiscard]] inline EnvelopeSteer envelopeSteerFor(const Settings& s, const AutomodDerived& d, float env,
                                                    float derivPerSecond) {
	const float depth = std::isfinite(s.depth) ? std::clamp(s.depth, 0.0f, 1.0f) : 0.0f;
	const float level = std::isfinite(env) ? std::clamp(env, 0.0f, 1.0f) : 0.0f;
	const float rate = std::isfinite(derivPerSecond) ? derivPerSecond : 0.0f;
	EnvelopeSteer out;
	out.derivNorm = std::clamp(rate / kDerivFullScalePerSecond, -1.0f, 1.0f);
	out.envValue = level * d.envValueInfluence;
	const float envScale = depth * d.envDepthInfluence * level;
	const float derivScale = depth * d.envDerivDepthInfluence * out.derivNorm;
	out.scale = std::clamp(depth + envScale + derivScale, -1.0f, 1.0f);
	return out;
}

/// THE FILTER SPRING'S TARGET: the firmware's `(lfo + envValue) * targetScale + manual`. Stop is
/// Manual alone, as `modulationFor` has it. The COMB keeps `modulationFor` — the firmware scales its
/// comb by |depth| and adds Manual, with no envelope road (`automodulator.cpp`, the comb spring input).
[[nodiscard]] inline float springTargetFor(const Settings& s, float tap, const EnvelopeSteer& steer) {
	const float manual = std::isfinite(s.manual) ? std::clamp(s.manual, -1.0f, 1.0f) : 0.0f;
	if (s.lfoMode == LfoModeChoice::Stop) {
		return manual;
	}
	const float t = std::isfinite(tap) ? tap : 0.0f;
	return (t + steer.envValue) * steer.scale + manual;
}

class Automod {
public:
	void prepare(double sampleRate);
	void reset();

	/// WHERE THE SMOOTHER IS WALKING TO. The device's controls, in its own units.
	void setTarget(const Settings& s);
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	/// WHAT NOTE IS PLAYING, for the pitch tracking. `Shaper` is the precedent — the fx lane carries
	/// a note even though it carries no clock, which is a distinction worth remembering because the
	/// two look alike and only one of them is missing.
	void setNoteContext(const device::NoteContext& note);

	/// WHAT TIME IT IS IN THE SONG. The fx lane gained this in `cc45832`; this device is its first
	/// caller. Held rather than acted on — see `effectiveRateHz`.
	void setTransportClock(const device::TransportClock& clock);

	/// **HOW FAR THROUGH ITS CYCLE THE SONG PUTS THE LFO, 0..1, or nothing where it cannot say** —
	/// backlog DQ. Public because it is a VALUE and a claim about it cannot be recovered from four
	/// taps of a shape: "the phase is X at this instant" has to be readable to be asserted.
	[[nodiscard]] std::optional<double> lockedTurns() const;

	/// THE RATE THE LFO IS ACTUALLY RUNNING AT, in Hz — free or synced, whichever the controls and
	/// the host's tempo add up to. A test-only seam AND the real road: `refresh()` calls it, so a
	/// case reading it is reading the number the sound uses rather than a restatement of it.
	[[nodiscard]] float effectiveRateHz() const;

	/// **WHERE THE LFO IS, HANDED TO WHOEVER IS DRAWING IT.**
	///
	/// A PUSH RATHER THAN A PULL, and the pool is the reason. A device instance lives in fixed-size
	/// storage that `retire()` may hand to a different device, so a pointer the editor remembered is
	/// a contemporaneous handle and never an identity — CLAUDE.md's ABA rule, and a phase read off a
	/// remembered `Automod*` is exactly the shape it forbids. `Owltt::setMeterCallback` is the
	/// precedent and this is the same road, one float wide.
	///
	/// **CALLED FROM `beginTick`, WHICH IS THE SEAM BOTH PROCESS FUNCTIONS FUNNEL THROUGH.** A publish
	/// at the end of `processStereo` would have to be repeated in `processMono` and would be silently
	/// missing from whichever one a later edit forgot; there is one tick loop and it is here. That
	/// makes the rate `sampleRate / kTickSamples` — 750 Hz at 48 k — which is far above any frame
	/// rate a display needs and far below per-sample.
	///
	/// **BEFORE THE LFO IS ADVANCED**, so the number is the phase whose taps this tick is playing:
	/// the same phase the card's waveform is a picture of. `lfo_.tick()` reads then advances, so
	/// publishing after it would put a marker one tick ahead of the sound it claims to mark.
	///
	/// A DISABLED OR STOPPED DEVICE PUBLISHES NOTHING NEW, and that is correct rather than a gap: a
	/// bypassed device never reaches `beginTick` at all, and Stop does not advance the phase, so in
	/// both cases the last published number is still the true one.
	/// TWO READINGS, NOT ONE, AND THE NAME SAYS SO. This was `PhaseCallback` carrying a phase; it now
	/// also carries the ENVELOPE FOLLOWER'S LEVEL, so a surface can show what the follower is doing.
	/// Renamed rather than widened in place, because a `PhaseCallback` delivering two unrelated
	/// quantities is exactly the misleading name this file complains about elsewhere.
	///
	/// `envelope` IS POST-TRIM AND BOUNDED TO 1, i.e. the number the steering actually uses rather
	/// than the raw follower state — a meter showing an input that the DSP then clamps would be
	/// telling the truth about the wrong quantity.
	using ReadingsCallback = std::function<void(float phase, float envelope)>;
	void setReadingsCallback(ReadingsCallback callback);

	/// WHAT THE FOLLOWER IS AT, post-trim and bounded — the same number published above. Test-only.
	[[nodiscard]] float envelopeForTest() const noexcept { return publishedEnvelope_; }

	/// WHERE THE LFO'S PHASE ACTUALLY IS, 0..1 — the value, not four taps of a shape. A claim that the
	/// transport put the phase somewhere cannot be recovered from the readings: this file's oldest
	/// rule, and the same reason `Lfo::phase()` exists.
	[[nodiscard]] double lfoPhaseForTest() const noexcept { return lfo_.phase(); }

	/// IN PLACE, ANY n FROM 1 UPWARD, AND BIT-EXACTLY THE IDENTITY WHEN THE DEVICE IS OFF — the
	/// buffers are not read and not written, which is the bypass contract every device here keeps.
	/// The next call's Mix window, or null — see `mixNow_`.
	void setMixRun(const float* run) noexcept { pendingMixRun_ = run; }
	void processStereo(float* left, float* right, int frames);
	void processMono(float* mono, int frames);

	//-- test-only seams. Each exists because the value is not recoverable from the output. -------//

	/// The numbers the coordinates decided. A card draws from these and a case asserts them.
	[[nodiscard]] const AutomodDerived& derived() const noexcept { return derived_; }
	/// Where the LFO is, and what its four taps read.
	[[nodiscard]] const Lfo& lfo() const noexcept { return lfo_; }
	/// The filter's corner AFTER modulation — a claim about a cutoff cannot be read off a buffer.
	[[nodiscard]] float filterCutoffForTest() const noexcept { return lastCutoffHz_; }
	/// The note it was last handed, in semitones — what Keytrack follows. TEST ONLY.
	[[nodiscard]] float noteSemitonesForTest() const noexcept { return noteSemitones_; }
	/// THE RIGHT EAR'S CORNER — the filter has one per ear since backlog IS.
	[[nodiscard]] float filterCutoffRightForTest() const noexcept { return lastCutoffRightHz_; }
	/// WHERE A FILTER SPRING IS — the quantity a rebuild must move by its target's change and no more.
	[[nodiscard]] float filterSpringForTest(int ch) const noexcept { return filterSpring_[ch].position(); }
	/// THE BAND BLEND THE FILTER WAS LAST GIVEN, normalised — the ramp's output, one sample at a time.
	[[nodiscard]] const FilterMix& filterMixForTest() const noexcept { return filter_.mix(); }
	/// The comb's delay after modulation and clamping, in samples.
	[[nodiscard]] float combDelayForTest() const noexcept { return lastCombDelay_; }
	/// **WHETHER THE COMB'S LINE STILL HOLDS SOUND** — `AudioOps::holdsTail` (backlog MC): its echoes are up to 37 ms
	/// apart, with exact zeroes between them once the filter has decayed. Off, the first bypassed block clears it.
	[[nodiscard]] bool holdsTail() const noexcept { return target_.enabled && running_ && comb_.holdsSound(); }
	/// How many times the derive has actually run. THE GATE'S OWN COUNTER — "it only recomputes when
	/// the coordinates move" is a claim about work not done, and work not done leaves no trace in
	/// the audio.
	[[nodiscard]] long deriveCountForTest() const noexcept { return deriveCount_; }
	/// THE FILTER SPRING'S TARGET AND ITS PARTS, for the block most recently begun — see `BlockState`.
	/// Channel 0 is left. The envelope's three roads into the LFO (depth, derivative→depth, value)
	/// cannot be told apart in the audio, so a case reads the parts and restates the sum.
	[[nodiscard]] float springTargetForTest(int ch) const noexcept { return block_.springTarget[ch]; }
	[[nodiscard]] float lfoTapForTest(int ch) const noexcept { return block_.tap[ch]; }
	[[nodiscard]] float envValueForTest(int ch) const noexcept { return block_.envValue[ch]; }
	[[nodiscard]] float envScaleForTest(int ch) const noexcept { return block_.envScale[ch]; }
	[[nodiscard]] float derivNormForTest(int ch) const noexcept { return block_.derivNorm[ch]; }

private:
	/// Recompute the derived layer if and only if the coordinates differ from the ones it was built
	/// from, then push the result into the components.
	/// WHAT A TICK'S REFRESH DID: whether it rebuilt the waveform, whether that was the first build, and the
	/// LFO read at the SAME phase under the old shape and the new one — the pair the springs move by.
	struct Rebuild {
		bool rebuilt = false;
		bool first = false;
		LfoTaps before {};
		LfoTaps after {};
	};
	Rebuild refresh(float width);

	/// Everything decided once per TICK — see the definition for why a tick is not a block.
	void beginTick();
	/// One sample through the whole chain, both channels.
	void processFrame(float& left, float& right);
	/// Count exactly-silent input and clear the state once there has been enough of it.
	void noteInput(float left, float right);

	/// WHAT beginBlock DECIDED. Held rather than passed because `processFrame` takes a sample and
	/// nothing else — a frame function with eight parameters is a block function in disguise.
	struct BlockState {
		float springTarget[2] {};
		float combTarget[2] {};
		FilterMix mixVector {};
		float mix = 0.0f;
		/// THE PARTS THE FILTER'S SPRING TARGET WAS BUILT FROM, kept so a case can restate the sum —
		/// `(tap + envValue) * envScale + manual` — from the same numbers the sound used. The LFO's tap
		/// this block read; the envelope's contribution to the LFO's VALUE; the depth scale after the
		/// envelope and its derivative have steered it; and the normalised derivative that fed it.
		float tap[2] {};
		float envValue[2] {};
		float envScale[2] {};
		float derivNorm[2] {};
	} block_ {};
	/// **MIX, PER SAMPLE, WHILE A ROUTED SOURCE MOVES IT** (backlog FK): the next call's tick window (`setMixRun`), taken by
	/// that call; `mixNow_` is the sample's value, which the wet/dry line reads — `block_.mix` when nothing moves.
	const float* pendingMixRun_ = nullptr;
	const float* mixRun_ = nullptr;
	float mixNow_ = 0.0f;

	/// WHERE WE ARE IN THE TICK GRID, 0..kTickSamples-1, counted from the start of the stream.
	int tickPhase_ = 0;

	/// The coordinates the held `derived_` was computed from. Compared bitwise — see the header.
	AutomodCoords coordsWas_ {};
	AutomodDerived derived_ {};
	bool derivedValid_ = false;
	long deriveCount_ = 0;

	Settings target_ {};
	/// Whether the last block ran — so the first bypassed block resets, once (backlog EH, shape 6;
	/// the multimode filter's `active_`). A comb that kept its line across a bypass replayed it.
	bool running_ = false;
	double sampleRate_ = 48000.0;
	/// The song's clock, or `unknown()` when the host has said nothing.
	device::TransportClock clock_ {};
	/// Where the song is and the LFO's period, in ONE unit (beats when synced with a tempo, seconds
	/// otherwise), with a tick's length in it — what `lockedTurns` divides and `lock_` follows.
	struct SongPeriod {
		double position;
		double period;
		double tickLength;
		int road;
	};
	static constexpr int kBeatsRoad = 1;
	static constexpr int kSecondsRoad = 2;
	[[nodiscard]] std::optional<SongPeriod> songPeriod() const;
	/// FREE on a playing transport follows the song through this — see `device::TransportLock`.
	device::TransportLock lock_ {};

	/// WHO IS DRAWING THE READINGS, or empty. Checked before every call — see `setReadingsCallback`.
	ReadingsCallback readingsCallback_;
	/// THE LAST ENVELOPE LEVEL PUBLISHED, post-trim and bounded. Kept so the test seam and the
	/// publication cannot disagree: both read this one field.
	float publishedEnvelope_ = 0.0f;

	Lfo lfo_ {};
	Filter filter_ {};
	Comb comb_ {};
	EnvelopeFollower envelope_ {};
	/// THE ENVELOPE TRIM AS A LINEAR GAIN — `Settings::envTrim` is in dB and this is 10^(dB/20),
	/// converted once in `setTarget` rather than per sample.
	float envTrimGain_ = 0.12589254f; // 10^(-18/20), the declared default
	/// ONE SPRING CLASS, FOUR INSTANCES: the filter's and the comb's, per channel.
	Spring filterSpring_[2] {};
	Spring combSpring_[2] {};

	/// PITCH TRACKING, cached against the note that produced it — a `exp2` per block for a number
	/// that changes on a note-on is waste, and the note number is the honest key because it is what
	/// the ratio is a function of.
	float noteSemitones_ = 60.0f;
	float filterPitchRatio_ = 1.0f;
	float combPitchRatio_ = 1.0f;
	/// THE SAME PAIR, FROM THE FREQ KNOB RATHER THAN FROM THE NOTE. Kept separate from the pitch pair
	/// so each stays clamped on its own, as the firmware clamps them — see `beginTick`.
	float filterFreqRatio_ = 1.0f;
	float combFreqRatio_ = 1.0f;
	bool gateWas_ = false;

	float lastCutoffHz_ = 1000.0f;
	float lastCutoffRightHz_ = 1000.0f;
	/// THE TICK'S RAMPS — the band blend and the comb's base delay walk from the last tick's value to this
	/// one's across the tick rather than stepping at its start. See `beginTick`.
	FilterMix mixFrom_ {};
	FilterMix mixTo_ {};
	float combBaseFrom_ = 0.0f;
	float combBaseTo_ = 0.0f;
	bool rampsPrimed_ = false;
	float lastCombDelay_ = 0.0f;

	/// HOW LONG THE INPUT HAS BEEN EXACTLY SILENT, in samples, and how long it must be before the
	/// state is cleared. See `kRestSilentSeconds`.
	long silentRun_ = 0;
	long restSilenceNeeded_ = 48000;
	/// TRUE FROM THE FLUSH UNTIL THE NEXT NON-ZERO INPUT SAMPLE: the envelope, the filter and the comb hold exact zeros,
	/// so a zero input frame through them is a zero output frame and the work is skipped (`processFrame`).
	bool atRest_ = false;
	size_t framesChained_ = 0; ///< frames that ran the filter and the comb (test seam)
	size_t framesSkipped_ = 0; ///< frames at rest that did not
	bool restSkip_ = true;
public:
	/// TEST-ONLY: false runs the whole chain at rest too (the path before the skip existed), for a bit-identity comparison.
	void setRestSkipForTest(bool on) noexcept { restSkip_ = on; }
	[[nodiscard]] size_t framesChainedForTest() const noexcept { return framesChained_; }
	[[nodiscard]] size_t framesSkippedForTest() const noexcept { return framesSkipped_; }
};

} // namespace feathers::fx::automod
