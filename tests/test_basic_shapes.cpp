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

// THE BASIC SHAPES ENGINE, asserted by VALUE.
//
// Every reference here is an independent restatement: the shapes' Fourier series written out again in this
// file, the mip rule restated from its definition, Bessel functions by their own series. Where a render is
// compared with another render, it is the second assertion, there to prove the first is not vacuous.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/basic_shapes.h"
#include "dsp/halfband.h"
#include "plugin/character_params.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "spectrum.h"
#include "voice_harness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using feathers::Character;
using feathers::evalShapes;
using feathers::kShapesMipCount;
using feathers::kShapesTableSize;
using feathers::OscEngine;
using feathers::OscType;
using feathers::ParamId;
using feathers::ParamSet;
using feathers::PreparedCharacter;
using feathers::Shape;
using feathers::ShapesMix;
using feathers::ShapesWeights;
using feathers::Synth;
using feathers::test::BankedVoice;
using feathers::test::dB;
using feathers::test::Window;

constexpr double kSampleRate = 48000.0;
constexpr double kPi = std::numbers::pi;

// ---- the restatement --------------------------------------------------------------------------------------

/// Harmonic k's sine-series amplitude, written again from the textbook series rather than read from the
/// engine: saw (2/π)/k, square (4/π)/k on odd k, triangle (8/π²)(−1)^((k−1)/2)/k² on odd k.
double seriesAmplitude(Shape shape, int k) {
	const double kd = static_cast<double>(k);
	switch (shape) {
	case Shape::Saw: return 2.0 / (kPi * kd);
	case Shape::Square: return k % 2 == 1 ? 4.0 / (kPi * kd) : 0.0;
	case Shape::Triangle: return k % 2 == 1 ? ((k / 2) % 2 == 0 ? 1.0 : -1.0) * 8.0 / (kPi * kPi * kd * kd) : 0.0;
	default: return k == 1 ? 1.0 : 0.0;
	}
}

int topHarmonicOf(int mip) {
	return 1024 >> mip;
}

/// The band-limited shape at `phase`, summed directly.
double restated(Shape shape, int mip, double phase) {
	const int top = shape == Shape::Sine ? 1 : topHarmonicOf(mip);
	double sum = 0.0;
	for (int k = 1; k <= top; ++k) {
		sum += seriesAmplitude(shape, k) * std::sin(2.0 * kPi * static_cast<double>(k) * phase);
	}
	return sum;
}

/// The mip rule, restated: the richest ladder whose top partial at `topHz` stays at or under `limitHz`.
int restatedMip(double topHz, double limitHz) {
	for (int m = 0; m < kShapesMipCount; ++m) {
		if (static_cast<double>(topHarmonicOf(m)) * topHz <= limitHz) {
			return m;
		}
	}
	return kShapesMipCount - 1;
}

/// J_n(x) by its power series — the house test does not lean on std::cyl_bessel_j, which libc++ lacks.
double besselJ(int n, double x) {
	double term = std::pow(x / 2.0, n) / std::tgamma(static_cast<double>(n) + 1.0);
	double sum = term;
	for (int m = 1; m < 60; ++m) {
		term *= -(x * x / 4.0) / (static_cast<double>(m) * static_cast<double>(m + n));
		sum += term;
	}
	return sum;
}

Character shapesCharacter(const ShapesMix& mix) {
	Character c = feathers::test::characterAt(OscEngine::Shapes, 17);
	c.shapes = mix;
	return c;
}

/// `samples` of a steady note: a lead-in is rendered and dropped so the voice's start is not measured.
std::vector<float> renderVoice(const Character& c, double hz, int samples) {
	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(hz);
	v.setCharacter(c);
	std::vector<float> lead(4096);
	v.render(lead.data(), static_cast<int>(lead.size()));
	std::vector<float> out(static_cast<size_t>(samples));
	for (int i = 0; i < samples; i += 256) {
		v.render(out.data() + i, std::min(256, samples - i));
	}
	return out;
}

// ---- a synth on the real parameter road, for the honesty sweep --------------------------------------------

struct Rig {
	ParamSet params;
	Synth synth {params};

	explicit Rig(OscType engine) {
		synth.activate(kSampleRate, 4096);
		set(ParamId::OscType, static_cast<double>(engine));
		set(ParamId::Level, 1.0);
		// A bare Synth has no bridge to route the strip envelope into Voice Level (test_synth.cpp Rig says why). Without
		// this every render here was SILENT, and the honesty sweep compared silence with silence.
		set(ParamId::VoiceLevel, 1.0);
	}
	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }

	/// Renders `frames` with one note held from the start, after the change, and returns the left channel.
	std::vector<float> play(uint32_t frames) {
		synth.noteOn(0, 60, 1, 1.0);
		std::vector<float> l(frames);
		std::vector<float> r(frames);
		for (uint32_t at = 0; at < frames; at += 256) {
			const int n = static_cast<int>(std::min<uint32_t>(256, frames - at));
			synth.render(l.data() + at, r.data() + at, n);
		}
		return l;
	}
	const PreparedCharacter& table() const { return synth.mainBankForTest().current(); }
};

/// EVERYTHING A SHAPES VOICE READS FROM ITS TABLE, as one comparable value — the honesty sweep's measure.
struct Played {
	OscEngine engine;
	ShapesWeights shapes;
	float fmDepth, fmRatio, fmWidth, fmIndexAtC4, ampNoise, phaseJitter, warpDepth;
	int warpMode;
	bool stereo;
	friend bool operator==(const Played&, const Played&) = default;
};

Played playedBy(const Rig& rig) {
	const PreparedCharacter& t = rig.table();
	return { t.engine,
		     t.shapes,
		     t.fmDepth(),
		     t.fmFrequencyRatio(),
		     t.fmStereoWidth(),
		     t.fmIndexCyclesFor(261.6),
		     t.ampNoise(),
		     t.phaseJitter(),
		     t.recipe.warpDepth,
		     static_cast<int>(t.recipe.warpMode),
		     rig.synth.mainBankForTest().stereoActive() };
}

} // namespace

