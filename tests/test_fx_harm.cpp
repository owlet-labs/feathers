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

#include "fx/harm/harm.h"
#include "fx/harm/harm_types.h"

#include <array>
#include <cmath>
#include <complex>
#include <algorithm>
#include <new>
#include <numbers>
#include <string>
#include <vector>

using namespace feathers::fx::harm;

TEST_CASE("the notch follows the oscillator, which is what makes HARM one device") {
	// THE COUPLING IS THE WHOLE ARGUMENT FOR THE DEVICE EXISTING. HARM is its own device by design,
	// rather than a filter and an oscillator you assemble, and the clinching reason was that its two
	// halves know about each other: the notch centre tracks the sub-oscillator's harmonic. A
	// hand-assembled rack could not express that without routing this plugin does not have.
	//
	// So this is the case that would fail if someone "simplified" the notch to track the note.
	//
	// NO Enabled HERE, deliberately: notchCentreHz and oscillatorHz are ARITHMETIC over the settings
	// and never look at the switch. Setting it would suggest these numbers depend on it, and the day
	// somebody makes them depend on it this case should be the one that notices.
	Settings s;
	s.noteSemitones = 69.0f; // A4
	s.level = 0.5f;          // the oscillator is sounding

	SUBCASE("at unison the notch sits on the fundamental") {
		s.harmonic = kUnisonHarmonic;
		CHECK(oscillatorHz(s) == doctest::Approx(440.0));
		CHECK(notchCentreHz(s) == doctest::Approx(440.0));
	}

	SUBCASE("move the harmonic and the notch moves with it") {
		// A fifth above: the table's 3/2 entry.
		size_t fifth = kHarmonicCount;
		for (size_t i = 0; i < kHarmonicCount; ++i) {
			if (kHarmonics[i].num == 3 && kHarmonics[i].den == 2) {
				fifth = i;
				break;
			}
		}
		REQUIRE(fifth < kHarmonicCount);
		s.harmonic = fifth;
		// THE VALUE, not "it changed" — 440 x 3/2.
		CHECK(oscillatorHz(s) == doctest::Approx(660.0));
		CHECK(notchCentreHz(s) == doctest::Approx(660.0));
		// AND THEY ARE THE SAME NUMBER, which is the coupling stated directly rather than inferred
		// from two separate expectations that happen to agree.
		CHECK(notchCentreHz(s) == doctest::Approx(oscillatorHz(s)));
	}

	SUBCASE("with the oscillator silent the notch STAYS on the harmonic — parity with the firmware, whose "
	        "notch never reads Level") {
		// This used to assert a fall-back to the fundamental at Level 0, citing the firmware's
		// `isOscEnabled() ? ratio : 1.0f`. That test is on the harmonic SELECTOR having an off position,
		// which this device's control does not have; the firmware's notch never consults level. A hand
		// turning Level down to hear the strip alone gets the strip where the harmonic is (HARM parity
		// sweep, 2026-09-16).
		size_t twoOctaves = kHarmonicCount;
		for (size_t i = 0; i < kHarmonicCount; ++i) {
			if (kHarmonics[i].num == 4 && kHarmonics[i].den == 1) {
				twoOctaves = i;
				break;
			}
		}
		REQUIRE(twoOctaves < kHarmonicCount);
		s.harmonic = twoOctaves;
		s.level = 0.0f;
		CHECK(oscillatorHz(s) == doctest::Approx(1760.0));
		CHECK(notchCentreHz(s) == doctest::Approx(1760.0)); // the notch stays put
	}

	SUBCASE("fine tune and bend move both together") {
		s.harmonic = kUnisonHarmonic;
		s.fineSemitones = 12.0f;
		CHECK(oscillatorHz(s) == doctest::Approx(880.0));
		CHECK(notchCentreHz(s) == doctest::Approx(880.0));
		s.fineSemitones = 0.0f;
		s.bendSemitones = -12.0f;
		CHECK(notchCentreHz(s) == doctest::Approx(220.0));
	}
}

TEST_CASE("backlog EH: the notch forgets its state while Width is at zero, so Width coming back does not ring the tail it parked") {
	// A STAGE THAT KEEPS ITS STATE ACROSS A BYPASS REPLAYS IT ON THE WAY BACK IN — the bus utility's DC
	// stage names the precedent. The notch's biquad was cleared only by `reset`; Width to zero froze
	// it mid-signal and Width back up resumed a Q=30 filter from there. Measured by value: Width up,
	// loud; Width zero, loud; Width up again on SILENCE — nothing out, from the first sample.
	Harm harm;
	harm.prepare(48000.0);
	Settings s;
	s.noteSemitones = 69.0f;
	s.level = 0.0f; // no oscillator: whatever comes out is the filter's own
	const auto run = [&](float width, float amp) {
		s.width = width;
		harm.setTarget(s);
		std::vector<float> l(4800), r(4800);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = r[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0));
		}
		for (int at = 0; at < 4800; at += 64) {
			harm.processStereo(l.data() + at, r.data() + at, 64);
		}
		float peak = 0.0f;
		for (size_t i = 0; i < 256; ++i) {
			peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
		}
		return peak;
	};
	REQUIRE(run(0.5f, 0.5f) > 0.0f);
	REQUIRE(run(0.0f, 0.5f) > 0.0f);
	const float ring = run(0.5f, 0.0f);
	CAPTURE(ring);
	CHECK(ring < 1e-6f);
}

TEST_CASE("the width control is width, whatever the firmware calls it") {
	// THE FIRMWARE NAMES THIS KNOB `hpf` AND ITS COMMENT CALLS IT A CUTOFF. It is neither: it is the
	// notch's WIDTH, and it is inverted — narrow at the bottom of the travel, wide at the top. A port
	// that trusted the name would have produced a plausible number doing the wrong job, which is why
	// the value is pinned here rather than left to the implementation.
	//
	// NO Enabled HERE either — notchQ is arithmetic over the width control, not a rendering.
	// WIDTH SET EXPLICITLY, NOT TAKEN FROM THE DEFAULT. This read `Settings s;` and relied on Width
	// defaulting to 0; the default moved to 0.3 and the case went red claiming the arithmetic was wrong.
	// THE CLAIM IS ABOUT THE MAPPING, so the input is stated — a default that moves must not be able
	// to move what a case means.
	Settings s;
	s.width = 0.0f;
	CHECK(notchQ(s) == doctest::Approx(30.0)); // width 0 — surgical

	s.width = 1.0f;
	CHECK(notchQ(s) == doctest::Approx(0.5)); // wide

	s.width = 0.5f;
	CHECK(notchQ(s) == doctest::Approx(15.25));

	// MONOTONE DOWNWARD across the whole travel, so "wider" never means "sharper" anywhere in it.
	double previous = 1e9;
	for (int i = 0; i <= 20; ++i) {
		s.width = static_cast<float>(i) / 20.0f;
		const double q = notchQ(s);
		CAPTURE(i);
		CHECK(q < previous);
		previous = q;
	}
}

TEST_CASE("HARM's row is its own indexed view, and the per-render fields stay out of it") {
	State state;
	state.settings.harmonic = 20;
	state.settings.level = 0.7f;
	state.settings.fineSemitones = -3.5f;
	state.settings.phase = 0.25f;
	state.settings.spread = 0.6f;
	state.settings.attackSeconds = 0.01f;
	state.settings.releaseSeconds = 0.5f;
	state.settings.width = 0.4f;
	state.settings.portaSeconds = 0.2f;
	state.poly = true;

	SUBCASE("a full row round-trips") {
		const State back = stateOf(rowOf(state), static_cast<size_t>(Param::kCount));
		CHECK(back.settings.harmonic == state.settings.harmonic);
		CHECK(back.settings.level == doctest::Approx(state.settings.level));
		CHECK(back.settings.fineSemitones == doctest::Approx(state.settings.fineSemitones));
		CHECK(back.settings.attackSeconds == doctest::Approx(state.settings.attackSeconds));
		CHECK(back.settings.releaseSeconds == doctest::Approx(state.settings.releaseSeconds));
		CHECK(back.settings.portaSeconds == doctest::Approx(state.settings.portaSeconds));
		CHECK(back.poly == state.poly);
	}

	SUBCASE("the note, the bend and the gate are NOT in the row") {
		// They are supplied per render by whoever runs HARM, exactly as the filter's
		// keyOffsetSemitones is. A row carrying them would claim to know something only the caller
		// knows; a patch storing them would restore a note nobody is playing.
		state.settings.noteSemitones = 42.0f;
		state.settings.bendSemitones = 5.0f;
		state.settings.gate = true;
		const State back = stateOf(rowOf(state), static_cast<size_t>(Param::kCount));
		CHECK(back.settings.noteSemitones == doctest::Approx(60.0f)); // the declared default
		CHECK(back.settings.bendSemitones == doctest::Approx(0.0f));
		CHECK_FALSE(back.settings.gate);
	}

	SUBCASE("a knob edit does not retune the device") {
		// setValueOf goes through the row, and the row does not carry the note — so without care a
		// single knob move would reset the device to middle C mid-render. Pinned because the bug
		// would be silent and intermittent.
		state.settings.noteSemitones = 48.0f;
		state.settings.gate = true;
		setValueOf(&state, Param::Level, 0.9f);
		CHECK(state.settings.level == doctest::Approx(0.9f));
		CHECK(state.settings.noteSemitones == doctest::Approx(48.0f));
		CHECK(state.settings.gate);
	}

	SUBCASE("every declared control reads back what it was set to, and stops at its ends") {
		for (size_t i = 0; i < std::size(kParamDescs); ++i) {
			const auto param = static_cast<Param>(i);
			const feathers::device::ParamDesc& d = kParamDescs[i];
			CAPTURE(i);
			CAPTURE(std::string(d.name));

			// THE SLOT'S FURNITURE (backlog OW): the row carries the defaults, the State holds nothing to read back.
			if (i == static_cast<size_t>(Param::Mix) || i == static_cast<size_t>(Param::WetGain)) {
				continue;
			}
			State s;
			const double mid = d.stepped ? std::round((d.min + d.max) * 0.5) : (d.min + d.max) * 0.5;
			setValueOf(&s, param, static_cast<float>(mid));
			CHECK(valueOf(s, param) == doctest::Approx(static_cast<float>(mid)).epsilon(1e-5));

			// PAST THE END STOPS AT THE END, not at the default.
			State hi;
			setValueOf(&hi, param, static_cast<float>(d.max) + 1000.0f);
			CHECK(valueOf(hi, param) == doctest::Approx(static_cast<float>(d.max)).epsilon(1e-5));
		}
	}

	SUBCASE("a short payload defaults the fields it does not reach") {
		// THE FIRST TWO CONTROLS ARE NOW Enabled AND Harmonic — `Enabled` was inserted at the front
		// when the level=0 convention was retired, which moved every other index by one. The claim
		// is unchanged and its subjects moved: what the payload REACHES is carried, what it does not
		// takes the DECLARED DEFAULT rather than zero.
		const State partial = stateOf(rowOf(state), 2); // Enabled and Harmonic only
		CHECK(partial.settings.enabled == state.settings.enabled);
		CHECK(partial.settings.harmonic == state.settings.harmonic);
		// LEVEL IS PAST THE END NOW, so it defaults — which is the rule this case exists for, and it
		// is a stronger example than the one it replaced because Level's default is not its set value.
		CHECK(partial.settings.level
		      == doctest::Approx(kParamDescs[static_cast<size_t>(Param::Level)].def));
		CHECK(partial.settings.width
		      == doctest::Approx(kParamDescs[static_cast<size_t>(Param::Width)].def));
		CHECK_FALSE(partial.poly);
	}
}

