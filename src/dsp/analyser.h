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

#include "dsp/analyser_bins.h"
#include "pffft/pffft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// THE SPECTRUM ANALYSER, LIFTED FROM eq.cpp (rack stage 2, §7a: "reuse the EQ's analyser; don't write a second one").
// The class is the EQ's, operation for operation — `test_fx_eq`'s "lifting the analyser out of eq.cpp left the EQ's
// readings BIT-IDENTICAL" hashes every frame the EQ publishes against the numbers measured before the move. What
// changed is only its edges: the control-grid step it is driven at is a template parameter (the ring's slack is eight
// of them), the view is `AnalyserView`, and the frame is read through accessors rather than written into the EQ's
// reading layout (eq.cpp does that now).

namespace feathers::dsp {

/// THE ANALYSER'S AUDIO HALF (doc §4.1, step 4). Four taps — PRE and POST, left and right — each kept in a
/// ring of the last N samples. Every hop (N/4) the ring is windowed and FFT'd, ONE TAP PER CONTROL-GRID
/// STEP so no 32-sample step carries more than one transform, and reduced to 96 log bins of power
/// density; when the last enabled tap is done, a frame is published. A tap switched off (the extra-state
/// block's Pre / Post) costs no FFT at all.
///
/// THE VIEW (step 6, doc §7): Stereo is the mean of the two channels' power, Mid the power of (L+R)/2 and
/// Side of (L-R)/2 — from the SAME two transforms, through the cross spectrum: |L±R|²/4 = (|L|² + |R|² ±
/// 2 Re(L R*)) / 4. That needs L and R transformed over the SAME samples, so every tap of a hop reads the
/// window that ended AT THE HOP (`hopPos_`), and the ring carries `kRingSlack` more samples than the window
/// so the later taps' windows are not overwritten while they wait their turn.
///
/// Every buffer is allocated in `prepare`, never on the audio thread.
template <int GridSamples> class Analyser {
public:
	Analyser() = default;
	Analyser(const Analyser&) = delete;
	Analyser& operator=(const Analyser&) = delete;
	~Analyser() { release(); }

	/// Four taps, one a grid step, the last one at most four steps and a partial one after the hop: 160
	/// samples. Twice that.
	static constexpr int kRingSlack = 8 * GridSamples;

	void prepare(double sampleRate) {
		release();
		// 8192 at 44.1/48 kHz, 16384 above: the same ~5.9 Hz of resolution (§4.1).
		n_ = sampleRate > 50000.0 ? 16384 : 8192;
		hop_ = n_ / 4;
		ringLen_ = n_ + kRingSlack;
		setup_ = pffft_new_setup(n_, PFFFT_REAL);
		for (float*& ring : rings_) {
			ring = alloc(ringLen_);
		}
		window_ = alloc(n_);
		in_ = alloc(n_);
		out_ = alloc(n_);
		work_ = alloc(n_);
		left_ = alloc(n_);
		for (int i = 0; i < n_; ++i) {
			window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * i / n_));
		}
		// THE LOG-BIN MAP. A bin wider than an FFT bin averages the FFT bins inside it; a narrower one
		// takes the power interpolated at its centre.
		const double hzPerBin = sampleRate / n_;
		const double ratio = std::pow(kAnalyserHighHz / kAnalyserLowHz, 1.0 / static_cast<double>(kAnalyserBins));
		for (size_t k = 0; k < kAnalyserBins; ++k) {
			const double lo = kAnalyserLowHz * std::pow(ratio, static_cast<double>(k));
			const double hi = lo * ratio;
			Span& s = spans_[k];
			s.first = static_cast<int>(std::ceil(lo / hzPerBin));
			s.last = std::min(static_cast<int>(std::ceil(hi / hzPerBin)), n_ / 2);
			const double centre = analyserBinHz(k) / hzPerBin;
			s.at = std::clamp(static_cast<int>(std::floor(centre)), 1, n_ / 2 - 1);
			s.frac = static_cast<float>(centre - std::floor(centre));
		}
		// A Hann-windowed sine of amplitude A peaks at A N / 4 in its FFT bin: this puts that at A^2.
		norm_ = 16.0f / (static_cast<float>(n_) * static_cast<float>(n_));
		reset();
	}

	void reset() noexcept {
		if (setup_ == nullptr) {
			return;
		}
		for (float* ring : rings_) {
			std::fill(ring, ring + ringLen_, 0.0f);
		}
		for (auto& tap : power_) {
			tap.fill(0.0f);
		}
		for (auto& pair : cross_) {
			pair.fill(0.0f);
		}
		pos_ = 0;
		hopPos_ = 0;
		sinceHop_ = 0;
		pending_ = 0;
		frame_ = 0.0f;
		frameReady_ = false;
		fftsForTest_ = 0;
		for (float& v : dbPre_) {
			v = kAnalyserFloorDb;
		}
		for (float& v : dbPost_) {
			v = kAnalyserFloorDb;
		}
	}

	/// One sample of all four taps.
	void push(float preL, float preR, float postL, float postR) noexcept {
		if (setup_ == nullptr) {
			return;
		}
		rings_[0][pos_] = preL;
		rings_[1][pos_] = preR;
		rings_[2][pos_] = postL;
		rings_[3][pos_] = postR;
		pos_ = (pos_ + 1) % ringLen_;
		if (++sinceHop_ >= hop_) {
			sinceHop_ = 0;
			hopPos_ = pos_;
			pending_ = (preOn_ ? 0b0011 : 0) | (postOn_ ? 0b1100 : 0);
		}
	}

	/// ONE CONTROL-GRID STEP: at most one transform.
	void step() noexcept {
		if (pending_ == 0) {
			return;
		}
		int tap = 0;
		while ((pending_ & (1 << tap)) == 0) {
			++tap;
		}
		transform(tap);
		pending_ &= ~(1 << tap);
		if (pending_ == 0) {
			finishFrame();
		}
	}

	void setTaps(bool pre, bool post) noexcept {
		preOn_ = pre;
		postOn_ = post;
	}

	/// The view the NEXT finished frame is computed in.
	void setView(AnalyserView view) noexcept { view_ = view; }

	/// THE LAST FINISHED FRAME: its counter (cycling below 2^24, so a float counts it in whole numbers), the view it
	/// was computed in, and each tap's 96 bins in dB (`kAnalyserFloorDb` for a tap switched off).
	[[nodiscard]] float frame() const noexcept { return frame_; }
	[[nodiscard]] int frameView() const noexcept { return frameView_; }
	[[nodiscard]] const std::array<float, kAnalyserBins>& pre() const noexcept { return dbPre_; }
	[[nodiscard]] const std::array<float, kAnalyserBins>& post() const noexcept { return dbPost_; }

	[[nodiscard]] int fftsForTest() const noexcept { return fftsForTest_; }