TEST_CASE("basic shapes: every mip of every ladder is its Fourier series, and nothing above its top partial") {
	feathers::warmShapeTables();
	double worstIn = 0.0;
	double worstAbove = 0.0;
	for (int mip = 0; mip < kShapesMipCount; ++mip) {
		// Exact sines and cosines at this mip's own length, so the analysis adds no error of its own.
		const int len = feathers::shapesMipLength(mip);
		REQUIRE(len >= kShapesTableSize);
		REQUIRE(len >= 32 * topHarmonicOf(mip));
		std::vector<double> s(static_cast<size_t>(len));
		std::vector<double> c(static_cast<size_t>(len));
		for (int n = 0; n < len; ++n) {
			s[static_cast<size_t>(n)] = std::sin(2.0 * kPi * n / len);
			c[static_cast<size_t>(n)] = std::cos(2.0 * kPi * n / len);
		}
		for (const Shape shape : { Shape::Triangle, Shape::Saw, Shape::Square }) {
			const float* row = feathers::shapeTable(shape, mip);
			REQUIRE(row[len] == row[0]); // the guard
			// Every partial the mip holds, and as many again above it, which is where a wrong top would show.
			for (int k = 1; k <= std::min(len / 2 - 1, 2 * topHarmonicOf(mip) + 8); ++k) {
				double b = 0.0;
				double a = 0.0;
				for (int n = 0; n < len; ++n) {
					const size_t at = static_cast<size_t>((static_cast<long>(k) * n) % len);
					b += static_cast<double>(row[n]) * s[at];
					a += static_cast<double>(row[n]) * c[at];
				}
				b *= 2.0 / len;
				a *= 2.0 / len;
				if (k <= topHarmonicOf(mip)) {
					worstIn = std::max(worstIn, std::abs(b - seriesAmplitude(shape, k)) + std::abs(a));
				}
				else {
					worstAbove = std::max(worstAbove, std::hypot(a, b));
				}
			}
		}
	}
	INFO("worst partial error " << worstIn << ", worst partial above the top " << worstAbove);
	// float storage of a ±1.09 signal: one part in ~1e7 per sample, ~1e-7 after the sum.
	CHECK(worstIn < 1e-6);
	CHECK(worstAbove < 1e-6);

	// The sine is one table at every mip, and it is a sine.
	for (int n = 0; n < kShapesTableSize; n += 7) {
		CHECK(std::abs(feathers::shapeTable(Shape::Sine, 5)[n] - std::sin(2.0 * kPi * n / kShapesTableSize)) < 1e-7);
	}
}

TEST_CASE("basic shapes: the mix is normalised by its sum, with a sine bias that makes all-zero a sine") {
	feathers::warmShapeTables(); // no bank here, and evalShapes reads the storage unguarded
	// Decision B: out = Σ wᵢ·shapeᵢ / Σ wᵢ, epsilon on sine.
	const ShapesWeights def = feathers::normaliseShapes(ShapesMix {});
	CHECK(def.sine == doctest::Approx(1.0).epsilon(1e-9));
	CHECK(def.saw == 0.0f);

	const ShapesWeights zero = feathers::normaliseShapes(ShapesMix { 0.0f, 0.0f, 0.0f, 0.0f });
	CHECK(zero.sine == 1.0f);
	CHECK(zero.triangle == 0.0f);
	CHECK(zero.saw == 0.0f);
	CHECK(zero.square == 0.0f);

	const ShapesWeights two = feathers::normaliseShapes(ShapesMix { 0.0f, 0.0f, 0.6f, 0.2f });
	const double sum = 0.8 + feathers::kShapesSineEpsilon;
	CHECK(std::abs(two.saw - 0.6 / sum) < 1e-6);
	CHECK(std::abs(two.square - 0.2 / sum) < 1e-6);
	CHECK(std::abs(two.sine - feathers::kShapesSineEpsilon / sum) < 1e-6);
	// One shape alone at 1 % of its travel is 99 % that shape: the bias is small enough not to be heard.
	CHECK(feathers::normaliseShapes(ShapesMix { 0.0f, 0.0f, 0.01f, 0.0f }).saw > 0.99f);

	// The decode against the restatement: on the table's own grid at the brightest mip (exact up to float),
	// and between grid points at mip 6, where linear interpolation of 16 partials is the only error.
	const ShapesMix mix { 0.3f, 0.5f, 0.9f, 0.4f };
	const ShapesWeights w = feathers::normaliseShapes(mix);
	const auto reference = [&](int mip, double phase) {
		return w.sine * restated(Shape::Sine, mip, phase) + w.triangle * restated(Shape::Triangle, mip, phase)
		       + w.saw * restated(Shape::Saw, mip, phase) + w.square * restated(Shape::Square, mip, phase);
	};
	double onGrid = 0.0;
	for (int n = 0; n < kShapesTableSize; n += 37) {
		const double phase = static_cast<double>(n) / kShapesTableSize;
		onGrid = std::max(onGrid, std::abs(evalShapes(w, 0, phase) - reference(0, phase)));
	}
	double between = 0.0;
	for (int i = 0; i < 101; ++i) {
		const double phase = (i + 0.37) / 101.0;
		between = std::max(between, std::abs(evalShapes(w, 6, phase) - reference(6, phase)));
	}
	INFO("on grid " << onGrid << ", between grid points " << between);
	CHECK(onGrid < 2e-6);
	CHECK(between < 2e-4);
}

TEST_CASE("basic shapes: the voice reads the mip its note's effective top frequency picks") {
	// Without FM the effective fundamental is the note: 1024 partials fit under 48 kHz up to 46.9 Hz.
	for (const double hz : { 30.0, 46.875, 47.0, 220.0, 750.0, 751.0, 3000.0, 12000.0, 30000.0 }) {
		BankedVoice v;
		v.setSampleRate(kSampleRate);
		v.setFrequency(hz);
		v.setCharacter(shapesCharacter(ShapesMix { 0.0f, 0.0f, 1.0f, 0.0f }));
		std::vector<float> out(64);
		v.render(out.data(), 64);
		INFO(hz << " Hz");
		CHECK(v.voice.shapesMipForTest() == restatedMip(hz, kSampleRate));
	}

	// With FM the index widens it: f·(1 + 2π·I·r), I and r the table's own (V5's accessors, asserted equal to
	// V5's in the FM case below). At 220 Hz full depth must push the voice onto a duller ladder.
	Character c = shapesCharacter(ShapesMix { 0.0f, 0.0f, 1.0f, 0.0f });
	c.sounds[0].fmDepth = 1.0f;
	c.sounds[1].fmDepth = 1.0f;
	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(220.0);
	v.setCharacter(c);
	std::vector<float> out(64);
	v.render(out.data(), 64);
	const PreparedCharacter& t = v.bank.current();
	const double index = t.fmIndexCyclesFor(220.0);
	REQUIRE(index > 0.5);
	const double top = 220.0 * (1.0 + 2.0 * kPi * index * t.fmFrequencyRatio());
	CHECK(v.voice.shapesMipForTest() == restatedMip(top, kSampleRate));
	CHECK(v.voice.shapesMipForTest() > restatedMip(220.0, kSampleRate));
}

TEST_CASE("basic shapes: a sine through the real voice is a sine") {
	// Bin-centred: 150 cycles in 16384 samples, so harmonic distortion lands on exact bins.
	constexpr int kN = 16384;
	const double hz = 150.0 * kSampleRate / kN;
	const std::vector<float> x = renderVoice(shapesCharacter(ShapesMix {}), hz, kN);
	const Window win(kN);
	const std::vector<double> spec = feathers::test::amplitudeSpectrum(x, win);
	const double fundamental = spec[150];
	double harmonics = 0.0;
	for (int k = 2; k * 150 < kN / 2; ++k) {
		harmonics += spec[static_cast<size_t>(k * 150)] * spec[static_cast<size_t>(k * 150)];
	}
	const double thd = dB(std::sqrt(harmonics) / fundamental);
	std::printf("[basic shapes] sine at %.1f Hz: fundamental %.4f, THD %.1f dB\n", hz, fundamental, thd);
	CHECK(fundamental == doctest::Approx(1.0).epsilon(0.002));
	CHECK(thd < -100.0);
}