TEST_CASE("the harmonic table is the musical one it was ported for") {
	// The table IS the device on this control — chosen intervals rather than a linear sweep — so a
	// few landmarks are pinned. If someone regenerates it arithmetically these go red, which is the
	// point: that would be a different instrument wearing the same name.
	CHECK(kHarmonicCount == 102);
	CHECK(ratioAt(kUnisonHarmonic) == doctest::Approx(1.0));
	CHECK(kHarmonics[kUnisonHarmonic].num == 1);
	CHECK(kHarmonics[kUnisonHarmonic].den == 1);
	CHECK(ratioAt(0) == doctest::Approx(0.125));   // two octaves and a third down
	CHECK(ratioAt(kHarmonicCount - 1) == doctest::Approx(32.0));

	// MONOTONE INCREASING, which a hand-written table can quietly break.
	for (size_t i = 1; i < kHarmonicCount; ++i) {
		CAPTURE(i);
		CHECK(kHarmonics[i].ratio > kHarmonics[i - 1].ratio);
	}

	// AND OUT OF RANGE ANSWERS UNISON rather than reading past the end.
	CHECK(ratioAt(kHarmonicCount) == doctest::Approx(1.0));
	CHECK(ratioAt(9999) == doctest::Approx(1.0));
}

// ---------------------------------------------------------------------------------------------
// THE DSP.
//
// Every claim here is about a VALUE — an amplitude ratio, a frequency, a sample compared to the one
// the same input produced at a different block size. CLAUDE.md's rule, and HARM is exactly the
// device it was written for: its output carries a notch, an envelope and a sine at once, so "the
// buffer changed" separates almost nothing.

