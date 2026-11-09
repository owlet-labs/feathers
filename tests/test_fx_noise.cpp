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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "dsp/random_events.h"
#include "fx/noise/noise_types.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <vector>

// NOISE (backlog MZ) — asserted by VALUE: the tilt's slope per octave against the target (by its coefficients,
// then by an independent octave-band measurement of what it renders), the impulses' rate by count, the seed's
// determinism, the filter's corner, and the pass-through to the bit at Level 0.

using namespace feathers;
namespace nz = fx::noise;
using P = nz::Param;

namespace {

/// ONE DEVICE, BUILT THE WAY A HOST BUILDS IT, driven through `setFields` and a real row.
struct Dev {
	const device::AudioOps& ops = *nz::kDescriptor.audio;
	void* mem = nullptr;
	nz::Row row = nz::rowOf(nz::Settings {});

	explicit Dev(double rate = 48000.0) {
		mem = feathers::test::heapAlignedAlloc(64, ((ops.stateBytes + 63) / 64) * 64);
		ops.construct(mem);
		ops.prepare(mem, rate, 4096);
		apply();
	}
	~Dev() {
		ops.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Dev(const Dev&) = delete;
	Dev& operator=(const Dev&) = delete;

	Dev& set(P p, float v) {
		row[static_cast<size_t>(p)] = v;
		return *this;
	}
	void apply() { ops.setFields(mem, row.data(), row.size()); }
	void run(std::vector<float>& l, std::vector<float>& r) {
		apply();
		ops.processStereo(mem, l.data(), r.data(), static_cast<int>(l.size()));
	}
	/// RENDER `frames` OF SILENCE in host-sized blocks, returning both ears.
	void render(std::vector<float>& l, std::vector<float>& r, size_t frames, int block = 256) {
		apply();
		l.assign(frames, 0.0f);
		r.assign(frames, 0.0f);
		for (size_t at = 0; at < frames; at += static_cast<size_t>(block)) {
			const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), frames - at));
			ops.processStereo(mem, l.data() + at, r.data() + at, n);
		}
	}
};

std::vector<float> probe(size_t n, uint32_t seed) {
	std::vector<float> out(n);
	uint32_t s = seed;
	for (size_t i = 0; i < n; ++i) {
		s = s * 1664525u + 1013904223u;
		out[i] = 0.3f * std::sin(0.013f * static_cast<float>(i)) + 0.1f * (static_cast<float>(s >> 8) / 16777216.0f - 0.5f);
	}
	return out;
}

/// A LEAST-SQUARES LINE through (x, y): the slope.
double slopeOf(const std::vector<double>& x, const std::vector<double>& y) {
	double mx = 0.0;
	double my = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		mx += x[i];
		my += y[i];
	}
	mx /= static_cast<double>(x.size());
	my /= static_cast<double>(x.size());
	double sxy = 0.0;
	double sxx = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		sxy += (x[i] - mx) * (y[i] - my);
		sxx += (x[i] - mx) * (x[i] - mx);
	}
	return sxy / sxx;
}

