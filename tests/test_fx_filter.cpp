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

// The multimode filter with no plugin around it, which is the point of it being a module.
//
// Four claims are load-bearing and each has a case that fails without it:
//
//   - OFF IS THE IDENTITY, to the bit. tests/test_synth.cpp:147 checks left == right on every render
//     it does, and every existing case there renders through a chain that now contains this device.
//     A default that was a unity-gain pass rather than a return would move bits everywhere.
//   - THE BLOCK SIZE CANNOT REACH THE AUDIO. Same file, the blocks-of-1-and-37 case. A coefficient
//     smoother stepped per block forks it, so the smoother here is measured mid-ramp at four block
//     sizes and must agree bit for bit.
//   - THE CURVE ON THE CARD IS THE FILTER THAT PLAYS. The response evaluator is a closed form and
//     the filter is a sample loop; they are checked against each other by measuring the actual
//     impulse response, and separately against the vendor's own per-mode formulas.
//   - THE MODULE INCLUDES NOTHING FROM src/plugin. Read off the sources themselves, because a
//     boundary nobody checks lasts one busy afternoon.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/filter/multimode_filter.h"
#include "device/registry.h"
#include "spectrum.h"

#include "fx/filter/cytomic_svf.h"

#include <algorithm>
#include <array>

#include "dsp/fast_math_approx.h"
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <fstream>
#include <new>
#include <numbers>
#include <string>
#include <vector>

namespace {

using namespace feathers::fx::filter;

constexpr double kSampleRate = 48000.0;

/// A deterministic noise source. Nothing here needs randomness — it needs the SAME signal every run,
/// with energy at every frequency.
std::vector<float> noise(size_t n, uint32_t seed = 0x5eed1234u) {
	std::vector<float> out(n);
	uint32_t state = seed;
	for (size_t i = 0; i < n; i++) {
		state = state * 1664525u + 1013904223u;
		out[i] = static_cast<float>(static_cast<int32_t>(state >> 8) - (1 << 23)) / static_cast<float>(1 << 23);
	}
	return out;
}

std::vector<float> impulse(size_t n) {
	std::vector<float> out(n, 0.0f);
	out[0] = 1.0f;
	return out;
}

Settings lowpassAt(double hz) {
	Settings s;
	s.enabled = true;
	s.type = Type::Lowpass;
	s.cutoffLog2Hz = static_cast<float>(std::log2(hz));
	s.resonance = 0.3f;
	return s;
}

/// Run `input` through a filter in blocks of `block`, switching to `after` at frame `changeAt` — the
/// way a host delivers automation, and the way the Rig in test_synth.cpp cuts a render at an event.
/// `changeAt` beyond the end means the settings never move.
std::vector<float> render(const Settings& before, const Settings& after, size_t changeAt,
                          const std::vector<float>& input, int block) {
	MultimodeFilter filter;
	filter.prepare(kSampleRate);
	filter.setTarget(before);

	std::vector<float> left = input;
	std::vector<float> right = input;
	size_t at = 0;
	bool changed = false;
	while (at < input.size()) {
		size_t until = std::min(input.size(), at + static_cast<size_t>(block));
		if (!changed && changeAt > at && changeAt < until) {
			until = changeAt;
		}
		if (!changed && at >= changeAt) {
			filter.setTarget(after);
			changed = true;
		}
		filter.processStereo(left.data() + at, right.data() + at, static_cast<int>(until - at));
		at = until;
	}
	CHECK(left == right); // one filter, two lanes, and they must not drift — see test_synth.cpp:147
	return left;
}

std::vector<float> render(const Settings& s, const std::vector<float>& input, int block) {
	return render(s, s, input.size() + 1, input, block);
}

/// |H(f)| measured off an impulse response — the discrete-time transform evaluated at one frequency,
/// unwindowed, because the transform of an impulse response IS the transfer function and a window
/// would smear it.
double measuredGainAt(const std::vector<float>& ir, double freqHz) {
	double re = 0.0;
	double im = 0.0;
	const double cyclesPerSample = freqHz / kSampleRate;
	for (size_t i = 0; i < ir.size(); i++) {
		const double angle =
		    2.0 * std::numbers::pi * std::fmod(cyclesPerSample * static_cast<double>(i), 1.0);
		re += static_cast<double>(ir[i]) * std::cos(angle);
		im -= static_cast<double>(ir[i]) * std::sin(angle);
	}
	return std::sqrt(re * re + im * im);
}

double dB(double ratio) {
	return 20.0 * std::log10(std::max(ratio, 1e-30));
}

/// What fraction of an impulse response's energy is still in its last eighth.
///
/// A MEASUREMENT'S OWN PRECONDITION, and it exists because the widened grid failed on one without
/// it. A transform of a TRUNCATED impulse response is not the filter's response — it is the
/// response of the filter's first N samples — and the two part company exactly where a filter rings
/// longest: low cutoff, high resonance, a second stage pushed lower still. That combination read
/// 0.33 dB off at 16384 samples and 0.002 dB off at 32768, and the tempting fix was to widen the
/// tolerance, which would have blinded the case to the real disagreements it exists to find.
///
/// So the window is asked whether it was long enough, and a reading is only believed if it was.
double tailEnergyFraction(const std::vector<float>& ir) {
	double total = 0.0;
	double tail = 0.0;
	for (size_t i = 0; i < ir.size(); i++) {
		const double energy = static_cast<double>(ir[i]) * static_cast<double>(ir[i]);
		total += energy;
		if (i >= ir.size() * 7 / 8) {
			tail += energy;
		}
	}
	return total > 0.0 ? tail / total : 0.0;
}

feathers::fx::filter::CytomicSVF::Mode vendorModeOf(Type t) {
	using Mode = feathers::fx::filter::CytomicSVF::Mode;
	switch (t) {
	case Type::Lowpass:
		return Mode::Lowpass;
	case Type::Highpass:
		return Mode::Highpass;
	case Type::Bandpass:
		return Mode::Bandpass;
	case Type::Notch:
		return Mode::Notch;
	case Type::Peak:
		return Mode::Peak;
	case Type::LowShelf:
		return Mode::LowShelf;
	case Type::HighShelf:
		return Mode::HighShelf;
	// THE COMB HAS NO SVF MODE, because a feedback delay is not a two-pole section at all. This
	// helper exists to check our evaluator against the kernel's closed form, and there is nothing to
	// check the comb against; its cases measure an impulse response instead. Listed rather than
	// defaulted so a new model is a compile error here rather than a silent comparison against a Bell.
	case Type::Comb:
	case Type::Brickwall: // nor the brickwall; its own cases measure it (backlog NL)
	case Type::Bell:
	case Type::kCount:
		break;
	}
	return Mode::Bell;
}

} // namespace

TEST_CASE("the descriptors are a contract a host could build a table from") {
	const auto descs = std::span<const feathers::fx::ParamDesc>(kParamDescs);
	REQUIRE(descs.size() == static_cast<size_t>(Param::kCount));

	for (const auto& d : descs) {
		CAPTURE(std::string(d.name));
		CHECK(d.name != nullptr);
		CHECK(d.min < d.max);
		CHECK(d.def >= d.min);
		CHECK(d.def <= d.max);
		// A choice with a range wider than its name list is a menu entry a host can select and this
		// module cannot name.
		if (d.unit == feathers::fx::ParamUnit::Choice) {
			CHECK(d.stepped);
			CHECK(d.names.size() == static_cast<size_t>(d.max - d.min) + 1);
		}
		else {
			CHECK(d.names.empty());
		}
	}

	// **THE DEFAULT IS ON — every effect defaults to on when it is added.** This read `== 0.0` and called it *"the whole bypass argument in one number"*; the
	// bypass is unchanged (a switched-off filter still returns without touching the buffer) and what
	// moved is which side of it a device starts on. The registry-wide sweep in
	// `test_gui_widgets_dahdsr.cpp` is where the rule lives; this is the filter agreeing with it.
	CHECK(descs[static_cast<size_t>(Param::Enabled)].def == 1.0);
	// Both macros are neutral at their default, in every type — the invariant that lets the dropdown
	// move under them.
	CHECK(descs[static_cast<size_t>(Param::Macro1)].def == 0.0);
	CHECK(descs[static_cast<size_t>(Param::Macro2)].def == 0.0);
	CHECK(descs[static_cast<size_t>(Param::Drive)].def == 0.0);

	// The traits the lane reads. By design the filter has no expanded view, and declaring so is how
	// it costs nothing.
	CHECK(kTraits.hasCard);
	CHECK_FALSE(kTraits.hasExpanded);
	CHECK(std::string(kTraits.tag).size() == 4);
}

TEST_CASE("every type names both its macros, and no macro is left meaning nothing") {
	for (int i = 0; i < static_cast<int>(Type::kCount); i++) {
		const auto type = static_cast<Type>(i);
		CAPTURE(i);
		CAPTURE(std::string(kTypeNames[i]));
		const TypeTraits& t = traitsOf(type);
		CHECK(t.macro1.label != nullptr);
		CHECK(t.macro2.label != nullptr);
		CHECK(std::string(t.macro1.label) != std::string(t.macro2.label));

		// A morph partner must share the pole family, or the blend is a crossfade between two
		// different filters rather than a walk across one filter's output mix. Bell is the one mode
		// whose k is divided by the gain, so it can never be a partner.
		for (const MacroMap& m : {t.macro1, t.macro2}) {
			if (m.kind == MacroKind::Morph) {
				CHECK(m.morphNeg != Type::Bell);
				CHECK(m.morphPos != Type::Bell);
				CHECK(m.morphNeg != m.morphPos);
			}
		}
	}
}

TEST_CASE("off is a return, not a filter set to neutral") {
	const std::vector<float> input = noise(2048);

	Settings off;
	off.enabled = false;
	const std::vector<float> untouched = render(off, input, 64);
	// Bit-exact, not close. Every case in test_synth.cpp renders through this device now.
	CHECK(untouched == input);

	// KNOWN-BAD: the same buffer with the device switched on must NOT come back unchanged, or the
	// case above is passing because nothing happens either way.
	Settings on = lowpassAt(800.0);
	const std::vector<float> filtered = render(on, input, 64);
	CHECK(filtered != input);
}

TEST_CASE("the block size cannot reach the audio, with the filter engaged and mid-ramp") {
	// The settings MOVE partway through, so the smoother is caught in the middle of a ramp at every
	// block size rather than sitting on its target where it could not disagree with itself.
	const std::vector<float> input = noise(4096);
	const Settings before = lowpassAt(4000.0);
	Settings after = lowpassAt(300.0);
	after.resonance = 0.8f;
	after.macro1 = 0.6f;
	after.macro2 = -0.4f;
	after.drive = 0.5f;

	const std::vector<float> reference = render(before, after, 700, input, 256);
	for (int block : {1, 37, 128, 512}) {
		CAPTURE(block);
		CHECK(render(before, after, 700, input, block) == reference);
	}
}

TEST_CASE("a lowpass takes the top off, and a highpass takes the bottom off") {
	constexpr size_t kFrames = 16384;
	const std::vector<float> input = noise(kFrames);
	const feathers::test::Window win(static_cast<int>(kFrames));
	const auto dry = feathers::test::amplitudeSpectrum(input, win);

	const auto binOf = [](double hz) {
		return static_cast<size_t>(std::lround(hz * static_cast<double>(kFrames) / kSampleRate));
	};
	const auto bandEnergy = [](const std::vector<double>& spec, size_t lo, size_t hi) {
		double acc = 0.0;
		for (size_t k = lo; k <= hi && k < spec.size(); k++) {
			acc += spec[k] * spec[k];
		}
		return acc;
	};

	SUBCASE("lowpass at 300 Hz") {
		const auto wet = feathers::test::amplitudeSpectrum(render(lowpassAt(300.0), input, 128), win);
		const double dropHigh = dB(std::sqrt(bandEnergy(wet, binOf(6000), binOf(12000))
		                                     / bandEnergy(dry, binOf(6000), binOf(12000))));
		const double dropLow = dB(std::sqrt(bandEnergy(wet, binOf(40), binOf(120))
		                                    / bandEnergy(dry, binOf(40), binOf(120))));
		CAPTURE(dropHigh);
		CAPTURE(dropLow);
		// Two poles from 300 Hz to 9 kHz is five octaves at 12 dB, so this is a loose floor under a
		// very large number rather than a tight measurement.
		CHECK(dropHigh < -50.0);
		CHECK(dropLow > -1.0);
	}

	SUBCASE("highpass at 3 kHz") {
		Settings hp = lowpassAt(3000.0);
		hp.type = Type::Highpass;
		const auto wet = feathers::test::amplitudeSpectrum(render(hp, input, 128), win);
		const double dropLow = dB(std::sqrt(bandEnergy(wet, binOf(40), binOf(120))
		                                    / bandEnergy(dry, binOf(40), binOf(120))));
		const double keepHigh = dB(std::sqrt(bandEnergy(wet, binOf(9000), binOf(15000))
		                                     / bandEnergy(dry, binOf(9000), binOf(15000))));
		CAPTURE(dropLow);
		CAPTURE(keepHigh);
		CHECK(dropLow < -50.0);
		CHECK(keepHigh > -1.0);
	}
}

TEST_CASE("the evaluator agrees with the vendor's own closed form, everywhere it is defined") {
	// THE CHEAP HALF OF THE CURVE'S VERIFICATION, and it is run EXHAUSTIVELY because it costs
	// arithmetic and nothing else. The vendor carries a separate formula per mode and hardcodes 48 kHz
	// in it; this module carries one formula for all of them so that a MORPHED mix can be drawn at
	// all. Wherever both are defined they must be the same number.
	//
	// WIDENED FROM ELEVEN HAND-PICKED SETTINGS, because eleven waypoints for the filter curve might
	// not be enough. Eleven chosen points can only find what somebody suspected; a grid finds
	// what nobody did. Every type, five cutoffs spanning the range, seven resonances INCLUDING the
	// 0.98 clamp and a value past it, at twenty-four log-spaced frequencies: 6720 comparisons, and the
	// whole case is milliseconds because no audio is rendered.
	//
	// The macros are absent here by necessity rather than by choice — the vendor has no notion of
	// them, so there is nothing to compare against. They are covered in the measured half below, which
	// is the only place they CAN be covered.
	constexpr double kCutoffs[] = {50.0, 200.0, 1000.0, 5000.0, 15000.0};
	// UP TO THE CLAMP AND NOT PAST IT, which is not a convenience — it is the boundary of where the
	// vendor's formula describes the vendor's own filter. See the case below: above 0.98 the two part
	// company by 4.3 dB, and it is the FORMULA that is wrong there, not the filter and not this
	// module. Comparing against it there would be pinning somebody else's oversight.
	constexpr double kResonances[] = {0.0, 0.25, 0.5, 0.75, 0.9, 0.95, 0.98};

 	int compared = 0;
	for (int i = 0; i < static_cast<int>(Type::kCount); i++) {
		const auto type = static_cast<Type>(i);
		const std::string typeName = kTypeNames[i];
		CAPTURE(typeName);
		// The kernel's closed form only describes the SVF family. The comb has no CytomicSVF
		// equivalent — skip it here.
		if (type == Type::Comb || type == Type::Brickwall) { // nor the brickwall (backlog NL)
			continue;
		}
		for (double cutoff : kCutoffs) {
			for (double res : kResonances) {
				Settings s = lowpassAt(cutoff);
				s.type = type;
				s.resonance = static_cast<float>(res);
				CAPTURE(cutoff);
				CAPTURE(res);

				for (int k = 0; k < 24; k++) {
					const double hz = 20.0 * std::pow(1000.0, k / 23.0);
					const double mine = gainAt(s, kSampleRate, hz);
					const double theirs = static_cast<double>(feathers::fx::filter::CytomicSVFGainAt(
					    vendorModeOf(type), static_cast<float>(cutoff), static_cast<float>(res), 1.0f,
					    static_cast<float>(hz)));
					// A null is not a disagreement. A notch sits exactly on its own corner, where both
					// formulas compute zero and their leftover rounding residues can be two hundred
					// decibels apart — two spellings of "nothing", and nothing an eye could tell apart.
					if (dB(mine) < -50.0 && dB(theirs) < -50.0) {
						continue;
					}
					CAPTURE(hz);
					CAPTURE(dB(mine));
					CAPTURE(dB(theirs));
					REQUIRE(std::abs(dB(mine) - dB(theirs)) < 0.02);
					compared++;
				}
			}
		}
	}
	// The grid really was walked — a loop that skipped everything would pass every check in it.
	CHECK(compared > 5000);
}

