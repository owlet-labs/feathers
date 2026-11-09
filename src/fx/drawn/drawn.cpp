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

#include "fx/drawn/drawn.h"

#include "dsp/fast_math_approx.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <new>

namespace feathers::fx::drawn {

namespace {

/// LERP EVERY STORED FLOAT OF A FRAME TOWARD ANOTHER. The four floats of a band and nothing else,
/// which is what keeps the smoother in the pole-zero domain — see `DrawnFilter::smoothed_`.
///
/// **TWO COEFFICIENTS, AND THE GAP BETWEEN THEM IS `Wake`.** The order and the expressions are
/// unchanged from the single-alpha version, so passing the same number twice reproduces it BIT FOR
/// BIT — which is what `wake == 0` does and what the first case asserts.
/// ONE FIELD'S STEP, AND IT PARKS ON THE TARGET RATHER THAN NEAR IT.
///
/// **A ONE-POLE IN FLOAT NEVER ARRIVES.** Once `(target - current) * alpha` falls below half an ulp
/// of `current` the addition rounds to nothing and the value stops, parked short by roughly one ulp
/// over alpha. **That residual is proportional to 1/alpha**, so a slow road parks further out than a
/// fast one — invisible while poles and zeros share a constant, and visible the instant `Wake` splits
/// them.
///
/// MEASURED, before this existed: with the puck moved and everything settled, an all-`Bypass` patch
/// came to rest 0.000329 dB from flat at `Wake` 0.25 and **0.0147 dB at `Wake` 1.0**, growing with
/// the knob. Inaudible, and still exactly what design §8 forbids — *"a control that permanently
/// coloured the curve would be a second Res"*. The zeros were parking a hair off the poles and
/// STAYING there.
///
/// **THE RULE IS "WHEN IT CAN NO LONGER MOVE, PARK ON IT", AND IT NEEDS NO CONSTANT.** A fixed
/// epsilon cannot do this job: the parking distance depends on alpha, so a threshold small enough to
/// be inaudible for the fast road is never reached by the slow one — MEASURED, a first attempt at
/// 1e-6 settled `Wake` 0 exactly and left every higher setting parked exactly where it had been. A
/// step that produced no change while the target is elsewhere IS the parking point, whatever alpha
/// is, so that is the test. Both roads use it, so at `Wake == 0` they move and land in lockstep and a
/// `Bypass` band stays bit-exactly bypassed the whole way.
float step(float current, float target, float alpha) {
	const float next = current + (target - current) * alpha;
	// STALLED SHORT: the one-pole cannot close the remaining distance, so close it.
	return (next == current && current != target) ? target : next;
}

/// One field's step, and whether it changed the field's BITS (so a -0 for a +0, or one NaN for another, counts as moved).
bool stepInto(float& current, float target, float alpha) {
	const float next = step(current, target, alpha);
	const bool moved = std::bit_cast<uint32_t>(next) != std::bit_cast<uint32_t>(current);
	current = next;
	return moved;
}

/// Returns whether ANY field moved: a frame that did not is the frame `realise` last saw (backlog PB 3).
bool approach(Frame& current, const Frame& towards, float poleAlpha, float zeroAlpha) {
	bool moved = false;
	for (size_t b = 0; b < kBands; ++b) {
		Band& c = current.band[b];
		const Band& t = towards.band[b];
		moved |= stepInto(c.pole.radius, t.pole.radius, poleAlpha);
		moved |= stepInto(c.pole.angleLog2, t.pole.angleLog2, poleAlpha);
		moved |= stepInto(c.zero.radius, t.zero.radius, zeroAlpha);
		moved |= stepInto(c.zero.angleLog2, t.zero.angleLog2, zeroAlpha);
	}
	return moved;
}

/// A ONE-POLE COEFFICIENT FOR A TIME CONSTANT, in control blocks. One function so the pole road and
/// the zero road cannot drift into different arithmetic — which is the whole basis of "identical at
/// wake zero".
float alphaFor(double seconds, double blocksPerSecond) {
	const double tau = seconds * blocksPerSecond;
	return tau > 1.0 ? static_cast<float>(1.0 - std::exp(-1.0 / tau)) : 1.0f;
}

} // namespace

void DrawnFilter::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	refreshAlphas();
	restart();
}

void DrawnFilter::refreshAlphas() {
	// ONE POLE PER CONTROL BLOCK, not per sample, so the time constants are expressed in blocks.
	const double blocksPerSecond = sampleRate_ / static_cast<double>(kControlBlock);
	poleAlpha_ = alphaFor(kSmoothingSeconds, blocksPerSecond);
	// GEOMETRIC IN TIME, and EXACT at zero: `pow(x, 0)` is exactly 1 in IEEE, so `wake == 0` feeds
	// `alphaFor` the identical argument the poles get and the two coefficients come out bit-identical.
	// Nothing here is a tolerance.
	const double wake = static_cast<double>(std::clamp(target_.wake, 0.0f, 1.0f));
	const double seconds = kSmoothingSeconds * std::pow(kWakeMaxSeconds / kSmoothingSeconds, wake);
	zeroAlpha_ = alphaFor(seconds, blocksPerSecond);
}

