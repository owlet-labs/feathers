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

// A TEST-ONLY COPY OF THE SINGLE-PARTIAL HARM, taken from the device as it stood before it grew four
// partials and a full envelope. It is the reference the bit-identity cases render against: with only the
// first partial active (and the envelope's new stages at their defaults) the shipping device must produce
// the very same floats this one does. It is deliberately NOT kept in step with the device: it reads only
// the first partial's fields of `Settings` and never the new ones.

#include "dsp/elliptic_highpass.h"
#include "fx/harm/harm_types.h"
#include "fx/scrub.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace harmref {

using namespace feathers::fx::harm;

class Harm {
public:
	static constexpr int kControlBlock = 16;
	static constexpr double kEnvelopeFloor = 1.0e-7;

	void prepare(double sampleRate) {
		sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		reset();
	}
	void reset() {
		left_.reset();
		right_.reset();
		hpLeft_.reset();
		hpRight_.reset();
		phase_ = 0.0;
		catchup_ = 0.0;
		pinkScale_ = 1.0;
		envelope_ = 0.0;
		currentHz_ = 0.0;
		gateWas_ = false;
		haveFrequency_ = false;
		gridPhase_ = 0;
	}
	void setTarget(const Settings& settings) { target_ = settings; }
	const Settings& target() const { return target_; }
	void setWindowRuns(const float* level, const float* phase, const float* spread) noexcept {
		pendingLevelRun_ = level;
		pendingPhaseRun_ = phase;
		pendingSpreadRun_ = spread;
	}
	void processStereo(float* left, float* right, int frames);
	void processMono(float* mono, int frames) { processStereo(mono, nullptr, frames); }

private:
	struct Notch {
		double z1 = 0.0;
		double z2 = 0.0;
		void reset() { z1 = z2 = 0.0; }
		float process(float x, double b0, double b1, double b2, double a1, double a2) {
			const double in = static_cast<double>(x);
			const double out = b0 * in + z1;
			z1 = b1 * in - a1 * out + z2;
			z2 = b2 * in - a2 * out;
			return static_cast<float>(out);
		}
	};
	void refreshCoefficients();
	void advanceControl(int samples);

	Settings target_ {};
	const float* pendingLevelRun_ = nullptr;
	const float* pendingPhaseRun_ = nullptr;
	const float* pendingSpreadRun_ = nullptr;
	double sampleRate_ = 48000.0;
	bool notchOn_ = false;
	double notchDepthNow_ = 0.0;
	double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
	Notch left_ {};
	Notch right_ {};
	bool highpassOn_ = false;
	feathers::dsp::EllipticHighpass hpLeft_ {};
	feathers::dsp::EllipticHighpass hpRight_ {};
	double phase_ = 0.0;
	double currentHz_ = 0.0;
	double envelope_ = 0.0;
	bool gateWas_ = false;
	bool haveFrequency_ = false;
	double catchup_ = 0.0;
	double pinkScale_ = 1.0;
	int gridPhase_ = 0;
};

inline double smoothingCoefficient(double seconds, double sampleRate) {
	if (seconds <= 0.0 || sampleRate <= 0.0) {
		return 0.0;
	}
	return std::exp(-1.0 / (seconds * sampleRate));
}

