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

#include "fx/conv/convolver.h"

#include "pffft/pffft.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace feathers::fx::conv {

namespace {

constexpr size_t B0 = Convolver::kBlock;

float* allocFloats(size_t count) {
	auto* p = static_cast<float*>(pffft_aligned_malloc(std::max<size_t>(count, 1) * sizeof(float)));
	std::memset(p, 0, std::max<size_t>(count, 1) * sizeof(float));
	return p;
}

void freeFloats(float*& p) {
	if (p != nullptr) {
		pffft_aligned_free(p);
		p = nullptr;
	}
}

/// THE TAPS A STAGE COVERS for an IR of `frames`: [offset, end), end the next stage's offset (or the IR's end).
/// Under Lite, stage 1 reaches `kLiteSplit` and stage 2 covers the rest at half rate.
size_t stageEnd(size_t s, size_t frames, bool lite) {
	if (s == 1 && lite) {
		return std::min(kLiteSplit, frames);
	}
	return s + 1 < kStages.size() ? std::min(kStages[s + 1].offset, frames) : frames;
}

size_t stageTaps(size_t s, size_t frames, bool lite = false) {
	const size_t from = s == 2 && lite ? kLiteSplit : kStages[s].offset;
	const size_t to = stageEnd(s, frames, lite);
	return to > from ? to - from : 0;
}

/// THE HALF-RATE KERNEL'S LENGTH, in half-rate samples: the tail from `kStages[2].offset + 2·delay` on, halved.
size_t halfKernelSamples(size_t frames) {
	const size_t from = kStages[2].offset + 2 * kHalfbandDelay;
	return frames > kLiteSplit ? (frames - from + 1) / 2 : 0;
}

size_t stagePartitionsFor(size_t s, size_t frames, bool lite = false) {
	if (s == 2 && lite) {
		return (halfKernelSamples(frames) + kHalfBlock - 1) / kHalfBlock;
	}
	const size_t b = kStages[s].block;
	return (stageTaps(s, frames, lite) + b - 1) / b;
}

} // namespace

