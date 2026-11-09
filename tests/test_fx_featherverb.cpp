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
#include "doctest.h"

#include "fx/featherverb/featherverb.h"
#include "fx/featherverb/featherverb_core.h"
#include "device/format_value.h"

#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using feathers::fx::featherverb::Core;

namespace {

/// The reference's unpanned reverb level: `(0x20000000 >> 15) * (0x20000000 >> 14)` with no sidechain.
constexpr int32_t kUnpanned = 1 << 29;
/// The reference's full scale inside its mix (`EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128`).
constexpr double kFirmwareFullScale = 16777216.0;

/// EVERY SETTING THE REFERENCE HAS, in the order `getReverbParamsFromSong` applies them. HPF is not in
/// that function (a menu sets it); it goes after the four it does set, and before the zones.
struct Settings {
	float room = 0.6f;
	float damp = 0.72f;
	float width = 1.0f;
	float hpf = 0.0f;
	float lpf = 1.0f;
	int32_t z1 = 0;
	int32_t z2 = 512;
	int32_t z3 = 0;
	float predelay = 0.0f;
	bool dryMinus = false;
};

template <class R>
void applySongOrder(R& r, const Settings& s) {
	r.setRoomSize(s.room);
	r.setLPF(s.lpf);
	r.setDamping(s.damp);
	r.setWidth(s.width);
	r.setHPF(s.hpf);
	r.setZone1(s.z1);
	r.setZone2(s.z2);
	r.setZone3(s.z3);
	r.setPredelay(s.predelay);
	r.setDryMinus(s.dryMinus);
	r.setReturnLevels(kUnpanned, kUnpanned);
}

/// A deterministic signal in the reference's units: a burst of noise at `level` of full scale, then
/// silence, so both the driven reverb and its free tail are compared.
std::vector<int32_t> burstThenSilence(size_t frames, size_t burst, double level, uint32_t seed) {
	std::vector<int32_t> out(frames, 0);
	uint32_t s = seed;
	for (size_t i = 0; i < burst && i < frames; ++i) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		const double u = static_cast<double>(static_cast<int32_t>(s)) / 2147483648.0;
		out[i] = static_cast<int32_t>(u * level * kFirmwareFullScale);
	}
	return out;
}

} // namespace

TEST_CASE("Featherverb's output ceiling is the ARM conversion's: saturated, never wrapped to the other sign") {
	// A signal hot enough to drive the core past 1/32, where `float * 32 * INT32_MAX` leaves int32.
	// The port states the ARM behaviour: the value must be the saturated one — `smmulr(INT32_MAX, 2^29)`.
	const std::vector<int32_t> input = burstThenSilence(88200, 8820, 60.0, 0x7777u);
	Settings s;
	s.room = 1.0f;
	s.z2 = 7 * 128 + 127;
	std::vector<float> buffer(Core::bufferFloatsFor(44100.0));
	Core core {44100.0, buffer.data()};
	applySongOrder(core, s);
	std::vector<int32_t> in = input;
	int32_t maxOut = 0;
	int32_t minOut = 0;
	for (size_t at = 0; at < in.size(); at += 128) {
		const size_t n = std::min<size_t>(128, in.size() - at);
		std::vector<int32_t> l(n, 0), r(n, 0);
		core.process(in.data() + at, l.data(), r.data(), n);
		for (size_t i = 0; i < n; ++i) {
			maxOut = std::max({maxOut, l[i], r[i]});
			minOut = std::min({minOut, l[i], r[i]});
		}
	}
	const int32_t ceiling =
	    static_cast<int32_t>((static_cast<int64_t>(INT32_MAX) * kUnpanned + 0x80000000LL) >> 32);
	CHECK(maxOut == ceiling);
	CHECK(minOut == -ceiling);
}

//--------------------------------------------------------------------------------------------------//
// THE DEVICE AROUND THE CORE.                                                                        //
//--------------------------------------------------------------------------------------------------//

namespace {

namespace fv = feathers::fx::featherverb;

/// The tail's peak, in dB against the reference's full scale, over [from, to) seconds after a 512-sample
/// burst. The core alone, so the gate cannot end a runaway on the core's behalf.
double tailPeakDb(double sr, int32_t z1, int32_t z2, int32_t z3, float room, double from, double to) {
	std::vector<float> buf(Core::bufferFloatsFor(sr));
	Core c(sr, buf.data());
	c.setRoomSize(room);
	c.setLPF(1.0f);
	c.setDamping(0.72f);
	c.setWidth(1.0f);
	c.setHPF(0.0f);
	c.setZone1(z1);
	c.setZone2(z2);
	c.setZone3(z3);
	c.setPredelay(0.0f);
	c.setDryMinus(false);
	c.setReturnLevels(kUnpanned, kUnpanned);
	const int n = c.bufferFrames();
	const auto total = static_cast<long>(sr * to);
	const auto start = static_cast<long>(sr * from);
	double peak = 0.0;
	std::vector<int32_t> in(static_cast<size_t>(n)), l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
	for (long f = 0; f < total; f += n) {
		std::fill(in.begin(), in.end(), 0);
		std::fill(l.begin(), l.end(), 0);
		std::fill(r.begin(), r.end(), 0);
		for (int i = 0; i < n; ++i) {
			// A FIXED 882 Hz FOR A FIXED 11.6 ms, at any rate: the first draft wrote the burst per SAMPLE
			// (0.02 of the rate), so at 96 kHz it rang the room an octave higher and "decayed faster".
			if (static_cast<double>(f + i) < 0.0116 * sr) {
				in[static_cast<size_t>(i)] = static_cast<int32_t>(0.8 * kFirmwareFullScale
				                                                  * std::sin(2.0 * 3.14159265358979 * 882.0 * static_cast<double>(f + i) / sr));
			}
		}
		c.process(in.data(), l.data(), r.data(), static_cast<size_t>(n));
		if (f >= start) {
			for (int i = 0; i < n; ++i) {
				peak = std::max(peak, static_cast<double>(std::abs(l[static_cast<size_t>(i)])));
			}
		}
	}
	return 20.0 * std::log10(peak / kFirmwareFullScale + 1e-30);
}

} // namespace

TEST_CASE("Featherverb's Vast does not run away at other sample rates (the multi-tap offsets keep their parity)") {
	// MEASURED before the fix: at 48 kHz, 24 of 48 Vast settings climbed to the output ceiling after the
	// input stopped; at 96 kHz, 36 of 48. At 44.1 kHz, none. The cause was an offset scaled from ODD
	// (509) to EVEN (554), which switched on a multi-tap write the firmware never reads.
	for (const double sr : {48000.0, 96000.0}) {
		for (const int32_t inMode : {0, 127}) {
			for (const int32_t z3 : {300, 614, 1023}) {
				CAPTURE(sr);
				CAPTURE(inMode);
				CAPTURE(z3);
				const double late = tailPeakDb(sr, 614, 7 * 128 + inMode, z3, 0.84f, 20.0, 25.0);
				CHECK(late < -60.0);
			}
		}
	}
}

TEST_CASE("Featherverb's decay is a TIME: a room rings as long at 48 and 96 kHz as at 44.1") {
	// By design the decay scales with the rate, so a tail must take the same seconds to fall. Measured as the
	// time the tail's peak takes to fall below -60 dB, in 50 ms steps; within 10% of 44.1 kHz's.
	const auto fallTime = [](double sr, int32_t z2) {
		for (double t = 0.1; t < 20.0; t += 0.05) {
			if (tailPeakDb(sr, 300, z2, 350, 0.6f, t, t + 0.05) < -60.0) {
				return t;
			}
		}
		return 20.0;
	};
	for (const int32_t z2 : {0 * 128 + 64, 2 * 128 + 64, 4 * 128 + 64}) {
		CAPTURE(z2);
		const double ref = fallTime(44100.0, z2);
		CHECK(ref > 0.2);
		CHECK(ref < 19.0);
		for (const double sr : {48000.0, 96000.0}) {
			CAPTURE(sr);
			const double t = fallTime(sr, z2);
			CAPTURE(ref);
			CAPTURE(t);
			CHECK(std::fabs(t - ref) <= 0.1 * ref + 0.05);
		}
	}
}

TEST_CASE("Featherverb's predelay is a time at every rate") {
	for (const double sr : {44100.0, 48000.0, 96000.0}) {
		std::vector<float> buf(Core::bufferFloatsFor(sr));
		Core c(sr, buf.data());
		c.setPredelay(1.0f);
		CAPTURE(sr);
		CHECK(std::fabs(static_cast<double>(c.predelayLengthForTest()) / sr - 0.05) < 1.0 / sr);
	}
}

TEST_CASE("Featherverb stops as the reference's reverb stops: 12 s after the last send, once quiet") {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = 0;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	std::vector<float> l(4800), r(4800);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = r[i] = 0.5f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
	}
	dev.processStereo(l.data(), r.data(), 4800);
	CHECK(dev.runningForTest());
	// 11.5 s of silence: still running (the gate is 12 s), whatever the tail is doing.
	for (int b = 0; b < 115; ++b) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		dev.processStereo(l.data(), r.data(), 4800);
	}
	CHECK(dev.runningForTest());
	// Past 12 s, a Mouse room has long gone quiet: the gate shuts, and the output is exactly zero.
	for (int b = 0; b < 10; ++b) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		dev.processStereo(l.data(), r.data(), 4800);
	}
	CHECK_FALSE(dev.runningForTest());
	CHECK(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; }));
	// One non-zero frame opens it again, on that frame.
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	l[100] = r[100] = 0.5f;
	dev.processStereo(l.data(), r.data(), 4800);
	CHECK(dev.runningForTest());
}

TEST_CASE("Featherverb's wet is linear below 0 dBFS and never above it — a quiet pluck scales, a hot one is held at the ceiling") {
	// HISTORY, because the decisions went both ways. 2026-09-24: a 0 dBFS CLAMP on the wet was the plain clipping that was
	// heard, so it went, and this case asserted a hot pluck scaled cleanly past 1.0. 2026-09-25, with Owl's Decay putting
	// +18 dBFS on -6 dBFS plucks, the wet was fixed at 0 dBFS — a stereo-linked peak LIMITER (instant attack, 100 ms
	// release: gain reduction, not a squared-off clip) at `kWetCeiling`. So: below it, still linear sample for sample;
	// above it, no sample past it. Sine plucks, the material it was heard on, in every mode.
	for (int mode = 0; mode < fv::kModes; ++mode) {
		const auto render = [mode](float gain, bool owlComp) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setOwlCompressorForTest(owlComp);
			std::vector<float> l(48000 * 2), r;
			for (size_t i = 0; i < l.size(); ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				const double u = std::fmod(t, 0.5);
				l[i] = gain * static_cast<float>(0.9 * std::exp(-u * 8.0) * std::sin(2.0 * 3.14159265358979 * 440.0 * u));
			}
			r = l;
			for (size_t at = 0; at < l.size(); at += 512) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, l.size() - at)));
			}
			return l;
		};
		CAPTURE(mode);
		// BELOW THE CEILING: LINEAR, 4x in is 4x out.
		// OWL'S COMPRESSOR IS LEVEL-DEPENDENT BY DESIGN (at the loop's start, its threshold falling as the servo climbs):
		// the linear claim is about the loop, so Owl is asked it with the compressor off.
		const bool comp = mode != fv::kOwlMenu;
		const std::vector<float> quiet = render(0.1f, comp);
		const std::vector<float> louder = render(0.4f, comp);
		double worst = 0.0;
		float peak = 0.0f;
		for (size_t i = 0; i < louder.size(); ++i) {
			peak = std::max(peak, std::fabs(louder[i]));
			worst = std::max(worst, std::fabs(static_cast<double>(louder[i]) - 4.0 * static_cast<double>(quiet[i])));
		}
		CAPTURE(peak);
		CAPTURE(worst);
		REQUIRE(peak < fv::kWetCeiling); // the premise: the limiter never acted
		// Three modes are level-dependent BY DESIGN: Owl's servo latches an input peak above 1e-4 and floors at 1e-6, and
		// Sky and Vast scale their global feedback by the output's own envelope (`chain_fb_env_scale`).
		const bool envelopeLoop = mode == 5 || mode == fv::kVastMenu || mode == fv::kOwlMenu;
		CHECK(worst < (envelopeLoop ? 2.0e-3 : 1.0e-4) * static_cast<double>(peak));
		// AT THE CEILING: a hot pluck, and not one sample past it — and the limiter DID act (not vacuous).
		const std::vector<float> hot = render(2.0f, true);
		float hotPeak = 0.0f;
		for (const float v : hot) {
			hotPeak = std::max(hotPeak, std::fabs(v));
		}
		CAPTURE(hotPeak);
		CHECK(hotPeak <= fv::kWetCeiling);
		CHECK(hotPeak > 0.9f * fv::kWetCeiling);
	}
}

TEST_CASE("Featherverb's buffer grid stays inside the core's buffer across a prepare at a LOWER rate") {
	// FOUND UNDER ASAN (2026-09-24, while hunting a process-wide crush): the reference buffer's length was
	// read once at the top of `processStereo`, but a lazy rebuild inside the call swaps in a core at the
	// new rate. Down a rate, the grid ended a buffer at the old, longer length and left `gridPos_` past the
	// new one; the next call's `take` went negative and `run` wrote past its stack arrays. The claim is the
	// invariant itself, as a value: after every call the grid sits inside the live buffer.
	fv::Settings s;
	s.mix = 1.0f;
	fv::Featherverb dev;
	dev.setTarget(s);
	const double rates[] = {192000.0, 96000.0, 48000.0, 44100.0, 22050.0};
	std::vector<float> l(1000), r(1000);
	for (const double sr : rates) {
		dev.prepare(sr);
		for (int call = 0; call < 20; ++call) {
			// THE FIRST CALL AFTER `prepare` IS LONG: it is the one the rebuild happens inside, and only a call
			// longer than the new buffer can carry the stale grid past it (a 37-frame first call could not,
			// and the first draft of this case was green against the defect).
			const int n = call == 0 ? 1000 : 37 + call * 23;
			for (int i = 0; i < n; ++i) {
				l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.5f * static_cast<float>(std::sin(0.01 * (call * 1000 + i)));
			}
			dev.processStereo(l.data(), r.data(), n);
			CAPTURE(sr);
			CAPTURE(call);
			CHECK(dev.gridPositionForTest() >= 0);
			CHECK(dev.gridPositionForTest() < dev.coreForTest().bufferFrames());
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// CHARACTER — a rolling phi coordinate onto each mode's own internals.                               //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A device in `mode` at Character `c`, run for a buffer so `beginBuffer` has read the knob.
Core::CharacterReading readingAt(int mode, float c, float predelay = 0.0f) {
	fv::Settings s;
	s.sizeMode = mode;
	s.character = c;
	s.predelay = predelay;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	std::vector<float> l(1024, 0.2f), r(1024, 0.2f);
	dev.processStereo(l.data(), r.data(), 1024);
	return dev.coreForTest().characterForTest();
}

} // namespace

TEST_CASE("Featherverb's Character bank is zero at 0 and nowhere near it elsewhere") {
	const auto zero = fv::characterBankOf(0.0f);
	for (const float t : zero) {
		CHECK(t == 0.0f);
	}
	// Six distinct powers of phi: across a lap they spread, never move together.
	int moved = 0;
	for (float c = 0.05f; c < 1.0f; c += 0.1f) {
		const auto b = fv::characterBankOf(c);
		for (size_t i = 0; i < b.size(); ++i) {
			CHECK(b[i] >= -1.0f);
			CHECK(b[i] <= 1.0f);
			moved += std::fabs(b[i]) > 0.1f ? 1 : 0;
		}
		CHECK(b[0] != b[1]);
	}
	CHECK(moved > 30);
}

TEST_CASE("Featherverb's Character at 0 is the firmware, in every mode, and moves each mode's own internals elsewhere") {
	for (const int mode : fv::kFirmwareMenus) { // the core's modes: Aurora maps Character itself
		CAPTURE(mode);
		const Core::CharacterReading at0 = readingAt(mode, 0.0f);
		// AT 0: every effective value is the firmware's own.
		CHECK(at0.coeffs == at0.firmwareCoeffs);
		CHECK(at0.rotAdd == 0.0f);
		CHECK(at0.loopScale == 1.0f);
		CHECK(at0.wobbleMul == 1.0f);
		CHECK(at0.lfoFreqMul == 1.0f);
		CHECK(at0.kneeMul == 1.0f);
		CHECK(at0.servoMul == 1.0f);
		CHECK(at0.widthMul == 1.0f);
		CHECK(at0.phaserDepth == 0.0f);
		CHECK(at0.flangerDepth == 0.0f);

		// ELSEWHERE: the mode's own mapping, restated from the bank.
		const float c = 0.37f;
		const auto t = fv::characterBankOf(c);
		const Core::CharacterReading r = readingAt(mode, c);
		const auto oct = [](float x) { return std::exp2(x); };
		if (mode == fv::kOwlMenu) { // OWL
			CHECK(r.kneeMul == doctest::Approx(oct(t[0] > 0.0f ? t[0] / 3.0f : t[0])).epsilon(1e-6));
			CHECK(r.servoMul == doctest::Approx(oct(1.5f * t[1])).epsilon(1e-6));
			CHECK(r.envPredelay == std::fabs(t[2]));
			CHECK(r.lfoFreqMul == doctest::Approx(oct(t[4])).epsilon(1e-6));
		}
		else if (mode == fv::kVastMenu) { // VAST
			CHECK(r.wobbleMul == doctest::Approx(oct(t[0])).epsilon(1e-6));
			CHECK(r.loopScale == 1.0f - 0.25f * std::fabs(t[4]));
		}
		else if (mode == 5) { // SKY
			CHECK(r.lfoFreqMul == doctest::Approx(oct(t[1])).epsilon(1e-6));
			CHECK(r.loopScale == 1.0f - 0.25f * std::fabs(t[3]));
		}
		else if (mode == 4) { // FEATHER
			CHECK(r.rotAdd == 0.35f * t[0]);
			for (size_t i = 0; i < 4; ++i) {
				CHECK(r.coeffs[i] == std::clamp(r.firmwareCoeffs[i] + 0.08f * t[2], 0.2f, 0.9f));
			}
		}
		else { // THE ROOMS
			for (size_t i = 0; i < 4; ++i) {
				CHECK(r.coeffs[i] == std::clamp(r.firmwareCoeffs[i] + 0.12f * t[0], 0.2f, 0.9f));
			}
		}
		// WIDTH, every mode.
		CHECK(r.widthMul == doctest::Approx(oct(0.75f * t[6])).epsilon(1e-6));
		// NEVER MORE FEEDBACK than the firmware's.
		CHECK(r.loopScale <= 1.0f);
	}
}

TEST_CASE("Featherverb's Predelay is only a delay on the device: Owl's swell is Character's") {
	// The firmware couples Predelay into the level envelope and Owl's attack; the core still does by
	// default (the parity cases), the device decouples it.
	for (const int mode : fv::kFirmwareMenus) {
		CAPTURE(mode);
		CHECK(readingAt(mode, 0.0f, 1.0f).envPredelay == 0.0f);
	}
	std::vector<float> buf(Core::bufferFloatsFor(44100.0));
	Core core(44100.0, buf.data());
	core.setPredelay(0.7f);
	std::vector<int32_t> in(128, 0), l(128, 0), r(128, 0);
	core.process(in.data(), l.data(), r.data(), 128);
	CHECK(core.characterForTest().envPredelay == 0.7f);
}

TEST_CASE("Featherverb does not run away anywhere on Character's travel") {
	// Character only ever turns feedback DOWN (asserted above), but it also moves diffusion, balance and
	// wobble, which move where a loop's gain peaks. MEASURED here: a 440 Hz pluck, then 20..25 s later.
	int checked = 0;
	for (const int mode : fv::kFirmwareMenus) {
		if (mode == fv::kOwlMenu) {
			continue; // Owl sustains by design (the firmware's; asserted as such elsewhere)
		}
		std::vector<float> coords = {0.13f, 0.37f, 0.61f, 0.89f, 1.7f, 3.3f};
		// AND INSIDE BOTH POCKETS: a modulated delay or allpass moves where a loop's gain peaks.
		for (float c = 0.0f, found = 0.0f; c < 8.0f && found < 2.0f; c += 0.002f) {
			const auto bank = fv::characterBankOf(c);
			if (bank[7] > 0.8f || bank[8] > 0.8f) {
				coords.push_back(c);
				found += 1.0f;
				c += 0.5f;
			}
		}
		for (const float c : coords) {
			CAPTURE(mode);
			CAPTURE(c);
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.size = 1.0f;
			s.decay = 1.0f;
			s.room = 1.0f;
			s.character = c;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			std::vector<float> l(4800), r(4800);
			double late = 0.0;
			for (int b = 0; b < 250; ++b) {
				for (size_t i = 0; i < l.size(); ++i) {
					const double t = static_cast<double>(b * 4800 + static_cast<int>(i)) / 48000.0;
					l[i] = r[i] = t < 0.2 ? static_cast<float>(0.8 * std::sin(2.0 * 3.14159265358979 * 440.0 * t)) : 0.0f;
				}
				dev.processStereo(l.data(), r.data(), 4800);
				if (b >= 200) {
					for (const float v : l) {
						late = std::max(late, static_cast<double>(std::fabs(v)));
					}
				}
			}
			CHECK(20.0 * std::log10(late + 1e-30) < -60.0);
			++checked;
		}
	}
	CHECK(checked >= 56);
}

TEST_CASE("Featherverb's Character pockets: a phaser and a flanger that each appear for a window of the lap") {
	// LOW DUTY (modulated settings with lower duty cycles: stereo phasers and flangers): measured
	// across a fine sweep, each pocket is on for about its duty of the travel and off for the rest.
	int phaserOn = 0, flangerOn = 0;
	[[maybe_unused]] int both = 0;
	constexpr int kSteps = 4000;
	float inPhaser = -1.0f, inFlanger = -1.0f;
	for (int i = 0; i < kSteps; ++i) {
		const float c = 8.0f * static_cast<float>(i) / kSteps;
		const auto b = fv::characterBankOf(c);
		phaserOn += b[7] > 0.0f ? 1 : 0;
		flangerOn += b[8] > 0.0f ? 1 : 0;
		both += (b[7] > 0.0f && b[8] > 0.0f) ? 1 : 0;
		if (inPhaser < 0.0f && b[7] > 0.6f) {
			inPhaser = c;
		}
		if (inFlanger < 0.0f && b[8] > 0.6f) {
			inFlanger = c;
		}
	}
	const double pf = static_cast<double>(phaserOn) / kSteps;
	const double ff = static_cast<double>(flangerOn) / kSteps;
	CAPTURE(pf);
	CAPTURE(ff);
	CHECK(pf > 0.15);
	CHECK(pf < 0.30);
	CHECK(ff > 0.12);
	CHECK(ff < 0.25);
	REQUIRE(inPhaser > 0.0f);
	REQUIRE(inFlanger > 0.0f);

	// INSIDE THE PHASER'S WINDOW its LFO sweeps the allpasses, C0 against C1: read two buffers far apart.
	fv::Settings s;
	s.sizeMode = 0;
	s.character = inPhaser;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	std::vector<float> l(24000, 0.1f), r(24000, 0.1f);
	dev.processStereo(l.data(), r.data(), 128);
	const Core::CharacterReading a = dev.coreForTest().characterForTest();
	dev.processStereo(l.data(), r.data(), 24000);
	const Core::CharacterReading b = dev.coreForTest().characterForTest();
	CHECK(a.phaserDepth > 0.6f);
	CHECK(a.coeffs[0] != b.coeffs[0]);
	// OPPOSITE: whatever C0 gained from the sweep, C1 lost (neither is near a clamp at this depth).
	const auto base = fv::characterBankOf(inPhaser);
	const float fw0 = std::clamp(b.firmwareCoeffs[0] + 0.12f * base[0], 0.2f, 0.9f);
	const float fw1 = std::clamp(b.firmwareCoeffs[1] + 0.12f * base[0], 0.2f, 0.9f);
	CHECK((b.coeffs[0] - fw0) == doctest::Approx(-(b.coeffs[1] - fw1)).epsilon(1e-4));

	// AND IN THE FLANGER'S WINDOW THE EARLY LINES' READ MOVES, by up to 3 ms, and keeps moving.
	fv::Settings fs;
	fs.sizeMode = 0;
	fs.character = inFlanger;
	fv::Featherverb fdev;
	fdev.setTarget(fs);
	fdev.prepare(48000.0);
	std::vector<float> fl(4800, 0.1f), fr(4800, 0.1f);
	fdev.processStereo(fl.data(), fr.data(), 4800);
	const float off1 = fdev.coreForTest().lastFlangeForTest();
	fdev.processStereo(fl.data(), fr.data(), 4800);
	const float off2 = fdev.coreForTest().lastFlangeForTest();
	CHECK(fdev.coreForTest().characterForTest().flangerDepth > 0.6f);
	CHECK(off1 > 0.0f);
	CHECK(off1 <= 0.003f * 48000.0f);
	CHECK(off1 != off2);
}

namespace {

/// OWL'S TAIL, 1..3 s after a 0.1 s noise burst, with only Character's register (t9) and focus (t10) set:
/// its spectral centroid (Hz) and the share of its energy in the strongest 1% of FFT bins.
std::pair<double, double> owlTailShape(float reg, float foc) {
	const double sr = 48000.0;
	std::vector<float> buf(Core::bufferFloatsFor(sr));
	Core c(sr, buf.data());
	c.setRoomSize(0.6f);
	c.setLPF(1.0f);
	c.setDamping(0.72f);
	c.setWidth(1.0f);
	c.setHPF(0.0f);
	c.setZone1(300);
	c.setZone2(6 * 128 + 64);
	c.setZone3(307);
	c.setPredelay(0.0f);
	c.setReturnLevels(kUnpanned, kUnpanned);
	std::array<float, feathers::fx::featherverb::kCharacterTriangles> t {};
	t[9] = reg;
	t[10] = foc;
	c.setCharacter(t);
	const int n = c.bufferFrames();
	std::vector<double> tail;
	std::vector<int32_t> in(static_cast<size_t>(n)), l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
	uint32_t rng = 7;
	for (long f = 0; f < static_cast<long>(sr * 3); f += n) {
		for (int i = 0; i < n; ++i) {
			rng = rng * 1664525u + 1013904223u;
			const bool burst = static_cast<double>(f + i) < sr * 0.1;
			in[static_cast<size_t>(i)] = burst ? static_cast<int32_t>(static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0 * 0.3 * kFirmwareFullScale) : 0;
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0;
		}
		c.process(in.data(), l.data(), r.data(), static_cast<size_t>(n));
		for (int i = 0; i < n; ++i) {
			if (static_cast<double>(f + i) >= sr) {
				tail.push_back(l[static_cast<size_t>(i)] / kFirmwareFullScale);
			}
		}
	}
	constexpr size_t N = 8192;
	std::vector<double> power(N / 2, 0.0);
	for (size_t at = 0; at + N <= tail.size(); at += N / 2) {
		std::vector<std::complex<double>> a(N);
		for (size_t i = 0; i < N; ++i) {
			a[i] = tail[at + i] * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) / (N - 1)));
		}
		for (size_t i = 1, j = 0; i < N; ++i) {
			size_t bit = N >> 1;
			for (; j & bit; bit >>= 1) {
				j ^= bit;
			}
			j ^= bit;
			if (i < j) {
				std::swap(a[i], a[j]);
			}
		}
		for (size_t len = 2; len <= N; len <<= 1) {
			const std::complex<double> wl(std::cos(-2.0 * 3.14159265358979 / static_cast<double>(len)),
			                              std::sin(-2.0 * 3.14159265358979 / static_cast<double>(len)));
			for (size_t i = 0; i < N; i += len) {
				std::complex<double> w(1.0);
				for (size_t k = 0; k < len / 2; ++k) {
					const auto u = a[i + k];
					const auto v = a[i + k + len / 2] * w;
					a[i + k] = u + v;
					a[i + k + len / 2] = u - v;
					w *= wl;
				}
			}
		}
		for (size_t k = 0; k < N / 2; ++k) {
			power[k] += std::norm(a[k]);
		}
	}
	double total = 0.0, moment = 0.0;
	for (size_t k = 1; k < N / 2; ++k) {
		total += power[k];
		moment += power[k] * static_cast<double>(k);
	}
	std::vector<double> sorted(power.begin() + 1, power.end());
	std::sort(sorted.rbegin(), sorted.rend());
	double top = 0.0;
	for (size_t k = 0; k < sorted.size() / 100; ++k) {
		top += sorted[k];
	}
	return {moment / total * sr / N, top / total};
}

} // namespace