TEST_CASE("basic shapes: a saw through the real voice has partials 2/(πk) and nothing else above the floor") {
	constexpr int kN = 16384;
	const Window win(kN);
	// 59 Hz, 440 Hz, 3.5 kHz and 10 kHz, each bin-centred.
	for (const int bin : { 20, 150, 1200, 3413 }) {
		const double hz = bin * kSampleRate / kN;
		const std::vector<float> x = renderVoice(shapesCharacter(ShapesMix { 0.0f, 0.0f, 1.0f, 0.0f }), hz, kN);
		const std::vector<double> spec = feathers::test::amplitudeSpectrum(x, win);
		double worstPartialDb = 0.0;
		for (int k = 1; static_cast<double>(k) * hz < 18000.0; ++k) {
			const double want = 2.0 / (kPi * k);
			worstPartialDb = std::max(worstPartialDb, std::abs(dB(spec[static_cast<size_t>(k * bin)] / want)));
		}
		// Every bin farther than the window's main lobe from a partial is not a partial, so it is an alias (or
		// noise); the worst of them, relative to the fundamental, below 20 kHz.
		double worstOther = 0.0;
		for (size_t b = 1; b < spec.size() && static_cast<double>(b) * kSampleRate / kN < 20000.0; ++b) {
			const int nearest = static_cast<int>(std::lround(static_cast<double>(b) / bin)) * bin;
			if (std::abs(static_cast<int>(b) - nearest) > 8) {
				worstOther = std::max(worstOther, spec[b]);
			}
		}
		const double floorDb = dB(worstOther / spec[static_cast<size_t>(bin)]);
		std::printf("[basic shapes] saw at %.1f Hz: worst partial %.3f dB off 2/(pi k); worst non-partial %.1f dB\n",
		            hz, worstPartialDb, floorDb);
		INFO(hz << " Hz");
		CHECK(worstPartialDb < 0.05);
		CHECK(floorDb < -110.0);
	}
}

TEST_CASE("basic shapes: a mip switch only moves partials the decimator removes") {
	// At 750 Hz the voice is on the boundary between the 64-partial ladder and the 32-partial one; the two
	// differ only by partials 33..64, 24.75..48 kHz. Through the real decimator the difference must leave
	// nothing below 20 kHz.
	feathers::warmShapeTables();
	constexpr int kN = 8192;
	const double hz = 750.0;
	const ShapesWeights saw = feathers::normaliseShapes(ShapesMix { 0.0f, 0.0f, 1.0f, 0.0f });
	const auto decimated = [&](int mip) {
		feathers::Halfband4x dec;
		std::vector<float> out(kN + 4096);
		std::array<float, 4 * feathers::Halfband4x::kMaxBlock> over {};
		double phase = 0.0;
		const double inc = hz / (4.0 * kSampleRate);
		for (size_t at = 0; at < out.size(); at += feathers::Halfband4x::kMaxBlock) {
			for (float& s : over) {
				s = evalShapes(saw, mip, phase);
				phase += inc;
				phase -= std::floor(phase);
			}
			dec.decimate(over.data(), out.data() + at, feathers::Halfband4x::kMaxBlock);
		}
		return std::vector<float>(out.begin() + 4096, out.end());
	};
	const int lower = restatedMip(hz, kSampleRate);
	REQUIRE(topHarmonicOf(lower) == 64);
	const std::vector<float> a = decimated(lower);
	const std::vector<float> b = decimated(lower + 1);
	std::vector<float> diff(kN);
	for (int i = 0; i < kN; ++i) {
		diff[static_cast<size_t>(i)] = a[static_cast<size_t>(i)] - b[static_cast<size_t>(i)];
	}
	const Window win(kN);
	const std::vector<double> spec = feathers::test::amplitudeSpectrum(diff, win);
	double worst = 0.0;
	for (size_t k = 1; static_cast<double>(k) * kSampleRate / kN < 20000.0; ++k) {
		worst = std::max(worst, spec[k]);
	}
	std::printf("[basic shapes] mip switch at 750 Hz: worst audible difference %.1f dBFS\n", dB(worst));
	CHECK(dB(worst) < -110.0);
}

TEST_CASE("basic shapes: FM is V5's — the same terms for the same sounds, and a sine carrier makes Bessel sidebands") {
	// THE SAME TERMS: one constellation built as V5 and as Basic Shapes hands a voice identical FM.
	Character v5 = feathers::test::characterAt(OscEngine::Morph, 17, 0.31f);
	v5.flagship = true;
	v5.sounds[0].fmDepth = 0.6f;
	v5.sounds[1].fmDepth = 0.6f;
	Character shapes = v5;
	shapes.engine = OscEngine::Shapes;
	shapes.flagship = false;
	BankedVoice a;
	BankedVoice b;
	a.setSampleRate(kSampleRate);
	b.setSampleRate(kSampleRate);
	a.setCharacter(v5);
	b.setCharacter(shapes);
	const PreparedCharacter& ta = a.bank.current();
	const PreparedCharacter& tb = b.bank.current();
	REQUIRE(ta.fmDepth() > 0.0f);
	CHECK(tb.fmDepth() == ta.fmDepth());
	CHECK(tb.fmFrequencyRatio() == ta.fmFrequencyRatio());
	CHECK(tb.fmStereoWidth() == ta.fmStereoWidth());
	CHECK(tb.fmIndexCyclesFor(440.0) == ta.fmIndexCyclesFor(440.0));

	// AND IT MODULATES: a sine carrier at index I (cycles) has sidebands at fc ± n·fm of amplitude |J_n(2πI)|.
	// The ratio is the field's at the sound's place, so the place is chosen where every sideband this reads,
	// and its mirror images through zero, lie clear of one another — at a ratio near 1 the lower sidebands
	// fold back onto the upper ones and no window can separate them (MEASURED: 0.97 dB at offset 0.31).
	constexpr int kN = 32768;
	const double fc = 300.0;
	Character fmCase = shapes;
	double ratio = 0.0;
	for (float offset = 0.0f; offset < 4.0f; offset += 0.01f) {
		fmCase.sounds[0].phaseOffset = offset;
		fmCase.sounds[1].phaseOffset = offset;
		BankedVoice probe;
		probe.setSampleRate(kSampleRate);
		probe.setCharacter(fmCase);
		const double r = probe.bank.current().fmFrequencyRatio();
		std::vector<double> lines;
		for (int n = -3; n <= 3; ++n) {
			lines.push_back(std::abs(fc + n * fc * r));
		}
		std::sort(lines.begin(), lines.end());
		// And no phase jitter or grit at this place: both are broadband and would take level from every line.
		bool clear = lines.front() > 60.0 && probe.bank.current().phaseJitter() == 0.0f
		             && probe.bank.current().ampNoise() == 0.0f;
		for (size_t i = 1; i < lines.size(); ++i) {
			clear = clear && lines[i] - lines[i - 1] > 60.0;
		}
		if (clear) {
			ratio = r;
			break;
		}
	}
	REQUIRE(ratio > 0.0);
	const std::vector<float> x = renderVoice(fmCase, fc, kN);
	BankedVoice built;
	built.setSampleRate(kSampleRate);
	built.setCharacter(fmCase);
	const double index = built.bank.current().fmIndexCyclesFor(fc);
	const double fm = fc * ratio;
	const double beta = 2.0 * kPi * index;
	const Window win(kN);
	for (const int n : { 0, 1, 2 }) {
		const double want = std::abs(besselJ(n, beta));
		const double up = feathers::test::magnitudeAt(x, win, kSampleRate, fc + n * fm);
		std::printf("[basic shapes] FM sideband %d: %.4f against J=%.4f (beta %.3f, ratio %.3f)\n", n, up, want, beta,
		            fm / fc);
		INFO("sideband " << n);
		CHECK(std::abs(dB(up / want)) < 0.02);
	}
}

