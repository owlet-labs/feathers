/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/waypoint/waypoint_types.h"

#include <array>
#include <cstdint>
#include <vector>
#include <cmath>
#include <cstddef>
#include <utility>

namespace wp = feathers::fx::waypoint;
namespace flt = feathers::fx::filter;

namespace {

/// A DEVICE WITH FOUR DISTINCT WAYPOINTS, an octave apart, so every landing below is a different
/// number and no assertion can pass by two waypoints happening to agree.
wp::Settings fourPoints() {
	wp::Settings s;
	s.enabled = true;
	s.type = flt::Type::Lowpass;
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		// 250, 500, 1000, 2000 Hz — chosen so the midpoints are exact powers of two and a geometric
		// mean is checkable by eye as well as by assertion.
		s.point[i].cutoffLog2Hz = static_cast<float>(std::log2(250.0 * std::pow(2.0, i)));
		s.point[i].resonance = 0.1f * static_cast<float>(i + 1);
		s.point[i].macro1 = 0.2f * static_cast<float>(i);
		s.point[i].macro2 = 0.05f * static_cast<float>(i);
	}
	return s;
}

double hzOf(const flt::Settings& s) { return std::pow(2.0, static_cast<double>(s.cutoffLog2Hz)); }

} // namespace

TEST_CASE("the morph walks the waypoints BY INDEX, in equal thirds") {
	// A POLYLINE, FOR NOW. Three segments across four waypoints, so the landings
	// are at 0, 1/3, 2/3 and 1 — and they are landings ON the waypoints, not positions along a line
	// whose dots may sit anywhere. See the plan: arc length is what this is deliberately not.
	const wp::Settings s = fourPoints();

	// EXACT LANDINGS, asserted as VALUES rather than as "it moved". Each is a different frequency, so
	// a lookup that returned the wrong waypoint could not pass by coincidence.
	CHECK(hzOf(wp::settingsAtMorph(s, 0.0)) == doctest::Approx(250.0));
	CHECK(hzOf(wp::settingsAtMorph(s, 1.0 / 3.0)) == doctest::Approx(500.0));
	CHECK(hzOf(wp::settingsAtMorph(s, 2.0 / 3.0)) == doctest::Approx(1000.0));
	CHECK(hzOf(wp::settingsAtMorph(s, 1.0)) == doctest::Approx(2000.0));

	// THE FENCEPOST, WHICH THIS SHAPE ALWAYS HAS. morph == 1.0 must land ON the last waypoint rather
	// than index one past the end; the segment is floored and then clamped for exactly this.
	CHECK(wp::settingsAtMorph(s, 1.0).resonance == doctest::Approx(s.point[3].resonance));

	// AND THE ASSERTION ABOVE CANNOT SEE THE BUG IT LOOKS LIKE IT IS ABOUT — MEASURED, not argued.
	// Deleting the clamp makes the segment 3, so `point[seg + 1]` reads ONE PAST THE END; but the
	// fraction at morph == 1.0 is 0, so the garbage endpoint is multiplied by zero and the returned
	// Settings is still right. A known-bad that removed the clamp came back GREEN against every
	// assertion in this file.
	//
	// SO THE CLAIM IS MADE ABOUT THE NUMBER. `morphAt` exists for this: the out-of-bounds read is
	// undefined behaviour and unobservable from the output, and the only honest guard is the index
	// itself.
	CHECK(wp::morphAt(1.0).segment == wp::kWaypoints - 2);
	CHECK(wp::morphAt(1.0).fraction == doctest::Approx(1.0));
	// EVERY SEGMENT IS IN RANGE for the whole domain, including past the ends.
	for (const double m : { -1.0, 0.0, 0.2, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.99, 1.0, 3.0 }) {
		CAPTURE(m);
		CHECK(wp::morphAt(m).segment <= wp::kWaypoints - 2);
	}

	SUBCASE("and out-of-range morph is clamped rather than extrapolated") {
		// A ROUTE CAN PUSH A DESTINATION PAST ITS RANGE — kMaxAmount is 2.0 — so this is a reachable
		// input, not a defensive nicety.
		CHECK(hzOf(wp::settingsAtMorph(s, -0.5)) == doctest::Approx(250.0));
		CHECK(hzOf(wp::settingsAtMorph(s, 1.7)) == doctest::Approx(2000.0));
	}
}

