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

#include "fx/conv/ir_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#if defined(_MSC_VER)
// dr_wav and dr_flac are vendored single-header libraries compiled in this TU; their warnings are theirs, as the GCC
// block above says for GCC.
#pragma warning(push, 0)
#endif
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO_WIDE
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace feathers::fx::conv {

namespace {

double besselI0(double x) {
	double sum = 1.0;
	double term = 1.0;
	const double q = 0.25 * x * x;
	for (int k = 1; k < 64; ++k) {
		term *= q / (static_cast<double>(k) * static_cast<double>(k));
		sum += term;
		if (term < 1.0e-12 * sum) {
			break;
		}
	}
	return sum;
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
	const std::string suf(suffix);
	if (s.size() < suf.size()) {
		return false;
	}
	for (size_t i = 0; i < suf.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(s[s.size() - suf.size() + i]))
		    != std::tolower(static_cast<unsigned char>(suf[i]))) {
			return false;
		}
	}
	return true;
}

} // namespace

std::vector<float> resampleOffline(const float* in, size_t frames, double fromRate, double toRate) {
	if (frames == 0 || fromRate <= 0.0 || toRate <= 0.0) {
		return {};
	}
	if (fromRate == toRate) {
		return std::vector<float>(in, in + frames);
	}
	const double ratio = toRate / fromRate;
	const auto outFrames = static_cast<size_t>(std::ceil(static_cast<double>(frames) * ratio));
	// The cutoff, as a fraction of the SOURCE Nyquist: below the lower of the two, with a little room
	// for the window's transition band.
	const double fc = std::min(1.0, ratio) * 0.97;
	const double halfWidth = static_cast<double>(kResampleZeros) / fc; // in source samples
	constexpr double kBeta = 8.6;
	const double i0Beta = besselI0(kBeta);
	// THE WINDOW, TABULATED over |u| in [0, 1] and read with linear interpolation: a Bessel series per tap
	// would cost ~20x the multiply it scales.
	constexpr size_t kTable = 4096;
	std::vector<double> window(kTable + 2);
	for (size_t k = 0; k <= kTable + 1; ++k) {
		const double u = std::min(1.0, static_cast<double>(k) / static_cast<double>(kTable));
		window[k] = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - u * u))) / i0Beta;
	}
	constexpr double kPi = 3.14159265358979323846;

	std::vector<float> out(outFrames);
	for (size_t j = 0; j < outFrames; ++j) {
		const double t = static_cast<double>(j) / ratio;
		const auto lo = static_cast<long long>(std::ceil(t - halfWidth));
		const auto hi = static_cast<long long>(std::floor(t + halfWidth));
		double acc = 0.0;
		for (long long i = std::max(lo, 0LL); i <= hi && i < static_cast<long long>(frames); ++i) {
			const double x = static_cast<double>(i) - t;
			const double u = x / halfWidth;
			const double at = std::fabs(u) * static_cast<double>(kTable);
			const auto k = std::min(static_cast<size_t>(at), kTable);
			const double w = window[k] + (at - static_cast<double>(k)) * (window[k + 1] - window[k]);
			const double arg = kPi * fc * x;
			const double sinc = x == 0.0 ? 1.0 : std::sin(arg) / arg;
			acc += static_cast<double>(in[i]) * fc * sinc * w;
		}
		out[j] = static_cast<float>(acc);
	}
	return out;
}

LoadedIr resampleIr(const float* interleaved, size_t frames, unsigned channels, double fileRate, double hostRate) {
	LoadedIr ir;
	ir.fileSampleRate = fileRate;
	ir.fileChannels = channels;
	ir.sampleRate = hostRate;
	if (interleaved == nullptr || frames == 0 || channels == 0) {
		ir.error = "empty file";
		return ir;
	}
	if (fileRate <= 0.0 || hostRate <= 0.0) {
		ir.error = "no sample rate";
		return ir;
	}
	const auto keep = std::min(frames, static_cast<size_t>(std::ceil(kIrMaxFileSeconds * fileRate)));
	ir.truncated = keep < frames;
	std::vector<float> l(keep);
	std::vector<float> r(keep);
	const unsigned rc = channels >= 2 ? 1u : 0u;
	for (size_t i = 0; i < keep; ++i) {
		l[i] = interleaved[i * channels];
		r[i] = interleaved[i * channels + rc];
	}
	ir.left = resampleOffline(l.data(), l.size(), fileRate, hostRate);
	ir.right = resampleOffline(r.data(), r.size(), fileRate, hostRate);
	ir.ok = true;
	return ir;
}