/// A CONSTANT-Q BAND: FOUR RBJ BANDPASSES IN A ROW, written here and nowhere in the device — the instrument the
/// rendered slope is read with must not share code with what it measures. FOUR, because one has skirts of only
/// 6 dB an octave, and against violet's +6 the skirt's reach to Nyquist outweighs the band (MEASURED: one section
/// read violet as +3.2 dB per octave). Any constant-Q shape gathers a PSD of f^a as fc^(a+1), so the offset of
/// 3.01 dB per octave holds however sharp the band is.
double bandPowerDb(const std::vector<float>& x, double centreHz, double fs) {
	const double w0 = 2.0 * std::numbers::pi * centreHz / fs;
	const double q = std::sqrt(2.0);
	const double alpha = std::sin(w0) / (2.0 * q);
	const double a0 = 1.0 + alpha;
	const double b0 = alpha / a0;
	const double b2 = -alpha / a0;
	const double a1 = -2.0 * std::cos(w0) / a0;
	const double a2 = (1.0 - alpha) / a0;
	constexpr int kStages = 4;
	double x1[kStages] {}, x2[kStages] {}, y1[kStages] {}, y2[kStages] {};
	double power = 0.0;
	const size_t skip = 9600; // the filters' own settling
	for (size_t i = 0; i < x.size(); ++i) {
		double v = x[i];
		for (int s = 0; s < kStages; ++s) {
			const double y = b0 * v + b2 * x2[s] - a1 * y1[s] - a2 * y2[s];
			x2[s] = x1[s];
			x1[s] = v;
			y2[s] = y1[s];
			y1[s] = y;
			v = y;
		}
		if (i >= skip) {
			power += v * v;
		}
	}
	return 10.0 * std::log10(power / static_cast<double>(x.size() - skip));
}

size_t countAbove(const std::vector<float>& x, float threshold) {
	size_t n = 0;
	for (const float v : x) {
		n += std::fabs(v) > threshold ? 1 : 0;
	}
	return n;
}

} // namespace

TEST_CASE("NOISE: Level 0 and switched off are a bit-identical pass-through, and Level up adds to what passes") {
	const std::vector<float> in = probe(4096, 7);
	// LEVEL 0: the buffer is not written.
	{
		Dev d;
		d.set(P::Level, 0.0f);
		std::vector<float> l = in;
		std::vector<float> r = in;
		d.run(l, r);
		CHECK(std::memcmp(l.data(), in.data(), in.size() * sizeof(float)) == 0);
		CHECK(std::memcmp(r.data(), in.data(), in.size() * sizeof(float)) == 0);
	}
	// SWITCHED OFF FROM THE START, at a Level that would otherwise be heard: the same.
	{
		Dev d;
		d.set(P::Enabled, 0.0f).set(P::Level, 0.5f).apply();
		d.ops.reset(d.mem); // fresh: the enable is landed, not ramping
		std::vector<float> l = in;
		std::vector<float> r = in;
		d.run(l, r);
		CHECK(std::memcmp(l.data(), in.data(), in.size() * sizeof(float)) == 0);
		CHECK(std::memcmp(r.data(), in.data(), in.size() * sizeof(float)) == 0);
	}
	// AND THE PASSING SIGNAL IS NOT TOUCHED WITH LEVEL UP: output minus input is exactly what the device makes from
	// silence with the same seed (to float rounding of the one addition).
	for (const int source : { 0, 1 }) {
		CAPTURE(source);
		Dev withInput;
		Dev fromSilence;
		for (Dev* d : { &withInput, &fromSilence }) {
			d->set(P::Source, static_cast<float>(source)).set(P::Level, 0.5f).set(P::Density, 9.0f);
		}
		std::vector<float> l = in;
		std::vector<float> r = in;
		withInput.run(l, r);
		std::vector<float> nl(in.size(), 0.0f);
		std::vector<float> nr(in.size(), 0.0f);
		fromSilence.run(nl, nr);
		double worst = 0.0;
		double added = 0.0;
		for (size_t i = 0; i < in.size(); ++i) {
			worst = std::max(worst, static_cast<double>(std::fabs((l[i] - in[i]) - nl[i])));
			worst = std::max(worst, static_cast<double>(std::fabs((r[i] - in[i]) - nr[i])));
			added = std::max(added, static_cast<double>(std::fabs(nl[i])));
		}
		CHECK(worst < 1e-6);
		CHECK(added > 0.05); // non-vacuity: something was added
	}
}