TEST_CASE("above the resonance clamp the vendor's formula stops describing the vendor's filter") {
	// FOUND BY WIDENING THE GRID, which is exactly what a grid is for:
	// eleven hand-picked settings all sat at one resonance, so no amount of staring at them could have
	// reached this. The first run of the widened case failed at Lowpass / 50 Hz / res 1.0, and the
	// failure was real — in the vendor's helper.
	//
	// CytomicSVF::setCoeff CLAMPS resonance to 0.98 before it becomes a coefficient (CytomicSVF.h:110).
	// CytomicSVFGainAt, the analytic helper beside it, does not: it takes res straight into k = 2 − 2r,
	// so at r = 1 it describes a filter with k = 0 — infinite Q — that setCoeff will never build. This
	// module's evaluator clamps the way setCoeff clamps, because its whole job is to draw the filter
	// that plays rather than the one the algebra allows.
	//
	// Which is right is not a matter of opinion, and this case settles it against the ACTUAL FILTER.
	constexpr size_t kIrLength = 65536; // long enough for a Q of 25 to ring itself out
	constexpr double kCutoff = 1000.0;

	Settings s = lowpassAt(kCutoff);
	s.resonance = 1.0f; // past the clamp, which is a place a knob at maximum genuinely reaches

	const std::vector<float> ir = render(s, impulse(kIrLength), 64);

	// The peak is where the disagreement lives, so that is where it is measured.
	double best = 0.0;
	double bestHz = kCutoff;
	for (int i = 0; i < 400; i++) {
		const double hz = kCutoff * std::pow(2.0, -0.5 + i / 399.0);
		const double g = gainAt(s, kSampleRate, hz);
		if (g > best) {
			best = g;
			bestHz = hz;
		}
	}

	const double measured = dB(measuredGainAt(ir, bestHz));
	const double mine = dB(gainAt(s, kSampleRate, bestHz));
	const double vendor = dB(static_cast<double>(feathers::fx::filter::CytomicSVFGainAt(
	    feathers::fx::filter::CytomicSVF::Mode::Lowpass, static_cast<float>(kCutoff), 1.0f, 1.0f,
	    static_cast<float>(bestHz))));
	CAPTURE(bestHz);
	CAPTURE(measured);
	CAPTURE(mine);
	CAPTURE(vendor);

	// THIS MODULE FOLLOWS THE FILTER.
	CHECK(std::abs(mine - measured) < 0.1);
	// THE VENDOR'S HELPER DOES NOT, up here — by whole decibels, not by rounding. Asserted rather than
	// merely noted, so that a vendor update which fixes it tells us instead of passing silently.
	CHECK(std::abs(vendor - measured) > 1.0);

	// And the clamp is a clamp: asking for more than 0.98 gets exactly 0.98, in the drawn curve as in
	// the filter, so a knob at maximum and a knob just under it do not sound or look different.
	Settings clamped = s;
	clamped.resonance = 0.98f;
	for (double hz : {100.0, 500.0, 1000.0, 4000.0}) {
		CAPTURE(hz);
		CHECK(gainAt(s, kSampleRate, hz) == doctest::Approx(gainAt(clamped, kSampleRate, hz)));
	}
}

TEST_CASE("the curve on the card is the filter that plays") {
	// The claim the card rests on, measured rather than argued: run an impulse through the real
	// sample loop, transform it, and compare against what gainAt() would have drawn — including with
	// both macros engaged, which is the part no vendor formula covers.
	constexpr size_t kIrLength = 32768;

	struct Case {
		const char* what;
		Type type;
		double cutoffHz;
		float resonance;
		float macro1;
		float macro2;
	};
	const Case cases[] = {
	    {"lowpass, plain", Type::Lowpass, 700.0, 0.4f, 0.0f, 0.0f},
	    {"lowpass, spread up", Type::Lowpass, 700.0, 0.4f, 0.8f, 0.0f},
	    {"lowpass, spread down", Type::Lowpass, 700.0, 0.4f, -0.8f, 0.0f},
	    {"lowpass, morphed toward band", Type::Lowpass, 700.0, 0.4f, 0.0f, 0.7f},
	    {"lowpass, both macros", Type::Lowpass, 700.0, 0.4f, 0.5f, -0.5f},
	    {"highpass, morphed", Type::Highpass, 1500.0, 0.5f, 0.3f, 0.6f},
	    {"bandpass, widened", Type::Bandpass, 1200.0, 0.6f, 0.5f, 0.0f},
	    {"peak, tilted", Type::Peak, 1000.0, 0.3f, 0.0f, -0.6f},
	    {"low shelf, boosted", Type::LowShelf, 600.0, 0.2f, 0.7f, 0.0f},
	    {"high shelf, cut", Type::HighShelf, 2500.0, 0.2f, -0.7f, 0.0f},
	    {"bell, boosted and narrowed", Type::Bell, 1400.0, 0.5f, 0.6f, -0.5f},
	};

	for (const Case& c : cases) {
		CAPTURE(std::string(c.what));
		Settings s = lowpassAt(c.cutoffHz);
		s.type = c.type;
		s.resonance = c.resonance;
		s.macro1 = c.macro1;
		s.macro2 = c.macro2;

		const std::vector<float> ir = render(s, impulse(kIrLength), 64);
		for (double hz : {50.0, 200.0, 700.0, 1400.0, 3000.0, 8000.0, 16000.0}) {
			const double predicted = gainAt(s, kSampleRate, hz);
			if (dB(predicted) < -50.0) {
				continue; // a null is where a float impulse response and a double formula stop agreeing
			}
			CAPTURE(hz);
			CAPTURE(dB(predicted));
			CAPTURE(dB(measuredGainAt(ir, hz)));
			CHECK(std::abs(dB(measuredGainAt(ir, hz)) - dB(predicted)) < 0.1);
		}
	}
}

TEST_CASE("the measured filter matches the drawn curve across a grid, not just at eleven points") {
	// THE EXPENSIVE HALF, widened the same way. The case above this one compares two FORMULAS; this
	// one compares the formula against the filter that actually ran, by driving an impulse through the
	// real sample loop and transforming what came out. That is the only half where the MACROS can be
	// checked at all — the vendor has no notion of them — so it is the half that has to cover them.
	//
	// A FIXED-SEED STRATIFIED SUBSET rather than the whole grid. The full space here is 8 types x 4
	// cutoffs x 3 resonances x 5 macro-1 positions x 5 macro-2 positions = 2400 combinations, each one
	// an impulse response and several transforms. The sampling is deterministic — same seed, same
	// draw, every run on every machine — so a failure names a setting somebody can reproduce, which a
	// per-run random subset could not. Stratified by TYPE so no type can go unvisited by luck.
	// LONG ENOUGH FOR THE LONGEST RINGING IN THE GRID, which is not a guess: the case below asserts
	// that each measurement actually settled inside its window, so a length that was too short fails
	// as a bad measurement rather than as a bad filter. 16384 was the first attempt and it read one
	// combination 0.33 dB off — low cutoff, high resonance, spread pushing a second stage lower
	// still — purely from truncation.
	constexpr size_t kIrLength = 65536;
	// AND FOUR TIMES THAT FOR THE COMB, because its ring is a delay loop rather than a pole pair and
	// it outlasts everything else here. MEASURED rather than guessed: an 80 Hz comb at resonance 0.95
	// left 6.9e-4 of its energy in the last eighth of a 32768 window and read 0.40 dB off; at 131072
	// the leftover was 6.8e-11 and the reading was 0.0000 dB. The precondition below is what turned
	// that from a mystery into a length.
	constexpr size_t kCombIrLength = 262144;
	constexpr int kPerType = 16; // 128 combinations, all of them measured to the end of the ringing

	constexpr double kCutoffs[] = {80.0, 400.0, 2000.0, 9000.0};
	constexpr double kResonances[] = {0.1, 0.6, 0.95};
	constexpr double kMacros[] = {-1.0, -0.5, 0.0, 0.5, 1.0};
	constexpr size_t kCombosPerType =
	    std::size(kCutoffs) * std::size(kResonances) * std::size(kMacros) * std::size(kMacros);

	// A plain LCG, seeded once. Deterministic by construction rather than by hoping the standard
	// library's generator is the same everywhere — which it is not.
	uint32_t state = 0x9E3779B9u;
	const auto next = [&state] {
		state = state * 1664525u + 1013904223u;
		return state;
	};

 	int checked = 0;
	for (int t = 0; t < static_cast<int>(Type::kCount); t++) {
		const std::string typeName = kTypeNames[t];
		CAPTURE(typeName);

		// Without replacement inside the type, by shuffling the type's own combination indices and
		// taking a prefix — so twenty-four DISTINCT settings rather than twenty-four draws that might
		// be the same one twice.
		std::vector<size_t> order(kCombosPerType);
		for (size_t i = 0; i < order.size(); i++) {
			order[i] = i;
		}
		for (size_t i = order.size(); i > 1; i--) {
			std::swap(order[i - 1], order[next() % i]);
		}

		for (int pick = 0; pick < kPerType; pick++) {
			size_t combo = order[static_cast<size_t>(pick)];
			const double macro2 = kMacros[combo % std::size(kMacros)];
			combo /= std::size(kMacros);
			const double macro1 = kMacros[combo % std::size(kMacros)];
			combo /= std::size(kMacros);
			const double res = kResonances[combo % std::size(kResonances)];
			combo /= std::size(kResonances);
			const double cutoff = kCutoffs[combo % std::size(kCutoffs)];

			Settings s = lowpassAt(cutoff);
			s.type = static_cast<Type>(t);
			s.resonance = static_cast<float>(res);
			s.macro1 = static_cast<float>(macro1);
			s.macro2 = static_cast<float>(macro2);
			CAPTURE(cutoff);
			CAPTURE(res);
			CAPTURE(macro1);
			CAPTURE(macro2);

			const std::vector<float> ir = render(
			    s, impulse(s.type == Type::Comb ? kCombIrLength : kIrLength), 64);
			// THE MEASUREMENT'S PRECONDITION, checked before anything is concluded from it: the filter
			// finished ringing inside the window, so what follows is its response and not its first
			// 65536 samples' response.
			const double tail = tailEnergyFraction(ir);
			CAPTURE(tail);
			// THE THRESHOLD IS DERIVED, not picked. Two measured points on the same setting fix the
			// relationship: a tail of 2.2e-4 read 0.33 dB off, and a tail of 1.7e-7 read 0.002 dB off.
			// The error goes as the square root of the leftover energy, so 1e-6 buys about 0.01 dB —
			// an order of magnitude inside the 0.1 dB this case then asserts. A first attempt at 1e-9
			// was tighter than the measurement needs and failed a shelf that was perfectly well
			// settled, which is its own lesson about preconditions invented rather than computed.
			REQUIRE(tail < 1e-6);

			for (int k = 0; k < 12; k++) {
				const double hz = 30.0 * std::pow(500.0, k / 11.0);
				const double predicted = gainAt(s, kSampleRate, hz);
				if (dB(predicted) < -45.0) {
					continue; // a null is where a float impulse response and a double formula part
				}
				CAPTURE(hz);
				CAPTURE(dB(predicted));
				CAPTURE(dB(measuredGainAt(ir, hz)));
				// TIGHT, and it stays tight because the window is now known to be long enough. A
				// tolerance widened to swallow a truncation artifact would also swallow the real
				// disagreements this grid exists to find.
				REQUIRE(std::abs(dB(measuredGainAt(ir, hz)) - dB(predicted)) < 0.1);
				checked++;
			}
		}
	}
	// The grid really was walked, and every type really was reached.
	CHECK(checked > 1000);
}

TEST_CASE("the curve ignores drive, deliberately, and says so by not moving") {
	// WHY DRIVE IS ABSENT FROM EVERY GRID ABOVE. A waveshaper has no magnitude response: what it does
	// to a signal depends on how loud that signal is, so there is no single curve to draw and any
	// curve drawn would be a lie about one input level or another. The card shows the LINEAR part and
	// the header says so.
	//
	// That is a decision rather than an omission, so it gets a case: moving drive across its whole
	// range must not move the drawn curve by so much as a rounding step. If somebody later teaches
	// the evaluator to show drive, this fails and makes them say it out loud.
	Settings quiet = lowpassAt(900.0);
	quiet.macro1 = 0.4f;
	quiet.macro2 = -0.6f;
	Settings driven = quiet;
	driven.drive = 1.0f;
	Settings middling = quiet;
	middling.drive = 0.5f;

	for (double hz : {30.0, 200.0, 900.0, 4000.0, 18000.0}) {
		CAPTURE(hz);
		CHECK(gainAt(driven, kSampleRate, hz) == gainAt(quiet, kSampleRate, hz));
		CHECK(gainAt(middling, kSampleRate, hz) == gainAt(quiet, kSampleRate, hz));
	}

	// KNOWN-BAD for it: drive is not inert in the AUDIO, so the case above is a statement about the
	// drawing rather than about a control that does nothing.
	const std::vector<float> input = noise(2048);
	CHECK(render(driven, input, 128) != render(quiet, input, 128));
}

TEST_CASE("a gain macro at zero is flat, which is what makes the dropdown safe to move") {
	for (Type type : {Type::LowShelf, Type::HighShelf, Type::Bell}) {
		CAPTURE(kTypeNames[static_cast<int>(type)]);
		CHECK(traitsOf(type).macro1.kind == MacroKind::Gain);

		Settings s = lowpassAt(1000.0);
		s.type = type;
		s.resonance = 0.7f; // resonance must not leak into the response either, at A = 1
		s.macro1 = 0.0f;
		for (double hz : {20.0, 200.0, 1000.0, 5000.0, 19000.0}) {
			CAPTURE(hz);
			CHECK(gainAt(s, kSampleRate, hz) == doctest::Approx(1.0).epsilon(1e-9));
		}

		// KNOWN-BAD for the case above: the gain axis has to actually do something, and in the
		// direction its sign says.
		s.macro1 = 1.0f;
		const double boosted = gainAt(s, kSampleRate, type == Type::HighShelf ? 19000.0 : 100.0);
		s.macro1 = -1.0f;
		const double cut = gainAt(s, kSampleRate, type == Type::HighShelf ? 19000.0 : 100.0);
		if (type == Type::Bell) {
			// A bell's feature is at its own corner, not at the edges.
			s.macro1 = 1.0f;
			CHECK(dB(gainAt(s, kSampleRate, 1000.0)) > 12.0);
			s.macro1 = -1.0f;
			CHECK(dB(gainAt(s, kSampleRate, 1000.0)) < -12.0);
		}
		else {
			CHECK(dB(boosted) > 20.0);
			CHECK(dB(cut) < -20.0);
		}
	}
}

TEST_CASE("a macro crossing zero does not click, in either mechanism") {
	// Spread and Morph both take their weight from |macro| and their direction from its sign, so the
	// sign flip happens where the weight is zero. Two settings either side of zero must therefore
	// render essentially the same samples — this is the case that would fail if the direction were
	// taken from anything but the sign, or the weight from anything but the magnitude.
	const std::vector<float> input = noise(2048);
	for (Type type : {Type::Lowpass, Type::Bandpass, Type::LowShelf}) {
		CAPTURE(kTypeNames[static_cast<int>(type)]);
		for (int which = 0; which < 2; which++) {
			Settings neg = lowpassAt(1200.0);
			neg.type = type;
			(which == 0 ? neg.macro1 : neg.macro2) = -1e-5f;
			Settings pos = neg;
			(which == 0 ? pos.macro1 : pos.macro2) = 1e-5f;

			const std::vector<float> a = render(neg, input, 128);
			const std::vector<float> b = render(pos, input, 128);
			double worst = 0.0;
			for (size_t i = 0; i < a.size(); i++) {
				worst = std::max(worst, std::abs(static_cast<double>(a[i] - b[i])));
			}
			CAPTURE(which);
			CAPTURE(worst);
			CHECK(worst < 1e-4);
		}
	}
}

TEST_CASE("drive at zero is the untouched signal, and drive above zero is not") {
	const std::vector<float> input = noise(2048);
	Settings s = lowpassAt(6000.0);
	s.drive = 0.0f;
	const std::vector<float> clean = render(s, input, 128);

	// The shaper is mixed in BY the amount, so an amount of zero is the identity in floating point
	// and not merely in the limit — the filter still runs, and its output is bit-identical to the
	// same filter with the drive stage compiled out, which is what "0.0" on a knob has to mean.
	MultimodeFilter bare;
	bare.prepare(kSampleRate);
	bare.setTarget(s);
	std::vector<float> byHand = input;
	bare.processMono(byHand.data(), static_cast<int>(byHand.size()));
	CHECK(byHand == clean);

	s.drive = 0.9f;
	const std::vector<float> driven = render(s, input, 128);
	CHECK(driven != clean);

	// Drive is a saturation and not a fader: a full-scale input comes back near full scale rather
	// than piling up above it.
	float peak = 0.0f;
	for (float v : driven) {
		peak = std::max(peak, std::abs(v));
	}
	CAPTURE(peak);
	CHECK(peak < 1.6f);
}

TEST_CASE("mono and stereo are the same filter") {
	// The per-voice architecture renders mono chunks and the bus renders stereo ones. A device that
	// filtered them differently would make keytrack sound like a different filter rather than like
	// the same filter tracking.
	const std::vector<float> input = noise(2048);
	Settings s = lowpassAt(900.0);
	s.macro1 = 0.4f;
	s.macro2 = -0.6f;
	s.drive = 0.3f;

	MultimodeFilter mono;
	mono.prepare(kSampleRate);
	mono.setTarget(s);
	std::vector<float> m = input;
	for (size_t at = 0; at < m.size(); at += 37) {
		mono.processMono(m.data() + at, static_cast<int>(std::min<size_t>(37, m.size() - at)));
	}

	CHECK(m == render(s, input, 37));
}

TEST_CASE("keytracking moves the cutoff by exactly the note's distance") {
	// The module does not decide bus-versus-per-voice — the owner does, by how many of these it runs
	// — but it does decide what a key offset MEANS, and an octave has to be an octave.
	Settings at60 = lowpassAt(500.0);
	Settings at72 = at60;
	at72.keyOffsetSemitones = 12.0f;
	Settings octaveUp = lowpassAt(1000.0);

	for (double hz : {100.0, 500.0, 1000.0, 4000.0}) {
		CAPTURE(hz);
		CHECK(std::abs(dB(gainAt(at72, kSampleRate, hz)) - dB(gainAt(octaveUp, kSampleRate, hz))) < 1e-9);
	}
	// KNOWN-BAD: without the offset the two are a full octave apart, which at 4 kHz on a two-pole
	// roll-off is about 12 dB of difference rather than none.
	{
		const double apart = std::abs(dB(gainAt(at60, kSampleRate, 4000.0))
		                              - dB(gainAt(octaveUp, kSampleRate, 4000.0)));
		CAPTURE(apart);
		CHECK(apart > 6.0);
	}
}

