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

#include "dsp/vocode_bands.h"
#include "fx/spectral/spectral_envelope.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

struct PFFFT_Setup;

// THE SPECTRAL DEVICE'S STFT: analyse, multiply every bin by a real
// gain, resynthesise. Periodic sqrt-Hann on both sides at a hop of N/4, so the squared window overlap-adds to exactly 2.
//
// **THE LATENCY IS A FUNCTION OF THE RESOLUTION AND THE RATE, AND OF NOTHING ELSE**: L = round(ms · sr / 1000). N is the
// largest real transform size no larger than L, and the remaining L − N samples are a pad: the frame is read that far
// behind the newest input. A sample therefore leaves exactly L samples after it entered, whatever the gains, the Mix, or
// whether frames are being computed at all.
//
// **EXACT IDENTITY**: with the gains declared the identity, no frame is computed and the output is the input delayed by L,
// bit for bit. Switching to an active curve computes frames but keeps the output on the delayed dry until N/H frames have
// overlapped (the first complete wet sample), then crossfades over one hop. Switching back crossfades at once.
namespace feathers::fx::spectral {

enum class Resolution : int { Fast, Balanced, Fine, kCount };
inline constexpr int kResolutionCount = static_cast<int>(Resolution::kCount);
inline constexpr double kResolutionMs[kResolutionCount] = { 5.0, 10.0, 40.0 };
/// The overlap: four frames cover every sample.
inline constexpr int kOverlap = 4;
/// The smallest real transform this device runs (pffft's SIMD minimum).
inline constexpr int kMinFftSize = 32;
/// How many samples a new Mix ramps over: the house control grid, so it is the same whatever the host block.
inline constexpr int kMixRampSamples = 32;
/// THE INPUT SPECTRUM THE CARD DRAWS: this many bands, even in log frequency from kBandLowHz to kBandHighHz, each the
/// loudest bin in it in dBFS (a full-scale sine at a bin centre reads 0), floored at kBandFloorDb.
inline constexpr int kSpectrumBands = 48;
/// PARTIALS' RANGE (design §11), and each kept partial's main lobe either side of its bin (sqrt-Hann's is under two).
inline constexpr double kPartialsMin = 1.0;
inline constexpr double kPartialsMax = 320.0; ///< the drawer's kMaxCount 
inline constexpr int kPartialsLobe = 2;
inline constexpr double kBandLowHz = 20.0;
inline constexpr double kBandHighHz = 20000.0;
inline constexpr float kBandFloorDb = -96.0f;

/// Whether `n` is a real transform size: a multiple of 32 whose quotient has no prime factor above 5. Stricter than
/// pffft's rule on a scalar build, so N is the same on every platform.
[[nodiscard]] bool isRealFftSize(int n) noexcept;
/// THE REPORTED LATENCY: round(ms · sr / 1000), and never below kMinFftSize.
[[nodiscard]] uint32_t latencyFor(Resolution r, double sampleRate) noexcept;
/// The largest real transform size no larger than latencyFor(r, sampleRate).
[[nodiscard]] int fftSizeFor(Resolution r, double sampleRate) noexcept;

class Stft {
public:
	Stft() = default;
	Stft(const Stft&) = delete;
	Stft& operator=(const Stft&) = delete;
	~Stft();

	/// ALLOCATE, for every stop at this rate, so a Resolution change on the audio thread allocates nothing.
	void prepare(double sampleRate);
	/// AS IF JUST SWITCHED ON: history, overlap-add and crossfade cleared. Allocates nothing.
	void reset() noexcept;

