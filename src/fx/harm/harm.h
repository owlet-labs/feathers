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

#include "dsp/elliptic_highpass.h"
#include "fx/harm/harm_envelope.h"
#include "fx/harm/harm_types.h"

#include <algorithm>
#include <array>

// HARM'S DSP: STRIP A HARMONIC AND PUT ONE BACK.
//
// THE TWO HALVES ARE ONE DEVICE BECAUSE THEY SHARE A FREQUENCY. The notch sits where the oscillator
// sits — move the harmonic to a fifth and the notch moves to that fifth — so the thing being removed
// is always the thing being replaced. Split into two devices they would be a notch and a sine, and a
// player would have to keep two controls in sync by hand to get the one effect either is for.
//
// WHAT IS NOT HERE, deliberately: the note. `Settings::noteSemitones` and `gate` are supplied per
// render by whoever runs HARM, exactly as the filter's `keyOffsetSemitones` is, and they are not in
// the row and not in a patch. See harm_types.h.

namespace feathers::fx::harm {

class Harm {
public:
	/// The coefficient grid, in samples, and the reason it is a member rather than a local: it is
	/// ABSOLUTE, carried across calls, so a host handing over blocks of 1 and a host handing over
	/// blocks of 512 update the notch at the same instants and render the same samples. The filter
	/// learned this the hard way — see MultimodeFilter's own grid.
	static constexpr int kControlBlock = 16;

	void prepare(double sampleRate);
	void reset();

	void setTarget(const Settings& settings) { target_ = settings; }
	const Settings& target() const { return target_; }

	/// **LEVEL, PHASE AND SPREAD OF EACH PARTIAL, PER SAMPLE, FOR THE NEXT CALL** — their tick windows, each null while it is
	/// not moving (`AudioOps::setFieldWindows`, backlog FK). A routed Level stepped once per tick before, audibly.
	void setWindowRuns(int partial, const float* level, const float* phase, const float* spread) noexcept {
		if (partial >= 0 && partial < kPartialCount) {
			Runs& r = pending_[static_cast<size_t>(partial)];
			r.level = level;
			r.phase = phase;
			r.spread = spread;
		}
	}
	void setWindowRuns(const float* level, const float* phase, const float* spread) noexcept {
		setWindowRuns(0, level, phase, spread);
	}
	void processStereo(float* left, float* right, int frames);
	void processMono(float* mono, int frames);

	/// THE FREQUENCY A PARTIAL'S OSCILLATOR IS ACTUALLY RUNNING AT, for tests.
	///
	/// A five-line seam rather than an argument about buffers: portamento is a claim about a value at
	/// an instant, and no amount of ingenuity recovers one frequency from a signal carrying a notch,
	/// an envelope and a sine at once. CLAUDE.md's rule, and the same reason voicePitchForTest exists.
	double currentHzForTest(int partial = 0) const { return voices_[idx(partial)].currentHz; }
	/// Where the envelope has reached, 0..1.
	double envelopeForTest() const { return envelope_.level(); }
	Envelope::Stage envelopeStageForTest() const { return envelope_.stage(); }
	/// The depth a partial's notch is running at, 0..1 — Depth, times the envelope when Depth Follows Env is on; 0 while
	/// the notch is out (Width zero, or a silent envelope it follows).
	double notchDepthForTest(int partial = 0) const { return voices_[idx(partial)].notchDepthNow; }
	/// THE f0 A PARTIAL'S HIGH-PASS IS TRACKING, in hertz, or 0 while it runs its notch (backlog NL) — the value seam
	/// for "the Mode switch reached the engine": a corner is not recoverable from a buffer.
	double highpassCornerForTest(int partial = 0) const {
		const Voice& v = voices_[idx(partial)];
		return v.highpassOn ? v.hpLeft.cornerHz() : 0.0;
	}
	//-- HARM parity sweep seams (2026-09-16): a start phase and a gain law are not recoverable from
	//-- a sine that carries pitch, envelope and both at once.
	/// The oscillator's accumulator, in turns 0..1, BEFORE the Phase and Spread offsets are added.
	double phaseForTest(int partial = 0) const { return voices_[idx(partial)].phase; }
	/// What remains of a retrigger's phase catch-up, in turns; zero once the start phase is reached.
	double phaseCatchupForTest(int partial = 0) const { return voices_[idx(partial)].catchup; }
	/// The pink-noise scale on the sine: sqrt(40 / Hz) above 40 Hz, 1 below.
	double pinkScaleForTest(int partial = 0) const { return voices_[idx(partial)].pinkScale; }
	/// **THE WORK COUNTER** — how many samples a partial's notch (or high-pass) and sine have been RENDERED for, and how many
	/// control-grid points it was advanced on. Both stay at zero while a partial is inactive: an inactive partial costs nothing,
	/// and this is the value that says so rather than a timing.
	size_t partialSamplesForTest(int partial) const { return voices_[idx(partial)].samplesRendered; }
	size_t partialGridPointsForTest(int partial) const { return voices_[idx(partial)].gridPoints; }