TEST_CASE("the morph interpolates OCTAVES, not hertz") {
	// THE CLAIM THIS DEVICE RESTS ON, and the one a "the cutoff moved" check cannot see.
	// `cutoffLog2Hz` is already logarithmic, so a straight lerp of it IS octave interpolation. Lerp
	// hertz instead and every intermediate corner crowds toward the bottom of the sweep — audible
	// precisely where a filter sweep is most audible.
	const wp::Settings s = fourPoints();

	// HALFWAY THROUGH THE FIRST SEGMENT: 250 Hz to 500 Hz.
	const double mid = hzOf(wp::settingsAtMorph(s, 1.0 / 6.0));

	// THE GEOMETRIC MEAN, which is the value the log road produces...
	CHECK(mid == doctest::Approx(std::sqrt(250.0 * 500.0)).epsilon(0.001)); // ≈ 353.55
	// ...AND EXPLICITLY NOT THE ARITHMETIC ONE, which is what lerping hertz would give. Stated as its
	// own assertion because the two are only 6% apart here: an epsilon loose enough to hide the
	// difference would hide the bug, and this says which number is wrong rather than only that the
	// right one is right.
	CHECK(mid != doctest::Approx(375.0).epsilon(0.001));
	CHECK(mid < 375.0);

	SUBCASE("resonance and the macros are linear, because they are not logarithmic quantities") {
		// NOT EVERYTHING IS A LOG. The same lerp is correct for these and wrong for cutoff, which is
		// why the units are the device's own rather than one rule applied to every field.
		const flt::Settings half = wp::settingsAtMorph(s, 1.0 / 6.0);
		CHECK(half.resonance
		      == doctest::Approx(0.5f * (s.point[0].resonance + s.point[1].resonance)));
		CHECK(half.macro1 == doctest::Approx(0.5f * (s.point[0].macro1 + s.point[1].macro1)));
	}
}

TEST_CASE("the type is shared, so a morph never changes type mid-sweep") {
	// THE TYPE IS CONSTANT FOR ALL MORPH POINTS, by design. This is what removes the whole DSP risk
	// the roadmap flagged: with one type there is no cross-type blend anywhere, so `Bell` is an
	// ordinary choice here rather than the barred morph partner it is on the multimode filter.
	wp::Settings s = fourPoints();
	s.type = flt::Type::Bell;
	for (const double m : { 0.0, 0.17, 1.0 / 3.0, 0.5, 2.0 / 3.0, 0.9, 1.0 }) {
		CAPTURE(m);
		CHECK(wp::settingsAtMorph(s, m).type == flt::Type::Bell);
	}
	// AND THE CORNER STILL MOVES while the type does not — or the assertion above is satisfied by a
	// function that returns a constant.
	CHECK(hzOf(wp::settingsAtMorph(s, 0.0)) != doctest::Approx(hzOf(wp::settingsAtMorph(s, 1.0))));
}

TEST_CASE("a row that is short reads its missing fields at their DECLARED defaults, not at zero") {
	// device_row.h's rule, and the reason `stateOf` takes a count. A patch written by a build with
	// fewer waypoint fields must not read them as zero: zero cutoff is log2(1 Hz), which is not a
	// filter anybody asked for.
	wp::Row row {};
	row[static_cast<size_t>(wp::Param::Enabled)] = 1.0f;

	// NOTHING PRESENT PAST THE FIRST THREE CONTROLS.
	const wp::Settings s = wp::stateOf(row, 3);
	const double def = wp::kParamDescs[static_cast<size_t>(wp::Param::Cutoff0)].def;
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		CAPTURE(i);
		CHECK(s.point[i].cutoffLog2Hz == doctest::Approx(def));
	}
	// AND THE DEFAULT IS NOT ZERO — or the loop above is satisfied by the zeroed row it was handed,
	// which is precisely the failure this rule exists to prevent.
	CHECK(def > 1.0);
}