TEST_CASE("NOISE: the colour tilt falls the target dB per octave, by its coefficients and by what it renders") {
	for (const float colour : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f }) {
		CAPTURE(colour);
		const double target = nz::slopeDbPerOctave(colour);
		Dev d;
		d.set(P::Colour, colour);
		std::vector<float> l;
		std::vector<float> r;
		d.render(l, r, 64);

		// BY THE INSTALLED COEFFICIENTS: |H| at every third of an octave from 63 Hz to 8 kHz, a line through it, and how
		// far the response strays from that line (the ripple of one-section-per-octave).
		std::vector<double> oct;
		std::vector<double> db;
		for (double o = std::log2(63.0); o <= std::log2(8000.0); o += 1.0 / 3.0) {
			oct.push_back(o);
			db.push_back(20.0 * std::log10(nz::tiltGainForTest(d.mem, std::exp2(o))));
		}
		const double slope = slopeOf(oct, db);
		CAPTURE(slope);
		CHECK(std::fabs(slope - target) < 0.1);
		double mean = 0.0;
		double meanO = 0.0;
		for (size_t i = 0; i < oct.size(); ++i) {
			mean += db[i];
			meanO += oct[i];
		}
		mean /= static_cast<double>(oct.size());
		meanO /= static_cast<double>(oct.size());
		double ripple = 0.0;
		for (size_t i = 0; i < oct.size(); ++i) {
			ripple = std::max(ripple, std::fabs(db[i] - (mean + slope * (oct[i] - meanO))));
		}
		CAPTURE(ripple);
		CHECK(ripple < 0.5);

		// BY WHAT IT RENDERS, read with an instrument written here: octave bands from 250 Hz to 4 kHz. A constant-Q band
		// gathers 3.01 dB more per octave of white than the octave below, so the band powers rise by target + 3.01.
		// The filter is opened wide (a highpass at 20 Hz) so the tilt is the only slope in the signal.
		Dev m;
		m.set(P::Colour, colour)
		    .set(P::FilterType, static_cast<float>(nz::FilterType::Highpass))
		    .set(P::Cutoff, static_cast<float>(nz::kCutoffMinLog2))
		    .set(P::Level, 0.5f);
		m.render(l, r, 48000 * 6);
		std::vector<double> bands;
		std::vector<double> powers;
		for (const double hz : { 250.0, 500.0, 1000.0, 2000.0, 4000.0 }) {
			bands.push_back(std::log2(hz));
			powers.push_back(bandPowerDb(l, hz, 48000.0));
		}
		const double measured = slopeOf(bands, powers) - 20.0 * std::log10(std::sqrt(2.0));
		CAPTURE(measured);
		CHECK(std::fabs(measured - target) < 0.5);
	}
}

TEST_CASE("NOISE: the tilt holds the loudness — every colour has white's power over 20 Hz .. 20 kHz") {
	// THE LEVEL CONTROL MUST MEAN THE SAME AT EVERY COLOUR: the tilt is normalised to white's power over the audible
	// band. Asked of the INSTALLED response, integrated here on a 1 Hz grid — an independent restatement of the
	// device's own 96-point log integral.
	for (const float colour : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f }) {
		CAPTURE(colour);
		Dev d;
		d.set(P::Colour, colour);
		std::vector<float> l;
		std::vector<float> r;
		d.render(l, r, 64);
		double power = 0.0;
		for (double hz = 20.5; hz < 20000.0; hz += 1.0) {
			const double g = nz::tiltGainForTest(d.mem, hz);
			power += g * g;
		}
		const double db = 10.0 * std::log10(power / 19980.0);
		CAPTURE(db);
		CHECK(std::fabs(db) < 0.2);
	}
	// AND ON THE RENDERED OUTPUT, for the two colours with little energy outside that band (violet's lives up to
	// Nyquist and brown's below 20 Hz, which a whole-signal RMS counts and the band does not): Level at unity (0.5),
	// the filter open.
	std::vector<double> rms;
	for (const float colour : { 0.0f, 0.5f }) {
		Dev d;
		d.set(P::Colour, colour)
		    .set(P::FilterType, static_cast<float>(nz::FilterType::Highpass))
		    .set(P::Cutoff, static_cast<float>(nz::kCutoffMinLog2))
		    .set(P::Level, 0.5f);
		std::vector<float> l;
		std::vector<float> r;
		d.render(l, r, 48000 * 2);
		double p = 0.0;
		for (size_t i = 4800; i < l.size(); ++i) {
			p += static_cast<double>(l[i]) * l[i];
		}
		rms.push_back(10.0 * std::log10(p / static_cast<double>(l.size() - 4800)));
	}
	CAPTURE(rms[0]);
	CAPTURE(rms[1]);
	// WHITE IS UNIFORM ON [-1, 1): -4.77 dB. Pink is within a dB of it (MEASURED -1.03: its sub-20 Hz share is cut
	// by the highpass that opens the filter).
	CHECK(std::fabs(rms[0] + 4.77) < 0.3);
	CHECK(std::fabs(rms[1] - rms[0]) < 1.5);
}