namespace {

constexpr double kRate = 48000.0;

/// Root-mean-square of a buffer, which is what "how much of this tone survived" actually means.
double rmsOf(const std::vector<float>& x, size_t from = 0) {
	double sum = 0.0;
	size_t n = 0;
	for (size_t i = from; i < x.size(); ++i) {
		sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
		++n;
	}
	return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

std::vector<float> sineAt(double hz, size_t frames, double amplitude = 1.0) {
	std::vector<float> out(frames);
	for (size_t i = 0; i < frames; ++i) {
		out[i] = static_cast<float>(
		    amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
	}
	return out;
}

/// HARM at a note, rendered over `frames`, in blocks of `block`.
struct Run {
	std::vector<float> left;
	std::vector<float> right;
	double currentHz = 0.0;
	double envelope = 0.0;
};

Run render(const Settings& settings, const std::vector<float>& input, int block) {
	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(settings);
	Run run;
	run.left = input;
	run.right = input;
	int at = 0;
	const int frames = static_cast<int>(input.size());
	while (at < frames) {
		const int take = std::min(block, frames - at);
		harm.processStereo(run.left.data() + at, run.right.data() + at, take);
		at += take;
	}
	run.currentHz = harm.currentHzForTest();
	run.envelope = harm.envelopeForTest();
	return run;
}

Settings notchOnly(double widthValue, size_t harmonic) {
	Settings s;
	// SWITCHED ON. HARM has an Enabled control now, off by default like every other effect — the
	// "level=0 means off" convention was retired as hardware-specific on 2026-08-24. A helper that
	// builds a device to MEASURE has to put it in the signal; the identity guarantees below are about
	// what an enabled device does with its controls down, which is a different claim entirely.
	s.enabled = true;
	s.level = 0.0f; // the oscillator silent, so what is measured is the notch alone
	s.width = static_cast<float>(widthValue);
	s.harmonic = harmonic;
	s.noteSemitones = 69.0f; // A4 = 440 Hz, so the arithmetic below is checkable by hand
	s.gate = true;
	return s;
}

} // namespace

TEST_CASE("an enabled HARM with its controls DOWN does not touch a sample") {
	// THE IDENTITY GUARANTEE, AND ITS NAME CHANGED WITH THE DEFAULTS RATHER THAN ITS MEANING.
	//
	// IT USED TO SAY "at its defaults" AND THAT WAS ALWAYS INCIDENTAL. What the case has always
	// tested is that Level 0 adds nothing and Width 0 removes nothing — the defaults merely happened
	// to be those numbers. The defaults are now Level 1.0 and Width 0.3, so a fresh HARM DOES colour its
	// input, deliberately.
	//
	// THE TRAP HERE, NAMED BECAUSE IT IS THE OBVIOUS FIX AND IT IS WRONG: re-pinning the REQUIREs to
	// the new defaults would leave a green case asserting that a device which notches its input is
	// transparent. **The guarantee is about the CONTROLS, so the controls are set explicitly.** A
	// default that moves must not be able to move what this case means.
	//
	// TO THE BIT, because "close enough" is what a notch at a very high Q also produces — and that is
	// exactly the wrong answer dressed as the right one.
	//
	// ENABLED, which is the stronger form: a switched-off HARM would satisfy this trivially through
	// the bypass and say nothing about Level or Width at all.
	const std::vector<float> input = sineAt(440.0, 512);
	Settings quiet;
	quiet.enabled = true;
	quiet.level = 0.0f; // nothing added
	quiet.width = 0.0f; // nothing removed
	quiet.noteSemitones = 69.0f;
	const Run run = render(quiet, input, 64);
	for (size_t i = 0; i < input.size(); ++i) {
		CAPTURE(i);
		REQUIRE(run.left[i] == input[i]);
		REQUIRE(run.right[i] == input[i]);
	}

	SUBCASE("and a HARM AT ITS DEFAULTS now does the opposite, which is the point of them") {
		// THE OTHER HALF OF THE DEFAULTS DECISION, PINNED. These numbers are set so that switching HARM on
		// does something — knobs that seemed not to respond were a device that turned every
		// control and made no sound. A defaults change that quietly reverted would put that back.
		Settings defaults;
		defaults.enabled = true;
		defaults.noteSemitones = 69.0f;
		CHECK(defaults.level == doctest::Approx(1.0f));
		CHECK(defaults.width == doctest::Approx(0.3f));

		const Run acted = render(defaults, input, 64);
		bool moved = false;
		for (size_t i = 0; i < input.size(); ++i) {
			moved = moved || acted.left[i] != input[i];
		}
		CHECK(moved);
		// AND THE NOTCH IS AT THE SELECTED HARMONIC RATHER THAN THE FUNDAMENTAL — `notchCentreHz`
		// falls back only while the oscillator is silent, and at Level 1 that branch never runs on a
		// fresh device. That is what makes the first PICTURE the one the ratio axis was designed for.
		CHECK(notchRatio(defaults)
		      == doctest::Approx(static_cast<double>(ratioAt(defaults.harmonic))).epsilon(1e-9));
	}
}

TEST_CASE("a switched-off HARM does not touch a sample, whatever its controls say") {
	// THE BYPASS, WHICH IS A DIFFERENT CLAIM FROM THE IDENTITY GUARANTEE ABOVE — and it needs its own
	// case because that one no longer covers it. "A HARM at its defaults does not touch a sample" now
	// ENABLES the device, so it rests entirely on Level 0 adding nothing and Width 0 removing
	// nothing; the switch is never exercised. A known-bad that deleted the bypass outright left it
	// green, which is how this hole was found.
	//
	// SO THIS IS THE OPPOSITE ARRANGEMENT: both halves turned UP, and the switch OFF. Under the
	// retired convention this was not even expressible — "off" WAS "both halves down".
	//
	// TO THE BIT, because a bypass that ran the notch at unity or the oscillator at zero level would
	// be arithmetically close and audibly identical, and would still be reading and writing every
	// sample of somebody's signal.
	const std::vector<float> input = sineAt(440.0, 512);
	Settings off;
	off.enabled = false;
	off.level = 1.0f;
	off.width = 1.0f;
	off.noteSemitones = 69.0f;
	off.gate = true;

	const Run run = render(off, input, 64);
	for (size_t i = 0; i < input.size(); ++i) {
		CAPTURE(i);
		REQUIRE(run.left[i] == input[i]);
		REQUIRE(run.right[i] == input[i]);
	}

	// NOT VACUOUS: the same settings with the switch ON must change the signal, or this case is
	// asserting that a device which never works does nothing.
	Settings on = off;
	on.enabled = true;
	const Run acting = render(on, input, 64);
	bool moved = false;
	for (size_t i = 0; i < input.size(); ++i) {
		moved = moved || acting.left[i] != input[i];
	}
	CHECK(moved);

	SUBCASE("and switching it off mid-phrase leaves nothing ringing or sounding") {
		// A DEVICE SWITCHED OUT MUST GO QUIET, not freeze. An envelope left open or a notch left
		// ringing would resume a note nobody is playing the moment it came back.
		Harm harm;
		harm.prepare(kRate);
		harm.setTarget(on);
		std::vector<float> block = sineAt(440.0, 256);
		harm.processStereo(block.data(), nullptr, static_cast<int>(block.size()));
		REQUIRE(harm.envelopeForTest() > 0.0);

		harm.setTarget(off);
		std::vector<float> after = input;
		harm.processStereo(after.data(), nullptr, static_cast<int>(after.size()));
		CHECK(harm.envelopeForTest() == doctest::Approx(0.0));
		for (size_t i = 0; i < after.size(); ++i) {
			CAPTURE(i);
			REQUIRE(after[i] == input[i]);
		}
	}
}

TEST_CASE("depth sets how much of the centre the notch removes, exactly and linearly") {
	// A DEPTH CONTROL DID NOT EXIST. And no partial cut COULD exist as the
	// notch was built: RBJ's form puts the zero pair exactly on the unit circle, so the centre was
	// removed by construction and Width only said how WIDE the cut was.
	//
	// THE REFERENCE IS DERIVED, NOT RUN. At the centre both numerator and denominator collapse to
	// the same form and everything but the two alphas cancels, so |H(w0)| = alphaZ/alpha = 1 - depth
	// — exactly, linearly, and independent of fc, Q and sample rate. Computing the expectation by
	// running the biquad would be the same code agreeing with itself.
	//
	// AND A DEPTH IS *PRECISELY* THE PARTIAL EFFECT CLAUDE.md WARNS ABOUT. A difference check passes
	// for a correct implementation, for one that ignores depth at every value but 0, and for one
	// with the right curve shape and the wrong magnitude. So the magnitude is asserted; the
	// difference check below is only there to prove the magnitude assertion is not vacuous.
	constexpr size_t kFrames = 24576;
	const std::vector<float> onCentre = sineAt(440.0, kFrames);
	const double in = rmsOf(onCentre, kFrames / 2);
	REQUIRE(in > 0.5);

	const auto gainAtCentre = [&](double width, double depth) {
		Settings s;
		s.enabled = true;
		s.level = 0.0f; // the oscillator silent, so what is measured is the notch alone
		s.width = static_cast<float>(width);
		s.depth = static_cast<float>(depth);
		s.noteSemitones = 69.0f; // A4 = 440, so the centre is the tone being measured
		s.gate = true;
		REQUIRE(notchCentreHz(s) == doctest::Approx(440.0).epsilon(1e-6));
		// SETTLED HALF ONLY — a biquad's own transient counted as signal would understate the cut.
		return rmsOf(render(s, onCentre, 128).left, kFrames / 2) / in;
	};

	SUBCASE("the attenuation at the centre is 1 - depth") {
		for (const double depth : { 0.0, 0.25, 0.5, 0.75 }) {
			CAPTURE(depth);
			CHECK(gainAtCentre(0.5, depth) == doctest::Approx(1.0 - depth).epsilon(2e-3));
		}
		// FULL DEPTH IS THE NOTCH THAT SHIPPED — not "very quiet" but the zeros back on the circle.
		CHECK(gainAtCentre(0.5, 1.0) < 1.0e-3);
	}

	SUBCASE("and neither Q nor the centre frequency moves it") {
		// THE PROPERTY THAT PROVES THE ZEROS MOVED AND NOT THE WHOLE BIQUAD. A depth that shifted the
		// poles would be a second width control wearing another name, and its centre attenuation
		// would drift with Q.
		for (const double width : { 0.05, 0.2, 0.5, 0.9 }) {
			CAPTURE(width);
			CAPTURE(notchQ([&] {
				Settings s;
				s.width = static_cast<float>(width);
				return s;
			}()));
			CHECK(gainAtCentre(width, 0.5) == doctest::Approx(0.5).epsilon(2e-3));
		}

		// AND AT A DIFFERENT CENTRE ENTIRELY — two octaves down, same half-depth.
		Settings low;
		low.enabled = true;
		low.level = 0.0f;
		low.width = 0.5f;
		low.depth = 0.5f;
		low.noteSemitones = 45.0f; // A2 = 110 Hz
		low.gate = true;
		REQUIRE(notchCentreHz(low) == doctest::Approx(110.0).epsilon(1e-6));
		const std::vector<float> at110 = sineAt(110.0, kFrames);
		const double lowIn = rmsOf(at110, kFrames / 2);
		CHECK(rmsOf(render(low, at110, 128).left, kFrames / 2) / lowIn
		      == doctest::Approx(0.5).epsilon(5e-3));
	}

	SUBCASE("depth zero is a wire, to the bit") {
		// NOT "NEARLY FLAT". At depth 0 the numerator equals the denominator termwise, so the filter
		// is an identity — and an identity is a claim about samples, not about an RMS ratio.
		Settings flat;
		flat.enabled = true;
		flat.level = 0.0f;
		flat.width = 1.0f; // the widest notch there is, so this cannot pass by the notch being narrow
		flat.depth = 0.0f;
		flat.noteSemitones = 69.0f;
		flat.gate = true;
		const std::vector<float> input = sineAt(440.0, 512);
		const Run run = render(flat, input, 64);
		for (size_t i = 0; i < input.size(); ++i) {
			CAPTURE(i);
			REQUIRE(run.left[i] == input[i]);
		}
	}

	SUBCASE("and the default is a full notch, which is what the device is for") {
		// THE DEFAULT IS RIGHT RATHER THAN COMPATIBLE. A device whose job is *strip the fundamental*
		// should strip it when switched on; that depth 1 also reproduces what shipped is a
		// convenience and not the argument.
		Settings s;
		CHECK(s.depth == doctest::Approx(1.0f));
		CHECK(kParamDescs[static_cast<size_t>(Param::Depth)].def == doctest::Approx(1.0));
	}
}

TEST_CASE("the notch removes the frequency it is sitting on, and leaves its neighbours alone") {
	// WHAT THE DEVICE IS FOR, measured as a ratio rather than asserted from the coefficients. A
	// coefficient check would pass for a notch centred an octave away.
	const size_t frames = 8192;
	const Settings s = notchOnly(0.5, kUnisonHarmonic);
	REQUIRE(notchCentreHz(s) == doctest::Approx(440.0).epsilon(1e-6));

	// SKIPPING THE FIRST 2048 SAMPLES, so what is measured is the steady state rather than the
	// biquad's own settling — a transient counted as signal would understate the attenuation.
	const std::vector<float> onCentre = sineAt(440.0, frames);
	const Run notched = render(s, onCentre, 128);
	const double before = rmsOf(onCentre, 2048);
	const double after = rmsOf(notched.left, 2048);
	CHECK(before > 0.5);
	// Measured at better than -40 dB; the bar is -20, which no "notch that is really a gentle dip"
	// clears and no correct implementation misses.
	CHECK(after < before * 0.1);

	SUBCASE("and two octaves up is essentially untouched") {
		const std::vector<float> away = sineAt(1760.0, frames);
		const Run passed = render(s, away, 128);
		CHECK(rmsOf(passed.left, 2048) > rmsOf(away, 2048) * 0.9);
	}

	SUBCASE("and a silent oscillator leaves the notch ON the harmonic — the firmware's notch never reads Level") {
		// THIS USED TO PIN A FALL-BACK TO THE FUNDAMENTAL AT LEVEL 0, citing the firmware. The firmware's
		// test is on its harmonic selector's OFF position, which this device has no equivalent of; its
		// notch never consults level. So with Level at zero the strip sits where the harmonic says,
		// which is what a hand turning Level down to hear the strip alone is asking for (HARM parity
		// sweep, 2026-09-16).
		Settings silentOctave = notchOnly(0.5, 19);
		REQUIRE(silentOctave.level == 0.0f);
		CHECK(notchCentreHz(silentOctave) == doctest::Approx(880.0).epsilon(1e-6));

		const std::vector<float> at440 = sineAt(440.0, frames);
		CHECK(rmsOf(render(silentOctave, at440, 128).left, 2048) > rmsOf(at440, 2048) * 0.9); // untouched
		const std::vector<float> at880 = sineAt(880.0, frames);
		CHECK(rmsOf(render(silentOctave, at880, 128).left, 2048) < rmsOf(at880, 2048) * 0.1); // stripped
	}

	SUBCASE("and moving the harmonic moves what is removed, once the oscillator is audible") {
		// THE COUPLING, AT THE DSP RATHER THAN IN THE ARITHMETIC. harm_types.h's own case pins
		// notchCentreHz; this one proves the DSP is built from it.
		//
		// LEVEL IS RAISED JUST OFF ZERO — enough to make the oscillator real, and small enough that
		// the tone it ADDS at the notch centre (the oscillator is mixed in after the notch, which is
		// the whole "strip and replace" order) stays far below the bar the surviving input has to
		// clear. A louder oscillator would refill the notch it just cut and the measurement would be
		// reading its own sub-oscillator back.
		Settings octave = notchOnly(0.5, 19);
		octave.level = 0.02f;
		REQUIRE(ratioAt(19) == doctest::Approx(2.0)); // read off the table, not guessed
		REQUIRE(notchCentreHz(octave) == doctest::Approx(880.0).epsilon(1e-6));

		const std::vector<float> at880 = sineAt(880.0, frames);
		const std::vector<float> at440 = sineAt(440.0, frames);
		CHECK(rmsOf(render(octave, at880, 128).left, 2048) < rmsOf(at880, 2048) * 0.1);
		CHECK(rmsOf(render(octave, at440, 128).left, 2048) > rmsOf(at440, 2048) * 0.5);
	}
}

TEST_CASE("width zero is no notch at all, not a very narrow one") {
	// THE CONTROL'S DECLARED MEANING, and the distinction is audible: a notch at Q=30 still removes a
	// real amount of a tone sitting exactly on it, so "off" implemented as "very sharp" would colour
	// a sound the player asked HARM to leave alone.
	const std::vector<float> input = sineAt(440.0, 4096);
	Settings off = notchOnly(0.0, kUnisonHarmonic);
	const Run run = render(off, input, 128);
	for (size_t i = 0; i < input.size(); ++i) {
		CAPTURE(i);
		REQUIRE(run.left[i] == input[i]);
	}

	// AND THE SMALLEST WIDTH ABOVE ZERO REALLY DOES NOTCH, so the case above is a gate rather than a
	// notch that never worked.
	Settings narrow = notchOnly(0.001, kUnisonHarmonic);
	CHECK(rmsOf(render(narrow, input, 128).left, 2048) < rmsOf(input, 2048) * 0.5);
}

TEST_CASE("the oscillator runs at the harmonic it is asked for, and the value says so") {
	// THE VALUE, NOT THE BUFFER. A frequency is not recoverable from a signal that also carries a
	// notch and an envelope, which is the whole reason currentHzForTest exists — the same five-line
	// seam voicePitchForTest and voiceFilterTargetForTest are.
	Settings s;
	s.enabled = true;
	s.level = 0.5f;
	s.noteSemitones = 69.0f;
	s.harmonic = kUnisonHarmonic;
	s.gate = true;

	const std::vector<float> silence(2048, 0.0f);
	CHECK(render(s, silence, 128).currentHz == doctest::Approx(440.0).epsilon(1e-6));

	s.harmonic = 19; // an octave up
	CHECK(render(s, silence, 128).currentHz == doctest::Approx(880.0).epsilon(1e-6));

	// AND FINE TUNE IS SEMITONES ON THE SAME NUMBER, restated independently rather than read back.
	s.harmonic = kUnisonHarmonic;
	s.fineSemitones = 12.0f;
	CHECK(render(s, silence, 128).currentHz == doctest::Approx(880.0).epsilon(1e-6));
}

TEST_CASE("HARM renders the same samples at any block size") {
	// THE REPO'S OWN RULE, and HARM has three separate places to break it: a coefficient grid, a
	// portamento, and an envelope. The firmware's porta was raised to the BLOCK LENGTH, which makes
	// the glide depend on the host's buffer — that is the bug this case would have caught, and it is
	// why our porta is declared in seconds and smoothed per sample.
	Settings s;
	s.enabled = true;
	s.level = 0.6f;
	s.width = 0.4f;
	s.harmonic = 19;
	s.noteSemitones = 69.0f;
	s.attackSeconds = 0.01f;
	s.portaSeconds = 0.05f;
	s.spread = 0.3f;
	s.gate = true;

	const std::vector<float> input = sineAt(660.0, 4096, 0.4);
	const Run reference = render(s, input, 256);
	CHECK(rmsOf(reference.left, 1024) > 0.01);

	for (const int block : {1, 37, 128, 512}) {
		CAPTURE(block);
		const Run other = render(s, input, block);
		for (size_t i = 0; i < input.size(); ++i) {
			CAPTURE(i);
			REQUIRE(other.left[i] == reference.left[i]);
			REQUIRE(other.right[i] == reference.right[i]);
		}
	}
}

TEST_CASE("portamento glides the tracked frequency and a retrigger snaps it") {
	// PORTA IS A CLAIM ABOUT A NUMBER AT AN INSTANT, so it is read off the number.
	Settings s;
	s.enabled = true;
	s.level = 0.5f;
	s.noteSemitones = 69.0f;
	s.portaSeconds = 0.5f;
	s.gate = true;

	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(s);
	std::vector<float> block(256, 0.0f);
	harm.processStereo(block.data(), nullptr, 256);
	// THE FIRST NOTE SNAPS. A porta that applied to the start of a phrase would bend into every
	// attack, which is not what the control means.
	REQUIRE(harm.currentHzForTest() == doctest::Approx(440.0).epsilon(1e-6));

	// A NEW TARGET WHILE THE GATE STAYS HELD IS A GLIDE — partway after a tenth of the time constant,
	// and NOT yet arrived, which is the assertion that separates a glide from a snap.
	s.noteSemitones = 81.0f; // an octave up, 880 Hz
	harm.setTarget(s);
	harm.processStereo(block.data(), nullptr, 256);
	const double gliding = harm.currentHzForTest();
	CHECK(gliding > 440.0);
	CHECK(gliding < 880.0);

	// AND A RETRIGGER SNAPS, whatever the glide was doing.
	s.gate = false;
	harm.setTarget(s);
	harm.processStereo(block.data(), nullptr, 16);
	s.gate = true;
	harm.setTarget(s);
	harm.processStereo(block.data(), nullptr, 16);
	CHECK(harm.currentHzForTest() == doctest::Approx(880.0).epsilon(1e-6));
}

TEST_CASE("portamento takes the time it is asked for, in seconds") {
	// THIS CASE EXISTS BECAUSE A MUTATION SURVIVED THE OTHERS. Applying the smoothing coefficient
	// once per CALL instead of once per sample — the firmware's own shape — changes the glide's speed
	// by a factor of sixteen and was caught by NOTHING: the block-size case still passed, because the
	// control grid is absolute and the result stays block-independent either way, and the glide case
	// above only asked that the frequency be somewhere between its start and its target, which a
	// sixteen-times-too-slow glide also is.
	//
	// THE RATE IS THE CLAIM, so the rate is asserted — restated from the exponential rather than read
	// back from the device: after exactly one time constant a first-order glide has closed all but
	// 1/e of its distance.
	constexpr double kTimeConstant = 0.25;
	Settings s;
	s.enabled = true;
	s.level = 0.5f;
	s.noteSemitones = 69.0f; // 440 Hz
	s.portaSeconds = static_cast<float>(kTimeConstant);
	s.gate = true;

	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(s);
	std::vector<float> block(64, 0.0f);
	harm.processStereo(block.data(), nullptr, 64);
	REQUIRE(harm.currentHzForTest() == doctest::Approx(440.0).epsilon(1e-6));

	// The target moves while the gate stays held, so this is a glide rather than a retrigger.
	s.noteSemitones = 81.0f; // 880 Hz
	harm.setTarget(s);

	const int frames = static_cast<int>(kTimeConstant * kRate);
	for (int at = 0; at < frames; at += 64) {
		harm.processStereo(block.data(), nullptr, std::min(64, frames - at));
	}

	const double expected = 880.0 + (440.0 - 880.0) * std::exp(-1.0);
	// A percent of tolerance covers the sixteen-sample grid the frequency is stepped on; it does not
	// cover being wrong by a factor of sixteen, which is what it is here to refuse.
	CHECK(harm.currentHzForTest() == doctest::Approx(expected).epsilon(0.01));
}

TEST_CASE("spread separates the channels and zero spread leaves them identical") {
	// SEPARATE FROM PHASE, unlike the firmware, which folded the two into one control and therefore
	// could not offer a centred oscillator with any width at all.
	Settings s;
	s.enabled = true;
	s.level = 0.8f;
	s.noteSemitones = 69.0f;
	s.gate = true;
	const std::vector<float> silence(2048, 0.0f);

	const Run centred = render(s, silence, 128);
	for (size_t i = 0; i < silence.size(); ++i) {
		CAPTURE(i);
		REQUIRE(centred.left[i] == centred.right[i]);
	}
	// And it is a real signal, so the equality above is not two silences agreeing.
	CHECK(rmsOf(centred.left, 512) > 0.1);

	s.spread = 1.0f; // a half turn: the channels in antiphase
	const Run wide = render(s, silence, 128);
	CHECK(rmsOf(wide.left, 512) > 0.1);
	for (size_t i = 512; i < silence.size(); ++i) {
		CAPTURE(i);
		REQUIRE(wide.left[i] == doctest::Approx(-wide.right[i]).epsilon(1e-5));
	}
}

TEST_CASE("the harmonic axis: the device seen in ratio, and the same picture at every note") {
	// THE OBJECTION THIS ANSWERS IS THE CARD'S OWN, and it dissolves rather than being overridden.
	// `harm_card.h` recorded why HARM has no picture: *"the notch's frequency depends on the NOTE
	// BEING PLAYED, and a card has no note … What the card shows instead is the RATIO, which is the
	// part that is actually a setting: '3:2' is true at every pitch."*
	//
	// The notch centre is the note TIMES a ratio, so on a RATIO axis it sits at the selected harmonic
	// for every note. This case is the proof of that sentence, because the expanded view is about to
	// be built on it.

	SUBCASE("the notch's position on the axis is the selected ratio, at every note") {
		// FOUR OCTAVES OF NOTE. If the position moved with the note the axis would be a lie and the
		// whole picture with it.
		for (const size_t harmonic : { size_t { 2 }, size_t { 9 }, size_t { 13 }, size_t { 14 },
			                           size_t { 19 }, kHarmonicCount - 1 }) {
			CAPTURE(harmonic);
			CAPTURE(harmonicLabel(harmonic));
			double first = 0.0;
			for (const double note : { 21.0, 45.0, 69.0, 93.0, 108.0 }) {
				CAPTURE(note);
				Settings s;
				s.enabled = true;
				s.level = 0.5f; // sounding, so the level<=0 fallback is not what is being measured
				s.width = 0.5f;
				s.harmonic = harmonic;
				s.noteSemitones = static_cast<float>(note);
				s.gate = true;
				// THE VALUE, not "the same as the last one": at fine 0 the position IS the table's
				// ratio, which is a number this case reads from the table rather than from the model.
				CHECK(notchRatio(s)
				      == doctest::Approx(static_cast<double>(ratioAt(harmonic))).epsilon(1e-9));
				if (first == 0.0) {
					first = notchRatio(s);
				}
				else {
					CHECK(notchRatio(s) == doctest::Approx(first).epsilon(1e-9));
				}
			}
		}
	}

	SUBCASE("fine detune moves the notch OFF its exact harmonic, by exactly the detune") {
		// AND THIS IS WHY THE AXIS'S ORIGIN EXCLUDES FINE. Fine detunes the OSCILLATOR; an origin
		// that moved with it could not show the detune at all, and Fine is one of the controls the
		// expanded view exists to give a picture to.
		for (const float fine : { -1.0f, -0.25f, 0.0f, 0.25f, 1.0f, 7.0f }) {
			CAPTURE(fine);
			Settings s;
			s.enabled = true;
			s.level = 0.5f;
			s.width = 0.5f;
			s.harmonic = 14; // 3:2
			s.fineSemitones = fine;
			s.noteSemitones = 69.0f;
			s.gate = true;
			REQUIRE(ratioAt(14) == doctest::Approx(1.5f));
			CHECK(notchRatio(s)
			      == doctest::Approx(1.5 * std::exp2(static_cast<double>(fine) / 12.0)).epsilon(1e-9));
		}
	}

	SUBCASE("a silent oscillator leaves the notch on the harmonic — Level is not an input to the axis") {
		// THE FALLBACK IS REAL AND A PLAYER WILL HIT IT: `notchCentreHz` drops the harmonic to unison
		// when the oscillator is silent, because a notch tracking a harmonic nobody is sounding would
		// be stripping a frequency for no reason. The picture must follow it rather than hold a
		// stale position.
		//
		// THE BRIEF SAID "the notch jumping to 1:1" AND THAT IS EXACT ONLY AT FINE ZERO. The fallback
		// zeroes the HARMONIC and leaves the detune alone, so at fine f the notch lands on 2^(f/12).
		// That is what the DSP does; the number is asserted rather than rounded to the tidy answer.
		Settings s;
		s.enabled = true;
		s.width = 0.5f;
		s.harmonic = 19; // 2:1 — somewhere the fallback has to travel from
		s.noteSemitones = 69.0f;
		s.gate = true;

		s.level = 0.5f;
		REQUIRE(notchRatio(s) == doctest::Approx(2.0).epsilon(1e-9));

		// LEVEL DOES NOT MOVE THE NOTCH (HARM parity sweep, 2026-09-16 — the fall-back to 1:1 this
		// subcase used to pin was ours, not the firmware's, whose notch never reads level). The axis
		// stays on the harmonic; fine tune still rides on top of it.
		s.level = 0.0f;
		CHECK(notchRatio(s) == doctest::Approx(2.0).epsilon(1e-9));

		s.fineSemitones = 3.0f;
		CHECK(notchRatio(s) == doctest::Approx(2.0 * std::exp2(3.0 / 12.0)).epsilon(1e-9));
	}

	SUBCASE("the axis and the hertz cannot drift, because one is defined as the other") {
		// `notchRatio` IS `notchCentreHz / playedNoteHz`, not a second derivation that agrees with
		// it. This case is what stops somebody "simplifying" it into a harmonic-table lookup, which
		// would be right until the level<=0 fallback or the fine detune moved.
		for (const size_t harmonic : { size_t { 0 }, size_t { 2 }, size_t { 9 }, size_t { 40 },
			                           kHarmonicCount - 1 }) {
			for (const float fine : { -2.0f, 0.0f, 5.0f }) {
				for (const float level : { 0.0f, 0.7f }) {
					for (const float bend : { -12.0f, 0.0f, 2.0f }) {
						CAPTURE(harmonic);
						CAPTURE(fine);
						CAPTURE(level);
						CAPTURE(bend);
						Settings s;
						s.enabled = true;
						s.harmonic = harmonic;
						s.fineSemitones = fine;
						s.level = level;
						s.bendSemitones = bend;
						s.width = 0.5f;
						s.noteSemitones = 60.0f;
						s.gate = true;
						CHECK(playedNoteHz(s) * notchRatio(s)
						      == doctest::Approx(notchCentreHz(s)).epsilon(1e-9));
					}
				}
			}
		}
	}

	SUBCASE("at the centre the drawn magnitude is exactly 1 - depth") {
		// THE SAME NUMBER `Param::Depth` PROMISES AND THE BIQUAD MEASURES, reached from the other
		// direction — the picture and the DSP checked against one derivation rather than against
		// each other. See "depth sets how much of the centre the notch removes" for the biquad half.
		for (const double depth : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
			for (const double width : { 0.05, 0.3, 0.8, 1.0 }) {
				CAPTURE(depth);
				CAPTURE(width);
				Settings s;
				s.enabled = true;
				s.level = 0.5f;
				s.harmonic = 14;
				s.width = static_cast<float>(width);
				s.depth = static_cast<float>(depth);
				s.noteSemitones = 69.0f;
				s.gate = true;
				CHECK(notchMagnitudeAtRatio(s, notchRatio(s))
				      == doctest::Approx(1.0 - depth).epsilon(1e-9));
			}
		}
	}

	SUBCASE("width zero is a wire on the axis too, not a very narrow trough") {
		// `refreshCoefficients`' OWN RULE, inherited rather than restated: "width zero is no notch at
		// all, which is the control's declared meaning and not a small-Q approximation of it". A
		// picture that drew a narrow dip at width 0 would contradict the device at its DEFAULT.
		Settings s;
		s.enabled = true;
		s.level = 0.5f;
		s.harmonic = 14;
		s.width = 0.0f;
		s.depth = 1.0f;
		s.noteSemitones = 69.0f;
		s.gate = true;
		REQUIRE(s.width == 0.0f);
		for (const double ratio : { 0.125, 0.5, 1.0, 1.5, 3.0, 32.0 }) {
			CAPTURE(ratio);
			CHECK(notchMagnitudeAtRatio(s, ratio) == doctest::Approx(1.0).epsilon(1e-12));
		}
	}

	SUBCASE("the drawn curve is what the biquad actually does, measured") {
		// A MODEL IS EVIDENCE ABOUT MECHANISM, NOT ABOUT MAGNITUDE — so the magnitude is measured
		// against the real filter rather than argued from the algebra that produced it.
		//
		// MEASURED, 2026-08-24, at 48 kHz over four notes (110/220/440/880 Hz), three widths
		// (Q = 28.5 / 21.2 / 6.4) and two depths, at ten points from r = 0.25 to r = 8:
		//
		//   worst absolute error 0.0056, and that one is the deep null at r = 1 where the model says
		//   0.0 and the RMS of a settling biquad says 0.0056 — a floor in the INSTRUMENT, not in the
		//   model. Excluding the nulls the worst is 0.0014.
		//
		// WHERE IT STOPS BEING TRUE, also measured, because the axis reaches 32:1 and a high harmonic
		// on a high note is a real place to be. The prototype is the ANALOG one, so it does not carry
		// the bilinear warping; probed at r = 1.15 with the centre swept up the spectrum:
		//
		//   centre at fs/100  err 0.00014      centre at fs/12   err 0.0026
		//   centre at fs/46   err 0.00018      centre at fs/6    err 0.0092
		//   centre at fs/23   err 0.00068      centre at fs/4    err 0.017
		//                                      centre at fs/3    err 0.023
		//
		// So it is exact to a thousandth while the notch is below about fs/20 — 2.4 kHz at 48 kHz —
		// and the real filter's shoulder rides ABOVE the drawn one past that, because a digital notch
		// narrows in absolute hertz as it approaches Nyquist. THE AXIS DOES NOT REPRESENT THIS AND
		// SHOULD NOT: a ratio has no sample rate. It is written down here so the next person reads a
		// number instead of the word "close".
		constexpr size_t kFrames = 32768;
		for (const double note : { 45.0, 57.0, 69.0, 81.0 }) {
			for (const double width : { 0.05, 0.3, 0.8 }) {
				for (const double depth : { 0.4, 1.0 }) {
					Settings s;
					s.enabled = true;
					s.level = 0.0f; // the oscillator silent, so what is measured is the notch alone
					s.width = static_cast<float>(width);
					s.depth = static_cast<float>(depth);
					s.noteSemitones = static_cast<float>(note);
					s.gate = true;
					const double base = playedNoteHz(s);
					for (const double r : { 0.25, 0.5, 0.8, 0.95, 1.0, 1.05, 1.25, 2.0, 4.0 }) {
						const double hz = base * r;
						if (hz < 20.0 || hz > kRate * 0.4) {
							continue;
						}
						CAPTURE(note);
						CAPTURE(width);
						CAPTURE(depth);
						CAPTURE(r);
						const std::vector<float> in = sineAt(hz, kFrames);
						const double amplitude = rmsOf(in, kFrames / 2);
						REQUIRE(amplitude > 0.5);
						const double measured =
						    rmsOf(render(s, in, 128).left, kFrames / 2) / amplitude;
						const double modelled = notchMagnitudeAtRatio(s, r);
						CAPTURE(measured);
						CAPTURE(modelled);
						// ABSOLUTE, not relative: at full depth the model is 0.0 at the centre and a
						// relative tolerance against zero has no meaning. The bar is the measured
						// worst above, rounded up by a hair.
						CHECK(std::abs(measured - modelled) < 0.006);
					}
				}
			}
		}
	}
}

TEST_CASE("the drawn curve contains its own trough, which no amount of resolution would buy") {
	// THE PICTURE'S RESOLUTION PROBLEM, AND IT IS NOT THE FILTER'S. `filter_card.h` records that its
	// curve shipped at 96 points and missed a resonant peak by 1.97 dB, and answers it with more
	// points — which works, because a resonant peak is finite and a fine enough grid straddles it.
	//
	// A NOTCH IS NOT FINITE. MEASURED across every harmonic in the table at width 0.02 (Q = 29.4) and
	// full depth, taking the deepest point a log-spaced curve contains:
	//
	//      96 points  -1.3 dB      1024 points  -16.1 dB
	//     256 points  -5.4 dB      2048 points  -22.0 dB
	//     601 points -12.0 dB      4096 points  -28.0 dB
	//
	// The truth is minus infinity. Every one of those is wrong, doubling the count buys 6 dB, and
	// there is no count at which the curve is right. **So the one point that matters is PLACED.** Its
	// location is known exactly — it is `notchRatio` — and `ratioProfile` moves the nearest sample
	// onto it.

	const auto profileOf = [](const Settings& s, size_t n) {
		std::vector<float> ratios(n);
		std::vector<float> gains(n);
		ratioProfile(s, ratios.data(), gains.data(), n);
		return std::pair { ratios, gains };
	};

	SUBCASE("the deepest point of the curve is EXACTLY 1 - depth, at any resolution") {
		// THE VALUE, and the reason this case exists rather than a "the curve dips" check: a dip is
		// what the 96-point curve had too, 1.3 dB of it, and it would have passed a difference check
		// while drawing a notch that removes a quarter of the signal as one that removes nothing.
		for (const size_t n : { size_t { 96 }, size_t { 256 }, size_t { 601 }, size_t { 2048 } }) {
			for (const double width : { 0.02, 0.05, 0.3, 1.0 }) {
				for (const double depth : { 0.0, 0.35, 1.0 }) {
					for (const size_t harmonic : { size_t { 0 }, size_t { 9 }, size_t { 14 },
						                           size_t { 70 }, kHarmonicCount - 1 }) {
						CAPTURE(n);
						CAPTURE(width);
						CAPTURE(depth);
						CAPTURE(harmonic);
						Settings s;
						s.enabled = true;
						s.level = 0.5f;
						s.harmonic = harmonic;
						s.width = static_cast<float>(width);
						s.depth = static_cast<float>(depth);
						s.noteSemitones = 69.0f;
						s.gate = true;
						const auto [ratios, gains] = profileOf(s, n);
						const auto lowest = std::min_element(gains.begin(), gains.end());
						REQUIRE(lowest != gains.end());
						CHECK(static_cast<double>(*lowest)
						      == doctest::Approx(1.0 - depth).epsilon(1e-6));
						// AND IT IS THE CENTRE THAT IS LOWEST, not some other point that happens to
						// match — the depth being right at the wrong place is exactly the partial
						// effect a value check has to rule out.
						//
						// ONLY WHERE THERE IS A TROUGH TO FIND. At depth 0 the curve is flat at 1.0
						// and `min_element` returns the FIRST element, so this would be asserting
						// that the notch sits at 1:8. That is not the check failing, it is the check
						// having nothing to say — and a version of it that "passed" there would have
						// been passing on the left-hand edge of every flat curve.
						if (depth > 0.0) {
							const size_t at = static_cast<size_t>(lowest - gains.begin());
							CHECK(static_cast<double>(ratios[at])
							      == doctest::Approx(notchRatio(s)).epsilon(1e-5));
						}
					}
				}
			}
		}
	}

	SUBCASE("the substituted point moves by less than half a step, so the curve keeps its spacing") {
		// THE COST OF PLACING IT, measured rather than waved at. If the substitution could move a
		// point a visible distance it would be trading a wrong depth for a wrong position.
		constexpr size_t n = 601;
		const double step =
		    std::log2(kProfileHiRatio / kProfileLoRatio) / static_cast<double>(n - 1);
		double worst = 0.0;
		for (size_t harmonic = 0; harmonic < kHarmonicCount; ++harmonic) {
			CAPTURE(harmonic);
			Settings s;
			s.enabled = true;
			s.level = 0.5f;
			s.harmonic = harmonic;
			s.width = 0.5f;
			s.noteSemitones = 69.0f;
			s.gate = true;
			const auto [ratios, gains] = profileOf(s, n);
			// EVERY point is within half a step of where the plain log grid would have put it —
			// asked of all 601 rather than of the one that was moved, since a substitution that
			// shifted its neighbours would be a worse bug than the one being fixed.
			for (size_t i = 0; i < n; ++i) {
				const double grid = kProfileLoRatio * std::exp2(step * static_cast<double>(i));
				const double moved = std::abs(std::log2(static_cast<double>(ratios[i]) / grid));
				CHECK(moved <= step * 0.5 + 1e-9);
				worst = std::max(worst, moved);
			}
		}
		// MEASURED, 2026-08-24: the step is 0.01333 octaves and the worst displacement across the
		// whole table is 0.00635 — 0.16 px on a 200 px eight-octave axis.
		CAPTURE(worst);
		CHECK(worst > 0.0);  // or the substitution never happened and the case above is a coincidence
		CHECK(worst < 0.007);
	}

	SUBCASE("the ends of the axis are the harmonic table's ends") {
		// A PICTURE WHOSE AXIS WAS NARROWER THAN ITS CONTROL WOULD HAVE POSITIONS IT COULD NOT DRAW.
		CHECK(static_cast<double>(ratioAt(0)) == doctest::Approx(kProfileLoRatio));
		CHECK(static_cast<double>(ratioAt(kHarmonicCount - 1)) == doctest::Approx(kProfileHiRatio));
		for (size_t harmonic = 0; harmonic < kHarmonicCount; ++harmonic) {
			CAPTURE(harmonic);
			CHECK(static_cast<double>(ratioAt(harmonic)) >= kProfileLoRatio);
			CHECK(static_cast<double>(ratioAt(harmonic)) <= kProfileHiRatio);
		}
		constexpr size_t n = 128;
		Settings s;
		s.enabled = true;
		s.level = 0.5f;
		s.width = 0.5f;
		s.noteSemitones = 69.0f;
		s.gate = true;
		const auto [ratios, gains] = profileOf(s, n);
		CHECK(static_cast<double>(ratios.front()) == doctest::Approx(kProfileLoRatio));
		CHECK(static_cast<double>(ratios.back()) == doctest::Approx(kProfileHiRatio));
	}

	SUBCASE("it writes nothing it was not given room for, and a null ratio buffer is allowed") {
		// THE CONTRACT filter::gainProfile SET, kept rather than re-invented: caller storage, no
		// allocation, and the frequencies optional because a curve of fixed width places by index.
		Settings s;
		s.enabled = true;
		s.level = 0.5f;
		s.width = 0.5f;
		s.noteSemitones = 69.0f;
		s.gate = true;

		std::vector<float> gains(4, -1.0f);
		ratioProfile(s, nullptr, gains.data(), 4);
		for (const float g : gains) {
			CHECK(g >= 0.0f);
		}

		// ONE POINT HAS NO SPAN TO DIVIDE, and the loop's `n - 1` would be a division by zero.
		std::vector<float> single(1, -1.0f);
		std::vector<float> singleRatio(1, -1.0f);
		ratioProfile(s, singleRatio.data(), single.data(), 1);
		CHECK(static_cast<double>(singleRatio[0]) == doctest::Approx(kProfileLoRatio));
		CHECK(static_cast<double>(single[0])
		      == doctest::Approx(notchMagnitudeAtRatio(s, kProfileLoRatio)));

		// AND ZERO POINTS TOUCHES NOTHING.
		std::vector<float> untouched(2, -7.0f);
		ratioProfile(s, nullptr, untouched.data(), 0);
		CHECK(untouched[0] == -7.0f);
		CHECK(untouched[1] == -7.0f);
	}
}

TEST_CASE("every declared default agrees with the struct's, not just the five with a static_assert") {
	// **THE RULE IS WRITTEN DOWN AND HOLDS FOR FIVE OF ELEVEN.** `harm_types.h` states it and names
	// its own mechanism: *"THESE MUST AGREE WITH `kParamDescs[].def` AND NOTHING MAKES THEM… A
	// static_assert below is what stops them drifting."* There are eleven patch-state fields and five
	// static_asserts — level, width, depth, harmonic, enabled. **Zero drift today: all eleven agree.**
	// So the comment is true about the mechanism and false about its reach, which is the shape a
	// rule-comment sweep is for.
	//
	// **AND SIX MORE static_asserts IS THE WRONG FIX, for a reason the finding itself demonstrates.**
	// Three of the six unguarded — Attack, Release, Porta — are stored as **log2-seconds** in the
	// descriptor and as **seconds** in the field. Release's `def` is -2.0 and `releaseSeconds` is
	// 0.25; `exp2(-2) == 0.25`, so they agree exactly and the naive assert reads FALSE. The first
	// probe at this reported three drifts for precisely that reason. And the obvious remedy is
	// foreclosed anyway: **`std::exp2` is not constexpr**, so the three fields most likely to drift
	// are the three no `static_assert` can reach.
	//
	// **SO THE COMPARISON IS MADE IN ONE UNIT BY CONSTRUCTION, AND THE TEST CARRIES NO CONVERSION.**
	// `rowOf` is the device's own indexed view of itself and already holds the seconds/log-seconds
	// mapping — once, in production, where `stateOf` inverts it. Comparing `rowOf(State {})` against
	// the declared defaults asks the same question with no arithmetic on this side of the seam at
	// all. **A test that applied the conversion itself would be a second copy of it, which is what
	// this whole finding is about.**
	constexpr size_t kCount = static_cast<size_t>(Param::kCount);
	static_assert(std::size(kParamDescs) == kCount, "the walk below is driven from kCount");

	Row declared {};
	for (size_t i = 0; i < kCount; ++i) {
		declared[i] = static_cast<float>(kParamDescs[i].def);
	}

	const Row fresh = rowOf(State {});

	// THE WHOLE THING, so nothing escapes.
	CHECK(fresh == declared);

	// AND EACH FIELD BY NAME, so a failure says WHICH — driven from `Param::kCount`, so a twelfth
	// control is covered without anybody coming back.
	int checked = 0;
	std::vector<std::string> disagreed;
	for (size_t i = 0; i < kCount; ++i) {
		++checked;
		if (fresh[i] != declared[i]) {
			disagreed.push_back(std::string(kParamDescs[i].name) + ": struct "
			                    + std::to_string(fresh[i]) + " vs declared "
			                    + std::to_string(declared[i]));
		}
	}
	for (const std::string& s : disagreed) {
		MESSAGE("DEFAULT DISAGREES: " << s);
	}
	CHECK(disagreed.empty());
	CHECK(checked == 48); // sixteen as they were, partial 1's switch, the envelope's D/H/D/S, and three more partials of nine

	// AND THE ROUND TRIP, which is the claim as a caller meets it: a patch that omits every field
	// decodes to the same device as `State {}`.
	CHECK(stateOf(declared, kCount) == State {});

	// **NON-VACUITY, AND IT IS NOT DECORATION HERE.** If `rowOf` returned zeros, or the declared
	// defaults were all zero, every assertion above would hold while saying nothing. So: the defaults
	// must be a spread of real values, and a row of the range MINIMUMS must decode to a DIFFERENT
	// device — which is what shows the comparison can tell two states apart at all.
	int nonZero = 0;
	for (size_t i = 0; i < kCount; ++i) {
		if (declared[i] != 0.0f) {
			++nonZero;
		}
	}
	CAPTURE(nonZero);
	CHECK(nonZero >= 4);

	Row minimums {};
	for (size_t i = 0; i < kCount; ++i) {
		minimums[i] = static_cast<float>(kParamDescs[i].min);
	}
	CHECK_FALSE(stateOf(minimums, kCount) == State {});
}

// ---------------------------------------------------------------------------------------------
// THE HARM PARITY SWEEP, 2026-09-16 — the port read against the firmware's `harm.h` at the commit it
// was ported from. Values through seams; each case has a known-bad that went red (the commit says which).
// ---------------------------------------------------------------------------------------------

namespace {

/// A HARM with the notch off (Width 0 is a wire), the oscillator at full level with an instant
/// attack, so the output IS the sine and its gain law can be read off the samples.
Settings sineOnly() {
	Settings s;
	s.enabled = true;
	s.level = 1.0f;
	s.width = 0.0f;
	s.attackSeconds = 0.0f;
	s.releaseSeconds = 0.05f;
	s.noteSemitones = 69.0f;
	s.harmonic = kUnisonHarmonic;
	s.gate = true;
	return s;
}

void renderSilence(Harm& harm, int frames, int block = 64) {
	std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
	int at = 0;
	while (at < frames) {
		const int take = std::min(block, frames - at);
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		harm.processStereo(l.data(), r.data(), take);
		at += take;
	}
}

} // namespace

TEST_CASE("parity: the sine is pink-scaled — -3 dB per octave above 40 Hz, unity below, never a boost") {
	// The firmware's `pinkScale = outputHz > 40 ? sqrt(40 / outputHz) : 1`, dropped by the port. The
	// Harmonic control spans 1:8 to 32:1, so this is the law that keeps 32:1 from screaming.
	Settings s = sineOnly();
	s.harmonic = 19; // 2:1 — 880 Hz
	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(s);
	renderSilence(harm, 4096);
	const double expected = std::sqrt(40.0 / 880.0);
	CHECK(harm.pinkScaleForTest() == doctest::Approx(expected).epsilon(1e-6));

	// AND IT IS ON THE SAMPLES: a full-level sine at 880 comes out at the pink gain, read as RMS * sqrt 2.
	Run run = render(s, std::vector<float>(8192, 0.0f), 128);
	CHECK(rmsOf(run.left, 4096) * std::sqrt(2.0) == doctest::Approx(expected).epsilon(0.02));

	// BELOW 40 Hz THE SCALE IS ONE — and never above it.
	s.harmonic = kUnisonHarmonic;
	s.noteSemitones = 20.0f; // ~25.96 Hz
	Harm low;
	low.prepare(kRate);
	low.setTarget(s);
	renderSilence(low, 1024);
	CHECK(low.pinkScaleForTest() == doctest::Approx(1.0));
}

TEST_CASE("parity: a released envelope reaches exactly zero — the firmware's 1e-7 floor, so the sine's gain "
          "does not sit denormal after every note") {
	Settings s = sineOnly();
	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(s);
	renderSilence(harm, 2048);
	REQUIRE(harm.envelopeForTest() == doctest::Approx(1.0));
	s.gate = false;
	harm.setTarget(s);
	// A one-pole at 50 ms never reaches zero on its own; two seconds is forty time constants,
	// e^-40 ≈ 4e-18 — far below the floor, so the floor is what makes this exactly 0.
	renderSilence(harm, static_cast<int>(2.0 * kRate));
	CHECK(harm.envelopeForTest() == 0.0);
}

TEST_CASE("parity: Phase is a START phase — a retrigger aims the oscillator at it and converges, as the "
          "firmware's phase catch-up does") {
	// The port added Phase as a constant offset to a free-running accumulator, so only the first note
	// after a reset started where the knob said. Now a retrigger arms a catch-up toward a whole turn
	// (the offset rides on the read), paid off at increment/16 per sample.
	Settings s = sineOnly();
	s.phase = 0.25f;
	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(s);
	// 3008 + 512 = 3520 samples: a multiple of the 16-sample control grid, so the retrigger is seen at
	// the FIRST sample of the next call (the grid is where the trigger is read), and 32.27 cycles of
	// 440 Hz, so the accumulator is mid-cycle when it is.
	renderSilence(harm, 3008);
	s.gate = false;
	harm.setTarget(s);
	renderSilence(harm, 512);
	const double before = harm.phaseForTest();
	REQUIRE(std::fabs(before - std::floor(before + 0.5)) > 0.05); // mid-cycle, not already at a turn

	// RETRIGGER, then read the catch-up before it has been paid: non-vacuously armed.
	s.gate = true;
	harm.setTarget(s);
	renderSilence(harm, Harm::kControlBlock);
	CHECK(harm.phaseCatchupForTest() != 0.0);

	// AFTER ENOUGH CYCLES the accumulator sits exactly where a note started at a whole turn would:
	// n samples of increment past zero. Restated from the increment, not read back.
	const int settle = 48000; // a second: ~440 cycles against a ~16-cycle convergence
	renderSilence(harm, settle);
	CHECK(harm.phaseCatchupForTest() == 0.0);
	const double increment = 440.0 / kRate;
	const double expected = std::fmod(static_cast<double>(Harm::kControlBlock + settle) * increment, 1.0);
	const double error = harm.phaseForTest() - expected;
	CHECK(std::fabs(error - std::round(error)) < 1e-6); // equal modulo a turn
}

TEST_CASE("parity: Spread is symmetric about Phase — +-spread/2, as the firmware places the pair") {
	// With Phase 0 and Spread at the top, the first sample is L at -1/4 turn and R at +1/4 turn: -1 and
	// +1 (times the pink gain at 440 Hz). The port used to hold L at Phase and move R by the whole
	// spread, which put the first sample at 0 and 0.
	Settings s = sineOnly();
	s.spread = 1.0f;
	Harm harm;
	harm.prepare(kRate);
	harm.setTarget(s);
	std::vector<float> l(1, 0.0f), r(1, 0.0f);
	harm.processStereo(l.data(), r.data(), 1);
	const double pink = harm.pinkScaleForTest();
	REQUIRE(pink > 0.2); // sqrt(40/440) ≈ 0.30
	CHECK(l[0] == doctest::Approx(-pink).epsilon(1e-5));
	CHECK(r[0] == doctest::Approx(pink).epsilon(1e-5));
}

TEST_CASE("backlog FK: moving Level, Phase and Spread are followed per sample inside the call — the samples of setting them before every sample") {
	// Restated independently: a twin handed Level, Phase and Spread through `setTarget` before EACH one-sample call (the
	// control grid is absolute, so one-sample calls are the same stream) hears what a window describes. The windowed
	// device, handed the three windows once per 64-sample call through the ops road, must render those samples.
	constexpr int kFrames = 64;
	constexpr int kTotal = 4096;
	Settings s;
	s.enabled = true;
	s.noteSemitones = 57.0f;
	s.gate = true;
	s.level = 0.5f;
	s.phase = 0.1f;
	s.spread = 0.2f;
	std::array<float, kFrames> level {}, phase {}, spread {};
	for (int i = 0; i < kFrames; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kFrames - 1);
		level[static_cast<size_t>(i)] = t;
		phase[static_cast<size_t>(i)] = 0.25f * t;
		spread[static_cast<size_t>(i)] = 1.0f - t;
	}
	std::array<const float*, 32> runs {};
	runs[static_cast<size_t>(Param::Level)] = level.data();
	runs[static_cast<size_t>(Param::Phase)] = phase.data();
	runs[static_cast<size_t>(Param::Spread)] = spread.data();
	REQUIRE(kAudioOps.setFieldWindows != nullptr);

	std::vector<float> in(kTotal);
	for (int i = 0; i < kTotal; ++i) {
		in[static_cast<size_t>(i)] = 0.2f * std::sin(0.02f * static_cast<float>(i));
	}
	Harm windowed;
	Harm twin;
	windowed.prepare(48000.0);
	twin.prepare(48000.0);
	windowed.setTarget(s);
	twin.setTarget(s);
	std::vector<float> wl = in, wr = in, tl = in, tr = in;
	for (int at = 0; at < kTotal; at += kFrames) {
		kAudioOps.setFieldWindows(&windowed, runs.data(), runs.size(), kFrames);
		windowed.processStereo(wl.data() + at, wr.data() + at, kFrames);
	}
	float apart = 0.0f;
	for (int i = 0; i < kTotal; ++i) {
		Settings at = s;
		at.level = level[static_cast<size_t>(i % kFrames)];
		at.phase = phase[static_cast<size_t>(i % kFrames)];
		at.spread = spread[static_cast<size_t>(i % kFrames)];
		twin.setTarget(at);
		twin.processStereo(tl.data() + i, tr.data() + i, 1);
		apart = std::max(apart, std::abs(tl[static_cast<size_t>(i)] - in[static_cast<size_t>(i)]));
	}
	float worst = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		worst = std::max({ worst, std::abs(wl[i] - tl[i]), std::abs(wr[i] - tr[i]) });
	}
	CAPTURE(worst);
	CHECK(apart > 0.05f); // not vacuous: the oscillator sounds
	CHECK(worst < 1e-6f);
}

