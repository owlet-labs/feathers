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

// THE OFFLINE FIT BEHIND THE FITTED TERRAIN MAP (plugin/ui/terrain_fit.h). Developer tool: the build does not run it,
// the table it writes is committed.
//
//   gen    <additive|v5> <count> <seed> <hold_s> <range> <out.csv>
//            renders <count> stances through the real engine (one fixed note) and writes, per stance, the decoded
//            inputs, today's map features, and the measurements of the audio that stand in for them
//   fit    <table.cpp> <report.txt> <sample.inc> --additive <csv>... --v5 <csv>... [--calibrate-ratios]
//            fits each channel (quadratic or restricted cubic, ridge chosen on a validation split) and writes the
//            generated table, the held-out report and the evaluator's cross-check sample. Deterministic.
//   zooms  <out.txt>   per engine and zoom: how much the true map varies across the window against the fit's error
//   bench  <out.txt>   per-cell cost of today's map and the fitted one, and what resolution the fit could afford
//
// THE TARGETS. The map's five cell features are energy (RMS of one 32-sample cycle), and four ratios of the first
// twelve harmonic magnitudes: centroid, roughness (share above the sixth), parity (odd share), fundamental share.
// Their audio counterparts are measured from a held note of the real engine: the same four ratios taken over the
// twelve harmonic magnitudes of the rendered note (so they are on the map's scale by construction), and the RMS of the
// note, which is not on the energy channel's scale — so energy's target is the affine image of the (log) RMS that best
// regresses the map's own energy channel, recorded in the table.

#include "dsp/partial_spectrum.h"
#include "dsp/slice_basis.h"
#include "dsp/slice_build.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "plugin/ui/terrain.h"
#include "plugin/ui/terrain_fit.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace feathers;
using feathers::ui::kFitChannels;
using feathers::ui::kFitMaxInputs;
using feathers::ui::TerrainFeatures;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kKey = 48;
constexpr double kF0 = 130.8127826502993; // MIDI 48
constexpr int kHarmonics = 12;
constexpr int kMeasureMagnitudes = 1 + kHarmonics; // rms, then the twelve magnitudes
constexpr uint64_t kShuffleSeed = 0xFEA7EDull;
constexpr const char* kChannelNames[kFitChannels] = { "energy", "centroid", "roughness", "parity", "fundamental" };

//---------------------------------------------------------------- engines
struct EngineInfo {
	const char* name;
	OscType engine;
	int inputs;
};
const EngineInfo kEngines[] = { { "additive", OscType::Additive, ui::kFitAdditiveInputs },
	                            { "v5", OscType::V5, ui::kFitV5Inputs } };

const EngineInfo* engineByName(const std::string& name) {
	for (const EngineInfo& e : kEngines) {
		if (name == e.name) {
			return &e;
		}
	}
	return nullptr;
}