TEST_CASE("Featherverb's Owl: Character's register moves where it rings, and focus how peaky") {
	// Owl's resonance character is adjustable: it can ring lower or higher, and be more or less intrinsically peaky.
	// MEASURED at Decay 30% (register at ±1 octave): -1/0/+1 puts the
	// centroid at 653/798/1114 Hz; focus -1/0/+1 puts 70/76/84% of the energy in the top 1% of bins.
	const auto low = owlTailShape(-1.0f, 0.0f);
	const auto mid = owlTailShape(0.0f, 0.0f);
	const auto high = owlTailShape(1.0f, 0.0f);
	CAPTURE(low.first);
	CAPTURE(mid.first);
	CAPTURE(high.first);
	CHECK(low.first < 0.9 * mid.first);
	CHECK(high.first > 1.3 * mid.first);

	const auto smooth = owlTailShape(0.0f, -1.0f);
	const auto peaky = owlTailShape(0.0f, 1.0f);
	CAPTURE(smooth.second);
	CAPTURE(mid.second);
	CAPTURE(peaky.second);
	CHECK(smooth.second < mid.second - 0.04);
	CHECK(peaky.second > mid.second + 0.05);
	// FOCUS LEAVES THE REGISTER ALONE (within 5%).
	CHECK(std::fabs(peaky.first - mid.first) < 0.05 * mid.first);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG LB: HPF AND LPF OVER THE FULL RANGE                                                    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A device at `sr` with these filter corners (log2 Hz), one block run so its settings reach the core.
struct FilterRig {
	fv::Featherverb dev;
	FilterRig(double sr, float hpLog2, float lpLog2, int mode = 4, bool oversample = false) {
		fv::Settings s;
		s.hpfLog2Hz = hpLog2;
		s.lpfLog2Hz = lpLog2;
		s.sizeMode = mode;
		s.oversample = oversample;
		dev.setTarget(s);
		dev.prepare(sr);
		std::vector<float> l(256, 0.0f), r(256, 0.0f);
		l[0] = r[0] = 0.1f; // something sent, so the gate opens and the core is built (`rebuild`)
		dev.processStereo(l.data(), r.data(), 256);
	}
};

/// THE INDEPENDENT RESTATEMENT of the intended filter: a trapezoidal (bilinear) one-pole with its corner prewarped,
/// `g = tan(π f / rate)`, `G = g / (1 + g)` — and one step of it, the lowpass (the highpass is the input minus it).
double tptGainAt(double hz, double rate) {
	const double g = std::tan(3.14159265358979323846 * hz / rate);
	return g / (1.0 + g);
}
/// The firmware's one-pole with its pole at `hz`: `1 - e^(-2π f / rate)` (the lowpass's coefficient).
double poleCoeff(double hz, double rate) { return 1.0 - std::exp(-2.0 * 3.14159265358979323846 * hz / rate); }
double tptLow(double x, double& s, double G) {
	const double v = (x - s) * G;
	const double lp = v + s;
	s = lp + v;
	return lp;
}

/// A FILTER'S GAIN AT `f`, MEASURED: a second of sine through `step`, the first half discarded, the rest demodulated
/// at exactly `f` against the input's own demodulation. dB.
template <class Step>
double measuredGainDb(Step step, double f, double rate) {
	const auto n = static_cast<size_t>(rate);
	std::complex<double> in(0.0, 0.0), out(0.0, 0.0);
	for (size_t i = 0; i < n; ++i) {
		const double ph = 2.0 * 3.14159265358979323846 * f * static_cast<double>(i) / rate;
		const double x = std::sin(ph);
		const double y = step(x);
		if (i >= n / 2) {
			const std::complex<double> w(std::cos(ph), -std::sin(ph));
			in += x * w;
			out += y * w;
		}
	}
	return 20.0 * std::log10(std::abs(out) / std::abs(in));
}

} // namespace

TEST_CASE("Featherverb's HPF and LPF are corners in Hz over 20 Hz..20 kHz, on a log knob, and print them") {
	for (const fv::Param p : { fv::Param::Hpf, fv::Param::Lpf }) {
		const auto& d = fv::kParamDescs[static_cast<size_t>(p)];
		const std::string key = d.key;
		CAPTURE(key);
		CHECK(d.unit == feathers::device::Unit::LogHertz); // the value IS log2(Hz): equal travel per octave
		CHECK(std::exp2(d.min) == doctest::Approx(20.0).epsilon(1e-12));
		CHECK(std::exp2(d.max) == doctest::Approx(20000.0).epsilon(1e-12));
		CHECK(feathers::device::formatValue(d, d.min) == "20");
		CHECK(feathers::device::formatValue(d, d.max) == "20k");
		CHECK(d.modulatable);
	}
	// THE KEYS NAME THE NEW MEANING, AND THE OLD ONES ARE RETIRED (CLAUDE.md, the schema ledger).
	CHECK(std::string(fv::kParamDescs[static_cast<size_t>(fv::Param::Hpf)].key) == "featherverb_hpf_hz");
	CHECK(std::string(fv::kParamDescs[static_cast<size_t>(fv::Param::Lpf)].key) == "featherverb_lpf_hz");
	int retired = 0;
	for (const auto& k : fv::kRetiredKeys) {
		retired += std::string(k.key) == "featherverb_hpf" || std::string(k.key) == "featherverb_lpf" ? 1 : 0;
	}
	CHECK(retired == 2);
}


TEST_CASE("Featherverb's HPF and LPF: each is set by its corner in Hz at the core's own rate, in every mode, and is 3 dB down there") {
	// The core's own rate: the host's, or twice it oversampled.
	struct Venue {
		double host;
		bool oversample;
	};
	const double lo = fv::kFilterMinLog2, hi = fv::kFilterMaxLog2;
	for (const Venue v : { Venue {44100.0, false}, Venue {48000.0, false}, Venue {96000.0, false}, Venue {192000.0, false},
	                       Venue {48000.0, true} }) {
		const double rate = v.host * (v.oversample ? 2.0 : 1.0);
		for (const int mode : { 0, 4, 5, fv::kVastMenu, fv::kOwlMenu }) {
			for (const double t : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
				const auto x = static_cast<float>(lo + t * (hi - lo));
				FilterRig rig(v.host, x, x, mode, v.oversample);
				REQUIRE(rig.dev.coreOversampleForTest() == (v.oversample ? 2 : 1));
				const double hz = std::exp2(static_cast<double>(x));
				CAPTURE(rate);
				CAPTURE(mode);
				CAPTURE(hz);
				const double hp = rig.dev.coreForTest().hpGainForTest();
				const double lp = rig.dev.coreForTest().lpCoeffForTest();
				// THE VALUES: the highpass's trapezoid gain and the lowpass's pole, each the corner's at the core's own rate.
				CHECK(hp == doctest::Approx(tptGainAt(hz, rate)).epsilon(1e-6));
				CHECK(lp == doctest::Approx(poleCoeff(hz, rate)).epsilon(1e-6));
				// AND WHAT THEY DO, run on those values. The highpass is 3 dB down at the knob's corner, at every corner from
				// 20 Hz to 20 kHz (MEASURED within 0.01 dB). The lowpass is too, within 0.15 dB, wherever a one-pole can put
				// its corner (a twentieth of the rate); above that it is opener than its label, never darker.
				if (t == 0.0 || t == 0.5 || t == 1.0) {
					double sl = 0.0, sh = 0.0;
					const double high = measuredGainDb([&](double in) { return in - tptLow(in, sh, hp); }, hz, rate);
					const double low = measuredGainDb([&](double in) { sl += lp * (in - sl); return sl; }, hz, rate);
					CAPTURE(high);
					CAPTURE(low);
					CHECK(std::fabs(high + 3.0103) < 0.02);
					CHECK(low > -3.0103 - 0.02);
					if (hz <= rate / 20.0) {
						CHECK(std::fabs(low + 3.0103) < 0.15);
					}
				}
			}
		}
	}
}

TEST_CASE("Featherverb's filters at the firmware's knob positions match its one-poles, within a tenth of a dB to 10 kHz") {
	// The HPF at the firmware's 0 is a pole at 35.18 Hz, set here by hand: the default moved to 150 Hz (backlog MR). The
	// LPF's default is still the firmware's 1.
	const double firmwareHpLog2 = std::log2(-std::log(0.995) * 44100.0 / (2.0 * 3.14159265358979323846));
	for (const double sr : { 44100.0, 48000.0, 96000.0 }) {
		CAPTURE(sr);
		fv::Settings s;
		FilterRig rig(sr, static_cast<float>(firmwareHpLog2), s.lpfLog2Hz);
		const double hpG = rig.dev.coreForTest().hpGainForTest();
		const double lpNew = rig.dev.coreForTest().lpCoeffForTest();
		// THE FIRMWARE'S KNOBS AT ITS SONG DEFAULTS (HPF 0, LPF 1), on a bare core at the same rate: its coefficients.
		std::vector<float> buf(Core::bufferFloatsFor(sr));
		Core fw {sr, buf.data()};
		fw.setHPF(0.0f);
		fw.setLPF(1.0f);
		const double hpA = fw.firmwareHpAlphaForTest();
		const double lpA = fw.firmwareLpCoeffForTest();
		REQUIRE(fw.hpGainForTest() < 0.0f); // the firmware's own one-poles, not the Hz road
		REQUIRE(fw.lpCoeffForTest() < 0.0f);
		// The two roads, restated: the firmware's highpass `in - s` then `s += a (in - s)`, its lowpass `s += a (in - s)`.
		for (const double f : { 20.0, 35.0, 60.0, 200.0 }) {
			double sNew = 0.0, sOld = 0.0;
			const double now = measuredGainDb([&](double in) { return in - tptLow(in, sNew, hpG); }, f, sr);
			const double was = measuredGainDb([&](double in) { const double y = in - sOld; sOld += hpA * y; return y; }, f, sr);
			MESSAGE(sr << " Hz, HPF at " << f << " Hz: default " << now << " dB, the firmware's " << was << " dB");
			// MEASURED: a constant 0.02 dB, the firmware's highpass running that much over unity above its corner.
			CHECK(std::fabs(now - was) < 0.03);
		}
		for (const double f : { 1000.0, 5000.0, 10000.0, 16000.0 }) {
			double sNew = 0.0, sOld = 0.0;
			const double now = measuredGainDb([&](double in) { sNew += lpNew * (in - sNew); return sNew; }, f, sr);
			const double was = measuredGainDb([&](double in) { sOld += lpA * (in - sOld); return sOld; }, f, sr);
			MESSAGE(sr << " Hz, LPF at " << f << " Hz: default " << now << " dB, the firmware's " << was << " dB");
			// The LPF's top is 20 kHz against the firmware's pole at 21.03 kHz. MEASURED darker by 0.02 dB at 5 kHz, 0.07..0.08
			// at 10 kHz and 0.12..0.16 at 16 kHz (44.1, 48, 96 kHz).
			CHECK(std::fabs(now - was) < (f <= 10000.0 ? 0.1 : 0.2));
		}
	}
	CHECK(fv::kLpfDefaultLog2 == fv::kFilterMaxLog2);
}

TEST_CASE("a fresh Featherverb's HPF reads 150 Hz: the settings, the descriptor and the core's one-pole") {
	// Backlog MR: Featherverb's HPF defaults to 150 Hz. Same key, same meaning; only the start moves.
	CHECK(std::exp2(static_cast<double>(fv::Settings {}.hpfLog2Hz)) == doctest::Approx(150.0).epsilon(1e-6));
	const auto& desc = fv::kParamDescs[static_cast<size_t>(fv::Param::Hpf)];
	CHECK(std::exp2(desc.def) == doctest::Approx(150.0).epsilon(1e-12));
	// And what the core runs: its trapezoidal gain at 150 Hz, restated independently, on a device built from defaults.
	for (const double sr : { 44100.0, 48000.0, 96000.0 }) {
		CAPTURE(sr);
		fv::Settings s;
		FilterRig rig(sr, s.hpfLog2Hz, s.lpfLog2Hz);
		const double g = rig.dev.coreForTest().hpGainForTest();
		CAPTURE(g);
		CHECK(std::fabs(g / tptGainAt(150.0, sr) - 1.0) < 1e-5); // relative: doctest's Approx adds a scale of 1

	}
}

TEST_CASE("Featherverb's Size Mode menu lists Owl after Vast, and each still selects its firmware zone") {
	// Owl comes after Vast in the menu, because with the feedback servo Owl is the vaster of the two.
	CHECK(std::string(fv::kSizeModeNames[fv::kVastMenu]) == "Vast");
	CHECK(std::string(fv::kSizeModeNames[fv::kOwlMenu]) == "Owl");
	for (const int menu : fv::kFirmwareMenus) {
		fv::Settings s;
		s.sizeMode = menu;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		std::vector<float> l(512, 0.1f), r(512, 0.1f);
		dev.processStereo(l.data(), r.data(), 512);
		CAPTURE(menu);
		const int topology = dev.coreForTest().topologyForTest();
		const int want = menu == fv::kOwlMenu ? 6 : menu == fv::kVastMenu ? 7 : (menu < 4 ? 0 : menu);
		CHECK(topology == want);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE CONVOLUTION (backlog JD, first pass).                                                          //
//--------------------------------------------------------------------------------------------------//

#include "fx/conv/convolver.h"
#include "fx/conv/test_ir.h"

namespace {

namespace cv = feathers::fx::conv;

/// Plucks, 3 s at 48 kHz, stereo-identical.
std::vector<float> plucks() {
	std::vector<float> x(48000 * 3);
	for (size_t i = 0; i < x.size(); ++i) {
		const double u = std::fmod(static_cast<double>(i) / 48000.0, 0.5);
		x[i] = static_cast<float>(0.5 * std::exp(-u * 8.0) * std::sin(2.0 * 3.14159265358979 * 330.0 * u));
	}
	return x;
}

/// A device in `convMode` at Blend `b`, Mix 1, run over `x` (L = R = x) in odd host blocks.
std::pair<std::vector<float>, std::vector<float>> renderConv(int convMode, float b, const std::vector<float>& x,
                                                            uint64_t* ffts = nullptr, bool* reverbRan = nullptr) {
	fv::Settings s;
	s.mix = 1.0f;
	s.convMode = convMode;
	s.convBlend = b;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setWetLimiterForTest(false); // the convolution's arithmetic against its reference: the wet ceiling is not the claim
	// THE WET LIMITER'S LOOKAHEAD (backlog LD) delays the whole wet by `lookahead` samples, limiting or not: the input
	// runs that much longer (zeros) and the output is read that much later, so what returns is on the input's timing.
	const size_t lookahead = dev.wetLimiterForTest().lookahead();
	std::vector<float> l = x, r = x;
	l.resize(x.size() + lookahead, 0.0f);
	r.resize(x.size() + lookahead, 0.0f);
	bool ran = false;
	const size_t blocks[] = {1, 37, 128, 500, 3, 255, 64, 1000};
	for (size_t at = 0, k = 0; at < l.size(); ++k) {
		const size_t n = std::min(blocks[k % 8], l.size() - at);
		dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(n));
		ran = ran || dev.runningForTest();
		at += n;
	}
	l.erase(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(lookahead));
	r.erase(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(lookahead));
	if (ffts != nullptr) {
		*ffts = dev.convolverForTest().forwardFftsForTest();
	}
	if (reverbRan != nullptr) {
		*reverbRan = ran;
	}
	return {l, r};
}

/// THE REFERENCE: the generated IR at 48 kHz, through a standalone convolver, on `in`.
std::pair<std::vector<float>, std::vector<float>> referenceConv(const std::vector<float>& in) {
	const cv::GeneratedIr ir = cv::generateTestIr(48000.0, fv::kConvIrSeconds);
	cv::Convolver c;
	c.prepare(ir.left.size());
	c.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	std::vector<float> l(in.size(), 0.0f), r(in.size(), 0.0f);
	c.process(in.data(), l.data(), r.data(), in.size());
	return {l, r};
}

double worstDiff(const std::vector<float>& a, const std::vector<double>& b) {
	double w = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		w = std::max(w, std::fabs(static_cast<double>(a[i]) - b[i]));
	}
	return w;
}

} // namespace

TEST_CASE("Featherverb's conv Off costs nothing: the convolver never runs") {
	uint64_t ffts = 1;
	(void)renderConv(fv::kConvOff, 0.5f, plucks(), &ffts);
	CHECK(ffts == 0);
}

TEST_CASE("Featherverb's Conv Only is the convolution of the input, and the reverb never runs") {
	// TO FLOAT ROUNDING, NOT TO THE BIT, and for a measured reason: the device's first host block is one
	// zero sample, which the convolver SKIPS (silence costs nothing), so its 128-frame blocks sit one
	// sample off the reference's. Mathematically the same convolution; the FFT partitions round
	// differently. MEASURED: 4.2e-6 at worst, on a sample of 1.36.
	const std::vector<float> x = plucks();
	uint64_t ffts = 0;
	bool reverbRan = true;
	const auto [l, r] = renderConv(fv::kConvOnly, 0.5f, x, &ffts, &reverbRan);
	const auto [wl, wr] = referenceConv(x); // mid of L = R = x is x
	std::vector<double> dl(wl.begin(), wl.end()), dr(wr.begin(), wr.end());
	double peak = 0.0;
	for (const float v : wl) {
		peak = std::max(peak, static_cast<double>(std::fabs(v)));
	}
	CHECK(peak > 0.5);
	CHECK(worstDiff(l, dl) < 1.0e-5 * peak);
	CHECK(worstDiff(r, dr) < 1.0e-5 * peak);
	CHECK_FALSE(reverbRan);
	CHECK(ffts > 1000);
}

TEST_CASE("Featherverb's Parallel and Series: the reverb's share of the wet, plus the convolution of the right signal") {
	const std::vector<float> x = plucks();
	const float b = 0.3f;
	const auto [offL, offR] = renderConv(fv::kConvOff, b, x); // the reverb alone
	SUBCASE("Parallel: (1 - b)·reverb + conv(b · input)") {
		const auto [l, r] = renderConv(fv::kConvParallel, b, x);
		std::vector<float> scaled(x.size());
		for (size_t i = 0; i < x.size(); ++i) {
			scaled[i] = b * x[i];
		}
		const auto [cl, cr] = referenceConv(scaled);
		std::vector<double> wantL(x.size()), wantR(x.size());
		for (size_t i = 0; i < x.size(); ++i) {
			wantL[i] = (1.0 - b) * offL[i] + cl[i];
			wantR[i] = (1.0 - b) * offR[i] + cr[i];
		}
		CHECK(worstDiff(l, wantL) < 1.0e-5);
		CHECK(worstDiff(r, wantR) < 1.0e-5);
	}
	SUBCASE("Series: (1 - b)·reverb + conv(b · the reverb's mid)") {
		const auto [l, r] = renderConv(fv::kConvSeries, b, x);
		std::vector<float> revMid(x.size());
		for (size_t i = 0; i < x.size(); ++i) {
			revMid[i] = b * (0.5f * (offL[i] + offR[i]));
		}
		const auto [cl, cr] = referenceConv(revMid);
		std::vector<double> wantL(x.size()), wantR(x.size());
		for (size_t i = 0; i < x.size(); ++i) {
			wantL[i] = (1.0 - b) * offL[i] + cl[i];
			wantR[i] = (1.0 - b) * offR[i] + cr[i];
		}
		CHECK(worstDiff(l, wantL) < 1.0e-5);
		CHECK(worstDiff(r, wantR) < 1.0e-5);
	}
}

TEST_CASE("Featherverb's convolution stops computing once its tail has passed, and a mode change does not click") {
	fv::Settings s;
	s.mix = 1.0f;
	s.convMode = fv::kConvParallel;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	const std::vector<float> x = plucks();
	std::vector<float> l = x, r = x;
	// Parallel for 1.5 s, then Series (mid-pluck), then Off for the rest.
	double worstStep = 0.0, worstStepAtSwitch = 0.0;
	float prev = 0.0f;
	for (size_t at = 0; at < x.size(); at += 480) {
		if (at == 48000 * 3 / 4) {
			s.convMode = fv::kConvSeries;
			dev.setTarget(s);
		}
		if (at == 48000) {
			s.convMode = fv::kConvOff;
			dev.setTarget(s);
		}
		dev.processStereo(l.data() + at, r.data() + at, 480);
		for (size_t i = at; i < at + 480; ++i) {
			const double step = std::fabs(static_cast<double>(l[i]) - prev);
			prev = l[i];
			const bool nearSwitch = (i >= 36000 && i < 36000 + 1000);
			(nearSwitch ? worstStepAtSwitch : worstStep) = std::max(nearSwitch ? worstStepAtSwitch : worstStep, step);
		}
	}
	CAPTURE(worstStep);
	CAPTURE(worstStepAtSwitch);
	// NO CLICK: the switch's largest sample-to-sample step is no worse than the signal's own.
	CHECK(worstStepAtSwitch <= 1.5 * worstStep);
	// AND OFF STOPS COSTING: over the last second (Off, and the IR's 2.5 s... measured from here on),
	// feed silence long enough for the tail to pass, then no FFT runs.
	std::vector<float> z(48000, 0.0f), zr(48000, 0.0f);
	for (int i = 0; i < 4; ++i) {
		dev.processStereo(z.data(), zr.data(), 48000);
		std::fill(z.begin(), z.end(), 0.0f);
		std::fill(zr.begin(), zr.end(), 0.0f);
	}
	const uint64_t settled = dev.convolverForTest().forwardFftsForTest();
	dev.processStereo(z.data(), zr.data(), 48000);
	CHECK(dev.convolverForTest().forwardFftsForTest() == settled);
}