TEST_CASE("basic shapes: Phase offsets the shapes' read phase, as it does every table engine's") {
	// voice-level's Phase (osc.phase) is added to the voice's READ phase before the side is evaluated, so a Shapes
	// side gets it through the same seam as Morph and V5. Asserted by value: the read phase moves by exactly the
	// offset, and the sample is the mix read there.
	const Character c = shapesCharacter(ShapesMix { 0.0f, 0.0f, 1.0f, 0.0f });
	const ShapesWeights w = feathers::normaliseShapes(c.shapes);
	const auto first = [&](float offset, double& read, int& mip) {
		BankedVoice v;
		v.setSampleRate(kSampleRate);
		v.setFrequency(110.0);
		v.setCharacter(c);
		v.voice.setReadPhase(offset, nullptr, 0);
		float out[4] {};
		v.renderWithoutOversamplingForTest(out, 4);
		read = v.voice.lastReadPhaseForTest();
		mip = v.voice.shapesMipForTest();
		return out[3];
	};
	double readA = 0.0;
	double readB = 0.0;
	int mipA = 0;
	int mipB = 0;
	const float outA = first(0.0f, readA, mipA);
	const float outB = first(0.25f, readB, mipB);
	const double moved = (readB - readA) - std::floor(readB - readA);
	CHECK(moved == doctest::Approx(0.25).epsilon(1e-9));
	CHECK(outA == doctest::Approx(evalShapes(w, mipA, readA)).epsilon(1e-6));
	CHECK(outB == doctest::Approx(evalShapes(w, mipB, readB)).epsilon(1e-6));
	CHECK(std::abs(outB - outA) > 0.1f); // not vacuous: a saw a quarter cycle on is somewhere else
}

TEST_CASE("basic shapes KV: an operator knob at or below neutral, and the terrain's operator fields, leave the played table and the sound as they were") {
	// THE HONESTY SWEEP, on the real parameter road. Since backlog KV the four operator amounts are LIVE on Basic Shapes
	// above neutral (the cases below), but the terrain's own operator fields do not act (flagship.h): an amount is
	// fieldWithOverride(0, knob), so at and below the knob's neutral it is zero and the engine is what it was, to the
	// bit. Points is structure, read only under Smoothness. Each move is on a patch with FM, Warp and Grit in play.
	const auto base = [](Rig& r) {
		r.set(ParamId::AFmDepth, 0.7);
		r.set(ParamId::WarpMode, 1.0);
		r.set(ParamId::AWarp, 0.6);
		r.set(ParamId::AGrit, 0.8);
		r.set(ParamId::ShapesSaw, 0.5);
	};
	Rig ref(OscType::BasicShapes);
	base(ref);
	const std::vector<float> refSound = ref.play(4096);
	const Played refPlayed = playedBy(ref);
	REQUIRE(refPlayed.engine == OscEngine::Shapes);
	REQUIRE(refPlayed.fmDepth > 0.0f);
	REQUIRE_FALSE(ref.table().flagship.operated);
	// NOT SILENCE: until backlog KV the rig left Voice Level at 0, so every comparison below was silence against silence.
	REQUIRE(*std::max_element(refSound.begin(), refSound.end()) > 0.1f);

	struct Move {
		ParamId id;
		double value;
		const char* name;
	};
	for (const Move m : { Move { ParamId::ASmoothness, 0.0, "Smoothness 0" }, Move { ParamId::AQuantize, 0.0, "Quantize 0" },
	                      Move { ParamId::ASync, 0.0, "Sync 0" }, Move { ParamId::AFeedback, 0.0, "Feedback 0" },
	                      Move { ParamId::APoints, 0.0, "Points 0" }, Move { ParamId::APoints, 1.0, "Points 1" },
	                      Move { ParamId::AZone, 700.0, "Zone (the terrain's operator fields)" } }) {
		Rig r(OscType::BasicShapes);
		base(r);
		r.set(m.id, m.value);
		const std::vector<float> sound = r.play(4096);
		INFO(m.name);
		CHECK_FALSE(r.table().flagship.operated);
		CHECK(r.table().recipe.quantize == 0.0f);
		CHECK(r.table().recipe.syncRatio == 1.0f);
		CHECK(r.table().recipe.feedback == 0.0f);
		CHECK(r.table().recipe.smoothness == 0.0f);
		if (m.id != ParamId::AZone) { // the zone moves the FM interval, which is its job
			CHECK(playedBy(r) == refPlayed);
			CHECK(sound == refSound);
		}
	}
}