TEST_CASE("Depth Follows Env: no note is no notch, the depth rises with the attack and falls to exactly zero with the release; off, the notch holds its Depth with no note") {
	// An option: the filter depth follows the envelope. The claim is a VALUE — the depth the
	// coefficients carry — read where the notch is built, not inferred from a buffer.
	constexpr double kRate = 48000.0;
	const auto make = [&](bool follow) {
		Settings s;
		s.noteSemitones = 57.0f;
		s.level = 0.0f; // no oscillator: the notch alone
		s.width = 0.3f;
		s.depth = 0.8f;
		s.attackSeconds = 0.05f;
		s.releaseSeconds = 0.05f;
		s.depthFollowsEnv = follow;
		return s;
	};
	std::vector<float> l(256), r(256);
	const auto run = [&](Harm& harm, int frames) {
		for (int done = 0; done < frames; done += 256) {
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = r[i] = 0.25f * static_cast<float>(std::sin(0.05 * static_cast<double>(i + static_cast<size_t>(done))));
			}
			harm.processStereo(l.data(), r.data(), 256);
		}
	};

	SUBCASE("on") {
		Harm harm;
		harm.prepare(kRate);
		Settings s = make(true);
		harm.setTarget(s);
		run(harm, 4096);
		CHECK(harm.notchDepthForTest() == 0.0); // no note, no notch
		// AND NOT A NOTCH AT DEPTH ZERO: the cheap road, which passes the input untouched.
		std::array<float, 256> in {};
		for (size_t i = 0; i < in.size(); ++i) {
			in[i] = l[i] = r[i] = 0.25f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
		}
		harm.processStereo(l.data(), r.data(), 256);
		CHECK(std::equal(in.begin(), in.end(), l.begin()));

		s.gate = true;
		harm.setTarget(s);
		run(harm, 512); // ~10.7 ms into a 50 ms attack
		const double early = harm.notchDepthForTest();
		run(harm, 1024);
		const double later = harm.notchDepthForTest();
		CAPTURE(early);
		CAPTURE(later);
		CHECK(early > 0.0);
		CHECK(later > early);
		CHECK(later < 0.8);
		run(harm, 48000);
		// SETTLED: Depth times a full envelope.
		CHECK(harm.notchDepthForTest() == doctest::Approx(0.8 * harm.envelopeForTest()).epsilon(1e-3));
		CHECK(harm.notchDepthForTest() > 0.79);

		s.gate = false;
		harm.setTarget(s);
		run(harm, 1024);
		const double releasing = harm.notchDepthForTest();
		CAPTURE(releasing);
		CHECK(releasing > 0.0);
		CHECK(releasing < 0.8);
		run(harm, 96000);
		CHECK(harm.envelopeForTest() == 0.0);
		CHECK(harm.notchDepthForTest() == 0.0);
	}

	SUBCASE("off: today's notch, at its Depth, note or none") {
		Harm harm;
		harm.prepare(kRate);
		harm.setTarget(make(false));
		run(harm, 4096);
		CHECK(harm.envelopeForTest() == 0.0);
		CHECK(harm.notchDepthForTest() == doctest::Approx(0.8));
	}
}