TEST_CASE("NOISE: the random-event core makes the rate it is asked for — Regular, Poisson and Clustered by count") {
	constexpr double kRate = 48000.0;
	const auto run = [&](const dsp::RandomEventSettings& s, double seconds, uint64_t seed, std::vector<size_t>* at) {
		dsp::RandomEvents e;
		e.setSampleRate(kRate);
		e.seed(seed);
		size_t n = 0;
		const auto frames = static_cast<size_t>(seconds * kRate);
		for (size_t i = 0; i < frames; ++i) {
			if (e.next(s) != 0.0f) {
				++n;
				if (at != nullptr) {
					at->push_back(i);
				}
			}
		}
		return n;
	};
	SUBCASE("Regular with no jitter is a metronome: every gap the interval, to the sample") {
		dsp::RandomEventSettings s;
		s.timing = dsp::EventTiming::Regular;
		s.jitter = 0.0f;
		s.rateHz = 16.0;
		std::vector<size_t> at;
		CHECK(run(s, 10.0, 3, &at) == 160);
		for (size_t i = 1; i < at.size(); ++i) {
			CHECK(at[i] - at[i - 1] == 3000);
		}
	}
	SUBCASE("Regular with full jitter keeps the count: the displacement never accumulates") {
		dsp::RandomEventSettings s;
		s.timing = dsp::EventTiming::Regular;
		s.jitter = 1.0f;
		s.rateHz = 16.0;
		std::vector<size_t> at;
		const size_t n = run(s, 100.0, 5, &at);
		CHECK(n >= 1599);
		CHECK(n <= 1601);
		bool varied = false;
		for (size_t i = 1; i < at.size(); ++i) {
			varied = varied || at[i] - at[i - 1] != 3000;
		}
		CHECK(varied);
	}
	SUBCASE("Poisson: the count is the rate, and the counts in windows scatter like Poisson (variance = mean)") {
		dsp::RandomEventSettings s;
		s.timing = dsp::EventTiming::Poisson;
		s.rateHz = 50.0;
		std::vector<size_t> at;
		const size_t n = run(s, 200.0, 11, &at);
		CAPTURE(n);
		// 10,000 expected, sigma 100: four sigma.
		CHECK(std::fabs(static_cast<double>(n) - 10000.0) < 400.0);
		std::vector<double> counts(200, 0.0);
		for (const size_t i : at) {
			counts[std::min<size_t>(i / 48000, 199)] += 1.0;
		}
		double mean = 0.0;
		for (const double c : counts) {
			mean += c;
		}
		mean /= 200.0;
		double var = 0.0;
		for (const double c : counts) {
			var += (c - mean) * (c - mean);
		}
		var /= 199.0;
		CAPTURE(var / mean);
		CHECK(var / mean > 0.7);
		CHECK(var / mean < 1.3);
	}
	SUBCASE("Clustered keeps the mean rate and is burstier than Poisson") {
		dsp::RandomEventSettings s;
		s.timing = dsp::EventTiming::Clustered;
		s.cluster = 1.0f;
		s.rateHz = 50.0;
		std::vector<size_t> at;
		const size_t n = run(s, 200.0, 13, &at);
		CAPTURE(n);
		CHECK(std::fabs(static_cast<double>(n) - 10000.0) < 1500.0);
		std::vector<double> counts(400, 0.0);
		for (const size_t i : at) {
			counts[std::min<size_t>(i / 24000, 399)] += 1.0;
		}
		double mean = 0.0;
		for (const double c : counts) {
			mean += c;
		}
		mean /= 400.0;
		double var = 0.0;
		for (const double c : counts) {
			var += (c - mean) * (c - mean);
		}
		var /= 399.0;
		CAPTURE(var / mean);
		CHECK(var / mean > 2.0);
	}
	SUBCASE("hit sizes: Fixed is 1, Uniform spans (0, 1], Exponential is mostly small, Random polarity is half negative") {
		dsp::RandomEventSettings s;
		s.rateHz = 1000.0;
		const auto values = [&](dsp::HitSize size, bool polarity) {
			s.size = size;
			s.randomPolarity = polarity;
			dsp::RandomEvents e;
			e.setSampleRate(kRate);
			e.seed(17);
			std::vector<float> v;
			for (int i = 0; i < 48000 * 4; ++i) {
				const float x = e.next(s);
				if (x != 0.0f) {
					v.push_back(x);
				}
			}
			return v;
		};
		for (const float v : values(dsp::HitSize::Fixed, false)) {
			REQUIRE(v == 1.0f);
		}
		const std::vector<float> uni = values(dsp::HitSize::Uniform, false);
		double mean = 0.0;
		for (const float v : uni) {
			REQUIRE(v > 0.0f);
			REQUIRE(v <= 1.0f);
			mean += v;
		}
		CHECK(mean / static_cast<double>(uni.size()) == doctest::Approx(0.5).epsilon(0.03));
		const std::vector<float> ex = values(dsp::HitSize::Exponential, false);
		size_t full = 0;
		for (const float v : ex) {
			REQUIRE(v > 0.0f);
			REQUIRE(v <= 1.0f);
			full += v == 1.0f ? 1 : 0;
		}
		// P(e >= 1) = e^-3 = 4.98%.
		CHECK(static_cast<double>(full) / static_cast<double>(ex.size()) == doctest::Approx(std::exp(-3.0)).epsilon(0.15));
		const std::vector<float> pol = values(dsp::HitSize::Fixed, true);
		size_t negative = 0;
		for (const float v : pol) {
			REQUIRE(std::fabs(v) == 1.0f);
			negative += v < 0.0f ? 1 : 0;
		}
		CHECK(static_cast<double>(negative) / static_cast<double>(pol.size()) == doctest::Approx(0.5).epsilon(0.05));
	}
	SUBCASE("the draw order is fixed: Hit Size and Polarity move no event in time") {
		dsp::RandomEventSettings a;
		a.rateHz = 40.0;
		dsp::RandomEventSettings b = a;
		b.size = dsp::HitSize::Exponential;
		b.randomPolarity = true;
		std::vector<size_t> atA;
		std::vector<size_t> atB;
		run(a, 20.0, 23, &atA);
		run(b, 20.0, 23, &atB);
		const bool sameTimes = atA == atB;
		CHECK(sameTimes);
	}
}