TEST_CASE("a Settings survives a round trip through its Row") {
	// THE ROW IS WHAT A PATCH STORES, so a field that rowOf writes and stateOf does not read is a
	// setting that silently resets on load.
	const wp::Settings before = fourPoints();
	const wp::Row row = wp::rowOf(before);
	const wp::Settings after = wp::stateOf(row, row.size());
	CHECK(after == before);
}

//------------------------------------------------------------------------------------------------//
// PHASE 2 — THE RENDER PATH                                                                        //
//------------------------------------------------------------------------------------------------//

// EVERY CLAIM BELOW IS ABOUT A TARGET VALUE, READ THROUGH THE FILTER, NOT ABOUT A BUFFER.
//
// A FILTER'S CORNER IS NOT RECOVERABLE FROM ITS OUTPUT — this repo found that independently twice in
// one week and grew `voiceFilterTargetForTest` for it. So a case asking "did the morph move the
// corner" by comparing two renders would pass whenever ANYTHING differed, which is exactly the
// partial-effect blindness these rules exist for. `MultimodeFilter::target()` is the seam, and the
// audio check below it is the SECOND assertion proving the target really reaches the DSP.

namespace {

struct Ops {
	// SIZED FROM THE DEVICE'S OWN DECLARATION, NOT FROM A LITERAL.
	//
	// THIS WAS `std::array<std::byte, 4096>` AND THE MERGE BROKE IT. Waypoint embeds a
	// `MultimodeFilter`, and the comb type that landed the same evening put an
	// `SincDelayLine<4096>` inside one -- so this device's `stateBytes` went from under 4096 to
	// 34416 without a line of Waypoint changing. Neither branch could see it: comb was written
	// before Waypoint merged and Waypoint before comb did.
	//
	// A BUFFER THAT ASKS `stateBytes` CANNOT GO STALE THAT WAY, and asking is also what the runner
	// does -- `DeviceInstance` allocates exactly `stateBytes` aligned to `stateAlign`, so a fixture
	// that hardcodes anything is testing a different arrangement from the one that ships.
	std::vector<std::byte> storage = std::vector<std::byte>(wp::kAudioOps.stateBytes);

	Ops() {
		REQUIRE(wp::kAudioOps.stateBytes <= storage.size());
		REQUIRE(reinterpret_cast<std::uintptr_t>(storage.data()) % wp::kAudioOps.stateAlign == 0);
		wp::kAudioOps.construct(storage.data());
		wp::kAudioOps.prepare(storage.data(), 48000.0, 4096);
	}
	~Ops() { wp::kAudioOps.destruct(storage.data()); }
	Ops(const Ops&) = delete;
	Ops& operator=(const Ops&) = delete;

	void fields(const wp::Settings& s) {
		const wp::Row row = wp::rowOf(s);
		wp::kAudioOps.setFields(storage.data(), row.data(), row.size());
	}
	void note(float pitch) {
		REQUIRE(wp::kAudioOps.setNoteContext != nullptr);
		wp::kAudioOps.setNoteContext(storage.data(), feathers::device::NoteContext { pitch, true });
	}
	/// THE TARGET THE DSP WAS ACTUALLY HANDED — the seam this whole section rests on.
	[[nodiscard]] const flt::Settings& target() const { return wp::targetForTest(storage.data()); }
};

} // namespace

TEST_CASE("the render path targets the interpolated state, and the morph moves it") {
	Ops ops;
	wp::Settings s = fourPoints();
	s.enabled = true;

	// EACH LANDING, AS A VALUE. Four different corners, so a wrong waypoint cannot pass by accident.
	for (const auto& [morph, hz] : std::initializer_list<std::pair<float, double>> {
	         { 0.0f, 250.0 }, { 1.0f / 3.0f, 500.0 }, { 2.0f / 3.0f, 1000.0 }, { 1.0f, 2000.0 } }) {
		CAPTURE(morph);
		s.morph = morph;
		ops.fields(s);
		CHECK(std::pow(2.0, static_cast<double>(ops.target().cutoffLog2Hz))
		      == doctest::Approx(hz).epsilon(0.001));
	}

	SUBCASE("and the target really reaches the audio, which the assertions above cannot say") {
		// THE NON-VACUITY HALF, AND THE ONLY THING A BUFFER IS ASKED HERE. Not "what is the corner" —
		// that is unrecoverable — but "does a corner this far apart change the output at all". A
		// target set on a filter nothing drives would pass every assertion above.
		s.morph = 0.0f;
		ops.fields(s);
		std::array<float, 256> lowL {};
		std::array<float, 256> lowR {};
		for (size_t i = 0; i < lowL.size(); ++i) {
			lowL[i] = lowR[i] = (i % 2 == 0) ? 0.5f : -0.5f; // Nyquist-ish: a lowpass must eat it
		}
		wp::kAudioOps.processStereo(ops.storage.data(), lowL.data(), lowR.data(),
		                            static_cast<int>(lowL.size()));
		double sum = 0.0;
		for (const float v : lowL) {
			sum += std::abs(static_cast<double>(v));
		}
		CHECK(sum < 0.5 * static_cast<double>(lowL.size())); // it was filtered, not passed through
	}
}

