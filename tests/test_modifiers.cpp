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

// The two per-sample modifiers PhiMorph carries: phase jitter and amplitude-dependent grit.
//
// Most zone positions have both at zero, which is the first thing tested here — a modifier that
// changes a sample it was not asked to change would be a regression in every sound the plugin makes,
// not just the ones with grit. Everything after that is a claim about a difference, and each one is
// measured against the same table with the depths cleared rather than against a different zone.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/modifiers.h"
#include "dsp/morph_renderer.h"
#include "character/phi_morph.hpp"
#include "spectrum.h"
#include "voice_harness.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using owlet::dsp::buildPhiMorphWavetable;
using owlet::dsp::PhiMorphWavetable;
using feathers::Character;
using feathers::CharacterNoise;
using feathers::OscEngine;
using feathers::evalMorphTable;
using feathers::test::BankedVoice;
using feathers::test::magnitudeAt;
using feathers::test::Window;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

Character morph(uint16_t zone, float offset) {
	return feathers::test::characterAt(OscEngine::Morph, zone, offset);
}

Character stair(uint16_t zone, float offset) {
	Character c = morph(zone, offset);
	c.engine = OscEngine::Stair;
	return c;
}

struct RenderOpts {
	double freqHz{261.6255653005986}; // C4.
	int samples{4096};
	int block{kBlock};
	uint32_t seed{CharacterNoise::kDefaultSeed};
	bool clearModifiers{false};
};

std::vector<float> renderVoice(const Character& c, const RenderOpts& o) {
	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(o.freqHz);
	v.setNoiseSeed(o.seed);
	v.setCharacter(c);
	if (o.clearModifiers) {
		// After setCharacter, which is what builds the depths, and before the first render, which is
		// what installs the table the depths are on. Both prepared slots are cleared, so this holds
		// across the install too.
		v.clearModifiersForTest();
	}

	std::vector<float> out(static_cast<size_t>(o.samples), 0.0f);
	for (int i = 0; i < o.samples; i += o.block) {
		v.render(out.data() + i, std::min(o.block, o.samples - i));
	}
	return out;
}

/// The zone positions the character layer gives a nonzero depth to, out of the handful the golden
/// capture covers. Everything else in the plugin's test corpus has both at zero, which is the point:
/// these are rare.
struct ModZone {
	const char* name;
	uint16_t zone;
	float offset;
};

const ModZone kJitterZones[] = {
    {"z0 po0.25 (jitter 0.354)", 0, 0.25f},
    {"z127 po1.618 (jitter 0.971, the deepest in the corpus)", 127, 1.618f},
};

const ModZone kGritZones[] = {
    {"z17 po0.25 (ampNoise 0.035)", 17, 0.25f},
    {"z64 po0.25 (ampNoise 0.173)", 64, 0.25f},
};

double correlation(const std::vector<double>& x, const std::vector<double>& y) {
	const auto n = static_cast<double>(x.size());
	double mx = 0.0;
	double my = 0.0;
	for (size_t i = 0; i < x.size(); i++) {
		mx += x[i];
		my += y[i];
	}
	mx /= n;
	my /= n;
	double sxy = 0.0;
	double sxx = 0.0;
	double syy = 0.0;
	for (size_t i = 0; i < x.size(); i++) {
		const double dx = x[i] - mx;
		const double dy = y[i] - my;
		sxy += dx * dy;
		sxx += dx * dx;
		syy += dy * dy;
	}
	return sxy / std::sqrt(sxx * syy);
}

} // namespace