TEST_CASE("NOISE: impulses through the device come at the rate asked — counted at the output, Regular and Poisson") {
	// A HIGHPASS AT 20 Hz leaves each click a one-sample spike just under its size, so a click is a sample over a half.
	const auto clicks = [](float timing, float density, double seconds) {
		Dev d;
		d.set(P::Source, 1.0f)
		    .set(P::Timing, timing)
		    .set(P::Jitter, 0.0f)
		    .set(P::Density, density)
		    .set(P::FilterType, static_cast<float>(nz::FilterType::Highpass))
		    .set(P::Cutoff, static_cast<float>(nz::kCutoffMinLog2))
		    .set(P::Level, 0.5f)
		    .set(P::Stereo, 0.0f);
		std::vector<float> l;
		std::vector<float> r;
		d.render(l, r, static_cast<size_t>(seconds * 48000.0));
		CHECK(nz::hitRateForTest(d.mem) == doctest::Approx(std::exp2(static_cast<double>(density))));
		return countAbove(l, 0.5f);
	};
	CHECK(clicks(0.0f, 4.0f, 10.0) == 160); // Regular, no jitter, 16 a second: exactly 160
	const size_t poisson = clicks(1.0f, 4.0f, 30.0);
	CAPTURE(poisson);
	CHECK(std::fabs(static_cast<double>(poisson) - 480.0) < 4.0 * std::sqrt(480.0));
}