TEST_CASE("the key offset moves the filter itself, not only the drawn curve") {
	// The case above pins what the EVALUATOR does with a key offset. This one pins what the SAMPLE
	// LOOP does with it, and the two are separate claims — a first attempt at the synth-level
	// keytrack case measured a frequency where two voices overlapped and read 1.4 dB where there were
	// 15, which is exactly the kind of thing that is easier to catch here with one filter and an
	// impulse than up there with a chord.
	constexpr size_t kIrLength = 16384;

	Settings tracked = lowpassAt(300.0);
	tracked.keyOffsetSemitones = 24.0f;
	Settings shifted = lowpassAt(1200.0); // the same corner, written the other way

	const std::vector<float> a = render(tracked, impulse(kIrLength), 64);
	const std::vector<float> b = render(shifted, impulse(kIrLength), 64);
	// Not merely close: an offset of two octaves and a cutoff two octaves up are the SAME filter, and
	// the smoother folds the offset into the cutoff before either becomes a coefficient.
	CHECK(a == b);

	// KNOWN-BAD for it: without the offset the two are an octave-and-a-half apart at a frequency that
	// sits between them.
	const std::vector<float> untracked = render(lowpassAt(300.0), impulse(kIrLength), 64);
	const double apart = dB(measuredGainAt(untracked, 700.0)) - dB(measuredGainAt(a, 700.0));
	CAPTURE(apart);
	CHECK(apart < -12.0); // measured at -14.4: a factor of five in amplitude, not a rounding argument
}

TEST_CASE("the knob's range does not bound the corner a key offset asks for") {
	// THE RANGE IN kParamDescs IS THE KNOB'S TRAVEL, NOT THE FILTER'S LIMIT. What a hand can dial
	// runs 20 Hz to 20 kHz because that is the musical span and a knob has only so many pixels; what
	// the DSP can run is bounded by Nyquist, which gOf applies itself to whatever it is handed. A
	// clamp of the SUM to the knob's ends silently stops keytracking at the point where the knob's
	// value plus the key offset leaves the range — the filter goes on being a filter, it just stops
	// following the key, which is the one thing the feature promises.
	//
	// MEASURED BEFORE THE FIX, with the knob at its own maximum: C6 asked for 80 kHz and got 20 kHz,
	// two octaves of clip, and every note above it got the same 20 kHz. With the knob at its minimum,
	// C2 asked for 5 Hz and got 20 Hz, two octaves the other way — a low note could not be closed as
	// far as a middle one.
	Settings top = lowpassAt(20000.0);
	top.keyOffsetSemitones = 24.0f;
	CHECK(effectiveCutoffHz(top) == doctest::Approx(80000.0));

	Settings bottom = lowpassAt(20.0);
	bottom.keyOffsetSemitones = -24.0f;
	CHECK(effectiveCutoffHz(bottom) == doctest::Approx(5.0));

	// AND IT STILL TRACKS UP THERE, which is the claim the clip destroyed: an octave of key is an
	// octave of corner at every distance, not only at the distances that happen to land inside the
	// knob's range.
	Settings octaveUp = top;
	octaveUp.keyOffsetSemitones = 36.0f;
	CHECK(effectiveCutoffHz(octaveUp) / effectiveCutoffHz(top) == doctest::Approx(2.0));
}

TEST_CASE("a corner driven far past Nyquist stays a filter") {
	// The bound that replaces the knob's is the vendor's own: setCoeff clamps f/sr to 0.499, so a
	// corner four octaves above the top of the range is a legal coefficient rather than a hazard.
	// Checked rather than assumed, because "the DSP tolerates it" was the premise the whole fix
	// rested on, and a premise that is only read is not a premise that is known.
	Settings s = lowpassAt(20000.0);
	s.keyOffsetSemitones = 48.0f; // 320 kHz asked of a 48 kHz stream
	s.resonance = 0.9f;
	s.drive = 0.5f;

	const std::vector<float> input = noise(4096);
	const std::vector<float> out = render(s, input, 64);

	float peak = 0.0f;
	for (float v : out) {
		REQUIRE(std::isfinite(v));
		peak = std::max(peak, std::abs(v));
	}
	CAPTURE(peak);
	// MEASURED at 1.13 against a full-scale noise input — the drive's own shaping and nothing else.
	// The bound is where it is because the same case run against the clamped code measured 3.31: with
	// the corner held at 20 kHz the resonance sat inside the band and rang, which is the reading this
	// number is chosen to be able to tell apart from a filter that is out of the way.
	CHECK(peak < 2.0f);
	// Wide open rather than silent: a filter whose corner is past Nyquist passes what it is given.
	CHECK(peak > 0.1f);
}

TEST_CASE("the profile is log-spaced and every point is finite") {
	// The card's curve goes straight to a renderer that has no opinion about NaN, and wave_display's
	// rule is that a non-finite point never reaches it. Cheaper to guarantee at the source.
	constexpr size_t kPoints = 129;
	std::array<float, kPoints> freq{};
	std::array<float, kPoints> gain{};

	Settings s = lowpassAt(1000.0);
	s.resonance = 0.99f; // above the SVF's own clamp, on purpose
	s.macro1 = 1.0f;
	s.macro2 = -1.0f;
	gainProfile(s, kSampleRate, freq.data(), gain.data(), kPoints);

	CHECK(freq.front() == doctest::Approx(kProfileLoHz));
	CHECK(freq.back() == doctest::Approx(kProfileHiHz));
	for (size_t i = 0; i < kPoints; i++) {
		CAPTURE(i);
		CHECK(std::isfinite(freq[i]));
		CHECK(std::isfinite(gain[i]));
		CHECK(gain[i] >= 0.0f);
		if (i > 0) {
			CHECK(freq[i] > freq[i - 1]);
			// Log spacing: the same ratio every step, which is what makes an octave the same width
			// everywhere on the card.
			if (i > 1) {
				CHECK(static_cast<double>(freq[i]) / freq[i - 1]
				      == doctest::Approx(static_cast<double>(freq[i - 1]) / freq[i - 2]).epsilon(1e-4));
			}
		}
	}
}

TEST_CASE("the module includes nothing from the plugin") {
	// THE INCLUDE GRAPH IS THE BOUNDARY, so the boundary is checked by reading the include graph. A
	// tenant that reached into src/plugin would still compile and still work, and would quietly stop
	// being a device that could be hosted anywhere else — which is the entire reason it is a module.
	const std::filesystem::path root(FEATHERS_FX_SOURCE_DIR);
	REQUIRE(std::filesystem::exists(root));

	int filesRead = 0;
	for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) {
			continue;
		}
		const auto ext = entry.path().extension().string();
		if (ext != ".h" && ext != ".cpp") {
			continue;
		}
		filesRead++;
		CAPTURE(entry.path().string());

		std::ifstream in(entry.path());
		REQUIRE(in.good());
		std::string line;
		int lineNumber = 0;
		while (std::getline(in, line)) {
			lineNumber++;
			if (line.rfind("#include", 0) != 0) {
				continue;
			}
			CAPTURE(lineNumber);
			CAPTURE(line);
			CHECK(line.find("plugin/") == std::string::npos);
			// AND NOTHING FROM THE GPL VENDOR LIBRARIES the licence migration took out of the effects
			// (2026-09-24): the SVF is our MIT copy, and the one-pole, sinc line and fast math are ours.
			CHECK(line.find("sst/basic-blocks") == std::string::npos);
			CHECK(line.find("sst/filters") == std::string::npos);
		}
	}
	// If the walk found nothing, the check above passed by not running.
	CHECK(filesRead >= 3);
}

TEST_CASE("nothing in src or tools includes a GPL sst library") {
	// THE LICENCE MIGRATION'S LAST STEP (2026-09-24): sst-basic-blocks and sst-filters (GPL-3) are no
	// longer submodules, so the core can carry a plugin exception. A build would already fail on one of
	// their headers — this case exists so the failure names the RULE rather than a missing file, and so
	// re-adding the submodule to "fix" that build is refused here too. The case above covers src/fx for
	// the boundary; this one walks every source tree we ship or build tools from.
	//
	// Stricter than the case above about what an include LOOKS like: leading whitespace and `# include`
	// are both legal preprocessor spellings, and an include inside an #if is often indented.
	// sst-voicemanager and sst-cpputils are MIT and stay; only these two prefixes are refused.
	int filesRead = 0;
	for (const char* const dir : {FEATHERS_SRC_DIR, FEATHERS_TOOLS_DIR}) {
		const std::filesystem::path root(dir);
		REQUIRE(std::filesystem::exists(root));
		for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			const auto ext = entry.path().extension().string();
			if (ext != ".h" && ext != ".hpp" && ext != ".cpp" && ext != ".c" && ext != ".mm") {
				continue;
			}
			filesRead++;
			CAPTURE(entry.path().string());

			std::ifstream in(entry.path());
			REQUIRE(in.good());
			std::string line;
			int lineNumber = 0;
			while (std::getline(in, line)) {
				lineNumber++;
				size_t i = line.find_first_not_of(" \t");
				if (i == std::string::npos || line[i] != '#') {
					continue;
				}
				i = line.find_first_not_of(" \t", i + 1);
				if (i == std::string::npos || line.compare(i, 7, "include") != 0) {
					continue;
				}
				CAPTURE(lineNumber);
				CAPTURE(line);
				CHECK(line.find("sst/basic-blocks") == std::string::npos);
				CHECK(line.find("sst/filters") == std::string::npos);
			}
		}
	}
	// The whole of src/ is several hundred files; a walk that found a handful walked the wrong place.
	CHECK(filesRead >= 200);
}

TEST_CASE("the slope control is a pole count, and every pole is 6 dB of it") {
	// THE WHOLE FEATURE, measured where a roll-off is a straight line: two octaves above a 500 Hz
	// corner, far enough down the skirt that the corner's own shape has stopped mattering and far
	// enough below Nyquist that the bilinear warp has not started. (One octave higher the same
	// measurement reads 6.6 / 13.2 / 19.9 / 26.6 — that steepening is the warp, not the filter, and
	// measuring there would have made every number look wrong by a decibel.)
	//
	// MEASURED: 5.98, 11.95, 17.93, 24.28 dB per octave. The tolerance is ±0.4 dB, which is wide
	// enough for the warp that is left and far too narrow for a pole miscounted anywhere.
	constexpr double kCorner = 500.0;
	const std::array<double, 4> expected{6.0, 12.0, 18.0, 24.0};

	for (int i = 0; i < 4; i++) {
		const auto slope = static_cast<Slope>(i);
		CAPTURE(i);
		Settings s = lowpassAt(kCorner);
		s.resonance = 0.0f; // a skirt measurement, not a corner one
		s.slope = slope;

		const double perOctave = dB(gainAt(s, kSampleRate, 2000.0)) - dB(gainAt(s, kSampleRate, 4000.0));
		CAPTURE(perOctave);
		CHECK(std::abs(perOctave - expected[static_cast<size_t>(i)]) < 0.4);
		// And the name is the arithmetic rather than a label beside it.
		CHECK(polesOf(slope) * 6 == static_cast<int>(expected[static_cast<size_t>(i)]));
	}
}

TEST_CASE("a 24 dB corner peaks once") {
	// THE DESIGN DECISION, and the reason the second section is Butterworth rather than a second copy of
	// the knob: two sections both wearing the user's Q would multiply their peaks, so a resonance
	// that reads 24 dB at 12 dB/oct would read about 48 at 24 dB/oct — a knob whose meaning depends
	// on a control that is not it.
	//
	// MEASURED, 12 dB against 24 dB at the same resonance: +1.2 vs +0.7 dB at res 0.5, +8.1 vs +5.6
	// at 0.8, +24.4 vs +21.4 at 0.97. The steeper filter peaks slightly LOWER — the Butterworth
	// section is 3 dB down at the corner and takes a little off the top — which is the shape to
	// expect and the opposite of the doubling this case exists to catch.
	const auto peakDb = [](const Settings& s) {
		double best = 0.0;
		for (int i = 0; i < 800; i++) {
			best = std::max(best, gainAt(s, kSampleRate, 1000.0 * std::pow(2.0, -1.0 + i / 399.0)));
		}
		return dB(best);
	};

	for (double res : {0.5, 0.8, 0.97}) {
		CAPTURE(res);
		Settings twelve = lowpassAt(1000.0);
		twelve.resonance = static_cast<float>(res);
		Settings twentyFour = twelve;
		twentyFour.slope = Slope::db24;

		const double a = peakDb(twelve);
		const double b = peakDb(twentyFour);
		CAPTURE(a);
		CAPTURE(b);
		// Never above the two-pole's peak, and never more than 3 dB below it: one peak, of about the
		// height the knob asked for. A squared peak would sit tens of decibels above this window.
		CHECK(b <= a);
		CHECK(b > a - 3.5);
	}
}

TEST_CASE("the curve is the filter that plays, at every slope") {
	// THE CARD'S PROMISE, extended to the axis this task adds — and it is a hard requirement rather
	// than a nicety, because the closed form is what the card draws and the sample loop is what the
	// ear hears. A cascade that the evaluator multiplied differently from the way the loop runs it
	// would show a curve nobody is playing.
	//
	// Measured off the ACTUAL impulse response, the way the existing cross-checks in this file do,
	// with the one-pole in the mix at slopes 6 and 18 — the stage with no vendor formula to lean on
	// and therefore the one most worth measuring.
	constexpr size_t kIrLength = 32768;

	for (int i = 0; i < 4; i++) {
		const auto slope = static_cast<Slope>(i);
		CAPTURE(i);
		for (Type type : {Type::Lowpass, Type::Highpass}) {
			CAPTURE(static_cast<int>(type));
			Settings s = lowpassAt(800.0);
			s.type = type;
			s.resonance = 0.4f;
			s.slope = slope;

			const std::vector<float> ir = render(s, impulse(kIrLength), 64);
			// The window has to be long enough for the transform to be the filter's response rather
			// than the response of its first N samples — this file's own precondition, and a cascade
			// rings longer than one section.
			CAPTURE(tailEnergyFraction(ir));
			REQUIRE(tailEnergyFraction(ir) < 1e-6);

			for (double hz : {200.0, 800.0, 2000.0, 6000.0}) {
				CAPTURE(hz);
				const double measured = dB(measuredGainAt(ir, hz));
				const double drawn = dB(gainAt(s, kSampleRate, hz));
				CAPTURE(measured);
				CAPTURE(drawn);
				// The same 0.1 dB the resonance case above holds the evaluator to. What is left is
				// fasttan against std::tan, which is where this module has always sat.
				CHECK(std::abs(drawn - measured) < 0.1);
			}
		}
	}
}

TEST_CASE("a one-pole has no resonance to give, and says so by not moving") {
	// A SINGLE POLE CANNOT RESONATE — there is no pole pair to bring toward the unit circle — so the
	// resonance knob at 6 dB is a control with nothing to control. That is a real limitation and the
	// honest way to state it is a case: the samples are IDENTICAL, not merely close, because the
	// resonance never reaches a coefficient at all.
	const std::vector<float> input = noise(2048);

	Settings quiet = lowpassAt(700.0);
	quiet.slope = Slope::db6;
	quiet.resonance = 0.0f;
	Settings loud = quiet;
	loud.resonance = 0.97f;

	CHECK(render(quiet, input, 64) == render(loud, input, 64));

	// THE CONTROL, so this is not passing because the whole slope is inert: at 12 dB the same two
	// resonances are a filter apart.
	Settings twelveQuiet = quiet;
	twelveQuiet.slope = Slope::db12;
	Settings twelveLoud = loud;
	twelveLoud.slope = Slope::db12;
	CHECK(render(twelveQuiet, input, 64) != render(twelveLoud, input, 64));
}

TEST_CASE("a slope a type cannot wear rounds up rather than lying about it") {
	// Only a lowpass and a highpass have a one-pole form; a bandpass and a notch each need a zero on
	// both sides and cannot be built from one pole. Those two round an odd slope UP — more is never
	// less — and the gain shapes ignore the control entirely, which is what the card asks
	// slopeAppliesTo so it can grey it rather than offer four settings that are one setting.
	CHECK(stagePlanOf(Type::Bandpass, Slope::db6) == stagePlanOf(Type::Bandpass, Slope::db12));
	CHECK(stagePlanOf(Type::Bandpass, Slope::db18) == stagePlanOf(Type::Bandpass, Slope::db24));
	CHECK(stagePlanOf(Type::Notch, Slope::db18) == stagePlanOf(Type::Notch, Slope::db24));
	// Monotone: rounding up never gives a bandpass FEWER sections than the slope below it.
	CHECK(stagePlanOf(Type::Bandpass, Slope::db24).twoPoleStages
	      > stagePlanOf(Type::Bandpass, Slope::db12).twoPoleStages);

	for (Type type : {Type::Bell, Type::LowShelf, Type::HighShelf, Type::Peak}) {
		CAPTURE(static_cast<int>(type));
		CHECK_FALSE(slopeAppliesTo(type));
		for (int i = 0; i < 4; i++) {
			// One section at every entry, so a "24 dB" bell is not quietly a +48 dB one.
			CHECK(stagePlanOf(type, static_cast<Slope>(i)) == stagePlanOf(type, Slope::db12));
		}
	}
	CHECK(slopeAppliesTo(Type::Lowpass));
	CHECK(slopeAppliesTo(Type::Bandpass));

	// And the plans a lowpass wears are the four distinct ones, in order.
	CHECK(stagePlanOf(Type::Lowpass, Slope::db6).twoPoleStages == 0);
	CHECK(stagePlanOf(Type::Lowpass, Slope::db6).onePole);
	CHECK(stagePlanOf(Type::Lowpass, Slope::db12) == StagePlan{1, false, false});
	CHECK(stagePlanOf(Type::Lowpass, Slope::db18) == StagePlan{1, true, false});
	CHECK(stagePlanOf(Type::Lowpass, Slope::db24) == StagePlan{2, false, false});
	CHECK(stagePlanOf(Type::Highpass, Slope::db18).onePoleHighpass);
}