TEST_CASE("the modifier fields are zero on most zone positions, and there they change nothing at all") {
	// The depths are zone-derived and most positions have none. A voice on one of those has to render
	// what it rendered before this file existed, bit for bit — so the noise stream must be unable to
	// reach the output at all, not merely be quiet. Two proofs: clearing the depths that are already
	// zero is a no-op, and reseeding the generator is too.
	const Character clean = morph(17, 0.0f);
	const PhiMorphWavetable t = buildPhiMorphWavetable(17, 0.0f);
	REQUIRE(t.phaseJitter == 0.0f);
	REQUIRE(t.ampNoise == 0.0f);

	RenderOpts o;
	const std::vector<float> plain = renderVoice(clean, o);

	RenderOpts cleared = o;
	cleared.clearModifiers = true;
	CHECK(renderVoice(clean, cleared) == plain);

	RenderOpts reseeded = o;
	reseeded.seed = 0x12345678u;
	CHECK(renderVoice(clean, reseeded) == plain);

	// Stair has no per-sample modifiers at all — the original fixed-point Stair renderer applies neither — so no
	// seed can move a stair sample either, whatever the morph half of a recycled table slot holds.
	const Character st = stair(108, 0.25f);
	CHECK(renderVoice(st, reseeded) == renderVoice(st, o));
}

TEST_CASE("modifiers on stay bounded and finite") {
	// The grit is added after the waveform has already been evaluated against the same rail the
	// hardware clips at, so the sum is re-clipped; the jitter can only move where the waveform is
	// sampled, never how far it goes. Neither can therefore push the oversampled stream past ±1. What
	// the decimator does after that can: a linear-phase-ish filter overshoots on an edge, and the
	// allowance below is for that and nothing else.
	for (double freq : {55.0, 261.6255653005986, 1500.0}) {
		for (const ModZone& z : {kJitterZones[0], kJitterZones[1], kGritZones[0], kGritZones[1]}) {
			INFO("zone: ", std::string(z.name));
			CAPTURE(freq);
			RenderOpts o;
			o.freqHz = freq;
			o.samples = 8192;
			const std::vector<float> x = renderVoice(morph(z.zone, z.offset), o);

			float peak = 0.0f;
			bool finite = true;
			for (float s : x) {
				finite = finite && std::isfinite(s);
				peak = std::max(peak, std::abs(s));
			}
			CHECK(finite);
			CAPTURE(peak);
			CHECK(peak <= 1.05f);
		}
	}
}

TEST_CASE("the noise stream is the voice's own, so a render is reproducible from its seed") {
	// Determinism is not a nicety here: two instances of the plugin holding the same note must not
	// drift apart, and a bounce must match the one before it. That means no global generator state and
	// no clock — the stream depends on the seed and on how many samples have been rendered, nothing
	// else. The mid-run rebuild is in the loop because the fade path draws too.
	const ModZone& z = kGritZones[1];
	INFO("zone: ", std::string(z.name));

	BankedVoice a;
	BankedVoice b;
	for (auto* p : {&a, &b}) {
		p->setSampleRate(kSampleRate);
		p->setFrequency(261.6255653005986);
		p->setCharacter(morph(z.zone, z.offset));
	}

	std::vector<float> xa(static_cast<size_t>(kBlock));
	std::vector<float> xb(static_cast<size_t>(kBlock));
	for (int i = 0; i < 8; i++) {
		if (i == 3) {
			a.setCharacter(morph(0, 0.25f));
			b.setCharacter(morph(0, 0.25f));
		}
		a.render(xa.data(), kBlock);
		b.render(xb.data(), kBlock);
		CAPTURE(i);
		REQUIRE(xa == xb);
	}

	// Block size must not reach the noise either. The hold that keeps the draw rate at the host rate
	// is a voice member for exactly this reason: it has to stay aligned to the decimator's groups of
	// four across a block boundary, however the host chops its buffer up.
	RenderOpts big;
	big.samples = 4096;
	RenderOpts small = big;
	small.block = 37;
	RenderOpts single = big;
	single.block = 1;
	const std::vector<float> ref = renderVoice(morph(z.zone, z.offset), big);
	CHECK(renderVoice(morph(z.zone, z.offset), small) == ref);
	CHECK(renderVoice(morph(z.zone, z.offset), single) == ref);

	// And a different seed has to be a different render, or the seed is decoration.
	RenderOpts other = big;
	other.seed = 0x12345678u;
	CHECK(renderVoice(morph(z.zone, z.offset), other) != ref);

	// reset() returns the stream to the seed rather than leaving it where the last note ended, so a
	// retrigger is reproducible too.
	BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(261.6255653005986);
	v.setCharacter(morph(z.zone, z.offset));
	std::vector<float> first(static_cast<size_t>(kBlock));
	std::vector<float> again(static_cast<size_t>(kBlock));
	v.render(first.data(), kBlock);
	v.reset();
	v.render(again.data(), kBlock);
	CHECK(first == again);
}