inline void Harm::refreshCoefficients() {
	// WIDTH ZERO IS NO NOTCH AT ALL, which is the control's declared meaning and not a small-Q
	// approximation of it. A notch at Q=30 still removes a real amount of signal, so a device at its
	// DEFAULT settings would colour a sound it was asked to leave alone — the identity guarantee the
	// Level control also carries.
	const bool wasOn = notchOn_;
	// **DEPTH FOLLOWS THE ENVELOPE, WHEN ASKED** (`Param::DepthFollowsEnv`): the depth is scaled by
	// where the note envelope stands at this grid point, so the notch fades in over Attack and out over Release.
	// Exactly 1 when the option is off, and `x * 1.0` is `x`, so that road is today's bits. An envelope at ZERO
	// is the cheap case: the notch takes the width-zero road below — no biquad runs, and its state is cleared on
	// the way out so a note brings it back from silence. Read at the grid, so with Attack at zero the notch
	// engages at the first grid point after the gate rather than on the gate's sample (at most kControlBlock late).
	// **HIGH-PASS MODE (backlog NL): THE BRICKWALL RUNS INSTEAD, at the frequency the oscillator is actually at.**
	// `currentHz_` rather than the target, so Porta glides the corner with the oscillator — the coupling the
	// header promises. Width, Depth and the envelope do not reach it (the brickwall has one shape), so it is
	// always on in this mode. Each filter's state is cleared on the way OUT, for the notch's reason below:
	// switching back must not replay a tail from whenever that filter last ran.
	const bool wasHighpass = highpassOn_;
	highpassOn_ = target_.highpass;
	if (highpassOn_) {
		if (notchOn_) {
			left_.reset();
			right_.reset();
		}
		notchOn_ = false;
		notchDepthNow_ = 0.0;
		const double f0 = currentHz_ > 0.0 ? currentHz_ : notchCentreHz(target_);
		hpLeft_.setCorner(f0, sampleRate_);
		hpRight_.setCorner(f0, sampleRate_);
		return;
	}
	if (wasHighpass) {
		hpLeft_.reset();
		hpRight_.reset();
	}
	const double envelopeDepth = target_.depthFollowsEnv ? envelope_ : 1.0;
	notchOn_ = target_.width > 0.0f && envelopeDepth > 0.0;
	notchDepthNow_ = 0.0;
	if (!notchOn_) {
		// AND THE BIQUAD FORGETS WHERE IT WAS (backlog EH, shape 6): a stage that keeps its state across
		// a bypass replays it on the way back in — a Q=30 notch holding a tail from whenever Width was
		// last up rang it into the first samples after Width came back. The bus utility's DC stage names
		// the same precedent. Cleared on the way OUT, so the way back in starts silent.
		if (wasOn) {
			left_.reset();
			right_.reset();
		}
		return;
	}

	// THE CENTRE TRACKS THE OSCILLATOR, which is the coupling that makes HARM one device — and falls
	// back to the fundamental when the oscillator is silent, because a notch tracking a harmonic
	// nobody is sounding would be stripping a frequency for no reason. Both live in notchCentreHz.
	const double centre = notchCentreHz(target_);
	// BOUNDED BY NYQUIST HERE, where the sample rate is known. The control declares a musical range
	// and the DSP owns its own limit — the same division the filter's cutoff draws.
	const double nyquist = sampleRate_ * 0.5;
	const double fc = std::clamp(centre, 1.0, nyquist * 0.98);
	const double q = std::max(notchQ(target_), 0.01);

	// RBJ's notch, verbatim in shape: a zero pair exactly on the unit circle at w0, so the centre is
	// removed rather than attenuated, and the poles inside it by alpha.
	const double w0 = 2.0 * std::numbers::pi * fc / sampleRate_;
	const double cosW0 = std::cos(w0);
	const double alpha = std::sin(w0) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	// THE ZEROS COME INSIDE THE CIRCLE BY THE DEPTH. RBJ's notch puts them exactly ON it — which is
	// why the centre was removed outright and why no partial cut was expressible. Giving the
	// NUMERATOR its own alpha is the whole change: at depth 1, alphaZ is 0 and these are the
	// coefficients that shipped; at depth 0 the numerator equals the denominator termwise and the
	// filter is a wire.
	//
	// THE POLES DO NOT MOVE, and that is the property worth stating: `a1_`, `a2_` and `a0` are
	// untouched, so the notch keeps its WIDTH while its DEPTH changes. A depth that moved the poles
	// would be a second width control wearing another name.
	notchDepthNow_ = notchDepth(target_) * envelopeDepth;
	const double alphaZ = alpha * (1.0 - notchDepthNow_);
	b0_ = (1.0 + alphaZ) / a0;
	b1_ = -2.0 * cosW0 / a0;
	b2_ = (1.0 - alphaZ) / a0;
	a1_ = -2.0 * cosW0 / a0;
	a2_ = (1.0 - alpha) / a0;
}