void DrawnFilter::setTarget(const Settings& settings) {
	target_ = settings;
	refreshAlphas();
	// BLEND, THEN GLOBALS. Realisation happens in `advanceControl`, off the SMOOTHED frame — the
	// target frame is a position in pole-zero space and nothing else.
	targetFrame_ = withGlobals(blend(settings), settings.cutoff, settings.res);

	driveAmount_ = std::clamp(settings.drive, 0.0f, 1.0f);
	// THE SAME MAPPING `MultimodeFilter` USES — multimode_filter.cpp's refresh(): 1x to 16x with a
	// tanh and a compensating gain, so a drive at the top is a clipper and one in the middle is a
	// colour. Restated here rather than shared because that one is a const member reading the
	// filter's own precomputed state; the numbers are the contract and the test asserts them.
	driveGain_ = std::exp2(driveAmount_ * 4.0f);
	driveComp_ = 1.0f / feathers::dsp::fasttanh(std::min(driveGain_, 4.5f));

	if (!active_) {
		// FIRST UPDATE AFTER A RESTART SNAPS. Ramping in from a default frame would be an audible
		// sweep every time the device is switched on.
		smoothed_ = targetFrame_;
		realised_ = realise(smoothed_);
		active_ = true;
	}
}

void DrawnFilter::reset() {
	for (auto& s : left_) {
		s.reset();
	}
	for (auto& s : right_) {
		s.reset();
	}
	// BACK TO THE START OF THE CONTROL GRID, so a device switched on mid-block does not inherit a
	// phase from whatever ran before it. Zeroed here rather than only in `restart` because `restart`
	// calls this, which is the arrangement `Harm::reset` already uses.
	gridPhase_ = 0;
}

void DrawnFilter::restart() {
	reset();
	active_ = false;
	smoothed_ = targetFrame_;
	realised_ = realise(smoothed_);
}

void DrawnFilter::advanceControl() {
	// REALISED ONLY WHEN THE FRAME MOVED (backlog PB 3: the trim sweep is ~570 magnitude evaluations, and running it every 32
	// samples at rest was +539 us a block with nothing playing). `realise` reads the frame and nothing else, and every other
	// write of `smoothed_` (setTarget's snap, restart) realises it on the spot, so an unmoved frame's realisation is the one
	// already held — the same bits (test_fx_drawn's "the idle skip" case, against the reference road).
	if (approach(smoothed_, targetFrame_, poleAlpha_, zeroAlpha_) || referenceForTest_) {
		realised_ = realise(smoothed_);
		++realisations_;
	}
}

float DrawnFilter::driveShape(float x, float gain, float comp) {
	// CLAMPED BEFORE THE TANH, which is what makes a hot input at high drive a clip rather than an
	// explosion — multimode_filter.cpp's line, and its reason.
	const float t = std::clamp(x * gain, -4.5f, 4.5f);
	return feathers::dsp::fasttanh(t) * comp;
}

void DrawnFilter::runBlock(float* left, float* right, int frames, const float* drive) {
	const float trim = realised_.trim;
	for (int i = 0; i < frames; ++i) {
		float l = left[i];
		float r = right != nullptr ? right[i] : 0.0f;
		// A MOVING DRIVE (backlog FK) derives its gain and compensation per sample, by `setTarget`'s mapping.
		float amount = driveAmount_;
		float gain = driveGain_;
		float comp = driveComp_;
		if (drive != nullptr) {
			amount = std::clamp(drive[i], 0.0f, 1.0f);
			gain = std::exp2(amount * 4.0f);
			comp = 1.0f / feathers::dsp::fasttanh(std::min(gain, 4.5f));
		}
		if (amount > 0.0f) {
			// IN FRONT OF THE CASCADE, where `MultimodeFilter` puts its own drive stage, AND BLENDED BY
			// THE AMOUNT AS THAT STAGE BLENDS (backlog EH, shape 5): `x + amount * (shape(x) - x)`. This
			// ran the shaper at full wet, and `driveComp_` normalises the PEAK rather than the
			// small-signal slope, so a drive just above zero was not "a colour" but a +2.4 dB step
			// (tanh(x)/tanh(1) at small x) — the control lied at its rest by lying just past it. Zero
			// drive is still exactly the untouched signal; the blend makes the first step after it small.
			l += amount * (driveShape(l, gain, comp) - l);
			r += amount * (driveShape(r, gain, comp) - r);
		}
		for (size_t b = 0; b < kBands; ++b) {
			l = left_[b].process(l, realised_.section[b]);
			if (right != nullptr) {
				r = right_[b].process(r, realised_.section[b]);
			}
		}
		left[i] = l * trim;
		if (right != nullptr) {
			right[i] = r * trim;
		}
	}
}