TEST_CASE("Featherverb's routing GLIDES to a new mode: the gains move over tens of milliseconds, not in a step") {
	// A CLICK IS NOT VISIBLE IN A STEP METRIC ON PLUCKS — their own sample-to-sample steps are bigger than
	// a routing switch's, and a known-bad that JUMPED the gains stayed green against one. So the claim is
	// the value: 48 frames (1 ms) after the switch, the series gain has started and is far from its target;
	// 200 ms after, it is there.
	fv::Settings s;
	s.mix = 1.0f;
	s.convMode = fv::kConvParallel;
	s.convBlend = 0.6f;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	std::vector<float> l(4800, 0.2f), r(4800, 0.2f);
	dev.processStereo(l.data(), r.data(), 4800);
	CHECK(dev.convGainsForTest().series == 0.0f);
	s.convMode = fv::kConvSeries;
	dev.setTarget(s);
	dev.processStereo(l.data(), r.data(), 48);
	const float early = dev.convGainsForTest().series;
	CAPTURE(early);
	CHECK(early > 0.0f);
	CHECK(early < 0.3f * s.convBlend);
	dev.processStereo(l.data(), r.data(), 4800);
	dev.processStereo(l.data(), r.data(), 4800);
	// 200 ms is ten 20 ms time constants: e^-10 of the step, 4.5e-5, remains.
	CHECK(std::fabs(dev.convGainsForTest().series - s.convBlend) < 1.0e-4f);
	CHECK(dev.convGainsForTest().input < 1.0e-4f);
}

//--------------------------------------------------------------------------------------------------//
// AURORA: another algorithm in the Sky-Owl field — original, good-sounding and light to compute    //
//--------------------------------------------------------------------------------------------------//

#include "fx/featherverb/featherverb_aurora.h"

namespace {

/// Aurora alone over `x` (int32 at the send's scale), left channel out, in 256-frame calls.
std::vector<double> runAurora(const fv::Aurora::Params& p, const std::vector<int32_t>& x) {
	fv::Aurora a;
	a.prepare(48000.0);
	a.set(p);
	std::vector<int32_t> l(x.size()), r(x.size());
	for (size_t at = 0; at < x.size(); at += 256) {
		const size_t n = std::min<size_t>(256, x.size() - at);
		a.run(x.data() + at, l.data() + at, r.data() + at, n);
	}
	std::vector<double> out(x.size());
	for (size_t i = 0; i < x.size(); ++i) {
		out[i] = static_cast<double>(l[i]) / kFirmwareFullScale;
	}
	return out;
}

/// The energy-weighted mean frequency in [lo, hi] Hz of `len` samples from `from`, by a direct DFT (Hann window).
double bandCentroid(const std::vector<double>& y, size_t from, size_t len, double lo, double hi) {
	double num = 0.0, den = 0.0;
	for (double f = lo; f <= hi; f += 2.0) {
		double re = 0.0, im = 0.0;
		const double w = 6.283185307179586 * f / 48000.0;
		for (size_t i = 0; i < len; ++i) {
			const double hann = 0.5 - 0.5 * std::cos(6.283185307179586 * static_cast<double>(i) / static_cast<double>(len));
			re += hann * y[from + i] * std::cos(w * static_cast<double>(i));
			im -= hann * y[from + i] * std::sin(w * static_cast<double>(i));
		}
		const double p = re * re + im * im;
		num += f * p;
		den += p;
	}
	return den > 0.0 ? num / den : 0.0;
}

/// The RT60 of `y`'s energy over [from, to] seconds: a line through its 50 ms energies in dB.
double rt60Of(const std::vector<double>& y, double from, double to) {
	const auto win = static_cast<size_t>(0.05 * 48000.0);
	std::vector<double> ts, ds;
	for (auto at = static_cast<size_t>(from * 48000.0); at + win <= static_cast<size_t>(to * 48000.0); at += win) {
		double e = 0.0;
		for (size_t i = at; i < at + win; ++i) {
			e += y[i] * y[i];
		}
		ts.push_back(static_cast<double>(at) / 48000.0);
		ds.push_back(10.0 * std::log10(e + 1e-300));
	}
	double mt = 0.0, md = 0.0;
	for (size_t i = 0; i < ts.size(); ++i) {
		mt += ts[i];
		md += ds[i];
	}
	mt /= static_cast<double>(ts.size());
	md /= static_cast<double>(ts.size());
	double num = 0.0, den = 0.0;
	for (size_t i = 0; i < ts.size(); ++i) {
		num += (ts[i] - mt) * (ds[i] - md);
		den += (ts[i] - mt) * (ts[i] - mt);
	}
	return -60.0 / (num / den);
}

std::vector<int32_t> noiseBurst(double seconds, double total, uint32_t seed) {
	std::vector<int32_t> x(static_cast<size_t>(total * 48000.0), 0);
	uint32_t s = seed;
	for (size_t i = 0; i < static_cast<size_t>(seconds * 48000.0); ++i) {
		s = s * 1664525u + 1013904223u;
		x[i] = static_cast<int32_t>(static_cast<double>(static_cast<int32_t>(s)) / 2147483648.0 * 0.1 * kFirmwareFullScale);
	}
	return x;
}

} // namespace

TEST_CASE("Aurora's tail lasts as Decay says: about 1.2 s at 0 and 12 s at 1") {
	fv::Aurora::Params p;
	p.damping = 0.0f; // no damping, so the broadband slope is the loop's own
	p.shift = 0.0f;
	p.decay = 0.0f;
	const std::vector<int32_t> burst = noiseBurst(0.05, 6.0, 0x51u);
	const double shortTail = rt60Of(runAurora(p, burst), 0.15, 0.9);
	p.decay = 1.0f;
	const double longTail = rt60Of(runAurora(p, burst), 0.5, 5.5);
	MESSAGE("Aurora RT60: Decay 0 " << shortTail << " s (1.2), Decay 1 " << longTail << " s (12)");
	CHECK(std::fabs(shortTail - 1.2) < 0.2 * 1.2);
	CHECK(std::fabs(longTail - 12.0) < 0.2 * 12.0);
}

TEST_CASE("Aurora's shift is real: at Matrix 1 a tail climbs; at Matrix 0 it barely moves") {
	std::vector<int32_t> tone(static_cast<size_t>(4.0 * 48000.0), 0);
	for (size_t i = 0; i < static_cast<size_t>(0.3 * 48000.0); ++i) {
		tone[i] = static_cast<int32_t>(0.1 * std::sin(6.283185307179586 * 1000.0 * static_cast<double>(i) / 48000.0)
		                               * kFirmwareFullScale);
	}
	const auto rise = [&](float shift) {
		fv::Aurora::Params p;
		p.decay = 1.0f;
		p.damping = 0.0f;
		p.shift = shift;
		const std::vector<double> y = runAurora(p, tone);
		const size_t len = 16384;
		const double early = bandCentroid(y, static_cast<size_t>(0.5 * 48000.0), len, 850.0, 2400.0);
		const double late = bandCentroid(y, static_cast<size_t>(2.5 * 48000.0), len, 850.0, 2400.0);
		MESSAGE("shift " << shift << ": centroid " << early << " Hz at 0.5 s, " << late << " Hz at 2.5 s");
		return late - early;
	};
	const double moving = rise(1.0f);
	const double still = rise(0.0f);
	CHECK(moving > 20.0);
	CHECK(moving > 10.0 * std::fabs(still));
}

TEST_CASE("Aurora's shift LFO: Character sets how far and how fast the shift swings; at Character 0 it holds still") {
	// THE LFO'S OWN NUMBERS, read off the engine each block (`lastShiftHzForTest`), not inferred from a tail.
	const auto sweep = [](float depthTri, float rateTri, double seconds) {
		fv::Aurora a;
		a.prepare(48000.0);
		fv::Aurora::Params p;
		p.shift = 0.0f; // the quarter hertz, so the swing is all the LFO's
		p.character[1] = depthTri;
		p.character[4] = rateTri;
		a.set(p);
		std::vector<int32_t> x(256, 0), l(256), r(256);
		std::vector<float> hz;
		for (double t = 0.0; t < seconds; t += 256.0 / 48000.0) {
			a.run(x.data(), l.data(), r.data(), 256);
			hz.push_back(a.lastShiftHzForTest());
		}
		struct Swing {
			float lo, hi, rate;
			int crossings;
		};
		Swing s { *std::min_element(hz.begin(), hz.end()), *std::max_element(hz.begin(), hz.end()),
			      a.wobbleRateHzForTest(), 0 };
		for (size_t i = 1; i < hz.size(); ++i) {
			s.crossings += (hz[i - 1] < 0.25f) != (hz[i] < 0.25f) ? 1 : 0; // across the centre, twice a lap
		}
		return s;
	};
	const auto still = sweep(0.0f, 0.0f, 10.0);
	CHECK(still.lo == doctest::Approx(0.25f).epsilon(1e-6));
	CHECK(still.hi == doctest::Approx(0.25f).epsilon(1e-6));

	// DEPTH: 3 Hz either side of the quarter hertz at the top — through zero, so the climb turns into a fall.
	const auto quick = sweep(-1.0f, 1.0f, 20.0);
	MESSAGE("quick: " << quick.lo << " .. " << quick.hi << " Hz at " << quick.rate << " Hz, " << quick.crossings
	                  << " crossings in 20 s");
	CHECK(quick.lo < -2.6f);
	CHECK(quick.hi > 3.1f);
	CHECK(quick.lo > -2.76f);
	CHECK(quick.hi < 3.26f);
	// RATE: 0.3 Hz is six laps in 20 s, twelve crossings; the slow end, 0.02 Hz, not even one lap.
	CHECK(quick.rate == doctest::Approx(0.30f).epsilon(1e-5));
	CHECK(quick.crossings >= 11);
	CHECK(quick.crossings <= 13);
	const auto slow = sweep(1.0f, 0.0f, 20.0);
	CHECK(slow.rate == doctest::Approx(0.02f).epsilon(1e-5));
	CHECK(slow.crossings <= 1);
	const auto half = sweep(0.5f, 0.5f, 20.0);
	CHECK(half.hi == doctest::Approx(0.25f + 1.5f).epsilon(0.02));
	CHECK(half.rate == doctest::Approx(0.16f).epsilon(1e-5));

	// AND THE DEVICE'S CHARACTER REACHES BOTH: somewhere in the first lap each triangle is well off zero.
	bool depthSeen = false;
	bool rateSeen = false;
	for (float c = 0.0f; c < 4.0f; c += 0.05f) {
		const auto t = fv::characterBankOf(c);
		depthSeen = depthSeen || std::fabs(t[1]) > 0.8f;
		rateSeen = rateSeen || std::fabs(t[4]) > 0.8f;
	}
	CHECK(depthSeen);
	CHECK(rateSeen);
}

TEST_CASE("Aurora through the device: seated between Feather and Sky, and the Exciter drives it too") {
	const auto rmsOf = [](int mode, int conv, float blend, bool impulse) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.convMode = conv;
		s.convBlend = blend;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		std::vector<float> l(48000 * 4, 0.0f), r;
		for (size_t i = 0; i < l.size(); ++i) {
			const double u = std::fmod(static_cast<double>(i) / 48000.0, 0.5);
			l[i] = impulse ? (i == 0 ? 0.5f : 0.0f) : static_cast<float>(0.5 * std::exp(-u * 8.0) * std::sin(6.283185307 * 330.0 * u));
		}
		r = l;
		for (size_t at = 0; at < l.size(); at += 512) {
			dev.processStereo(l.data() + at, r.data() + at, 512);
		}
		double e = 0.0;
		for (size_t i = 48000; i < l.size(); ++i) {
			e += static_cast<double>(l[i]) * l[i];
		}
		return std::sqrt(e / static_cast<double>(l.size() - 48000));
	};
	const double feather = rmsOf(4, fv::kConvOff, 0.5f, false);
	const double sky = rmsOf(5, fv::kConvOff, 0.5f, false);
	const double aurora = rmsOf(fv::kAuroraMenu, fv::kConvOff, 0.5f, false);
	MESSAGE("rms: Feather " << feather << ", Aurora " << aurora << ", Sky " << sky);
	CHECK(aurora > feather);
	CHECK(aurora < sky);
	// EXCITER AT BLEND 1: no dry send, so Aurora rings from the injected room alone.
	CHECK(rmsOf(fv::kAuroraMenu, fv::kConvExciter, 1.0f, true) > 1.0e-6);
}

TEST_CASE("MEASURE: Aurora's cost against Feather's, 10 s at 48 kHz" * doctest::skip()) {
	const auto secondsFor = [](int mode) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		std::vector<float> l(48000 * 10), r;
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = static_cast<float>(0.3 * std::sin(0.03 * static_cast<double>(i)));
		}
		r = l;
		const auto t0 = std::chrono::steady_clock::now();
		for (size_t at = 0; at < l.size(); at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, 256);
		}
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	};
	for (int round = 0; round < 2; ++round) {
		const double feather = secondsFor(4);
		const double aurora = secondsFor(fv::kAuroraMenu);
		MESSAGE("10 s: Feather " << feather * 10.0 << "% of a core, Aurora " << aurora * 10.0 << "%");
	}
	CHECK(true);
}

//--------------------------------------------------------------------------------------------------//
// OVERSAMPLE (backlog JE: oversampling, with an enable button)                                   //
//--------------------------------------------------------------------------------------------------//

#include "fx/featherverb/featherverb_oversample.h"

namespace {

/// The averaged power spectrum of `x` (Hann windows of `N`, half-overlapped), bin k at k·sr/N.
std::vector<double> powerSpectrum(const std::vector<double>& x, size_t N) {
	std::vector<double> power(N / 2, 0.0);
	for (size_t at = 0; at + N <= x.size(); at += N / 2) {
		std::vector<std::complex<double>> a(N);
		for (size_t i = 0; i < N; ++i) {
			a[i] = x[at + i] * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(i) / (N - 1)));
		}
		for (size_t i = 1, j = 0; i < N; ++i) {
			size_t bit = N >> 1;
			for (; j & bit; bit >>= 1) {
				j ^= bit;
			}
			j ^= bit;
			if (i < j) {
				std::swap(a[i], a[j]);
			}
		}
		for (size_t len = 2; len <= N; len <<= 1) {
			const std::complex<double> wl(std::cos(-2.0 * 3.14159265358979 / static_cast<double>(len)),
			                              std::sin(-2.0 * 3.14159265358979 / static_cast<double>(len)));
			for (size_t i = 0; i < N; i += len) {
				std::complex<double> w(1.0);
				for (size_t k = 0; k < len / 2; ++k) {
					const auto u = a[i + k];
					const auto v = a[i + k + len / 2] * w;
					a[i + k] = u + v;
					a[i + k + len / 2] = u - v;
					w *= wl;
				}
			}
		}
		for (size_t k = 0; k < N / 2; ++k) {
			power[k] += std::norm(a[k]);
		}
	}
	return power;
}

double bandPower(const std::vector<double>& power, double sr, size_t N, double lo, double hi) {
	double e = 0.0;
	for (size_t k = 0; k < power.size(); ++k) {
		const double f = static_cast<double>(k) * sr / static_cast<double>(N);
		if (f >= lo && f < hi) {
			e += power[k];
		}
	}
	return e;
}

/// The device's wet (Mix 1) for sine plucks of `hz`, 3 s at 48 kHz, left channel.
std::vector<double> osRender(int mode, bool oversample, double hz, double seconds = 3.0) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = mode;
	s.oversample = oversample;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	const auto total = static_cast<size_t>(seconds * 48000.0);
	std::vector<float> l(total), r(total);
	for (size_t i = 0; i < total; ++i) {
		const double t = static_cast<double>(i) / 48000.0;
		const double env = std::fmod(t, 0.5) < 0.1 ? 0.5 : 0.0;
		l[i] = r[i] = static_cast<float>(env * std::sin(6.283185307179586 * hz * t));
	}
	for (size_t at = 0; at < total; at += 512) {
		const auto n = static_cast<int>(std::min<size_t>(512, total - at));
		dev.processStereo(l.data() + at, r.data() + at, n);
	}
	return { l.begin(), l.end() };
}

} // namespace

TEST_CASE("Oversample's resamplers: up then down is the input, 23 host samples late, at unity") {
	fv::HalfbandUp up;
	fv::HalfbandDown down;
	std::vector<int32_t> x(4096), mid(8192), y(4096);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<int32_t>(1.0e8 * std::sin(6.283185307179586 * 5000.0 * static_cast<double>(i) / 48000.0) + 2.0e7);
	}
	for (size_t at = 0; at < x.size(); at += 256) {
		up.run(x.data() + at, mid.data() + 2 * at, 256);
		down.run(mid.data() + 2 * at, y.data() + at, 256);
	}
	CHECK(fv::kOversampleDelayFrames == 23);
	double worst = 0.0;
	for (size_t i = 200; i < x.size(); ++i) {
		worst = std::max(worst, std::fabs(static_cast<double>(y[i]) - static_cast<double>(x[i - 23])));
	}
	MESSAGE("worst error " << worst / 1.2e8 << " of the peak");
	CHECK(worst < 1.0e-3 * 1.2e8);
}

TEST_CASE("Oversample takes the core's images out of the band: sine plucks at 48 kHz, every firmware kind") {
	for (const int mode : { 0, 4, 5, fv::kVastMenu, fv::kOwlMenu }) {
		for (const double hz : { 3000.0, 7000.0 }) {
			double tone[2], image[2], high[2];
			for (int os = 0; os < 2; ++os) {
				const std::vector<double> y = osRender(mode, os == 1, hz);
				const std::vector<double> tail(y.begin() + 4800, y.end());
				const auto p = powerSpectrum(tail, 8192);
				tone[os] = bandPower(p, 48000.0, 8192, hz - 300.0, hz + 300.0);
				image[os] = bandPower(p, 48000.0, 8192, 24000.0 - hz - 300.0, 24000.0 - hz + 300.0);
				high[os] = bandPower(p, 48000.0, 8192, 12000.0, 24000.0);
			}
			const auto db = [](double a, double b) { return 10.0 * std::log10((a + 1e-30) / (b + 1e-30)); };
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << " " << hz << " Hz: image " << db(image[0], tone[0]) << " dB -> "
			                                 << db(image[1], tone[1]) << " dB; 12-24k " << db(high[0], tone[0])
			                                 << " -> " << db(high[1], tone[1]) << "; tone level "
			                                 << db(tone[1], tone[0]) << " dB");
			// THE VENUE CAN SAY IT: Off, the image of a pluck sits within 15 dB of the pluck (MEASURED -9 at 3 kHz, level
			// with it at 7). Oversampled it falls at least 18 dB, and at least 40 in the rooms, Feather and Sky
			// (MEASURED at 3 kHz: 68, 69, 45; Vast and Owl, whose own loops make highs, 25 and 23 — and at 7 kHz 20.4, 19.4).
			CHECK(db(image[0], tone[0]) > -15.0);
			CHECK(db(image[1], tone[1]) < db(image[0], tone[0]) - (hz < 5000.0 && mode <= 5 ? 40.0 : 18.0));
		}
	}
}

TEST_CASE("Oversample at 48 kHz is the core at 96 kHz: the same level in every kind, to a decibel") {
	const auto toneDb = [](int mode, double sr, bool os, double hz) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(sr);
		const auto total = static_cast<size_t>(3.0 * sr);
		std::vector<float> l(total), r(total);
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / sr;
			const double env = std::fmod(t, 0.5) < 0.1 ? 0.5 : 0.0;
			l[i] = r[i] = static_cast<float>(env * std::sin(6.283185307179586 * hz * t));
		}
		for (size_t at = 0; at < total; at += 512) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, total - at)));
		}
		// The tone's own energy (a Goertzel-free narrow DFT over the tail at exactly `hz`).
		double re = 0.0, im = 0.0;
		for (size_t i = static_cast<size_t>(0.1 * sr); i < total; ++i) {
			const double ph = 6.283185307179586 * hz * static_cast<double>(i) / sr;
			re += l[i] * std::cos(ph);
			im += l[i] * std::sin(ph);
		}
		return 10.0 * std::log10((re * re + im * im) / static_cast<double>(total * total) + 1e-30);
	};
	for (const int mode : { 0, 4, 5, fv::kVastMenu, fv::kOwlMenu }) {
		const double a = toneDb(mode, 48000.0, false, 3000.0);
		const double b = toneDb(mode, 96000.0, false, 3000.0);
		const double c = toneDb(mode, 48000.0, true, 3000.0);
		MESSAGE("mode " << mode << ": 48k Off " << a << ", 96k Off " << b << " (" << b - a << "), 48k 2x " << c << " ("
		                << c - a << ")");
		// THE CLAIM: what 96 kHz gets (backlog JE: "so a 44.1/48 kHz session gets what 96 kHz gets"). MEASURED within
		// 0.6 dB in all five. The level Off moves between the rates is the core's own (Owl +8.8 dB at 96 kHz).
		CHECK(std::fabs(c - b) < 1.0);
	}
}

TEST_CASE("A turn of Oversample mid-tail: the reverb fades out, the core is rebuilt at the new rate only once silent, and a turn back halfway cancels") {
	fv::Settings s;
	s.mix = 1.0f; // the wet alone, so what the output does is the reverb's
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	uint32_t rng = 99;
	const auto block = [&](int n) {
		std::vector<float> l(static_cast<size_t>(n)), r(static_cast<size_t>(n));
		for (int k = 0; k < n; ++k) {
			rng = rng * 1664525u + 1013904223u;
			l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] =
			    0.3f * static_cast<float>(static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0);
		}
		dev.processStereo(l.data(), r.data(), n);
		float peak = 0.0f;
		for (const float v : l) {
			peak = std::max(peak, std::fabs(v));
		}
		return peak;
	};
	float steady = 0.0f;
	for (int b = 0; b < 750; ++b) { // 1 s, the tail built up
		steady = std::max(steady, block(64));
	}
	REQUIRE(dev.coreOversampleForTest() == 1);
	REQUIRE(steady > 0.01f);

	// ON: 16-frame steps through the change.
	s.oversample = true;
	dev.setTarget(s);
	float lastFadeBefore = 1.0f;
	float peakBefore = 1.0f;
	int flipAt = -1;
	float previousFade = 1.0f;
	bool monotone = true;
	for (int b = 0; b < 200 && flipAt < 0; ++b) {
		const float peak = block(16);
		if (dev.coreOversampleForTest() == 2) {
			flipAt = b;
			break;
		}
		monotone = monotone && dev.oversampleFadeForTest() <= previousFade;
		previousFade = dev.oversampleFadeForTest();
		lastFadeBefore = previousFade;
		peakBefore = peak;
	}
	MESSAGE("rebuilt at block " << flipAt << " (" << flipAt * 16 << " frames); the fade there " << lastFadeBefore
	                            << ", the last peak before " << peakBefore << " against " << steady);
	REQUIRE(flipAt >= 0);
	CHECK(monotone);
	CHECK(lastFadeBefore == 0.0f); // silent BEFORE the core was replaced
	CHECK(peakBefore < 0.01f * steady);
	CHECK(flipAt * 16 >= 480);       // not before the 10 ms fade could finish
	CHECK(flipAt * 16 <= 480 + 400); // and within a firmware buffer of it
	CHECK(dev.oversampleFadeForTest() == 1.0f);
	float after = 0.0f;
	for (int b = 0; b < 750; ++b) {
		after = std::max(after, block(64));
	}
	CHECK(after > 0.3f * steady); // and the reverb is back, at twice the rate

	// OFF, AND BACK ON BEFORE THE FADE ENDS: the fade climbs back, and nothing is rebuilt.
	s.oversample = false;
	dev.setTarget(s);
	for (int b = 0; b < 15; ++b) { // 5 ms of the 10
		(void)block(16);
	}
	const float halfway = dev.oversampleFadeForTest();
	s.oversample = true;
	dev.setTarget(s);
	for (int b = 0; b < 60; ++b) {
		(void)block(16);
	}
	CHECK(halfway > 0.2f);
	CHECK(halfway < 0.8f);
	CHECK(dev.oversampleFadeForTest() == 1.0f);
	CHECK(dev.coreOversampleForTest() == 2);
}

