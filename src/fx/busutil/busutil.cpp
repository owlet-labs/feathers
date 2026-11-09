/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "fx/busutil/busutil_types.h"

#include "fx/busutil/busutil_meter.h"

#include <algorithm>
#include <cmath>
#include <new>

// BUS UTILITY'S RENDER PATH — four coefficients, slewed toward their targets over a fixed TIME.
//
// THERE IS NO DSP STATE HERE BEYOND THE SLEW'S POSITION, which is the point of composing the device
// into one linear map: nothing is filtered, delayed or remembered, so there is nothing to reset
// except where the slew had got to.
//
// **AND THE SLEW IS COUNTED IN SAMPLES, NOT IN BLOCKS, BECAUSE THE FIRST VERSION WAS NOT.** It
// interpolated across whatever buffer arrived, which made the host's block size audible —
// `test_fx_runner`'s partition case measured a worst |diff| of 0.0844 on the same audio through the
// same settings. See `kRampSeconds`.

namespace feathers::fx::busutil {

namespace {

struct OpsBusUtil {
	Settings state {};
	/// WHAT THE SAMPLE JUST RENDERED USED — the slew's running position, not a setting. **PER SAMPLE
	/// AND CARRIED ACROSS BLOCK BOUNDARIES**, which is what makes the device partition-independent.
	Matrix current {};
	/// WHAT `state` COMPOSES TO — recomputed only when the row changes, never per sample.
	Matrix target {};
	/// THE PER-SAMPLE INCREMENT WHILE `remaining > 0`. Zero at rest, and it must be spelled: a
	/// default-constructed `Matrix` is the IDENTITY, which as a step would add 1.0 to the diagonal
	/// every sample.
	Matrix step { .ll = 0.0f, .lr = 0.0f, .rl = 0.0f, .rr = 0.0f };
	/// SAMPLES LEFT OF THE CURRENT SLEW. Counted down per sample, never per block.
	int remaining = 0;
	/// `kRampSeconds` IN SAMPLES, so the ramp is a fixed TIME rather than a fixed fraction of
	/// whatever buffer the host happened to hand over.
	int rampSamples = 1;
	/// **HAS A BLOCK EVER RUN?** Until one has, `current` is meaningless and the first block must
	/// START at the target rather than sliding toward it from an invented origin. Without this a
	/// freshly seated device fades its own first block in — audible on a chain added mid-note, and
	/// invisible to any test that only looks at settled output.
	bool primed = false;

	/// **THIS CALL'S MOVING CONTROLS, PER SAMPLE** (backlog FK) —
	/// Gain, Pan, Pan L, Pan R, Width, in that order; null where the control holds its `state` value. Valid
	/// for the one `processStereo` that follows `setFieldWindows`, then cleared.
	static constexpr size_t kWindowedCount = 5;
	static constexpr int kWindowStep = 8;
	std::array<const float*, kWindowedCount> runs {};
	bool windowed = false;
	size_t windowedCalls = 0; ///< TEST ONLY — see `windowedCallsForTest`

	/// **THE METER READS THE OUTPUT**, so it is fed at the END of the block rather than alongside the
	/// matrix. The device's job is setting a level and the number a hand needs is what is LEAVING it;
	/// a meter ahead of the gain would move when nothing about the mix had.
	Meter meter {};