TEST_CASE("a stage that stops running leaves the signal entirely") {
	// A SLOPE IS A CHANGE OF TOPOLOGY, which is the thing a smoother cannot smooth: at 24 dB there
	// are two sections and at 12 there is one, and the one that stops has to be OUT rather than
	// running at unity with stale state. Measured as bit-identity against a filter that was never
	// anything else — the same shape as the voice-reuse case in test_synth.cpp, and for the same
	// reason: a leftover stage ADDS something, and no "is there sound here" assertion can see it.
	const std::vector<float> input = noise(4096);

	Settings twelve = lowpassAt(600.0);
	twelve.resonance = 0.6f;

	MultimodeFilter switched;
	switched.prepare(kSampleRate);
	Settings loud = twelve;
	loud.slope = Slope::db24;
	switched.setTarget(loud);
	std::vector<float> warm = noise(4096, 0x1234abcdu);
	switched.processMono(warm.data(), static_cast<int>(warm.size())); // run the second section hot
	switched.setTarget(twelve);
	// Long enough for the smoother to arrive, so what is left is topology rather than a ramp.
	std::vector<float> settle(4096, 0.0f);
	switched.processMono(settle.data(), static_cast<int>(settle.size()));

	std::vector<float> after = input;
	switched.processMono(after.data(), static_cast<int>(after.size()));

	MultimodeFilter fresh;
	fresh.prepare(kSampleRate);
	fresh.setTarget(twelve);
	std::vector<float> quiet(4096, 0.0f);
	fresh.processMono(quiet.data(), static_cast<int>(quiet.size()));
	std::vector<float> reference = input;
	fresh.processMono(reference.data(), static_cast<int>(reference.size()));

	CHECK(after == reference);
}

TEST_CASE("the cascade's state cost is declared rather than discovered") {
	// KEYTRACK MEANS ONE OF THESE PER VOICE, so a section added here is twenty sections added to the
	// synth. MEASURED: the object went from 816 to 1216 bytes — one more CytomicSVF at 352, plus the
	// four one-poles at eight bytes each — and four since-dropped models added ~112 bytes on top, to
	// 1328 (they left with the licence migration, 2026-09-24, taking those bytes with them). Cheap,
	// but not free, and pinned so the next stage somebody adds has to come and change this number on
	// purpose.
	//
	// AND THEN THE COMB PUT A 32 KB DELAY LINE IN IT — 1376 to 34320, a factor of twenty-five, which
	// is the whole of the storage decision on the `comb` branch arriving as a number. Two
	// SincDelayLine<4096> at 16448 bytes each; everything else this device holds is the 1376 that
	// was here before. THE POOL USED TO BE WHERE IT WAS PAID: FxRunner sized a uniform stride from the
	// widest device and allocated one block per (voice slot, fx slot) pair — 20 x 8 of them — so the
	// fx pool measured 236 KB before this device grew and 5.5 MB after, on every patch, whether or not
	// any comb was selected. That stride is gone (2026-08-26): a slot costs its own device now, so
	// this number is paid once per filter that actually exists rather than once per slot that might
	// hold one. It is still worth pinning — one of these per voice is still one of these per voice.
	//
	// TWO NUMBERS, DELIBERATELY. The first is the ceiling on the object; the second says out loud that
	// the delay line is the object, so a future stage added to the CASCADE cannot hide inside the
	// comb's headroom the way it would if only one number were pinned.
	//
	// RE-MEASURED ON PURPOSE FOR STEREO MODULATION (backlog KU, 2026-10-04): the right ear's six smoothers, the
	// eleven right-channel twins (morph/spread weight, drive, the comb's head and loop) and a live mask — 1360 and
	// 34256, both bounds now at the measured size; the headroom the dropped models left absorbed the growth.
 	CAPTURE(sizeof(MultimodeFilter));
	CAPTURE(sizeof(feathers::fx::filter::CytomicSVF));
	//
	// AND AGAIN FOR THE BRICKWALL HP (backlog NL, 2026-10-26): two EllipticHighpass channels — four double SVF sections
	// each, five coefficients and two integrators per section — 464 bytes, to 1824 and 34720.
	CHECK(sizeof(MultimodeFilter) <= 34720);
	CHECK(sizeof(MultimodeFilter)
	      - 2 * sizeof(feathers::dsp::SincDelayLine<kCombLineSamples>) <= 1824);
}

TEST_CASE("spread is a knee and slope is a tail") {
	// THE CORRECTION THAT DECIDED THE DESIGN, and it overturned a claim this session had already
	// written down: that Spread offers a CONTINUOUS traversal between 12 and 24 dB which a stepped
	// control cannot reach. It does not, and the algebra says why. The loop computes
	//
	//     out = h1·(1−w) + h1·h2·w
	//
	// and as f→∞ the first term falls as 1/f² while the second falls as 1/f⁴, so for EVERY w < 1 the
	// surviving term is the 12 dB one, merely attenuated by (1−w). Only at exactly w = 1 does it
	// vanish. THE ASYMPTOTIC SLOPE OF ANY RATIONAL FILTER IS AN INTEGER TIMES 6 dB/OCT — there is no
	// continuous slope control in this filter or in anyone else's, and none was lost by making the
	// control stepped.
	//
	// MEASURED at a 200 Hz corner, the octave from 5 to 10 kHz: 14.2, 14.1, 13.9, 13.0 dB for w =
	// 0.25, 0.5, 0.75, 0.9 — all of them the 12 dB tail plus a little warp — against 28.2 at w = 1.
	// (Measured no higher: the octave from 8 to 16 kHz reads 19 dB even at w = 0.25, which is the
	// bilinear warp against Nyquist rather than any filter.)
	const auto tail = [](const Settings& s) {
		return dB(gainAt(s, kSampleRate, 5000.0)) - dB(gainAt(s, kSampleRate, 10000.0));
	};

	for (double w : {0.25, 0.5, 0.75, 0.9}) {
		CAPTURE(w);
		Settings s = lowpassAt(200.0);
		s.resonance = 0.0f;
		s.spread = static_cast<float>(w);
		CAPTURE(tail(s));
		CHECK(tail(s) < 16.0); // a 12 dB tail, wearing the warp
	}

	Settings full = lowpassAt(200.0);
	full.resonance = 0.0f;
	full.spread = 1.0f;
	CAPTURE(tail(full));
	CHECK(tail(full) > 24.0); // the 12 dB term is gone only here

	// AND WHAT SPREAD REALLY OFFERS, which is the reason it survives alongside Slope rather than
	// being replaced by it: a 24 dB KNEE over a 12 dB TAIL — a shelf between two corners two octaves
	// apart. Slope cannot make that shape at any setting, and Spread cannot make Slope's coincident
	// corner at any setting either.
	Settings knee = lowpassAt(200.0);
	knee.resonance = 0.0f;
	knee.spread = 0.9f;
	const double kneeSteepness =
	    dB(gainAt(knee, kSampleRate, 400.0)) - dB(gainAt(knee, kSampleRate, 1600.0));
	CAPTURE(kneeSteepness);
	CHECK(kneeSteepness > 36.0); // two octaves of knee, steeper than the 24 dB its tail is not
}

TEST_CASE("an odd slope past Nyquist is still a filter") {
	// THE ONE STAGE THAT DOES NOT CLAMP ITSELF. CytomicSVF::setCoeff limits fc/sr to 0.499 itself;
	// the one-pole takes tan(pi·fc/sr) as given, and tan of an angle past pi/2 is NEGATIVE — which
	// would make the one-pole not a filter at all. That is reachable rather than theoretical since
	// the cutoff fix on this branch's parent: a keytracked high note genuinely asks for a corner
	// above Nyquist, and this module now hands it over instead of clamping it to the knob's top.
	const std::vector<float> input = noise(4096);

	for (Slope slope : {Slope::db6, Slope::db18}) {
		CAPTURE(static_cast<int>(slope));
		Settings s = lowpassAt(20000.0);
		s.slope = slope;
		s.keyOffsetSemitones = 48.0f; // 320 kHz asked of a 48 kHz stream
		s.resonance = 0.5f;

		const std::vector<float> out = render(s, input, 64);
		float peak = 0.0f;
		for (float v : out) {
			REQUIRE(std::isfinite(v));
			peak = std::max(peak, std::abs(v));
		}
		CAPTURE(peak);
		// Wide open rather than inverted or exploding: a corner past Nyquist passes what it is given.
		CHECK(peak > 0.5f);
		CHECK(peak < 2.0f);
	}
}

TEST_CASE("the macro table declares what a type spends and what it does not") {
	// SPREAD LEFT THE MACRO TABLE, and the five types that spent macro 1 on it now declare NOTHING
	// there — deliberately empty rather than refilled, because comb and phaser are the devices that
	// will spend that slot and a placeholder now is a control they would have to take away.
	for (Type type : {Type::Lowpass, Type::Highpass, Type::Bandpass, Type::Notch, Type::Peak}) {
		CAPTURE(static_cast<int>(type));
		CHECK(traitsOf(type).macro1.kind == MacroKind::None);
		CHECK(traitsOf(type).macro2.kind == MacroKind::Morph);
	}

	// THE THREE WITH A GAIN AXIS keep it, and now have a Morph they did not have while Spread was a
	// macro — the shelves into each other. BELL IS NOT A PARTNER IN EITHER DIRECTION: setCoeff
	// divides its k by the gain, so a Bell lane and a non-Bell lane are two different filters rather
	// than two mixes of one, and this module's whole morph mechanism rests on them being one.
	for (Type type : {Type::LowShelf, Type::HighShelf, Type::Bell}) {
		CAPTURE(static_cast<int>(type));
		CHECK(traitsOf(type).macro1.kind == MacroKind::Gain);
	}
	CHECK(traitsOf(Type::LowShelf).macro2.kind == MacroKind::Morph);
	CHECK(traitsOf(Type::HighShelf).macro2.kind == MacroKind::Morph);
	CHECK(traitsOf(Type::Bell).macro2.kind == MacroKind::None);
	for (Type type : {Type::LowShelf, Type::HighShelf}) {
		CHECK(traitsOf(type).macro2.morphNeg != Type::Bell);
		CHECK(traitsOf(type).macro2.morphPos != Type::Bell);
	}
	// Nor is Bell reachable as a partner from anywhere else in the table.
	for (int i = 0; i < static_cast<int>(Type::kCount); i++) {
		const TypeTraits& traits = traitsOf(static_cast<Type>(i));
		CAPTURE(i);
		for (const MacroMap& map : {traits.macro1, traits.macro2}) {
			if (map.kind != MacroKind::Morph) {
				continue;
			}
			CHECK(map.morphNeg != Type::Bell);
			CHECK(map.morphPos != Type::Bell);
		}
	}
}

TEST_CASE("every type declares its response drawable, and the card's promise rests on it") {
	// THE PROMISE MADE CHECKABLE. "The curve is the filter that plays" has been true of this module
	// since it existed, but it lived in a comment — a thing a new model can be added without having
	// read. Now a type SAYS it.
	//
	// Every SVF type and the comb satisfy it: they are LINEAR, so they have one magnitude response
	// and the curve IS the filter that plays.
	//
	// A NONLINEAR MODEL WOULD NOT, AND WOULD HAVE TO SAY SO — there is no single response to draw,
	// and what the card shows for one is a proxy, labelled as such. Three such models once arrived
	// declaring drawable = true, which is how a nonlinear model gets past the gate this flag exists
	// to be. They are gone (licence migration, 2026-09-24), and every type left is linear.
	for (int i = 0; i < static_cast<int>(Type::kCount); i++) {
		CAPTURE(i);
		CAPTURE(std::string(kTypeNames[i]));
		CHECK(traitsOf(static_cast<Type>(i)).drawable);
	}
}

//--------------------------------------------------------------------------------//
// THE COMB — a feedback delay of one period, and the four claims it rests on.      //
//--------------------------------------------------------------------------------//

namespace {

Settings combAt(double hz, float resonance = 0.9f, float damp = 0.0f) {
	Settings s;
	s.enabled = true;
	s.type = Type::Comb;
	s.cutoffLog2Hz = static_cast<float>(std::log2(hz));
	s.resonance = resonance;
	s.macro1 = damp;
	return s;
}

/// THE FREQUENCY THE COMB IS ACTUALLY RESONATING AT, found by refining a search on the measured
/// magnitude response rather than by asking the code what it thinks. Four passes, each 400 points
/// wide over the previous pass's winning interval, so the answer is not the grid.
double measuredPeakHz(const std::vector<float>& ir, double aboutHz) {
	double lo = aboutHz * 0.94;
	double hi = aboutHz * 1.06;
	double bestHz = aboutHz;
	for (int pass = 0; pass < 4; pass++) {
		const double step = (hi - lo) / 400.0;
		double best = -1.0;
		for (double hz = lo; hz <= hi; hz += step) {
			const double m = measuredGainAt(ir, hz);
			if (m > best) {
				best = m;
				bestHz = hz;
			}
		}
		lo = bestHz - step;
		hi = bestHz + step;
	}
	return bestHz;
}

double cents(double measured, double want) {
	return 1200.0 * std::log2(measured / want);
}

double hzOfKey(int key) {
	return 440.0 * std::pow(2.0, (key - 69) / 12.0);
}

} // namespace

TEST_CASE("a comb arrives tuned to the note, and the delay is the note's own period") {
	// THE POINT OF PUTTING A COMB IN *THIS* FILTER. The roadmap's claim is that Serum's comb needs
	// retuning by hand because its keytrack is not calibrated to unison, and that ours does not
	// because kKeytrackReferenceKey is exact — one octave per octave from middle C. That is a claim
	// about a VALUE, so the value is what is asserted: the delay the sample loop runs must be the
	// played note's period in samples, to the last bit a double carries.
	//
	// AND THEN MEASURED ANYWAY, because a delay being right is not the same as the comb resonating
	// there — an off-by-one in the read pointer, a fencepost in the sinc window, or an interpolator
	// reading the wrong row of the table would all leave the value assertion green and move the pitch.
	// The two together are the claim; either alone is not.
	for (int key : {36, 48, 60, 69, 72, 84}) {
		CAPTURE(key);
		const double want = hzOfKey(key);
		Settings s = combAt(hzOfKey(kKeytrackReferenceKey), 0.95f);
		s.keyOffsetSemitones = static_cast<float>(key - kKeytrackReferenceKey);

		// THE VALUE. The corner the filter runs at is the note, and the delay is its period.
		const double corner = effectiveCutoffHz(s);
		CAPTURE(corner);
		CHECK(std::abs(corner - want) < want * 1e-6);
		const double delay = combDelaySamples(corner, kSampleRate);
		CAPTURE(delay);
		// RELATIVE, AND THE FIGURE IS WHERE THE FLOAT STOPS. Cutoff is stored as a float log2, so the
		// corner carries about seven digits and no assertion about the delay can be tighter than that.
		// One part in a million is 0.0017 cents — three orders of magnitude finer than the measured
		// assertion below, so this is still the value being pinned and not the storage.
		CHECK(std::abs(delay - kSampleRate / want) < delay * 1e-6);

		// AND THE INSTRUMENT. 65536 samples is long enough for the peak search to resolve; what is
		// asserted is a fifth of a cent, which is two orders of magnitude finer than a human ear
		// hears and about five times the search's own resolution at these frequencies.
		const std::vector<float> ir = render(s, impulse(65536), 64);
		const double peak = measuredPeakHz(ir, want);
		CAPTURE(peak);
		CAPTURE(cents(peak, want));
		CHECK(std::abs(cents(peak, want)) < 0.2);
	}

	// KNOWN-BAD, IN THE MEASUREMENT'S OWN TERMS: a comb deliberately detuned by thirty cents must be
	// measured thirty cents out. Without this the case would pass on a build whose peak search always
	// answered "wherever I was told to look".
	//
	// THE FIRST DRAFT OF THIS KNOWN-BAD WAS VACUOUS AND SAID NOTHING. It compared an UNTRACKED comb at
	// middle C against the note two octaves and a fifth above it — and 1046.5 Hz is exactly the fourth
	// harmonic of 261.6 Hz, so the untracked comb has a peak of its own there and the search found it
	// dead on. A comb's spectrum is a harmonic series; any "wrong" frequency chosen from the same
	// keyboard is liable to be one of its peaks. A detune that is not a rational ratio is the version
	// that cannot land on one by accident.
	{
		const double want = hzOfKey(84);
		Settings detuned = combAt(want * std::pow(2.0, 30.0 / 1200.0), 0.95f);
		const std::vector<float> ir = render(detuned, impulse(65536), 64);
		const double peak = measuredPeakHz(ir, want);
		CAPTURE(peak);
		CAPTURE(cents(peak, want));
		CHECK(std::abs(cents(peak, want) - 30.0) < 1.0);
	}
}