TEST_CASE("MEASURE: Oversample's cost, 10 s at 48 kHz, alternated Off and On (a shared host)" * doctest::skip()) {
	const auto secondsFor = [](int mode, bool os) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		std::vector<float> l(48000 * 10), r;
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = static_cast<float>(0.3 * std::sin(0.03 * static_cast<double>(i)));
		}
		r = l;
		const auto t0 = std::chrono::steady_clock::now();
		for (size_t at = 0; at < l.size(); at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, 256);
		}
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	};
	for (const int mode : { 4, fv::kOwlMenu }) {
		for (int round = 0; round < 3; ++round) {
			const double off = secondsFor(mode, false);
			const double on = secondsFor(mode, true);
			MESSAGE("mode " << mode << ": Off " << off * 10.0 << "% of a core, On " << on * 10.0 << "% (x" << on / off << ")");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl under Oversample — the level over time, Off against 2x against 96 kHz" * doctest::skip()) {
	const auto run = [](double sr, bool os, float room, float decay, std::vector<double>& env, double& ceiling) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = fv::kOwlMenu;
		s.room = room;
		s.decay = decay;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(sr);
		const auto total = static_cast<size_t>(10.0 * sr);
		std::vector<float> l(total), r(total);
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / sr;
			const double e = t < 3.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 : 0.0;
			l[i] = r[i] = static_cast<float>(e * std::sin(6.283185307179586 * 440.0 * t));
		}
		for (size_t at = 0; at < total; at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
		}
		const auto win = static_cast<size_t>(0.5 * sr);
		env.clear();
		ceiling = 0.0;
		for (size_t at = 0; at + win <= total; at += win) {
			double e = 0.0;
			for (size_t i = at; i < at + win; ++i) {
				e += static_cast<double>(l[i]) * l[i];
				ceiling = std::max(ceiling, static_cast<double>(std::fabs(l[i])));
			}
			env.push_back(10.0 * std::log10(e / static_cast<double>(win) + 1e-20));
		}
	};
	for (const float room : { 0.6f, 1.0f }) {
		for (const float decay : { 0.0f, 0.5f, 1.0f }) {
			std::vector<double> a, b, c;
			double pa = 0, pb = 0, pc = 0;
			run(48000.0, false, room, decay, a, pa);
			run(48000.0, true, room, decay, b, pb);
			run(96000.0, false, room, decay, c, pc);
			std::string line;
			for (size_t k = 0; k < a.size(); k += 2) {
				char buf[64];
				std::snprintf(buf, sizeof buf, " %5.1f/%5.1f/%5.1f", a[k], b[k], c[k]);
				line += buf;
			}
			MESSAGE("room " << room << " decay " << decay << " peak " << pa << "/" << pb << "/" << pc << " dB@1s:" << line);
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl's late tail under Oversample, by band" * doctest::skip()) {
	for (const bool os : { false, true }) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = fv::kOwlMenu;
		s.room = 1.0f;
		s.decay = 1.0f;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		const size_t total = 48000 * 12;
		std::vector<float> l(total), r(total);
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / 48000.0;
			const double e = t < 3.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 : 0.0;
			l[i] = r[i] = static_cast<float>(e * std::sin(6.283185307179586 * 440.0 * t));
		}
		for (size_t at = 0; at < total; at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, 256);
		}
		for (const double from : { 4.0, 7.0, 10.0 }) {
			std::vector<double> tail(l.begin() + static_cast<long>(from * 48000), l.begin() + static_cast<long>((from + 2) * 48000));
			const auto p = powerSpectrum(tail, 8192);
			const auto db = [&](double lo, double hi) { return 10.0 * std::log10(bandPower(p, 48000.0, 8192, lo, hi) + 1e-30); };
			MESSAGE((os ? "2x " : "Off") << " " << from << "-" << from + 2 << " s: <500 " << db(20, 500) << ", 0.5-2k "
			                           << db(500, 2000) << ", 2-6k " << db(2000, 6000) << ", 6-12k " << db(6000, 12000)
			                           << ", >12k " << db(12000, 24000));
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl's servo over time at 48 kHz, 2x and 96 kHz" * doctest::skip()) {
	const auto trace = [](double sr, bool os) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = fv::kOwlMenu;
		s.room = 1.0f;
		s.decay = 1.0f;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(sr);
		const auto total = static_cast<size_t>(10.0 * sr);
		std::vector<float> l(256), r(256);
		std::string line;
		for (size_t at = 0; at < total; at += 256) {
			for (size_t k = 0; k < 256; ++k) {
				const double t = static_cast<double>(at + k) / sr;
				const double e = t < 3.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 : 0.0;
				l[k] = r[k] = static_cast<float>(e * std::sin(6.283185307179586 * 440.0 * t));
			}
			dev.processStereo(l.data(), r.data(), 256);
			const size_t step = static_cast<size_t>(sr);
			if ((at / 256) % (step / 256) == 0) {
				const Core& c = dev.coreForTest();
				char buf[96];
				std::snprintf(buf, sizeof buf, " [%.0fs %.3f env %.4f in %.4f]", static_cast<double>(at) / sr,
				              c.owlServoScaleForTest(), c.owlOutputEnvelopeForTest(), c.owlInputPeakForTest());
				line += buf;
			}
		}
		return line;
	};
	MESSAGE("48k Off:" << trace(48000.0, false));
	MESSAGE("48k 2x :" << trace(48000.0, true));
	MESSAGE("96k Off:" << trace(96000.0, false));
	CHECK(true);
}

TEST_CASE("MEASURE: Owl's late energy (4-10 s) over many pitches and settings: 48 kHz, 2x, 96 kHz" * doctest::skip()) {
	const auto late = [](double sr, bool os, float room, float decay, double hz) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = fv::kOwlMenu;
		s.room = room;
		s.decay = decay;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(sr);
		const auto total = static_cast<size_t>(10.0 * sr);
		std::vector<float> l(total), r(total);
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / sr;
			const double e = t < 3.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 : 0.0;
			l[i] = r[i] = static_cast<float>(e * std::sin(6.283185307179586 * hz * t));
		}
		for (size_t at = 0; at < total; at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
		}
		double e = 0.0;
		for (size_t i = static_cast<size_t>(4.0 * sr); i < total; ++i) {
			e += static_cast<double>(l[i]) * l[i];
		}
		return 10.0 * std::log10(e / (6.0 * sr) + 1e-20);
	};
	for (const float room : { 0.3f, 0.6f, 1.0f }) {
		for (const float decay : { 0.0f, 0.5f, 1.0f }) {
			double sum[3] = { 0, 0, 0 };
			int worse = 0;
			const double pitches[] = { 110.0, 220.0, 330.0, 440.0, 660.0, 880.0, 1320.0, 2000.0 };
			for (const double hz : pitches) {
				const double a = late(48000.0, false, room, decay, hz);
				const double b = late(48000.0, true, room, decay, hz);
				const double c = late(96000.0, false, room, decay, hz);
				sum[0] += a;
				sum[1] += b;
				sum[2] += c;
				worse += b > a + 6.0 ? 1 : 0;
			}
			MESSAGE("room " << room << " decay " << decay << ": mean late dB 48k " << sum[0] / 8 << ", 2x " << sum[1] / 8
			                << ", 96k " << sum[2] / 8 << "; 2x >6 dB over 48k in " << worse << "/8");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl on bright broadband input (noise bursts), driven and after, 48 kHz against 2x" * doctest::skip()) {
	const auto run = [](bool os, float room, float decay, uint32_t seed, double& driven, double& after, double& peak) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = fv::kOwlMenu;
		s.room = room;
		s.decay = decay;
		s.oversample = os;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		const size_t total = 48000 * 8;
		std::vector<float> l(total), r(total);
		uint32_t rng = seed;
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / 48000.0;
			rng = rng * 1664525u + 1013904223u;
			const double n = static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0;
			const double e = t < 4.0 && std::fmod(t, 0.25) < 0.03 ? 0.5 * std::exp(-std::fmod(t, 0.25) * 100.0) : 0.0;
			l[i] = r[i] = static_cast<float>(e * n);
		}
		for (size_t at = 0; at < total; at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, 256);
		}
		double e1 = 0, e2 = 0;
		peak = 0;
		for (size_t i = 0; i < total; ++i) {
			(i < 48000 * 4 ? e1 : e2) += static_cast<double>(l[i]) * l[i];
			peak = std::max(peak, static_cast<double>(std::fabs(l[i])));
		}
		driven = 10 * std::log10(e1 / (48000.0 * 4) + 1e-20);
		after = 10 * std::log10(e2 / (48000.0 * 4) + 1e-20);
	};
	for (const float room : { 0.6f, 1.0f }) {
		for (const float decay : { 0.0f, 0.5f, 1.0f }) {
			double d[2] = { 0, 0 }, a[2] = { 0, 0 }, pk[2] = { 0, 0 };
			for (uint32_t seed = 1; seed <= 4; ++seed) {
				for (int os = 0; os < 2; ++os) {
					double dd, aa, pp;
					run(os == 1, room, decay, seed, dd, aa, pp);
					d[os] += dd / 4;
					a[os] += aa / 4;
					pk[os] = std::max(pk[os], pp);
				}
			}
			MESSAGE("room " << room << " decay " << decay << ": driven " << d[0] << " -> " << d[1] << " dB, after " << a[0]
			                << " -> " << a[1] << " dB, peak " << pk[0] << " -> " << pk[1]);
		}
	}
	CHECK(true);
}

namespace {

struct OwlScore {
	double driven = 0, after = 0, peak = 0, pinned = 0, boosting = 0, meanScale = 0, afterEarly = 0, afterLate = 0;
};

/// One Owl render at 48 kHz, Mix 1: `kind` 0 noise bursts (percussive), 1 sine plucks (440 Hz). The input stops at 4 s.
OwlScore owlScore(const Core::OwlProbe& probe, int kind, float room, float decay, uint32_t seed, bool os = false,
                  float character = 0.0f) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = fv::kOwlMenu;
	s.room = room;
	s.decay = decay;
	s.oversample = os;
	s.character = character;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setOwlProbeForTest(probe);
	dev.setOwlCompressorForTest(false); // the FIRMWARE's Owl, which the probes measure against
	const size_t total = 48000 * 10;
	std::vector<float> l(256), r(256);
	uint32_t rng = seed;
	OwlScore sc;
	double e1 = 0, e2 = 0, e3 = 0, e4 = 0;
	size_t pinned = 0, blocks = 0, boosting = 0;
	for (size_t at = 0; at < total; at += 256) {
		for (size_t k = 0; k < 256; ++k) {
			const double t = static_cast<double>(at + k) / 48000.0;
			rng = rng * 1664525u + 1013904223u;
			const double n = static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0;
			double x = 0.0;
			if (t < 4.0) {
				if (kind == 0) {
					const double ph = std::fmod(t, 0.25);
					x = ph < 0.03 ? 0.5 * n * std::exp(-ph * 100.0) : 0.0;
				}
				else {
					x = std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 440.0 * t) : 0.0;
				}
			}
			l[k] = r[k] = static_cast<float>(x);
		}
		dev.processStereo(l.data(), r.data(), 256);
		for (size_t k = 0; k < 256; ++k) {
			const double v = l[k];
			(at < 48000 * 4 ? e1 : e2) += v * v;
			if (at >= 48000 * 4) {
				(at < 48000 * 7 ? e3 : e4) += v * v;
			}
			sc.peak = std::max(sc.peak, std::fabs(v));
			pinned += std::fabs(v) > 24.0 ? 1 : 0;
		}
		const float scale = dev.coreForTest().owlServoScaleForTest();
		sc.meanScale += scale;
		boosting += scale > 1.0f ? 1 : 0;
		++blocks;
	}
	sc.driven = 10 * std::log10(e1 / (48000.0 * 4) + 1e-20);
	sc.after = 10 * std::log10(e2 / (48000.0 * 6) + 1e-20);
	sc.afterEarly = 10 * std::log10(e3 / (48000.0 * 3) + 1e-20);
	sc.afterLate = 10 * std::log10(e4 / (48000.0 * 3) + 1e-20);
	sc.pinned = static_cast<double>(pinned) / static_cast<double>(total);
	sc.boosting = static_cast<double>(boosting) / static_cast<double>(blocks);
	sc.meanScale /= static_cast<double>(blocks);
	return sc;
}

} // namespace

TEST_CASE("MEASURE: Owl probes — what each candidate change does to percussive runaway and to the sustain" * doctest::skip()) {
	struct Variant {
		const char* name;
		Core::OwlProbe probe;
	};
	const Variant variants[] = {
		{ "firmware        ", {} },
		{ "down .03 <=2    ", { 0, 2.0f, 0.0f, 0.03f, 1.0f } },
		{ "  + knee x1.5   ", { 0, 2.0f, 0.0f, 0.03f, 1.5f } },
		{ "  + knee x2     ", { 0, 2.0f, 0.0f, 0.03f, 2.0f } },
		{ "  + knee x3     ", { 0, 2.0f, 0.0f, 0.03f, 3.0f } },
		{ "down .1 <=2 k2  ", { 0, 2.0f, 0.0f, 0.1f, 2.0f } },
		{ "down .03<=1.5 k3", { 0, 1.5f, 0.0f, 0.03f, 3.0f } },
	};
	for (const Variant& v : variants) {
		for (const int kind : { 0, 1 }) {
			OwlScore avg;
			int n = 0;
			for (const float room : { 0.6f, 1.0f }) {
				for (const float decay : { 0.0f, 0.5f, 1.0f }) {
					for (uint32_t seed = 1; seed <= 2; ++seed) {
						const OwlScore s = owlScore(v.probe, kind, room, decay, seed);
						avg.driven += s.driven;
						avg.after += s.after;
						avg.afterEarly += s.afterEarly;
						avg.afterLate += s.afterLate;
						avg.peak = std::max(avg.peak, s.peak);
						avg.pinned += s.pinned;
						avg.boosting += s.boosting;
						avg.meanScale += s.meanScale;
						++n;
					}
				}
			}
			MESSAGE(std::string(v.name) << (kind == 0 ? " NOISE " : " PLUCKS") << ": driven " << avg.driven / n << " dB, after "
			               << avg.after / n << " dB (4-7 s " << avg.afterEarly / n << ", 7-10 s " << avg.afterLate / n << "), worst peak " << avg.peak << ", pinned " << 100 * avg.pinned / n
			               << "%, servo boosting " << 100 * avg.boosting / n << "% (mean " << avg.meanScale / n << ")");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the proposed Owl servo against the firmware — with Oversample, and across Character" * doctest::skip()) {
	const Core::OwlProbe fw {};
	const Core::OwlProbe prop { 0, 2.0f, 0.0f, 0.03f, 2.0f };
	for (const bool os : { false, true }) {
		for (const float ch : { 0.0f, 0.37f, 1.3f, 2.9f, 5.55f }) {
			for (const int kind : { 0, 1 }) {
				OwlScore a, b;
				int n = 0;
				for (const float room : { 0.6f, 1.0f }) {
					for (const float decay : { 0.0f, 0.5f, 1.0f }) {
						const OwlScore x = owlScore(fw, kind, room, decay, 1, os, ch);
						const OwlScore y = owlScore(prop, kind, room, decay, 1, os, ch);
						a.driven += x.driven;
						a.after += x.after;
						a.peak = std::max(a.peak, x.peak);
						a.pinned += x.pinned;
						b.driven += y.driven;
						b.after += y.after;
						b.peak = std::max(b.peak, y.peak);
						b.pinned += y.pinned;
						++n;
					}
				}
				MESSAGE((os ? "2x " : "1x ") << "char " << ch << (kind == 0 ? " NOISE " : " PLUCKS") << ": driven " << a.driven / n
				                             << " -> " << b.driven / n << ", after " << a.after / n << " -> " << b.after / n
				                             << ", peak " << a.peak << " -> " << b.peak << ", pinned " << 100 * a.pinned / n
				                             << "% -> " << 100 * b.pinned / n << "%");
			}
		}
	}
	CHECK(true);
}

namespace {

/// Owl at 48 kHz, Mix 1, 10 s; `kind` 0 noise bursts, 1 sine plucks, 2 a held two-note pad. The whole left output,
/// and the guard's firing count.
std::vector<float> owlRender(const Core::OwlProbe& probe, int kind, float room, float decay, float character, uint64_t& fires) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = fv::kOwlMenu;
	s.room = room;
	s.decay = decay;
	s.character = character;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setOwlProbeForTest(probe);
	dev.setOwlCompressorForTest(false); // the FIRMWARE's Owl, which the probes measure against
	const size_t total = 48000 * 10;
	std::vector<float> out(total), l(256), r(256);
	uint32_t rng = 7;
	for (size_t at = 0; at < total; at += 256) {
		for (size_t k = 0; k < 256; ++k) {
			const double t = static_cast<double>(at + k) / 48000.0;
			rng = rng * 1664525u + 1013904223u;
			const double n = static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0;
			double x = 0.0;
			if (t < 4.0) {
				if (kind == 0) {
					const double ph = std::fmod(t, 0.25);
					x = ph < 0.03 ? 0.5 * n * std::exp(-ph * 100.0) : 0.0;
				}
				else if (kind == 1) {
					x = std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 440.0 * t) : 0.0;
				}
				else {
					x = 0.2 * (std::sin(6.283185307179586 * 220.0 * t) + std::sin(6.283185307179586 * 330.0 * t));
				}
			}
			l[k] = r[k] = static_cast<float>(x);
		}
		dev.processStereo(l.data(), r.data(), 256);
		std::copy(l.begin(), l.end(), out.begin() + static_cast<long>(at));
	}
	fires = dev.coreForTest().owlGuardFiresForTest();
	return out;
}

} // namespace

TEST_CASE("MEASURE: an Owl runaway GUARD — silent (bit-identical) on ordinary material, acting only on a runaway" * doctest::skip()) {
	for (const float at : { 2.0f, 3.0f, 4.0f, 6.0f }) {
		const Core::OwlProbe guard { 0, 10.0f, 0.0f, 0.0f, 1.0f, at, 0.05f };
		int identical[3] = { 0, 0, 0 }, runs[3] = { 0, 0, 0 };
		double drivenFw = 0, drivenG = 0, peakFw = 0, peakG = 0, pinFw = 0, pinG = 0;
		for (const int kind : { 0, 1, 2 }) {
			for (const float room : { 0.6f, 1.0f }) {
				for (const float decay : { 0.0f, 0.5f, 1.0f }) {
					for (const float ch : { 0.0f, 1.3f, 2.9f }) {
						uint64_t f0 = 0, f1 = 0;
						const auto a = owlRender({}, kind, room, decay, ch, f0);
						const auto b = owlRender(guard, kind, room, decay, ch, f1);
						++runs[kind];
						identical[kind] += a == b ? 1 : 0;
						if (kind == 0) {
							double e0 = 0, e1 = 0;
							for (size_t i = 0; i < 48000 * 4; ++i) {
								e0 += static_cast<double>(a[i]) * a[i];
								e1 += static_cast<double>(b[i]) * b[i];
							}
							drivenFw += 10 * std::log10(e0 / (48000.0 * 4) + 1e-20) / 18;
							drivenG += 10 * std::log10(e1 / (48000.0 * 4) + 1e-20) / 18;
							for (size_t i = 0; i < a.size(); ++i) {
								peakFw = std::max(peakFw, static_cast<double>(std::fabs(a[i])));
								peakG = std::max(peakG, static_cast<double>(std::fabs(b[i])));
								pinFw += std::fabs(a[i]) > 24.0f ? 1.0 / (18.0 * a.size()) : 0.0;
								pinG += std::fabs(b[i]) > 24.0f ? 1.0 / (18.0 * a.size()) : 0.0;
							}
						}
					}
				}
			}
		}
		MESSAGE("guard at " << at << "x: identical to the firmware — noise " << identical[0] << "/" << runs[0] << ", plucks "
		                    << identical[1] << "/" << runs[1] << ", pad " << identical[2] << "/" << runs[2]
		                    << "; noise driven " << drivenFw << " -> " << drivenG << " dB, peak " << peakFw << " -> " << peakG
		                    << ", pinned " << 100 * pinFw << "% -> " << 100 * pinG << "%");
	}
	CHECK(true);
}

TEST_CASE("MEASURE: what the Owl guard does to TONE on ordinary material — level, brightness, pumping" * doctest::skip()) {
	const auto describe = [](const std::vector<float>& y, double& level, double& centroid, double& pump) {
		std::vector<double> x(y.begin() + 48000, y.end());
		double e = 0;
		for (const double v : x) {
			e += v * v;
		}
		level = 10 * std::log10(e / static_cast<double>(x.size()) + 1e-20);
		const auto p = powerSpectrum(x, 8192);
		double tot = 0, mom = 0;
		for (size_t k = 1; k < p.size(); ++k) {
			tot += p[k];
			mom += p[k] * static_cast<double>(k);
		}
		centroid = mom / (tot + 1e-30) * 48000.0 / 8192.0;
		// PUMPING: the spread (sd, dB) of 50 ms levels across the tail.
		std::vector<double> w;
		for (size_t at = 0; at + 2400 <= x.size(); at += 2400) {
			double s = 0;
			for (size_t i = at; i < at + 2400; ++i) {
				s += x[i] * x[i];
			}
			w.push_back(10 * std::log10(s / 2400.0 + 1e-20));
		}
		double m = 0;
		for (const double v : w) {
			m += v;
		}
		m /= static_cast<double>(w.size());
		double sd = 0;
		for (const double v : w) {
			sd += (v - m) * (v - m);
		}
		pump = std::sqrt(sd / static_cast<double>(w.size()));
	};
	for (const float at : { 3.0f, 4.0f }) {
		const Core::OwlProbe guard { 0, 10.0f, 0.0f, 0.0f, 1.0f, at, 0.05f };
		for (const int kind : { 1, 2 }) {
			double dl = 0, dc = 0, dp = 0, worstL = 0, worstC = 0, pf = 0;
			int n = 0;
			for (const float room : { 0.6f, 1.0f }) {
				for (const float decay : { 0.0f, 0.5f, 1.0f }) {
					for (const float ch : { 0.0f, 1.3f, 2.9f }) {
						uint64_t f0 = 0, f1 = 0;
						double l0, c0, p0, l1, c1, p1;
						describe(owlRender({}, kind, room, decay, ch, f0), l0, c0, p0);
						describe(owlRender(guard, kind, room, decay, ch, f1), l1, c1, p1);
						dl += (l1 - l0);
						dc += 1200.0 * std::log2(c1 / c0);
						dp += p1 - p0;
						pf += p0;
						worstL = std::max(worstL, std::fabs(l1 - l0));
						worstC = std::max(worstC, std::fabs(1200.0 * std::log2(c1 / c0)));
						++n;
					}
				}
			}
			MESSAGE("guard " << at << "x " << (kind == 1 ? "PLUCKS" : "PAD   ") << ": level " << dl / n << " dB (worst "
			                 << worstL << "), brightness " << dc / n << " cents (worst " << worstC << "), pumping sd "
			                 << pf / n << " -> " << (pf + dp) / n << " dB");
		}
	}
	CHECK(true);
}