//---------------------------------------------------------------- FFT (the spike's)
void fft(std::vector<std::complex<double>>& a) {
	const size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(a[i], a[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const double ang = -2.0 * M_PI / static_cast<double>(len);
		const std::complex<double> wl(std::cos(ang), std::sin(ang));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (size_t j = 0; j < len / 2; ++j) {
				const auto u = a[i + j];
				const auto v = a[i + j + len / 2] * w;
				a[i + j] = u + v;
				a[i + j + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

std::vector<double> powerSpectrum(const float* x, size_t len, size_t nfft) {
	std::vector<std::complex<double>> a(nfft);
	for (size_t i = 0; i < len; ++i) {
		const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * (static_cast<double>(i) + 0.5) / static_cast<double>(len));
		a[i] = static_cast<double>(x[i]) * w;
	}
	fft(a);
	std::vector<double> p(nfft / 2 + 1);
	for (size_t k = 0; k < p.size(); ++k) {
		p[k] = std::norm(a[k]);
	}
	return p;
}

//---------------------------------------------------------------- the rig (the spike's, one engine at a time)
struct Rig {
	ParamSet params;
	Synth synth { params };
	std::vector<float> left = std::vector<float>(kBlock), right = std::vector<float>(kBlock);
	int32_t nextId = 1;
	explicit Rig(OscType engine) {
		synth.activate(kRate, 4096);
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(engine));
		params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);
		params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 1.0);
		params.setBase(static_cast<clap_id>(ParamId::Weather), 1.0);
		params.setBase(static_cast<clap_id>(soundParam(1, SoundField::Enable)), 0.0);
		for (int i = 0; i < 40; ++i) {
			synth.render(left.data(), right.data(), kBlock);
		}
	}
	void stance(int zone, double offset, double gamma, double rotate, double tx, double ty) {
		params.setBase(static_cast<clap_id>(ParamId::AZone), static_cast<double>(zone));
		params.setBase(static_cast<clap_id>(ParamId::AOffset), offset);
		params.setBase(static_cast<clap_id>(ParamId::AGamma), gamma);
		params.setBase(static_cast<clap_id>(ParamId::PosX), 0.0);
		params.setBase(static_cast<clap_id>(ParamId::PosY), 0.0);
		SoundTravel st;
		st.offset[0] = AnchorPoint { tx, ty }; // travel is the sound's drift (unbounded), how Explore stands anywhere
		synth.setSoundTravel(st);
		SliceOrientationPin pin;
		pin.pinned = true;
		pin.generator.rotate = static_cast<float>(rotate);
		synth.setOrientation(pin);
	}
	std::vector<float> play(double hold) {
		std::vector<float> held;
		for (int i = 0; i < 24; ++i) { // the rebuild fades of the new stance run while nothing sounds
			synth.render(left.data(), right.data(), kBlock);
		}
		const int id = nextId++;
		synth.noteOn(0, kKey, id, 100.0 / 127.0);
		const int blocks = static_cast<int>(std::ceil(hold * kRate / kBlock));
		for (int b = 0; b < blocks; ++b) {
			synth.render(left.data(), right.data(), kBlock);
			held.insert(held.end(), left.begin(), left.end());
		}
		synth.noteOff(0, kKey, id, 0.0);
		for (int b = 0; b < 8; ++b) {
			synth.render(left.data(), right.data(), kBlock);
		}
		synth.choke(0, kKey, id);
		for (int b = 0; b < 4; ++b) {
			synth.render(left.data(), right.data(), kBlock);
		}
		return held;
	}
};

//---------------------------------------------------------------- one stance: decode, today's map, the audio
struct Stance {
	int zone = 0;
	double offset = 0.0, gamma = 0.0, rotate = 0.0, tx = 0.0, ty = 0.0;
	float phaseOffset() const { return static_cast<float>(offset + gamma); } // the engine's own line
	SliceOrientationPin pin() const {
		SliceOrientationPin p;
		p.pinned = true;
		p.generator.rotate = static_cast<float>(rotate);
		return p;
	}
};

struct Row {
	Stance stance;
	std::array<double, kFitMaxInputs> in {};
	std::array<double, kFitChannels> map {};          ///< today's map features (the channels the fit replaces)
	std::array<double, kMeasureMagnitudes> meas {};   ///< rms, magnitudes 1..12 of the rendered note
};

void decodeAndMap(OscType engine, const Stance& s, Row& row) {
	const ui::TerrainFrames frames = ui::terrainFramesAt(s.pin(), sliceHome(static_cast<uint16_t>(s.zone), s.phaseOffset(), 0.0),
	                                                     ui::kNoPlaneResidue);
	ui::terrainFitDecode(engine, frames, static_cast<uint16_t>(s.zone), s.phaseOffset(), s.tx, s.ty, 0.0, {}, {}, nullptr,
	                     row.in.data());
	const TerrainFeatures f = ui::terrainFeaturesOn(engine, frames, static_cast<uint16_t>(s.zone), s.phaseOffset(), s.tx,
	                                                s.ty, 0.0);
	row.map = { f.energy, f.centroid, f.roughness, f.parity, f.fundamental };
}

/// The audio's counterparts: RMS of the held note and the magnitudes at harmonics 1..12 (square root of the band energy
/// within a quarter of the fundamental either side, so a harmonic's own sidebands count towards it).
bool measureNote(const std::vector<float>& held, double hold, std::array<double, kMeasureMagnitudes>& out) {
	const size_t start = static_cast<size_t>(0.15 * kRate);
	const size_t end = std::min(held.size(), static_cast<size_t>(hold * kRate));
	if (end <= start + 4096) {
		return false;
	}
	const size_t len = end - start;
	const float* x = held.data() + start;
	double ss = 0.0;
	for (size_t i = 0; i < len; ++i) {
		ss += static_cast<double>(x[i]) * static_cast<double>(x[i]);
	}
	const double rms = std::sqrt(ss / static_cast<double>(len));
	if (!(rms > 1e-7)) {
		return false;
	}
	size_t nfft = 1;
	while (nfft < len * 2) {
		nfft <<= 1;
	}
	const double binHz = kRate / static_cast<double>(nfft);
	const auto p = powerSpectrum(x, len, nfft);
	out[0] = rms;
	double total = 0.0;
	for (int k = 1; k <= kHarmonics; ++k) {
		const long lo = std::lround((k - 0.25) * kF0 / binHz);
		const long hi = std::lround((k + 0.25) * kF0 / binHz);
		double e = 0.0;
		for (long b = std::max(1L, lo); b <= std::min(static_cast<long>(p.size()) - 1, hi); ++b) {
			e += p[static_cast<size_t>(b)];
		}
		out[static_cast<size_t>(k)] = std::sqrt(e);
		total += out[static_cast<size_t>(k)];
	}
	return total > 0.0;
}

/// The map's own reduction, restated over the measured magnitudes: the same four ratios terrainFeaturesOfCycle takes
/// over its twelve harmonic magnitudes (centroid /11, share above the sixth, odd share, fundamental share).
std::array<double, 4> ratiosOf(const double* m) {
	double total = 0.0, weighted = 0.0, high = 0.0, odd = 0.0;
	for (int k = 1; k <= kHarmonics; ++k) {
		const double v = m[k - 1];
		total += v;
		weighted += v * (k - 1);
		if (k > kHarmonics / 2) {
			high += v;
		}
		if (k % 2 == 1) {
			odd += v;
		}
	}
	if (!(total > 0.0)) {
		return { 0.0, 0.0, 0.0, 0.0 };
	}
	return { weighted / total / static_cast<double>(kHarmonics - 1), high / total, odd / total, m[0] / total };
}

//---------------------------------------------------------------- CSV
constexpr int kStanceCols = 6;

void writeHeader(FILE* o, int inputs) {
	std::fprintf(o, "zone,offset,gamma,rotate,tx,ty");
	for (int i = 0; i < inputs; ++i) {
		std::fprintf(o, ",d%d", i);
	}
	for (const char* n : kChannelNames) {
		std::fprintf(o, ",map_%s", n);
	}
	std::fprintf(o, ",rms");
	for (int k = 1; k <= kHarmonics; ++k) {
		std::fprintf(o, ",m%d", k);
	}
	std::fprintf(o, "\n");
}

void writeRow(FILE* o, const Row& r, int inputs) {
	std::fprintf(o, "%d,%.17g,%.17g,%.17g,%.17g,%.17g", r.stance.zone, r.stance.offset, r.stance.gamma, r.stance.rotate,
	             r.stance.tx, r.stance.ty);
	for (int i = 0; i < inputs; ++i) {
		std::fprintf(o, ",%.17g", r.in[static_cast<size_t>(i)]);
	}
	for (double v : r.map) {
		std::fprintf(o, ",%.17g", v);
	}
	for (double v : r.meas) {
		std::fprintf(o, ",%.17g", v);
	}
	std::fprintf(o, "\n");
}

bool readRows(const std::string& path, int inputs, std::vector<Row>& rows) {
	FILE* f = std::fopen(path.c_str(), "r");
	if (f == nullptr) {
		std::fprintf(stderr, "cannot open %s\n", path.c_str());
		return false;
	}
	std::string line;
	int c;
	bool first = true;
	while (true) {
		line.clear();
		while ((c = std::fgetc(f)) != EOF && c != '\n') {
			line.push_back(static_cast<char>(c));
		}
		if (line.empty() && c == EOF) {
			break;
		}
		if (first) {
			first = false;
			continue;
		}
		std::vector<double> v;
		const char* p = line.c_str();
		char* end = nullptr;
		while (*p != '\0') {
			v.push_back(std::strtod(p, &end));
			p = end;
			if (*p == ',') {
				++p;
			}
		}
		const size_t want = static_cast<size_t>(kStanceCols + inputs + kFitChannels + kMeasureMagnitudes);
		if (v.size() != want) {
			std::fprintf(stderr, "%s: a row has %zu columns, wanted %zu\n", path.c_str(), v.size(), want);
			std::fclose(f);
			return false;
		}
		Row r;
		r.stance = { static_cast<int>(v[0]), v[1], v[2], v[3], v[4], v[5] };
		size_t at = kStanceCols;
		for (int i = 0; i < inputs; ++i) {
			r.in[static_cast<size_t>(i)] = v[at++];
		}
		for (double& m : r.map) {
			m = v[at++];
		}
		for (double& m : r.meas) {
			m = v[at++];
		}
		rows.push_back(r);
		if (c == EOF) {
			break;
		}
	}
	std::fclose(f);
	return true;
}

//---------------------------------------------------------------- gen
int runGen(int argc, char** argv) {
	if (argc < 8) {
		return 2;
	}
	const EngineInfo* e = engineByName(argv[2]);
	if (e == nullptr) {
		return 2;
	}
	const int count = std::atoi(argv[3]);
	const uint64_t seed = std::strtoull(argv[4], nullptr, 10);
	const double hold = std::atof(argv[5]);
	const double range = std::atof(argv[6]);
	FILE* o = std::fopen(argv[7], "w");
	if (o == nullptr) {
		return 2;
	}
	writeHeader(o, e->inputs);
	std::mt19937_64 rng(seed);
	std::uniform_real_distribution<double> u(0.0, 1.0);
	Rig rig(e->engine);
	int kept = 0;
	for (int i = 0; i < count; ++i) {
		Stance s;
		s.zone = static_cast<int>(u(rng) * 1024.0) % 1024;
		s.offset = u(rng) * 2.0;
		s.gamma = -4.0 + 8.0 * u(rng);
		s.rotate = 2.0 * u(rng);
		s.tx = range * (-1.0 + 2.0 * u(rng));
		s.ty = range * (-1.0 + 2.0 * u(rng));
		rig.stance(s.zone, s.offset, s.gamma, s.rotate, s.tx, s.ty);
		const auto held = rig.play(hold);
		Row row;
		row.stance = s;
		if (measureNote(held, hold, row.meas)) {
			decodeAndMap(e->engine, s, row);
			writeRow(o, row, e->inputs);
			++kept;
		}
		if ((i + 1) % 500 == 0) {
			std::fprintf(stderr, "%s range %g: %d/%d (kept %d)\n", e->name, range, i + 1, count, kept);
		}
	}
	std::fclose(o);
	std::fprintf(stderr, "%s range %g: %d stances, %d kept (the rest sounded silent)\n", e->name, range, count, kept);
	return 0;
}

//---------------------------------------------------------------- statistics
std::vector<double> ranks(const std::vector<double>& a) {
	std::vector<size_t> order(a.size());
	std::iota(order.begin(), order.end(), size_t { 0 });
	std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return a[x] < a[y]; });
	std::vector<double> r(a.size());
	for (size_t i = 0; i < order.size();) {
		size_t j = i;
		while (j + 1 < order.size() && a[order[j + 1]] == a[order[i]]) {
			++j;
		}
		const double mid = 0.5 * static_cast<double>(i + j);
		for (size_t k = i; k <= j; ++k) {
			r[order[k]] = mid;
		}
		i = j + 1;
	}
	return r;
}