TEST_CASE("keytrack is an OFFSET, per voice and on the bus alike") {
	// KEYTRACK IS AN OPTION, AS AN OFFSET: the filter adds keyOffsetSemitones/12 to cutoffLog2Hz, the same log domain the waypoints
	// live in, so the note shifts the INTERPOLATED corner and the whole morph path transposes.
	Ops ops;
	wp::Settings s = fourPoints();
	s.enabled = true;
	s.morph = 0.0f; // sitting on waypoint 1, 250 Hz

	SUBCASE("off: the note changes nothing") {
		s.keytrack = false;
		s.poly = true;
		ops.fields(s);
		ops.note(72.0f);
		CHECK(ops.target().keyOffsetSemitones == doctest::Approx(0.0f));
	}

	SUBCASE("on, with poly: the offset is exactly the distance from the reference key") {
		s.keytrack = true;
		s.poly = true;
		ops.fields(s);
		ops.note(72.0f); // an octave above middle C
		CHECK(ops.target().keyOffsetSemitones
		      == doctest::Approx(72.0f - static_cast<float>(flt::kKeytrackReferenceKey)));
		// AND IT IS A TRANSPOSITION OF THE WHOLE PATH, not a change to any one waypoint: the stored
		// cutoff is untouched and the offset carries the note. Asserted because the alternative
		// implementation — folding the note into each waypoint before interpolating — would give the
		// same audible answer today and a different structure, and this pins which one is here.
		CHECK(ops.target().cutoffLog2Hz
		      == doctest::Approx(s.point[0].cutoffLog2Hz)); // unshifted; the offset is separate
	}

	SUBCASE("on, with poly OFF: the bus instance follows the host's one note") {
		// IT USED TO RENDER AS POLY OFF AND NOTHING ELSE — "a corner cannot follow the note on a bus
		// carrying six of them". A host hands the bus ONE note (the rack its last held key, Feathers its
		// newest held voice), and the rack plugin has no voices, so that rule made keytrack inert there
		// (2026-10-04). Same offset as per voice: the distance from the reference key.
		s.keytrack = true;
		s.poly = false;
		ops.fields(s);
		ops.note(72.0f);
		CHECK(ops.target().keyOffsetSemitones
		      == doctest::Approx(72.0f - static_cast<float>(flt::kKeytrackReferenceKey)));
	}

	SUBCASE("the note road and the fields road are order-independent") {
		// registry.h REQUIRES IT: a host may deliver them in either order and delivers fields far less
		// often. Each road latches its own input and re-derives, so both orders must land identically
		// — and the failure mode is silent, which is why it is asserted rather than assumed.
		s.keytrack = true;
		s.poly = true;

		Ops fieldsFirst;
		fieldsFirst.fields(s);
		fieldsFirst.note(84.0f);

		Ops noteFirst;
		noteFirst.note(84.0f);
		noteFirst.fields(s);

		CHECK(fieldsFirst.target().keyOffsetSemitones
		      == doctest::Approx(noteFirst.target().keyOffsetSemitones));
		CHECK(fieldsFirst.target().cutoffLog2Hz == doctest::Approx(noteFirst.target().cutoffLog2Hz));
		// NOT VACUOUS: the offset is really non-zero, or both sides agree on nothing happening.
		CHECK(fieldsFirst.target().keyOffsetSemitones > 1.0f);
	}
}
