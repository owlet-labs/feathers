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

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feathers::fx::featherverb {

//--------------------------------------------------------------------------------------------------//
// AURORA — a frequency-shifted feedback delay network ("take a crack at another //
// algorithm in the sky-owl field emphasis again on originality good sound and light compute").          //
//--------------------------------------------------------------------------------------------------//
//
// **THE IDEA.** Four delay lines recirculate through a Householder mix, as any small FDN does — and the energy
// fed back into the first line passes a SINGLE-SIDEBAND FREQUENCY SHIFTER. A frequency shift moves every
// component by the same number of Hz (a pitch shift MULTIPLIES them), so it never lines up with the loop's own
// modes: each lap nudges the spectrum, and the tail climbs (or falls) as a slow barber-pole curtain instead of
// ringing on the same pitches.
//
// - A FRACTION OF A HERTZ breaks up the metallic modes a four-line network otherwise has: dense and smooth from
//   very little.
// - A FEW HERTZ is heard as a glide — an aurora's slow curtains. Matrix sets it, bipolar about its centre:
//   below rising... (see `Params::shift`).
// - NOT SHIMMER: shimmer pitch-shifts by octaves and piles up harmonics; this slides.
//
// **THE COST, BY DESIGN LIGHT:** four lines read with linear interpolation, a Householder mix (adds and one
// multiply), four one-pole dampers, four input allpasses, and one 90° network (Olli Niemitalo's eight
// first-order allpasses) with a rotating phasor — about eighty operations a sample at the host rate.
//
// **THE SAME ROAD AS THE FIRMWARE CORE**: int32 in and out at the reference's scale, and the Exciter's injection, so
// the device's gate, send, return and convolution work on it unchanged. The firmware core is untouched: Aurora
// is its own engine, run in the core's place when the Size Mode names it.
class Aurora {
public:
	struct Params {
		float size = 0.0f;      ///< 0..1: the lines' lengths, ~40 ms to ~190 ms at the longest line
		float decay = 0.0f;     ///< 0..1: the tail's RT60, 1.2 s to 12 s (a squared taper)
		float shift = 0.0f;     ///< -1..1: the frequency shift, falling below 0 and rising above; 0 = a quarter
		                        ///  hertz rising (the decorrelating minimum), the ends ±12 Hz
		float diffusion = 0.6f; ///< 0..1: the input allpasses' gain
		float damping = 0.7f;   ///< 0..1: the in-loop lowpass
		float width = 1.0f;     ///< 0..1: mono to wide
		float hpfHz = 35.18f;   ///< the input highpass's corner, Hz, as the core's (`Core::setHpfHz`)
		float lpfHz = 20000.0f; ///< the output lowpass's corner, Hz, as the core's
		float predelay = 0.0f;  ///< 0..1: up to 50 ms, as the core's
		/// CHARACTER'S TRIANGLES (`characterBankOf`): [0] line drift, [1] the shift LFO's depth, [4] its rate, [2] diffusion, [3] damping
		/// tilt, [6] width — all zero at Character 0.
		std::array<float, 11> character {};
	};

	/// ALLOCATE for this rate. Message thread.
	void prepare(double sampleRate);
	void reset() noexcept;
	/// THE CONTROLS, between chunks. Allocates nothing.
	void set(const Params& params) noexcept;
	void run(const int32_t* input, int32_t* outL, int32_t* outR, size_t frames, const int32_t* inject = nullptr) noexcept;

	/// TEST SEAMS.
	[[nodiscard]] float shiftHzForTest() const noexcept { return shiftHz_; }
	[[nodiscard]] float rt60ForTest() const noexcept { return rt60_; }
	/// THE SHIFT'S LFO: how far (Hz either side) and how fast (Hz), and the shift the last `run` used.
	[[nodiscard]] float wobbleDepthHzForTest() const noexcept { return wobbleHz_; }
	[[nodiscard]] float wobbleRateHzForTest() const noexcept { return wobbleRateHz_; }
	[[nodiscard]] float lastShiftHzForTest() const noexcept { return lastShiftHz_; }

	static constexpr int kLines = 4;
	static constexpr int kDiffusers = 4;

private:
	struct Line {
		std::vector<float> buf;
		size_t pos = 0;
		float length = 1.0f; // samples, before drift
		float gain = 0.0f;   // per lap, for the RT60
		float lp = 0.0f;     // the damper's state
		double drift = 0.0;  // the length's slow LFO phase
		double driftStep = 0.0;
	};
	struct Allpass {
		std::vector<float> buf;
		size_t pos = 0;
		size_t length = 1;
	};
	/// Niemitalo's 90° pair: two branches of four first-order allpasses on z^-2, the real branch a sample late.
	struct Hilbert {
		std::array<float, 4> a {};
		std::array<float, 4> b {};
		std::array<std::array<float, 4>, 2> xa {}; // x[n-1], x[n-2] per stage, branch A
		std::array<std::array<float, 4>, 2> ya {};
		std::array<std::array<float, 4>, 2> xb {};
		std::array<std::array<float, 4>, 2> yb {};
		float realDelay = 0.0f;
	};

	[[nodiscard]] float readLine(const Line& line, float delay) const noexcept;

	double sampleRate_ = 48000.0;
	std::array<Line, kLines> lines_ {};
	std::array<Allpass, kDiffusers> diffusers_ {};
	std::vector<float> predelayBuf_;
	size_t predelayPos_ = 0;
	size_t predelayLength_ = 0;
	Hilbert hilbert_ {};
	double phasor_ = 0.0;   // the shifter's oscillator phase, cycles
	double wobble_ = 0.0;   // the shift's own slow LFO, cycles
	float hpState_ = 0.0f;
	float lpL_ = 0.0f;
	float lpR_ = 0.0f;

	// THE CONTROLS, AS THE LOOP READS THEM
	float shiftHz_ = 0.25f;
	float wobbleHz_ = 0.0f;
	float wobbleRateHz_ = 0.02f;
	float lastShiftHz_ = 0.25f;
	float rt60_ = 1.2f;
	float diffusionGain_ = 0.5f;
	float dampCoeff_ = 0.3f;
	float driftDepth_ = 0.0f;
	float widthMix_ = 1.0f;
	float hpG_ = 0.0f; ///< the input highpass's trapezoidal gain (`Core::tptGain`)
	float lpCoeff_ = 1.0f;
	float outGain_ = 1.0f;
};

} // namespace feathers::fx::featherverb