double pearson(const std::vector<double>& a, const std::vector<double>& b) {
	const double n = static_cast<double>(a.size());
	double ma = 0.0, mb = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		ma += a[i];
		mb += b[i];
	}
	ma /= n;
	mb /= n;
	double sab = 0.0, saa = 0.0, sbb = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		sab += (a[i] - ma) * (b[i] - mb);
		saa += (a[i] - ma) * (a[i] - ma);
		sbb += (b[i] - mb) * (b[i] - mb);
	}
	return (saa > 0.0 && sbb > 0.0) ? sab / std::sqrt(saa * sbb) : NAN;
}

double spearman(const std::vector<double>& a, const std::vector<double>& b) {
	return pearson(ranks(a), ranks(b));
}

double stddev(const std::vector<double>& a) {
	double m = 0.0;
	for (double v : a) {
		m += v;
	}
	m /= static_cast<double>(a.size());
	double s = 0.0;
	for (double v : a) {
		s += (v - m) * (v - m);
	}
	return std::sqrt(s / static_cast<double>(a.size()));
}

struct Metrics {
	double r2 = NAN, rmse = NAN, spearman = NAN;
};

Metrics metricsOf(const std::vector<double>& pred, const std::vector<double>& truth) {
	double mean = 0.0;
	for (double v : truth) {
		mean += v;
	}
	mean /= static_cast<double>(truth.size());
	double sst = 0.0, sse = 0.0;
	for (size_t i = 0; i < truth.size(); ++i) {
		sst += (truth[i] - mean) * (truth[i] - mean);
		sse += (truth[i] - pred[i]) * (truth[i] - pred[i]);
	}
	return { sst > 0.0 ? 1.0 - sse / sst : NAN, std::sqrt(sse / static_cast<double>(truth.size())), spearman(pred, truth) };
}

/// y ~ a + b x, ordinary least squares.
void affineFit(const std::vector<double>& x, const std::vector<double>& y, double& a, double& b) {
	const double n = static_cast<double>(x.size());
	double mx = 0.0, my = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		mx += x[i];
		my += y[i];
	}
	mx /= n;
	my /= n;
	double sxy = 0.0, sxx = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		sxy += (x[i] - mx) * (y[i] - my);
		sxx += (x[i] - mx) * (x[i] - mx);
	}
	b = sxx > 0.0 ? sxy / sxx : 0.0;
	a = my - b * mx;
}

//---------------------------------------------------------------- calibration: audio measurement -> the map's scale
struct Calibration {
	int energyLog = 0;                            ///< energy's source is log(rms) rather than rms
	std::array<bool, kFitChannels> affine {};     ///< target = a + b * measured, else the measured value itself
	std::array<double, kFitChannels> a {};
	std::array<double, kFitChannels> b {};
	std::array<double, kFitChannels> regA {}, regB {}; ///< the regression of the map's channel on its counterpart, applied or not
};

double energySource(const Calibration& c, double rms) {
	return c.energyLog != 0 ? std::log(std::max(rms, 1e-12)) : rms;
}

std::array<double, kFitChannels> targetsOf(const Calibration& c, const std::array<double, kMeasureMagnitudes>& meas) {
	const std::array<double, 4> r = ratiosOf(meas.data() + 1);
	const std::array<double, kFitChannels> measured = { energySource(c, meas[0]), r[0], r[1], r[2], r[3] };
	std::array<double, kFitChannels> out {};
	for (size_t ch = 0; ch < kFitChannels; ++ch) {
		out[ch] = c.affine[ch] ? c.a[ch] + c.b[ch] * measured[ch] : measured[ch];
	}
	return out;
}

//---------------------------------------------------------------- dense linear algebra
int termCount(int n, int kind) {
	return 1 + n + n * (n + 1) / 2 + (kind == ui::kFitCubic ? n : 0);
}