inline void Harm::advanceControl(int samples) {
	// PORTAMENTO ON THE TRACKED FREQUENCY, and the notch rides it because the notch is recomputed
	// from the same number. A glide that moved the oscillator and left the notch behind would pull
	// the two halves of the device apart for the length of the glide.
	const double targetHz = oscillatorHz(target_);
	// LATCHED HERE, ON THE GRID, NOT AT THE END OF THE CALL. It used to be written after the whole
	// buffer, so a 256-frame block saw "retriggered" at sixteen grid points and a 1-frame block at
	// one — harmless while the trigger only snapped a frequency that was already at its target, and a
	// block-size dependence the moment the trigger also aimed the phase (HARM parity sweep,
	// 2026-09-16; the block-size case caught it at 1e-6).
	const bool retriggered = target_.gate && !gateWas_;
	gateWas_ = target_.gate;
	const double coefficient = smoothingCoefficient(target_.portaSeconds, sampleRate_);
	if (!haveFrequency_ || retriggered || coefficient == 0.0) {
		// SNAP ON RETRIGGER, so a new note starts AT its pitch. A porta that applied to the first note
		// of a phrase would bend into every attack, which is not what a portamento control means.
		currentHz_ = targetHz;
		haveFrequency_ = true;
	}
	else if (currentHz_ != targetHz) {
		const double retention = std::pow(coefficient, static_cast<double>(samples));
		currentHz_ = targetHz + (currentHz_ - targetHz) * retention;
	}

	// PHASE IS A START PHASE, so a retrigger AIMS the oscillator at it — and converges rather than
	// snapping, which clicks. The firmware computes the signed error to the target on the trigger and
	// pays it off at `error * increment / 16` per sample (~16 cycles; its comment says ~24). The port
	// added Phase as a constant offset to a free-running accumulator, so only the first note after a
	// reset started where the knob said (HARM parity sweep, 2026-09-16). The offset itself is applied
	// at the read, so the target for the accumulator is zero: the error is the accumulator's own
	// distance from a whole turn, shortest way round.
	if (retriggered) {
		const double away = phase_ - std::floor(phase_ + 0.5); // in (-0.5, 0.5]: how far past the nearest turn
		catchup_ = -away;
	}

	// PINK-NOISE SCALING OF THE SINE, -3 dB/octave above 40 Hz and unity below, recomputed with the
	// tracked frequency (the firmware's `cachedPinkQ31`, recomputed when the pitch moves).
	pinkScale_ = currentHz_ > 40.0 ? std::sqrt(40.0 / currentHz_) : 1.0;

	refreshCoefficients();
}