TEST_CASE("the comb the card draws is the comb that plays") {
	// THE CARD'S PROMISE, ASKED OF THE ONE TYPE WHOSE CLOSED FORM IS NOT AN SVF. A comb is
	// 1/(1 - fb.D(z).I(z)) — the loop, the damper and the interpolator — and the third factor is the
	// one that had to be MEASURED rather than assumed. A first draft used an ideal fractional delay,
	// e^{-jwd}, and missed the rendered response BY UP TO 14.6 dB at 12 kHz: the vendor's sinc window
	// is cut at 0.455 of the sample rate, so the real loop loses its top where an ideal delay keeps
	// it. Reading the vendor's twelve taps into the formula took the same case to 0.0000 dB.
	struct Case {
		const char* what;
		double hz;
		float resonance;
		float damp;
		size_t irLength;
	};
	const Case cases[] = {
	    {"200 Hz, half resonance, undamped", 200.0, 0.5f, 0.0f, 32768},
	    {"200 Hz, deep resonance, undamped", 200.0, 0.9f, 0.0f, 65536},
	    {"440 Hz, damped dark", 440.0, 0.8f, 0.6f, 32768},
	    {"440 Hz, damped bright", 440.0, 0.8f, -0.6f, 32768},
	    {"1 kHz, shallow", 1000.0, 0.3f, 0.0f, 32768},
	    {"3 kHz, resonant", 3000.0, 0.7f, 0.0f, 32768},
	    // THE LONGEST RING IN THE FILE and the reason kCombIrLength exists: at 32768 this reads
	    // 0.40 dB off with 6.9e-4 of its energy still in the last eighth, which is a truncated
	    // measurement rather than a wrong curve.
	    {"B1 at the edge of the range", 61.735, 0.95f, 0.2f, 262144},
	};

	for (const Case& c : cases) {
		CAPTURE(std::string(c.what));
		const Settings s = combAt(c.hz, c.resonance, c.damp);
		const std::vector<float> ir = render(s, impulse(c.irLength), 64);
		const double tail = tailEnergyFraction(ir);
		CAPTURE(tail);
		REQUIRE(tail < 1e-6);

		int checked = 0;
		for (double hz = 50.0; hz < 20000.0; hz *= 1.1) {
			const double predicted = gainAt(s, kSampleRate, hz);
			if (dB(predicted) < -50.0) {
				continue;
			}
			CAPTURE(hz);
			CAPTURE(dB(predicted));
			CAPTURE(dB(measuredGainAt(ir, hz)));
			CHECK(std::abs(dB(measuredGainAt(ir, hz)) - dB(predicted)) < 0.1);
			checked++;
		}
		CHECK(checked > 30);
	}
}

TEST_CASE("the damper is exactly nothing at zero, and darkens or brightens either side") {
	// THE MACRO INVARIANT THIS MODULE CARRIES EVERYWHERE — bipolar, and the identity at zero — asked
	// of the fourth mechanism. The damper is (1 - |a|)/(1 - a.z^-1), which at a = 0 is the number one
	// in floating point and not merely in the limit, so a comb whose Damp knob is centred must be
	// bit-identical to one that has no damper at all.
	const std::vector<float> input = noise(4096);
	const Settings flat = combAt(300.0, 0.85f, 0.0f);

	// NOT A CLICK ACROSS ZERO, the same question the Morph and Spread mechanisms answer — and the
	// bound is looser than theirs for a reason that is measurable rather than convenient: a comb at
	// resonance 0.85 has about 16 dB of loop gain, so the same coefficient nudge comes out of it an
	// order of magnitude larger. MEASURED at 1.06e-4 against the SVF mechanisms' 1e-4. The check
	// beside it is what stops that loosening from being vacuous.
	const auto worstDifference = [&](float amount) {
		Settings neg = flat;
		neg.macro1 = -amount;
		Settings pos = flat;
		pos.macro1 = amount;
		const std::vector<float> a = render(neg, input, 128);
		const std::vector<float> b = render(pos, input, 128);
		double worst = 0.0;
		for (size_t i = 0; i < a.size(); i++) {
			worst = std::max(worst, std::abs(static_cast<double>(a[i] - b[i])));
		}
		return worst;
	};
	{
		const double atRest = worstDifference(1e-5f);
		const double atFullTravel = worstDifference(1.0f);
		CAPTURE(atRest);
		CAPTURE(atFullTravel);
		CHECK(atRest < 1e-3);
		// AND THE SAME MEASUREMENT AT FULL TRAVEL IS THOUSANDS OF TIMES LARGER, which is what makes
		// the line above a statement about the sign flip rather than about a knob that does nothing.
		CHECK(atFullTravel > 1000.0 * atRest);
	}

	// AND IT IS A DAMPER RATHER THAN A DIFFERENCE, which is the part a comparison of two renders
	// cannot say. The claim is directional — positive puts a lowpass in the loop and negative puts
	// the pole at Nyquist instead — so what is asserted is the TILT of the resonant peaks across the
	// spectrum, high against low, in decibels. A render that merely "differs" would pass with the
	// damper wired backwards.
	const double lowHz = 300.0;    // the fundamental of the comb
	const double highHz = 8400.0;  // the 28th peak, where a damper has had 28 passes to act
	const auto tiltOf = [&](float damp) {
		const Settings s = combAt(lowHz, 0.85f, damp);
		return dB(gainAt(s, kSampleRate, highHz)) - dB(gainAt(s, kSampleRate, lowHz));
	};
	const double flatTilt = tiltOf(0.0f);
	const double darkTilt = tiltOf(0.7f);
	const double brightTilt = tiltOf(-0.7f);
	CAPTURE(flatTilt);
	CAPTURE(darkTilt);
	CAPTURE(brightTilt);
	CHECK(darkTilt < flatTilt - 3.0);
	CHECK(brightTilt > flatTilt + 3.0);

	// AND THE SAME TILT IS IN THE AUDIO, not only in the evaluator — measured off the real loop, so
	// a damper that existed in the curve and nowhere else would fail here.
	const auto measuredTiltOf = [&](float damp) {
		const std::vector<float> ir = render(combAt(lowHz, 0.85f, damp), impulse(65536), 64);
		return dB(measuredGainAt(ir, highHz)) - dB(measuredGainAt(ir, lowHz));
	};
	CHECK(measuredTiltOf(0.7f) < measuredTiltOf(0.0f) - 3.0);
	CHECK(measuredTiltOf(-0.7f) > measuredTiltOf(0.0f) + 3.0);
}

TEST_CASE("the comb's corner stops where its buffer does, and the curve stops with it") {
	// A DECLARED LIMIT RATHER THAN A LIE. The cutoff knob travels to 20 kHz and the delay line cannot
	// be read closer than six samples, so above sampleRate/6 the comb's corner STOPS. The roadmap's
	// rule for this is that the model must SAY so — kCombMinDelaySamples and combCeilingHz are the
	// saying — and the thing that makes it honest rather than a bug is that the drawn curve stops in
	// the same place, because gainAt and the sample loop call the same combDelaySamples.
	CHECK(combCeilingHz(kSampleRate) == doctest::Approx(8000.0));
	CHECK(combFloorHz(kSampleRate) == doctest::Approx(48000.0 / 4080.0));

	// THE KNOB'S OWN TOP END AND THE FIRST STOP PAST THE CEILING ARE THE SAME COMB, in the curve AND
	// in the audio. Both are asked from ABOVE the ceiling rather than one of them sitting exactly on
	// it: the corner is stored as a float log2, so "exactly 8000 Hz" is 8000 give or take a part in
	// ten million and lands on either side of the clamp by luck. A claim about a clamp is a claim
	// about what happens PAST it.
	const std::vector<float> input = noise(2048);
	CHECK(combDelaySamples(12000.0, kSampleRate) == kCombMinDelaySamples);
	CHECK(combDelaySamples(20000.0, kSampleRate) == kCombMinDelaySamples);
	CHECK(combDelaySamples(combCeilingHz(kSampleRate), kSampleRate)
	      == doctest::Approx(kCombMinDelaySamples));

	const Settings atCeiling = combAt(12000.0, 0.8f);
	const Settings past = combAt(20000.0, 0.8f);
	for (double hz : {100.0, 1000.0, 8000.0, 16000.0}) {
		CAPTURE(hz);
		CHECK(gainAt(atCeiling, kSampleRate, hz) == doctest::Approx(gainAt(past, kSampleRate, hz)));
	}
	CHECK(render(atCeiling, input, 64) == render(past, input, 64));

	// KNOWN-BAD: a corner BELOW the ceiling is a different comb, so the equality above is a clamp
	// rather than a comb that never moves.
	const Settings under = combAt(2000.0, 0.8f);
	CHECK(render(under, input, 64) != render(atCeiling, input, 64));
	CHECK(dB(gainAt(under, kSampleRate, 2000.0)) > dB(gainAt(atCeiling, kSampleRate, 2000.0)) + 3.0);

	// The floor, from the other end. Twenty hertz is inside the line at 48 kHz — the design — and
	// the clamp only bites below 11.8 Hz.
	CHECK(combDelaySamples(20.0, kSampleRate) == doctest::Approx(2400.0));
	CHECK(combDelaySamples(5.0, kSampleRate) == doctest::Approx(kCombMaxDelaySamples));
}

TEST_CASE("a comb comes back to a clean line rather than to the tail it left") {
	// THE RULE stage2_ AND stageButter_ ALREADY CARRY, applied to 32 KB of delay: a section that
	// stops running and starts again must start from zero, or it rings a setting from two type
	// changes ago into the signal.
	//
	// IT IS ALSO WHERE THE OBVIOUS SPELLING IS WRONG. The flag that says the line is running must be
	// dropped the moment the type is not a comb, on every road out of the comb — so the road
	// Comb -> Lowpass -> Comb must come back to a cleared line. That road is what this case walks.
	const std::vector<float> silence(4096, 0.0f);
	const std::vector<float> input = noise(4096);
	const Settings comb = combAt(400.0, 0.95f);

	// A filter that has been ringing, taken away to another type and brought back, against one that
	// was never anything else. Both are then fed silence: whatever comes out is the tail.
	MultimodeFilter loaded;
	loaded.prepare(kSampleRate);
	loaded.setTarget(comb);
	std::vector<float> l = input;
	std::vector<float> r = input;
	loaded.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	Settings away = comb;
	away.type = Type::Lowpass;
	loaded.setTarget(away);
	std::vector<float> al(64, 0.0f);
	std::vector<float> ar(64, 0.0f);
	loaded.processStereo(al.data(), ar.data(), 64);
	loaded.setTarget(comb);

	std::vector<float> tailL = silence;
	std::vector<float> tailR = silence;
	loaded.processStereo(tailL.data(), tailR.data(), static_cast<int>(tailL.size()));

	double loudest = 0.0;
	for (float v : tailL) {
		loudest = std::max(loudest, std::abs(static_cast<double>(v)));
	}
	CAPTURE(loudest);
	CHECK(loudest < 1e-12);

	// KNOWN-BAD FOR THE INSTRUMENT: the same filter WITHOUT the trip away is still ringing, so
	// "silence in, silence out" is a fact about the clear rather than about a comb that never rings.
	MultimodeFilter ringing;
	ringing.prepare(kSampleRate);
	ringing.setTarget(comb);
	std::vector<float> rl = input;
	std::vector<float> rr = input;
	ringing.processStereo(rl.data(), rr.data(), static_cast<int>(rl.size()));
	std::vector<float> stillL = silence;
	std::vector<float> stillR = silence;
	ringing.processStereo(stillL.data(), stillR.data(), static_cast<int>(stillL.size()));
	double stillLoudest = 0.0;
	for (float v : stillL) {
		stillLoudest = std::max(stillLoudest, std::abs(static_cast<double>(v)));
	}
	CAPTURE(stillLoudest);
	CHECK(stillLoudest > 1e-3);
}

TEST_CASE("the comb's delay glides rather than stepping, so a cutoff jump does not click") {
	// THE DELAY IS THE ONE QUANTITY IN THIS MODULE THAT MAY NOT STEP ON THE COEFFICIENT TICK. Every
	// other number solve() writes is a coefficient, and a small step in g or k is a small step in the
	// output; a step in the READ POSITION is a step in the signal itself. So the tick sets a
	// per-sample increment and the sample loop walks it.
	//
	// THE FIRST VERSION OF THIS CASE COULD NOT FAIL, and that is why it is written this way. It fed
	// NOISE and asserted that the worst step at the move was no larger than the signal's own worst
	// step elsewhere — but noise already steps full scale sample to sample, so the bound was satisfied
	// by anything at all. The known-bad (delay assigned straight to its target on the tick) went GREEN.
	// A tone has a slew you can divide by; noise has one that swamps you.
	//
	// MEASURED, on the instrument the Spread-glide case above already established. Good build, three
	// octaves jumped mid-note: 2.06 and 2.27 times the tone's own slew going down, 2.37 and 3.17 going
	// up. Same jump with the delay stepped: 3.91 and 4.34 down, 10.10 and 12.68 up. The reference is
	// the DEVICE ITSELF — the same jump on a lowpass through the same smoother — so the bound cannot
	// rot when the smoothing time changes.
	constexpr double kToneHz = 220.0;
	constexpr float kAmplitude = 0.5f;
	const double toneSlew = kAmplitude * 2.0 * std::numbers::pi * kToneHz / kSampleRate;

	const auto worstStepOnJump = [&](Type type, float res, float from, float to) {
		MultimodeFilter filter;
		filter.prepare(kSampleRate);
		Settings s;
		s.enabled = true;
		s.type = type;
		s.resonance = res;
		s.cutoffLog2Hz = from;
		constexpr int kSamples = 8192;
		constexpr int kBlock = 128;
		std::vector<float> out(kSamples);
		for (int i = 0; i < kSamples; i += kBlock) {
			if (i == kSamples / 2) {
				s.cutoffLog2Hz = to;
			}
			filter.setTarget(s);
			for (int k = 0; k < kBlock; k++) {
				out[static_cast<size_t>(i + k)] =
				    kAmplitude
				    * static_cast<float>(
				        std::sin(2.0 * std::numbers::pi * kToneHz * (i + k) / kSampleRate));
			}
			filter.processMono(out.data() + i, kBlock);
		}
		double worst = 0.0;
		for (size_t k = 1; k < out.size(); k++) {
			worst = std::max(worst, static_cast<double>(std::abs(out[k] - out[k - 1])));
		}
		return worst / toneSlew;
	};

	const float lo = static_cast<float>(std::log2(150.0));
	const float hi = static_cast<float>(std::log2(1200.0));
	const double lowpassDown = worstStepOnJump(Type::Lowpass, 0.7f, hi, lo);
	const double lowpassUp = worstStepOnJump(Type::Lowpass, 0.7f, lo, hi);
	CAPTURE(lowpassDown);
	CAPTURE(lowpassUp);

	// THE TWO DEEP RESONANCES, IN BOTH DIRECTIONS, and every one of the four discriminates: each is
	// inside the bound on the good build and outside it with the delay stepped. Shallow resonances
	// are left out on purpose — at 0.5 going down the stepped build measured 2.54 against a bound of
	// 3.04, so that row would have been an assertion that cannot fail, which is the whole fault this
	// case was rewritten to fix.
	for (float res : {0.85f, 0.95f}) {
		CAPTURE(res);
		const double down = worstStepOnJump(Type::Comb, res, hi, lo);
		const double up = worstStepOnJump(Type::Comb, res, lo, hi);
		CAPTURE(down);
		CAPTURE(up);
		CHECK(down < lowpassDown * 2.0);
		CHECK(up < lowpassUp * 2.0);
	}

	// AND THE SECOND ASSERTION, which is what stops the first being vacuous: a bound on a
	// discontinuity is satisfied perfectly by a comb whose corner never moved at all.
	const std::vector<float> input = noise(4096);
	CHECK(render(combAt(150.0, 0.85f), input, 64) != render(combAt(1200.0, 0.85f), input, 64));
}

TEST_CASE("the comb's block size cannot reach the audio, with its delay mid-glide") {
	// THE DEVICE-WIDE INVARIANT, asked of the one control that carries a per-sample ramp. The
	// increment is a function of kTickSamples and of nothing else, and the tick boundaries sit at
	// absolute sample positions — so a ramp scaled by the block length, or a tick counted from the
	// start of each buffer, would fork here.
	const std::vector<float> input = noise(8192);
	const Settings before = combAt(1200.0, 0.85f);
	const Settings after = combAt(150.0, 0.85f);

	const std::vector<float> reference = render(before, after, 1500, input, 256);
	for (int block : {1, 37, 128, 512}) {
		CAPTURE(block);
		CHECK(render(before, after, 1500, input, block) == reference);
	}
	// The settings really did move, so the agreement above is four renders of a filter in motion.
	CHECK(reference != render(before, input, 256));
}

TEST_CASE("Drive reaches the comb by the shared road in front of the loop") {
	// THE HONESTY RULE, ASKED OF THE ONE CONTROL A NEW TYPE CAN SILENTLY KILL. solve() ends the comb's
	// branch with a `return`, and two since-dropped models had branches that returned before the
	// drive stage was set up, leaving the knob dead — a case beside this one is what found it. The
	// comb takes the shared pre-filter shaper (it has no saturation of its own), so the claim here is
	// the same VALUE claim: driving the comb must equal combing the driven signal.
	const std::vector<float> input = noise(4096);
	constexpr float kAmount = 0.8f;
	const float driveGain = std::exp2(kAmount * 4.0f);
	const float driveComp = 1.0f / feathers::dsp::fasttanh(std::min(driveGain, 4.5f));

	std::vector<float> shaped(input.size());
	for (size_t i = 0; i < input.size(); i++) {
		const float t = std::clamp(input[i] * driveGain, -4.5f, 4.5f);
		const float shape = feathers::dsp::fasttanh(t) * driveComp;
		shaped[i] = input[i] + kAmount * (shape - input[i]);
	}

	Settings driven = combAt(1000.0, 0.3f);
	driven.drive = kAmount;
	const Settings dry = combAt(1000.0, 0.3f);

	CHECK(render(driven, input, 64) == render(dry, shaped, 64));
	// The second assertion, which is what proves the first is not vacuous: the shaper is not the
	// identity, so the two settings genuinely differ.
	CHECK(render(driven, input, 64) != render(dry, input, 64));
}

