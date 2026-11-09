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

#pragma once

#include "dsp/vocode.h"
#include "fx/spectral/spectral_envelope.h"
#include "fx/spectral/spectral_stft.h"

#include <array>
#include <cstdint>
#include <vector>

struct PFFFT_Setup;

// **THE SIDECHAIN'S ENVELOPE, ONCE PER SYNTH** (backlog OE): the analysis
// half of the Spectral effect's vocoder (`Stft::sidechainFrame`) without the resynthesis — the same Resolution stops and transform
// sizes, the same periodic sqrt-Hann, the same EnvelopeEstimator and the same attack/release per grid point at the hop's rate —
// for the additive engine's voices, which read it at their own partials (dsp/vocode.h). Mono: ½(L + R), or the left alone.
// Allocates only in `prepare`.
namespace feathers::fx::spectral {

static_assert(feathers::spectral::VocodeEnvelope::kPoints == kEnvelopePoints);
static_assert(feathers::spectral::VocodeEnvelope::kLowHz == kEnvelopeLowHz);
static_assert(feathers::spectral::VocodeEnvelope::kOctaves == kEnvelopeOctaves);

class SidechainEnvelope {
public:
	struct Settings {
		Resolution resolution = Resolution::Balanced;
		double bands = 24.0;
		double attackMs = 10.0;
		double releaseMs = 80.0;
		bool freeze = false;
		/// OE2: Stereo analyses the two channels apart (channel 0 the left, 1 the right); otherwise channel 0 is the mono sum.
		bool stereo = false;
		/// OE2 SIBILANCE: make the noise layer (unit amount: the synth scales and gates it). Off, nothing is made.
		bool sibilance = false;
		friend bool operator==(const Settings&, const Settings&) = default;
	};

	SidechainEnvelope() = default;
	SidechainEnvelope(const SidechainEnvelope&) = delete;
	SidechainEnvelope& operator=(const SidechainEnvelope&) = delete;
	~SidechainEnvelope();

	/// ALLOCATE, for every stop at this rate.
	void prepare(double sampleRate);
	/// The ring cleared and the envelope un-primed (silent). Allocates nothing.
	void reset() noexcept;
	/// A new stop keeps the envelope, moved onto the new stop's dBFS scale (9.2: the calibrated level does not jump).
	void setSettings(const Settings& s) noexcept;
	[[nodiscard]] const Settings& settings() const noexcept { return settings_; }

	/// THE SIDECHAIN'S NEXT `frames`: into the ring, and one frame each hop — unless Freeze holds a primed envelope.
	/// OE2: AND THE SIBILANCE NOISE for the same frames into `noiseL`/`noiseR` (null: not made) — the Spectral effect's: random-phase
	/// noise at the sidechain's own level above 2 kHz times its flatness there, resynthesised by overlap-add, unit amount, one channel
	/// a channel (the right is the left's unless Stereo). Zero while `settings().sibilance` is off.
	void push(const float* left, const float* right, int frames, float* noiseL = nullptr, float* noiseR = nullptr) noexcept;
	/// Channel `c`'s spectral flatness above 2 kHz (0 a tone, 1 noise), as last measured or primed.
	[[nodiscard]] float flatness(int c = 0) const noexcept { return ch_[static_cast<size_t>(c & 1)].flatness; }

	/// Channel `c`'s envelope in dBFS on the grid (VocodeEnvelope::kSilentDb where silent), and the bin spacing it was measured at.
	[[nodiscard]] const std::array<float, kEnvelopePoints>& gridDbfs(int c = 0) const noexcept { return ch_[static_cast<size_t>(c & 1)].dbfs; }
	[[nodiscard]] double binHz() const noexcept { return binHz_; }
	/// Whether an envelope has been made (or primed) since the reset; and whether Freeze holds it.
	[[nodiscard]] bool primed() const noexcept { return primed_; }
	[[nodiscard]] bool holding() const noexcept { return settings_.freeze && primed_; }
	/// Moves when the envelope changes (a frame that left it as it was does not move it).
	[[nodiscard]] uint64_t serial() const noexcept { return serial_; }
	/// Z10b's road: CATCH a saved envelope (dBFS on the grid per channel, `right` null for mono: both the left's), measured at
	/// `binHz`, as if the sidechain had just made it.
	void prime(const float* left, const float* right, double binHz, float flatnessL = 0.0f, float flatnessR = 0.0f) noexcept;
	[[nodiscard]] uint64_t framesForTest() const noexcept { return frames_; }

private:
	void frame(int c) noexcept;
	/// One hop of channel `c`'s noise into its overlap-add, from its envelope and flatness.
	void noiseFrame(int c) noexcept;
	void release() noexcept;

	double sampleRate_ = 0.0;
	Settings settings_ {};
	int n_ = 0;
	double binHz_ = 100.0;
	std::array<PFFFT_Setup*, kResolutionCount> setups_ {};
	float* buffer_ = nullptr; ///< aligned, nMax
	float* work_ = nullptr;   ///< aligned, nMax
	std::vector<float> window_;
	std::vector<float> synthesis_; ///< sqrt-Hann · 1/(2N): the overlap and pffft's unscaled inverse folded in (Stft's)
	std::vector<float> envBins_;   ///< scratch: an envelope read back at each bin
	int outPos_ = 0;
	/// ONE RING AND ONE ENVELOPE PER CHANNEL: the estimator's scale (dB of |X|, smoothed) and dBFS.
	struct Channel {
		std::vector<float> history;
		std::array<float, kEnvelopePoints> grid {};
		std::array<float, kEnvelopePoints> dbfs {};
		float flatness = 0.0f;
		std::vector<float> ola;   ///< N: the noise's overlap-add
		std::vector<float> out;   ///< H: the noise finalised, played over the next hop
		uint32_t rng = 0x2545f491u;
	};
	std::array<Channel, 2> ch_ {};
	std::vector<float> mag_;
	EnvelopeEstimator estimator_;
	size_t mask_ = 0;
	size_t write_ = 0;
	int hopCount_ = 0;
	bool primed_ = false;
	uint64_t serial_ = 0;
	uint64_t frames_ = 0;
};

} // namespace feathers::fx::spectral