TEST_CASE("NOISE: a synced density is one hit per division at the host's tempo, 120 BPM without one") {
	Dev d;
	d.set(P::Source, 1.0f).set(P::Sync, 13.0f); // 1 + index of "1/4" (12)
	REQUIRE(std::string(nz::kSyncNames[13]) == "1/4");
	std::vector<float> l;
	std::vector<float> r;
	d.render(l, r, 64);
	CHECK(nz::hitRateForTest(d.mem) == doctest::Approx(2.0)); // no clock: 0.5 s a beat
	d.ops.setTransportClock(d.mem, device::TransportClock::fromSecondsPerBeat(1.0));
	d.render(l, r, 64);
	CHECK(nz::hitRateForTest(d.mem) == doctest::Approx(1.0));
	// AND DENSITY IS NOT READ WHILE SYNCED.
	d.set(P::Density, 9.0f);
	d.render(l, r, 64);
	CHECK(nz::hitRateForTest(d.mem) == doctest::Approx(1.0));
}

TEST_CASE("NOISE: a Stable seed is the same sound every time; another seed, or Free, is not") {
	for (const int source : { 0, 1 }) {
		CAPTURE(source);
		const auto render = [&](Dev& d) {
			std::vector<float> l;
			std::vector<float> r;
			d.render(l, r, 9600, 64);
			l.insert(l.end(), r.begin(), r.end());
			return l;
		};
		Dev a;
		Dev b;
		Dev c;
		for (Dev* d : { &a, &b, &c }) {
			d->set(P::Source, static_cast<float>(source)).set(P::Density, 8.0f).set(P::Seed, 7.0f);
		}
		c.set(P::Seed, 8.0f);
		const std::vector<float> ra = render(a);
		const bool sameInstances = ra == render(b); // two instances, one seed: the same bits
		CHECK(sameInstances);
		const bool sameAsOtherSeed = ra == render(c); // another seed: another pattern
		CHECK_FALSE(sameAsOtherSeed);
		// RESET IS "AS IF JUST SWITCHED ON": the pattern starts again.
		a.ops.reset(a.mem);
		const bool sameAfterReset = ra == render(a);
		CHECK(sameAfterReset);
		// FREE: deterministic at birth (two instances built alike render alike, so a patch renders the same bits on
		// every host), but a reset does not restart it — the next note hears the stream where the last one left it.
		Dev f;
		Dev g;
		for (Dev* d : { &f, &g }) {
			d->set(P::Source, static_cast<float>(source)).set(P::Density, 8.0f).set(P::SeedMode, 1.0f);
		}
		const std::vector<float> rf = render(f);
		const bool freeAlike = rf == render(g);
		CHECK(freeAlike);
		const bool freeIsNotStable = rf == ra;
		CHECK_FALSE(freeIsNotStable);
		f.ops.reset(f.mem);
		const bool freeRestarted = rf == render(f);
		CHECK_FALSE(freeRestarted);
	}
}

