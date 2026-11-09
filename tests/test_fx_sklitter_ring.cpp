/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/ring.h"

using namespace feathers::fx::sklitter;

TEST_CASE("capacity comes from the HOST sample rate, not from 44100") {
	// `kLooperBufferSize = 44100 * 4` is the ONLY hard-coded sample rate in the whole reference --
	// grepped, not assumed. A plugin runs at whatever the host says, and four seconds has to stay
	// four seconds.
	// **DERIVED FROM `kLooperSeconds`, NOT RE-PINNED AS A LITERAL.** The claim is that capacity follows
	// the HOST RATE; three hand-written products tested that AND the particular number of seconds, so
	// when the seconds changed for `Param::BufferLength` all three went red about the wrong thing.
	// Written as the arithmetic, this case now fails only if the rate stops being a factor.
	const auto want = [](double rate) { return static_cast<size_t>(rate * kLooperSeconds); };
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	CHECK(r.capacity() == want(48000.0));

	Ring r96;
	r96.prepare(96000.0, kLooperSeconds);
	CHECK(r96.capacity() == want(96000.0));

	Ring r441;
	r441.prepare(44100.0, kLooperSeconds);
	CHECK(r441.capacity() == want(44100.0));
	// AND THE THREE ARE NOT THE SAME NUMBER, which is what makes the check above non-vacuous.
	CHECK(r.capacity() != r96.capacity());
	CHECK(r.capacity() != r441.capacity());
}

TEST_CASE("a redundant prepare preserves the captured audio and the ring's position") {
	// **THIS CASE USED TO ASSERT POINTER STABILITY AND WAS VACUOUS.** A known-bad that deleted the
	// early-return guard entirely left it GREEN, which is how the emptiness surfaced: `data_.assign(n,
	// 0.0f)` on a vector that ALREADY holds exactly n reuses its buffer, so `dataForTest()` returns
	// the same address with or without the guard. The assertion was true for a reason that had
	// nothing to do with what it claimed to test -- and it failed in the SAFE direction, reading as a
	// passing guarantee.
	//
	// SO WHAT DOES THE GUARD ACTUALLY BUY? Not a malloc: `assign` at the same size does not allocate.
	// It buys (a) not zeroing up to 1.5 MB on a redundant activate, and (b) not resetting the ring's
	// length, write position and filled flag out from under a buffer that already holds audio. Both
	// are observable, so both are asserted -- and the known-bad now goes red.
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	r.setLength(4);
	for (int i = 0; i < 6; ++i) {
		r.write(static_cast<float>(i + 1), static_cast<float>(i + 1));
	}
	REQUIRE(r.filled() == true);
	REQUIRE(r.writePos() == 2u);

	r.prepare(48000.0, kLooperSeconds); // the same rate: must be a no-op

	CHECK(r.capacity() == static_cast<size_t>(48000.0 * kLooperSeconds));
	CHECK(r.length() == 4u);      // not reset to capacity
	CHECK(r.writePos() == 2u);    // not rewound
	CHECK(r.filled() == true);    // not forgotten
	float l = 0.0f;
	float rr = 0.0f;
	r.readAt(2, &l, &rr);
	CHECK(l == 3.0f);             // the audio is still there
	CHECK(rr == 3.0f);

	// A DIFFERENT rate must resize -- that is still activate-time, and refusing would leave the ring
	// the wrong length for the session.
	r.prepare(96000.0, kLooperSeconds);
	CHECK(r.capacity() == static_cast<size_t>(96000.0 * kLooperSeconds));
	CHECK(r.length() == r.capacity()); // a resize DOES reset the logical length
	CHECK(r.filled() == false);
}

TEST_CASE("the logical length is free to change; the allocation is not") {
	// This replaces allocating a rate-sized buffer when the gesture begins -- which would ALLOCATE, at
	// gesture time, on the audio thread. We allocate the worst case once and move
	// a length within it.
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	const float* before = r.dataForTest();

	r.setLength(1000);
	CHECK(r.length() == 1000u);
	CHECK(r.dataForTest() == before); // no reallocation

	r.setLength(999999);
	CHECK(r.length() == r.capacity()); // clamped, never past the end
	CHECK(r.dataForTest() == before);

	r.setLength(0);
	CHECK(r.length() == 1u); // a zero-length ring would divide by zero on every wrap
}

TEST_CASE("writes wrap at the LOGICAL length and the oldest frames are the ones lost") {
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	r.setLength(4);

	// Write six frames into a ring of four. Frames 0 and 1 must be gone, 2..5 must remain, and they
	// must be readable AT THE RIGHT OFFSETS -- "something is in there" is not the claim.
	for (int i = 0; i < 6; ++i) {
		r.write(static_cast<float>(i), static_cast<float>(-i));
	}
	CHECK(r.writePos() == 2u); // 6 % 4
	CHECK(r.filled() == true);

	float l = 0.0f;
	float rr = 0.0f;
	r.readAt(0, &l, &rr);
	CHECK(l == 4.0f); // slot 0 was overwritten by frame 4
	CHECK(rr == -4.0f);
	r.readAt(1, &l, &rr);
	CHECK(l == 5.0f);
	r.readAt(2, &l, &rr);
	CHECK(l == 2.0f); // still the original frame 2
	r.readAt(3, &l, &rr);
	CHECK(l == 3.0f);
}