void DrawnFilter::processStereo(float* left, float* right, int frames) {
	const float* driveRun = pendingDriveRun_; // this call's window, taken before any return
	pendingDriveRun_ = nullptr;
	if (!target_.enabled) {
		// THE FILTER STATES GO WITH THE BYPASS (backlog EH, shape 6): `reset` clears them and drops
		// `active_`, so the next enabled block snaps to its frame rather than ringing the tail that
		// was standing when the device was switched off. Once, on the first bypassed block.
		if (active_) {
			reset();
		}
		return;
	}
	if (left == nullptr || frames <= 0) {
		return;
	}
	int done = 0;
	while (done < frames) {
		// THE GRID IS ABSOLUTE, carried in gridPhase_ across calls, so where a host's block boundary
		// falls cannot change how often the coefficients are updated. HARM's loop is this loop, and
		// says the same thing; the version without the counter took one step per CALL, which made
		// `kSmoothingSeconds` mean 5 ms only at block sizes that are multiples of kControlBlock.
		if (gridPhase_ == 0) {
			advanceControl();
		}
		const int take = std::min(kControlBlock - gridPhase_, frames - done);
		runBlock(left + done, right != nullptr ? right + done : nullptr, take, driveRun != nullptr ? driveRun + done : nullptr);
		done += take;
		gridPhase_ = (gridPhase_ + take) % kControlBlock;
	}
}

void DrawnFilter::processMono(float* mono, int frames) { processStereo(mono, nullptr, frames); }

//==================================================================================================
// THE DEVICE'S OPS
//==================================================================================================

namespace {

struct OpsDrawn {
	DrawnFilter dsp;
};

OpsDrawn& asDrawn(void* storage) { return *static_cast<OpsDrawn*>(storage); }
const OpsDrawn& asDrawn(const void* storage) { return *static_cast<const OpsDrawn*>(storage); }

} // namespace

const Settings& targetForTest(const void* storage) { return asDrawn(storage).dsp.target(); }
const Realised& realisedForTest(const void* storage) { return asDrawn(storage).dsp.realised(); }
const Frame& smoothedFrameForTest(const void* storage) { return asDrawn(storage).dsp.smoothedFrame(); }
void setReferenceForTest(void* storage, bool on) { asDrawn(storage).dsp.setReferenceForTest(on); }
uint64_t realisationsForTest(const void* storage) { return asDrawn(storage).dsp.realisationsForTest(); }

/// THE CONTROLS READ PER SAMPLE WHEN MODULATED — see `AudioOps::windowedControls`.
constexpr std::array<uint8_t, 1> kWindowedControls { static_cast<uint8_t>(Param::Drive) };

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsDrawn),
    .stateAlign = alignof(OpsDrawn),
    // CONSTRUCTED INTO STORAGE THE HOST OWNS, because the host is the one under
    // no-allocation-after-activate. The device says how big it is; where it lives is not its business.
    .construct = [](void* storage) { new (storage) OpsDrawn(); },
    .destruct = [](void* storage) { asDrawn(storage).~OpsDrawn(); },
    .prepare = [](void* storage, double sampleRate, int) { asDrawn(storage).dsp.prepare(sampleRate); },
    // RESTART, NOT reset — `AudioOps::reset` means "as if just switched on", and the distinction is
    // the one the filter device learned the hard way: reset() forgets the delay lines, restart() also
    // forgets that the device was running, so the next block snaps its coefficients instead of
    // sweeping in from wherever the puck last left them.
    .reset = [](void* storage) { asDrawn(storage).dsp.restart(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, which is what puts absent fields at their DECLARED
	        // defaults rather than at zero — device_row.h's rule, arriving as a bare pointer and a
	        // count because the device vocabulary cannot see the host's DeviceRow.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asDrawn(storage).dsp.setTarget(stateOf(row, take));
        },
    // NO NOTE ROAD: plan §5.2 is bus-only in v1, so there is no keytrack and nothing for a note to
    // say. Left null rather than declared-and-empty, because `setNoteContext != nullptr` is what a
    // host reads to decide whether to bother.
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asDrawn(storage).dsp.processStereo(left, right, frames);
        },
    // THE MONO ROAD, DECLARED RATHER THAN LEFT TO CONVENTION. registry.h: a host holding a mono
    // buffer for a device with no mono road knows to give it two channels instead of hoping.
    .processMono =
        [](void* storage, float* mono, int frames) { asDrawn(storage).dsp.processMono(mono, frames); },
    // DRIVE IS READ PER SAMPLE WHILE IT MOVES (backlog FK); the frame's poles and zeros already glide on their own grid.
    .windowedControls = kWindowedControls,
    .setFieldWindows =
        [](void* storage, const float* const* runs, size_t count, int) {
	        const auto drive = static_cast<size_t>(Param::Drive);
	        asDrawn(storage).dsp.setDriveRun(drive < count ? runs[drive] : nullptr);
        },
};

} // namespace feathers::fx::drawn