LoadedIr shapeIr(const LoadedIr& whole, float begin, float end, float stretch, const std::vector<float>& envelope,
                 double maxSeconds) {
	LoadedIr ir;
	ir.fileSampleRate = whole.fileSampleRate;
	ir.fileChannels = whole.fileChannels;
	ir.sampleRate = whole.sampleRate;
	if (!whole.ok || whole.left.empty()) {
		ir.error = whole.error.empty() ? "empty file" : whole.error;
		return ir;
	}
	const size_t total = std::min(whole.left.size(), whole.right.size());
	float b = std::clamp(std::isfinite(begin) ? begin : 0.0f, 0.0f, 1.0f);
	float e = std::clamp(std::isfinite(end) ? end : 1.0f, 0.0f, 1.0f);
	if (e <= b) {
		b = 0.0f;
		e = 1.0f;
	}
	const auto from = static_cast<size_t>(std::floor(static_cast<double>(b) * static_cast<double>(total)));
	auto to = static_cast<size_t>(std::ceil(static_cast<double>(e) * static_cast<double>(total)));
	to = std::clamp(to, from + 1, total);
	std::vector<float> l(whole.left.begin() + static_cast<std::ptrdiff_t>(from),
	                     whole.left.begin() + static_cast<std::ptrdiff_t>(to));
	std::vector<float> r(whole.right.begin() + static_cast<std::ptrdiff_t>(from),
	                     whole.right.begin() + static_cast<std::ptrdiff_t>(to));

	// THE ENVELOPE, OVER THE SPAN: `envelope` is gains at evenly spaced points from the span's first
	// sample to its last, read with linear interpolation. Before the stretch, so it is drawn over the
	// span as the file holds it and stretches with it.
	if (envelope.size() >= 2) {
		const double last = static_cast<double>(l.size() > 1 ? l.size() - 1 : 1);
		const double points = static_cast<double>(envelope.size() - 1);
		for (size_t i = 0; i < l.size(); ++i) {
			const double at = static_cast<double>(i) / last * points;
			const auto k = std::min(static_cast<size_t>(at), envelope.size() - 2);
			const auto g = static_cast<float>(envelope[k] + (at - static_cast<double>(k)) * (envelope[k + 1] - envelope[k]));
			l[i] *= g;
			r[i] *= g;
		}
	}
	else if (envelope.size() == 1) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] *= envelope[0];
			r[i] *= envelope[0];
		}
	}

	// THE STRETCH: the span resampled to `stretch` times its length — longer and lower together.
	const float st = std::clamp(std::isfinite(stretch) ? stretch : 1.0f, 0.01f, 100.0f);
	if (st != 1.0f) {
		l = resampleOffline(l.data(), l.size(), 1.0, static_cast<double>(st));
		r = resampleOffline(r.data(), r.size(), 1.0, static_cast<double>(st));
	}

	bool cutEnd = to < total;
	const auto cap = static_cast<size_t>(std::floor(maxSeconds * whole.sampleRate));
	if (l.size() > cap) {
		l.resize(cap);
		r.resize(cap);
		ir.truncated = true;
		cutEnd = true;
	}
	const size_t n = l.size();
	if (from > 0) {
		const auto fade = std::min(n, static_cast<size_t>(kIrBeginFadeSeconds * whole.sampleRate));
		for (size_t i = 0; i < fade; ++i) {
			const float g = static_cast<float>(i) / static_cast<float>(fade);
			l[i] *= g;
			r[i] *= g;
		}
	}
	if (cutEnd) {
		const auto fade = std::min(n, static_cast<size_t>(kIrCutFadeSeconds * whole.sampleRate));
		for (size_t i = 0; i < fade; ++i) {
			const float g = static_cast<float>(i) / static_cast<float>(fade);
			l[n - 1 - i] *= g;
			r[n - 1 - i] *= g;
		}
	}

	double energy = 0.0;
	for (size_t i = 0; i < n; ++i) {
		energy += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
	}
	energy *= 0.5;
	if (!(energy > 1.0e-20)) {
		ir.error = "silent file";
		return ir;
	}
	const auto gain = static_cast<float>(1.0 / std::sqrt(energy));
	for (size_t i = 0; i < n; ++i) {
		l[i] *= gain;
		r[i] *= gain;
	}
	ir.left = std::move(l);
	ir.right = std::move(r);
	ir.ok = true;
	return ir;
}

LoadedIr trimIr(const LoadedIr& whole, IrTrim trim, double maxSeconds) {
	return shapeIr(whole, trim.begin, trim.end, 1.0f, {}, maxSeconds);
}

LoadedIr loadIrFile(const std::string& path, double hostRate) {
	unsigned channels = 0;
	unsigned rate = 0;
	uint64_t frames = 0;
	float* data = nullptr;
	const bool flac = endsWithNoCase(path, ".flac");
	if (flac) {
		drflac_uint64 n = 0;
		data = drflac_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &n, nullptr);
		frames = n;
	} else {
		drwav_uint64 n = 0;
		data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &n, nullptr);
		frames = n;
	}
	if (data == nullptr) {
		LoadedIr ir;
		ir.error = "cannot read file";
		return ir;
	}
	LoadedIr ir = resampleIr(data, static_cast<size_t>(frames), channels, static_cast<double>(rate), hostRate);
	if (flac) {
		drflac_free(data, nullptr);
	} else {
		drwav_free(data, nullptr);
	}
	return ir;
}

} // namespace feathers::fx::conv