TEST_CASE("Depth Follows Env is a stored control, off by default, and round-trips through the row") {
	CHECK_FALSE(Settings {}.depthFollowsEnv);
	State s;
	CHECK(valueOf(s, Param::DepthFollowsEnv) == 0.0f);
	setValueOf(&s, Param::DepthFollowsEnv, 1.0f);
	CHECK(s.settings.depthFollowsEnv);
	CHECK(stateOf(rowOf(s), static_cast<size_t>(Param::kCount)).settings.depthFollowsEnv);
}

//------------------------------------------------------------------------------------------------//
// NL: THE NOTCH / HIGH-PASS SWITCH.                                                                //
//------------------------------------------------------------------------------------------------//

namespace {

/// A HARM BUILT AND DRIVEN THROUGH ITS OPS TABLE — the road a host takes, so a switch that only reached
/// `Settings` and not the row would show here. Level 0, so what comes out is the filter alone.
struct OpsHarm {
	void* mem = nullptr;
	explicit OpsHarm(bool highpass, float note) {
		mem = ::operator new(kAudioOps.stateBytes, std::align_val_t(kAudioOps.stateAlign));
		kAudioOps.construct(mem);
		kAudioOps.prepare(mem, 48000.0, 512);
		setMode(highpass);
		feathers::device::NoteContext n;
		n.pitchSemitones = note;
		n.gate = true;
		kAudioOps.setNoteContext(mem, n);
	}
	~OpsHarm() {
		kAudioOps.destruct(mem);
		::operator delete(mem, std::align_val_t(kAudioOps.stateAlign));
	}
	OpsHarm(const OpsHarm&) = delete;
	OpsHarm& operator=(const OpsHarm&) = delete;
	void setMode(bool highpass) {
		State st;
		st.settings.level = 0.0f;
		st.settings.highpass = highpass;
		const Row row = rowOf(st);
		kAudioOps.setFields(mem, row.data(), row.size());
	}
	/// The amplitude a sine at `freq` comes out at: three seconds to settle, then a quadrature correlation over a
	/// whole number of periods.
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
					acc += static_cast<double>(l[static_cast<size_t>(i)])
					       * std::exp(std::complex<double>(0.0, -w * static_cast<double>(at + i)));
				}
			}
		}
		return 2.0 * std::abs(acc) / static_cast<double>(span);
	}
};