/// The design row: 1, x, x_i x_j (i <= j), x_i^3 — always the cubic layout; a quadratic model takes its prefix.
void designRow(const double* x, int n, double* out) {
	int c = 0;
	out[c++] = 1.0;
	for (int i = 0; i < n; ++i) {
		out[c++] = x[i];
	}
	for (int i = 0; i < n; ++i) {
		for (int j = i; j < n; ++j) {
			out[c++] = x[i] * x[j];
		}
	}
	for (int i = 0; i < n; ++i) {
		out[c++] = x[i] * x[i] * x[i];
	}
}

/// In-place Cholesky of the leading `m` x `m` block of `a` (leading dimension `ld`), lower triangle. False if not SPD.
bool cholesky(std::vector<double>& a, int ld, int m) {
	for (int j = 0; j < m; ++j) {
		double d = a[static_cast<size_t>(j) * ld + j];
		for (int k = 0; k < j; ++k) {
			d -= a[static_cast<size_t>(j) * ld + k] * a[static_cast<size_t>(j) * ld + k];
		}
		if (!(d > 0.0)) {
			return false;
		}
		const double dj = std::sqrt(d);
		a[static_cast<size_t>(j) * ld + j] = dj;
		for (int i = j + 1; i < m; ++i) {
			double s = a[static_cast<size_t>(i) * ld + j];
			for (int k = 0; k < j; ++k) {
				s -= a[static_cast<size_t>(i) * ld + k] * a[static_cast<size_t>(j) * ld + k];
			}
			a[static_cast<size_t>(i) * ld + j] = s / dj;
		}
	}
	return true;
}

void cholSolve(const std::vector<double>& l, int ld, int m, std::vector<double>& b) {
	for (int i = 0; i < m; ++i) {
		double s = b[static_cast<size_t>(i)];
		for (int k = 0; k < i; ++k) {
			s -= l[static_cast<size_t>(i) * ld + k] * b[static_cast<size_t>(k)];
		}
		b[static_cast<size_t>(i)] = s / l[static_cast<size_t>(i) * ld + i];
	}
	for (int i = m - 1; i >= 0; --i) {
		double s = b[static_cast<size_t>(i)];
		for (int k = i + 1; k < m; ++k) {
			s -= l[static_cast<size_t>(k) * ld + i] * b[static_cast<size_t>(k)];
		}
		b[static_cast<size_t>(i)] = s / l[static_cast<size_t>(i) * ld + i];
	}
}

//---------------------------------------------------------------- the fit
struct ChannelFit {
	int kind = 0;
	double lambda = 0.0;
	double lo = 0.0, hi = 0.0;
	std::vector<double> w;
	Metrics heldOut;
	Metrics map;           ///< today's map channel, as a prediction of the target, on the same held-out rows
	Metrics mapAffine;     ///< and after its best affine image
	double targetSd = 0.0;
};

struct EngineFit {
	const EngineInfo* info = nullptr;
	int rows = 0, pool = 0, test = 0;
	std::vector<double> mean, sd;
	Calibration cal;
	std::array<ChannelFit, kFitChannels> channel;
	std::array<double, kFitChannels> calR2 {}, calSpearman {}; ///< today's map channel vs its audio counterpart (pool)
	std::vector<size_t> testOrder;                              ///< the held-out rows, in shuffle order
	std::vector<Row> data;
};

constexpr double kLambdas[] = { 0.1, 1.0, 10.0, 100.0, 1000.0, 1e4, 1e5 };