TEST_CASE("the comb's loop cannot run away, at any setting the controls can reach") {
	// A FEEDBACK LOOP IS THE ONE THING IN THIS DEVICE THAT CAN DIVERGE, and every other type here is
	// either a bounded TPT section or a self-clamping nonlinearity. Two things hold it: the loop gain
	// is capped at kCombMaxFeedback, and the damper's own gain is (1 - |a|), so |D| never exceeds one
	// and the round trip never exceeds the cap.
	//
	// SWEPT RATHER THAN ARGUED, over the corners of everything a hand can reach — four sample rates
	// including two the delay line cannot reach 20 Hz at, corners either side of both clamps, and
	// resonance, damp and drive at their ends. 504 settings; MEASURED at zero non-finite samples and a
	// loudest output of 37.3 from a full-scale input, which is the +31 dB a loop gain of 0.98 is
	// arithmetically worth and is the same league as this device's own SVF at resonance 0.98.
	int nonFinite = 0;
	double loudest = 0.0;
	int swept = 0;
	for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
		for (double hz : {5.0, 20.0, 61.7, 440.0, 4000.0, 12000.0, 40000.0}) {
			for (float res : {0.0f, 0.5f, 1.0f}) {
				for (float damp : {-1.0f, 0.0f, 1.0f}) {
					for (float drive : {0.0f, 1.0f}) {
						Settings s = combAt(hz, res, damp);
						s.drive = drive;
						MultimodeFilter filter;
						filter.prepare(sr);
						filter.setTarget(s);
						std::vector<float> left(4096);
						std::vector<float> right(4096);
						for (size_t i = 0; i < left.size(); i++) {
							left[i] = right[i] = static_cast<float>(
							    std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / sr));
						}
						filter.processStereo(left.data(), right.data(), static_cast<int>(left.size()));
						for (float v : left) {
							if (!std::isfinite(v)) {
								nonFinite++;
							}
							loudest = std::max(loudest, std::abs(static_cast<double>(v)));
						}
						swept++;
					}
				}
			}
		}
	}
	CAPTURE(swept);
	CAPTURE(nonFinite);
	CAPTURE(loudest);
	CHECK(swept == 504);
	CHECK(nonFinite == 0);
	CHECK(loudest < 64.0);
	// AND THE SWEEP REALLY DROVE THE LOOP: a bound on the loudest sample is satisfied perfectly by a
	// comb that never resonates.
	CHECK(loudest > 8.0);
}

TEST_CASE("Comb off is a return, not a filter set to neutral") {
	const std::vector<float> input = noise(2048);
	Settings off = combAt(400.0);
	off.enabled = false;
	CHECK(render(off, input, 64) == input);

	Settings on = combAt(400.0);
	CHECK(render(on, input, 64) != input);
}

TEST_CASE("the comb is one filter whether it is handed one channel or two") {
	const std::vector<float> input = noise(2048);
	Settings s = combAt(700.0, 0.85f, 0.4f);
	s.drive = 0.3f;

	MultimodeFilter mono;
	mono.prepare(kSampleRate);
	mono.setTarget(s);
	std::vector<float> m = input;
	for (size_t at = 0; at < m.size(); at += 37) {
		mono.processMono(m.data() + at, static_cast<int>(std::min<size_t>(37, m.size() - at)));
	}
	CHECK(m == render(s, input, 37));
}

TEST_CASE("Spread glides like every other continuous control, instead of clicking") {
	// Moving the filter's Spread with sound playing clicked and left artefacts.
	//
	// THE SMOOTHER'S LIST WAS MISSING ONE ENTRY. tick() glides cutoff, resonance, drive and the two
	// macros, and solve() builds coefficients from those — but it copies the rest of Settings straight
	// off the raw target, and `spread` was in the rest. So the second stage's mix weight AND its corner
	// both stepped, once per coefficient update, while every other knob on the device glided.
	//
	// A REGRESSION FROM ITS OWN PROMOTION: Spread used to arrive through macro1/macro2, which are on
	// that list, and giving it a field of its own quietly took it off.
	//
	// MEASURED AGAINST AN INDEPENDENT REFERENCE — the tone's own per-sample slew — rather than against
	// a remembered number, and located as well as sized, because a discontinuity that is not AT the
	// move is the signal doing its job. Before: 22 to 29 times the slew, on the sample the control
	// moved. After: 1.5 to 2.1, which is where the smoothed cutoff already sat for a comparable jump.
	constexpr double kToneHz = 220.0;
	constexpr float kAmplitude = 0.5f;
	const double toneSlew = kAmplitude * 2.0 * std::numbers::pi * kToneHz / kSampleRate;

	// A control jumped mid-note, delivered the way a host delivers one: per process block.
	const auto worstStepOnJump = [&](float res, bool moveSpread, float from, float to) {
		MultimodeFilter filter;
		filter.prepare(kSampleRate);
		Settings s;
		s.enabled = true;
		s.type = Type::Lowpass;
		s.resonance = res;
		s.cutoffLog2Hz = static_cast<float>(std::log2(1200.0));
		if (moveSpread) {
			s.spread = from;
		}
		else {
			s.cutoffLog2Hz = from;
		}
		constexpr int kSamples = 4096;
		constexpr int kBlock = 128;
		std::vector<float> out(kSamples);
		for (int i = 0; i < kSamples; i += kBlock) {
			if (i == kSamples / 2) {
				(moveSpread ? s.spread : s.cutoffLog2Hz) = to;
			}
			filter.setTarget(s);
			for (int k = 0; k < kBlock; k++) {
				out[static_cast<size_t>(i + k)] = kAmplitude
				    * static_cast<float>(std::sin(2.0 * std::numbers::pi * kToneHz * (i + k) / kSampleRate));
			}
			filter.processMono(out.data() + i, kBlock);
		}
		double worst = 0.0;
		for (size_t k = 1; k < out.size(); k++) {
			worst = std::max(worst, static_cast<double>(std::abs(out[k] - out[k - 1])));
		}
		return worst / toneSlew;
	};

	const float lo = static_cast<float>(std::log2(1200.0));
	const float hi = static_cast<float>(std::log2(4800.0));
	for (float res : {0.3f, 0.7f, 0.95f}) {
		CAPTURE(res);
		// THE REFERENCE IS THE DEVICE ITSELF: a four-octave cutoff jump through the same smoother is a
		// comparable gesture, and whatever that costs is what a glided control costs here. Asserting
		// against it rather than against a constant means the bound cannot rot when the smoothing
		// time changes.
		const double cutoffJump = worstStepOnJump(res, false, lo, hi);
		CHECK(worstStepOnJump(res, true, 0.0f, 1.0f) < cutoffJump * 2.0);
		CHECK(worstStepOnJump(res, true, 0.0f, -1.0f) < cutoffJump * 2.0);
	}

	// AND THE SECOND ASSERTION, which is what stops the first being vacuous: a bound on the
	// discontinuity is satisfied perfectly by a control that does nothing at all, and Spread is dead
	// on four of the twelve types for real reasons. On this one it must still arrive.
	const std::vector<float> input = noise(4096);
	Settings flat;
	flat.enabled = true;
	flat.type = Type::Lowpass;
	flat.cutoffLog2Hz = static_cast<float>(std::log2(1200.0));
	flat.resonance = 0.5f;
	Settings wide = flat;
	wide.spread = 1.0f;
	CHECK(render(flat, input, 64) != render(wide, input, 64));
}

TEST_CASE("what a type declares it wears is what turning the control actually does") {
	// THE HONESTY RULE, ASKED OF THE TABLE ITSELF. TypeTraits is what the card draws its controls from,
	// so a row that overstates a type is a live-looking knob with nothing behind it — and three rows
	// did. Four since-dropped models arrived declaring Spread (none of them ran the cascade its stage
	// mixes over), three of them declaring four slopes (their coefficient functions took no slope),
	// and all of them `drawable`, a field whose own comment says it exists "to be the thing that
	// REFUSES a nonlinear model rather than letting one land and break the contract quietly".
	//
	// So the table is checked against the SAMPLE LOOP rather than trusted: for every type, turning the
	// control must change the rendered output exactly when the row says it can. Add a model and get
	// this wrong and the case says so before a musician does.
	std::vector<float> input = noise(2048);

	for (int i = 0; i < static_cast<int>(Type::kCount); ++i) {
		const Type type = static_cast<Type>(i);
		CAPTURE(i);
		CAPTURE(std::string(kTypeNames[i]));
		const TypeTraits& traits = traitsOf(type);

		Settings base;
		base.enabled = true;
		base.type = type;
		base.cutoffLog2Hz = static_cast<float>(std::log2(1200.0));
		base.resonance = 0.4f;
		base.slope = Slope::db12;
		// THE GAIN SHAPES NEED GAIN BEFORE THE QUESTION MEANS ANYTHING. Bell and the two shelves are
		// algebraically flat at A = 1 — their m-vectors reduce to (1, 0, 0) — so at a Gain macro of
		// zero the filter is a unity pass and NOTHING can change its output, Spread included. Asking
		// there produced three false "dead control" readings on the first run of this case, which is
		// the difference check's own weakness showing up in the instrument rather than the subject.
		if (traits.macro1.kind == MacroKind::Gain) {
			base.macro1 = 0.5f;
		}

		Settings spread = base;
		spread.spread = 1.0f;
		const bool spreadMoves = render(base, input, 64) != render(spread, input, 64);
		CHECK(spreadMoves == spreadSupported(traits));

		// The same question for the slope list: a slope the row offers must not render identically to
		// the one beside it, and one it withholds is not asked about.
		for (Slope other : {Slope::db6, Slope::db18, Slope::db24}) {
			if (!slopeSupported(traits, other) || !slopeSupported(traits, Slope::db12)) {
				continue;
			}
			CAPTURE(kSlopeNames[static_cast<int>(other)]);
			Settings stepped = base;
			stepped.slope = other;
			CHECK(render(base, input, 64) != render(stepped, input, 64));
		}
	}
}

TEST_CASE("Drive reaches the SVF types by the shared road in front of the filter") {
	// THE HONESTY RULE, ASKED OF DRIVE ON THE TYPES THAT RUN THE CASCADE. The knob was once a dead
	// control on two models whose branches returned before the drive setup; those models are gone
	// (licence migration, 2026-09-24), and what is left to pin is that the shared pre-filter shaper
	// is exactly what Drive does to the SVF family — asserted as a VALUE, not as "the output changed".
	const std::vector<float> input = noise(4096);
	constexpr float kAmount = 0.8f;
	const float driveGain = std::exp2(kAmount * 4.0f);
	const float driveComp = 1.0f / feathers::dsp::fasttanh(std::min(driveGain, 4.5f));

	// The intended path, restated independently: a pre-filter shaper applied to the input.
	std::vector<float> shaped(input.size());
	for (size_t i = 0; i < input.size(); i++) {
		const float t = std::clamp(input[i] * driveGain, -4.5f, 4.5f);
		const float shape = feathers::dsp::fasttanh(t) * driveComp;
		shaped[i] = input[i] + kAmount * (shape - input[i]);
	}

	const auto settingsFor = [](Type type, float drive) {
		Settings s;
		s.enabled = true;
		s.type = type;
		s.cutoffLog2Hz = static_cast<float>(std::log2(1000.0));
		s.resonance = 0.3f;
		s.drive = drive;
		return s;
	};

	// THE SHARED STAGE, ASSERTED AS A VALUE rather than as "the output changed". Driving the filter
	// must equal filtering the driven signal — which a `dry != driven` check cannot tell from a shaper
	// applied at the wrong depth, in the wrong place, or to one channel only.
	for (Type type : {Type::Lowpass, Type::Bandpass, Type::Highpass}) {
		CAPTURE(kTypeNames[static_cast<int>(type)]);
		CHECK(render(settingsFor(type, kAmount), input, 64)
		      == render(settingsFor(type, 0.0f), shaped, 64));
		// The second assertion, which is what proves the first is not vacuous: the shaper is not the
		// identity, so the two settings genuinely differ.
		CHECK(render(settingsFor(type, kAmount), input, 64)
		      != render(settingsFor(type, 0.0f), input, 64));
	}
}

// THE DEVICE'S INDEXED VIEW OF ITSELF — the vocabulary that lets a host carry this filter's
// controls without knowing what a filter is.
//
// It exists because, by design, a docked effect's controls are not the plugin's parameters. The
// DAHDSR published this from its first day and the filter published none of it, so this is the
// filter catching up — and the case is aimed at the joins, because the field order now lives in
// exactly one function and everything else reads it.
TEST_CASE("the filter's row is its own indexed view, and it round-trips") {
	using namespace feathers::fx::filter;

	State state;
	state.settings.enabled = true;
	state.settings.type = Type::Bandpass;
	state.settings.cutoffLog2Hz = 9.5f;
	state.settings.resonance = 0.7f;
	state.settings.drive = 0.35f;
	state.settings.macro1 = -0.4f;
	state.settings.macro2 = 0.25f;
	state.settings.slope = Slope::db24;
	state.settings.spread = -0.6f;
	state.keytrack = true;

	SUBCASE("a full row round-trips to the same state") {
		const State back = stateOf(rowOf(state), static_cast<size_t>(Param::kCount));
		CHECK(back == state);
	}

	SUBCASE("keytrack rides the row though it is not in Settings") {
		// Settings deliberately omits it — keytracking decides how many filters there are, which is
		// the owner's architecture — but it IS a declared control and IS patch state, so a device
		// carrying its own state must carry it. Asserted because the two structs disagreeing about
		// this is exactly the kind of omission that loses a setting silently on save.
		const Row row = rowOf(state);
		CHECK(row[static_cast<size_t>(Param::Keytrack)] == doctest::Approx(1.0f));
		state.keytrack = false;
		CHECK(rowOf(state)[static_cast<size_t>(Param::Keytrack)] == doctest::Approx(0.0f));
	}

	SUBCASE("every declared control reads back the value it was set to") {
		// WALKED BY THE DECLARATIONS rather than by a list written here, so a control appended to the
		// module widens this case without anyone remembering to come and say so.
		for (size_t i = 0; i < std::size(kParamDescs); ++i) {
			const auto param = static_cast<Param>(i);
			const feathers::device::ParamDesc& desc = kParamDescs[i];
			CAPTURE(i);
			CAPTURE(std::string(desc.name));
			// A value strictly inside the range, and on a step for a stepped control.
			const double mid = desc.stepped ? std::round((desc.min + desc.max) * 0.5)
			                                : (desc.min + desc.max) * 0.5;
			State s;
			setValueOf(&s, param, static_cast<float>(mid));
			CHECK(valueOf(s, param) == doctest::Approx(static_cast<float>(mid)));
		}
	}

	SUBCASE("a write past a control's end stops at the end") {
		// NOT AT THE DEFAULT. Right for a hand on a knob, and the trap the DAHDSR's own setValueOf
		// records falling into: an out-of-range write that fell through to the file-reading path
		// would snap a dragged knob to the middle instead of stopping it at the top.
		for (size_t i = 0; i < std::size(kParamDescs); ++i) {
			const auto param = static_cast<Param>(i);
			const feathers::device::ParamDesc& desc = kParamDescs[i];
			CAPTURE(i);
			CAPTURE(std::string(desc.name));
			State hi;
			setValueOf(&hi, param, static_cast<float>(desc.max) + 1000.0f);
			CHECK(valueOf(hi, param) == doctest::Approx(static_cast<float>(desc.max)));
			State lo;
			setValueOf(&lo, param, static_cast<float>(desc.min) - 1000.0f);
			CHECK(valueOf(lo, param) == doctest::Approx(static_cast<float>(desc.min)));
		}
	}

	SUBCASE("a short payload defaults the fields it does not reach") {
		// A RECORD WRITTEN BY AN OLDER BUILD is shorter, and what it omits must read as the DECLARED
		// default rather than as zero — zero is a real value for some of these controls and a
		// meaningless one for others.
		const Row row = rowOf(state);
		const State partial = stateOf(row, 2); // Enabled and Type only
		CHECK(partial.settings.enabled == state.settings.enabled);
		CHECK(partial.settings.type == state.settings.type);
		CHECK(partial.settings.cutoffLog2Hz
		      == doctest::Approx(kParamDescs[static_cast<size_t>(Param::Cutoff)].def));
		CHECK(partial.settings.resonance
		      == doctest::Approx(kParamDescs[static_cast<size_t>(Param::Resonance)].def));
		CHECK_FALSE(partial.keytrack);
	}

	SUBCASE("a non-finite write is refused rather than stored") {
		State s;
		const float before = valueOf(s, Param::Cutoff);
		setValueOf(&s, Param::Cutoff, std::numeric_limits<float>::quiet_NaN());
		CHECK(valueOf(s, Param::Cutoff) == doctest::Approx(before));
	}

	SUBCASE("keyOffsetSemitones is not in the row at all") {
		// It is PER-VOICE — set by whoever runs the filter, never patch state. A row that carried it
		// would be claiming to know something only the caller knows.
		state.settings.keyOffsetSemitones = 7.0f;
		const State back = stateOf(rowOf(state), static_cast<size_t>(Param::kCount));
		CHECK(back.settings.keyOffsetSemitones == doctest::Approx(0.0f));
	}
}

TEST_CASE("no morph target is a model that cannot be drawn as an SVF mix") {
	// AN INVARIANT OF A TABLE, PINNED BECAUSE THE TABLE IS EDITED BY PEOPLE ADDING FILTER MODELS.
	//
	// mixVectorOf answers only for the SVF family. The comb is a different topology with its own
	// closed form, and gainAt dispatches it to its own evaluator BEFORE mixVectorOf is reached. So
	// `r.base` can never be it.
	//
	// `r.morph` is the road that could still deliver one, because the morph target is NOT dispatched.
	// Today no macro map names a non-SVF type, so it cannot happen — and nothing else enforces that.
	// The day someone adds a morph target pointing at the comb (or at a model added later),
	// mixVectorOf would answer with the LOWPASS mix and the card would draw a lowpass for it, in
	// silence, with every suite green.
	//
	// This walks the real table rather than restating it, so a new model is covered without anyone
	// remembering to come back — provided it is added to the predicate below, which is the one list.
	using namespace feathers::fx::filter;
	const auto isNotSvfMix = [](Type t) { return t == Type::Comb; };

	size_t checked = 0;
	for (int i = 0; i < static_cast<int>(Type::kCount); ++i) {
		const auto type = static_cast<Type>(i);
		const TypeTraits& traits = traitsOf(type);
		CAPTURE(i);
		for (const MacroMap* map : { &traits.macro1, &traits.macro2 }) {
			CHECK_FALSE(isNotSvfMix(map->morphNeg));
			CHECK_FALSE(isNotSvfMix(map->morphPos));
			++checked;
		}
	}
	// NOT VACUOUS: a table that had lost its entries would pass the loop by never running it.
	CHECK(checked == 2 * static_cast<size_t>(Type::kCount));

	// AND THE NON-SVF TYPE REALLY DOES EXIST, so the predicate above is not testing an empty set.
	CHECK(isNotSvfMix(Type::Comb));
	CHECK(static_cast<int>(Type::kCount) > static_cast<int>(Type::Comb));
}