namespace {

/// THE MIX AS THE BAKE READS IT, restated from the Fourier series at the bake's mip (128 harmonics).
double restatedMix(const ShapesWeights& w, double phase) {
	const int mip = feathers::kShapesBakeMip;
	return static_cast<double>(w.sine) * restated(Shape::Sine, mip, phase)
	       + static_cast<double>(w.triangle) * restated(Shape::Triangle, mip, phase)
	       + static_cast<double>(w.saw) * restated(Shape::Saw, mip, phase)
	       + static_cast<double>(w.square) * restated(Shape::Square, mip, phase);
}

/// One operator knob on EVERY sound (so the blend is the knob's own value whatever the puck's weights), on a saw.
Rig& operated(Rig& r, std::initializer_list<std::pair<ParamId (*)(size_t), double>> knobs) {
	r.set(ParamId::ShapesSine, 0.0);
	r.set(ParamId::ShapesSaw, 1.0);
	for (const auto& [param, value] : knobs) {
		for (size_t s = 0; s < feathers::kMaxSounds; ++s) {
			r.set(param(s), value);
		}
	}
	r.play(256);
	return r;
}

/// The baked base grid against `restatedRaw`, normalised by the recipe's own offset and gain.
void checkGrid(const PreparedCharacter& t, const std::function<double(double)>& restatedRaw, double tolerance) {
	REQUIRE(t.flagship.operated);
	// AND THE VOICE IS HANDED IT: the grid road (gridFor) is the bake, at every stair mip.
	CHECK(t.gridFor(0) == t.flagship.slots);
	double worst = 0.0;
	for (int j = 0; j < feathers::kFlagshipSlots; ++j) {
		const double p = static_cast<double>(j) / static_cast<double>(feathers::kFlagshipSlots);
		const double want = (restatedRaw(p) - static_cast<double>(t.recipe.dcOffset)) * static_cast<double>(t.recipe.gain);
		worst = std::max(worst, std::abs(static_cast<double>(t.flagship.slots[j]) - want));
	}
	CAPTURE(worst);
	CHECK(worst < tolerance);
}

/// The smooth source, restated: the cubic through the mix at K even points, K crossfaded floor to ceiling.
double restatedSmooth(const ShapesWeights& w, double pointCount, double p) {
	const auto curve = [&](int n) {
		std::array<float, feathers::kFlagshipMaxSmoothPoints> amp {};
		for (int j = 0; j < n; ++j) {
			amp[static_cast<size_t>(j)] = static_cast<float>(restatedMix(w, static_cast<double>(j) / n));
		}
		return feathers::flagshipSmoothCurveForTest(amp.data(), n, p);
	};
	const double want = std::clamp(pointCount, 2.0, static_cast<double>(feathers::kFlagshipMaxSmoothPoints));
	const int lo = static_cast<int>(std::floor(want));
	const double frac = want - lo;
	const double low = curve(lo);
	return frac <= 0.0 || lo >= feathers::kFlagshipMaxSmoothPoints ? low : low + frac * (curve(lo + 1) - low);
}

} // namespace

TEST_CASE("basic shapes KV: Quantize holds the MIX at each step's centre, and the grid plays the staircase") {
	Rig r(OscType::BasicShapes);
	const PreparedCharacter& t = operated(r, { { feathers::quantizeParam, 1.0 } }).table();
	CHECK(t.recipe.quantize == feathers::fieldWithOverride(0.0f, 1.0f));
	REQUIRE(t.recipe.steps >= 2);
	const double sc = static_cast<double>(t.recipe.stepCount);
	for (int i = 0; i < t.recipe.steps; ++i) {
		const double centre = 0.5 * (i / sc + std::min((i + 1) / sc, 1.0));
		CAPTURE(i);
		CHECK(static_cast<double>(t.recipe.held[static_cast<size_t>(i)]) == doctest::Approx(restatedMix(t.shapes, centre)).epsilon(1e-4));
	}
	// At q = 1, past each step's riser, the grid IS the held value (normalised).
	for (int j = 0; j < feathers::kFlagshipSlots; ++j) {
		const double p = static_cast<double>(j) / feathers::kFlagshipSlots;
		const int i = std::min(static_cast<int>(p * sc), t.recipe.steps - 1);
		if (p * sc - i < static_cast<double>(t.recipe.riserSlope) + 1e-9) {
			continue;
		}
		CAPTURE(j);
		CHECK(t.flagship.slots[j] == doctest::Approx(feathers::flagshipNormalised(t.recipe, t.recipe.held[static_cast<size_t>(i)])));
	}
	CHECK(t.gridFor(0) == t.flagship.slots);
}

TEST_CASE("basic shapes KV: Sync reads the mix at three times its rate, resetting each cycle") {
	Rig r(OscType::BasicShapes);
	const PreparedCharacter& t = operated(r, { { feathers::syncParam, 1.0 } }).table();
	CHECK(t.recipe.syncRatio == 1.0f + (feathers::kFlagshipSyncMaxRatio - 1.0f) * feathers::fieldWithOverride(0.0f, 1.0f));
	CHECK(t.recipe.syncRatio == 3.0f);
	checkGrid(t, [&](double p) { const double x = 3.0 * p; return restatedMix(t.shapes, x - std::floor(x)); }, 1e-4);
}

TEST_CASE("basic shapes KV: Feedback folds the mix through itself, y = S(p + depth y), its iterations") {
	Rig r(OscType::BasicShapes);
	const PreparedCharacter& t = operated(r, { { feathers::feedbackParam, 1.0 } }).table();
	CHECK(t.recipe.feedback == feathers::fieldWithOverride(0.0f, 1.0f));
	const double depth = static_cast<double>(t.recipe.feedback) * feathers::kFlagshipFeedbackMax;
	checkGrid(t, [&](double p) {
		const auto at = [&](double x) { return restatedMix(t.shapes, x - std::floor(x)); };
		double y = at(p);
		for (int k = 0; k < feathers::kFlagshipFeedbackIterations; ++k) {
			y = at(p + depth * y);
		}
		return y;
	}, 1e-3);
}

TEST_CASE("basic shapes KV: Smoothness replaces the mix by the cubic through it at K even points") {
	Rig r(OscType::BasicShapes);
	const PreparedCharacter& t = operated(r, { { feathers::smoothnessParam, 1.0 } }).table();
	CHECK(t.recipe.smoothness == feathers::fieldWithOverride(0.0f, 1.0f));
	CHECK(t.recipe.smoothFromShapes);
	checkGrid(t, [&](double p) { return restatedSmooth(t.shapes, static_cast<double>(t.recipe.pointCount), p); }, 1e-4);
}

TEST_CASE("basic shapes KV: Points sets how many points the smooth cubic passes through, and the grid follows it") {
	Rig few(OscType::BasicShapes);
	Rig many(OscType::BasicShapes);
	// SMOOTHNESS AT 0.75, NOT 1: V5's law walks the count toward two over the knob's top half (pointCountWithOverride),
	// so at 1 every Points setting is two points. At 0.75 the amount is a half and Points 0 / 1 keep 2 / 5.
	const PreparedCharacter& a = operated(few, { { feathers::smoothnessParam, 0.75 }, { feathers::pointsParam, 0.0 } }).table();
	const PreparedCharacter& b = operated(many, { { feathers::smoothnessParam, 0.75 }, { feathers::pointsParam, 1.0 } }).table();
	CHECK(a.recipe.smoothness == doctest::Approx(0.5f));
	CAPTURE(a.recipe.pointCount);
	CAPTURE(b.recipe.pointCount);
	CHECK(a.recipe.pointCount < b.recipe.pointCount);
	const auto halfSmooth = [](const PreparedCharacter& t) {
		return [&t](double p) {
			const double mix = restatedMix(t.shapes, p);
			return mix + 0.5 * (restatedSmooth(t.shapes, static_cast<double>(t.recipe.pointCount), p) - mix);
		};
	};
	checkGrid(a, halfSmooth(a), 1e-4);
	checkGrid(b, halfSmooth(b), 1e-4);
}

