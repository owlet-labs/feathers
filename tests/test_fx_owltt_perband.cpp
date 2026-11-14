/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// OWLTT'S PER-BAND CONTROLS: an upward and a downward ratio trim for each band, a per-band attack and release offset, a per-band
// input level, and the three-way clip. THE GATE IS BIT-IDENTITY: `tests/owltt_ref/` holds the device as it was before
// any of them, in its own namespace and compiled into this binary, so "every new control neutral is the old device"
// is asserted against the old code itself, not against numbers recorded from it.

#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "owltt_ref/owltt.h"
#include "rack/rack_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <string_view>
#include <vector>

namespace nw = feathers::fx::owltt;
namespace od = feathers::fx::owltt_ref;

namespace {

constexpr float kRate = 48000.0f;

/// THE COMMON FIELDS, set the same on either device's `Settings`.
struct Cfg {
	float mode = 6.0f;
	float threshold = 0.5f, ratio = 0.5f, bandwidth = 0.5f, skew = 0.5f, attack = 0.25f, release = 0.25f;
	float character = 0.0f, vibe = 0.0f;
	std::array<float, 3> bandThreshold { 0.5f, 0.5f, 0.5f };
	std::array<float, 3> bandRatio { 0.5f, 0.5f, 0.5f };
	std::array<float, 3> bandBandwidth { 0.5f, 0.5f, 0.5f };
	std::array<float, 3> bandLevel { 0.5f, 0.5f, 0.5f };
	std::array<float, 3> bandWidth { 1.0f, 1.0f, 1.0f };
	float lowHz = 200.0f, highHz = 2000.0f, outputGain = 0.5f, blend = 1.0f;
	bool softClip = true;
	uint32_t lookahead = 0;
	float link = 1.0f;
	bool midSide = false;
	float driveDb = 0.0f;
};

template <class S>
S settingsOf(const Cfg& c) {
	S s;
	s.mode = c.mode;
	s.threshold = c.threshold;
	s.ratio = c.ratio;
	s.bandwidth = c.bandwidth;
	s.upDownSkew = c.skew;
	s.attack = c.attack;
	s.release = c.release;
	s.character = c.character;
	s.vibe = c.vibe;
	auto band = [&](auto& b, size_t i) {
		b.threshold = c.bandThreshold[i];
		b.ratio = c.bandRatio[i];
		b.bandwidth = c.bandBandwidth[i];
		b.level = c.bandLevel[i];
		b.width = c.bandWidth[i];
	};
	band(s.low, 0);
	band(s.mid, 1);
	band(s.high, 2);
	s.lowCrossover = c.lowHz;
	s.highCrossover = c.highHz;
	s.outputGain = c.outputGain;
	s.blend = c.blend;
	s.softClip = c.softClip;
	s.lookahead = c.lookahead;
	s.stereoLink = c.link;
	s.midSide = c.midSide;
	s.driveDb = c.driveDb;
	return s;
}

/// A DETERMINISTIC PROGRAM: tones in each band, bursts, a quiet tail, different left and right (or identical, for mono).
struct Program {
	std::vector<float> l, r;
};

Program programOf(size_t frames, bool mono, uint32_t seed) {
	Program p;
	p.l.resize(frames);
	p.r.resize(frames);
	uint32_t x = seed;
	const auto noise = [&x] {
		x = x * 1664525u + 1013904223u;
		return static_cast<float>(static_cast<int32_t>(x >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
	};
	const float tau = 2.0f * std::numbers::pi_v<float>;
	for (size_t i = 0; i < frames; ++i) {
		const float t = static_cast<float>(i) / kRate;
		const float env = 0.5f + 0.5f * std::sin(tau * 3.0f * t);
		const float burst = (i / 6000) % 3 == 1 ? 1.0f : 0.15f;
		const float a = burst * (0.5f * std::sin(tau * 90.0f * t) + 0.3f * std::sin(tau * 800.0f * t) * env
		                         + 0.2f * std::sin(tau * 5200.0f * t));
		const float b = burst * (0.45f * std::sin(tau * 110.0f * t + 0.4f) + 0.3f * std::sin(tau * 1300.0f * t)
		                         + 0.15f * std::sin(tau * 7000.0f * t * (1.0f + 0.05f * env)));
		p.l[i] = a + 0.02f * noise();
		p.r[i] = mono ? p.l[i] : b + 0.02f * noise();
	}
	return p;
}

template <class D, class S>
std::pair<std::vector<float>, std::vector<float>> render(const S& s, const Program& in, int block) {
	D d;
	d.prepare(kRate);
	d.setTarget(s);
	std::vector<float> l = in.l, r = in.r;
	for (size_t at = 0; at < l.size(); at += static_cast<size_t>(block)) {
		const int n = static_cast<int>(std::min(static_cast<size_t>(block), l.size() - at));
		d.processStereo(&l[at], &r[at], n);
	}
	return { l, r };
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

TEST_CASE("per-band controls: with every new control neutral OWLTT is the device before them, to the bit") {
	// THE GATE. The reference is the pre-change source, compiled in as `owltt_ref`. A sweep over crossover types, skew,
	// the Character and Vibe zones, the globals, block sizes, mono and stereo, lookahead, link, M/S and the clipper.
	uint32_t pick = 12345u;
	const auto next = [&pick](uint32_t n) {
		pick = pick * 1664525u + 1013904223u;
		return (pick >> 10) % n;
	};
	constexpr std::array<float, 10> kModes { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
	constexpr std::array<float, 5> kSkew { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f };
	constexpr std::array<float, 6> kChar { 0.0f, 0.3f, 1.1f, 2.7f, -1.4f, 5.05f };
	constexpr std::array<float, 5> kVibe { 0.0f, 0.6f, 3.2f, -0.7f, 1.5f };
	constexpr std::array<int, 4> kBlocks { 32, 64, 480, 1024 };
	int runs = 0;
	for (int k = 0; k < 96; ++k) {
		Cfg c;
		c.mode = kModes[k % kModes.size()];
		c.skew = kSkew[next(kSkew.size())];
		c.character = kChar[next(kChar.size())];
		c.vibe = kVibe[next(kVibe.size())];
		c.threshold = 0.2f + 0.1f * static_cast<float>(next(7));
		c.ratio = 0.1f * static_cast<float>(next(11));
		c.bandwidth = 0.1f * static_cast<float>(next(11));
		c.attack = 0.1f * static_cast<float>(next(11));
		c.release = 0.1f * static_cast<float>(next(11));
		for (size_t b = 0; b < 3; ++b) {
			c.bandThreshold[b] = 0.1f * static_cast<float>(next(11));
			c.bandRatio[b] = 0.1f * static_cast<float>(next(11));
			c.bandBandwidth[b] = 0.1f * static_cast<float>(next(11));
			c.bandLevel[b] = 0.3f + 0.1f * static_cast<float>(next(6));
			c.bandWidth[b] = 0.25f * static_cast<float>(next(9));
		}
		c.softClip = next(4) != 0;
		c.lookahead = next(3) == 0 ? 3 : 0;
		c.link = next(3) == 0 ? 0.3f : 1.0f;
		c.midSide = next(4) == 0;
		c.driveDb = next(3) == 0 ? 9.0f : 0.0f;
		c.blend = next(4) == 0 ? 0.6f : 1.0f;
		const bool mono = next(3) == 0;
		const int block = kBlocks[next(kBlocks.size())];
		INFO("run " << k << " mode " << c.mode << " block " << block << (mono ? " mono" : " stereo"));
		const Program in = programOf(9000, mono, 77u + static_cast<uint32_t>(k));
		const auto before = render<od::Owltt>(settingsOf<od::Settings>(c), in, block);
		const auto after = render<nw::Owltt>(settingsOf<nw::Settings>(c), in, block);
		CHECK(sameBits(before.first, after.first));
		CHECK(sameBits(before.second, after.second));
		++runs;
	}
	CHECK(runs == 96);
}

TEST_CASE("per-band controls: the neutral values are the declared defaults, and the laws have an identity there") {
	const nw::Settings s {};
	CHECK(s.low.upRatio == 0.5f);
	CHECK(s.mid.downRatio == 0.5f);
	CHECK(s.high.attack == 0.5f);
	CHECK(s.high.release == 0.5f);
	CHECK(s.mid.inputDb == 0.0f);
	for (const char* key : { "low_up_ratio", "low_down_ratio", "mid_up_ratio", "mid_down_ratio", "high_up_ratio", "high_down_ratio",
	                         "low_attack", "low_release", "mid_attack", "mid_release", "high_attack", "high_release" }) {
		bool found = false;
		for (const auto& p : nw::kParamDescs) {
			if (std::string_view(p.key) == key) {
				found = true;
				CHECK(p.def == 0.5);
				CHECK(p.modulatable);
			}
		}
		CHECK(found);
	}
	for (const char* key : { "low_input", "mid_input", "high_input" }) {
		bool found = false;
		for (const auto& p : nw::kParamDescs) {
			if (std::string_view(p.key) == key) {
				found = true;
				CHECK(p.def == 0.0);
				CHECK(p.min == -18.0);
				CHECK(p.max == 18.0);
				CHECK(p.modulatable);
			}
		}
		CHECK(found);
	}
	// THE NEUTRAL ROW IS THE DEFAULT ROW: a Settings with nothing touched round-trips to the declared defaults.
	const nw::Row row = nw::rowOf(nw::Settings {});
	for (size_t i = static_cast<size_t>(nw::Param::LowUpRatio); i < static_cast<size_t>(nw::Param::kCount); ++i) {
		CHECK(row[i] == static_cast<float>(nw::kParamDescs[i].def));
	}
}

namespace {

float clamp01(float v) {
	return std::clamp(v, 0.0f, 1.0f);
}

/// WHAT EACH BAND'S COMPRESSOR SHOULD HAVE BEEN HANDED, restated from the law and not read off the device.
struct Quantities {
	std::array<float, 3> ratioUp, ratioDown, attackMs, releaseMs, inputGain;
};

Quantities lawOf(const nw::Settings& s) {
	Quantities q {};
	for (size_t b = 0; b < 3; ++b) {
		const nw::BandParams& band = s.band(b);
		const float ratio = clamp01(band.ratio + (s.ratio - 0.5f));
		q.ratioDown[b] = clamp01(ratio + (band.downRatio - 0.5f));
		q.ratioUp[b] = clamp01(ratio + (band.upRatio - 0.5f));
		const float attack = clamp01(s.attack + (band.attack - 0.5f));
		const float release = clamp01(s.release + (band.release - 0.5f));
		q.attackMs[b] = 0.5f + (std::exp(2.0f * attack) - 1.0f) * 15.0f;
		q.releaseMs[b] = 5.0f + (std::exp(2.0f * release) - 1.0f) * 75.0f;
		q.inputGain[b] = std::pow(10.0f, band.inputDb / 20.0f);
	}
	return q;
}

Quantities seamOf(const nw::Settings& s) {
	nw::Owltt d;
	d.prepare(kRate);
	d.setTarget(s);
	std::vector<float> l(256, 0.1f), r(256, 0.1f);
	d.processStereo(l.data(), r.data(), 256);
	Quantities q {};
	for (size_t b = 0; b < 3; ++b) {
		q.ratioUp[b] = d.ratioUpForTest(b);
		q.ratioDown[b] = d.ratioDownForTest(b);
		q.attackMs[b] = d.attackMsForTest(b);
		q.releaseMs[b] = d.releaseMsForTest(b);
		q.inputGain[b] = d.inputGainTargetForTest(b);
	}
	return q;
}

nw::Settings baseline() {
	nw::Settings s {};
	s.mode = 6.0f;
	s.threshold = 0.4f;
	s.ratio = 0.62f;
	s.attack = 0.3f;
	s.release = 0.2f;
	s.low.ratio = 0.3f;
	s.mid.ratio = 0.5f;
	s.high.ratio = 0.8f;
	return s;
}

void expectNear(const Quantities& got, const Quantities& want) {
	for (size_t b = 0; b < 3; ++b) {
		INFO("band " << b);
		CHECK(got.ratioUp[b] == doctest::Approx(want.ratioUp[b]).epsilon(1e-6));
		CHECK(got.ratioDown[b] == doctest::Approx(want.ratioDown[b]).epsilon(1e-6));
		CHECK(got.attackMs[b] == doctest::Approx(want.attackMs[b]).epsilon(2e-3));
		CHECK(got.releaseMs[b] == doctest::Approx(want.releaseMs[b]).epsilon(2e-3));
		CHECK(got.inputGain[b] == doctest::Approx(want.inputGain[b]).epsilon(1e-5));
	}
}

} // namespace

TEST_CASE("per-band controls: each one moved alone changes exactly the quantity it names, by the law") {
	const nw::Settings base = baseline();
	const Quantities q0 = seamOf(base);
	expectNear(q0, lawOf(base));

	struct Move {
		const char* what;
		float nw::BandParams::* field;
		float value;
		size_t band;
	};
	std::vector<Move> moves;
	for (size_t b = 0; b < 3; ++b) {
		moves.push_back({ "up", &nw::BandParams::upRatio, 0.9f, b });
		moves.push_back({ "down", &nw::BandParams::downRatio, 0.15f, b });
		moves.push_back({ "attack", &nw::BandParams::attack, 0.85f, b });
		moves.push_back({ "release", &nw::BandParams::release, 0.1f, b });
		moves.push_back({ "input", &nw::BandParams::inputDb, 6.0f, b });
	}
	for (const Move& m : moves) {
		INFO(m.what << " on band " << m.band);
		nw::Settings s = base;
		(m.band == 0 ? s.low : (m.band == 1 ? s.mid : s.high)).*m.field = m.value;
		const Quantities got = seamOf(s);
		expectNear(got, lawOf(s));
		// AND ONLY THE NAMED QUANTITY MOVED: every other number is the baseline's, bit for bit.
		const std::string_view what = m.what;
		for (size_t b = 0; b < 3; ++b) {
			const bool mine = b == m.band;
			CHECK((got.ratioUp[b] == q0.ratioUp[b]) == !(mine && what == "up"));
			CHECK((got.ratioDown[b] == q0.ratioDown[b]) == !(mine && what == "down"));
			CHECK((got.attackMs[b] == q0.attackMs[b]) == !(mine && what == "attack"));
			CHECK((got.releaseMs[b] == q0.releaseMs[b]) == !(mine && what == "release"));
			CHECK((got.inputGain[b] == q0.inputGain[b]) == !(mine && what == "input"));
		}
	}
}

TEST_CASE("per-band controls: a trim of the ratio is a trim of its own direction, and the global Ratio still shifts both") {
	nw::Settings s = baseline();
	s.mid.upRatio = 0.7f;
	const Quantities a = seamOf(s);
	CHECK(a.ratioUp[1] == doctest::Approx(clamp01(0.5f + (0.62f - 0.5f)) + 0.2f).epsilon(1e-6));
	CHECK(a.ratioDown[1] == doctest::Approx(0.62f).epsilon(1e-6));
	s.ratio = 0.3f; // the global moves every band and both directions by the same displacement
	const Quantities b = seamOf(s);
	CHECK(b.ratioUp[1] == doctest::Approx(0.3f + 0.2f).epsilon(1e-6));
	CHECK(b.ratioDown[1] == doctest::Approx(0.3f).epsilon(1e-6));
	CHECK(b.ratioDown[0] == doctest::Approx(clamp01(0.3f + (0.3f - 0.5f))).epsilon(1e-6));
}

TEST_CASE("per-band controls: +6 dB of Input raises that band's detector by 6 dB and leaves the other bands' gains alone") {
	// A tone in each band, held, compressing: band thresholds low and ratio up so the gains are not unity.
	Cfg c;
	c.threshold = 0.5f;
	c.ratio = 0.8f;
	c.bandThreshold = { 0.3f, 0.3f, 0.3f };
	const Program in = [] {
		Program p;
		p.l.resize(48000);
		p.r.resize(48000);
		const float tau = 2.0f * std::numbers::pi_v<float>;
		for (size_t i = 0; i < p.l.size(); ++i) {
			const float t = static_cast<float>(i) / kRate;
			p.l[i] = p.r[i] = 0.05f * std::sin(tau * 80.0f * t) + 0.05f * std::sin(tau * 700.0f * t) + 0.05f * std::sin(tau * 6000.0f * t);
		}
		return p;
	}();
	const auto run = [&](float lowDb) {
		nw::Settings s = settingsOf<nw::Settings>(c);
		s.low.inputDb = lowDb;
		nw::Owltt d;
		d.prepare(kRate);
		d.setTarget(s);
		std::vector<float> l = in.l, r = in.r;
		for (size_t at = 0; at < l.size(); at += 480) {
			d.processStereo(&l[at], &r[at], 480);
		}
		return std::array<float, 8> { d.getBandInputLevelLog(0), d.getBandInputLevelLog(1), d.getBandInputLevelLog(2),
			                          d.lastTargetBandGainForTest(0), d.lastTargetBandGainForTest(1),
			                          d.lastTargetBandGainForTest(2), d.inputGainForTest(0), d.inputGainTargetForTest(0) };
	};
	const auto flat = run(0.0f);
	const auto hot = run(6.0f);
	// The detector's level is a natural log of amplitude: 6 dB is ln(10^(6/20)) of it.
	CHECK(hot[0] - flat[0] == doctest::Approx(std::log(std::pow(10.0f, 6.0f / 20.0f))).epsilon(2e-3));
	CHECK(hot[1] == flat[1]);
	CHECK(hot[2] == flat[2]);
	CHECK(hot[4] == flat[4]);
	CHECK(hot[5] == flat[5]);
	// NOT VACUOUS: the hot band's own gain did move (it is compressing harder), and the smoother arrived at the target.
	CHECK(hot[3] != flat[3]);
	CHECK(hot[6] == hot[7]);
	CHECK(hot[7] == doctest::Approx(1.99526f).epsilon(1e-4));
}

namespace {

float softClipLaw(float x, float knee) {
	if (x > knee) {
		return knee + (x - knee) * 0.5f;
	}
	if (x < -knee) {
		return -knee + (x + knee) * 0.5f;
	}
	return x;
}

/// A LOW BAND ALONE: mid and high Levels at zero, ratios at zero (no compression), a low tone of `amp`. The sum is then the
/// low band's, so the clip law can be restated sample for sample against the unclipped render.
std::vector<float> lowOnly(float amp, bool softClip, uint8_t clipOnly) {
	Cfg c;
	c.mode = 6.0f;
	c.ratio = 0.5f;
	c.bandRatio = { 0.5f, 0.5f, 0.5f };
	c.bandLevel = { 0.5f, 0.0f, 0.0f };
	nw::Settings s = settingsOf<nw::Settings>(c);
	s.ratio = 0.0f; // a global at its neutral shifts nothing; at 0 the band ratio 0.5 + (0 - 0.5) = 0: no compression
	s.softClip = softClip;
	s.clipOnly = clipOnly;
	nw::Owltt d;
	d.prepare(kRate);
	d.setTarget(s);
	std::vector<float> l(30000), r(30000);
	const float tau = 2.0f * std::numbers::pi_v<float>;
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = r[i] = amp * std::sin(tau * 100.0f * static_cast<float>(i) / kRate);
	}
	for (size_t at = 0; at < l.size(); at += 480) {
		d.processStereo(&l[at], &r[at], static_cast<int>(std::min<size_t>(480, l.size() - at)));
	}
	return l;
}

} // namespace

TEST_CASE("per-band controls: Clip Off is the identity, Master clips the sum only, Bands each band only, Both each band and then the sum") {
	for (const float amp : { 3.0f, 1.5f }) {
		INFO("amplitude " << amp);
		const auto off = lowOnly(amp, false, 0);
		const auto both = lowOnly(amp, true, 0);
		const auto bands = lowOnly(amp, true, 1);
		const auto master = lowOnly(amp, true, 2);
		float offPeak = 0.0f, dBands = 0.0f, dMaster = 0.0f, dBoth = 0.0f, spread = 0.0f;
		for (size_t i = 24000; i < off.size(); ++i) {
			const float x = off[i];
			offPeak = std::max(offPeak, std::fabs(x));
			// THE LAW, restated: Bands is the 2.0 knee alone; Master is the 1.0 knee alone; Both is the first and then the second.
			dBands = std::max(dBands, std::fabs(bands[i] - softClipLaw(x, 2.0f)));
			dMaster = std::max(dMaster, std::fabs(master[i] - softClipLaw(x, 1.0f)));
			dBoth = std::max(dBoth, std::fabs(both[i] - softClipLaw(softClipLaw(x, 2.0f), 1.0f)));
			spread = std::max(spread, std::fabs(amp > 2.0f ? master[i] - both[i] : master[i] - bands[i]));
		}
		CHECK(offPeak > 1.1f); // the tone really reached the clippers
		CHECK(dBands < 1e-5f);
		CHECK(dMaster < 1e-5f);
		CHECK(dBoth < 1e-5f);
		CHECK(spread > 0.05f); // NOT VACUOUS: Master differs from Both where a band is over its knee, and from Bands where only the sum is over its own
	}
	// A BAND OVER ITS KNEE (peak > 2): Master leaves the band unclipped and clips only the sum; Bands the reverse.
	const auto off = lowOnly(3.0f, false, 0);
	const auto bands = lowOnly(3.0f, true, 1);
	const auto master = lowOnly(3.0f, true, 2);
	float offPeak = 0.0f, bandsPeak = 0.0f, masterPeak = 0.0f;
	for (size_t i = 24000; i < off.size(); ++i) {
		offPeak = std::max(offPeak, std::fabs(off[i]));
		bandsPeak = std::max(bandsPeak, std::fabs(bands[i]));
		masterPeak = std::max(masterPeak, std::fabs(master[i]));
	}
	REQUIRE(offPeak > 2.0f);
	CHECK(bandsPeak == doctest::Approx(2.0f + (offPeak - 2.0f) * 0.5f).epsilon(1e-4));
	CHECK(masterPeak == doctest::Approx(1.0f + (offPeak - 1.0f) * 0.5f).epsilon(1e-4));
}

TEST_CASE("per-band controls: the Clip choice stores by label, an old On is Both, and Both is what On was") {
	const feathers::device::ParamDesc* clip = nullptr;
	for (const auto& p : nw::kParamDescs) {
		if (std::string_view(p.key) == "soft_clip") {
			clip = &p;
		}
	}
	REQUIRE(clip != nullptr);
	REQUIRE(clip->names.size() == 4u);
	CHECK(std::string_view(clip->names[0]) == "Off");
	CHECK(std::string_view(clip->names[1]) == "Master");
	CHECK(std::string_view(clip->names[2]) == "Bands");
	CHECK(std::string_view(clip->names[3]) == "Both");
	CHECK(clip->def == 3.0); // the old On
	REQUIRE(clip->choiceAliases.size() == 1u);
	CHECK(std::string_view(clip->choiceAliases[0].from) == "On");
	CHECK(std::string_view(clip->choiceAliases[0].to) == "Both");

	// A PATCH SAVED BEFORE THIS CHANGE: written by the old device's descriptor (Off|On, 38 controls), loaded by the new one.
	od::Row oldRow {};
	for (size_t i = 0; i < std::size(od::kParamDescs); ++i) {
		oldRow[i] = static_cast<float>(od::kParamDescs[i].def);
	}
	oldRow[static_cast<size_t>(od::Param::SoftClip)] = 1.0f; // On
	oldRow[static_cast<size_t>(od::Param::Threshold)] = 0.35f;
	oldRow[static_cast<size_t>(od::Param::LowRatio)] = 0.7f;
	feathers::rack::RackPatch::Device saved;
	feathers::rack::valuesOfRow(od::kDescriptor, oldRow, oldRow.size(), &saved);
	bool sawOn = false;
	for (const auto& v : saved.values) {
		if (v.key == "soft_clip") {
			sawOn = v.kind == feathers::rack::RackPatch::Value::Kind::Label && v.text == "On";
		}
	}
	REQUIRE(sawOn); // the old file really says "On"

	std::vector<float> loaded(std::size(nw::kParamDescs), 0.0f);
	feathers::rack::LoadReport report;
	feathers::rack::rowOfValues(nw::kDescriptor, saved, loaded, &report, "OWLTT", nullptr);
	CHECK(loaded[static_cast<size_t>(nw::Param::SoftClip)] == 3.0f); // Both, not Master or Off
	for (size_t i = static_cast<size_t>(nw::Param::LowUpRatio); i < static_cast<size_t>(nw::Param::kCount); ++i) {
		CHECK(loaded[i] == static_cast<float>(nw::kParamDescs[i].def)); // every new control neutral
	}
	nw::Row row {};
	std::copy(loaded.begin(), loaded.end(), row.begin());
	const nw::Settings s = nw::stateOf(row);
	CHECK(s.softClip);
	CHECK(s.clipOnly == 0); // both stages, as On always ran

	// AND IT SOUNDS AS BEFORE: the old row through the old device, the loaded row through the new one, to the bit.
	const Program in = programOf(9000, false, 4242u);
	const auto before = render<od::Owltt>(od::stateOf(oldRow), in, 480);
	const auto after = render<nw::Owltt>(s, in, 480);
	CHECK(sameBits(before.first, after.first));
	CHECK(sameBits(before.second, after.second));
}

TEST_CASE("per-band controls: a short row from before them takes the neutral defaults, and renders as the old device did") {
	// THROUGH THE AUDIO OPS, the road a stored device record takes: 38 fields, the rest absent. A zero for an absent trim would
	// be a trim of -0.5, so the absent ones must read their declared default.
	od::Row oldRow {};
	for (size_t i = 0; i < std::size(od::kParamDescs); ++i) {
		oldRow[i] = static_cast<float>(od::kParamDescs[i].def);
	}
	oldRow[static_cast<size_t>(od::Param::Threshold)] = 0.3f;
	oldRow[static_cast<size_t>(od::Param::Ratio)] = 0.8f;
	const auto& ops = *nw::kDescriptor.audio;
	alignas(64) unsigned char storage[sizeof(nw::Owltt) + 64];
	REQUIRE(ops.stateBytes <= sizeof(storage) - 64);
	void* at = storage;
	ops.construct(at);
	ops.prepare(at, kRate, 480);
	// (A NUMBER IS NOT A LABEL: the old On was the value 1, which the four-way choice reads as Master. A file carries labels, and
	// the alias sends "On" to Both; a bare short row is given Both, the value that means what On meant.)
	od::Row shortRow = oldRow;
	shortRow[static_cast<size_t>(od::Param::SoftClip)] = 3.0f;
	ops.setFields(at, shortRow.data(), shortRow.size());
	const Program in = programOf(9000, false, 99u);
	std::vector<float> l = in.l, r = in.r;
	for (size_t i = 0; i < l.size(); i += 480) {
		ops.processStereo(at, &l[i], &r[i], static_cast<int>(std::min<size_t>(480, l.size() - i)));
	}
	ops.destruct(at);
	const auto before = render<od::Owltt>(od::stateOf(oldRow), in, 480);
	CHECK(sameBits(before.first, l));
	CHECK(sameBits(before.second, r));
}

TEST_CASE("per-band controls: every new control non-default survives a save and a fresh instance, and rests at exact zero") {
	nw::Settings s = baseline();
	s.low.upRatio = 0.8f;
	s.low.downRatio = 0.2f;
	s.mid.upRatio = 0.65f;
	s.mid.downRatio = 0.35f;
	s.high.upRatio = 0.1f;
	s.high.downRatio = 0.9f;
	s.low.attack = 0.9f;
	s.low.release = 0.1f;
	s.mid.attack = 0.2f;
	s.mid.release = 0.7f;
	s.high.attack = 0.4f;
	s.high.release = 0.95f;
	s.low.inputDb = -7.5f;
	s.mid.inputDb = 4.0f;
	s.high.inputDb = 12.0f;
	s.softClip = true;
	s.clipOnly = 1;
	const nw::Row row = nw::rowOf(s);
	// By key and label, as a file stores it, into a fresh row.
	feathers::rack::RackPatch::Device dev;
	feathers::rack::valuesOfRow(nw::kDescriptor, row, row.size(), &dev);
	std::vector<float> back(std::size(nw::kParamDescs), 0.0f);
	feathers::rack::LoadReport report;
	feathers::rack::rowOfValues(nw::kDescriptor, dev, back, &report, "OWLTT", nullptr);
	for (size_t i = 0; i < row.size(); ++i) {
		INFO("control " << nw::kParamDescs[i].key);
		CHECK(std::memcmp(&row[i], &back[i], sizeof(float)) == 0);
	}
	CHECK(back[static_cast<size_t>(nw::Param::SoftClip)] == 2.0f); // Bands
	nw::Row loaded {};
	std::copy(back.begin(), back.end(), loaded.begin());
	const nw::Settings t = nw::stateOf(loaded);
	CHECK(t.low.upRatio == s.low.upRatio);
	CHECK(t.high.release == s.high.release);
	CHECK(t.mid.inputDb == s.mid.inputDb);
	CHECK(t.clipOnly == 1);
	// A fresh instance rendering the loaded state is the original's render.
	const Program in = programOf(9000, false, 5u);
	const auto a = render<nw::Owltt>(s, in, 480);
	const auto b = render<nw::Owltt>(t, in, 480);
	CHECK(sameBits(a.first, b.first));
	CHECK(sameBits(a.second, b.second));

	// REST: a burst, then exact silence, with the per-band settings non-default, reaches exact zero.
	nw::Owltt d;
	d.prepare(kRate);
	d.setTarget(s);
	std::vector<float> l(480), r(480);
	for (int block = 0; block < 400; ++block) {
		for (size_t i = 0; i < l.size(); ++i) {
			const float x = block < 20 ? 0.6f * std::sin(0.05f * static_cast<float>(i + 480u * static_cast<unsigned>(block))) : 0.0f;
			l[i] = x;
			r[i] = x;
		}
		d.processStereo(l.data(), r.data(), 480);
		for (size_t i = 0; i < l.size(); ++i) {
			REQUIRE(std::isfinite(l[i]));
		}
	}
	float peak = 0.0f;
	for (size_t i = 0; i < l.size(); ++i) {
		peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
	}
	CHECK(peak == 0.0f);
}