// ---------------------------------------------------------------------------------------------
// THE OPS TABLE, which is how a swallowed filter is actually driven.
//
// These exist because the table shipped without them and was wrong. `setFields` decodes a row into a
// fresh State, and `keyOffsetSemitones` is NOT in the row — it is per-render context — so every call
// handed the DSP an offset of zero. A filter driven entirely through AudioOps kept filtering, at a
// corner that never followed the note.
//
// THE ASSERTION IS ON THE OFFSET, NOT ON A BUFFER. CLAUDE.md's rule, and this defect is precisely the
// partial effect it was written for: the wrong build still filters, still renders, still differs from
// silence. Only the value separates it from the right one.

namespace {

using namespace feathers;
using namespace feathers::fx::filter;

/// EXACTLY the storage the ops table's own declaration asks for, so a wrong stateBytes cannot hide
/// behind a generously-sized local.
///
/// IT USED TO BE A 4096-BYTE ARRAY and the comb's delay line made that a number to keep chasing. An
/// allocation of precisely `stateBytes`, aligned to precisely `stateAlign`, is the same guarantee
/// with nothing to maintain — and it is a stricter one, because an over-read of even a byte now runs
/// off the end of the block rather than into the slack.
struct OpsHarness {
	const device::AudioOps& ops = kAudioOps;
	std::byte* storage = nullptr;

	OpsHarness() {
		REQUIRE(ops.stateBytes > 0);
		REQUIRE(ops.stateAlign > 0);
		storage = static_cast<std::byte*>(
		    ::operator new(ops.stateBytes, std::align_val_t {ops.stateAlign}));
		ops.construct(storage);
		ops.prepare(storage, 48000.0, 4096);
	}
	~OpsHarness() {
		ops.destruct(storage);
		::operator delete(storage, std::align_val_t {ops.stateAlign});
	}
	OpsHarness(const OpsHarness&) = delete;
	OpsHarness& operator=(const OpsHarness&) = delete;

	void fields(const State& state) {
		const Row row = rowOf(state);
		ops.setFields(storage, row.data(), row.size());
	}
	void note(float pitchSemitones, bool gate = true) {
		ops.setNoteContext(storage, device::NoteContext {pitchSemitones, gate});
	}
	const Settings& target() const { return opsTargetForTest(storage); }
};

State keytracking(bool on) {
	State state {};
	state.settings.enabled = true;
	state.settings.cutoffLog2Hz = 10.0f;
	state.keytrack = on;
	state.poly = true;
	return state;
}

} // namespace

TEST_CASE("an ops-driven filter tracks the note, and the offset is the value the per-voice road computes") {
	OpsHarness h;
	h.fields(keytracking(true));

	// AN OCTAVE ABOVE THE REFERENCE IS TWELVE SEMITONES, restated independently of the device: the
	// per-voice road is `pitch - kKeytrackReferenceKey`, so this is the number that road would produce.
	h.note(static_cast<float>(kKeytrackReferenceKey) + 12.0f);
	CHECK(h.target().keyOffsetSemitones == doctest::Approx(12.0));

	// AND BELOW IT GOES NEGATIVE, so a sign error cannot pass by symmetry.
	h.note(static_cast<float>(kKeytrackReferenceKey) - 7.0f);
	CHECK(h.target().keyOffsetSemitones == doctest::Approx(-7.0));

	// THE OFFSET REACHES THE CORNER THE SAMPLE LOOP RUNS ON, which is what proves the assertions above
	// are not merely bookkeeping: seven semitones down is seven twelfths of an octave in log2(Hz).
	const Settings tracked = h.target();
	CHECK(tracked.cutoffLog2Hz + tracked.keyOffsetSemitones / 12.0f
	      == doctest::Approx(10.0 - 7.0 / 12.0));

	// AND A KNOB MOVED WHILE THE NOTE IS HELD KEEPS IT — the sequence the original defect actually
	// broke, and the reason this is asserted rather than assumed. Fields arrive when a hand moves a
	// control; notes arrive every render. So a build that recomputes only on the note road repairs
	// itself by the next block and is wrong for exactly one — a real glitch, and invisible to every
	// case that happens to end on a note call.
	State moved = keytracking(true);
	moved.settings.cutoffLog2Hz = 9.0f;
	h.fields(moved);
	CHECK(h.target().keyOffsetSemitones == doctest::Approx(-7.0));
	CHECK(h.target().cutoffLog2Hz == doctest::Approx(9.0));
}

TEST_CASE("a comb driven through the ops table is tuned to the note the host delivered") {
	// THE WHOLE ROAD, END TO END, in the configuration a comb is actually FOR: per-voice, keytracked,
	// and reached the way the host reaches it — a row and a note through AudioOps, never by setting
	// Settings by hand. The tuning case above proves the DSP tunes to a key offset; this proves the
	// offset a real host delivers arrives as one, which is a different claim and the one the filter
	// has already been caught getting wrong once (setFields used to zero keyOffsetSemitones).
	for (int key : {36, 60, 84}) {
		CAPTURE(key);
		OpsHarness h;
		State state {};
		state.settings.enabled = true;
		state.settings.type = Type::Comb;
		state.settings.cutoffLog2Hz = static_cast<float>(std::log2(hzOfKey(kKeytrackReferenceKey)));
		state.settings.resonance = 0.9f;
		state.keytrack = true;
		state.poly = true;
		h.fields(state);
		h.note(static_cast<float>(key));

		const double want = hzOfKey(key);
		const double corner = effectiveCutoffHz(h.target());
		CAPTURE(corner);
		CHECK(std::abs(corner - want) < want * 1e-6);
		CHECK(std::abs(combDelaySamples(corner, 48000.0) - 48000.0 / want) < 1e-3);
	}

	// KNOWN-BAD IN THE CASE ITSELF: with keytrack off the same note leaves the corner on the knob, so
	// the assertions above are about the note road rather than about a corner that happens to sit
	// where middle C is.
	{
		OpsHarness h;
		State state {};
		state.settings.enabled = true;
		state.settings.type = Type::Comb;
		state.settings.cutoffLog2Hz = static_cast<float>(std::log2(hzOfKey(kKeytrackReferenceKey)));
		state.keytrack = false;
		state.poly = true;
		h.fields(state);
		h.note(static_cast<float>(84));
		CHECK(std::abs(effectiveCutoffHz(h.target()) - hzOfKey(kKeytrackReferenceKey))
		      < hzOfKey(kKeytrackReferenceKey) * 1e-6);
	}
}

TEST_CASE("keytrack off pins the ops-driven corner to the knob, at any note") {
	OpsHarness h;
	h.fields(keytracking(false));

	// ZERO IS A SETTING, NOT A FALLBACK: every voice filters at the knob's own corner. The case that
	// makes this worth asserting is that the BROKEN build passes it — which is why it is not the only
	// case here.
	h.note(static_cast<float>(kKeytrackReferenceKey) + 24.0f);
	CHECK(h.target().keyOffsetSemitones == doctest::Approx(0.0));
	h.note(static_cast<float>(kKeytrackReferenceKey) - 24.0f);
	CHECK(h.target().keyOffsetSemitones == doctest::Approx(0.0));
}

TEST_CASE("the ops roads latch, so fields and note may arrive in either order") {
	// THE CONTRACT registry.h STATES, and the one a host is most likely to break: notes arrive per
	// render and fields only when a knob moves, so the very first render of a patch delivers them in
	// the order this case checks second.
	const float pitch = static_cast<float>(kKeytrackReferenceKey) + 5.0f;

	OpsHarness fieldsFirst;
	fieldsFirst.fields(keytracking(true));
	fieldsFirst.note(pitch);

	OpsHarness noteFirst;
	noteFirst.note(pitch);
	noteFirst.fields(keytracking(true));

	CHECK(fieldsFirst.target().keyOffsetSemitones == doctest::Approx(5.0));
	CHECK(noteFirst.target().keyOffsetSemitones == doctest::Approx(5.0));
}

TEST_CASE("modulation may leave a knob's range only where the control says it may") {
	// THE RULE THAT USED TO LIVE IN ParamSet::resolved, and then briefly in a setModulatedValueOf of
	// this device's own. It lives in two places now, both of them roads that actually run: the generic
	// walk in Synth::modulatedRowAt applies an offset to a ROW and consults the descriptor before
	// clamping, and `stateOf` preserves what that walk produced instead of clamping it away on the way
	// back in. This case pins the half that is this module's.
	//
	// A KNOB WRITE AND A DECODE ARE DIFFERENT WRITES, which is the whole distinction: a hand stops at
	// the end of the travel, a row may be carrying a value modulation already moved out of range.
	// Both are asserted, because a version of this that simply stopped clamping would pass half of it.
	Row row {};
	State base {};
	row = rowOf(base);

	// THE DECODE ROAD PRESERVES A MODULATED CUTOFF — two octaves past the knob, which is the
	// measurement the original report turned on: with the knob at its own maximum a +2 and a +4 octave
	// offset both moved the corner by exactly nothing.
	row[static_cast<size_t>(Param::Cutoff)] = static_cast<float>(kCutoffMaxLog2 + 2.0);
	CHECK(stateOf(row, static_cast<size_t>(Param::kCount)).settings.cutoffLog2Hz
	      == doctest::Approx(kCutoffMaxLog2 + 2.0));
	// And downward past the bottom by the same rule, so a high tracked note can still be closed.
	row[static_cast<size_t>(Param::Cutoff)] = static_cast<float>(kCutoffMinLog2 - 2.0);
	CHECK(stateOf(row, static_cast<size_t>(Param::kCount)).settings.cutoffLog2Hz
	      == doctest::Approx(kCutoffMinLog2 - 2.0));

	// THE CONTROL, and the reason this is an opt-in rather than a change to the rule: the resonance
	// beside it travels the SAME road and is still held at its end, because its descriptor makes no
	// such claim.
	const device::ParamDesc& res = kParamDescs[static_cast<size_t>(Param::Resonance)];
	REQUIRE_FALSE(res.offsetsMayLeaveRange);
	row[static_cast<size_t>(Param::Resonance)] = static_cast<float>(res.max + 2.0);
	CHECK(stateOf(row, static_cast<size_t>(Param::kCount)).settings.resonance
	      == doctest::Approx(res.max));

	// AND THE KNOB ROAD STILL STOPS AT THE TOP, whatever the decode road allows.
	State knob {};
	setValueOf(&knob, Param::Cutoff, static_cast<float>(kCutoffWideMaxLog2 + 2.0)); // the knob's top since Z8: 24 kHz
	CHECK(knob.settings.cutoffLog2Hz == doctest::Approx(kCutoffWideMaxLog2));

	// AND THE CUTOFF IS THE ONLY CONTROL THAT ASKS, stated as a walk rather than as a fact about one
	// control, so a second control gaining the flag cannot arrive unnoticed.
	for (size_t i = 0; i < static_cast<size_t>(Param::kCount); ++i) {
		CAPTURE(std::string(kParamDescs[i].name));
		CHECK(kParamDescs[i].offsetsMayLeaveRange == (i == static_cast<size_t>(Param::Cutoff)));
	}
}