double dbOf(double x) { return 20.0 * std::log10(x); }

} // namespace

TEST_CASE("NL: Harm's Mode switch reaches the engine — High-pass removes the tracked harmonic and everything below it, Notch only the harmonic") {
	// A2, 110 Hz, at the unison harmonic: f0 is the note.
	constexpr float kNote = 45.0f;
	const double f0 = 110.0;
	SUBCASE("High-pass") {
		OpsHarm h(true, kNote);
		CHECK(opsTargetForTest(h.mem).highpass);
		(void)h.gainAt(f0); // run it, so the grid has ticked and the brickwall is tracking
		// THE VALUE: the brickwall is running, at the note's frequency.
		CHECK(opsHighpassCornerForTest(h.mem) == doctest::Approx(f0).epsilon(1e-9));
		CHECK(opsNotchDepthForTest(h.mem) == 0.0);
		// AND WHAT IT DOES, measured: the fundamental and an octave below at least 60 dB down, the octave above
		// within the 0.1 dB ripple.
		CHECK(dbOf(h.gainAt(f0)) <= -60.0);
		CHECK(dbOf(h.gainAt(0.5 * f0)) <= -60.0);
		const double octave = dbOf(h.gainAt(2.0 * f0));
		CAPTURE(octave);
		CHECK(octave >= -0.11);
		CHECK(octave <= 0.01);
	}
	SUBCASE("Notch") {
		OpsHarm h(false, kNote);
		CHECK_FALSE(opsTargetForTest(h.mem).highpass);
		(void)h.gainAt(f0);
		CHECK(opsHighpassCornerForTest(h.mem) == 0.0);
		CHECK(opsNotchDepthForTest(h.mem) == doctest::Approx(1.0));
		// THE DIFFERENCE BETWEEN THE TWO: the notch takes the harmonic and leaves the octave below it.
		CHECK(dbOf(h.gainAt(f0)) <= -40.0);
		CHECK(dbOf(h.gainAt(0.5 * f0)) >= -0.5);
	}
	SUBCASE("switching back and forth through the row follows, and the corner follows the note") {
		OpsHarm h(false, kNote);
		(void)h.gainAt(f0);
		CHECK(opsHighpassCornerForTest(h.mem) == 0.0);
		h.setMode(true);
		(void)h.gainAt(f0);
		CHECK(opsHighpassCornerForTest(h.mem) == doctest::Approx(f0).epsilon(1e-9));
		feathers::device::NoteContext n;
		n.pitchSemitones = kNote + 12.0f;
		n.gate = true;
		kAudioOps.setNoteContext(h.mem, n);
		CHECK(dbOf(h.gainAt(2.0 * f0)) <= -60.0); // the new fundamental, 220 Hz, is now the one removed
		CHECK(opsHighpassCornerForTest(h.mem) == doctest::Approx(2.0 * f0).epsilon(1e-9));
		h.setMode(false);
		(void)h.gainAt(f0);
		CHECK(opsHighpassCornerForTest(h.mem) == 0.0);
	}
}