namespace {

struct Tone {
	double level = 0, centroid = 0, pump = 0;
};

Tone toneOf(const std::vector<float>& y) {
	Tone t;
	std::vector<double> x(y.begin() + 48000, y.end());
	double e = 0;
	for (const double v : x) {
		e += v * v;
	}
	t.level = 10 * std::log10(e / static_cast<double>(x.size()) + 1e-20);
	const auto p = powerSpectrum(x, 8192);
	double tot = 0, mom = 0;
	for (size_t k = 1; k < p.size(); ++k) {
		tot += p[k];
		mom += p[k] * static_cast<double>(k);
	}
	t.centroid = mom / (tot + 1e-30) * 48000.0 / 8192.0;
	std::vector<double> w;
	for (size_t at = 0; at + 2400 <= x.size(); at += 2400) {
		double s = 0;
		for (size_t i = at; i < at + 2400; ++i) {
			s += x[i] * x[i];
		}
		w.push_back(10 * std::log10(s / 2400.0 + 1e-20));
	}
	double m = 0, sd = 0;
	for (const double v : w) {
		m += v;
	}
	m /= static_cast<double>(w.size());
	for (const double v : w) {
		sd += (v - m) * (v - m);
	}
	t.pump = std::sqrt(sd / static_cast<double>(w.size()));
	return t;
}

/// A variant against the firmware: the runaway on noise, and the tone on plucks and on a held pad.
std::string owlVerdict(const Core::OwlProbe& probe) {
	double dDriven = 0, pinFw = 0, pinV = 0, peakV = 0;
	double tl[3] = {}, tc[3] = {}, tp[3] = {}, pf[3] = {}, worstL[3] = {};
	int n = 0;
	for (const float room : { 0.6f, 1.0f }) {
		for (const float decay : { 0.0f, 0.5f, 1.0f }) {
			for (const float ch : { 0.0f, 1.3f, 2.9f }) {
				for (const int kind : { 0, 1, 2 }) {
					uint64_t f0 = 0, f1 = 0;
					const auto a = owlRender({}, kind, room, decay, ch, f0);
					const auto b = owlRender(probe, kind, room, decay, ch, f1);
					if (kind == 0) {
						double e0 = 0, e1 = 0;
						for (size_t i = 0; i < 48000 * 4; ++i) {
							e0 += static_cast<double>(a[i]) * a[i];
							e1 += static_cast<double>(b[i]) * b[i];
						}
						dDriven += 10 * std::log10((e1 + 1e-20) / (e0 + 1e-20));
						for (size_t i = 0; i < a.size(); ++i) {
							pinFw += std::fabs(a[i]) > 24.0f ? 1.0 : 0.0;
							pinV += std::fabs(b[i]) > 24.0f ? 1.0 : 0.0;
							peakV = std::max(peakV, static_cast<double>(std::fabs(b[i])));
						}
					}
					else {
						const Tone x = toneOf(a);
						const Tone y = toneOf(b);
						tl[kind] += y.level - x.level;
						tc[kind] += 1200.0 * std::log2(y.centroid / x.centroid);
						tp[kind] += y.pump - x.pump;
						pf[kind] += x.pump;
						worstL[kind] = std::max(worstL[kind], std::fabs(y.level - x.level));
					}
				}
				++n;
			}
		}
	}
	const double samples = 18.0 * 48000.0 * 10.0;
	char buf[400];
	std::snprintf(buf, sizeof buf,
	              "NOISE driven %+.1f dB, pinned %.2f%% -> %.2f%%, peak %.1f | PLUCKS %+.1f dB (worst %.1f), %+.0f c, pump %+.1f | "
	              "PAD %+.1f dB (worst %.1f), %+.0f c, pump %.1f -> %.1f",
	              dDriven / n, 100 * pinFw / samples, 100 * pinV / samples, peakV, tl[1] / n, worstL[1], tc[1] / n, tp[1] / n,
	              tl[2] / n, worstL[2], tc[2] / n, pf[2] / n, (pf[2] + tp[2]) / n);
	return buf;
}

} // namespace

TEST_CASE("MEASURE: a compressor on Owl's servo — A: its applied gain above a knee; B: the loop's return" * doctest::skip()) {
	struct Variant {
		const char* name;
		Core::OwlProbe probe;
	};
	auto a = [](float knee, float ratio) {
		Core::OwlProbe p;
		p.gainKnee = knee;
		p.gainRatio = ratio;
		return p;
	};
	auto b = [](float at, float ratio) {
		Core::OwlProbe p;
		p.compAt = at;
		p.compRatio = ratio;
		return p;
	};
	const Variant variants[] = {
		{ "A knee 1.1 r4 ", a(1.1f, 4.0f) }, { "A knee 1.3 r4 ", a(1.3f, 4.0f) }, { "A knee 1.5 r4 ", a(1.5f, 4.0f) },
		{ "A knee 1.3 r10", a(1.3f, 10.0f) }, { "B .002 r4     ", b(0.002f, 4.0f) }, { "B .005 r4     ", b(0.005f, 4.0f) },
		{ "B .01 r4      ", b(0.01f, 4.0f) }, { "B .02 r4      ", b(0.02f, 4.0f) },
	};
	for (const Variant& v : variants) {
		MESSAGE(std::string(v.name) << ": " << owlVerdict(v.probe));
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl's chaos floor — how far a change that does NOTHING audible moves the tone measures" * doctest::skip()) {
	Core::OwlProbe nothing;
	nothing.loopSat = 0.5f; // L·tanh(x/L) at L = 0.5: a loop sample of 0.01 moves by ~1e-6 of itself — inaudible, not identical
	MESSAGE("chaos floor   : " << owlVerdict(nothing));
	CHECK(true);
}

namespace {

/// The device at 48 kHz, Mix 1, 10 s of `kind` (0 noise bursts, 1 sine plucks, 2 a held pad), through the Settings road.
std::vector<float> verbRender(int mode, bool owlComp, bool os, int kind, float room, float decay, float character) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = mode;
	s.room = room;
	s.decay = decay;
	s.character = character;
	s.oversample = os;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setOwlCompressorForTest(owlComp);
	dev.setWetLimiterForTest(false); // what the LOOP does: the wet ceiling would hide every pin
	dev.setOwlTrimForTest(false);     // and the trim, which takes Owl's level down after the loop
	const size_t total = 48000 * 10;
	std::vector<float> out(total), l(256), r(256);
	uint32_t rng = 7;
	for (size_t at = 0; at < total; at += 256) {
		for (size_t k = 0; k < 256; ++k) {
			const double t = static_cast<double>(at + k) / 48000.0;
			rng = rng * 1664525u + 1013904223u;
			const double n = static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0;
			double x = 0.0;
			if (t < 4.0) {
				if (kind == 0) {
					const double ph = std::fmod(t, 0.25);
					x = ph < 0.03 ? 0.5 * n * std::exp(-ph * 100.0) : 0.0;
				}
				else if (kind == 1) {
					x = std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 440.0 * t) : 0.0;
				}
				else {
					x = 0.2 * (std::sin(6.283185307179586 * 220.0 * t) + std::sin(6.283185307179586 * 330.0 * t));
				}
			}
			l[k] = r[k] = static_cast<float>(x);
		}
		dev.processStereo(l.data(), r.data(), 256);
		std::copy(l.begin(), l.end(), out.begin() + static_cast<long>(at));
	}
	return out;
}

double pinnedShare(const std::vector<float>& y) {
	size_t n = 0;
	for (const float v : y) {
		n += std::fabs(v) > 24.0f ? 1 : 0;
	}
	return static_cast<double>(n) / static_cast<double>(y.size());
}

} // namespace

TEST_CASE("Owl Comp: percussive input no longer pins the ceiling, plucks keep their level, and no other mode hears it") {
	double pinOff = 0, pinOn = 0, pinOn2x = 0, pluckDiff = 0, padDiff = 0;
	int n = 0;
	for (const float room : { 0.6f, 1.0f }) {
		for (const float decay : { 0.5f, 1.0f }) {
			for (const float ch : { 0.0f, 2.9f }) {
				pinOff += pinnedShare(verbRender(fv::kOwlMenu, false, false, 0, room, decay, ch));
				pinOn += pinnedShare(verbRender(fv::kOwlMenu, true, false, 0, room, decay, ch));
				pinOn2x += pinnedShare(verbRender(fv::kOwlMenu, true, true, 0, room, decay, ch));
				pluckDiff += toneOf(verbRender(fv::kOwlMenu, true, false, 1, room, decay, ch)).level
				             - toneOf(verbRender(fv::kOwlMenu, false, false, 1, room, decay, ch)).level;
				padDiff += toneOf(verbRender(fv::kOwlMenu, true, false, 2, room, decay, ch)).level
				           - toneOf(verbRender(fv::kOwlMenu, false, false, 2, room, decay, ch)).level;
				++n;
			}
		}
	}
	MESSAGE("pinned: Off " << 100 * pinOff / n << "%, On " << 100 * pinOn / n << "%, On with 2x " << 100 * pinOn2x / n
	                       << "%; plucks " << pluckDiff / n << " dB, pad " << padDiff / n << " dB");
	// THE VENUE CAN SAY IT: Off, the firmware pins (MEASURED 1.58% over the wider sweep).
	CHECK(pinOff / n > 0.005);
	CHECK(pinOn / n < 0.0001);
	CHECK(pinOn2x / n < 0.0001);
	CHECK(std::fabs(pluckDiff / n) < 2.5); // MEASURED -1.6 dB at these decays (loop start, servo^2; was -0.5 on the return)
	CHECK(padDiff / n > -8.0);              // MEASURED -6.5 dB at these decays (was -3.8 on the return)
	// HONEST: in every other mode the switch changes not one sample.
	for (const int mode : { 0, 4, 5, fv::kAuroraMenu, fv::kVastMenu }) {
		CHECK(verbRender(mode, true, false, 0, 1.0f, 1.0f, 0.0f) == verbRender(mode, false, false, 0, 1.0f, 1.0f, 0.0f));
	}
}

TEST_CASE("MEASURE: when Owl Comp first changes a sample on steady noise (the honesty sweep's window is 4096 frames)" * doctest::skip()) {
	for (const float decay : { 0.0f, 0.5f, 1.0f }) {
	for (const float amp : { 0.35f, 1.0f }) {
		std::vector<float> a, b;
		for (const bool on : { false, true }) {
			fv::Settings s;
			s.sizeMode = fv::kOwlMenu;
			s.decay = decay;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setOwlCompressorForTest(on);
			std::vector<float> l(48000 * 4), r;
			uint32_t rng = 0xc0ffeeu;
			for (float& v : l) {
				rng = rng * 1664525u + 1013904223u;
				v = amp * static_cast<float>(static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0);
			}
			r = l;
			for (size_t at = 0; at < l.size(); at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, 256);
			}
			(on ? b : a) = l;
		}
		size_t first = a.size();
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i] != b[i]) {
				first = i;
				break;
			}
		}
		MESSAGE("decay " << decay << " noise " << amp << ": first differing sample " << first << " (" << static_cast<double>(first) / 48.0 << " ms)");
	}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl Comp's threshold and ratio — the runaway (loudest second) against the tone" * doctest::skip()) {
	const auto loudestSecond = [](const std::vector<float>& y, size_t from) {
		double worst = -200.0;
		for (size_t at = from; at + 48000 <= y.size(); at += 12000) {
			double e = 0;
			for (size_t i = at; i < at + 48000; ++i) {
				e += static_cast<double>(y[i]) * y[i];
			}
			worst = std::max(worst, 10 * std::log10(e / 48000.0 + 1e-20));
		}
		return worst;
	};
	struct Setting {
		float at, ratio;
	};
	const Setting settings[] = { { 0.0f, 0.0f }, { 0.005f, 4.0f }, { 0.005f, 20.0f }, { 0.003f, 8.0f },
		                         { 0.002f, 8.0f }, { 0.002f, 20.0f }, { 0.001f, 20.0f }, { 0.001f, 50.0f } };
	for (const Setting& st : settings) {
		Core::OwlProbe p;
		p.compAt = st.at;
		p.compRatio = st.ratio;
		double worstAll = -200, worstAfter = -200, meanAfter = 0, pluck = 0, pad = 0, padBright = 0, peak = 0;
		int n = 0;
		for (const float room : { 0.6f, 1.0f }) {
			for (const float decay : { 0.5f, 1.0f }) {
				for (const float ch : { 0.0f, 1.3f, 2.9f, 5.55f }) {
					uint64_t f = 0;
					const auto y = owlRender(p, 0, room, decay, ch, f);
					worstAll = std::max(worstAll, loudestSecond(y, 0));
					const double after = loudestSecond(y, 48000 * 4);
					worstAfter = std::max(worstAfter, after);
					meanAfter += after;
					for (const float v : y) {
						peak = std::max(peak, static_cast<double>(std::fabs(v)));
					}
					const Tone a = toneOf(owlRender({}, 1, room, decay, ch, f));
					const Tone b = toneOf(owlRender(p, 1, room, decay, ch, f));
					pluck += b.level - a.level;
					const Tone c = toneOf(owlRender({}, 2, room, decay, ch, f));
					const Tone d = toneOf(owlRender(p, 2, room, decay, ch, f));
					pad += d.level - c.level;
					padBright += 1200.0 * std::log2(d.centroid / c.centroid);
					++n;
				}
			}
		}
		MESSAGE((st.at == 0.0f ? std::string("firmware        ") : "at " + std::to_string(st.at).substr(0, 5) + " r"
		                                                             + std::to_string(static_cast<int>(st.ratio)) + "   ")
		        << ": NOISE loudest second " << worstAll << " dB, after the input " << worstAfter << " (mean "
		        << meanAfter / n << "), peak " << peak << " | plucks " << pluck / n << " dB | pad " << pad / n << " dB, "
		        << padBright / n << " c");
	}
	CHECK(true);
}

namespace {

struct Reentry {
	double firstPeak = 0, againPeak = 0, scaleBefore = 0, overMs = 0, sustain = 0;
};

/// THE REPORTED CASE (2026-09-25: with the servo sitting at high gain, new notes clip; 800 Hz sine plucks):
/// plucks for 2 s, 6 s of tail for the servo to climb, then plucks again. Mix 1, the wet alone.
Reentry owlReentry(const Core::OwlProbe& probe, bool comp, float room, float decay, float character, bool os = false) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = fv::kOwlMenu;
	s.room = room;
	s.decay = decay;
	s.character = character;
	s.oversample = os;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setOwlProbeForTest(probe);
	dev.setOwlCompressorForTest(comp);
	Reentry out;
	std::vector<float> l(256), r(256);
	const size_t total = 48000 * 12;
	double sustainE = 0;
	size_t over = 0;
	for (size_t at = 0; at < total; at += 256) {
		for (size_t k = 0; k < 256; ++k) {
			const double t = static_cast<double>(at + k) / 48000.0;
			const bool playing = t < 2.0 || (t >= 8.0 && t < 10.0);
			const double x = playing && std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 800.0 * t) : 0.0;
			l[k] = r[k] = static_cast<float>(x);
		}
		if (at == 48000 * 8 - 256 * 1) {
			out.scaleBefore = dev.coreForTest().owlServoScaleForTest();
		}
		dev.processStereo(l.data(), r.data(), 256);
		for (size_t k = 0; k < 256; ++k) {
			const double v = std::fabs(static_cast<double>(l[k]));
			const size_t i = at + k;
			if (i < 48000 * 2) {
				out.firstPeak = std::max(out.firstPeak, v);
			}
			else if (i >= 48000 * 6 && i < 48000 * 8) {
				sustainE += v * v;
			}
			else if (i >= 48000 * 8 && i < 48000 * 10) {
				out.againPeak = std::max(out.againPeak, v);
				over += v > 1.0 ? 1 : 0;
			}
		}
	}
	out.overMs = static_cast<double>(over) / 48.0;
	out.sustain = 10 * std::log10(sustainE / (48000.0 * 2) + 1e-20);
	return out;
}

} // namespace

TEST_CASE("MEASURE: Owl, notes into a climbed servo — 800 Hz plucks, 6 s of tail, plucks again" * doctest::skip()) {
	struct Variant {
		const char* name;
		Core::OwlProbe probe;
		bool comp;
	};
	auto ceil = [](float db, int env = 0) {
		Core::OwlProbe p;
		p.ceilDb = db;
		p.envelope = env;
		return p;
	};
	auto comp = [](float at, float ratio) {
		Core::OwlProbe p;
		p.compAt = at;
		p.compRatio = ratio;
		return p;
	};
	const Variant variants[] = {
		{ "firmware          ", {}, false },
		{ "comp .005 r4 (now)", {}, true },
		{ "comp .002 r20     ", comp(0.002f, 20.0f), false },
		{ "ceiling 0 dBFS    ", ceil(0.0f), false },
		{ "ceiling -6        ", ceil(-6.0f), false },
		{ "ceiling -6, mean  ", ceil(-6.0f, 1), false },
		{ "ceiling -6 + comp ", ceil(-6.0f), true },
		{ "ceil -6 mean+comp ", ceil(-6.0f, 1), true },
	};
	for (const Variant& v : variants) {
		double first = 0, again = 0, scale = 0, overMs = 0, worstOver = 0, sustain = 0, worstJump = -100;
		int n = 0;
		for (const float room : { 0.6f, 1.0f }) {
			for (const float decay : { 0.0f, 0.5f, 1.0f }) {
				for (const float ch : { 0.0f, 1.3f, 2.9f, 5.55f }) {
					const Reentry e = owlReentry(v.probe, v.comp, room, decay, ch);
					first += 20 * std::log10(e.firstPeak + 1e-9);
					again += 20 * std::log10(e.againPeak + 1e-9);
					worstJump = std::max(worstJump, 20 * std::log10((e.againPeak + 1e-9) / (e.firstPeak + 1e-9)));
					scale += e.scaleBefore;
					overMs += e.overMs;
					worstOver = std::max(worstOver, e.overMs);
					sustain += e.sustain;
					++n;
				}
			}
		}
		MESSAGE(std::string(v.name) << ": servo before the notes " << scale / n << "; peak first " << first / n
		                            << " dBFS, again " << again / n << " (worst jump " << worstJump << " dB); over 0 dBFS "
		                            << overMs / n << " ms (worst " << worstOver << "); tail 6-8 s " << sustain / n << " dB");
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the wet's peak and level against its input in every kind — 800 Hz plucks at -6 dBFS" * doctest::skip()) {
	for (int mode = 0; mode < fv::kModes; ++mode) {
		for (const float decay : { 0.0f, 0.5f }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.decay = decay;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			std::vector<float> l(48000 * 3), r;
			for (size_t i = 0; i < l.size(); ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				l[i] = static_cast<float>(t < 2.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < l.size(); at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, l.size() - at)));
			}
			double peak = 0, e = 0;
			for (const float v : l) {
				peak = std::max(peak, static_cast<double>(std::fabs(v)));
				e += static_cast<double>(v) * v;
			}
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << " decay " << decay << ": wet peak " << 20 * std::log10(peak + 1e-9)
			                                               << " dBFS, rms " << 10 * std::log10(e / l.size() + 1e-20)
			                                               << " dB (input peak -6 dBFS)");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl's wet level along Decay — 800 Hz plucks at -6 dBFS, compressor on and off" * doctest::skip()) {
	for (int step = 0; step <= 10; ++step) {
		const float decay = static_cast<float>(step) / 10.0f;
		double pk[2], rms[2], servo[2];
		for (int comp = 0; comp < 2; ++comp) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = fv::kOwlMenu;
			s.decay = decay;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setOwlCompressorForTest(comp == 1);
			const size_t total = 48000 * 3;
			std::vector<float> l(256), r(256);
			double peak = 0, e = 0, sv = 0;
			int svn = 0;
			for (size_t at = 0; at < total; at += 256) {
				for (size_t k = 0; k < 256; ++k) {
					const double t = static_cast<double>(at + k) / 48000.0;
					l[k] = r[k] = static_cast<float>(t < 2.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
				}
				dev.processStereo(l.data(), r.data(), 256);
				for (const float v : l) {
					peak = std::max(peak, static_cast<double>(std::fabs(v)));
					e += static_cast<double>(v) * v;
				}
				sv += dev.coreForTest().owlServoScaleForTest();
				++svn;
			}
			pk[comp] = 20 * std::log10(peak + 1e-9);
			rms[comp] = 10 * std::log10(e / total + 1e-20);
			servo[comp] = sv / svn;
		}
		MESSAGE("decay " << decay << ": peak " << pk[0] << " / comp " << pk[1] << " dBFS, rms " << rms[0] << " / " << rms[1]
		                 << ", mean servo " << servo[0] << " / " << servo[1]);
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Owl's compressor at the loop's start with a servo-aware threshold — re-entry, Decay, tone" * doctest::skip()) {
	struct Variant {
		const char* name;
		float at, ratio, pow;
		int place;
	};
	const Variant variants[] = {
		{ "return .005 r4 (now)  ", 0.005f, 4.0f, 0.0f, 0 }, { "start .005 r4         ", 0.005f, 4.0f, 0.0f, 1 },
		{ "start .005 r4 servo^1 ", 0.005f, 4.0f, 1.0f, 1 }, { "start .005 r4 servo^2 ", 0.005f, 4.0f, 2.0f, 1 },
		{ "start .003 r8 servo^2 ", 0.003f, 8.0f, 2.0f, 1 }, { "start .002 r8 servo^2 ", 0.002f, 8.0f, 2.0f, 1 },
		{ "return .005 r4 servo^2", 0.005f, 4.0f, 2.0f, 0 },
	};
	for (const Variant& v : variants) {
		Core::OwlProbe p;
		p.compAt = v.at;
		p.compRatio = v.ratio;
		p.compServoPow = v.pow;
		p.compPlace = v.place;
		double again = 0, overMs = 0, worstOver = 0, pluck = 0, pad = 0, padC = 0, sustain = 0;
		int n = 0;
		for (const float room : { 0.6f, 1.0f }) {
			for (const float decay : { 0.0f, 0.5f, 1.0f }) {
				for (const float ch : { 0.0f, 1.3f, 2.9f }) {
					const Reentry e = owlReentry(p, false, room, decay, ch);
					again += 20 * std::log10(e.againPeak + 1e-9);
					overMs += e.overMs;
					worstOver = std::max(worstOver, e.overMs);
					sustain += e.sustain;
					uint64_t f = 0;
					const Tone a = toneOf(owlRender({}, 1, room, decay, ch, f));
					const Tone b = toneOf(owlRender(p, 1, room, decay, ch, f));
					pluck += b.level - a.level;
					const Tone c = toneOf(owlRender({}, 2, room, decay, ch, f));
					const Tone d = toneOf(owlRender(p, 2, room, decay, ch, f));
					pad += d.level - c.level;
					padC += 1200.0 * std::log2(d.centroid / c.centroid);
					++n;
				}
			}
		}
		MESSAGE(std::string(v.name) << ": notes into a climbed servo — peak " << again / n << " dBFS, over 0 dBFS "
		                            << overMs / n << " ms (worst " << worstOver << "); tail " << sustain / n << " dB | plucks "
		                            << pluck / n << " dB | pad " << pad / n << " dB, " << padC / n << " c");
	}
	CHECK(true);
}

namespace {

/// OWL'S WET LEVEL (dB, the mean of its RMS) at `decay`, over Room .3/.6/1 x Character 0/1.3/2.9 x 220/800/2000 Hz
/// sine plucks at -6 dBFS, the compressor on, the wet ceiling off (the level the trim is for, not the ceiling's).
double owlLevelAt(float decay, bool trim) {
	double sum = 0.0;
	int n = 0;
	for (const float room : { 0.3f, 0.6f, 1.0f }) {
		for (const float ch : { 0.0f, 1.3f, 2.9f }) {
			for (const double hz : { 220.0, 800.0, 2000.0 }) {
				fv::Settings s;
				s.mix = 1.0f;
				s.sizeMode = fv::kOwlMenu;
				s.room = room;
				s.decay = decay;
				s.character = ch;
				fv::Featherverb dev;
				dev.setTarget(s);
				dev.prepare(48000.0);
				dev.setWetLimiterForTest(false);
				dev.setOwlTrimForTest(trim);
				const size_t total = 48000 * 3;
				std::vector<float> l(total), r;
				for (size_t i = 0; i < total; ++i) {
					const double t = static_cast<double>(i) / 48000.0;
					l[i] = static_cast<float>(t < 2.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * hz * t) : 0.0);
				}
				r = l;
				for (size_t at = 0; at < total; at += 256) {
					dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
				}
				double e = 0;
				for (const float v : l) {
					e += static_cast<double>(v) * v;
				}
				sum += 10 * std::log10(e / static_cast<double>(total) + 1e-20);
				++n;
			}
		}
	}
	return sum / n;
}

} // namespace