TEST_CASE("basic shapes: every control the plan enables moves what the voice reads") {
	const auto read = [](std::initializer_list<std::pair<ParamId, double>> sets) {
		Rig r(OscType::BasicShapes);
		for (const auto& [id, v] : sets) {
			r.set(id, v);
		}
		r.play(256);
		return playedBy(r);
	};
	const Played plain = read({});
	// The four shapes, each by its normalised weight.
	CHECK(read({ { ParamId::ShapesSine, 0.0 } }).shapes == plain.shapes); // all-zero is the sine
	const Played tri = read({ { ParamId::ShapesSine, 0.0 }, { ParamId::ShapesTriangle, 1.0 } });
	CHECK(tri.shapes.triangle == doctest::Approx(1.0 / (1.0 + feathers::kShapesSineEpsilon)));
	CHECK(read({ { ParamId::ShapesSaw, 1.0 } }).shapes.saw == doctest::Approx(1.0 / (2.0 + feathers::kShapesSineEpsilon)));
	CHECK(read({ { ParamId::ShapesSquare, 0.5 } }).shapes.square
	      == doctest::Approx(0.5 / (1.5 + feathers::kShapesSineEpsilon)));
	// FM Depth (per sound), Warp + its mode, Grit.
	CHECK(plain.fmDepth == 0.0f);
	CHECK(read({ { ParamId::AFmDepth, 1.0 } }).fmDepth > 0.0f);
	const Played warped = read({ { ParamId::WarpMode, 1.0 }, { ParamId::AWarp, 0.6 } });
	CHECK(warped.warpMode == 1);
	CHECK(warped.warpDepth > 0.0f);
	CHECK(plain.ampNoise == 0.0f);
	CHECK(read({ { ParamId::AGrit, 1.0 } }).ampNoise > 0.0f);
	// The constellation places the FM: moving the sound moves the interval its field gives.
	const Played here = read({ { ParamId::AFmDepth, 1.0 }, { ParamId::AZone, 100.0 } });
	const Played there = read({ { ParamId::AFmDepth, 1.0 }, { ParamId::AZone, 700.0 } });
	CHECK(here.fmRatio != there.fmRatio);
}

namespace {

/// The parameter road's reading for one ear, built by the mapping the synth calls: a patch's values over the
/// descriptors' defaults.
Character earCharacter(std::initializer_list<std::pair<ParamId, double>> sets, feathers::EarSide ear) {
	const auto read = [&](ParamId id) {
		for (const auto& [at, v] : sets) {
			if (at == id) {
				return v;
			}
		}
		const feathers::ParamDesc* d = feathers::findParam(static_cast<clap_id>(id));
		REQUIRE(d != nullptr);
		return d->def;
	};
	return feathers::characterFromParams(read, OscType::BasicShapes, feathers::AuditionTuning {},
	                                     feathers::AnchorPoint {}, feathers::SoundTravel {},
	                                     feathers::SoundOrientations {}, feathers::SoundResidue {},
	                                     feathers::SoundGamma {}, feathers::Preview {}, ear);
}

/// What one prepared table hands a Shapes voice, as a comparable value.
Played playedOf(const PreparedCharacter& t, bool stereo) {
	return { t.engine,           t.shapes,         t.fmDepth(),
		     t.fmFrequencyRatio(), t.fmStereoWidth(), t.fmIndexCyclesFor(261.6),
		     t.ampNoise(),       t.phaseJitter(),  t.recipe.warpDepth,
		     static_cast<int>(t.recipe.warpMode), stereo };
}

} // namespace

TEST_CASE("basic shapes: Width on Basic Shapes builds two ears, each its own character's table, and the voice plays them") {
	// Backlog KV / decision D: the synth's terrain-stereo predicate includes Basic Shapes. With the master Width up and
	// two enabled sounds wide, the bank builds each ear from that ear's character (anchors displaced per sound), so the
	// ears differ in what the displaced weights blend: FM ratio and width, warp depth, grit.
	const std::initializer_list<std::pair<ParamId, double>> wide = {
		{ ParamId::Width, 1.0 },     { ParamId::AWidth, 1.0 },  { ParamId::BWidth, 1.0 },
		{ ParamId::AFmDepth, 0.7 },  { ParamId::BFmDepth, 0.7 }, { ParamId::AZone, 100.0 },
		{ ParamId::BZone, 700.0 },   { ParamId::WarpMode, 1.0 }, { ParamId::AWarp, 0.6 },
		{ ParamId::AGrit, 0.8 },     { ParamId::ShapesSaw, 0.5 },
	};
	Rig rig(OscType::BasicShapes);
	for (const auto& [id, v] : wide) {
		rig.set(id, v);
	}
	rig.play(256);
	const feathers::CharacterBank& bank = rig.synth.mainBankForTest();
	REQUIRE(bank.stereoActive());

	// Each ear's table is the one its own character builds, restated through a bank of its own.
	BankedVoice left;
	BankedVoice right;
	left.setSampleRate(kSampleRate);
	right.setSampleRate(kSampleRate);
	left.setCharacter(earCharacter(wide, feathers::EarSide::Left));
	right.setCharacter(earCharacter(wide, feathers::EarSide::Right));
	const Played l = playedOf(bank.current(0), true);
	const Played r = playedOf(bank.current(1), true);
	CHECK(l == playedOf(left.bank.current(), true));
	CHECK(r == playedOf(right.bank.current(), true));
	CHECK(l.engine == OscEngine::Shapes);
	CHECK(r.engine == OscEngine::Shapes);
	CHECK(l.fmRatio != r.fmRatio); // two ears, not one twice

	// The voice plays both: its two-ear stream is on and the channels differ.
	rig.synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> outL(4096), outR(4096);
	for (int at = 0; at < 4096; at += 256) {
		rig.synth.render(outL.data() + at, outR.data() + at, 256);
	}
	const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), 0);
	CHECK(osc.stereoStreamForTest());
	CHECK(*std::max_element(outL.begin(), outL.end()) > 0.1f);
	CHECK(outL != outR);

	// AND AT WIDTH 0 THE PATCH IS MONO, the road it took before (FM off too, whose stereo flip needs two ears at any
	// width — CharacterBank::fmNeedsTwoEars): no two-ear build, the voice on its mono stream, and the
	// two channels the same samples to the bit.
	Rig narrow(OscType::BasicShapes);
	for (const auto& [id, v] : wide) {
		if (id != ParamId::AWidth && id != ParamId::BWidth && id != ParamId::AFmDepth && id != ParamId::BFmDepth) {
			narrow.set(id, v);
		}
	}
	narrow.play(256);
	CHECK_FALSE(narrow.synth.mainBankForTest().stereoActive());
	narrow.synth.noteOn(0, 60, 1, 1.0);
	for (int at = 0; at < 4096; at += 256) {
		narrow.synth.render(outL.data() + at, outR.data() + at, 256);
	}
	CHECK_FALSE(narrow.synth.voiceStackOscForTest(narrow.synth.newestVoiceSlotForTest(), 0).stereoStreamForTest());
	CHECK(outL == outR);
}