TEST_CASE("NL: Harm's High-pass tracks the selected harmonic, and the picture draws the brickwall on the ratio axis") {
	Settings s;
	s.highpass = true;
	s.harmonic = 19; // 2:1
	REQUIRE(ratioAt(s.harmonic) == doctest::Approx(2.0));
	s.noteSemitones = 57.0f; // A3, 220 Hz
	// The picture: at the selected harmonic and below it, at least 60 dB down; an octave above it, within ripple.
	CHECK(dbOf(notchMagnitudeAtRatio(s, 2.0)) <= -60.0);
	CHECK(dbOf(notchMagnitudeAtRatio(s, 1.0)) <= -60.0);
	CHECK(dbOf(notchMagnitudeAtRatio(s, 4.0)) >= -0.1 - 1e-9);
	// Width and Depth do not reach it: the same curve at any of their settings.
	Settings narrow = s;
	narrow.width = 0.0f;
	narrow.depth = 0.0f;
	for (const double ratio : { 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0 }) {
		CHECK(notchMagnitudeAtRatio(narrow, ratio) == notchMagnitudeAtRatio(s, ratio));
	}
	// And the DSP: a HARM in High-pass on the 2:1 harmonic tracks 440 Hz.
	Harm h;
	h.prepare(48000.0);
	s.level = 0.0f;
	s.gate = true;
	h.setTarget(s);
	std::array<float, 64> l {}, r {};
	h.processStereo(l.data(), r.data(), 64);
	CHECK(h.highpassCornerForTest() == doctest::Approx(440.0).epsilon(1e-9));
}

TEST_CASE("NL: Mode is a stored control, Notch by default, stored by label, and round-trips through the row") {
	const auto& d = kParamDescs[static_cast<size_t>(Param::Mode)];
	CHECK(std::string(d.key) == "harm_mode");
	CHECK(d.unit == feathers::device::Unit::Choice);
	CHECK(d.def == 0.0);
	CHECK_FALSE(d.modulatable);
	CHECK(std::string(d.names[0]) == "Notch");
	CHECK(std::string(d.names[1]) == "High-pass");
	State st;
	CHECK_FALSE(st.settings.highpass);
	setValueOf(&st, Param::Mode, 1.0f);
	CHECK(st.settings.highpass);
	CHECK(valueOf(st, Param::Mode) == 1.0f);
	CHECK(stateOf(rowOf(st), static_cast<size_t>(Param::kCount)).settings.highpass);
}