double clampTo(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

double dot(const double* a, const double* b, int n) {
	double s = 0.0;
	for (int i = 0; i < n; ++i) {
		s += a[i] * b[i];
	}
	return s;
}

bool fitEngine(const EngineInfo& info, const std::vector<std::string>& files, bool calibrateRatios, EngineFit& fit) {
	fit.info = &info;
	for (const std::string& f : files) {
		if (!readRows(f, info.inputs, fit.data)) {
			return false;
		}
	}
	const size_t N = fit.data.size();
	fit.rows = static_cast<int>(N);
	if (N < 1000) {
		std::fprintf(stderr, "%s: only %zu rows\n", info.name, N);
		return false;
	}
	// A DETERMINISTIC SHUFFLE (Fisher-Yates over mt19937_64's own stream: the standard library's shuffle is not specified).
	std::vector<size_t> order(N);
	std::iota(order.begin(), order.end(), size_t { 0 });
	std::mt19937_64 rng(kShuffleSeed);
	for (size_t i = N - 1; i > 0; --i) {
		const size_t j = static_cast<size_t>(rng() % (i + 1));
		std::swap(order[i], order[j]);
	}
	const size_t nTest = N / 5;
	const std::vector<size_t> test(order.begin(), order.begin() + static_cast<long>(nTest));
	const std::vector<size_t> pool(order.begin() + static_cast<long>(nTest), order.end());
	fit.pool = static_cast<int>(pool.size());
	fit.test = static_cast<int>(nTest);
	fit.testOrder = test;

	// STANDARDISATION from the pool.
	const int n = info.inputs;
	fit.mean.assign(static_cast<size_t>(n), 0.0);
	fit.sd.assign(static_cast<size_t>(n), 0.0);
	for (size_t idx : pool) {
		for (int i = 0; i < n; ++i) {
			fit.mean[static_cast<size_t>(i)] += fit.data[idx].in[static_cast<size_t>(i)];
		}
	}
	for (double& m : fit.mean) {
		m /= static_cast<double>(pool.size());
	}
	for (size_t idx : pool) {
		for (int i = 0; i < n; ++i) {
			const double d = fit.data[idx].in[static_cast<size_t>(i)] - fit.mean[static_cast<size_t>(i)];
			fit.sd[static_cast<size_t>(i)] += d * d;
		}
	}
	for (double& s : fit.sd) {
		s = std::max(std::sqrt(s / static_cast<double>(pool.size())), 1e-9);
	}

	// CALIBRATION (pool): today's map channel regressed on its audio counterpart.
	Calibration& cal = fit.cal;
	{
		std::vector<double> rms, logRms, mapE;
		for (size_t idx : pool) {
			rms.push_back(fit.data[idx].meas[0]);
			logRms.push_back(std::log(std::max(fit.data[idx].meas[0], 1e-12)));
			mapE.push_back(fit.data[idx].map[0]);
		}
		const double rLin = pearson(rms, mapE), rLog = pearson(logRms, mapE);
		cal.energyLog = (rLog * rLog > rLin * rLin) ? 1 : 0;
		cal.affine[0] = true;
		affineFit(cal.energyLog != 0 ? logRms : rms, mapE, cal.a[0], cal.b[0]);
		for (size_t ch = 0; ch < kFitChannels; ++ch) {
			std::vector<double> measured, mapv;
			for (size_t idx : pool) {
				const Row& r = fit.data[idx];
				const std::array<double, 4> q = ratiosOf(r.meas.data() + 1);
				const double m = ch == 0 ? energySource(cal, r.meas[0]) : q[ch - 1];
				measured.push_back(m);
				mapv.push_back(r.map[ch]);
			}
			affineFit(measured, mapv, cal.regA[ch], cal.regB[ch]);
			if (ch > 0) {
				cal.affine[ch] = calibrateRatios;
				cal.a[ch] = calibrateRatios ? cal.regA[ch] : 0.0;
				cal.b[ch] = calibrateRatios ? cal.regB[ch] : 1.0;
			}
			const double rr = pearson(measured, mapv);
			fit.calR2[ch] = rr * rr;
			fit.calSpearman[ch] = spearman(measured, mapv);
		}
	}

	// TARGETS and standardised inputs, for every row.
	std::vector<std::array<double, kFitChannels>> y(N);
	const int terms = termCount(n, ui::kFitCubic);
	std::vector<double> A(N * static_cast<size_t>(terms));
	for (size_t r = 0; r < N; ++r) {
		y[r] = targetsOf(cal, fit.data[r].meas);
		double x[kFitMaxInputs];
		for (int i = 0; i < n; ++i) {
			x[i] = (fit.data[r].in[static_cast<size_t>(i)] - fit.mean[static_cast<size_t>(i)]) / fit.sd[static_cast<size_t>(i)];
		}
		designRow(x, n, &A[r * static_cast<size_t>(terms)]);
	}

	// NORMAL EQUATIONS over the training part of the pool, over its validation part, and (their sum) over the pool.
	const size_t nVal = pool.size() / 5;
	const std::vector<size_t> val(pool.begin(), pool.begin() + static_cast<long>(nVal));
	const std::vector<size_t> train(pool.begin() + static_cast<long>(nVal), pool.end());
	const auto gram = [&](const std::vector<size_t>& set, std::vector<double>& M, std::vector<std::vector<double>>& b) {
		M.assign(static_cast<size_t>(terms) * terms, 0.0);
		b.assign(kFitChannels, std::vector<double>(static_cast<size_t>(terms), 0.0));
		for (size_t idx : set) {
			const double* a = &A[idx * static_cast<size_t>(terms)];
			for (int i = 0; i < terms; ++i) {
				const double ai = a[i];
				double* row = &M[static_cast<size_t>(i) * terms];
				for (int j = i; j < terms; ++j) {
					row[j] += ai * a[j];
				}
				for (size_t ch = 0; ch < kFitChannels; ++ch) {
					b[ch][static_cast<size_t>(i)] += ai * y[idx][ch];
				}
			}
		}
	};
	std::vector<double> Mt, Mv;
	std::vector<std::vector<double>> bt, bv;
	gram(train, Mt, bt);
	gram(val, Mv, bv);
	std::vector<double> Mp = Mt;
	std::vector<std::vector<double>> bp = bt;
	for (size_t i = 0; i < Mp.size(); ++i) {
		Mp[i] += Mv[i];
	}
	for (size_t ch = 0; ch < kFitChannels; ++ch) {
		for (size_t i = 0; i < bp[ch].size(); ++i) {
			bp[ch][i] += bv[ch][i];
		}
	}
	const auto symmetrise = [&](std::vector<double>& M) { // the Gram matrices hold the upper triangle; Cholesky reads the lower
		for (int i = 0; i < terms; ++i) {
			for (int j = 0; j < i; ++j) {
				M[static_cast<size_t>(i) * terms + j] = M[static_cast<size_t>(j) * terms + i];
			}
		}
	};
	symmetrise(Mt);
	symmetrise(Mp);

	const auto solve = [&](const std::vector<double>& M, const std::vector<double>& b, int kind, double lambda,
	                       std::vector<double>& w) {
		const int m = termCount(n, kind);
		std::vector<double> a(static_cast<size_t>(m) * m);
		for (int i = 0; i < m; ++i) {
			for (int j = 0; j < m; ++j) {
				a[static_cast<size_t>(i) * m + j] = M[static_cast<size_t>(i) * terms + j];
			}
			if (i > 0) {
				a[static_cast<size_t>(i) * m + i] += lambda; // the intercept is not penalised
			}
		}
		if (!cholesky(a, m, m)) {
			return false;
		}
		w.assign(b.begin(), b.begin() + m);
		cholSolve(a, m, m, w);
		return true;
	};
	const auto predictRow = [&](const std::vector<double>& w, int kind, size_t idx, double lo, double hi) {
		return clampTo(dot(w.data(), &A[idx * static_cast<size_t>(terms)], termCount(n, kind)), lo, hi);
	};

	for (size_t ch = 0; ch < kFitChannels; ++ch) {
		ChannelFit& cf = fit.channel[ch];
		cf.lo = 1e300;
		cf.hi = -1e300;
		for (size_t idx : pool) {
			cf.lo = std::min(cf.lo, y[idx][ch]);
			cf.hi = std::max(cf.hi, y[idx][ch]);
		}
		double best = -1e300;
		for (const int kind : { ui::kFitQuadratic, ui::kFitCubic }) {
			for (const double lambda : kLambdas) {
				std::vector<double> w;
				if (!solve(Mt, bt[ch], kind, lambda, w)) {
					continue;
				}
				std::vector<double> pred, truth;
				for (size_t idx : val) {
					pred.push_back(predictRow(w, kind, idx, cf.lo, cf.hi));
					truth.push_back(y[idx][ch]);
				}
				const double r2 = metricsOf(pred, truth).r2;
				if (r2 > best) {
					best = r2;
					cf.kind = kind;
					cf.lambda = lambda;
				}
			}
		}
		if (!solve(Mp, bp[ch], cf.kind, cf.lambda, cf.w)) {
			std::fprintf(stderr, "%s %s: final solve failed\n", info.name, kChannelNames[ch]);
			return false;
		}
		std::vector<double> pred, truth, mapv;
		for (size_t idx : test) {
			pred.push_back(predictRow(cf.w, cf.kind, idx, cf.lo, cf.hi));
			truth.push_back(y[idx][ch]);
			mapv.push_back(fit.data[idx].map[ch]);
		}
		cf.heldOut = metricsOf(pred, truth);
		cf.targetSd = stddev(truth);
		cf.map = metricsOf(mapv, truth);
		double a = 0.0, b = 0.0;
		affineFit(mapv, truth, a, b);
		std::vector<double> mapAff;
		for (double v : mapv) {
			mapAff.push_back(a + b * v);
		}
		cf.mapAffine = metricsOf(mapAff, truth);
	}
	return true;
}

//---------------------------------------------------------------- output
void emitArray(FILE* o, const char* name, const std::vector<double>& v) {
	std::fprintf(o, "constexpr double %s[%zu] = {", name, v.size());
	for (size_t i = 0; i < v.size(); ++i) {
		std::fprintf(o, "%s%.17g", i % 4 == 0 ? "\n\t" : " ", v[i]);
		if (i + 1 < v.size()) {
			std::fprintf(o, ",");
		}
	}
	std::fprintf(o, "\n};\n\n");
}

void writeTable(FILE* o, const std::vector<EngineFit>& fits) {
	std::fprintf(o, "// GENERATED by tools/fit-terrain-map.cpp from rendered stances -- do not edit; regenerate with\n"
	                "// scripts/fit-terrain-map.sh. Format version %d (plugin/ui/terrain_fit.h). Layout of a channel's\n"
	                "// coef: 1, x_i, x_i*x_j (i <= j), then (cubic channels) x_i^3, over inputs standardised by mean and sd.\n"
	                "#include \"plugin/ui/terrain_fit.h\"\n\nnamespace feathers::ui {\n\nnamespace {\n\n",
	             ui::kFitFormatVersion);
	for (const EngineFit& f : fits) {
		const std::string tag = f.info->name;
		emitArray(o, (tag + "Mean").c_str(), f.mean);
		emitArray(o, (tag + "Sd").c_str(), f.sd);
		for (size_t ch = 0; ch < kFitChannels; ++ch) {
			emitArray(o, (tag + "Coef" + std::to_string(ch)).c_str(), f.channel[ch].w);
		}
		std::fprintf(o, "const FitEngineTable k%sTable = {\n\tkFitFormatVersion, %d, %d, %sMean, %sSd,\n\t{ {\n", f.info->name,
		             static_cast<int>(f.info->engine), f.info->inputs, f.info->name, f.info->name);
		for (size_t ch = 0; ch < kFitChannels; ++ch) {
			const ChannelFit& c = f.channel[ch];
			std::fprintf(o, "\t\t{ %d, %zu, %.17g, %.17g, %sCoef%zu }%s // %s\n", c.kind, c.w.size(), c.lo, c.hi,
			             f.info->name, ch, ch + 1 < kFitChannels ? "," : "", kChannelNames[ch]);
		}
		std::fprintf(o, "\t} },\n};\n\n");
	}
	std::fprintf(o, "} // namespace\n\nconst FitEngineTable* terrainFitTableFor(OscType engine) {\n\tswitch (engine) {\n");
	for (const EngineFit& f : fits) {
		std::fprintf(o, "\tcase static_cast<OscType>(%d): return &k%sTable;\n", static_cast<int>(f.info->engine), f.info->name);
	}
	std::fprintf(o, "\tdefault: break;\n\t}\n\treturn nullptr;\n}\n\n} // namespace feathers::ui\n");
}

void writeReport(FILE* o, const std::vector<EngineFit>& fits, bool calibrateRatios) {
	std::fprintf(o, "FITTED TERRAIN MAP -- held-out report (generated by tools/fit-terrain-map.cpp; do not edit)\n\n");
	std::fprintf(o, "Each cell feature the map colours from is predicted from the engine's decoded parameters. Targets are\n"
	                "measured from the real engine's audio (one held note, MIDI 48): the four spectral ratios over the twelve\n"
	                "harmonic magnitudes of the rendered note (the map's own reduction, so on its scale), and energy as the\n"
	                "%s regression of the map's energy channel on the note's RMS. %s\n"
	                "Held-out = a shuffled 20%% of the stances never used to fit or to choose; R2/RMSE/Spearman are of the\n"
	                "prediction against the audio target. 'map' rows give today's map channel as the predictor of the same\n"
	                "target (raw, and after its best affine image), on the same rows.\n\n",
	             "affine", calibrateRatios ? "The ratio targets are also mapped affinely onto the map's channel." :
	                                         "The ratio targets are the measured values themselves (identity).");
	for (const EngineFit& f : fits) {
		std::fprintf(o, "== %s: %d stances (pool %d, held-out %d), %d inputs; energy source: %s(rms)\n", f.info->name, f.rows,
		             f.pool, f.test, f.info->inputs, f.cal.energyLog != 0 ? "log" : "linear");
		std::fprintf(o, "%-12s %-9s %7s %6s %6s %7s %7s %8s | %-26s | %-26s\n", "channel", "model", "lambda", "ncoef", "tgtSD",
		             "R2", "RMSE", "Spearman", "map raw R2/RMSE/Sp", "map affine R2/RMSE/Sp");
		for (size_t ch = 0; ch < kFitChannels; ++ch) {
			const ChannelFit& c = f.channel[ch];
			std::fprintf(o, "%-12s %-9s %7g %6zu %6.3f %7.3f %7.4f %8.3f | %7.3f %7.4f %8.3f      | %7.3f %7.4f %8.3f\n",
			             kChannelNames[ch], c.kind == ui::kFitCubic ? "rcubic" : "quad", c.lambda, c.w.size(), c.targetSd,
			             c.heldOut.r2, c.heldOut.rmse, c.heldOut.spearman, c.map.r2, c.map.rmse, c.map.spearman,
			             c.mapAffine.r2, c.mapAffine.rmse, c.mapAffine.spearman);
		}
		std::fprintf(o, "today's map channel against its audio counterpart (pool): R2 of the straight line / Spearman\n");
		for (size_t ch = 0; ch < kFitChannels; ++ch) {
			std::fprintf(o, "  %-12s R2 %.3f  Spearman %.3f  map = %.6g + %.6g * measured (%s)\n", kChannelNames[ch], f.calR2[ch],
			             f.calSpearman[ch], f.cal.regA[ch], f.cal.regB[ch],
			             f.cal.affine[ch] ? "applied to the target" : "not applied: the target is the measured value");
		}
		std::fprintf(o, "\n");
	}
}

void writeSample(FILE* o, const std::vector<EngineFit>& fits) {
	std::fprintf(o, "// GENERATED by tools/fit-terrain-map.cpp -- do not edit. The offline fit's own predictions (a dense dot of its\n"
	                "// design row with its coefficients, clipped to the training range) for held-out stances, with the decoded\n"
	                "// inputs those stances give, for the C++ evaluator's cross-check.\n"
	                "struct FitSampleRow {\n\tint zone;\n\tdouble offset, gamma, rotate, tx, ty;\n\tdouble in[40];\n\tdouble predicted[5];\n};\n\n");
	for (const EngineFit& f : fits) {
		const int n = f.info->inputs;
		const size_t count = std::min<size_t>(200, f.testOrder.size());
		std::fprintf(o, "inline constexpr FitSampleRow kFitSample_%s[%zu] = {\n", f.info->name, count);
		for (size_t t = 0; t < count; ++t) {
			const Row& r = f.data[f.testOrder[t]];
			double x[kFitMaxInputs];
			for (int i = 0; i < n; ++i) {
				x[i] = (r.in[static_cast<size_t>(i)] - f.mean[static_cast<size_t>(i)]) / f.sd[static_cast<size_t>(i)];
			}
			std::vector<double> design(static_cast<size_t>(termCount(n, ui::kFitCubic)));
			designRow(x, n, design.data());
			std::fprintf(o, "\t{ %d, %.17g, %.17g, %.17g, %.17g, %.17g, { ", r.stance.zone, r.stance.offset, r.stance.gamma,
			             r.stance.rotate, r.stance.tx, r.stance.ty);
			for (int i = 0; i < kFitMaxInputs; ++i) {
				std::fprintf(o, "%.17g%s", i < n ? r.in[static_cast<size_t>(i)] : 0.0, i + 1 < kFitMaxInputs ? ", " : "");
			}
			std::fprintf(o, " }, { ");
			for (size_t ch = 0; ch < kFitChannels; ++ch) {
				const ChannelFit& c = f.channel[ch];
				const double v = clampTo(dot(c.w.data(), design.data(), termCount(n, c.kind)), c.lo, c.hi);
				std::fprintf(o, "%.17g%s", v, ch + 1 < kFitChannels ? ", " : "");
			}
			std::fprintf(o, " } },\n");
		}
		std::fprintf(o, "};\n\n");
	}
}

int runFit(int argc, char** argv) {
	if (argc < 5) {
		return 2;
	}
	std::vector<std::string> files[2];
	int target = -1;
	bool calibrateRatios = false;
	for (int i = 5; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--additive") {
			target = 0;
		}
		else if (a == "--v5") {
			target = 1;
		}
		else if (a == "--calibrate-ratios") {
			calibrateRatios = true;
		}
		else if (target >= 0) {
			files[target].push_back(a);
		}
	}
	std::vector<EngineFit> fits;
	for (int e = 0; e < 2; ++e) {
		if (files[e].empty()) {
			continue;
		}
		EngineFit f;
		if (!fitEngine(kEngines[e], files[e], calibrateRatios, f)) {
			return 1;
		}
		fits.push_back(std::move(f));
	}
	FILE* t = std::fopen(argv[2], "w");
	FILE* r = std::fopen(argv[3], "w");
	FILE* s = std::fopen(argv[4], "w");
	if (t == nullptr || r == nullptr || s == nullptr) {
		return 2;
	}
	writeTable(t, fits);
	writeReport(r, fits, calibrateRatios);
	writeSample(s, fits);
	std::fclose(t);
	std::fclose(r);
	std::fclose(s);
	writeReport(stdout, fits, calibrateRatios);
	return 0;
}