	/// The firmware's `kHarmEnvThreshold`: below this a releasing envelope is zero.
	static constexpr double kEnvelopeFloor = Envelope::kFloor;

private:
	static size_t idx(int partial) { return static_cast<size_t>(std::clamp(partial, 0, kPartialCount - 1)); }

	/// One channel's notch. A biquad in transposed direct form II — two state words, and the form the
	/// vendored filters use, so the numerical behaviour is the one this repo already reasons about.
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

	/// THE WINDOWS ONE PARTIAL TAKES FOR ONE CALL.
	struct Runs {
		const float* level = nullptr;
		const float* phase = nullptr;
		const float* spread = nullptr;
	};

	/// **ONE PARTIAL'S RUNNING STATE** — a notch (or the brickwall) and a sine, everything the first partial always had, four
	/// times over. An inactive partial's is reset and never touched.
	struct Voice {
		bool notchOn = false;
		double notchDepthNow = 0.0; ///< the depth the coefficients carry — see `notchDepthForTest`
		double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
		Notch left {};
		Notch right {};
		/// THE BRICKWALL, Mode = High-pass (backlog NL): one channel each, tracking the oscillator's frequency.
		bool highpassOn = false;
		feathers::dsp::EllipticHighpass hpLeft {};
		feathers::dsp::EllipticHighpass hpRight {};
		double phase = 0.0;     ///< turns, 0..1
		double currentHz = 0.0; ///< after portamento
		bool haveFrequency = false;
		/// A retrigger's remaining phase correction, in turns, paid off over ~16 cycles (the firmware's
		/// phase catch-up). Zero when there is none.
		double catchup = 0.0;
		/// The pink-noise scale on the sine for the tracked frequency; 1 at and below 40 Hz.
		double pinkScale = 1.0;
		bool wasActive = false;
		size_t samplesRendered = 0; ///< TEST ONLY — see `partialSamplesForTest`
		size_t gridPoints = 0;

		void reset() {
			left.reset();
			right.reset();
			hpLeft.reset();
			hpRight.reset();
			notchOn = false;
			highpassOn = false;
			notchDepthNow = 0.0;
			phase = 0.0;
			catchup = 0.0;
			pinkScale = 1.0;
			currentHz = 0.0;
			// SO THE NEXT NOTE SNAPS RATHER THAN GLIDING UP FROM NOTHING. A reset filter starts silent; a
			// reset oscillator with a remembered frequency would portamento from wherever it was left, which
			// is audible and wrong after a transport stop.
			haveFrequency = false;
		}
	};

	void refreshCoefficients(Voice& v, const Settings& partial);
	void advanceControl(Voice& v, const Settings& partial, bool retriggered);

	Settings target_ {};
	/// See `setWindowRuns`; taken by the next `processStereo`.
	std::array<Runs, kPartialCount> pending_ {};
	size_t windowedCalls_ = 0; ///< TEST ONLY — see `opsWindowedCallsForTest`
	friend size_t opsWindowedCallsForTest(const void* storage);
	double sampleRate_ = 48000.0;

	std::array<Voice, kPartialCount> voices_ {};
	/// THE ONE ENVELOPE, shared by every partial's sine and by Depth Follows Env — a note has one shape.
	Envelope envelope_ {};
	bool gateWas_ = false;

	int gridPhase_ = 0;
};

/// WHAT AN OPS-DRIVEN HARM IS RUNNING, for tests only — the companion to the filter's
/// opsTargetForTest, and there for the same reason: a frequency is not recoverable from a signal that
/// carries a notch, an envelope and a sine at once. `storage` must be a block this device's own
/// AudioOps::construct built.
double opsCurrentHzForTest(const void* storage, int partial = 0);
/// The depth the notch is running at — see `Harm::notchDepthForTest`.
double opsNotchDepthForTest(const void* storage, int partial = 0);
/// The f0 the brickwall tracks, 0 in Notch mode — see `Harm::highpassCornerForTest`.
double opsHighpassCornerForTest(const void* storage, int partial = 0);
/// TEST ONLY: the work counters — see `Harm::partialSamplesForTest` and `partialGridPointsForTest`.
size_t opsPartialSamplesForTest(const void* storage, int partial);
size_t opsPartialGridPointsForTest(const void* storage, int partial);
const Settings& opsTargetForTest(const void* storage);
/// TEST ONLY: how many calls this instance followed a moving window in (`AudioOps::setFieldWindows`, backlog FK).
size_t opsWindowedCallsForTest(const void* storage);

} // namespace feathers::fx::harm