const std::array<float, kHalfbandTaps>& halfband() noexcept {
	// A Kaiser-windowed sinc at a quarter of the rate. Every tap an even distance from the centre (but the centre)
	// is zero by construction, and is SET to zero, so the filters can skip them.
	static const std::array<float, kHalfbandTaps> taps = [] {
		std::array<float, kHalfbandTaps> h {};
		constexpr double kBeta = 7.0;
		const auto i0 = [](double x) {
			double sum = 1.0, term = 1.0;
			for (int k = 1; k < 50; ++k) {
				term *= (0.25 * x * x) / (static_cast<double>(k) * static_cast<double>(k));
				sum += term;
			}
			return sum;
		};
		double total = 0.0;
		const auto m = static_cast<double>(kHalfbandDelay);
		for (size_t k = 0; k < kHalfbandTaps; ++k) {
			const double d = static_cast<double>(k) - m;
			const long off = static_cast<long>(k) - static_cast<long>(kHalfbandDelay);
			double v = 0.0;
			if (off == 0) {
				v = 0.5;
			}
			else if (off % 2 != 0) {
				const double arg = 3.14159265358979323846 * 0.5 * d;
				const double u = d / m;
				v = 0.5 * std::sin(arg) / arg * i0(kBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0(kBeta);
			}
			h[k] = static_cast<float>(v);
			total += v;
		}
		for (float& v : h) {
			v = static_cast<float>(v / total);
		}
		return h;
	}();
	return taps;
}

//--------------------------------------------------------------------------------------------------//
// THE KERNEL                                                                                         //
//--------------------------------------------------------------------------------------------------//

ConvKernel::~ConvKernel() { release(); }

void ConvKernel::release() noexcept {
	for (int c = 0; c < 2; ++c) {
		freeFloats(head_[c]);
		for (auto& stage : spectra_) {
			freeFloats(stage[c]);
		}
	}
	stagePartitions_ = {};
	lite_ = false;
	irFrames_ = 0;
	peakGain_ = 0.0f;
	rmsGain_ = 0.0f;
}

size_t ConvKernel::partitions() const noexcept {
	size_t n = 0;
	for (const size_t p : stagePartitions_) {
		n += p;
	}
	return n;
}

void ConvKernel::build(const float* left, const float* right, size_t frames, bool lite) {
	release();
	irFrames_ = frames;
	lite_ = lite;
	const float* ir[2] = { left, right };
	for (int c = 0; c < 2; ++c) {
		// THE HEAD, REVERSED, so its dot product with the mirrored history runs forward in memory.
		head_[c] = allocFloats(kHeadTaps);
		for (size_t k = 0; k < kHeadTaps; ++k) {
			const size_t tap = kHeadTaps - 1 - k;
			head_[c][k] = tap < frames ? ir[c][tap] : 0.0f;
		}
	}
	for (size_t s = 0; s < kStages.size(); ++s) {
		const bool half = s == 2 && lite;
		const size_t b = half ? kHalfBlock : kStages[s].block;
		const size_t n = 2 * b;
		const size_t parts = stagePartitionsFor(s, frames, lite);
		stagePartitions_[s] = parts;
		PFFFT_Setup* setup = pffft_new_setup(static_cast<int>(n), PFFFT_REAL);
		float* window = allocFloats(n);
		float* work = allocFloats(n);
		for (int c = 0; c < 2; ++c) {
			spectra_[s][c] = allocFloats(parts * n);
			// THE STAGE'S TAPS, as the partitions will read them: full-rate taps from its offset, or — at half
			// rate — the tail ADVANCED by both filters' delay, low-passed (zero phase), and every second sample,
			// doubled: the half-rate sum of a band-limited product is half the full-rate one.
			std::vector<float> taps;
			if (half) {
				const size_t from = kStages[2].offset + 2 * kHalfbandDelay;
				const size_t len = frames > from ? frames - from : 0;
				std::vector<float> hs(len, 0.0f);
				for (size_t t = 0; t < len; ++t) {
					hs[t] = from + t >= kLiteSplit ? ir[c][from + t] : 0.0f; // stage 1 has everything before
				}
				const std::array<float, kHalfbandTaps>& hb = halfband();
				taps.assign(halfKernelSamples(frames), 0.0f);
				for (size_t j = 0; j < taps.size(); ++j) {
					double acc = 0.0;
					for (size_t k = 0; k < kHalfbandTaps; ++k) {
						const long t = static_cast<long>(2 * j) + static_cast<long>(k) - static_cast<long>(kHalfbandDelay);
						if (t >= 0 && static_cast<size_t>(t) < len) {
							acc += static_cast<double>(hb[k]) * hs[static_cast<size_t>(t)];
						}
					}
					taps[j] = static_cast<float>(2.0 * acc);
				}
			}
			else {
				const size_t from = kStages[s].offset;
				const size_t end = stageEnd(s, frames, lite);
				taps.assign(end > from ? end - from : 0, 0.0f);
				for (size_t t = 0; t < taps.size(); ++t) {
					taps[t] = ir[c][from + t];
				}
			}
			// EACH PARTITION: B taps in the first half of a 2B frame, zeros in the second.
			for (size_t p = 0; p < parts; ++p) {
				std::fill(window, window + n, 0.0f);
				for (size_t j = 0; j < b; ++j) {
					const size_t tap = p * b + j;
					window[j] = tap < taps.size() ? taps[tap] : 0.0f;
				}
				pffft_transform(setup, window, spectra_[s][c] + p * n, work, PFFFT_FORWARD);
			}
		}
		freeFloats(window);
		freeFloats(work);
		pffft_destroy_setup(setup);
	}


	// THE PEAK GAIN: the mid's spectrum over one transform as long as the IR (a power of two), its largest
	// magnitude.
	if (frames > 0) {
		size_t n = 64;
		while (n < frames) {
			n *= 2;
		}
		PFFFT_Setup* big = pffft_new_setup(static_cast<int>(n), PFFFT_REAL);
		double midEnergy = 0.0;
		float* in = allocFloats(n);
		float* out = allocFloats(n);
		float* scratch = allocFloats(n);
		for (size_t i = 0; i < frames; ++i) {
			in[i] = 0.5f * (left[i] + right[i]);
			midEnergy += static_cast<double>(in[i]) * in[i];
		}
		pffft_transform_ordered(big, in, out, scratch, PFFFT_FORWARD);
		// Ordered real output: out[0] = DC, out[1] = Nyquist, then (re, im) pairs for bins 1..n/2-1.
		float peak = std::max(std::fabs(out[0]), std::fabs(out[1]));
		for (size_t k = 1; k < n / 2; ++k) {
			peak = std::max(peak, std::hypot(out[2 * k], out[2 * k + 1]));
		}
		peakGain_ = peak;
		rmsGain_ = static_cast<float>(std::sqrt(midEnergy));
		freeFloats(in);
		freeFloats(out);
		freeFloats(scratch);
		pffft_destroy_setup(big);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE CONVOLVER                                                                                      //
//--------------------------------------------------------------------------------------------------//

Convolver::~Convolver() { release(); }

void Convolver::release() noexcept {
	for (Stage& st : stages_) {
		freeFloats(st.fdl);
		for (int c = 0; c < 2; ++c) {
			freeFloats(st.accum[c]);
			freeFloats(st.out[c]);
			freeFloats(st.ready[c]);
		}
		freeFloats(st.work);
		freeFloats(st.window);
		if (st.setupHalf != nullptr) {
			pffft_destroy_setup(st.setupHalf);
			st.setupHalf = nullptr;
		}
		if (st.setup != nullptr) {
			pffft_destroy_setup(st.setup);
			st.setup = nullptr;
		}
		st.capacity = 0;
	}
	freeFloats(history_);
	freeFloats(input_);
	freeFloats(half_);
	freeFloats(interp_[0]);
	freeFloats(interp_[1]);
	kernel_ = nullptr;
	capacityFrames_ = 0;
}

void Convolver::prepare(size_t maxIrFrames) {
	release();
	owned_.release();
	capacityFrames_ = maxIrFrames;
	for (size_t s = 0; s < kStages.size(); ++s) {
		Stage& st = stages_[s];
		const size_t n = 2 * kStages[s].block;
		st.setup = pffft_new_setup(static_cast<int>(n), PFFFT_REAL);
		// ROOM FOR EITHER LAYOUT: Lite gives stage 1 one more partition, and stage 2 its half-rate ones.
		st.capacity = std::max<size_t>({ 1, stagePartitionsFor(s, maxIrFrames, false), stagePartitionsFor(s, maxIrFrames, true) });
		if (s == 2) {
			st.setupHalf = pffft_new_setup(static_cast<int>(2 * kHalfBlock), PFFFT_REAL);
		}
		st.fdl = allocFloats(st.capacity * n);
		for (int c = 0; c < 2; ++c) {
			st.accum[c] = allocFloats(n);
			st.out[c] = allocFloats(kStages[s].block);
			st.ready[c] = allocFloats(kStages[s].block);
		}
		st.work = allocFloats(n);
		st.window = allocFloats(n);
	}
	history_ = allocFloats(2 * B0);
	input_ = allocFloats(kInputRing);
	half_ = allocFloats(kHalfRing);
	interp_[0] = allocFloats(2 * kInterpRing);
	interp_[1] = allocFloats(2 * kInterpRing);
	reset();
}

void Convolver::setIr(const float* left, const float* right, size_t frames) {
	if (history_ == nullptr) {
		return;
	}
	owned_.build(left, right, std::min(frames, capacityFrames()));
	(void)setKernel(&owned_);
}

bool Convolver::setKernel(const ConvKernel* kernel) noexcept {
	if (kernel != nullptr) {
		for (size_t s = 0; s < kStages.size(); ++s) {
			if (kernel->stagePartitions_[s] > stages_[s].capacity) {
				return false;
			}
		}
	}
	kernel_ = kernel;
	reset();
	return true;
}

void Convolver::reset() noexcept {
	if (history_ == nullptr) {
		return;
	}
	for (size_t s = 0; s < kStages.size(); ++s) {
		Stage& st = stages_[s];
		const size_t n = 2 * kStages[s].block;
		std::fill(st.fdl, st.fdl + st.capacity * n, 0.0f);
		for (int c = 0; c < 2; ++c) {
			std::fill(st.accum[c], st.accum[c] + n, 0.0f);
			std::fill(st.out[c], st.out[c] + kStages[s].block, 0.0f);
			std::fill(st.ready[c], st.ready[c] + kStages[s].block, 0.0f);
		}
		st.fdlHead = 0;
		st.next = 0;
	}
	std::fill(history_, history_ + 2 * B0, 0.0f);
	std::fill(input_, input_ + kInputRing, 0.0f);
	std::fill(half_, half_ + kHalfRing, 0.0f);
	std::fill(interp_[0], interp_[0] + 2 * kInterpRing, 0.0f);
	std::fill(interp_[1], interp_[1] + 2 * kInterpRing, 0.0f);
	halfPos_ = 0;
	interpPos_ = 0;
	samples_ = 0;
	inputPos_ = 0;
	pos_ = 0;
	tick_ = 0;
	ffts_ = 0;
}

void Convolver::recentInput(float* dst, size_t n) const noexcept {
	const size_t start = (inputPos_ + kInputRing - n) & (kInputRing - 1);
	const size_t first = std::min(n, kInputRing - start);
	std::memcpy(dst, input_ + start, first * sizeof(float));
	if (first < n) {
		std::memcpy(dst + first, input_, (n - first) * sizeof(float));
	}
}
void Convolver::recentHalf(float* dst, size_t n) const noexcept {
	const size_t start = (halfPos_ + kHalfRing - n) & (kHalfRing - 1);
	const size_t first = std::min(n, kHalfRing - start);
	std::memcpy(dst, half_ + start, first * sizeof(float));
	if (first < n) {
		std::memcpy(dst + first, half_, (n - first) * sizeof(float));
	}
}


void Convolver::stageEdge(size_t s) noexcept {
	Stage& st = stages_[s];
	const bool half = halfStage(s);
	const size_t b = half ? kHalfBlock : kStages[s].block;
	PFFFT_Setup* const setup = half ? st.setupHalf : st.setup;
	const size_t n = 2 * b;
	const size_t parts = kernel_->stagePartitions_[s];
	const float scale = 1.0f / static_cast<float>(n);
	const auto accumulate = [&](size_t p) {
		const size_t idx = (st.fdlHead + st.capacity - p) % st.capacity;
		for (int c = 0; c < 2; ++c) {
			pffft_zconvolve_accumulate(setup, st.fdl + idx * n, kernel_->spectra_[s][c] + p * n, st.accum[c],
			                           scale);
			work_ += n;
		}
	};
	const auto emit = [&]() {
		// OVERLAP-SAVE: the second half of the inverse is the valid linear convolution.
		for (int c = 0; c < 2; ++c) {
			pffft_transform(setup, st.accum[c], st.accum[c], st.work, PFFFT_BACKWARD);
			work_ += n * static_cast<uint64_t>(std::log2(static_cast<double>(n)));
			std::memcpy(st.out[c], st.accum[c] + b, b * sizeof(float));
			std::fill(st.accum[c], st.accum[c] + n, 0.0f);
		}
	};
	const auto push = [&]() {
		// THE WINDOW [previous block, this block] INTO THE FREQUENCY-DOMAIN DELAY LINE.
		if (half) {
			recentHalf(st.window, n);
		}
		else {
			recentInput(st.window, n);
		}
		st.fdlHead = (st.fdlHead + 1) % st.capacity;
		pffft_transform(setup, st.window, st.fdl + st.fdlHead * n, st.work, PFFFT_FORWARD);
		work_ += n * static_cast<uint64_t>(std::log2(static_cast<double>(n)));
		if (s == 0) {
			++ffts_;
		}
	};
	if (kStages[s].lag == 1) {
		// THE BLOCK JUST TAKEN IS NEEDED BY THE NEXT ONE: all of it, now.
		push();
		for (size_t p = 0; p < parts; ++p) {
			accumulate(p);
		}
		emit();
	}
	else {
		// THE BLOCK THAT STARTS NOW WAS FINISHED DURING THE LAST ONE (`stageStep`, into `ready`): play it, and
		// take the new input for the one after. Only this FFT lands at the edge.
		std::swap(st.out, st.ready);
		push();
		st.next = 0;
	}
}

void Convolver::stageStep(size_t s) noexcept {
	// **THE WORK FOR THE NEXT BLOCK, SPREAD.** Steps 0 .. R-3 of the block accumulate the partitions a share at a
	// time; step R-2, the last before the edge, finishes any left and runs the two inverse FFTs into `ready`.
	// So an edge carries one forward FFT and nothing else — no spike where a big stage turns over.
	Stage& st = stages_[s];
	const bool half = halfStage(s);
	const size_t b = half ? kHalfBlock : kStages[s].block;
	PFFFT_Setup* const setup = half ? st.setupHalf : st.setup;
	const size_t parts = kernel_->stagePartitions_[s];
	const size_t steps = kStages[s].block / B0; // R: base blocks per stage block (the edge period, either rate)
	const size_t accumulating = steps - 3;   // steps that accumulate (R >= 8 for every lag-2 stage)
	const size_t step = static_cast<size_t>(tick_ % steps);
	const size_t n = 2 * b;
	const float scale = 1.0f / static_cast<float>(n);
	const auto accumulate = [&](size_t p) {
		const size_t idx = (st.fdlHead + st.capacity - p) % st.capacity;
		for (int c = 0; c < 2; ++c) {
			pffft_zconvolve_accumulate(setup, st.fdl + idx * n, kernel_->spectra_[s][c] + p * n, st.accum[c],
			                           scale);
			work_ += n;
		}
	};
	if (step < accumulating) {
		const size_t per = (parts + accumulating - 1) / accumulating;
		for (size_t k = 0; k < per && st.next < parts; ++k) {
			accumulate(st.next++);
		}
		return;
	}
	while (st.next < parts) {
		accumulate(st.next++);
	}
	// OVERLAP-SAVE: the second half of the inverse is the valid linear convolution. ONE CHANNEL PER STEP — step
	// R-3 the left, R-2 the right — so no step carries two of the stage's biggest FFTs. MEASURED: both on one step
	// made the worst 64-frame call 11.8x the mean on a 6 s IR.
	const int c = step == steps - 3 ? 0 : 1;
	{
		pffft_transform(setup, st.accum[c], st.accum[c], st.work, PFFFT_BACKWARD);
		work_ += n * static_cast<uint64_t>(std::log2(static_cast<double>(n)));
		std::memcpy(st.ready[c], st.accum[c] + b, b * sizeof(float));
		std::fill(st.accum[c], st.accum[c] + n, 0.0f);
	}
}

void Convolver::endOfBaseBlock() noexcept {
	for (size_t s = 0; s < kStages.size(); ++s) {
		if (kernel_->stagePartitions_[s] == 0) {
			continue; // an IR too short to reach this stage
		}
		const uint64_t steps = kStages[s].block / B0;
		if ((tick_ + 1) % steps == 0) {
			stageEdge(s);
		}
		else if (kStages[s].lag == 2) {
			stageStep(s);
		}
	}
	++tick_;
}

void Convolver::process(const float* in, float* outL, float* outR, size_t frames) noexcept {
	if (history_ == nullptr || kernel_ == nullptr || kernel_->irFrames_ == 0) {
		return;
	}
	for (size_t i = 0; i < frames; ++i) {
		const float x = in[i];
		// THE HEAD'S HISTORY, MIRRORED so the last 128 inputs are always contiguous: slot r and r + 128.
		history_[pos_] = x;
		history_[pos_ + B0] = x;
		input_[inputPos_] = x;
		inputPos_ = (inputPos_ + 1) & (kInputRing - 1);
		const float* h = history_ + pos_ + 1;
		float y[2] = { 0.0f, 0.0f };
		for (int c = 0; c < 2; ++c) {
			const float* taps = kernel_->head_[c];
			float acc = 0.0f;
			for (size_t k = 0; k < B0; ++k) {
				acc += taps[k] * h[k];
			}
			y[c] = acc;
		}
		// LITE'S DECIMATOR: every even input sample, the halfband's output into the half-rate ring. Causal, so the
		// decimated signal lags by `kHalfbandDelay` — which the half-rate kernel was built advanced to meet.
		const bool lite = kernel_->lite_ && kernel_->stagePartitions_[2] > 0;
		const std::array<float, kHalfbandTaps>& hb = halfband();
		if (lite && (samples_ & 1u) == 0) {
			float acc = hb[kHalfbandDelay] * input_[(inputPos_ + kInputRing - 1 - kHalfbandDelay) & (kInputRing - 1)];
			for (size_t k = 0; k < kHalfbandTaps; k += 2) { // the odd offsets from the centre: the only other taps
				acc += hb[k] * input_[(inputPos_ + kInputRing - 1 - k) & (kInputRing - 1)];
			}
			half_[halfPos_] = acc;
			halfPos_ = (halfPos_ + 1) & (kHalfRing - 1);
			work_ += kHalfbandTaps / 2 + 1;
		}
		++samples_;
		// EACH STAGE'S BLOCK IN PLAY, at this sample's place in it.
		for (size_t s = 0; s < kStages.size(); ++s) {
			if (kernel_->stagePartitions_[s] == 0) {
				continue;
			}
			const uint64_t steps = kStages[s].block / B0;
			const size_t at = static_cast<size_t>(tick_ % steps) * B0 + pos_;
			if (halfStage(s)) {
				// LITE'S INTERPOLATOR, POLYPHASE: the half-rate block, zero-stuffed back to full rate and filtered by
				// the halfband at twice its gain — computed without the zeros. The halfband's nonzero taps are the
				// even ones and the (odd) centre, so an EVEN output sample reads the 24 even taps against the
				// half-rate history, and an ODD one reads the centre alone. MEASURED: the plain 47-tap form ate most
				// of what the half-rate stage saved (Lite did 91% of the exact work on a 6 s IR at 96 kHz).
				if ((at & 1u) == 0) {
					interpPos_ = interpPos_ + 1 == kInterpRing ? 0 : interpPos_ + 1;
					for (int c = 0; c < 2; ++c) {
						const float v = stages_[s].out[c][at / 2];
						interp_[c][interpPos_] = v;
						interp_[c][interpPos_ + kInterpRing] = v;
					}
				}
				const size_t newest = interpPos_ + kInterpRing; // mirrored: [newest - i] valid for i < kInterpRing
				for (int c = 0; c < 2; ++c) {
					float acc = 0.0f;
					if ((at & 1u) == 0) {
						for (size_t i = 0; 2 * i < kHalfbandTaps; ++i) {
							acc += hb[2 * i] * interp_[c][newest - i];
						}
					}
					else {
						acc = hb[kHalfbandDelay] * interp_[c][newest - (kHalfbandDelay - 1) / 2];
					}
					y[c] += 2.0f * acc;
				}
				work_ += (at & 1u) == 0 ? 2 * (kHalfbandTaps / 2 + 1) : 2;
				continue;
			}
			y[0] += stages_[s].out[0][at];
			y[1] += stages_[s].out[1][at];
		}
		work_ += 2 * B0;
		outL[i] += y[0];
		outR[i] += y[1];
		if (++pos_ == B0) {
			pos_ = 0;
			endOfBaseBlock();
		}
	}
}

} // namespace feathers::fx::conv