TEST_CASE("Owl's trim holds its level along Decay — and when it drifts, this case prints the table to paste") {
	// THE FIT, GUARDED (a fitted constant must be redone if the plugin changes, so this says when). The trim
	// (`kOwlDecayTrimDb`) is fitted to Owl's measured level; anything that moves Owl's level — its loop, its compressor,
	// the send staging — moves it off. So this re-measures Owl, TRIMMED, at every table point and asks each to stay within
	// 3 dB of Decay 0's. When one does not (or FEATHERS_OWL_TRIM_FIT is set), it measures the untrimmed level too and
	// prints the new table: refitting is a paste. (The untrimmed pass is the other half of the time, so only then.)
	std::vector<double> trimmed(11);
	bool drifted = false;
	for (int i = 0; i <= 10; ++i) {
		trimmed[static_cast<size_t>(i)] = owlLevelAt(static_cast<float>(i) / 10.0f, true);
		drifted = drifted || std::fabs(trimmed[static_cast<size_t>(i)] - trimmed[0]) >= 3.0;
	}
	if (drifted || std::getenv("FEATHERS_OWL_TRIM_FIT") != nullptr) {
		std::vector<double> raw(11);
		for (int i = 0; i <= 10; ++i) {
			raw[static_cast<size_t>(i)] = owlLevelAt(static_cast<float>(i) / 10.0f, false);
		}
		// THE FIT, as it would be pasted: each point brought back to Decay 0's level, never boosted, and never less trim
		// at a higher Decay than a lower one (a monotone table: Decay only ever turns Owl up).
		std::string table = "inline constexpr float kOwlDecayTrimDb[11] = {";
		std::string line;
		double deepest = 0.0;
		for (int i = 0; i <= 10; ++i) {
			deepest = std::min(deepest, std::min(0.0, raw[0] - raw[static_cast<size_t>(i)]));
			char buf[48];
			std::snprintf(buf, sizeof buf, "%s %.1ff", i == 0 ? "" : ",", deepest);
			table += buf;
			std::snprintf(buf, sizeof buf, " %.1f:%+.1f/%+.1f", i / 10.0, raw[static_cast<size_t>(i)], trimmed[static_cast<size_t>(i)]);
			line += buf;
		}
		table += " };";
		MESSAGE("Owl level (dB) at Decay: raw/trimmed —" << line);
		MESSAGE("THE FIT, to paste into featherverb_types.h:\n" << table);
	}
	for (int i = 0; i <= 10; ++i) {
		CAPTURE(i);
		CAPTURE(trimmed[static_cast<size_t>(i)]);
		CHECK(std::fabs(trimmed[static_cast<size_t>(i)] - trimmed[0]) < 3.0);
	}
}