TEST_CASE("NOISE: the filter's corner is Cutoff, moved by Key Track from C4") {
	const auto corner = [](float cutoffHz, float keyTrack, float note) {
		Dev d;
		d.set(P::Cutoff, std::log2(cutoffHz)).set(P::KeyTrack, keyTrack);
		d.ops.setNoteContext(d.mem, device::NoteContext { note, true });
		std::vector<float> l;
		std::vector<float> r;
		d.render(l, r, 4800); // past the ramp
		return nz::filterCornerForTest(d.mem);
	};
	CHECK(corner(1000.0f, 0.0f, 84.0f) == doctest::Approx(1000.0).epsilon(1e-4));
	CHECK(corner(1000.0f, 1.0f, 72.0f) == doctest::Approx(2000.0).epsilon(1e-4));
	CHECK(corner(1000.0f, 0.5f, 48.0f) == doctest::Approx(1000.0 / std::sqrt(2.0)).epsilon(1e-4));
	CHECK(corner(1000.0f, 1.0f, 60.0f) == doctest::Approx(1000.0).epsilon(1e-4));

	// AND THE RESPONSE AT THE CORNER: the lowpass is 1/k there, the bandpass is unity at its centre.
	Dev d;
	d.set(P::Cutoff, std::log2(1000.0f)).set(P::Resonance, 0.5f);
	std::vector<float> l;
	std::vector<float> r;
	d.render(l, r, 4800);
	const double k = 2.0 - 1.9 * 0.5;
	CHECK(nz::filterGainForTest(d.mem, 1000.0) == doctest::Approx(1.0 / k).epsilon(1e-3));
	d.set(P::FilterType, static_cast<float>(nz::FilterType::Bandpass));
	d.render(l, r, 64);
	CHECK(nz::filterGainForTest(d.mem, 1000.0) == doctest::Approx(1.0).epsilon(1e-3));
	CHECK(nz::filterGainForTest(d.mem, 100.0) < 0.15); // a decade below, at Q 0.95
}

TEST_CASE("NOISE: holdsTail is true while it can add sound, false at Level 0 or switched off") {
	Dev d;
	REQUIRE(d.ops.holdsTail != nullptr);
	CHECK(d.ops.holdsTail(d.mem));
	d.set(P::Level, 0.0f).apply();
	CHECK_FALSE(d.ops.holdsTail(d.mem));
	d.set(P::Level, 0.3f).set(P::Enabled, 0.0f).apply();
	CHECK_FALSE(d.ops.holdsTail(d.mem));
	d.set(P::Enabled, 1.0f).set(P::Source, 1.0f).set(P::Density, nz::kDensityMinLog2).apply();
	CHECK(d.ops.holdsTail(d.mem)); // a sparse click stream: silent blocks, more to come
}

TEST_CASE("NOISE: every control's liveness follows its gate chain") {
	nz::Row row = nz::rowOf(nz::Settings {});
	const auto live = [&row](P p) { return nz::liveIn(p, [&row](size_t i) { return static_cast<double>(row[i]); }); };
	// NOISE: Colour yes, every impulse control no — Jitter included, though Timing's own gate is Source.
	CHECK(live(P::Colour));
	for (const P p : { P::Density, P::Sync, P::Timing, P::Jitter, P::Cluster, P::HitSize, P::Polarity }) {
		CHECK_FALSE(live(p));
	}
	row[static_cast<size_t>(P::Timing)] = 0.0f; // Regular, still under Noise
	CHECK_FALSE(live(P::Jitter));
	row[static_cast<size_t>(P::Source)] = 1.0f;
	CHECK_FALSE(live(P::Colour));
	CHECK(live(P::Jitter));
	CHECK_FALSE(live(P::Cluster));
	CHECK(live(P::Density));
	row[static_cast<size_t>(P::Sync)] = 5.0f;
	CHECK_FALSE(live(P::Density));
	CHECK(live(P::Seed));
	row[static_cast<size_t>(P::SeedMode)] = 1.0f;
	CHECK_FALSE(live(P::Seed));
}