TEST_CASE("readAt wraps its index rather than reading past the end") {
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	r.setLength(4);
	for (int i = 0; i < 4; ++i) {
		r.write(static_cast<float>(i), 0.0f);
	}
	float l = 0.0f;
	float rr = 0.0f;
	r.readAt(4, &l, &rr);
	CHECK(l == 0.0f); // 4 % 4
	r.readAt(7, &l, &rr);
	CHECK(l == 3.0f);
	r.readAt(400, &l, &rr);
	CHECK(l == 0.0f);
}

TEST_CASE("filled() is false until the ring has actually wrapped") {
	// Sklitter reads a captured region; reading one that has never been written hands back silence
	// pretending to be audio. `looperBufferFull` exists in the reference for exactly this.
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	r.setLength(3);
	CHECK(r.filled() == false);
	r.write(1.0f, 1.0f);
	CHECK(r.filled() == false);
	r.write(1.0f, 1.0f);
	CHECK(r.filled() == false);
	r.write(1.0f, 1.0f); // the third write wraps the write position back to 0
	CHECK(r.filled() == true);
}

TEST_CASE("reset zeroes the audio and the state, and keeps the memory") {
	Ring r;
	r.prepare(48000.0, kLooperSeconds);
	const float* before = r.dataForTest();
	r.setLength(4);
	for (int i = 0; i < 6; ++i) {
		r.write(1.0f, 1.0f);
	}
	REQUIRE(r.filled() == true);

	r.reset();
	CHECK(r.dataForTest() == before); // NOT freed -- reset happens on the audio thread
	CHECK(r.capacity() == static_cast<size_t>(48000.0 * kLooperSeconds));
	CHECK(r.writePos() == 0u);
	CHECK(r.filled() == false);
	float l = 1.0f;
	float rr = 1.0f;
	r.readAt(0, &l, &rr);
	CHECK(l == 0.0f);
	CHECK(rr == 0.0f);

	// reset must NOT clear the logical length: it is a configuration, not a running value, and
	// zeroing it here would leave a ring that divides by zero on its next write.
	CHECK(r.length() == 4u);
}

TEST_CASE("an unprepared ring is inert rather than a crash") {
	// A device can be asked to process before activate in a badly behaved host. Every accessor must
	// survive it.
	Ring r;
	CHECK(r.capacity() == 0u);
	CHECK(r.filled() == false);
	r.setLength(100);
	r.write(1.0f, 1.0f);
	float l = 7.0f;
	float rr = 7.0f;
	r.readAt(0, &l, &rr);
	CHECK(l == 0.0f); // silence, not a read of nothing
	CHECK(rr == 0.0f);
	r.reset();
}

TEST_CASE("the three rings Sklitter needs are sized from the reference") {
	// **THE LOOPER RING IS A DELIBERATE DIVERGENCE NOW, AND IT IS THE ONLY ONE IN THIS CASE.**
	//
	// The reference's `kLooperBufferSize = 44100 * 4` is four seconds because its looper captures ONE
	// BAR and never more; at 60 BPM a bar is four seconds exactly, so four is the reference's own
	// worst case. `Param::BufferLength` (the ladder 1/4, 1/2, 1, 2, 4 bars) lets a
	// looper capture up to FOUR bars, so the worst case is four times the reference's — sixteen
	// seconds, which holds a four-bar buffer down to 60 BPM and clamps below that.
	//
	// **IT IS MEMORY, AND THE NUMBER IS WORTH SAYING OUT LOUD**: stereo floats at 48 kHz is ~6 MB per
	// SKLITTER instance, ~12 MB at 96 kHz, allocated in `prepare` and never on the audio thread.
	//
	// The delay ring below is UNCHANGED and still pinned to the reference's arithmetic, which is what
	// keeps this case a parity check rather than a record of our own constants.
	CHECK(kLooperSeconds == doctest::Approx(16.0));
	// FOUR BARS AT THE SLOWEST TEMPO THE LADDER'S TOP RUNG SURVIVES — 60 BPM, four beats to the bar.
	CHECK(kLooperSeconds == doctest::Approx(4.0 * (4.0 * 60.0 / 60.0)));
	// kDelayBufferSize = 32768 at 44.1kHz -> the reference calls it "~0.74s". Stated as the
	// arithmetic so the number is checkable rather than copied.
	CHECK(kDelaySeconds == doctest::Approx(32768.0 / 44100.0).epsilon(1e-6));
	CHECK(kDelaySeconds == doctest::Approx(0.743).epsilon(1e-3));

	Ring delay;
	delay.prepare(44100.0, kDelaySeconds);
	CHECK(delay.capacity() == 32768u); // exactly the reference's, at the reference's rate
}