TEST_CASE("MEASURE: the audible noise — energy OFF the note's harmonics, each Owl stage on and off" * doctest::skip()) {
	struct Config {
		const char* name;
		bool comp, trim, limiter;
		Core::OwlProbe probe;
	};
	auto smooth = [](float atk, float rel, float gainStep) {
		Core::OwlProbe p;
		p.compAt = 0.005f;
		p.compRatio = 4.0f;
		p.compPlace = 1;
		p.compServoPow = 2.0f;
		p.compAttack = atk;
		p.compRelease = rel;
		p.compGainSmooth = gainStep;
		return p;
	};
	const Config configs[] = {
		{ "firmware (all off)       ", false, false, false, {} },
		{ "+ trim only              ", false, true, false, {} },
		{ "+ limiter only           ", false, false, true, {} },
		{ "+ compressor only        ", true, false, false, {} },
		{ "as shipped (all three)   ", true, true, true, {} },
		{ "comp, gain smoothed .002 ", false, false, false, smooth(0.0f, 0.0f, 0.002f) },
		{ "comp, atk .002 + gain    ", false, false, false, smooth(0.002f, 0.0f, 0.002f) },
		{ "comp, atk .0005 rel .0001", false, false, false, smooth(0.0005f, 0.0001f, 0.0f) },
	};
	// OFF-HARMONIC ENERGY (dB, relative to all of it): outside +-60 Hz of every multiple of 800 Hz, 100 Hz..20 kHz.
	const auto offHarmonic = [](const std::vector<double>& x) {
		const auto p = powerSpectrum(x, 8192);
		double on = 0, off = 0;
		for (size_t k = 1; k < p.size(); ++k) {
			const double f = static_cast<double>(k) * 48000.0 / 8192.0;
			if (f < 100.0 || f > 20000.0) {
				continue;
			}
			const double nearest = std::round(f / 800.0) * 800.0;
			(std::fabs(f - nearest) <= 60.0 && nearest > 0.0 ? on : off) += p[k];
		}
		return 10 * std::log10(off / (on + off) + 1e-30);
	};
	for (const float decay : { 0.5f, 1.0f }) {
		for (const Config& c : configs) {
			double steady = 0, tail = 0;
			int n = 0;
			for (const float ch : { 0.0f, 1.3f }) {
				for (const int material : { 0, 1 }) { // 0 a held tone, 1 the reported plucks
					fv::Settings s;
					s.mix = 1.0f;
					s.sizeMode = fv::kOwlMenu;
					s.room = 0.6f;
					s.decay = decay;
					s.character = ch;
					fv::Featherverb dev;
					dev.setTarget(s);
					dev.prepare(48000.0);
					dev.setOwlCompressorForTest(c.comp);
					dev.setOwlTrimForTest(c.trim);
					dev.setWetLimiterForTest(c.limiter);
					dev.setOwlProbeForTest(c.probe);
					const size_t total = 48000 * 6;
					std::vector<float> l(total), r;
					for (size_t i = 0; i < total; ++i) {
						const double t = static_cast<double>(i) / 48000.0;
						const bool on = material == 0 ? t < 4.0 : (t < 4.0 && std::fmod(t, 0.5) < 0.1);
						l[i] = static_cast<float>(on ? 0.5 * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
					}
					r = l;
					for (size_t at = 0; at < total; at += 256) {
						dev.processStereo(l.data() + at, r.data() + at, 256);
					}
					const std::vector<double> driven(l.begin() + 48000, l.begin() + 48000 * 4);
					const std::vector<double> after(l.begin() + 48000 * 4, l.end());
					steady += offHarmonic(driven);
					tail += offHarmonic(after);
					++n;
				}
			}
			MESSAGE("decay " << decay << " " << std::string(c.name) << ": off-harmonic energy while playing " << steady / n
			                 << " dB, in the tail " << tail / n << " dB");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: where the off-harmonic energy sits in Owl, Sky and Vast, and what Oversample does to it" * doctest::skip()) {
	for (const int mode : { 5, fv::kVastMenu, fv::kOwlMenu }) {
		for (const bool os : { false, true }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.room = 0.6f;
			s.decay = 0.5f;
			s.oversample = os;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			const size_t total = 48000 * 5;
			std::vector<float> l(total), r;
			for (size_t i = 0; i < total; ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				l[i] = static_cast<float>(t < 4.0 && std::fmod(t, 0.5) < 0.1 ? 0.5 * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < total; at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
			}
			const std::vector<double> x(l.begin() + 48000, l.end());
			const auto p = powerSpectrum(x, 8192);
			double total_e = 0, offBands[5] = {};
			const double edges[] = { 100, 3000, 6000, 10000, 14000, 20000 };
			for (size_t k = 1; k < p.size(); ++k) {
				const double f = static_cast<double>(k) * 48000.0 / 8192.0;
				if (f < 100.0 || f > 20000.0) {
					continue;
				}
				total_e += p[k];
				const double nearest = std::round(f / 800.0) * 800.0;
				if (std::fabs(f - nearest) > 60.0) {
					for (int b = 0; b < 5; ++b) {
						if (f >= edges[b] && f < edges[b + 1]) {
							offBands[b] += p[k];
						}
					}
				}
			}
			std::string line;
			for (int b = 0; b < 5; ++b) {
				char buf[64];
				std::snprintf(buf, sizeof buf, " %.0f-%.0fk %.1f", edges[b] / 1000, edges[b + 1] / 1000,
				              10 * std::log10(offBands[b] / total_e + 1e-30));
				line += buf;
			}
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << (os ? " 2x " : " 1x ") << "off-harmonic by band (dB of all):" << line);
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: clicks around the attack — the second difference's spikes, the wet ceiling on and off" * doctest::skip()) {
	for (const int mode : { 5, fv::kOwlMenu, 4 }) {
		for (const float level : { 0.5f, 1.0f }) {
			double spike[2] = {}, limited[2] = {};
			for (int lim = 0; lim < 2; ++lim) {
				fv::Settings s;
				s.mix = 1.0f;
				s.sizeMode = mode;
				s.decay = 0.5f;
				fv::Featherverb dev;
				dev.setTarget(s);
				dev.prepare(48000.0);
				dev.setWetLimiterForTest(lim == 1);
				const size_t total = 48000 * 3;
				std::vector<float> l(total), r;
				for (size_t i = 0; i < total; ++i) {
					const double t = static_cast<double>(i) / 48000.0;
					const double u = std::fmod(t, 0.5);
					l[i] = static_cast<float>(u < 0.1 ? level * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
				}
				r = l;
				size_t over = 0;
				for (size_t at = 0; at < total; at += 256) {
					dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
					over += dev.limiterGainForTest() < 1.0f ? 1 : 0;
				}
				// THE SECOND DIFFERENCE, scaled by the local level: a smooth 800 Hz-ish wet gives a steady small ratio; a gain
				// step inside one sample is a spike on top of it.
				double worst = 0;
				for (size_t i = 2; i < total; ++i) {
					const double d2 = std::fabs(static_cast<double>(l[i]) - 2.0 * l[i - 1] + l[i - 2]);
					const double lv = std::fabs(static_cast<double>(l[i - 1])) + 1e-3;
					worst = std::max(worst, d2 / lv);
				}
				spike[lim] = worst;
				limited[lim] = static_cast<double>(over);
			}
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << " input " << level << ": worst d2/level, ceiling off " << spike[0]
			                                               << ", on " << spike[1] << "  (blocks limiting: " << limited[1] << ")");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Sky's C3 knee — how often it bites on plucks, and when" * doctest::skip()) {
	for (const float level : { 0.25f, 0.5f, 1.0f }) {
		for (const float decay : { 0.0f, 0.5f, 1.0f }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = 5;
			s.decay = decay;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			const size_t total = 48000 * 3;
			std::vector<float> l(256), r(256);
			uint64_t before = 0;
			uint64_t attack = 0, rest = 0;
			for (size_t at = 0; at < total; at += 256) {
				for (size_t k = 0; k < 256; ++k) {
					const double t = static_cast<double>(at + k) / 48000.0;
					const double u = std::fmod(t, 0.5);
					l[k] = r[k] = static_cast<float>(u < 0.1 ? level * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
				}
				dev.processStereo(l.data(), r.data(), 256);
				const uint64_t now = dev.coreForTest().clipsForTest();
				const double u = std::fmod(static_cast<double>(at) / 48000.0, 0.5);
				(u < 0.05 ? attack : rest) += now - before;
				before = now;
			}
			MESSAGE("input " << level << " decay " << decay << ": knee bit " << attack << " samples in the first 50 ms of a pluck, "
			                 << rest << " elsewhere");
		}
	}
	CHECK(true);
}

namespace {

/// CLICKS: energy above 15 kHz (a first-order difference, twice, is a steep highpass there), in dB relative to the whole
/// wet, split into the first 60 ms of each pluck (the attack) and the rest (the tail).
struct Clicks {
	double attack = 0, tail = 0;
};
Clicks clicksOf(int mode, const Core::OwlProbe& probe, float decay, bool os = false) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = mode;
	s.decay = decay;
	s.oversample = os;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setOwlProbeForTest(probe);
	const size_t total = 48000 * 4;
	std::vector<float> l(total), r;
	for (size_t i = 0; i < total; ++i) {
		const double t = static_cast<double>(i) / 48000.0;
		const double u = std::fmod(t, 0.5);
		l[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
	}
	r = l;
	for (size_t at = 0; at < total; at += 256) {
		dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
	}
	const std::vector<double> x(l.begin(), l.end());
	double all = 0, hiA = 0, hiT = 0;
	for (size_t i = 2; i < x.size(); ++i) {
		const double d2 = x[i] - 2.0 * x[i - 1] + x[i - 2]; // |H| ~ (2 sin(w/2))^2: -0.5 dB at 24k, -9 dB at 12k, -34 dB at 800
		all += x[i] * x[i];
		const double u = std::fmod(static_cast<double>(i) / 48000.0, 0.5);
		(u < 0.06 ? hiA : hiT) += d2 * d2;
	}
	return { 10 * std::log10(hiA / all + 1e-30), 10 * std::log10(hiT / all + 1e-30) };
}

} // namespace

TEST_CASE("MEASURE: Sky's and Owl's clicks — the modulation's per-buffer steps frozen, against as they are" * doctest::skip()) {
	Core::OwlProbe frozen;
	frozen.freezeLfo = true;
	for (const int mode : { 5, fv::kVastMenu, fv::kOwlMenu, 4 }) {
		for (const float decay : { 0.0f, 0.5f }) {
			const Clicks a = clicksOf(mode, {}, decay);
			const Clicks b = clicksOf(mode, frozen, decay);
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << " decay " << decay << ": highpassed energy, attack " << a.attack
			                                               << " -> frozen " << b.attack << " dB; tail " << a.tail << " -> "
			                                               << b.tail << " dB");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the tails' top octave — tonal images or broadband noise, and with Oversample" * doctest::skip()) {
	for (const int mode : { 5, fv::kOwlMenu, 4 }) {
		for (const bool os : { false, true }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.decay = 0.5f;
			s.oversample = os;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			const size_t total = 48000 * 4;
			std::vector<float> l(total), r;
			for (size_t i = 0; i < total; ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				const double u = std::fmod(t, 0.5);
				l[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < total; at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
			}
			const std::vector<double> x(l.begin() + 48000, l.end());
			const auto p = powerSpectrum(x, 8192);
			double all = 0;
			for (size_t k = 1; k < p.size(); ++k) {
				all += p[k];
			}
			// BANDS, and in the top one the peak bin against the band's median: tonal images show as peaks, noise is flat.
			std::string line;
			const double edges[] = { 6000, 10000, 14000, 18000, 22000, 24000 };
			for (int b = 0; b < 5; ++b) {
				std::vector<double> bins;
				double e = 0;
				for (size_t k = 1; k < p.size(); ++k) {
					const double f = static_cast<double>(k) * 48000.0 / 8192.0;
					if (f >= edges[b] && f < edges[b + 1]) {
						bins.push_back(p[k]);
						e += p[k];
					}
				}
				std::sort(bins.begin(), bins.end());
				const double crest = bins.empty() ? 0.0 : 10 * std::log10(bins.back() / (bins[bins.size() / 2] + 1e-30));
				char buf[80];
				std::snprintf(buf, sizeof buf, " %.0f-%.0fk %.1f dB (peak/median %.0f)", edges[b] / 1000, edges[b + 1] / 1000,
				              10 * std::log10(e / all + 1e-30), crest);
				line += buf;
			}
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << (os ? " 2x" : " 1x") << ":" << line);
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: Sky's noise floor (6-14 kHz, 2x on so no images) — the modulation frozen, and when in the pluck" * doctest::skip()) {
	const auto band = [](int mode, const Core::OwlProbe& probe, float decay, float character) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.decay = decay;
		s.character = character;
		s.oversample = true;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		dev.setOwlProbeForTest(probe);
		const size_t total = 48000 * 4;
		std::vector<float> l(total), r;
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / 48000.0;
			const double u = std::fmod(t, 0.5);
			l[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
		}
		r = l;
		for (size_t at = 0; at < total; at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
		}
		// Each 50 ms window's 6-14 kHz share, so "at the attack" and "in the tail" can be told apart.
		double atk = 0, tail = 0, allA = 0, allT = 0;
		for (size_t at = 48000; at + 2048 <= total; at += 2400) {
			const std::vector<double> w(l.begin() + static_cast<long>(at), l.begin() + static_cast<long>(at + 2048));
			const auto p = powerSpectrum(w, 2048);
			double hi = 0, all = 0;
			for (size_t k = 1; k < p.size(); ++k) {
				const double f = static_cast<double>(k) * 48000.0 / 2048.0;
				all += p[k];
				hi += f >= 6000.0 && f < 14000.0 ? p[k] : 0.0;
			}
			const double u = std::fmod(static_cast<double>(at) / 48000.0, 0.5);
			if (u < 0.1) {
				atk += hi;
				allA += all;
			}
			else {
				tail += hi;
				allT += all;
			}
		}
		return std::pair<double, double> { 10 * std::log10(atk / allA + 1e-30), 10 * std::log10(tail / allT + 1e-30) };
	};
	Core::OwlProbe frozen;
	frozen.freezeLfo = true;
	for (const int mode : { 5, fv::kVastMenu, fv::kOwlMenu, 4 }) {
		for (const float ch : { 0.0f, 1.3f }) {
			const auto a = band(mode, {}, 0.5f, ch);
			const auto b = band(mode, frozen, 0.5f, ch);
			MESSAGE(std::string(fv::kSizeModeNames[mode]) << " char " << ch << ": 6-14 kHz share, during the pluck " << a.first
			                                               << " (frozen " << b.first << "), after it " << a.second << " (frozen "
			                                               << b.second << ") dB");
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the modulation smoothed — the walk ramped, the cascade's taps fractional — against stepped and frozen" * doctest::skip()) {
	const auto band = [](int mode, const Core::OwlProbe& probe, bool os) {
		double atk = 0, tail = 0, allA = 0, allT = 0;
		for (const float ch : { 0.0f, 1.3f }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.decay = 0.5f;
			s.character = ch;
			s.oversample = os;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setOwlProbeForTest(probe);
			const size_t total = 48000 * 4;
			std::vector<float> l(total), r;
			for (size_t i = 0; i < total; ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				const double u = std::fmod(t, 0.5);
				l[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < total; at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
			}
			for (size_t at = 48000; at + 2048 <= total; at += 2400) {
				const std::vector<double> w(l.begin() + static_cast<long>(at), l.begin() + static_cast<long>(at + 2048));
				const auto p = powerSpectrum(w, 2048);
				double hi = 0, all = 0;
				for (size_t k = 1; k < p.size(); ++k) {
					const double f = static_cast<double>(k) * 48000.0 / 2048.0;
					all += p[k];
					hi += f >= 6000.0 && f < 14000.0 ? p[k] : 0.0;
				}
				const double u = std::fmod(static_cast<double>(at) / 48000.0, 0.5);
				if (u < 0.1) {
					atk += hi;
					allA += all;
				}
				else {
					tail += hi;
					allT += all;
				}
			}
		}
		return std::pair<double, double> { 10 * std::log10(atk / allA + 1e-30), 10 * std::log10(tail / allT + 1e-30) };
	};
	Core::OwlProbe ramp, frac, both, frozen;
	ramp.rampLfo = true;
	frac.fracCascade = true;
	both.rampLfo = true;
	both.fracCascade = true;
	frozen.freezeLfo = true;
	struct V {
		const char* name;
		Core::OwlProbe p;
	};
	const V variants[] = { { "as is      ", {} }, { "ramped walk", ramp }, { "frac taps  ", frac }, { "both       ", both },
		                   { "frozen     ", frozen } };
	for (const bool os : { false, true }) {
		for (const int mode : { 5, fv::kVastMenu, fv::kOwlMenu }) {
			for (const V& v : variants) {
				const auto r = band(mode, v.p, os);
				MESSAGE(std::string(fv::kSizeModeNames[mode]) << (os ? " 2x " : " 1x ") << std::string(v.name)
				                                               << ": 6-14 kHz during " << r.first << ", after " << r.second << " dB");
			}
		}
	}
	CHECK(true);
}

TEST_CASE("Sky's modulation is ramped, not stepped: its 6-14 kHz noise floor sits well under the firmware's") {
	// Sky had tiny noise impulses around the attack. The firmware steps its random walk — and
	// the wobble's lengths and gains — once per buffer; the device ramps the same values across it
	// (`Core::setSmoothModulation`). MEASURED 17-19 dB lower at 1x and 2x; the wobble itself is the same values.
	const auto floorOf = [](int smooth) { // -1: the device as shipped, 0/1: forced
		double atk = 0, tail = 0, allA = 0, allT = 0;
		for (const float ch : { 0.0f, 1.3f }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = 5;
			s.decay = 0.5f;
			s.character = ch;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			if (smooth >= 0) {
				dev.setSmoothModulationForTest(smooth == 1);
			}
			const size_t total = 48000 * 3;
			std::vector<float> l(total), r;
			for (size_t i = 0; i < total; ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				const double u = std::fmod(t, 0.5);
				l[i] = static_cast<float>(u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < total; at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
			}
			for (size_t at = 24000; at + 2048 <= total; at += 2400) {
				const std::vector<double> w(l.begin() + static_cast<long>(at), l.begin() + static_cast<long>(at + 2048));
				const auto p = powerSpectrum(w, 2048);
				double hi = 0, all = 0;
				for (size_t k = 1; k < p.size(); ++k) {
					const double f = static_cast<double>(k) * 48000.0 / 2048.0;
					all += p[k];
					hi += f >= 6000.0 && f < 14000.0 ? p[k] : 0.0;
				}
				if (std::fmod(static_cast<double>(at) / 48000.0, 0.5) < 0.1) {
					atk += hi;
					allA += all;
				}
				else {
					tail += hi;
					allT += all;
				}
			}
		}
		return std::pair<double, double> { 10 * std::log10(atk / allA + 1e-30), 10 * std::log10(tail / allT + 1e-30) };
	};
	const auto stepped = floorOf(0);
	const auto ramped = floorOf(1);
	const auto shipped = floorOf(-1);
	MESSAGE("Sky 6-14 kHz: during the pluck " << stepped.first << " -> " << ramped.first << " dB, after " << stepped.second
	                                          << " -> " << ramped.second << " dB");
	CHECK(ramped.first < stepped.first - 12.0);
	CHECK(ramped.second < stepped.second - 12.0);
	// AND THE DEVICE SHIPS RAMPED: untouched, it is the ramped one exactly.
	CHECK(shipped.first == ramped.first);
	CHECK(shipped.second == ramped.second);
}

TEST_CASE("MEASURE: the TAIL's crackle in Sky, Vast and Owl — which per-buffer step is left (the walk is ramped)" * doctest::skip()) {
	const auto tailFloor = [](int mode, const Core::OwlProbe& probe, bool os) {
		double hi = 0, all = 0, kurtNum = 0, kurtDen = 0;
		for (const float ch : { 0.0f, 1.3f }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.decay = 0.5f;
			s.character = ch;
			s.oversample = os;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setOwlProbeForTest(probe);
			const size_t total = 48000 * 6;
			std::vector<float> l(total), r;
			for (size_t i = 0; i < total; ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				const double u = std::fmod(t, 0.5);
				l[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < total; at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
			}
			// THE PURE TAIL, 3.5-6 s: the 6-14 kHz share, and the kurtosis of the highpassed signal (a crackle is
			// impulsive: a Gaussian noise floor reads 3, a click train far more).
			for (size_t at = 48000 * 7 / 2; at + 2048 <= total; at += 2048) {
				const std::vector<double> w(l.begin() + static_cast<long>(at), l.begin() + static_cast<long>(at + 2048));
				const auto p = powerSpectrum(w, 2048);
				for (size_t k = 1; k < p.size(); ++k) {
					const double f = static_cast<double>(k) * 48000.0 / 2048.0;
					all += p[k];
					hi += f >= 6000.0 && f < 14000.0 ? p[k] : 0.0;
				}
			}
			for (size_t i = 48000 * 7 / 2 + 2; i < total; ++i) {
				const double d2 = static_cast<double>(l[i]) - 2.0 * l[i - 1] + l[i - 2];
				kurtNum += d2 * d2 * d2 * d2;
				kurtDen += d2 * d2;
			}
		}
		const double n = 2.0 * static_cast<double>(48000 * 6 - 48000 * 7 / 2);
		const double kurt = (kurtNum / n) / ((kurtDen / n) * (kurtDen / n) + 1e-60);
		return std::pair<double, double> { 10 * std::log10(hi / all + 1e-30), kurt };
	};
	Core::OwlProbe lfo, env, servo, all;
	lfo.freezeLfo = true;
	env.freezeEnv = true;
	servo.freezeServo = true;
	all.freezeLfo = all.freezeEnv = all.freezeServo = true;
	struct V {
		const char* name;
		Core::OwlProbe p;
	};
	const V variants[] = { { "as shipped   ", {} }, { "walk frozen  ", lfo }, { "env frozen   ", env },
		                   { "servo frozen ", servo }, { "all frozen   ", all } };
	for (const bool os : { false, true }) {
		for (const int mode : { 5, fv::kVastMenu, fv::kOwlMenu }) {
			for (const V& v : variants) {
				const auto t = tailFloor(mode, v.p, os);
				MESSAGE(std::string(fv::kSizeModeNames[mode]) << (os ? " 2x " : " 1x ") << std::string(v.name) << ": tail 6-14 kHz "
				                                               << t.first << " dB, highpassed kurtosis " << t.second);
			}
		}
	}
	CHECK(true);
}

TEST_CASE("The tails of Sky, Vast and Owl are clean: with the modulation smoothed, their 6-14 kHz floor sits under the firmware's") {
	// Sky, Vast and Owl still had a slight crackle on the tail. With the walk ramped, the rest was
	// the stereo rotation and Owl's cascade lowpasses (still stepped per buffer), and Vast's modulated reads of its
	// DOUBLE-WRITTEN FDN lines (a staircase, read with a moving pointer). MEASURED, the pure tail (3.5-6 s), the firmware's
	// stepping -> smoothed: Sky 1x -45.9 -> -72.6 dB; Owl 2x -63.2 -> -90.4; Vast 2x -53.3 -> -86.4. EACH HALF IS NEEDED:
	// with the walk ramped but the rotation still stepped, Sky reached only -58.7 and Owl -68.5; without the pair reader,
	// Vast -79.2 — so each threshold sits between the two (a known-bad of either half goes red).

	const auto tailFloor = [](int mode, bool smooth, bool os) {
		double hi = 0, all = 0;
		for (const float ch : { 0.0f, 1.3f }) {
			fv::Settings s;
			s.mix = 1.0f;
			s.sizeMode = mode;
			s.decay = 0.5f;
			s.character = ch;
			s.oversample = os;
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setSmoothModulationForTest(smooth);
			const size_t total = 48000 * 6;
			std::vector<float> l(total), r;
			for (size_t i = 0; i < total; ++i) {
				const double t = static_cast<double>(i) / 48000.0;
				const double u = std::fmod(t, 0.5);
				l[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			r = l;
			for (size_t at = 0; at < total; at += 256) {
				dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(256, total - at)));
			}
			for (size_t at = 48000 * 7 / 2; at + 2048 <= total; at += 2048) {
				const std::vector<double> w(l.begin() + static_cast<long>(at), l.begin() + static_cast<long>(at + 2048));
				const auto p = powerSpectrum(w, 2048);
				for (size_t k = 1; k < p.size(); ++k) {
					const double f = static_cast<double>(k) * 48000.0 / 2048.0;
					all += p[k];
					hi += f >= 6000.0 && f < 14000.0 ? p[k] : 0.0;
				}
			}
		}
		return 10 * std::log10(hi / all + 1e-30);
	};
	struct Case {
		int mode;
		bool os;
		double atLeast; // dB lower than stepped: MEASURED 26.7, 27.2, 33.1 whole; 12.8, 5.3, 25.9 with a half missing
	};
	for (const Case c : { Case { 5, false, 20.0 }, Case { fv::kOwlMenu, true, 20.0 }, Case { fv::kVastMenu, true, 29.0 } }) {
		const double stepped = tailFloor(c.mode, false, c.os);
		const double smoothed = tailFloor(c.mode, true, c.os);
		CAPTURE(c.mode);
		CAPTURE(stepped);
		CAPTURE(smoothed);
		MESSAGE("mode " << c.mode << (c.os ? " 2x" : " 1x") << ": stepped " << stepped << ", smoothed " << smoothed);
		CHECK(smoothed < stepped - c.atLeast);
	}
}

namespace {

/// CLICKS IN A TAIL: samples whose second difference jumps above SIX times its local RMS (a 5 ms window either side).
/// A smooth tail — even a noisy one — almost never does (a Gaussian: ~2 per billion); a crackle is exactly that.
/// Per second of tail, both channels.
double clicksPerSecond(const std::vector<float>& l, const std::vector<float>& r, size_t from, size_t to) {
	double clicks = 0;
	for (const std::vector<float>* ch : { &l, &r }) {
		const std::vector<float>& x = *ch;
		std::vector<double> d2(x.size(), 0.0);
		for (size_t i = from + 2; i < to; ++i) {
			d2[i] = static_cast<double>(x[i]) - 2.0 * x[i - 1] + x[i - 2];
		}
		const size_t w = 240;
		double acc = 0;
		for (size_t i = from; i < from + 2 * w && i < to; ++i) {
			acc += d2[i] * d2[i];
		}
		for (size_t i = from + w; i + w < to; ++i) {
			const double rms = std::sqrt(acc / static_cast<double>(2 * w)) + 1e-30;
			if (std::fabs(d2[i]) > 6.0 * rms && std::fabs(d2[i]) > 1e-7) {
				clicks += 1.0;
			}
			acc += d2[i + w] * d2[i + w] - d2[i - w] * d2[i - w];
		}
	}
	return clicks / (static_cast<double>(to - from) / 48000.0);
}

} // namespace

TEST_CASE("MEASURE: clicks per second in every kind's tail — host block sizes, Oversample" * doctest::skip()) {
	for (int mode = 0; mode < fv::kModes; ++mode) {
		std::string line;
		for (const bool os : { false, true }) {
			for (const int block : { 64, 128, 512 }) {
				fv::Settings s;
				s.mix = 1.0f;
				s.sizeMode = mode;
				s.decay = 0.5f;
				s.oversample = os;
				fv::Featherverb dev;
				dev.setTarget(s);
				dev.prepare(48000.0);
				const size_t total = 48000 * 6;
				std::vector<float> l(total), r(total);
				for (size_t i = 0; i < total; ++i) {
					const double t = static_cast<double>(i) / 48000.0;
					const double u = std::fmod(t, 0.5);
					l[i] = r[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
				}
				for (size_t at = 0; at < total; at += static_cast<size_t>(block)) {
					dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(static_cast<size_t>(block), total - at)));
				}
				char buf[48];
				std::snprintf(buf, sizeof buf, " %s/%d: %.0f", os ? "2x" : "1x", block, clicksPerSecond(l, r, 48000 * 7 / 2, total));
				line += buf;
			}
		}
		MESSAGE(std::string(fv::kSizeModeNames[mode]) << " tail clicks/s —" << line);
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the click detector can see a click — one planted in a real Sky tail, at three sizes" * doctest::skip()) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = 5;
	s.decay = 0.5f;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	const size_t total = 48000 * 6;
	std::vector<float> l(total), r(total);
	for (size_t i = 0; i < total; ++i) {
		const double t = static_cast<double>(i) / 48000.0;
		const double u = std::fmod(t, 0.5);
		l[i] = r[i] = static_cast<float>(t < 3.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
	}
	for (size_t at = 0; at < total; at += 256) {
		dev.processStereo(l.data() + at, r.data() + at, 256);
	}
	double rms = 0;
	for (size_t i = 48000 * 4; i < 48000 * 5; ++i) {
		rms += static_cast<double>(l[i]) * l[i];
	}
	rms = std::sqrt(rms / 48000.0);
	MESSAGE("tail rms at 4-5 s: " << 20 * std::log10(rms + 1e-30) << " dBFS; clicks/s unplanted " << clicksPerSecond(l, r, 48000 * 7 / 2, total));
	for (const double rel : { 0.01, 0.05, 0.2 }) {
		std::vector<float> a = l, b = r;
		for (int k = 0; k < 20; ++k) { // twenty steps in the tail, a tenth of a second apart: 20 clicks in 2.5 s = 8/s
			const size_t at = 48000 * 4 + static_cast<size_t>(k) * 4800;
			for (size_t i = at; i < a.size(); ++i) {
				a[i] += static_cast<float>(rel * rms) * (k % 2 == 0 ? 1.0f : -1.0f) * (i == at ? 1.0f : 0.0f);
			}
		}
		MESSAGE("planted impulses of " << rel << " x rms: clicks/s " << clicksPerSecond(a, b, 48000 * 7 / 2, total));
	}
	CHECK(true);
}

TEST_CASE("MEASURE: a floor under the late tail? — band levels over 12 s at 44.1 kHz, 512-frame blocks (a typical session)" * doctest::skip()) {
	for (int mode = 0; mode < fv::kModes; ++mode) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.decay = 0.5f;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(44100.0);
		const size_t total = 44100 * 14;
		std::vector<float> l(total), r(total);
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / 44100.0;
			const double u = std::fmod(t, 0.5);
			l[i] = r[i] = static_cast<float>(t < 2.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
		}
		for (size_t at = 0; at < total; at += 512) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, total - at)));
		}
		std::string lo, hi;
		for (double t0 : { 2.5, 4.0, 6.0, 8.0, 10.0, 12.0 }) {
			const auto at = static_cast<size_t>(t0 * 44100.0);
			const std::vector<double> w(l.begin() + static_cast<long>(at), l.begin() + static_cast<long>(at + 8192));
			const auto p = powerSpectrum(w, 8192);
			double a = 0, b = 0;
			for (size_t k = 1; k < p.size(); ++k) {
				const double f = static_cast<double>(k) * 44100.0 / 8192.0;
				(f < 2000.0 ? a : (f >= 6000.0 && f < 16000.0 ? b : a)) += f < 2000.0 || (f >= 6000.0 && f < 16000.0) ? p[k] : 0.0;
			}
			char buf[40];
			std::snprintf(buf, sizeof buf, " %.0fs:%.0f", t0, 10 * std::log10(a / 8192.0 + 1e-30));
			lo += buf;
			std::snprintf(buf, sizeof buf, " %.0fs:%.0f", t0, 10 * std::log10(b / 8192.0 + 1e-30));
			hi += buf;
		}
		MESSAGE(std::string(fv::kSizeModeNames[mode]) << " <2 kHz dB:" << lo << " | 6-16 kHz dB:" << hi);
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the late tail's quantisation — its real level, and the smallest step in it (44.1 kHz, 512 blocks)" * doctest::skip()) {
	for (const int mode : { 4, 5, 0, fv::kAuroraMenu }) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.decay = 0.5f;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(44100.0);
		const size_t total = 44100 * 12;
		std::vector<float> l(total), r(total);
		for (size_t i = 0; i < total; ++i) {
			const double t = static_cast<double>(i) / 44100.0;
			const double u = std::fmod(t, 0.5);
			l[i] = r[i] = static_cast<float>(t < 2.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
		}
		for (size_t at = 0; at < total; at += 512) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, total - at)));
		}
		std::string line;
		for (const double t0 : { 3.0, 5.0, 7.0, 9.0 }) {
			const auto at = static_cast<size_t>(t0 * 44100.0);
			double e = 0, smallest = 1e9;
			std::vector<float> distinct;
			for (size_t i = at; i < at + 4410; ++i) {
				e += static_cast<double>(l[i]) * l[i];
				if (l[i] != 0.0f) {
					smallest = std::min(smallest, static_cast<double>(std::fabs(l[i])));
				}
				distinct.push_back(l[i]);
			}
			std::sort(distinct.begin(), distinct.end());
			const size_t n = static_cast<size_t>(std::unique(distinct.begin(), distinct.end()) - distinct.begin());
			char buf[96];
			std::snprintf(buf, sizeof buf, " [%.0fs rms %.1f dBFS, smallest step %.1f dBFS, %zu values]", t0,
			              10 * std::log10(e / 4410.0 + 1e-30), 20 * std::log10(smallest), n);
			line += buf;
		}
		MESSAGE(std::string(fv::kSizeModeNames[mode]) << ":" << line);
	}
	CHECK(true);
}

TEST_CASE("MEASURE: echo density along each kind's impulse response (Abel-Huang NED; 1 = as dense as noise) at 44.1 kHz" * doctest::skip()) {
	for (int mode = 0; mode < fv::kModes; ++mode) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.decay = 0.5f;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(44100.0);
		const size_t total = 44100 * 4;
		std::vector<float> l(total, 0.0f), r(total, 0.0f);
		l[100] = r[100] = 0.5f; // one click
		for (size_t at = 0; at < total; at += 512) {
			dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, total - at)));
		}
		// NED over 20 ms windows: the share of samples beyond one standard deviation, over a Gaussian's (0.3173).
		std::string line;
		for (const double t0 : { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0 }) {
			const auto at = static_cast<size_t>(t0 * 44100.0);
			const size_t w = 882;
			double m2 = 0;
			for (size_t i = at; i < at + w; ++i) {
				m2 += static_cast<double>(l[i]) * l[i];
			}
			const double sd = std::sqrt(m2 / static_cast<double>(w));
			size_t beyond = 0;
			for (size_t i = at; i < at + w; ++i) {
				beyond += std::fabs(l[i]) > sd ? 1 : 0;
			}
			const double ned = sd > 0 ? static_cast<double>(beyond) / static_cast<double>(w) / 0.3173 : 0.0;
			char buf[32];
			std::snprintf(buf, sizeof buf, " %.2fs:%.2f", t0, ned);
			line += buf;
		}
		MESSAGE(std::string(fv::kSizeModeNames[mode]) << " NED:" << line);
	}
	CHECK(true);
}

TEST_CASE("MEASURE: the cost of each 512-frame block along a decaying tail (44.1 kHz) — does it climb as the tail dies?" * doctest::skip()) {
	for (int mode = 0; mode < fv::kModes; ++mode) {
		fv::Settings s;
		s.mix = 1.0f;
		s.sizeMode = mode;
		s.decay = 0.5f;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(44100.0);
		const size_t total = 44100 * 20;
		std::vector<float> l(512), r(512);
		std::vector<double> us;
		for (size_t at = 0; at < total; at += 512) {
			for (size_t k = 0; k < 512; ++k) {
				const double t = static_cast<double>(at + k) / 44100.0;
				const double u = std::fmod(t, 0.5);
				l[k] = r[k] = static_cast<float>(t < 2.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
			}
			const auto t0 = std::chrono::steady_clock::now();
			dev.processStereo(l.data(), r.data(), 512);
			us.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
		}
		const auto seg = [&](double a, double b) {
			const auto i0 = static_cast<size_t>(a * 44100.0 / 512.0);
			const auto i1 = std::min(us.size(), static_cast<size_t>(b * 44100.0 / 512.0));
			std::vector<double> v(us.begin() + static_cast<long>(i0), us.begin() + static_cast<long>(i1));
			std::sort(v.begin(), v.end());
			return std::pair<double, double> { v[v.size() / 2], v.back() };
		};
		const auto a = seg(0.5, 2.0), b = seg(4.0, 8.0), c = seg(10.0, 14.0), d = seg(16.0, 20.0);
		MESSAGE(std::string(fv::kSizeModeNames[mode]) << " median/max us per block (11.6 ms budget): playing " << a.first << "/"
		                                               << a.second << ", 4-8 s " << b.first << "/" << b.second << ", 10-14 s "
		                                               << c.first << "/" << c.second << ", 16-20 s " << d.first << "/" << d.second);
	}
	CHECK(true);
}

namespace {

/// Aurora's wet for 800 Hz plucks at 44.1 kHz, the host handing it `block` frames at a time.
std::vector<float> auroraAtBlock(int block, float character) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = fv::kAuroraMenu;
	s.decay = 0.5f;
	s.character = character;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(44100.0);
	const size_t total = 44100 * 5;
	std::vector<float> l(total), r(total);
	for (size_t i = 0; i < total; ++i) {
		const double t = static_cast<double>(i) / 44100.0;
		const double u = std::fmod(t, 0.5);
		l[i] = r[i] = static_cast<float>(t < 2.0 && u < 0.1 ? 0.5 * std::exp(-u * 20.0) * std::sin(6.283185307179586 * 800.0 * t) : 0.0);
	}
	for (size_t at = 0; at < total; at += static_cast<size_t>(block)) {
		dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(static_cast<size_t>(block), total - at)));
	}
	return l;
}

} // namespace

TEST_CASE("MEASURE: Aurora against the host's block size — the difference between 512-frame and 32-frame calls, by band" * doctest::skip()) {
	for (const float ch : { 0.0f, 1.3f }) {
		const auto a = auroraAtBlock(512, ch);
		const auto b = auroraAtBlock(32, ch);
		std::vector<double> diff(a.size()), ref(a.size());
		for (size_t i = 0; i < a.size(); ++i) {
			diff[i] = static_cast<double>(a[i]) - b[i];
			ref[i] = b[i];
		}
		const std::vector<double> d(diff.begin() + 44100, diff.end()), x(ref.begin() + 44100, ref.end());
		const auto pd = powerSpectrum(d, 8192);
		const auto px = powerSpectrum(x, 8192);
		double all = 0, dLo = 0, dHi = 0;
		for (size_t k = 1; k < px.size(); ++k) {
			const double f = static_cast<double>(k) * 44100.0 / 8192.0;
			all += px[k];
			(f < 4000.0 ? dLo : dHi) += pd[k];
		}
		MESSAGE("Aurora char " << ch << ": 512 vs 32 frames differ by " << 10 * std::log10(dLo / all + 1e-30) << " dB under 4 kHz, "
		                       << 10 * std::log10(dHi / all + 1e-30) << " dB above (relative to the wet)");
	}
	CHECK(true);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG LD — Featherverb had tiny glitches when fed louder (not clipping) signals (2026-10-04).    //
// Every level-dependent site, counted (`Core::levelEventsForTest`, `clipsForTest`,       //
// `Featherverb::wetLimiterEventsForTest`), against material at legal levels, and the output read for  //
// discontinuities beside them.                                                                       //
//--------------------------------------------------------------------------------------------------//
namespace {

enum class Loud { Sine100, Sine1k, Sine5k, Pink, Drums };
constexpr const char* kLoudNames[] = { "sine100", "sine1k", "sine5k", "pink", "drums" };

/// THE MATERIAL, PEAKING AT `peak` (linear): driven for the first `driven` frames, then silence for the tail. Sines fade in
/// and out over 5 ms so the input itself has no corner; pink noise is Kellet's filter on a fixed white stream; the drums
/// are a pitched kick and a noise snare in turn, four hits a second, normalised so the loudest sample is `peak`.
std::vector<float> loudInput(Loud m, double peak, size_t total, size_t driven) {
	constexpr double sr = 48000.0;
	std::vector<double> x(total, 0.0);
	uint32_t seed = 0x2545F491u;
	const auto white = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return static_cast<double>(static_cast<int32_t>(seed)) / 2147483648.0;
	};
	if (m == Loud::Sine100 || m == Loud::Sine1k || m == Loud::Sine5k) {
		const double hz = m == Loud::Sine100 ? 100.0 : m == Loud::Sine1k ? 1000.0 : 5000.0;
		const size_t fade = 240;
		for (size_t i = 0; i < driven; ++i) {
			const double env = std::min({ 1.0, static_cast<double>(i) / fade, static_cast<double>(driven - i) / fade });
			x[i] = env * std::sin(6.283185307179586 * hz * static_cast<double>(i) / sr);
		}
	}
	else if (m == Loud::Pink) {
		double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
		for (size_t i = 0; i < driven; ++i) {
			const double w = white();
			b0 = 0.99886 * b0 + w * 0.0555179;
			b1 = 0.99332 * b1 + w * 0.0750759;
			b2 = 0.96900 * b2 + w * 0.1538520;
			b3 = 0.86650 * b3 + w * 0.3104856;
			b4 = 0.55000 * b4 - w * 0.5329522;
			b5 = -0.7616 * b5 - w * 0.0168980;
			x[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
			b6 = w * 0.115926;
		}
	}
	else {
		const size_t hit = static_cast<size_t>(sr / 4.0);
		for (size_t i = 0; i < driven; ++i) {
			const size_t n = i / hit;
			const double u = static_cast<double>(i % hit) / sr;
			if (n % 2 == 0) {
				const double phase = 6.283185307179586 * (50.0 * u + 120.0 * (1.0 - std::exp(-u * 30.0)) / 30.0);
				x[i] = std::exp(-u * 7.0) * std::sin(phase);
			}
			else {
				x[i] = std::exp(-u * 25.0) * white() + 0.5 * std::exp(-u * 15.0) * std::sin(6.283185307179586 * 190.0 * u);
			}
		}
	}
	double top = 1e-30;
	for (const double v : x) {
		top = std::max(top, std::fabs(v));
	}
	std::vector<float> y(total);
	for (size_t i = 0; i < total; ++i) {
		y[i] = static_cast<float>(x[i] * peak / top);
	}
	return y;
}

struct LoudRun {
	Core::LevelEvents core {};
	uint64_t c3 = 0;
	fv::Featherverb::WetLimiterEvents wet {};
	float wetPeak = 0.0f;
	std::vector<float> l, r;
};

/// ONE RENDER of `s` (Mix 1, so the output is the wet) at 48 kHz in 512-frame blocks.
LoudRun loudRender(fv::Settings s, const std::vector<float>& in, bool limiter, bool owlComp) {
	s.mix = 1.0f;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setWetLimiterForTest(limiter);
	dev.setOwlCompressorForTest(owlComp);
	LoudRun run;
	run.l = in;
	run.r = in;
	for (size_t at = 0; at < in.size(); at += 512) {
		dev.processStereo(run.l.data() + at, run.r.data() + at, static_cast<int>(std::min<size_t>(512, in.size() - at)));
	}
	run.core = dev.coreForTest().levelEventsForTest();
	run.c3 = dev.coreForTest().clipsForTest();
	run.wet = dev.wetLimiterEventsForTest();
	for (size_t i = 0; i < in.size(); ++i) {
		run.wetPeak = std::max({ run.wetPeak, std::fabs(run.l[i]), std::fabs(run.r[i]) });
	}
	return run;
}

/// CLICKS IN THE WHOLE OUTPUT (`clicksPerSecond`: a second difference past six times its local RMS). It is blind to
/// scale, so a linear reverb counts the same at -30 dBFS as at -1; what the loud run counts over the quiet one is a
/// corner the level put there. (A first draft subtracted the scaled quiet render instead: its own quantisation,
/// scaled up 30 dB, outran a 100 Hz sine's tiny second difference and "found" thousands of clicks in Mouse.)
double loudClicks(const LoudRun& run) { return clicksPerSecond(run.l, run.r, 0, run.l.size()); }

/// THE WHOLE SWEEP for one setting: every material, the four levels against a -30 dBFS reference, limiter on and off.
void loudSweep(const char* label, const fv::Settings& s, bool owlComp) {
	constexpr size_t kTotal = 48000 * 3;
	constexpr size_t kDriven = 48000 * 3 / 2;
	for (int m = 0; m < 5; ++m) {
		const Loud mat = static_cast<Loud>(m);
		const std::vector<float> levels = mat == Loud::Drums ? std::vector<float> { -1.0f } : std::vector<float> { -12.0f, -6.0f, -3.0f, -1.0f };
		const double refDb = -30.0;
		const double refClicks = loudClicks(loudRender(s, loudInput(mat, std::pow(10.0, refDb / 20.0), kTotal, kDriven), false, owlComp));
		for (const float db : levels) {
			const std::vector<float> in = loudInput(mat, std::pow(10.0, db / 20.0), kTotal, kDriven);
			const LoudRun on = loudRender(s, in, true, owlComp);
			const LoudRun off = loudRender(s, in, false, owlComp);
			char buf[320];
			std::snprintf(buf, sizeof buf,
			              "%-26s %-7s %5.0f dBFS | clamp %6llu sat %6llu wrap %4llu c3 %6llu | lim %6llu attack %5llu | wet peak %+6.1f dBFS (lim off %+6.1f) | clicks/s -30dB %6.1f, lim on %6.1f off %6.1f",
			              label, kLoudNames[m], static_cast<double>(db), static_cast<unsigned long long>(on.core.outClamp),
			              static_cast<unsigned long long>(on.core.outSat), static_cast<unsigned long long>(on.core.outWrap),
			              static_cast<unsigned long long>(on.c3), static_cast<unsigned long long>(on.wet.limited),
			              static_cast<unsigned long long>(on.wet.attack), 20.0 * std::log10(on.wetPeak + 1e-30),
			              20.0 * std::log10(off.wetPeak + 1e-30), refClicks, loudClicks(on), loudClicks(off));
			MESSAGE(buf);
		}
	}
}

} // namespace

TEST_CASE("MEASURE: LD — every level-dependent site against loud legal input, every kind, Oversample, Owl Comp, the conv modes" * doctest::skip()) {
	for (const bool longSet : { false, true }) {
		for (int mode = 0; mode < fv::kModes; ++mode) {
			for (const bool os : { false, true }) {
				fv::Settings s;
				s.sizeMode = mode;
				s.oversample = os;
				if (longSet) {
					s.room = 1.0f;
					s.size = 1.0f;
					s.decay = 1.0f;
				}
				const std::string label = std::string(fv::kSizeModeNames[mode]) + (os ? " 2x" : " 1x") + (longSet ? " LONG" : "");
				loudSweep(label.c_str(), s, true);
				if (mode == fv::kOwlMenu) {
					loudSweep((label + " comp-off").c_str(), s, false);
				}
			}
		}
	}
	for (int conv = 1; conv < static_cast<int>(std::size(fv::kConvModeNames)); ++conv) {
		for (const int mode : { 4, 5 }) {
			fv::Settings s;
			s.sizeMode = mode;
			s.convMode = conv;
			const std::string label = std::string(fv::kSizeModeNames[mode]) + " conv " + fv::kConvModeNames[conv];
			loudSweep(label.c_str(), s, true);
		}
	}
	CHECK(true);
}

TEST_CASE("MEASURE: LD — what the wet limiter does to the waveform: the gain's per-sample step, and flat tops at the ceiling" * doctest::skip()) {
	// Run the same render with the limiter on and off: where the wet is loud, on/off IS the limiter's gain, so its step
	// from one sample to the next is read directly. An instant attack sets the envelope to this very sample's peak, so the
	// sample leaves at exactly the ceiling: a run of such samples is a flat top, which is a clip.
	constexpr size_t kTotal = 48000 * 3;
	for (const int mode : { 4, 5, fv::kVastMenu, fv::kOwlMenu }) {
		for (const Loud mat : { Loud::Sine100, Loud::Sine1k, Loud::Drums }) {
			for (const bool os : { false, true }) {
				fv::Settings s;
				s.sizeMode = mode;
				s.oversample = os;
				const std::vector<float> in = loudInput(mat, std::pow(10.0, -1.0 / 20.0), kTotal, kTotal / 2);
				const LoudRun on = loudRender(s, in, true, true);
				const LoudRun off = loudRender(s, in, false, true);
				double worstStepDb = 0.0;
				double prevG = 1.0;
				size_t atCeiling = 0, run = 0, longest = 0;
				for (size_t i = 0; i < kTotal; ++i) {
					const double o = off.l[i];
					// CONSECUTIVE samples only: a ratio across a quiet gap is two gains a while apart, not a step.
					if (std::fabs(o) > 0.25) {
						const double g = static_cast<double>(on.l[i]) / o;
						if (prevG > 0.0) {
							worstStepDb = std::max(worstStepDb, 20.0 * std::log10(prevG / std::max(g, 1e-9)));
						}
						prevG = g;
					}
					else {
						prevG = -1.0;
					}
					const bool flat = std::fabs(on.l[i]) >= fv::kWetCeiling * 0.99999f;
					atCeiling += flat ? 1 : 0;
					run = flat ? run + 1 : 0;
					longest = std::max(longest, run);
				}
				// AND THE DEVICE AS A LISTENER HEARS IT: Mix at its default, so the OUTPUT is under 0 dBFS while the wet is limited.
				fv::Settings d = s;
				fv::Featherverb dev;
				dev.setTarget(d);
				dev.prepare(48000.0);
				std::vector<float> l = in, r = in;
				for (size_t at = 0; at < kTotal; at += 512) {
					dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, kTotal - at)));
				}
				float outPeak = 0.0f;
				for (size_t i = 0; i < kTotal; ++i) {
					outPeak = std::max({ outPeak, std::fabs(l[i]), std::fabs(r[i]) });
				}
				MESSAGE(std::string(fv::kSizeModeNames[mode]) << (os ? " 2x " : " 1x ") << std::string(kLoudNames[static_cast<int>(mat)])
				                                 << " -1 dBFS: attack frames " << on.wet.attack << ", samples AT the ceiling "
				                                 << atCeiling << " (longest flat run " << longest << "), worst one-sample gain drop "
				                                 << worstStepDb << " dB; at Mix " << d.mix << " the output peaks "
				                                 << 20.0 * std::log10(outPeak + 1e-30) << " dBFS, limited frames "
				                                 << dev.wetLimiterEventsForTest().limited);
			}
		}
	}
	CHECK(true);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG LD, THE FIX: the wet limiter looks ahead (option (a)).                                 //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("Featherverb's wet limiter is the lookahead limiter at 1.5 ms, linked, 0 dBFS, 100 ms — at every rate") {
	// The block's own behaviour is `test_lookahead_limiter`'s; this is the device's USE of it: the settings, at three
	// rates, and the same lookahead whatever the switches (it is the wet's delay against the dry, so it never moves).
	for (const auto& [rate, want] : { std::pair { 44100.0, size_t { 66 } }, std::pair { 48000.0, size_t { 72 } },
	                                  std::pair { 96000.0, size_t { 144 } } }) {
		CAPTURE(rate);
		for (const bool os : { false, true }) {
			for (const int mode : { 4, fv::kAuroraMenu, fv::kOwlMenu }) {
				fv::Settings s;
				s.sizeMode = mode;
				s.oversample = os;
				s.convMode = mode == fv::kOwlMenu ? fv::kConvOnly : fv::kConvOff;
				fv::Featherverb dev;
				dev.setTarget(s);
				dev.prepare(rate);
				std::vector<float> l(4096, 0.1f), r(4096, 0.1f);
				dev.processStereo(l.data(), r.data(), 4096);
				const auto& lim = dev.wetLimiterForTest();
				CHECK(lim.lookahead() == want);
				CHECK(lim.params().ceiling == fv::kWetCeiling);
				CHECK(lim.params().releaseSeconds == fv::kWetLimiterReleaseSeconds);
				CHECK(lim.params().link);
			}
		}
	}
}