private:
	struct Span {
		int first = 0, last = 0, at = 1;
		float frac = 0.0f;
	};

	static float* alloc(int count) {
		return static_cast<float*>(pffft_aligned_malloc(static_cast<size_t>(count) * sizeof(float)));
	}

	void release() noexcept {
		for (float*& ring : rings_) {
			pffft_aligned_free(ring);
			ring = nullptr;
		}
		for (float** p : { &window_, &in_, &out_, &work_, &left_ }) {
			pffft_aligned_free(*p);
			*p = nullptr;
		}
		if (setup_ != nullptr) {
			pffft_destroy_setup(setup_);
			setup_ = nullptr;
		}
	}

	/// A per-FFT-bin quantity reduced to the 96 log bins, the way power is.
	template <typename BinFn>
	void reduce(BinFn bin, std::array<float, kAnalyserBins>& to) const noexcept {
		for (size_t k = 0; k < kAnalyserBins; ++k) {
			const Span& s = spans_[k];
			float p = 0.0f;
			if (s.last > s.first) {
				for (int i = s.first; i < s.last; ++i) {
					p += bin(i);
				}
				p /= static_cast<float>(s.last - s.first);
			}
			else {
				p = bin(s.at) + s.frac * (bin(s.at + 1) - bin(s.at));
			}
			to[k] = p * norm_;
		}
	}

	void transform(int tap) noexcept {
		const float* ring = rings_[static_cast<size_t>(tap)];
		// THE WINDOW THAT ENDED AT THE HOP, oldest first — the same samples for every tap of this hop.
		const int start = (hopPos_ + ringLen_ - n_) % ringLen_;
		for (int i = 0; i < n_; ++i) {
			in_[i] = ring[(start + i) % ringLen_] * window_[i];
		}
		pffft_transform_ordered(setup_, in_, out_, work_, PFFFT_FORWARD);
		++fftsForTest_;
		// Ordered real layout: [DC, Nyquist, re1, im1, re2, im2, ...].
		const auto binPower = [&](int i) {
			if (i <= 0) {
				return out_[0] * out_[0];
			}
			if (i >= n_ / 2) {
				return out_[1] * out_[1];
			}
			return out_[2 * i] * out_[2 * i] + out_[2 * i + 1] * out_[2 * i + 1];
		};
		reduce(binPower, power_[static_cast<size_t>(tap)]);
		if ((tap & 1) == 0) {
			// A LEFT TAP: keep its spectrum for the right one's cross term, which is always the next transform.
			std::copy(out_, out_ + n_, left_);
			return;
		}
		// Re(L R*), bin by bin.
		const auto binCross = [&](int i) {
			if (i <= 0) {
				return left_[0] * out_[0];
			}
			if (i >= n_ / 2) {
				return left_[1] * out_[1];
			}
			return left_[2 * i] * out_[2 * i] + left_[2 * i + 1] * out_[2 * i + 1];
		};
		reduce(binCross, cross_[static_cast<size_t>(tap / 2)]);
	}

	void finishFrame() noexcept {
		const auto toDb = [](float p) {
			return p > 1e-16f ? 10.0f * std::log10(p) : kAnalyserFloorDb;
		};
		// Stereo: the mean of |L|² and |R|². Mid and Side: |L ± R|² / 4.
		const auto viewed = [this](size_t left, size_t right, size_t pair, size_t k) {
			const float l = power_[left][k], r = power_[right][k], c = cross_[pair][k];
			switch (view_) {
			case AnalyserView::Mid: return std::max(0.0f, 0.25f * (l + r + 2.0f * c));
			case AnalyserView::Side: return std::max(0.0f, 0.25f * (l + r - 2.0f * c));
			default: return 0.5f * (l + r);
			}
		};
		for (size_t k = 0; k < kAnalyserBins; ++k) {
			dbPre_[k] = preOn_ ? toDb(viewed(0, 1, 0, k)) : kAnalyserFloorDb;
			dbPost_[k] = postOn_ ? toDb(viewed(2, 3, 1, k)) : kAnalyserFloorDb;
		}
		frameView_ = static_cast<int>(view_);
		// THE COUNTER CYCLES BELOW 2^24, where a float still counts in whole numbers.
		frame_ = frame_ >= 16777215.0f ? 1.0f : frame_ + 1.0f;
		frameReady_ = true;
	}

	PFFFT_Setup* setup_ = nullptr;
	int n_ = 0, hop_ = 1, ringLen_ = 1;
	std::array<float*, 4> rings_ {};
	float* window_ = nullptr;
	float* in_ = nullptr;
	float* out_ = nullptr;
	float* work_ = nullptr;
	float* left_ = nullptr;
	std::array<Span, kAnalyserBins> spans_ {};
	std::array<std::array<float, kAnalyserBins>, 4> power_ {};
	std::array<std::array<float, kAnalyserBins>, 2> cross_ {};
	std::array<float, kAnalyserBins> dbPre_ {};
	std::array<float, kAnalyserBins> dbPost_ {};
	float norm_ = 1.0f;
	int pos_ = 0, hopPos_ = 0, sinceHop_ = 0, pending_ = 0;
	bool preOn_ = true, postOn_ = true;
	AnalyserView view_ = AnalyserView::Stereo;
	int frameView_ = 0;
	float frame_ = 0.0f;
	bool frameReady_ = false;
	int fftsForTest_ = 0;
};


} // namespace feathers::dsp