	/// **THE DC / RUMBLE FILTER, AND IT RUNS BEFORE THE METER.** It should be pre metering of
	/// course. Which is not merely an ordering preference — the meter's job is to report what
	/// LEAVES this device, so a filter downstream of it would have the numbers describing a signal
	/// nobody receives, and the LED would light on subsonic energy the output does not carry.
	///
	/// **TWO POLES, BUTTERWORTH, 12 dB PER OCTAVE.** It was asked whether a brickwall would work
	/// better; at these corners it would not, and the reason is arithmetic rather than taste. A 20 Hz
	/// cycle is 50 ms, so a filter that discriminates SHARPLY there must observe several cycles — and
	/// that cost lands either as group delay peaking at the corner (a high-order IIR smearing exactly
	/// the bass transients you were protecting) or as hundreds of milliseconds of latency and
	/// PRE-RINGING (a linear-phase FIR, which would also make this the only device in the chain
	/// reporting latency). What the first version was actually short of was SLOPE, not shape: one
	/// pole at a 30 Hz corner leaves 15 Hz only 6 dB down.
	///
	/// **NOT TWO ONE-POLES IN SERIES**, which is the tempting cheap version and is a different filter:
	/// cascading gives Q = 0.5 and -6 dB at the corner where Butterworth is 0.7071 and -3, so the
	/// knee sags and the corner is not where it says it is. A real biquad, designed by the RBJ
	/// formulae, reusing the `Biquad` the meter already carries.
	Biquad dcL {};
	Biquad dcR {};
	/// IS THE STAGE RUNNING AT ALL — false at `Off`, which is a genuine skip rather than a corner
	/// set low.
	bool dcOn = false;
	double sampleRate = 48000.0;

	/// **THE COEFFICIENTS ARE A FUNCTION OF A CORNER AND A RATE**, so they are derived here rather
	/// than stored, and at `Off` there is no corner at all.
	void refreshFilter() {
		const float hz = highpassHz(state.highpass);
		dcOn = hz > 0.0f;
		if (dcOn) {
			// RBJ HIGHPASS AT Q = 1/sqrt(2), which IS Butterworth: maximally flat passband, -3 dB at
			// the corner, 12 dB/oct beyond it — COMP's sidechain design (backlog JJ).
			// **THE STATE IS CARRIED ACROSS A RE-DESIGN, NOT RESET** (`designHighpass` writes only
			// coefficients). Moving the corner while audio is running must not silence the filter for
			// a block; only a BYPASS clears it.
			dcL.designHighpass(static_cast<double>(hz), sampleRate);
			dcR.designHighpass(static_cast<double>(hz), sampleRate);
		}
		if (hz <= 0.0f) {
			// **CLEARED WHEN BYPASSED, so switching it back on starts from silence rather than from
			// whatever the filter was holding when it was switched out.** A stage that keeps its
			// state across a bypass replays it on the way back in — the poly-switch defect the fx
			// runner records, one device along.
			dcL.clear();
			dcR.clear();
		}
	}
	ReadingsCallback onReadings = nullptr;
	void* readingsContext = nullptr;
	/// THE HOST'S SINK (`AudioOps::setReadingsSink`), kept here so `readingsContext` can point at it.
	device::ReadingsSink sink;