	/// Allocates nothing. A change of stop resets.
	void setResolution(Resolution r) noexcept;
	[[nodiscard]] Resolution resolution() const noexcept { return resolution_; }
	[[nodiscard]] uint32_t latency() const noexcept { return latency_; }
	[[nodiscard]] int fftSize() const noexcept { return n_; }
	[[nodiscard]] int hop() const noexcept { return n_ / kOverlap; }
	[[nodiscard]] int binCount() const noexcept { return n_ / 2 + 1; }
	[[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

	/// THE GAIN PER BIN, [0, N/2]: the caller writes binCount() values here. Bin i is i · sr / N Hz.
	[[nodiscard]] float* gains() noexcept { return gains_.data(); }
	[[nodiscard]] const float* gains() const noexcept { return gains_.data(); }
	/// Whether the gains are the identity: no frame is computed and the output is the delayed input.
	void setIdentity(bool identity) noexcept { identity_ = identity; }
	/// 0..1: wet against the dry delayed by L. RAMPED LINEARLY OVER kMixRampSamples, per sample, so a modulated Mix does not step;
	/// a FIXED length and not the host block's, so the output does not depend on how the host partitions it.
	void setMix(float mix) noexcept;

	/// WHETHER TO ANALYSE THE INPUT FOR THE CARD while no frame is being computed (the identity): one forward transform of the
	/// left channel a hop. While frames run the analysis is theirs and costs nothing more.
	void setAnalyse(bool analyse) noexcept { analyse_ = analyse; }
	/// The input spectrum as of the last analysed frame, kSpectrumBands values in dBFS.
	[[nodiscard]] const float* bands() const noexcept { return bands_.data(); }
	/// Analysed frames since the last reset: a reader compares it to know whether `bands` moved.
	[[nodiscard]] uint64_t analysedForTest() const noexcept { return analysed_; }

	/// **THE SIDECHAIN FOR THE NEXT `process`** (design §10.1): its two channels over the same frames (null is silence), and
	/// whether it is sounding (for the card only). Written, mono, into a ring beside the input's, so a frame reads both from the same instant.
	/// The pointers are used by that one call and dropped.
	void setSidechain(const float* left, const float* right, bool sounding) noexcept {
		sideL_ = left;
		sideR_ = right;
		sideSounding_ = sounding;
	}
	[[nodiscard]] bool sidechainSounding() const noexcept { return sideSounding_; }

	/// **THE VOCODER** (design §10.3): the carrier flattened by its own envelope (by `flatten`), the sidechain's envelope imposed,
	/// noise at the sidechain's level where it is noisy (`sibilance`). Acts whenever `on`, whatever the sidechain carries (a silent one
	/// silences the output, as a real vocoder does), and crossfades over one window as `on` changes; off, the stage is skipped.
	struct Vocode {
		bool on = false;
		double bands = 24.0;
		double attackMs = 10.0;
		double releaseMs = 80.0;
		float flatten = 1.0f;
		float sibilance = 0.0f;
		/// **WHAT IS VOCODED** : 0 the input (the stage as it always was), 1 a band-limited square at f0 (`setF0Hz`),
		/// 2 white noise, 3 pink. An internal carrier is generated per sample into its own ring beside the input's, and the frame
		/// reads it in place of the input; the dry side of Mix stays the input.
		int carrier = 0;
		/// THE BASICS (design §10.9): Depth 0..2 (1 the classic vocoder, exactly the stage as it was); a Gate in dBFS below which a
		/// band closes (at or below -96, off); Freeze holds the sidechain's envelope; Low/High in Hz, outside which the envelope is
		/// held at its edge value (20 Hz and 20 kHz: nothing held).
		double depth = 1.0;
		double gateDb = -96.0;
		bool freeze = false;
		double lowHz = 20.0;
		double highHz = 20000.0;
		/// STEREO: 0 Mono (one envelope from the sum, both channels), 1 Stereo (each channel by its own sidechain channel), 2 Spread
		/// (one envelope; its bands alternate between the channels). BANDWIDTH 0..1: below 1 the envelope is imposed through
		/// `bands` discrete bands, Gaussian in log frequency, narrow at 0 (a classic filterbank's ringing comb); at 1 the smooth
		/// envelope, exactly as before.
		int stereo = 0;
		double bandwidth = 1.0;
		/// THE ZONE (design §10.7.2, Z2): where the bands sit between Range Low and High — 0 Even, 1 Speech (Bark), 2 Low, 3 High —
		/// morphed from Even by `zoneAmount` (0..1); 4 Harmonic and 5 Inharmonic re-lay them on the carrier's harmonics or the gaps (Z3). Any Zone but Even at an amount puts the vocoder on the banded road, so it is
		/// heard at every Bandwidth.
		int zone = 0;
		double zoneAmount = 0.5;
		/// FREE'S OWN COORDINATE (Z7): unbounded, 0 Even; the named Zones read the fenced `zoneAmount`.
		double zoneFree = 0.5;
		/// THE BANDWIDTH ZONE (Z5): how widths spread across the layout — 0 Even, 1 Narrow Low, 2 Narrow High, 3 Harmonic, 4 Inharmonic,
		/// 5 Free — by `widthAmount` (in Free, its own coordinate). Any but Even at an amount puts the vocoder on the banded road.
		int widthZone = 0;
		double widthAmount = 0.5;
		/// The Widths' Free coordinate (Z7), likewise.
		double widthFree = 0.5;
	};
	void setVocode(const Vocode& v) noexcept { vocode_ = v; }
	/// THE OPERATORS' f0, Hz (the caller's, per block): Partials' Harmonics and the harmonic layouts — the note raised by Offset.
	void setF0Hz(double hz) noexcept { f0Hz_ = hz; }
	/// **THE SQUARE CARRIER'S PITCH, Hz (Z9; Requirement: it follows the NOTE only)** — Key Track's note, or C3 untracked; never Offset,
	/// which moves only the operators and the layouts.
	void setCarrierHz(double hz) noexcept { carrierHz_ = hz; }
	[[nodiscard]] double carrierHzForTest() const noexcept { return carrierHz_; }

	/// **GENERALISED PARTIALS** (design §11): per frame, keep the `count` strongest spectral PEAKS (mode 0; any audio) or
	/// HARMONICS 1..count of f0 (mode 1), each with its main lobe; everything else down by `depth`. `count` is continuous: the
	/// last kept partial is weighted by its fraction. Before Formant Shift and Vocode, so it simplifies whatever they shape.
	/// MODE 2, CENTERS (backlog OI's first scheme): keep only the harmonics of f0 AT the centre set — the
	/// fundamental and a few of 2..7 picked by `centersSet` through partials::sparseKeep, the generator the terrain's sparse country
	/// uses — each weighted by its keep (so a set glides, partials fading); `count` is not read.
	struct Partials {
		bool on = false;
		int mode = 0;
		double count = 8.0;
		float depth = 1.0f;
		float centersSet = 0.0f;
	};
	void setPartials(const Partials& p) noexcept { partials_ = p; }
	[[nodiscard]] bool partialsOn() const noexcept { return partials_.on; }
	/// TEST ONLY: the gain Partials gave bin `i` in the last frame (1 where nothing was taken).
	/// TEST ONLY: the imposed envelope at bin `bin` (the first sidechain's, as last built: banded or smooth), linear.
	[[nodiscard]] float sidechainBinEnvelopeForTest(int bin) const noexcept {
		const std::vector<float>& e = side_[0].env;
		return bin >= 0 && static_cast<size_t>(bin) < e.size() ? e[static_cast<size_t>(bin)] : 0.0f;
	}
	/// TEST ONLY: band `b`'s centre in log2 Hz, and the count, as last laid out.
	[[nodiscard]] double bandCentreForTest(int b) const noexcept { return bandCentre_[static_cast<size_t>(b)]; }
	[[nodiscard]] int bandCountForTest() const noexcept { return bandCount_; }
	/// TEST ONLY: how many times the Harmonic grouping has been laid (it re-lays on an f0 change, not every frame).
	[[nodiscard]] uint64_t groupingsForTest() const noexcept { return groupings_; }
	/// TEST ONLY: band `b`'s width in log2 Hz, as last laid out.
	[[nodiscard]] double bandSigmaForTest(int b) const noexcept { return bandSigma_[static_cast<size_t>(b)]; }
	/// TEST ONLY: the Bandwidth zone's Free axes at `x` — tilt and harmonic, both bipolar.
	[[nodiscard]] static std::array<double, 2> widthAxesForTest(double x) noexcept;
	/// TEST ONLY: the Free coordinate's axes at `x` — Speech, Low, High, Harmonic (bipolar).
	[[nodiscard]] static std::array<double, 4> freeAxesForTest(double x) noexcept;
	/// TEST ONLY: band `b`'s group, in members (harmonic numbers, 1 = f0; for the gaps, the gap above that harmonic).
	[[nodiscard]] std::pair<int, int> groupForTest(int b) const noexcept {
		return { grouping_.first(b) + grouping_.firstMember() + 1, grouping_.last(b) + grouping_.firstMember() + 1 };
	}
	/// Whether the vocoder takes the banded road: Bandwidth below 100%, or a Zone other than Even at an amount.
	[[nodiscard]] bool banded() const noexcept { return vocode_.bandwidth < 0.999 || zoneDrive() != 0.0 || widthDrive() != 0.0; }
	/// **WHAT THE ZONE'S MODE READS (Z7)**: nothing in Even (0), a named Zone's FENCED amount (0..1), or Free's (6) UNBOUNDED
	/// coordinate, where 0 is Even too.
	[[nodiscard]] double zoneDrive() const noexcept {
		return vocode_.zone <= 0 ? 0.0 : vocode_.zone >= 6 ? vocode_.zoneFree : std::clamp(vocode_.zoneAmount, 0.0, 1.0);
	}
	/// The same for the Widths (Free is 5).
	[[nodiscard]] double widthDrive() const noexcept {
		return vocode_.widthZone <= 0 ? 0.0 : vocode_.widthZone >= 5 ? vocode_.widthFree : std::clamp(vocode_.widthAmount, 0.0, 1.0);
	}
	/// TEST ONLY: the sum of the band weights at bin `i` (the layout as last built): 1 by partition of unity.
	[[nodiscard]] float bandWeightSumForTest(int bin) const noexcept;
	[[nodiscard]] float partialsGainForTest(int i) const noexcept { return partialsGain_.empty() ? 1.0f : partialsGain_[static_cast<size_t>(i)]; }
	/// Whether the frame reads an internal carrier rather than the input: while Vocode is on, and while it fades out.
	[[nodiscard]] bool internalCarrier() const noexcept { return vocode_.carrier != 0 && (vocode_.on || vocodeMix_ > 0.0f); }
	/// Whether the vocoder acts in the next frame: whenever it is on (never a dry fallback on a silent sidechain).
	[[nodiscard]] bool vocodeActive() const noexcept { return vocode_.on; }
	/// **FORMANT SHIFT** (design §10.4): the ratio per bin (`shaper::formantRatios`, the caller's, N/2 + 1 of them) a bin reads the
	/// envelope at hz / ratio by. Vocoding, it moves the sidechain's envelope; otherwise the input's own (gain E(hz / r) / E(hz)).
	[[nodiscard]] float* formantRatios() noexcept { return fshiftRatio_.data(); }
	void setFormantShift(bool on) noexcept { fshiftOn_ = on; }
	[[nodiscard]] bool formantShiftOn() const noexcept { return fshiftOn_; }
	/// **THE FROZEN ENVELOPE'S TRANSPOSITION, semitones (Z10; Requirement: Freeze respects Key Track)** — while Freeze holds, the held
	/// envelope is re-read at hz / 2^(st/12), so it moves with the keyboard like a transposed recording. It adds to Formant Shift on
	/// the same re-read (one resampling path). 0 is the held envelope where it was caught.
	void setFreezeShift(double semitones) noexcept { freezeShift_ = semitones; }
	/// TEST ONLY: the frozen envelope's transposition now, semitones.
	[[nodiscard]] double freezeShiftForTest() const noexcept { return freezeShift_; }
	/// Whether the vocoder still sounds: active, or fading out (frames must keep running until it has).
	[[nodiscard]] bool vocodeBusy() const noexcept { return vocodeActive() || vocodeMix_ > 0.0f; }
	/// The sidechain's envelope as last estimated, on the grid in dB (attack and release applied).
	[[nodiscard]] const std::array<float, kEnvelopePoints>& sidechainEnvelope(int k = 0) const noexcept { return side_[static_cast<size_t>(k)].gridS; }
	/// **THE FROZEN ENVELOPE FOR THE PATCH (Z10b)**: whether sidechain channel `k`'s envelope is held (Freeze on and caught), its
	/// flatness above 2 kHz, and what this stop's grid adds to read dBFS (the FFT's magnitude scaled by pi / N, as the Gate does).
	[[nodiscard]] bool frozen(int k) const noexcept { return vocode_.freeze && side_[static_cast<size_t>(k)].primed; }
	[[nodiscard]] float sidechainFlatness(int k) const noexcept { return side_[static_cast<size_t>(k)].flatness; }
	[[nodiscard]] double envelopeToFs() const noexcept {
		return 20.0 * std::log10(3.141592653589793 / static_cast<double>(std::max(1, n_)));
	}
	/// Z10b: CATCH channel `k`'s envelope from a saved one (`dbfs`, kEnvelopePoints in dBFS, read onto this stop's grid) and its
	/// flatness, as if the sidechain had just made it: held from the next frame while Freeze is on.
	void primeFrozen(int k, const float* dbfs, float flatness) noexcept;
	/// TEST ONLY: the envelope as last IMPOSED (Range held, Gate closed), on the grid in dB.
	[[nodiscard]] const std::array<float, kEnvelopePoints>& imposedEnvelopeForTest(int k = 0) const noexcept {
		const Side& s = side_[static_cast<size_t>(k)];
		return s.copy ? s.gridE : s.gridS;
	}
	/// TEST ONLY: the vocoder's crossfade weight now (0 off, 1 fully on).
	[[nodiscard]] float vocodeMixForTest() const noexcept { return vocodeMix_; }
	/// TEST ONLY: the internal carrier's sample written `ago` samples before the newest (0 while the input is the carrier).
	[[nodiscard]] float carrierAgoForTest(int ago) const noexcept {
		return carrierHistory_.empty() ? 0.0f : carrierHistory_[(write_ - 1 - static_cast<size_t>(ago)) & mask_];
	}
	/// TEST ONLY: the sidechain sample written `ago` samples before the newest (0 is the newest).
	[[nodiscard]] float sidechainAgoForTest(int ago) const noexcept {
		const std::vector<float>& h = side_[0].history;
		return h.empty() ? 0.0f : h[(write_ - 1 - static_cast<size_t>(ago)) & mask_];
	}

	/// IN PLACE. `right` may be null: the left channel alone (the mono road).
	void process(float* left, float* right, int frames) noexcept;

	/// TEST ONLY: frames computed since the last reset, and the crossfade's weight now.
	[[nodiscard]] uint64_t framesForTest() const noexcept { return frames_; }
	[[nodiscard]] float wetWeightForTest() const noexcept { return w_; }

private:
	struct Aligned {
		float* p = nullptr;
		void allocate(size_t count);
		void release() noexcept;
	};
	void computeFrame(int channels) noexcept;
	/// Sidechain `k`'s frame, its spectrum and its smoothed and imposed envelopes (`side_[k]`), once a frame; answers whether it held any sound.
	bool sidechainFrame(int k) noexcept;
	/// The vocoder on channel `c`'s spectrum (spectrum_), before the operators' gains.
	void vocodeSpectrum(float mix, int c) noexcept;
	/// Formant Shift on the input's own envelope (not vocoding): spectrum_ times E_x(hz / r) / E_x(hz).
	void shiftSpectrum() noexcept;
	/// The left channel's windowed frame into `frame_` (window applied); answers whether any sample was non-zero.
	bool loadFrame(int channel, bool carrier) noexcept;
	/// `spectrum_` (pffft ordered, before the gains) into `bands_`.
	void measureBands() noexcept;
	void release() noexcept;

	double sampleRate_ = 0.0;
	Resolution resolution_ = Resolution::Balanced;
	uint32_t latency_ = 0;
	int n_ = 0;
	int pad_ = 0;

	std::array<PFFFT_Setup*, kResolutionCount> setups_ {};
	Aligned frame_, spectrum_, work_;
	std::vector<float> analysisWindow_;  ///< sqrt-Hann
	std::vector<float> synthesisWindow_; ///< sqrt-Hann · 1/(2N): the overlap and pffft's unscaled inverse folded in
	std::vector<float> gains_;
	// THE VOCODER'S STATE (allocated in prepare).
	Vocode vocode_ {};
	EnvelopeEstimator estimator_;
	Aligned sideSpectrum_;
	Aligned drySpectrum_; ///< the input's spectrum, kept while an internal carrier crossfades in or out
	std::vector<float> mag_, envX_;
	std::array<float, kEnvelopePoints> gridX_ {};
	/// **ONE SIDECHAIN'S ANALYSIS** (design §10.3, §10.9): its ring, its envelope on the grid (S, smoothed), as imposed (E: Range
	/// held, Gate closed, a copy only while either acts) and at each bin (env), its peak and its flatness. The first is the mono
	/// sum (Mono, Spread) or the left channel (Stereo); the second, the right channel, only in Stereo.
	struct Side {
		std::vector<float> history;
		std::array<float, kEnvelopePoints> gridS {};
		std::array<float, kEnvelopePoints> gridE {};
		std::vector<float> env;
		float maxDb = kEnvelopeFloorDb;
		float flatness = 0.0f;
		bool primed = false;
		bool copy = false;
	};
	std::array<Side, 2> side_ {};
	std::vector<float> fshiftRatio_; ///< per bin, 1 everywhere until the caller writes it
	bool fshiftOn_ = false;
	double freezeShift_ = 0.0; ///< `setFreezeShift`
	float vocodeMix_ = 0.0f;
	uint32_t noiseRng_ = 0x2545f491u;

	struct Channel {
		std::vector<float> history; ///< the raw input, a power-of-two ring
		std::vector<float> ola;     ///< N: the overlap-add accumulator
		std::vector<float> out;     ///< H: the finalised samples, played over the next hop
	};
	std::array<Channel, 2> ch_ {};
	std::vector<float> carrierHistory_; ///< the internal carrier, the same ring again (zeros while the input is the carrier)
	double carrierPhase_ = 0.0; ///< the square's phase, 0..1
	uint32_t carrierRng_ = 0x9e3779b9u; ///< the noises' xorshift, seeded at reset
	std::array<float, 7> pink_ {}; ///< Kellet's pink filter state
	double f0Hz_ = 261.6255653005986; ///< the operators' f0 (`setF0Hz`)
	double carrierHz_ = 261.6255653005986; ///< the square carrier's pitch (`setCarrierHz`)
	Partials partials_ {};
	std::vector<float> partialsGain_; ///< per bin, the mask the first channel chose (the second takes the same)
	std::vector<int> peaks_; ///< scratch: the frame's local maxima
	void partialsSpectrum(bool choose) noexcept;
	/// Bandwidth below 1: sidechain `sd`'s per-bin envelope rebuilt through discrete Gaussian bands (design §10.9).
	void bandEnvelope(Side& sd, int bins, double binHz) noexcept;
	/// THE BAND LAYOUT (design §10.7.2, Z1): per bin, the first band within reach and up to kBandReach weights summing to 1.
	static constexpr int kBandReach = 9;
	struct BandKey {
		int count = 0;
		double bw = -1.0;
		double low = 0.0;
		double high = 0.0;
		double binHz = 0.0;
		int bins = 0;
		int zone = -1;
		double zoneAmount = -1.0;
		double f0 = 0.0;
		uint64_t picks = 0;
		int widthZone = -1;
		double widthAmount = -1.0;
		double widthF0 = 0.0;
		friend bool operator==(const BandKey&, const BandKey&) = default;
	};
	void rebuildBands(int bins, double binHz) noexcept;
	/// The carrier's energy per band on the layout's weights, into envX_ as each bin's flattening amplitude.
	void bandCarrier(int bins) noexcept;
	BandKey bandKey_ {};
	int bandCount_ = 0;
	std::array<int, static_cast<size_t>(kMaxBands) + 1> bandCentreBin_ {};
	std::array<double, static_cast<size_t>(kMaxBands) + 1> bandCentre_ {}; ///< log2 Hz
	std::array<double, static_cast<size_t>(kMaxBands) + 1> bandSigma_ {};  ///< log2 Hz
	/// **HARMONIC** (design §10.7.2, Z3): the carrier's harmonics of f0 (or the gaps between them, inharmonic) in the Range are
	/// the MEMBERS; each band covers a GROUP of neighbouring members, centred on its loudest member (quietest for the gaps) as the
	/// carrier last showed it (`pickMembers`). The grouping and the layout's arithmetic are the shared ones (dsp/vocode_bands.h),
	/// which the additive engine's vocoder lays too.
	void regroup(int count, double low, double high, bool inharmonic) noexcept;
	void pickMembers() noexcept;
	[[nodiscard]] double memberHz(int m) const noexcept { return grouping_.memberHz(m); }
	feathers::spectral::BandGrouping grouping_ {};
	uint64_t groupings_ = 0;
	uint64_t pickVersion_ = 0;
	std::vector<int> bandFirst_;
	std::vector<float> bandW_;
	std::vector<float> bandU_; ///< each bin's band coordinate (band b centred at u = b, linear between centres), for Spread
	[[nodiscard]] float nextCarrier() noexcept;
	const float* sideL_ = nullptr;
	const float* sideR_ = nullptr;
	bool sideSounding_ = false;
	size_t mask_ = 0;
	size_t write_ = 0;
	int hopCount_ = 0;
	int outPos_ = 0;

	bool identity_ = true;
	bool running_ = false;  ///< frames are being computed
	int framesSinceStart_ = 0;
	float w_ = 0.0f;        ///< the crossfade's wet weight
	int wSteps_ = 0;        ///< w_ in samples of the hop: w_ = wSteps_ / H, exactly 1 at H
	float mix_ = 1.0f;       ///< the Mix this sample, ramping toward mixTarget_ across a block
	float mixTarget_ = 1.0f;
	bool mixFresh_ = true;   ///< nothing processed since the reset: a new Mix is taken at once, not ramped to
	float mixStep_ = 0.0f;
	int mixLeft_ = 0;        ///< samples of the ramp still to go
	uint64_t frames_ = 0;
	bool analyse_ = false;
	uint64_t analysed_ = 0;
	std::array<float, kSpectrumBands> bands_ {};
};

} // namespace feathers::fx::spectral