inline void Harm::processStereo(float* left, float* right, int frames) {
	// THIS CALL'S WINDOWS, taken before any return so none outlives its call.
	const float* levelRun = pendingLevelRun_;
	const float* phaseRun = pendingPhaseRun_;
	const float* spreadRun = pendingSpreadRun_;
	pendingLevelRun_ = pendingPhaseRun_ = pendingSpreadRun_ = nullptr;
	if (left == nullptr || frames <= 0) {
		return;
	}
	// THE BYPASS IS A RETURN, not a pass through a silent oscillator and an inert notch. Not one
	// sample is read or written, which is what lets a chain holding a switched-off HARM render the
	// same bits it rendered before the device was added — the filter's own rule, and the reason its
	// bypass is spelled the same way.
	//
	// AND THE STATE GOES QUIET WITH IT. A device switched out mid-phrase must not keep its envelope
	// open or its notch ringing, or switching it back on would resume a note nobody is playing.
	if (!target_.enabled) {
		if (haveFrequency_ || envelope_ > 0.0) {
			reset();
		}
		return;
	}
	// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 before anything sees it (as EQ and Comp do; `feathers::fx::scrubNonFinite`).
	feathers::fx::scrubNonFinite(left, right, frames);
	const double level = static_cast<double>(std::clamp(target_.level, 0.0f, 1.0f));
	// SPREAD IS A HALF TURN AT THE TOP, and it is separate from Phase — the firmware folded the two
	// into one control and could not offer a centred oscillator with any width at all.
	const double spreadTurns = static_cast<double>(std::clamp(target_.spread, 0.0f, 1.0f)) * 0.5;
	const double phaseOffset = static_cast<double>(std::clamp(target_.phase, 0.0f, 1.0f));

	int done = 0;
	while (done < frames) {
		// THE GRID IS ABSOLUTE, carried in gridPhase_ across calls, so where a host's block boundary
		// falls cannot change which samples the coefficients were updated on.
		if (gridPhase_ == 0) {
			advanceControl(kControlBlock);
		}
		const int take = std::min(frames - done, kControlBlock - gridPhase_);

		const double increment = currentHz_ / sampleRate_;
		const double attack = smoothingCoefficient(target_.attackSeconds, sampleRate_);
		const double release = smoothingCoefficient(target_.releaseSeconds, sampleRate_);

		for (int i = 0; i < take; ++i) {
			const int at = done + i;

			// THE ENVELOPE MOVES PER SAMPLE, not per grid: it is what a note's attack sounds like, and
			// a sixteen-sample staircase on it is audible in a way a sixteen-sample staircase on a
			// notch's centre is not.
			const double coefficient = target_.gate ? attack : release;
			const double destination = target_.gate ? 1.0 : 0.0;
			envelope_ = coefficient == 0.0 ? destination
			                               : destination + (envelope_ - destination) * coefficient;
			// FLOORED TO ZERO, the firmware's `kHarmEnvThreshold` (1e-7, ~-140 dB): a one-pole release
			// never reaches zero on its own, so without this the sine's gain went denormal after every
			// note and stayed there — on the audio thread (HARM parity sweep, 2026-09-16).
			if (!target_.gate && envelope_ < kEnvelopeFloor) {
				envelope_ = 0.0;
			}

			// STRIP FIRST, THEN REPLACE. The order is the device: the notch takes the harmonic out of
			// what arrived, and the oscillator puts one back in its place.
			float l = left[at];
			float r = right != nullptr ? right[at] : l;
			if (notchOn_) {
				l = left_.process(l, b0_, b1_, b2_, a1_, a2_);
				r = right_.process(r, b0_, b1_, b2_, a1_, a2_);
			}
			else if (highpassOn_) {
				l = hpLeft_.process(l);
				r = hpRight_.process(r);
			}

			// PER SAMPLE WHILE A ROUTED SOURCE MOVES THEM (backlog FK), by the same clamps as the scalars above.
			const double levelAt = levelRun != nullptr ? static_cast<double>(std::clamp(levelRun[at], 0.0f, 1.0f)) : level;
			if (levelAt > 0.0 && envelope_ > 0.0) {
				const double spreadAt =
				    spreadRun != nullptr ? static_cast<double>(std::clamp(spreadRun[at], 0.0f, 1.0f)) * 0.5 : spreadTurns;
				const double phaseAt = phaseRun != nullptr ? static_cast<double>(std::clamp(phaseRun[at], 0.0f, 1.0f)) : phaseOffset;
				// PINK-SCALED: -3 dB per octave above 40 Hz, never a boost — the firmware's `pinkScale`,
				// which the port had dropped (HARM parity sweep, 2026-09-16). Harmonic spans 1:8 to 32:1,
				// so without it 32:1 on middle C sat a full 23 dB above where the reference puts it.
				// SPREAD IS SYMMETRIC about Phase, +-spread/2, as the firmware places it (`targetPhaseL =
				// spread/2, targetPhaseR = -spread/2`); the port held L and moved R by the whole spread —
				// the same angle between them, a different place for both.
				// AND FULL LEVEL IS 0 dBFS — DECIDED (backlog EM). The firmware's sine leaves
				// its fixed-point chain ~36 dB down (`>>6`, then `<<1`, `<<2`), a headroom artefact rather
				// than a level anyone chose; the parity sweep asked and this is the answer.
				const double gain = levelAt * envelope_ * pinkScale_;
				l += static_cast<float>(
				    gain * std::sin(2.0 * std::numbers::pi * (phase_ + phaseAt - 0.5 * spreadAt)));
				r += static_cast<float>(
				    gain * std::sin(2.0 * std::numbers::pi * (phase_ + phaseAt + 0.5 * spreadAt)));
			}

			left[at] = l;
			if (right != nullptr) {
				right[at] = r;
			}

			phase_ += increment;
			// THE CATCH-UP, paid off a sixteenth of the increment's share per sample; the last crumb is
			// paid whole so the error reaches exactly zero rather than asymptoting into denormals.
			if (catchup_ != 0.0) {
				double correction = catchup_ * increment * (1.0 / 16.0);
				if (std::fabs(correction) >= std::fabs(catchup_) || std::fabs(correction) < 1.0e-9) {
					correction = catchup_;
				}
				phase_ += correction;
				catchup_ -= correction;
			}
			if (phase_ >= 1.0 || phase_ < 0.0) {
				phase_ -= std::floor(phase_);
			}
		}

		done += take;
		gridPhase_ = (gridPhase_ + take) % kControlBlock;
	}
}

} // namespace harmref