TEST_CASE("NOISE: under Free, two voices started at once on different notes play uncorrelated noise") {
	// A CHORD OF IDENTICAL NOISES SUMS COHERENTLY (+6 dB a doubling) — so per voice, under Free, each start is turned onto
	// its own streams. Two fresh voice instances, born alike, each reset (a note on) and handed its note, as the host does.
	const auto voice = [](float pitch, std::vector<float>& l) {
		Dev d;
		d.set(P::SeedMode, 1.0f).set(P::Colour, 0.0f).set(P::Stereo, 0.0f).set(P::Level, 0.5f).apply();
		d.ops.reset(d.mem);
		d.ops.setNoteContext(d.mem, device::NoteContext { pitch, true });
		std::vector<float> r;
		d.render(l, r, 48000);
	};
	const auto correlation = [](const std::vector<float>& a, const std::vector<float>& b) {
		double ab = 0.0, aa = 0.0, bb = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			ab += static_cast<double>(a[i]) * b[i];
			aa += static_cast<double>(a[i]) * a[i];
			bb += static_cast<double>(b[i]) * b[i];
		}
		return ab / std::sqrt(aa * bb);
	};
	std::vector<float> c4;
	std::vector<float> e4;
	voice(60.0f, c4);
	voice(64.0f, e4);
	const double rho = correlation(c4, e4);
	CAPTURE(rho);
	// 48,000 samples of independent noise through an open lowpass: |rho| ~ 1/sqrt(N) = 0.005; the bound is four times that.
	CHECK(std::fabs(rho) < 0.02);
	// NON-VACUITY: the instrument sees correlation where there is some — the same note twice is the same noise.
	std::vector<float> again;
	voice(60.0f, again);
	CHECK(correlation(c4, again) > 0.999);
}

TEST_CASE("NOISE: under Free, a unison of four voices on ONE note plays four uncorrelated noises") {
	// THE COMMON CASE, NOT AN EDGE: voices started together on one note (a unison, a retrigger). The pitch cannot tell
	// them apart, so the host's voice slot (`AudioOps::setVoice`) does. Every pair of the four must be uncorrelated.
	REQUIRE(nz::kDescriptor.audio->setVoice != nullptr);
	const auto voice = [](size_t slot, std::vector<float>& l) {
		Dev d;
		d.ops.setVoice(d.mem, slot);
		CHECK(nz::voiceForTest(d.mem) == static_cast<int64_t>(slot));
		d.set(P::SeedMode, 1.0f).set(P::Colour, 0.0f).set(P::Stereo, 0.0f).set(P::Level, 0.5f).apply();
		d.ops.reset(d.mem);
		d.ops.setNoteContext(d.mem, device::NoteContext { 60.0f, true });
		std::vector<float> r;
		d.render(l, r, 48000);
	};
	const auto correlation = [](const std::vector<float>& a, const std::vector<float>& b) {
		double ab = 0.0, aa = 0.0, bb = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			ab += static_cast<double>(a[i]) * b[i];
			aa += static_cast<double>(a[i]) * a[i];
			bb += static_cast<double>(b[i]) * b[i];
		}
		return ab / std::sqrt(aa * bb);
	};
	std::vector<std::vector<float>> out(4);
	for (size_t v = 0; v < out.size(); ++v) {
		voice(v, out[v]);
	}
	for (size_t a = 0; a < out.size(); ++a) {
		for (size_t b = a + 1; b < out.size(); ++b) {
			CAPTURE(a);
			CAPTURE(b);
			const double rho = correlation(out[a], out[b]);
			CAPTURE(rho);
			CHECK(std::fabs(rho) < 0.02);
		}
	}
	// NON-VACUITY: the same slot on the same note is the same noise — deterministic, so a bounce repeats.
	std::vector<float> again;
	voice(2, again);
	CHECK(correlation(out[2], again) > 0.999);
}