// ---- Warp: the mip from the warp's steepest read (backlog KV, decided 2026-10-11) -----------------------------

namespace {

using feathers::WarpMode;

/// warpRead's steepest read, measured: the largest |Δ read phase| / Δ output phase over a dense sweep of the cycle.
/// Independent of warpMaxSlope's closed forms, so the two can disagree.
double measuredMaxSlope(WarpMode mode, float knob) {
	constexpr int kSteps = 1 << 16;
	double worst = 0.0;
	double prev = feathers::warpRead(mode, 0.0, knob).phase;
	for (int i = 1; i <= kSteps; ++i) {
		const double p = static_cast<double>(i) / kSteps;
		const double now = feathers::warpRead(mode, std::min(p, 1.0 - 1e-12), knob).phase;
		worst = std::max(worst, std::abs(now - prev) * kSteps);
		prev = now;
	}
	return worst;
}

Character warpedSaw(WarpMode mode, float knob) {
	Character c = shapesCharacter(ShapesMix { 0.0f, 0.0f, 1.0f, 0.0f });
	c.warpMode = mode;
	c.sounds[0].warpDepth = knob;
	c.sounds[1].warpDepth = knob;
	return c;
}

/// Harmonic h of the IDEAL warped saw — the full-band saw 1 − 2φ read through warpRead — as an amplitude, from a dense
/// one-cycle DFT. The textbook shape, not the engine's band-limited ladder.
double idealWarpedSawHarmonic(WarpMode mode, float knob, int h) {
	constexpr int kL = 1 << 15;
	double re = 0.0;
	double im = 0.0;
	for (int i = 0; i < kL; ++i) {
		const double p = (static_cast<double>(i) + 0.5) / kL;
		const feathers::WarpRead r = feathers::warpRead(mode, p, knob);
		const double ph = r.phase - std::floor(r.phase);
		const double y = static_cast<double>(r.sign) * (1.0 - 2.0 * ph);
		re += y * std::cos(2.0 * kPi * h * p);
		im += y * std::sin(2.0 * kPi * h * p);
	}
	return 2.0 * std::hypot(re, im) / kL;
}

} // namespace

TEST_CASE("basic shapes: the warp's maximum slope is warpRead's steepest read, per mode and depth, and exactly 1 at rest") {
	for (int m = 0; m < static_cast<int>(WarpMode::kCount); ++m) {
		const auto mode = static_cast<WarpMode>(m);
		for (const float knob : { 0.0f, 0.3f, 0.5f, 0.6f, 1.0f }) {
			CAPTURE(m);
			CAPTURE(knob);
			const double want = mode == WarpMode::Flip ? 1.0 : measuredMaxSlope(mode, knob);
			CHECK(feathers::warpMaxSlope(mode, knob) == doctest::Approx(want).epsilon(1e-3));
		}
	}
	// AT REST IT IS EXACTLY ONE, so a voice that multiplies by it picks the mip it always did, to the bit.
	CHECK(feathers::warpMaxSlope(WarpMode::Off, 1.0f) == 1.0);
	CHECK(feathers::warpMaxSlope(WarpMode::BendPlus, 0.0f) == 1.0);
	CHECK(feathers::warpMaxSlope(WarpMode::Pwm, 0.0f) == 1.0);
	CHECK(feathers::warpMaxSlope(WarpMode::BendBipolar, 0.5f) == 1.0);
	CHECK(feathers::warpMaxSlope(WarpMode::AsymBipolar, 0.5f) == 1.0);
	// Not vacuous: full depth is a real bound (Bend and Asym 1 + 3a; PWM half a cycle over t = 0.05).
	CHECK(feathers::warpMaxSlope(WarpMode::BendPlus, 1.0f) == doctest::Approx(4.0));
	CHECK(feathers::warpMaxSlope(WarpMode::Pwm, 1.0f) == doctest::Approx(10.0));
}

TEST_CASE("basic shapes: a warped voice reads the mip its note's top frequency times the warp's steepest slope picks") {
	for (const double hz : { 61.0, 220.0, 1300.0, 3000.0 }) {
		for (const auto& [mode, knob] : { std::pair { WarpMode::BendPlus, 1.0f }, std::pair { WarpMode::AsymMinus, 0.5f },
		                                  std::pair { WarpMode::Pwm, 1.0f }, std::pair { WarpMode::Mirror, 1.0f },
		                                  std::pair { WarpMode::Flip, 1.0f }, std::pair { WarpMode::BendPlus, 0.0f } }) {
			BankedVoice v;
			v.setSampleRate(kSampleRate);
			v.setFrequency(hz);
			v.setCharacter(warpedSaw(mode, knob));
			std::vector<float> out(64);
			v.render(out.data(), 64);
			const double slope = mode == WarpMode::Flip ? 1.0 : measuredMaxSlope(mode, knob);
			CAPTURE(hz);
			CAPTURE(static_cast<int>(mode));
			CAPTURE(knob);
			CHECK(v.voice.shapesMipForTest() == restatedMip(hz * slope, kSampleRate));
			if (slope >= 2.0) {
				CHECK(v.voice.shapesMipForTest() > restatedMip(hz, kSampleRate)); // the warp moved it
			}
		}
	}
}