//---------------------------------------------------------------- zooms: where the fit can and cannot resolve a view
struct Home {
	const char* name;
	int zone;
	double offset, gamma, rotate;
};
const Home kHomes[] = { { "A", 311, 0.7, 1.3, 0.8 }, { "B", 800, 1.4, -2.1, 1.3 } };
struct Zoom {
	const char* name;
	double zoom;
};
const Zoom kZooms[] = { { "x1/100", 0.01 }, { "x1/32", 1.0 / 32.0 }, { "x1/8", 0.125 }, { "x1", 1.0 }, { "x7", 7.0 } };

/// Half the travel width of a view at `zoom`: the stage's own span (kBaseTravel / zoom) halved.
double halfSpanAt(double zoom) {
	return 0.5 * (64.0 / 1023.0) / zoom;
}

int runZooms(int argc, char** argv) {
	if (argc < 4) {
		return 2;
	}
	std::vector<std::string> files[2];
	int target = -1;
	for (int i = 3; i < argc; ++i) {
		const std::string a = argv[i];
		if (a == "--additive") {
			target = 0;
		}
		else if (a == "--v5") {
			target = 1;
		}
		else if (target >= 0) {
			files[target].push_back(a);
		}
	}
	FILE* o = std::fopen(argv[2], "w");
	if (o == nullptr) {
		return 2;
	}
	constexpr int kGrid = 20;
	std::fprintf(o, "FITTED TERRAIN MAP -- where the fit can and cannot resolve a view (generated by tools/fit-terrain-map.cpp zooms)\n\n"
	                "Per engine, home (A: zone 311 offset 0.7 gamma 1.3 rotate 0.8; B: zone 800 offset 1.4 gamma -2.1 rotate 1.3) and\n"
	                "zoom, a %dx%d grid across the view centred on travel (0,0) is RENDERED through the real engine (the truth), and\n"
	                "compared with the fit and with today's map channel. 'sd truth' is how much the truth varies across the window;\n"
	                "'held-out RMSE' is the fit's error on held-out stances over the whole space. UNRESOLVED means sd truth <\n"
	                "held-out RMSE: the variation the view shows is smaller than the error the fit makes, so the picture the fit gives\n"
	                "there is its error, not the sound. R2w / Sp are the fit against the truth WITHIN the window.\n\n",
	             kGrid, kGrid);
	for (int e = 0; e < 2; ++e) {
		if (files[e].empty()) {
			continue;
		}
		EngineFit fit;
		if (!fitEngine(kEngines[e], files[e], false, fit)) {
			return 1;
		}
		const ui::FitEngineTable* table = ui::terrainFitTableFor(kEngines[e].engine);
		if (table == nullptr) {
			std::fprintf(stderr, "no table linked for %s\n", kEngines[e].name);
			return 1;
		}
		std::fprintf(o, "== %s\n", kEngines[e].name);
		std::fprintf(o, "%-4s %-7s %-11s %9s %9s %9s %9s %8s %7s %7s  %s\n", "home", "zoom", "channel", "sd truth", "sd fit",
		             "sd map", "heldRMSE", "RMSEwin", "R2w", "Sp", "verdict");
		Rig rig(kEngines[e].engine);
		for (const Home& h : kHomes) {
			for (const Zoom& z : kZooms) {
				const double half = halfSpanAt(z.zoom);
				std::array<std::vector<double>, kFitChannels> truth, fitted, mapped;
				for (int j = 0; j < kGrid; ++j) {
					for (int i = 0; i < kGrid; ++i) {
						Stance s { h.zone, h.offset, h.gamma, h.rotate, -half + 2.0 * half * (i + 0.5) / kGrid,
							       -half + 2.0 * half * (j + 0.5) / kGrid };
						rig.stance(s.zone, s.offset, s.gamma, s.rotate, s.tx, s.ty);
						const auto held = rig.play(0.5);
						Row row;
						row.stance = s;
						if (!measureNote(held, 0.5, row.meas)) {
							continue;
						}
						decodeAndMap(kEngines[e].engine, s, row);
						const auto t = targetsOf(fit.cal, row.meas);
						const auto p = ui::terrainFitPredict(*table, row.in.data());
						for (size_t ch = 0; ch < kFitChannels; ++ch) {
							truth[ch].push_back(t[ch]);
							fitted[ch].push_back(p[ch]);
							mapped[ch].push_back(row.map[ch]);
						}
					}
				}
				for (size_t ch = 0; ch < kFitChannels; ++ch) {
					if (truth[ch].size() < 20) {
						continue;
					}
					const Metrics m = metricsOf(fitted[ch], truth[ch]);
					const double sdTruth = stddev(truth[ch]);
					const double held = fit.channel[ch].heldOut.rmse;
					std::fprintf(o, "%-4s %-7s %-11s %9.5f %9.5f %9.5f %9.5f %8.5f %7.3f %7.3f  %s\n", h.name, z.name,
					             kChannelNames[ch], sdTruth, stddev(fitted[ch]), stddev(mapped[ch]), held, m.rmse, m.r2, m.spearman,
					             sdTruth < held ? "UNRESOLVED" : "resolved");
				}
			}
		}
		std::fprintf(o, "\n");
		std::fflush(o);
	}
	std::fclose(o);
	return 0;
}