namespace {

/// FNV-1a OVER THE FLOATS' BITS — a render's identity, to the bit. The bits, not the values: a -0.0 for a +0.0
/// is a change this exists to see.
uint64_t foldBits(uint64_t h, const std::vector<float>& v) {
	for (const float x : v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &x, sizeof bits);
		for (int b = 0; b < 4; ++b) {
			h ^= (bits >> (8 * b)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
	return h;
}

} // namespace

TEST_CASE("stereo modulation's zero cost: the filter renders the bits it rendered before it (pinned at 616d4dad)") {
#if !(defined(__x86_64__) && defined(__linux__))
	MESSAGE("bit identity is pinned on Linux x86-64 only — skipped");
	return;
#endif
	// EVERY ROAD THE MONO EAR TAKES: each type, the cascade at every slope, both macros, drive, spread, a comb,
	// stereo and mono, through the DSP class and through the ops table (the host's road). Captured before any
	// stereo code existed; nothing in KU may move it.
	// THE TYPES THAT EXISTED AT THE PIN — Brickwall HP (backlog NL) was appended after it and has its own cases;
	// stopping short of it is what lets this keep proving that adding it moved no other type's bits.
	uint64_t h = 1469598103934665603ull;
	const std::vector<float> input = noise(4096);
	for (int type = 0; type < static_cast<int>(Type::Brickwall); ++type) {
		for (int slope = 0; slope < static_cast<int>(Slope::kCount); ++slope) {
			Settings s = lowpassAt(700.0);
			s.type = static_cast<Type>(type);
			s.slope = static_cast<Slope>(slope);
			s.macro1 = 0.45f;
			s.macro2 = -0.35f;
			s.drive = 0.25f;
			s.spread = 0.3f;
			Settings moved = s;
			moved.cutoffLog2Hz += 1.5f;
			moved.macro1 = -0.6f;
			h = foldBits(h, render(s, moved, 1500, input, 37));
			MultimodeFilter mono;
			mono.prepare(kSampleRate);
			mono.setTarget(s);
			std::vector<float> m = input;
			mono.processMono(m.data(), static_cast<int>(m.size()));
			h = foldBits(h, m);
		}
	}
	{
		OpsHarness ops;
		State state = keytracking(true);
		state.settings.type = Type::Comb;
		state.settings.resonance = 0.7f;
		state.settings.macro1 = 0.3f;
		ops.fields(state);
		ops.note(static_cast<float>(kKeytrackReferenceKey) + 5.0f);
		std::vector<float> l = input;
		std::vector<float> r = noise(4096, 0x0badf00du);
		ops.ops.processStereo(ops.storage, l.data(), r.data(), static_cast<int>(l.size()));
		h = foldBits(foldBits(h, l), r);
	}
	constexpr uint64_t kPinned = 15789100572263884902ull; // captured at 616d4dad, before any source edit
	CAPTURE(h);
	CHECK(h == kPinned);
}

namespace {

std::array<float, 4> lanesOf(__m128 v) {
	std::array<float, 4> out {};
	_mm_storeu_ps(out.data(), v);
	return out;
}

bool near(float a, float b, float tol) { return std::abs(a - b) <= tol; }

} // namespace

TEST_CASE("the two-lane SVF keeps each ear's gain in its own lane — Bell's k and a shelf's mix") {
	// `_mm_set_ps` takes its lanes HIGH TO LOW. `g` and `res` put L in lane 0; `bellShelfAmp` put R there. Asserted
	// per lane against the Cytomic formulas restated, so a swap cannot pass by symmetry (the two gains differ).
	using SVF = feathers::fx::filter::CytomicSVF;
	constexpr float kSrInv = 1.0f / 48000.0f;
	constexpr float kRes = 0.4f;
	constexpr float kAmpL = 2.0f;
	constexpr float kAmpR = 0.5f;
	const float kBase = 2.0f - 2.0f * kRes;
	SVF bell;
	bell.setCoeff(SVF::Mode::Bell, 1000.0f, 1000.0f, kRes, kRes, kSrInv, kAmpL, kAmpR);
	CHECK(near(lanesOf(bell.k)[0], kBase / kAmpL, 1e-6f));
	CHECK(near(lanesOf(bell.k)[1], kBase / kAmpR, 1e-6f));
	SVF shelf;
	shelf.setCoeff(SVF::Mode::LowShelf, 1000.0f, 1000.0f, kRes, kRes, kSrInv, kAmpL, kAmpR);
	CHECK(near(lanesOf(shelf.m2)[0], kAmpL * kAmpL - 1.0f, 1e-6f));
	CHECK(near(lanesOf(shelf.m2)[1], kAmpR * kAmpR - 1.0f, 1e-6f));
}

TEST_CASE("the per-lane SVF solve gives every lane its own corner, resonance, gain and mode") {
	using SVF = feathers::fx::filter::CytomicSVF;
	constexpr float kSrInv = 1.0f / 48000.0f;
	const std::array<SVF::Mode, 4> modes { SVF::Mode::Lowpass, SVF::Mode::Lowpass, SVF::Mode::Bell, SVF::Mode::Bell };
	const std::array<float, 4> freq { 500.0f, 2000.0f, 500.0f, 2000.0f };
	const std::array<float, 4> res { 0.1f, 0.7f, 0.1f, 0.7f };
	const std::array<float, 4> amp { 1.0f, 1.0f, 2.0f, 0.5f };
	SVF svf;
	svf.setCoeff(modes, freq, res, kSrInv, amp);
	const std::array<float, 4> g = lanesOf(svf.g);
	const std::array<float, 4> k = lanesOf(svf.k);
	for (size_t i = 0; i < 4; ++i) {
		CAPTURE(i);
		// RESTATED: g = tan(π·f/sr) through the same approximant the scalar overload uses; k = 2 − 2r, over A for Bell.
		const float wantG = feathers::dsp::fasttan(static_cast<float>(M_PI * std::clamp(freq[i] * kSrInv, 0.0f, 0.499f)));
		const float kBase = 2.0f - 2.0f * res[i];
		const float wantK = modes[i] == SVF::Mode::Bell ? kBase / amp[i] : kBase;
		CHECK(near(g[i], wantG, 1e-6f));
		CHECK(near(k[i], wantK, 1e-6f));
	}
	// NOT VACUOUS: the lanes really differ.
	CHECK(g[0] != g[1]);
	CHECK(k[2] != k[3]);
}

// STEREO MODULATION (backlog KU): the filter with a right ear. The mono road's bits are the pin above.
TEST_CASE("each ear's SVF lanes carry that ear's corner and resonance — lanes 0/2 left, 1/3 right") {
	MultimodeFilter filter;
	filter.prepare(kSampleRate);
	Settings s = lowpassAt(500.0);
	s.resonance = 0.2f;
	s.right.cutoffLog2Hz = static_cast<float>(std::log2(3000.0));
	s.right.resonance = 0.8f;
	s.rightLive = earBit(Ear::Cutoff) | earBit(Ear::Resonance);
	filter.setTarget(s);
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	filter.processStereo(l.data(), r.data(), 64); // activates and snaps: smoothing is at the target
	const std::array<float, 4> g = lanesOf(filter.stage1ForTest().g);
	const std::array<float, 4> k = lanesOf(filter.stage1ForTest().k);
	// RESTATED: corner = 2^cutoffLog2 (no key offset); g = fasttan(π·min(f/sr, 0.499)); k = 2 − 2·clamp(res, 0, 0.98).
	const auto gOf = [](double hz) {
		return feathers::dsp::fasttan(static_cast<float>(M_PI * std::min(hz / kSampleRate, 0.499)));
	};
	const float gL = gOf(500.0);
	const float gR = gOf(3000.0);
	CHECK(near(g[0], gL, 1e-6f));
	CHECK(near(g[2], gL, 1e-6f));
	CHECK(near(g[1], gR, 1e-6f));
	CHECK(near(g[3], gR, 1e-6f));
	CHECK(near(k[0], 2.0f - 2.0f * 0.2f, 1e-6f));
	CHECK(near(k[1], 2.0f - 2.0f * 0.8f, 1e-6f));
	CHECK(near(k[2], 2.0f - 2.0f * 0.2f, 1e-6f));
	CHECK(near(k[3], 2.0f - 2.0f * 0.8f, 1e-6f));
}

TEST_CASE("with nothing stereo-live the filter solves once, its right ear's twins equal its left") {
	MultimodeFilter filter;
	filter.prepare(kSampleRate);
	Settings s = lowpassAt(900.0);
	s.drive = 0.4f;
	s.macro1 = 0.3f;
	filter.setTarget(s);
	std::vector<float> l = noise(512);
	std::vector<float> r = l;
	filter.processStereo(l.data(), r.data(), 512);
	CHECK(l == r); // same input, mono settings: the two channels are one filter (the pinned case is the stronger proof)
	const std::array<float, 4> g = lanesOf(filter.stage1ForTest().g);
	CHECK(g[0] == g[1]);
}

TEST_CASE("a stereo-live morph macro can put a different partner TYPE in each ear's morph lane") {
	// THE MORPH PARTNER IS PICKED BY THE MACRO'S SIGN (resolve()), so ±macro gives morphNeg in one ear and morphPos in
	// the other. Lowpass spends macro 2 on Morph (Notch below, Bandpass above). Each ear's measured response is
	// asserted against the curve that ear draws — gainAt(left) and gainAt(rightEarOf(s)) — at the tolerance "the
	// curve on the card is the filter that plays" uses.
	constexpr size_t kIrLength = 32768;
	Settings s = lowpassAt(700.0);
	s.resonance = 0.4f;
	s.macro2 = -0.7f;
	s.right.macro2 = 0.7f;
	s.rightLive = earBit(Ear::Macro2);
	MultimodeFilter filter;
	filter.prepare(kSampleRate);
	filter.setTarget(s);
	std::vector<float> l = impulse(kIrLength);
	std::vector<float> r = impulse(kIrLength);
	for (size_t at = 0; at < kIrLength; at += 64) {
		filter.processStereo(l.data() + at, r.data() + at, 64);
	}
	Settings left = s;
	left.rightLive = 0;
	const Settings right = rightEarOf(s);
	CHECK(right.macro2 == 0.7f);
	bool differ = false;
	for (double hz : {50.0, 200.0, 700.0, 1400.0, 3000.0, 8000.0, 16000.0}) {
		CAPTURE(hz);
		const double wantL = gainAt(left, kSampleRate, hz);
		const double wantR = gainAt(right, kSampleRate, hz);
		differ = differ || std::abs(dB(wantL) - dB(wantR)) > 1.0;
		if (dB(wantL) > -50.0) {
			CHECK(std::abs(dB(measuredGainAt(l, hz)) - dB(wantL)) < 0.1);
		}
		if (dB(wantR) > -50.0) {
			CHECK(std::abs(dB(measuredGainAt(r, hz)) - dB(wantR)) < 0.1);
		}
	}
	CHECK(differ); // NOT VACUOUS: the two ears really draw different curves
}

namespace {
/// THE STEADY GAIN OF THE REAL FILTER at `hz`, in dB: a sine through `MultimodeFilter` (prepare, snap, 256-sample blocks),
/// output RMS over input RMS across whole periods of the last second, after two seconds' settling.
double steadyGainDb(double sr, const Settings& s, double hz) {
	MultimodeFilter f;
	f.prepare(sr);
	f.setTarget(s);
	f.snap();
	const int n = static_cast<int>(sr * 3.0);
	std::vector<float> x(static_cast<size_t>(n)), in(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		in[size_t(i)] = x[size_t(i)] = static_cast<float>(0.25 * std::sin(2.0 * std::numbers::pi * hz * i / sr));
	}
	for (int at = 0; at < n; at += 256) {
		f.processMono(x.data() + at, std::min(256, n - at));
	}
	const int from = static_cast<int>(sr * 2.0);
	const int periods = static_cast<int>(std::floor((n - from) * hz / sr));
	const int to = from + static_cast<int>(std::lround(periods * sr / hz));
	double a = 0.0, b = 0.0;
	for (int i = from; i < to; ++i) {
		a += double(x[size_t(i)]) * x[size_t(i)];
		b += double(in[size_t(i)]) * in[size_t(i)];
	}
	return 10.0 * std::log10(a / b);
}
} // namespace

TEST_CASE("Z8: the cutoff knob reaches 0.5 Hz and 24 kHz — Low Pass at the top and High Pass at the bottom are open, by value") {
	// MEASURED BEFORE (20 Hz..20 kHz): Low Pass at the top was -3.0 dB (6 dB/oct) to -9.0 dB (18/24 dB/oct) at 20 kHz, and High
	// Pass at the bottom the same at 20 Hz. The ends are read from the DESCRIPTOR, so the old range is the known-bad.
	const device::ParamDesc& cut = kParamDescs[static_cast<size_t>(Param::Cutoff)];
	for (const double sr : { 44100.0, 48000.0 }) {
		double worst = 0.0;
		for (int sl = 0; sl < static_cast<int>(Slope::kCount); ++sl) {
			for (const float res : { 0.0f, 0.2f, 1.0f }) {
				Settings s;
				s.type = Type::Lowpass;
				s.cutoffLog2Hz = static_cast<float>(cut.max);
				s.slope = static_cast<Slope>(sl);
				s.resonance = res;
				worst = std::max(worst, std::abs(steadyGainDb(sr, s, 20000.0)));
			}
		}
		MESSAGE("sr " << sr << ": Low Pass at the top, worst |gain| at 20 kHz " << worst << " dB");
		CHECK(worst < 0.01);
	}
	for (const double sr : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
		double worst = 0.0;
		for (int sl = 0; sl < static_cast<int>(Slope::kCount); ++sl) {
			for (const float res : { 0.0f, 0.2f, 1.0f }) {
				Settings s;
				s.type = Type::Highpass;
				s.cutoffLog2Hz = static_cast<float>(cut.min);
				s.slope = static_cast<Slope>(sl);
				s.resonance = res;
				worst = std::max(worst, std::abs(steadyGainDb(sr, s, 20.0)));
			}
		}
		MESSAGE("sr " << sr << ": High Pass at the bottom, worst |gain| at 20 Hz " << worst << " dB");
		CHECK(worst < 0.01);
	}
	// THE KNOWN LIMIT, DECIDED: at 96 kHz and above 24 kHz is a real corner, not the vendor's clamp.
	for (const double sr : { 96000.0, 192000.0 }) {
		Settings s;
		s.type = Type::Lowpass;
		s.cutoffLog2Hz = static_cast<float>(cut.max);
		MESSAGE("KNOWN LIMIT, sr " << sr << ": Low Pass at the top (12 dB, res 0.2) is " << steadyGainDb(sr, s, 20000.0) << " dB at 20 kHz");
	}
}

TEST_CASE("Z8: the cutoff's new key, range, default and route span — a route's depth still means the old knob's 9.97 octaves") {
	const device::ParamDesc& cut = kParamDescs[static_cast<size_t>(Param::Cutoff)];
	CHECK(std::string(cut.key) == "filter_cutoff_wide");
	CHECK(std::exp2(cut.min) == doctest::Approx(0.5));
	CHECK(std::exp2(cut.max) == doctest::Approx(24000.0));
	CHECK(std::exp2(cut.def) == doctest::Approx(1000.0)); // the default is unchanged in Hz
	CHECK(cut.modSpan == doctest::Approx(std::log2(20000.0 / 20.0)));
	CHECK(device::routeSpanOf(cut) == doctest::Approx(std::log2(1000.0)));
	CHECK(cut.offsetsMayLeaveRange); // modulation and Keytrack still carry the corner past the knob
	bool retired = false;
	for (const device::RetiredKey& r : kTraits.retiredKeys) {
		retired = retired || std::string(r.key) == "filter_cutoff";
	}
	CHECK(retired);
	// KEYTRACK IS UNAFFECTED: its offset is added after the knob, within the guard, so a high note still opens a corner past
	// the knob's new top.
	Settings s;
	s.cutoffLog2Hz = static_cast<float>(cut.max);
	s.keyOffsetSemitones = 24.0f;
	CHECK(std::abs(steadyGainDb(48000.0, s, 20000.0)) < 0.01);
}

TEST_CASE("Z8 MEASURE: every mode at the knob's new ends (48 kHz, defaults), and the ones that must be transparent are") {
	const device::ParamDesc& cut = kParamDescs[static_cast<size_t>(Param::Cutoff)];
	for (int t = 0; t < static_cast<int>(Type::kCount); ++t) {
		for (const double end : { cut.min, cut.max }) {
			for (const float m1 : { 0.0f, 1.0f }) {
				Settings s;
				s.type = static_cast<Type>(t);
				s.cutoffLog2Hz = static_cast<float>(end);
				s.macro1 = m1;
				const double g20 = steadyGainDb(48000.0, s, 20.0), g1k = steadyGainDb(48000.0, s, 1000.0),
				             g20k = steadyGainDb(48000.0, s, 20000.0);
				MESSAGE(std::string(kTypeNames[t]) << " at " << std::exp2(end) << " Hz, macro 1 " << m1 << ": 20 Hz " << g20 << " dB, 1 kHz "
				                                   << g1k << ", 20 kHz " << g20k);
				// NOTCH AND PEAK (gain shapes centred on the corner) LEAVE THE BAND AT EITHER END: within 0.01 dB everywhere.
				if (s.type == Type::Notch || s.type == Type::Peak) {
					CHECK(std::abs(g20) < 0.01);
					CHECK(std::abs(g1k) < 0.01);
					CHECK(std::abs(g20k) < 0.01);
				}
			}
		}
	}
}

//------------------------------------------------------------------------------------------------//
// NL: THE BRICKWALL HP TYPE — the keytracked elliptic high-pass HARM's High-pass mode shares.        //
//------------------------------------------------------------------------------------------------//

namespace {

/// A FILTER BUILT AND DRIVEN THROUGH ITS OPS TABLE, so the row, the keytrack and the note all take the host's
/// road to the DSP.
struct OpsBrickwall {
	void* mem = nullptr;
	OpsBrickwall(Type type, double cutoffHz, bool keytrack, float note) {
		mem = ::operator new(kAudioOps.stateBytes, std::align_val_t(kAudioOps.stateAlign));
		kAudioOps.construct(mem);
		kAudioOps.prepare(mem, 48000.0, 512);
		State st;
		st.settings.type = type;
		st.settings.cutoffLog2Hz = static_cast<float>(std::log2(cutoffHz));
		st.keytrack = keytrack;
		const Row row = rowOf(st);
		kAudioOps.setFields(mem, row.data(), row.size());
		feathers::device::NoteContext n;
		n.pitchSemitones = note;
		n.gate = true;
		kAudioOps.setNoteContext(mem, n);
	}
	~OpsBrickwall() {
		kAudioOps.destruct(mem);
		::operator delete(mem, std::align_val_t(kAudioOps.stateAlign));
	}
	OpsBrickwall(const OpsBrickwall&) = delete;
	OpsBrickwall& operator=(const OpsBrickwall&) = delete;
	/// Three seconds to settle, then a quadrature correlation over a whole number of periods.
	double gainAt(double freq) {
		constexpr int kBlock = 256;
		const double w = 2.0 * std::numbers::pi * freq / 48000.0;
		const int settle = 3 * 48000;
		const int span = static_cast<int>(std::lround(std::max(20.0, std::round(freq)) * 48000.0 / freq));
		std::array<float, kBlock> l {}, r {};
		std::complex<double> acc {0.0, 0.0};
		for (int at = 0; at < settle + span; at += kBlock) {
			const int n = std::min(kBlock, settle + span - at);
			for (int i = 0; i < n; ++i) {
				l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = static_cast<float>(std::sin(w * (at + i)));
			}
			kAudioOps.processStereo(mem, l.data(), r.data(), n);
			for (int i = 0; i < n; ++i) {
				if (at + i >= settle) {
					acc += static_cast<double>(r[static_cast<size_t>(i)])
					       * std::exp(std::complex<double>(0.0, -w * static_cast<double>(at + i)));
				}
			}
		}
		return 2.0 * std::abs(acc) / static_cast<double>(span);
	}
};

} // namespace

TEST_CASE("NL: Brickwall HP is the LAST type, appended, so every stored label keeps its meaning") {
	CHECK(std::string(kTypeNames[static_cast<size_t>(Type::Brickwall)]) == "Brickwall HP");
	CHECK(static_cast<size_t>(Type::Brickwall) + 1 == static_cast<size_t>(Type::kCount));
	const TypeTraits& t = traitsOf(Type::Brickwall);
	// NO KNOB THAT DOES NOTHING: no macros, no spread, no resonance, one slope.
	CHECK(t.macro1.kind == MacroKind::None);
	CHECK(t.macro2.kind == MacroKind::None);
	CHECK_FALSE(spreadSupported(t));
	CHECK_FALSE(resonanceSupported(t));
	CHECK_FALSE(slopeAppliesTo(Type::Brickwall));
	// And every other type keeps its resonance.
	for (int i = 0; i < static_cast<int>(Type::Brickwall); ++i) {
		CHECK(resonanceSupported(traitsOf(static_cast<Type>(i))));
	}
}

TEST_CASE("NL: the Filter's Brickwall HP keytracks the note — Cutoff at middle C with Keytrack on removes every note's fundamental, measured") {
	// CUTOFF NAMES f0. At the keytrack reference (C4) and Keytrack on, f0 is the played note's own frequency.
	const double c4 = 440.0 * std::exp2((kKeytrackReferenceKey - 69) / 12.0);
	for (const float note : { 33.0f, 45.0f, 64.0f }) {
		const double f0 = 440.0 * std::exp2((static_cast<double>(note) - 69.0) / 12.0);
		CAPTURE(note);
		OpsBrickwall f(Type::Brickwall, c4, true, note);
		(void)f.gainAt(f0); // run it: the smoother has settled and the brickwall is tracking
		// THE VALUE: the brickwall is running at the note.
		CHECK(opsBrickwallCornerForTest(f.mem) == doctest::Approx(f0).epsilon(1e-4));
		CHECK(effectiveCutoffHz(opsTargetForTest(f.mem)) == doctest::Approx(f0).epsilon(1e-4));
		// AND WHAT IT DOES: the fundamental and below at least 60 dB down, the octave within the 0.1 dB ripple —
		// and the drawn curve is the filter that runs.
		const Settings s = opsTargetForTest(f.mem);
		for (const double h : { 0.5, 1.0 }) {
			CAPTURE(h);
			CHECK(20.0 * std::log10(f.gainAt(h * f0)) <= -60.0);
			CHECK(20.0 * std::log10(gainAt(s, 48000.0, h * f0)) <= -60.0);
		}
		for (const double h : { 2.0, 3.0 }) {
			CAPTURE(h);
			const double got = 20.0 * std::log10(f.gainAt(h * f0));
			const double drawn = 20.0 * std::log10(gainAt(s, 48000.0, h * f0));
			CAPTURE(got);
			CAPTURE(drawn);
			CHECK(got >= -0.11);
			CHECK(got <= 0.01);
			CHECK(std::abs(got - drawn) < 0.01);
		}
	}
	SUBCASE("and with Keytrack off it sits at the knob, whatever the note") {
		OpsBrickwall f(Type::Brickwall, 200.0, false, 80.0f);
		(void)f.gainAt(400.0);
		CHECK(opsBrickwallCornerForTest(f.mem) == doctest::Approx(200.0).epsilon(1e-4));
	}
	SUBCASE("and any other type runs no brickwall") {
		OpsBrickwall f(Type::Highpass, c4, true, 45.0f);
		(void)f.gainAt(220.0);
		CHECK(opsBrickwallCornerForTest(f.mem) == 0.0);
	}
}