namespace {

struct WorstCase {
	const char* name;
	fv::Settings s;
};

/// THE WORST OF THE SWEEP (`MEASURE: LD — every level-dependent site`): the wet furthest over the ceiling, and the ones
/// whose clicks the instant attack made, at -1 dBFS.
std::vector<WorstCase> worstCases() {
	fv::Settings sky;
	sky.sizeMode = 5;
	fv::Settings featherLong;
	featherLong.sizeMode = 4;
	featherLong.oversample = true;
	featherLong.room = 1.0f;
	featherLong.size = 1.0f;
	featherLong.decay = 1.0f;
	fv::Settings convOnly;
	convOnly.sizeMode = 4;
	convOnly.convMode = fv::kConvOnly;
	return { { "Sky 1x", sky }, { "Feather 2x LONG", featherLong }, { "Feather Conv Only", convOnly } };
}

} // namespace

TEST_CASE("Featherverb's wet limiter at -1 dBFS in the worst settings: never above the ceiling, the gain a ramp, no clicks") {
	// (i) NO SAMPLE ABOVE THE CEILING, and the gain a RAMP, both derived in `dsp/lookahead_limiter.h`:
	// - its STEP per sample at most 1/boxA: the first box moves by (new - oldest) / boxA, each in (0, 1], and the second
	//   only averages those moves. At 48 kHz boxA is 37, so 0.0270;
	// - its SLOPE CHANGE per sample at most 2/(boxA·boxB): the second box's step is the difference of two of the first's
	//   steps over boxB. At 48 kHz, 2/(37·37) = 0.00146.
	// THE SECOND BOUND IS THE ONE THE INSTANT ATTACK BREAKS. MEASURED with it put back: its step per sample stays under
	// the first bound (0.006-0.012 here — it is not a big step, it is a step that follows the waveform), but its gain is
	// ceiling/|x| on every rising sample, so its slope turns as fast as the waveform does: a flat top.
	// THE GAIN IS READ AS on / off — the same render with the limiter's gain lifted, on the same timing (the lookahead
	// delay stays) — at consecutive samples where the wet is loud enough for the ratio to be exact to float rounding.
	// (ii) THE CLICK DETECTOR (`clicksPerSecond`) reads none: with the instant attack it read 160-280 a second here.
	for (const WorstCase& c : worstCases()) {
		CAPTURE(std::string(c.name));
		const std::vector<float> in = loudInput(Loud::Sine100, std::pow(10.0, -1.0 / 20.0), 48000 * 3, 48000 * 3 / 2);
		const LoudRun on = loudRender(c.s, in, true, true);
		const LoudRun off = loudRender(c.s, in, false, true);
		feathers::dsp::LookaheadLimiter shape;
		shape.prepare(48000.0, 72);
		shape.setParams({ .ceiling = fv::kWetCeiling, .lookahead = 72, .releaseSeconds = fv::kWetLimiterReleaseSeconds, .link = true });
		const double bound = 1.0 / static_cast<double>(shape.boxA());
		const double bendBound = 2.0 / static_cast<double>(shape.boxA() * shape.boxB());
		REQUIRE(shape.boxA() == 37);
		REQUIRE(shape.boxB() == 37);
		float peakOff = 0.0f;
		double worstStep = 0.0;
		double worstBend = 0.0;
		double prevG = -1.0;
		double prevStep = 0.0;
		bool havePrevStep = false;
		for (size_t i = 0; i < in.size(); ++i) {
			peakOff = std::max({ peakOff, std::fabs(off.l[i]), std::fabs(off.r[i]) });
			const double o = off.l[i];
			if (std::fabs(o) > 0.25) {
				const double g = static_cast<double>(on.l[i]) / o;
				if (prevG >= 0.0) {
					const double step = g - prevG;
					worstStep = std::max(worstStep, std::fabs(step));
					if (havePrevStep) {
						worstBend = std::max(worstBend, std::fabs(step - prevStep));
					}
					prevStep = step;
					havePrevStep = true;
				}
				prevG = g;
			}
			else {
				prevG = -1.0;
				havePrevStep = false;
			}
		}
		CAPTURE(peakOff);
		CAPTURE(worstStep);
		CAPTURE(worstBend);
		MESSAGE(std::string(c.name) << ": wet unlimited peaks " << 20.0 * std::log10(peakOff) << " dBFS; gain step at most " << worstStep
		               << " (bound " << bound << "), slope change at most " << worstBend << " (bound " << bendBound << ")");
		REQUIRE(peakOff > 1.5f * fv::kWetCeiling);    // the premise: the wet runs well over the ceiling here
		CHECK(on.wet.limited > 10000);                 // and the limiter acted
		CHECK(on.wetPeak <= fv::kWetCeiling);          // (i) never above it
		CHECK(on.wetPeak > 0.9f * fv::kWetCeiling);    // not vacuously far under it
		CHECK(worstStep <= bound + 1.0e-5);            // (i) a ramp, not a step
		CHECK(worstBend <= bendBound + 1.0e-5);        // (i) and a ramp whose slope turns slowly: no flat top
		CHECK(clicksPerSecond(on.l, on.r, 0, on.l.size()) == 0.0);  // (ii)
		CHECK(clicksPerSecond(off.l, off.r, 0, off.l.size()) == 0.0); // the detector's own floor on this material
	}
}

TEST_CASE("MEASURE: LD — the lookahead limiter's cost per sample against the whole Featherverb, idle and limiting" * doctest::skip()) {
	// A SHARED HOST (memory: benchmarking-on-a-shared-host): the device and the limiter are timed ALTERNATELY, five rounds,
	// and the best of each kept. The limiter runs on the device's own wet (Mix 1, limiter off, so it arrives unlimited).
	constexpr size_t kTotal = 512 * 940; // whole blocks: a short last block read past the buffer in the first draft
	for (const auto& [label, mode, db] : { std::tuple { "Feather -20 dBFS (idle)", 4, -20.0 },
	                                      std::tuple { "Sky -1 dBFS (limiting)", 5, -1.0 } }) {
		fv::Settings s;
		s.sizeMode = mode;
		s.mix = 1.0f;
		const std::vector<float> in = loudInput(Loud::Sine100, std::pow(10.0, db / 20.0), kTotal, kTotal);
		double bestDev = 1e9, bestLim = 1e9;
		uint64_t limited = 0;
		for (int round = 0; round < 5; ++round) {
			fv::Featherverb dev;
			dev.setTarget(s);
			dev.prepare(48000.0);
			dev.setWetLimiterForTest(false);
			std::vector<float> l = in, r = in;
			const auto t0 = std::chrono::steady_clock::now();
			for (size_t at = 0; at < kTotal; at += 512) {
				dev.processStereo(l.data() + at, r.data() + at, 512);
			}
			bestDev = std::min(bestDev, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
			feathers::dsp::LookaheadLimiter lim;
			lim.prepare(48000.0, 72);
			lim.setParams({ .ceiling = fv::kWetCeiling, .lookahead = 72, .releaseSeconds = fv::kWetLimiterReleaseSeconds, .link = true });
			const auto t1 = std::chrono::steady_clock::now();
			for (size_t at = 0; at < kTotal; at += 512) {
				lim.process(l.data() + at, r.data() + at, 512);
			}
			bestLim = std::min(bestLim, std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count());
			limited = lim.eventsForTest().limited;
		}
		MESSAGE(label << ": Featherverb " << bestDev / kTotal * 1e9 << " ns/frame, the limiter " << bestLim / kTotal * 1e9
		              << " ns/frame (" << 100.0 * bestLim / bestDev << "% of the device), frames limited " << limited << " of " << kTotal);
	}
	CHECK(true);
}


//--------------------------------------------------------------------------------------------------//
// BACKLOG JI: THE CORE'S LEVEL AT EVERY RATE (the principled fix: scale what the port left as a      //
// constant)                                                                                          //
//--------------------------------------------------------------------------------------------------//

namespace {

struct WetProbe {
	double hz = 3000.0;
	double amp = 0.5;     ///< the pluck's peak (0.5 = -6 dBFS)
	bool limiter = true;  ///< the wet's 0 dBFS limiter, as shipped
	bool oversample = false;
	double seconds = 3.0;
};
struct WetLevel {
	double rmsDb;  ///< the whole wet's RMS after the first 0.1 s, both channels
	double toneDb; ///< the tone's own energy at exactly `hz` (JI's measure), left channel
};

/// The device's wet (Mix 1) for sine plucks of `p.hz` at `p.amp`, 0.1 s in every 0.5 s, at `sr`.
WetLevel wetLevel(int mode, double sr, const WetProbe& p = {}) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = mode;
	s.oversample = p.oversample;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(sr);
	dev.setWetLimiterForTest(p.limiter);
	const auto total = static_cast<size_t>(p.seconds * sr);
	std::vector<float> l(total), r(total);
	for (size_t i = 0; i < total; ++i) {
		const double t = static_cast<double>(i) / sr;
		const double env = std::fmod(t, 0.5) < 0.1 ? p.amp : 0.0;
		l[i] = r[i] = static_cast<float>(env * std::sin(6.283185307179586 * p.hz * t));
	}
	for (size_t at = 0; at < total; at += 512) {
		dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, total - at)));
	}
	double e = 0.0, re = 0.0, im = 0.0;
	const auto from = static_cast<size_t>(0.1 * sr);
	for (size_t i = from; i < total; ++i) {
		e += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
		const double ph = 6.283185307179586 * p.hz * static_cast<double>(i) / sr;
		re += l[i] * std::cos(ph);
		im += l[i] * std::sin(ph);
	}
	const auto n = static_cast<double>(total - from);
	return {10.0 * std::log10(e / (2.0 * n) + 1e-30), 10.0 * std::log10((re * re + im * im) / (n * n) + 1e-30)};
}

} // namespace

TEST_CASE("MEASURE: JI — every kind's wet level at 44.1, 48, 88.2, 96 and 192 kHz and 2x, against 48 kHz" * doctest::skip()) {
	// A MEASUREMENT TOOL, NOT A PENDING FIX: the instrument behind JI's closing numbers (CLOSED, ACCEPTED on
	// 2026-10-10, no DSP change). Run it with --no-skip to read the table; the guard on the part that holds is the case
	// after it.
	for (const WetProbe p : { WetProbe {}, WetProbe {.hz = 3000.0, .amp = 0.1, .limiter = false}, WetProbe {.hz = 800.0, .amp = 0.5}, WetProbe {.hz = 800.0, .amp = 0.1, .limiter = false}, WetProbe {.hz = 200.0, .amp = 0.1, .limiter = false} }) {
		MESSAGE("plucks of " << p.hz << " Hz at " << 20.0 * std::log10(p.amp) << " dBFS, limiter " << p.limiter
		                     << ": rms / tone, dB against 48 kHz");
		for (int mode = 0; mode < fv::kModes; ++mode) {
			const WetLevel ref = wetLevel(mode, 48000.0, p);
			char line[512];
			int at = std::snprintf(line, sizeof line, "%-8s 48k %6.1f/%6.1f", fv::kSizeModeNames[mode], ref.rmsDb, ref.toneDb);
			for (const double sr : { 44100.0, 88200.0, 96000.0, 192000.0 }) {
				const WetLevel w = wetLevel(mode, sr, p);
				at += std::snprintf(line + at, sizeof line - at, "  %6.0f %+5.2f/%+5.2f", sr, w.rmsDb - ref.rmsDb, w.toneDb - ref.toneDb);
			}
			WetProbe os = p;
			os.oversample = true;
			const WetLevel w = wetLevel(mode, 48000.0, os);
			std::snprintf(line + at, sizeof line - at, "  48k2x %+5.2f/%+5.2f", w.rmsDb - ref.rmsDb, w.toneDb - ref.toneDb);
			MESSAGE(line);
		}
	}
	CHECK(true);
}

TEST_CASE("Featherverb's wet level does not move with the sample rate below the highs: every kind, 200 and 800 Hz, 44.1 to 192 kHz") {
	// JI, ACCEPTED: the per-buffer steps are already times (the buffer grid scales with the rate),
	// and what the rate moves is the highs, through the core's fixed 2x undersampler. This guards the part that holds:
	// below them, the whole wet's RMS at every rate is the 48 kHz one. MEASURED (2026-10-10): within 0.25 dB at 200 Hz
	// in every kind but Aurora at 192 kHz (0.69), and within 0.42 dB at 800 Hz.
	for (const double hz : { 200.0, 800.0 }) {
		const WetProbe p {.hz = hz, .amp = 0.1, .limiter = false};
		for (int mode = 0; mode < fv::kModes; ++mode) {
			const double ref = wetLevel(mode, 48000.0, p).rmsDb;
			for (const double sr : { 44100.0, 88200.0, 96000.0, 192000.0 }) {
				const double moved = wetLevel(mode, sr, p).rmsDb - ref;
				CAPTURE(hz);
				CAPTURE(std::string(fv::kSizeModeNames[mode]));
				CAPTURE(sr);
				CHECK(std::fabs(moved) < 0.75);
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG LA: INPUT GAIN AND THE INPUT LEVEL METER                                               //
//--------------------------------------------------------------------------------------------------//

namespace {

struct InputRun {
	std::vector<float> l, r;
	float inPeak, wetPeak, gr, gain;
	std::vector<float> published;
};

/// The device at 48 kHz, Mix 1 (so the output is the wet), on 800 Hz sine plucks of `amp`, Input at `db`.
InputRun inputRun(float db, float amp, int mode = 4, bool limiter = true, double seconds = 2.05) {
	fv::Settings s;
	s.mix = 1.0f;
	s.sizeMode = mode;
	s.inputGainDb = db;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	dev.setWetLimiterForTest(limiter);
	InputRun out {};
	feathers::device::ReadingsSink sink;
	sink.context = &out.published;
	sink.publish = [](void* c, const float* v, size_t n) { static_cast<std::vector<float>*>(c)->assign(v, v + n); };
	dev.setReadingsSink(sink);
	const auto total = static_cast<size_t>(seconds * 48000.0);
	out.l.resize(total);
	out.r.resize(total);
	for (size_t i = 0; i < total; ++i) {
		const double t = static_cast<double>(i) / 48000.0;
		const double env = std::fmod(t, 0.5) < 0.1 ? amp : 0.0;
		out.l[i] = out.r[i] = static_cast<float>(env * std::sin(6.283185307179586 * 800.0 * t));
	}
	// 2.05 s: the last call ends 50 ms into a pluck, so the held peaks are the pluck's.
	for (size_t at = 0; at < total; at += 512) {
		dev.processStereo(out.l.data() + at, out.r.data() + at, static_cast<int>(std::min<size_t>(512, total - at)));
	}
	out.inPeak = dev.readingForTest(fv::Reading::InPeak);
	out.wetPeak = dev.readingForTest(fv::Reading::WetPeak);
	out.gr = dev.readingForTest(fv::Reading::LimiterGr);
	out.gain = dev.inputGainForTest();
	return out;
}

double rmsDb(const std::vector<float>& x, size_t from) {
	double e = 0.0;
	for (size_t i = from; i < x.size(); ++i) {
		e += static_cast<double>(x[i]) * x[i];
	}
	return 10.0 * std::log10(e / static_cast<double>(x.size() - from) + 1e-30);
}

} // namespace

TEST_CASE("Featherverb's Input is a new modulatable dB control, -24..+24, default 0, on the card's face") {
	const auto& d = fv::kParamDescs[static_cast<size_t>(fv::Param::InputGain)];
	CHECK(std::string(d.key) == "featherverb_input_gain");
	CHECK(d.unit == feathers::device::Unit::Decibel);
	CHECK(d.min == -24.0);
	CHECK(d.max == 24.0);
	CHECK(d.def == 0.0);
	CHECK(d.modulatable);
	CHECK(fv::Settings {}.inputGainDb == 0.0f);
	CHECK(std::find(std::begin(fv::kFaceOrder), std::end(fv::kFaceOrder), static_cast<uint8_t>(fv::Param::InputGain))
	      != std::end(fv::kFaceOrder));
	// THE CODEC CARRIES IT.
	fv::State st;
	st.settings.inputGainDb = -7.5f;
	CHECK(fv::stateOf(fv::rowOf(st), fv::Row {}.size()).settings.inputGainDb == -7.5f);
}

TEST_CASE("Featherverb's Input moves what enters the reverb by exactly its dB, and the meter reads it after the gain") {
	// 800 Hz plucks at -20 dBFS (0.1), Feather, the limiter off so the wet is the core's alone (linear below its ceiling).
	const InputRun at0 = inputRun(0.0f, 0.1f, 4, false);
	CHECK(at0.gain == 1.0f);
	for (const float db : { -24.0f, -12.0f, -6.0f, 6.0f, 12.0f }) {
		CAPTURE(db);
		const InputRun g = inputRun(db, 0.1f, 4, false);
		// THE VALUE: the gain applied, and the meter's input peak, which is the mid of the input at that gain.
		const double want = std::pow(10.0, static_cast<double>(db) / 20.0);
		CHECK(g.gain == doctest::Approx(want).epsilon(1e-6));
		CHECK(g.inPeak == doctest::Approx(at0.inPeak * want).epsilon(1e-4));
		CHECK(at0.inPeak == doctest::Approx(0.1).epsilon(1e-3)); // a 0.1 sine's peak, L = R so the mid is the input
		// AND WHAT IT DOES: the wet moves by the same dB (MEASURED within 0.00001 dB), the
		// meter's wet peak with it.
		const double moved = rmsDb(g.l, 4800) - rmsDb(at0.l, 4800);
		MESSAGE("Input " << db << " dB: the wet moved " << moved << " dB, the meter's input peak " << g.inPeak);
		CHECK(std::fabs(moved - db) < 0.01);
		CHECK(20.0 * std::log10(g.wetPeak / at0.wetPeak) == doctest::Approx(db).epsilon(0.02));
	}
	// THE DRY IS UNTOUCHED: Mix 0 is the input, at any Input.
	fv::Settings s;
	s.mix = 0.0f;
	s.inputGainDb = 18.0f;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	std::vector<float> l(4096), r(4096);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = r[i] = 0.3f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
	}
	const std::vector<float> x = l;
	dev.processStereo(l.data(), r.data(), 4096);
	CHECK(l == x);
}

TEST_CASE("Featherverb's meter: the wet's peak after its limiter, the limiter's reduction, all published in order") {
	// QUIET: 800 Hz at -20 dBFS into Feather — the limiter idle, GR 0.
	const InputRun quiet = inputRun(0.0f, 0.1f);
	CHECK(quiet.gr == 0.0f);
	CHECK(quiet.wetPeak > 0.0f);
	REQUIRE(quiet.published.size() == static_cast<size_t>(fv::Reading::kCount));
	CHECK(quiet.published[0] == quiet.inPeak);
	CHECK(quiet.published[1] == quiet.wetPeak);
	CHECK(quiet.published[2] == quiet.gr);
	// THE WET PEAK IS THE OUTPUT'S at Mix 1, over the last hold window (50 ms).
	float outPeak = 0.0f;
	for (size_t i = quiet.l.size() - 2400; i < quiet.l.size(); ++i) {
		outPeak = std::max({outPeak, std::fabs(quiet.l[i]), std::fabs(quiet.r[i])});
	}
	CHECK(quiet.wetPeak >= outPeak);
	// DRIVEN: Sky at -1 dBFS with Input +12 — its wet runs far over 0 dBFS, so the limiter works and says how hard,
	// and the wet it lets out never passes 0 dBFS.
	const InputRun hot = inputRun(12.0f, 0.89f, 5);
	MESSAGE("Sky, -1 dBFS, Input +12: GR " << hot.gr << " dB, wet peak " << hot.wetPeak << "; quiet Feather: GR " << quiet.gr);
	CHECK(hot.gr > 6.0f);
	CHECK(hot.wetPeak <= fv::kWetCeiling);
	CHECK(hot.wetPeak > 0.5f);
}