TEST_CASE("grit scales with the signal, so zero crossings stay clean") {
	// The per-sample rule is exact: the contribution is u * ampNoise * |waveform|, u uniform on
	// [-1,1] — closed at both ends, per CharacterNoise::kToUnit, not the half-open range an earlier
	// revision claimed. At |waveform| = 0 there is nothing to scale, so the grit vanishes with the signal
	// instead of sitting under it as a constant hiss. That is the whole reason the original fixed-point Morph renderer
	// multiplies by |waveform| rather than adding flat noise.
	//
	// Measured on the rule itself first, where the claim is exact and the zero crossing really is
	// zero. A table with no grit of its own, forced to a depth well above anything the character layer
	// hands out, so the effect is unambiguous.
	PhiMorphWavetable forced = buildPhiMorphWavetable(17, 0.0f);
	REQUIRE(forced.ampNoise == 0.0f);
	forced.ampNoise = 0.4f;

	constexpr int kSteps = 8192;
	CharacterNoise noise;
	std::vector<double> level;
	std::vector<double> residual;
	double worstNearZero = 0.0;
	for (int i = 0; i < kSteps; i++) {
		const double phase = static_cast<double>(i) / static_cast<double>(kSteps);
		const float clean = evalMorphTable(forced, phase);
		const float u = noise.next();
		const float gritty = feathers::applyAmpNoise(clean, forced.ampNoise, u);
		level.push_back(std::abs(static_cast<double>(clean)));
		residual.push_back(std::abs(static_cast<double>(gritty) - static_cast<double>(clean)));
		if (std::abs(clean) < 1e-3f) {
			worstNearZero = std::max(worstNearZero, residual.back());
		}
	}

	const double r = correlation(level, residual);
	CAPTURE(r);
	// Not 1.0 even though the rule is exact: |residual| is |u| times the level and |u| is uniform, so
	// the scatter around the line is the noise itself. With X the level and U = |u| independent of it,
	// corr(X, UX) = 0.5σ / sqrt(σ²/3 + μ²/12), which is 0.866 at its best and lower for a waveform
	// that spends time away from zero. This one measures 0.77.
	CHECK(r > 0.7);
	CAPTURE(worstNearZero);
	CHECK(worstNearZero <= 1e-3 * 0.4);

	// Then through the real voice, on a zone that carries grit natively, against the same table with
	// the depth cleared. The correlation is looser here because the decimator has run: grit from a
	// loud sample smears a few samples into a quiet one, which is exactly what a hardware zero
	// crossing does not do — but the plugin evaluates at 4x and the hardware does not, and this is the
	// price of that. What survives is the part that matters, that the grit tracks the level.
	const ModZone& z = kGritZones[1];
	INFO("zone: ", std::string(z.name));
	RenderOpts o;
	o.samples = 16384;
	RenderOpts cleanOpts = o;
	cleanOpts.clearModifiers = true;
	const std::vector<float> gritty = renderVoice(morph(z.zone, z.offset), o);
	const std::vector<float> clean = renderVoice(morph(z.zone, z.offset), cleanOpts);

	std::vector<double> vLevel;
	std::vector<double> vResidual;
	for (size_t i = 0; i < gritty.size(); i++) {
		vLevel.push_back(std::abs(static_cast<double>(clean[i])));
		vResidual.push_back(std::abs(static_cast<double>(gritty[i]) - static_cast<double>(clean[i])));
	}
	const double vr = correlation(vLevel, vResidual);
	CAPTURE(vr);
	CHECK(vr > 0.5);

	// The same claim stated so the decimator's smearing cannot flatter it: sort by level, compare the
	// quietest tenth of the waveform with the loudest tenth. Flat noise would put the same energy in
	// both.
	std::vector<std::pair<double, double>> pairs;
	for (size_t i = 0; i < vLevel.size(); i++) {
		pairs.emplace_back(vLevel[i], vResidual[i]);
	}
	std::sort(pairs.begin(), pairs.end());
	const size_t decile = pairs.size() / 10;
	double quietest = 0.0;
	double loudest = 0.0;
	for (size_t i = 0; i < decile; i++) {
		quietest += pairs[i].second;
		loudest += pairs[pairs.size() - 1 - i].second;
	}
	CAPTURE(quietest / static_cast<double>(decile));
	CAPTURE(loudest / static_cast<double>(decile));
	CHECK(loudest > 5.0 * quietest);
}