TEST_CASE("basic shapes: a full warp on a 4 kHz saw leaves little alias, and the top band it costs is the accepted amount") {
	// ALIASING, by value: the summed energy of every non-harmonic bin below 20 kHz, re the fundamental. MEASURED
	// 2026-10-11 with the warp ignored by the mip (the old voice) against the warp's maximum slope (this):
	//   Bend+ −37.9 → −57.3 dB, Asym+ −30.1 → −44.0, PWM −19.6 → −36.6.
	constexpr int kN = 16384;
	const Window win(kN);
	const auto spectrumOf = [&](WarpMode mode, float knob, int bin) {
		return feathers::test::amplitudeSpectrum(renderVoice(warpedSaw(mode, knob), bin * kSampleRate / kN, kN), win);
	};
	for (const auto& [mode, bound] : { std::pair { WarpMode::BendPlus, -50.0 }, std::pair { WarpMode::AsymPlus, -40.0 },
	                                   std::pair { WarpMode::Pwm, -32.0 } }) {
		constexpr int kBin = 1365; // 3999 Hz
		const std::vector<double> spec = spectrumOf(mode, 1.0f, kBin);
		double other = 0.0;
		for (size_t b = 1; b < spec.size() && static_cast<double>(b) * kSampleRate / kN < 20000.0; ++b) {
			const int nearest = static_cast<int>(std::lround(static_cast<double>(b) / kBin)) * kBin;
			if (std::abs(static_cast<int>(b) - nearest) > 8) {
				other += spec[b] * spec[b];
			}
		}
		const double aliasDb = 10.0 * std::log10(other / (spec[kBin] * spec[kBin]));
		std::printf("[basic shapes] warp mode %d at 4 kHz: non-harmonic energy %.1f dB re the fundamental\n",
		            static_cast<int>(mode), aliasDb);
		CAPTURE(static_cast<int>(mode));
		CHECK(aliasDb < bound);
	}

	// THE TOP BAND, on the population a player is on — a low and a mid note: harmonic energy from 3 to 12 kHz against
	// the IDEAL warped saw. Bend keeps it (its saw jump sits in its fast region); Asym and PWM, which also read SLOWLY
	// where the jump is, lose 4–6 dB to the one bound for the whole cycle. DECIDED AND ACCEPTED, and
	// pinned here so the cost cannot grow unnoticed. Measured: Asym −5.9/−5.8, PWM −5.7/−4.2 dB at 99.6/439.5 Hz.
	for (const int bin : { 34, 150 }) { // 99.6 Hz, 439.5 Hz
		for (const auto& [mode, lo, hi] : { std::tuple { WarpMode::BendPlus, -0.5, 0.5 },
		                                    std::tuple { WarpMode::AsymPlus, -7.0, 0.5 },
		                                    std::tuple { WarpMode::Pwm, -7.0, 0.5 } }) {
			const double hz = bin * kSampleRate / kN;
			const std::vector<double> spec = spectrumOf(mode, 1.0f, bin);
			double got = 0.0;
			double want = 0.0;
			for (int h = static_cast<int>(std::ceil(3000.0 / hz)); h * hz < 12000.0; ++h) {
				const double a = idealWarpedSawHarmonic(mode, 1.0f, h);
				want += a * a;
				got += spec[static_cast<size_t>(h * bin)] * spec[static_cast<size_t>(h * bin)];
			}
			const double bandDb = 10.0 * std::log10(got / want);
			std::printf("[basic shapes] warp mode %d at %.1f Hz: 3-12 kHz harmonic energy %.2f dB vs the ideal\n",
			            static_cast<int>(mode), hz, bandDb);
			CAPTURE(static_cast<int>(mode));
			CAPTURE(hz);
			CHECK(bandDb > lo);
			CHECK(bandDb < hi);
		}
	}
}

TEST_CASE("basic shapes: a warp that means nothing is the unwarped voice to the bit") {
	// Warp 0 (a unipolar mode at knob 0, a bipolar one at its 0.5 rest) and Warp Off read the same mip and the same samples.
	for (const double hz : { 59.0, 440.0, 4000.0 }) {
		const std::vector<float> off = renderVoice(warpedSaw(WarpMode::Off, 1.0f), hz, 4096);
		REQUIRE(*std::max_element(off.begin(), off.end()) > 0.5f);
		CAPTURE(hz);
		CHECK(renderVoice(warpedSaw(WarpMode::BendPlus, 0.0f), hz, 4096) == off);
		CHECK(renderVoice(warpedSaw(WarpMode::Pwm, 0.0f), hz, 4096) == off);
		CHECK(renderVoice(warpedSaw(WarpMode::BendBipolar, 0.5f), hz, 4096) == off);
		// AND THE MIP IS THE UNWARPED NOTE'S: comparing the renders with each other cannot see a shift that hits all
		// of them alike, so the value is asserted too.
		for (const auto& [mode, knob] : { std::pair { WarpMode::Off, 1.0f }, std::pair { WarpMode::BendPlus, 0.0f },
		                                  std::pair { WarpMode::BendBipolar, 0.5f } }) {
			BankedVoice v;
			v.setSampleRate(kSampleRate);
			v.setFrequency(hz);
			v.setCharacter(warpedSaw(mode, knob));
			std::vector<float> out(64);
			v.render(out.data(), 64);
			CHECK(v.voice.shapesMipForTest() == restatedMip(hz, kSampleRate));
		}
	}
}

TEST_CASE("basic shapes KV: an operated mix is what the VOICE reads — its playing cycle is the baked grid, not the ladder") {
	// The bank's bake is only half of it: the voice must read it (sideEval's operated arm). Each modifier at full, one at
	// a time, through the real voice; the playing cycle is the grid the voice was handed, sample for sample.
	for (int which = 0; which < 4; ++which) {
		Character c = shapesCharacter({ 0.0f, 0.0f, 1.0f, 0.0f });
		for (auto& s : c.sounds) {
			(which == 0 ? s.quantizeScale : which == 1 ? s.syncScale : which == 2 ? s.feedbackScale : s.smoothnessScale) = 1.0f;
		}
		BankedVoice v;
		v.setSampleRate(kSampleRate);
		v.setFrequency(110.0);
		v.setCharacter(c);
		std::vector<float> out(256);
		v.render(out.data(), 256);
		const PreparedCharacter* t = v.voice.playingTable();
		REQUIRE(t != nullptr);
		CAPTURE(which);
		REQUIRE(t->flagship.operated);
		REQUIRE(v.voice.playingGrid() != nullptr);
		double worstGrid = 0.0;
		double worstLadder = 0.0;
		for (int j = 0; j < 64; ++j) {
			const double p = (j + 0.37) / 64.0;
			const double played = v.voice.playingCycleSample(p);
			worstGrid = std::max(worstGrid, std::abs(played - feathers::evalFlagshipGrid(v.voice.playingGrid(), p)));
			worstLadder = std::max(worstLadder, std::abs(played - t->evalShapesAt(0, p)));
		}
		CHECK(worstGrid == 0.0);
		CHECK(worstLadder > 0.01); // NOT VACUOUS: the operated cycle is not the plain saw (Feedback, gentlest, moves it 0.023)
	}
}

// **BACKLOG MA, CLOSED BY A GUARD RATHER THAN BY AN AUDIT** (2026-10-31). A bare-Synth rig that renders silence makes every
// "unchanged"/"differs" comparison on it vacuous: green whatever the code does. The audit of 2026-10-11 found none silent
// and put a REQUIRE in each comparing case; THIS asks the rig itself, so a rig edited to stop sounding (its Voice Level
// line deleted, its engine left silent) fails here by name, before any case that leans on it.
TEST_CASE("backlog MA: the bare rig's reference render SOUNDS, so a difference test on it can see a difference") {
	const auto peakOf = [](const std::vector<float>& x) { float p = 0.0f; for (const float v : x) { p = std::max(p, std::fabs(v)); } return p; };
	Rig rig(OscType::BasicShapes);
	const std::vector<float> out = rig.play(4096);
	CAPTURE(peakOf(out));
	REQUIRE(peakOf(out) > 0.05f);
}