//---------------------------------------------------------------- bench: the cost of a cell
double nowNs() {
	return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int runBench(int argc, char** argv) {
	if (argc < 3) {
		return 2;
	}
	FILE* o = std::fopen(argv[2], "w");
	if (o == nullptr) {
		return 2;
	}
	const auto load = [] {
		double a = 0.0;
		if (FILE* f = std::fopen("/proc/loadavg", "r")) {
			if (std::fscanf(f, "%lf", &a) != 1) {
				a = -1.0;
			}
			std::fclose(f);
		}
		return a;
	};
	std::fprintf(o, "FITTED TERRAIN MAP -- per-cell cost (generated by tools/fit-terrain-map.cpp bench)\n"
	                "Cells of a 64x64 grid across the view, today's measurement (terrainFeaturesOn) against the fitted road\n"
	                "(terrainFeaturesFitted), alternated 5 times each, minimum of the five reported, one thread, plane frames\n"
	                "hoisted as the builder hoists them. 'decode' and 'predict' are the fitted road's two halves.\n"
	                "'affordable' = the square side the fitted road could build in the time today's 128x128 map takes.\n\n");
	std::fprintf(o, "%-9s %-4s %-7s %10s %10s %10s %10s %8s %11s %9s\n", "engine", "home", "zoom", "map ns", "fitted ns",
	             "decode ns", "predict ns", "speedup", "affordable", "load1");
	constexpr int kGrid = 64;
	for (const EngineInfo& e : kEngines) {
		const ui::FitEngineTable* table = ui::terrainFitTableFor(e.engine);
		if (table == nullptr) {
			continue;
		}
		for (const Home& h : kHomes) {
			Stance base { h.zone, h.offset, h.gamma, h.rotate, 0.0, 0.0 };
			const ui::TerrainFrames frames = ui::terrainFramesAt(base.pin(), sliceHome(static_cast<uint16_t>(h.zone), base.phaseOffset(), 0.0),
			                                                     ui::kNoPlaneResidue);
			for (const double zoom : { 0.01, 1.0, 7.0 }) {
				const double half = halfSpanAt(zoom);
				const auto at = [&](int c, double& tx, double& ty) {
					tx = -half + 2.0 * half * ((c % kGrid) + 0.5) / kGrid;
					ty = -half + 2.0 * half * ((c / kGrid) + 0.5) / kGrid;
				};
				volatile double sink = 0.0;
				double bestMap = 1e300, bestFit = 1e300, bestDecode = 1e300, bestPredict = 1e300;
				const int cells = kGrid * kGrid;
				std::vector<std::array<double, kFitMaxInputs>> decoded(static_cast<size_t>(cells));
				const double l0 = load();
				for (int rep = 0; rep < 5; ++rep) {
					double t = nowNs();
					for (int c = 0; c < cells; ++c) {
						double tx, ty;
						at(c, tx, ty);
						sink = sink + ui::terrainFeaturesOn(e.engine, frames, static_cast<uint16_t>(h.zone), base.phaseOffset(), tx, ty, 0.0).centroid;
					}
					bestMap = std::min(bestMap, (nowNs() - t) / cells);
					t = nowNs();
					for (int c = 0; c < cells; ++c) {
						double tx, ty;
						at(c, tx, ty);
						sink = sink + ui::terrainFeaturesFitted(e.engine, frames, static_cast<uint16_t>(h.zone), base.phaseOffset(), tx, ty, 0.0, {}, {}, nullptr).centroid;
					}
					bestFit = std::min(bestFit, (nowNs() - t) / cells);
					t = nowNs();
					for (int c = 0; c < cells; ++c) {
						double tx, ty;
						at(c, tx, ty);
						ui::terrainFitDecode(e.engine, frames, static_cast<uint16_t>(h.zone), base.phaseOffset(), tx, ty, 0.0, {}, {}, nullptr,
						                     decoded[static_cast<size_t>(c)].data());
					}
					bestDecode = std::min(bestDecode, (nowNs() - t) / cells);
					t = nowNs();
					for (int c = 0; c < cells; ++c) {
						sink = sink + ui::terrainFitPredict(*table, decoded[static_cast<size_t>(c)].data())[1];
					}
					bestPredict = std::min(bestPredict, (nowNs() - t) / cells);
				}
				const char* zn = zoom == 0.01 ? "x1/100" : zoom == 1.0 ? "x1" : "x7";
				std::fprintf(o, "%-9s %-4s %-7s %10.0f %10.0f %10.0f %10.0f %7.1fx %8.0f^2 %4.1f-%4.1f\n", e.name, h.name, zn,
				             bestMap, bestFit, bestDecode, bestPredict, bestMap / bestFit, 128.0 * std::sqrt(bestMap / bestFit),
				             l0, load());
			}
		}
	}
	std::fclose(o);
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc >= 2) {
		const std::string mode = argv[1];
		if (mode == "gen") {
			return runGen(argc, argv);
		}
		if (mode == "fit") {
			return runFit(argc, argv);
		}
		if (mode == "zooms") {
			return runZooms(argc, argv);
		}
		if (mode == "bench") {
			return runBench(argc, argv);
		}
	}
	std::fprintf(stderr, "usage: fit-terrain-map gen|fit|zooms|bench ...\n");
	return 2;
}