	/// LATCH, THEN RECOMPUTE — the order-independence `registry.h` requires of any device with more
	/// than one input road. `setFields` is the only road here today; the rule is the contract's and
	/// not this device's to relax.
	///
	/// **THE SLEW IS RE-AIMED FROM WHEREVER IT IS**, not from where the last target was, so a control
	/// moved twice inside one ramp does not jump back to restart.
	void refresh() {
		target = matrixOf(state);
		refreshFilter();
		if (!primed) {
			// NOTHING HAS RENDERED YET, so there is no position to slew from and no click to avoid.
			current = target;
			remaining = 0;
			step = Matrix { .ll = 0.0f, .lr = 0.0f, .rl = 0.0f, .rr = 0.0f };
			return;
		}
		const float n = static_cast<float>(rampSamples);
		step = Matrix { .ll = (target.ll - current.ll) / n,
			            .lr = (target.lr - current.lr) / n,
			            .rl = (target.rl - current.rl) / n,
			            .rr = (target.rr - current.rr) / n };
		remaining = rampSamples;
	}
};

OpsBusUtil& asBusUtil(void* storage) { return *static_cast<OpsBusUtil*>(storage); }

} // namespace

Matrix matrixForTest(const void* storage) { return static_cast<const OpsBusUtil*>(storage)->target; }

size_t windowedCallsForTest(const void* storage) { return static_cast<const OpsBusUtil*>(storage)->windowedCalls; }

const Meter& meterForTest(const void* storage) { return static_cast<const OpsBusUtil*>(storage)->meter; }

void setReadingsCallback(void* storage, ReadingsCallback callback, void* context) noexcept {
	OpsBusUtil& ops = asBusUtil(storage);
	ops.onReadings = callback;
	ops.readingsContext = context;
}

namespace {
/// THE CONTROLS READ PER SAMPLE WHEN MODULATED, in `OpsBusUtil::runs` order — see `AudioOps::windowedControls`.
constexpr std::array<uint8_t, 5> kWindowedControls { static_cast<uint8_t>(Param::Gain), static_cast<uint8_t>(Param::Pan),
	                                                  static_cast<uint8_t>(Param::PanL), static_cast<uint8_t>(Param::PanR),
	                                                  static_cast<uint8_t>(Param::Width) };
static_assert(kWindowedControls.size() == OpsBusUtil::kWindowedCount, "one run slot per declared control");
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsBusUtil),
    .stateAlign = alignof(OpsBusUtil),
    // CONSTRUCTED INTO STORAGE THE HOST OWNS, because the host is the one under
    // no-allocation-after-activate. The device says how big it is; where it lives is not its business.
    .construct = [](void* storage) { new (storage) OpsBusUtil(); },
    .destruct = [](void* storage) { asBusUtil(storage).~OpsBusUtil(); },
    // **THE RAMP LENGTH IS THE ONE RATE-DEPENDENT THING HERE, AND IT IS THE WHOLE REASON THIS IS NOT
    // EMPTY.** It was empty, under a comment claiming the device had no rate-dependent state at all —
    // true of every coefficient (a gain, a cosine, a sign) and false of the smoothing, which was a
    // fixed fraction of the BLOCK and therefore made the host's buffer size audible. See
    // `kRampSeconds`.
    .prepare =
        [](void* storage, double sampleRate, int) {
	        OpsBusUtil& ops = asBusUtil(storage);
	        // AT LEAST ONE SAMPLE, so a nonsense rate cannot produce a zero-length ramp and a
	        // division by zero in `refresh`.
	        ops.rampSamples = std::max(
	            1, static_cast<int>(std::lround(kRampSeconds * std::max(sampleRate, 1.0))));
	        ops.meter.prepare(sampleRate);
	        // **AND THE RATE THE HIGHPASS IS DESIGNED AT** (backlog EH, shape 7: a cache key short of an
	        // input). `sampleRate` was a member initialised to 48 kHz that nothing ever assigned, so
	        // the rumble corner was designed for 48 kHz whatever the host ran at — an octave high at
	        // 96 kHz — and `refreshFilter` ran only from `refresh`, never from here. The rate is an input
	        // of the coefficient, so a new rate redesigns it.
	        ops.sampleRate = std::max(sampleRate, 1.0);
	        ops.refreshFilter();
        },
    // "AS IF JUST SWITCHED ON" — which for this device means the next block starts AT its target
    // instead of sliding to it from wherever the last one ended. Dropping the primed flag and the
    // slew in flight is the whole of the reset; there is no filter memory to forget.
    .reset =
        [](void* storage) {
	        OpsBusUtil& ops = asBusUtil(storage);
	        ops.primed = false;
	        ops.remaining = 0;
	        ops.meter.reset();
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, which is what puts absent fields at their DECLARED
	        // defaults rather than at zero — device_row.h's rule, arriving as a bare pointer and a
	        // count because the device vocabulary cannot see the host's DeviceRow. For THIS device
	        // the difference is loud: a row read as zeros is a mono, centre-collapsed, silent-ish
	        // signal rather than a pass-through.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asBusUtil(storage).state = stateOf(row, take);
	        asBusUtil(storage).refresh();
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        OpsBusUtil& ops = asBusUtil(storage);
	        if (!ops.primed) {
		        ops.current = ops.target;
		        ops.remaining = 0;
		        ops.primed = true;
	        }
	        // A MODULATED MATRIX FOLLOWS THE TICK'S WINDOWS. The matrix is composed every `kWindowStep`
	        // samples from the controls' values at the END of that step and reached by a straight line across
	        // it — the stepped tier: trig every 8 samples, a linear slide in between, no stair at the tick.
	        if (ops.windowed) {
		        ++ops.windowedCalls;
		        for (int start = 0; start < frames; start += OpsBusUtil::kWindowStep) {
			        const int len = std::min(OpsBusUtil::kWindowStep, frames - start);
			        Settings at = ops.state;
			        const int last = start + len - 1;
			        if (ops.runs[0] != nullptr) at.gain = ops.runs[0][last];
			        if (ops.runs[1] != nullptr) at.pan = ops.runs[1][last];
			        if (ops.runs[2] != nullptr) at.panL = ops.runs[2][last];
			        if (ops.runs[3] != nullptr) at.panR = ops.runs[3][last];
			        if (ops.runs[4] != nullptr) at.width = ops.runs[4][last];
			        const Matrix goal = matrixOf(at);
			        const float n = static_cast<float>(len);
			        const Matrix d { .ll = (goal.ll - ops.current.ll) / n,
				                     .lr = (goal.lr - ops.current.lr) / n,
				                     .rl = (goal.rl - ops.current.rl) / n,
				                     .rr = (goal.rr - ops.current.rr) / n };
			        for (int i = start; i < start + len; ++i) {
				        if (i == start + len - 1) {
					        ops.current = goal; // land exactly
				        }
				        else {
					        ops.current.ll += d.ll;
					        ops.current.lr += d.lr;
					        ops.current.rl += d.rl;
					        ops.current.rr += d.rr;
				        }
				        const float inL = left[i];
				        const float inR = right[i];
				        left[i] = ops.current.ll * inL + ops.current.lr * inR;
				        right[i] = ops.current.rl * inL + ops.current.rr * inR;
			        }
			        ops.target = goal;
		        }
		        ops.remaining = 0;
		        ops.windowed = false;
		        ops.runs = {};
	        }
	        else
	        for (int i = 0; i < frames; ++i) {
		        // **THE SLEW ADVANCES PER SAMPLE AND KNOWS NOTHING ABOUT THE BLOCK**, which is the
		        // whole of the partition-independence: rendering 128 frames or two lots of 64 walks
		        // the same positions in the same order.
		        if (ops.remaining > 0) {
			        ops.current.ll += ops.step.ll;
			        ops.current.lr += ops.step.lr;
			        ops.current.rl += ops.step.rl;
			        ops.current.rr += ops.step.rr;
			        if (--ops.remaining == 0) {
				        // **LAND ON THE TARGET EXACTLY RATHER THAN WHERE THE ADDITIONS ARRIVED.**
				        // `rampSamples` increments of a float do not sum to the difference, so
				        // without this a settled device sits a few ULPs off unity forever — and the
				        // sample-exact identity below would become a tolerance nobody could justify.
				        ops.current = ops.target;
			        }
		        }
		        // **STEADY STATE IS BIT-EXACT AND THAT IS LOAD-BEARING.** At rest nothing is added at
		        // all, so a settled device at its defaults multiplies by exactly 1 and 0 and
		        // `test_fx_busutil.cpp` asserts the identity sample-exact.
		        //
		        // READ BOTH INPUTS BEFORE WRITING EITHER OUTPUT. The off-diagonal terms mean each
		        // output depends on both inputs, so writing `left[i]` first and then reading it for
		        // `right[i]` would feed the new left sample into the right one — a mistake that is
		        // silent at the defaults, where the off-diagonals are zero, and only appears once
		        // somebody moves Width or a pan.
		        const float inL = left[i];
		        const float inR = right[i];
		        left[i] = ops.current.ll * inL + ops.current.lr * inR;
		        right[i] = ops.current.rl * inL + ops.current.rr * inR;
	        }

	        // **THE FILTER, BETWEEN THE MATRIX AND THE METER.** Skipped ENTIRELY at `Off` — not run
	        // with a coefficient of zero, which would still read and write every sample and cost the
	        // device its bit-exact identity. A `if` per block, not per sample.
	        if (ops.dcOn) {
		        for (int i = 0; i < frames; ++i) {
			        left[i] = static_cast<float>(ops.dcL.step(static_cast<double>(left[i])));
			        right[i] = static_cast<float>(ops.dcR.step(static_cast<double>(right[i])));
		        }
	        }

	        // **THE METER SEES THE OUTPUT, AFTER THE WHOLE BLOCK.** Not interleaved with the matrix
	        // loop: the buffers now hold what is leaving the device, which is what the numbers are
	        // about, and one pass over settled samples is also the cheaper shape.
	        ops.meter.process(left, right, frames);

	        // **THE CLIPPER, AFTER THE METER, AND THAT ORDER IS THE DESIGN RATHER THAN AN
	        // IMPLEMENTATION DETAIL.** Ahead of the meter it would guarantee the 0 dBFS LED never
	        // lights — nothing downstream of a clipper exceeds full scale — so the indicator that
	        // tells you it is working would be silenced by the thing it is reporting on. Here the LED
	        // lights exactly when the clipper has something to do.
	        //
	        // NO OVERSAMPLING AND NO LOOKAHEAD. A knee this shape aliases when driven hard, which is
	        // a property of every clipper that does not oversample and is not a defect to be surprised
	        // by later — it is the reason the curve is available rather than only the clamp.
	        if (clipEnabled(ops.state.clip)) {
		        const float knee = clipKnee(ops.state.clip);
		        for (int i = 0; i < frames; ++i) {
			        left[i] = clipSample(left[i], knee);
			        right[i] = clipSample(right[i], knee);
		        }
	        }

	        if (ops.onReadings != nullptr) {
		        // A FIXED-SIZE LOCAL, filled in `Reading`'s declared order — the enum IS the contract
		        // between this line and the card, which is what stops the two becoming lists that
		        // agree by habit. automod's readings road records the alternative: a first version
		        // answered index 0's value for ANY index, and a second declared reading silently
		        // returned the first one's number.
		        float values[static_cast<size_t>(Reading::kCount)];
		        for (size_t i = 0; i < static_cast<size_t>(Reading::kCount); ++i) {
			        values[i] = ops.meter.reading(static_cast<Reading>(i));
		        }
		        ops.onReadings(ops.readingsContext, values, static_cast<size_t>(Reading::kCount));
	        }
        },
    // **NO MONO ROAD, SPELLED RATHER THAN DEFAULTED.** `registry.h`: a host holding a mono buffer for
    // a device with no mono road knows to hand it two channels instead of guessing. This device is
    // stereo by nature and bus-only by design — pan, dual pan and width have no meaning on one
    // channel, and a mono road here could only be a gain stage wearing this device's name.
    .processMono = nullptr,
    // THE HOST'S EAR FOR THE METER (`device::ReadingsSink`) — the same road `setReadingsCallback` opens for
    // a plugin's own wiring, installed by the engine for every host.
    .setReadingsSink =
        [](void* storage, device::ReadingsSink sink) {
	        OpsBusUtil& ops = asBusUtil(storage);
	        ops.sink = sink;
	        ops.onReadings = [](void* context, const float* values, size_t count) {
		        const auto* s = static_cast<const device::ReadingsSink*>(context);
		        if (s->publish != nullptr) {
			        s->publish(s->context, values, count);
		        }
	        };
	        ops.readingsContext = &ops.sink;
        },
    .setOuterSources = nullptr,
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    // **THE FIVE CONTROLS THAT SHAPE THE MATRIX FOLLOW MODULATION INSIDE A TICK** (backlog FK): Gain, Pan, Pan L,
    // Pan R and Width. The switches, the highpass corner and the clipper are not modulatable at all.
    .windowedControls = kWindowedControls,
    // The moving ones for the next call — see `OpsBusUtil::runs`.
    .setFieldWindows =
        [](void* storage, const float* const* runs, size_t count, int frames) {
	        OpsBusUtil& ops = asBusUtil(storage);
	        (void)frames;
	        ops.windowed = false;
	        for (size_t k = 0; k < kWindowedControls.size(); ++k) {
		        const auto c = static_cast<size_t>(kWindowedControls[k]);
		        ops.runs[k] = c < count ? runs[c] : nullptr;
		        ops.windowed = ops.windowed || ops.runs[k] != nullptr;
	        }
        },
};

} // namespace feathers::fx::busutil