TEST_CASE("modifiers raise the noise floor without moving the fundamental") {
	// What both modifiers are for is the floor between the harmonics — the analogue drift and grit
	// that a perfectly periodic table does not have. What neither is allowed to do is change the note:
	// jitter is phase modulation and does cost the fundamental a little (J0 of the modulation index),
	// grit is zero-mean and costs it nothing but statistics, and both budgets are far inside the half
	// a dB below.
	//
	// 375 Hz is bin 256 of a 32768-point transform at 48 kHz, so every harmonic sits exactly on a bin
	// and "not a harmonic" is a question with an exact answer.
	constexpr int kN = 32768;
	constexpr double kF0 = 375.0;
	const Window win(kN);
	const double binHz = kSampleRate / static_cast<double>(kN);

	auto floorOf = [&](const std::vector<float>& x) {
		const std::vector<double> spec = feathers::test::amplitudeSpectrum(x, win);
		double acc = 0.0;
		for (size_t k = 1; k < spec.size(); k++) {
			const double hz = static_cast<double>(k) * binHz;
			const double harmonic = std::round(hz / kF0);
			// Twelve bins of guard either side. The seven-term window's main lobe is fourteen bins wide
			// peak to null, so a narrower guard measures window leakage and calls it a noise floor.
			// Harmonics are 256 bins apart here, so the guard costs under a tenth of the spectrum.
			//
			// Harmonic zero is in the loop, not excluded from it. These waveforms are not zero-mean —
			// endpoint amplitude and an asymmetric waypoint set both put energy at DC — and leaving the
			// bins around DC in the sum put the floor at -10 dBFS for every zone, modifiers or not,
			// which is the DC term's own main lobe and nothing else.
			if (std::abs(hz - harmonic * kF0) < 12.0 * binHz) {
				continue;
			}
			acc += spec[k] * spec[k];
		}
		return std::sqrt(acc);
	};

	for (const ModZone& z : {kJitterZones[0], kJitterZones[1], kGritZones[0], kGritZones[1]}) {
		INFO("zone: ", std::string(z.name));
		RenderOpts o;
		o.freqHz = kF0;
		o.samples = kN;
		RenderOpts cleanOpts = o;
		cleanOpts.clearModifiers = true;

		const std::vector<float> on = renderVoice(morph(z.zone, z.offset), o);
		const std::vector<float> off = renderVoice(morph(z.zone, z.offset), cleanOpts);

		const double fundOn = magnitudeAt(on, win, kSampleRate, kF0);
		const double fundOff = magnitudeAt(off, win, kSampleRate, kF0);
		const double moved = std::abs(feathers::test::dB(fundOn) - feathers::test::dB(fundOff));
		CAPTURE(moved);
		CHECK(moved < 0.5);

		const double floorOn = floorOf(on);
		const double floorOff = floorOf(off);
		CAPTURE(feathers::test::dB(floorOff));
		CAPTURE(feathers::test::dB(floorOn));
		// Modifiers off, there is no floor at all: the table is exactly periodic at a bin-centred
		// fundamental, so every non-harmonic bin holds arithmetic residue and nothing else — -190 to
		// -217 dBFS across these four zones, which is the double-precision transform's own noise. The
		// bound is set well above that, at -140 dB, so a host with a slacker FFT is not a failure.
		CHECK(floorOff < 1e-7);
		// And modifiers on there is: -36 dBFS for the shallowest grit zone here, -19 dBFS for the
		// deepest jitter one. The floor is the whole point of the modifiers, so a version of this that
		// only checked the fundamental had not moved would pass on code that did nothing.
		CHECK(floorOn > 0.01);
	}
}
