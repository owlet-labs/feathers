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

#include "mod/noteexpr/noteexpr.h"
#include "mod/noteexpr/noteexpr_types.h"

#include <array>
#include <vector>

// THE NOTE-EXPRESSION MODULATOR, with no host on it — notes and expressions go in as `NoteEvent`s and
// the published sources come out.
//
// **THE DEVICE IS TESTED HERE AND ITS PLUMBING IS TESTED IN `test_synth.cpp`**, which is the split the
// Tracker already follows: this file is about the RULE and the stack, and the synth's cases are about
// an expression reaching a voice through the lane. Neither can stand in for the other — a device that
// tracked perfectly and was never offered an event would pass everything below.

using namespace feathers;
namespace nx = feathers::mod::noteexpr;

namespace {

device::NoteEvent on(int32_t noteId, int16_t key) {
	device::NoteEvent e {};
	e.kind = device::NoteEvent::Kind::On;
	e.noteId = noteId;
	e.channel = 0;
	e.key = key;
	return e;
}

device::NoteEvent off(int32_t noteId, int16_t key) {
	device::NoteEvent e {};
	e.kind = device::NoteEvent::Kind::Off;
	e.noteId = noteId;
	e.channel = 0;
	e.key = key;
	return e;
}

device::NoteEvent expr(int32_t noteId, int16_t key, int32_t id, double value) {
	device::NoteEvent e {};
	e.kind = device::NoteEvent::Kind::Expression;
	e.noteId = noteId;
	e.channel = 0;
	e.key = key;
	e.expressionId = id;
	e.expressionValue = value;
	return e;
}

/// FEED THE DEVICE AND RETURN WHAT LEFT IT — the lane's shape, so a swallowed event is visible.
std::vector<device::NoteEvent> feed(nx::NoteExpressions& d,
                                    const std::vector<device::NoteEvent>& in) {
	std::array<device::NoteEvent, 64> out {};
	size_t produced = 0;
	d.processNotes(in.data(), in.size(), out.data(), out.size(), &produced);
	return std::vector<device::NoteEvent>(out.begin(), out.begin() + static_cast<long>(produced));
}

void setRule(nx::NoteExpressions& d, nx::Rule rule) {
	const std::array<float, 2> fields { static_cast<float>(static_cast<int>(rule)), 1.0f };
	d.setFields(fields.data(), fields.size());
}

constexpr size_t kPressure = 6; // CLAP_NOTE_EXPRESSION_PRESSURE, and the output index is the id
constexpr size_t kPan = 1;
constexpr size_t kVolume = 0;

} // namespace

TEST_CASE("an untouched note publishes each expression's NEUTRAL, which is not always zero") {
	// **THE ONE THAT WOULD HAVE SHIPPED WRONG WITHOUT BEING WRITTEN DOWN.** A host sends an expression
	// only when it CHANGES, so a note nobody has touched carries whatever the device started it at —
	// for the whole note. Starting everything at zero would make a fresh note read as hard-left pan
	// and silent volume the moment a patch routed either, which is the device inventing a gesture.
	nx::NoteExpressions d;
	d.prepare(48000.0);
	feed(d, { on(1, 60) });
	REQUIRE(d.heldCountForTest() == 1);

	CHECK(d.output(kVolume) == doctest::Approx(1.0f));  // a gain, unity
	CHECK(d.output(kPan) == doctest::Approx(0.5f));     // centred
	CHECK(d.output(kPressure) == doctest::Approx(0.0f)); // and the rest do rest at zero
}

TEST_CASE("an expression reaches the note it addresses, and only that note") {
	nx::NoteExpressions d;
	d.prepare(48000.0);
	feed(d, { on(1, 60), on(2, 64) });
	REQUIRE(d.heldCountForTest() == 2);

	// ADDRESSED BY NOTE ID with the other fields at CLAP's wildcard, which is how a host that tracks
	// ids sends them.
	feed(d, { expr(2, -1, static_cast<int32_t>(kPressure), 0.75) });

	setRule(d, nx::Rule::Highest);
	CHECK(d.output(kPressure) == doctest::Approx(0.75f)); // note 2 is key 64
	setRule(d, nx::Rule::Lowest);
	CHECK(d.output(kPressure) == doctest::Approx(0.0f));  // note 1 was never touched
}

TEST_CASE("the RULE decides which held note is published") {
	nx::NoteExpressions d;
	d.prepare(48000.0);
	feed(d, { on(1, 60), on(2, 72), on(3, 64) }); // played low, high, middle — so newest != highest
	REQUIRE(d.heldCountForTest() == 3);
	feed(d, { expr(1, -1, static_cast<int32_t>(kPressure), 0.1),
	          expr(2, -1, static_cast<int32_t>(kPressure), 0.9),
	          expr(3, -1, static_cast<int32_t>(kPressure), 0.5) });

	// **THE THREE-NOTE SHAPE ON PURPOSE.** Every one of these rules agrees on a two-note chord played
	// in ascending order; the middle note is what tells them apart, and a two-note fixture would pass
	// on an implementation that had them all wrong.
	setRule(d, nx::Rule::Last);
	CHECK(d.output(kPressure) == doctest::Approx(0.5f)); // note 3, most recent
	setRule(d, nx::Rule::Lowest);
	CHECK(d.output(kPressure) == doctest::Approx(0.1f)); // key 60
	setRule(d, nx::Rule::Highest);
	CHECK(d.output(kPressure) == doctest::Approx(0.9f)); // key 72
	setRule(d, nx::Rule::Max);
	CHECK(d.output(kPressure) == doctest::Approx(0.9f)); // the largest value, whichever note

	SUBCASE("and Max is PER EXPRESSION, not a note the device picked") {
		// The loudest note need not be the brightest. A Max implemented by selecting one NOTE would
		// answer 0.9's note for both, which is the thing this distinguishes.
		feed(d, { expr(1, -1, static_cast<int32_t>(kPan), 0.8) });
		setRule(d, nx::Rule::Max);
		CHECK(d.output(kPressure) == doctest::Approx(0.9f)); // note 2's
		CHECK(d.output(kPan) == doctest::Approx(0.8f));      // note 1's
	}
}

TEST_CASE("a released note stops contributing, and the stack keeps its recency") {
	// **RECENCY MUST SURVIVE A REMOVAL**, which is why the serial is a field and not the array index:
	// the device removes by swapping the last entry down, so position says nothing about order.
	nx::NoteExpressions d;
	d.prepare(48000.0);
	feed(d, { on(1, 60), on(2, 64), on(3, 67) });
	feed(d, { expr(1, -1, static_cast<int32_t>(kPressure), 0.1),
	          expr(2, -1, static_cast<int32_t>(kPressure), 0.2),
	          expr(3, -1, static_cast<int32_t>(kPressure), 0.3) });
	setRule(d, nx::Rule::Last);
	REQUIRE(d.output(kPressure) == doctest::Approx(0.3f));

	// RELEASE THE NEWEST: the answer falls back to the most recent note STILL HELD, which is note 2 —
	// not note 1, and not "whatever ended up last in the array".
	feed(d, { off(3, 67) });
	CHECK(d.heldCountForTest() == 2);
	CHECK(d.output(kPressure) == doctest::Approx(0.2f));

	SUBCASE("releasing a note that is NOT the newest changes nothing") {
		feed(d, { off(1, 60) });
		CHECK(d.heldCountForTest() == 1);
		CHECK(d.output(kPressure) == doctest::Approx(0.2f));
	}

	SUBCASE("and with nothing held it returns to the neutral") {
		feed(d, { off(1, 60), off(2, 64) });
		REQUIRE(d.heldCountForTest() == 0);
		CHECK(d.output(kPressure) == doctest::Approx(0.0f));
		CHECK(d.output(kPan) == doctest::Approx(0.5f)); // neutral, not zero
	}
}

TEST_CASE("EVERY event passes through, including the ones it acted on") {
	// `NoteOps` states the rule this protects: a dropped note-OFF is a stuck note. This device
	// observes, and it must not be POSSIBLE for it to swallow anything — including an expression it
	// consumed, and an expression id it does not know.
	nx::NoteExpressions d;
	d.prepare(48000.0);
	const std::vector<device::NoteEvent> in {
		on(1, 60),
		expr(1, -1, static_cast<int32_t>(kPressure), 0.5),
		expr(1, -1, 99, 0.5), // an id this build does not know
		off(1, 60),
	};
	const std::vector<device::NoteEvent> out = feed(d, in);
	REQUIRE(out.size() == in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		CAPTURE(i);
		CHECK(out[i].kind == in[i].kind);
		CHECK(out[i].noteId == in[i].noteId);
		CHECK(out[i].expressionId == in[i].expressionId);
		CHECK(out[i].expressionValue == doctest::Approx(in[i].expressionValue));
	}
}

TEST_CASE("a NaN never lands, and an unknown expression id is dropped rather than clamped") {
	nx::NoteExpressions d;
	d.prepare(48000.0);
	feed(d, { on(1, 60) });
	feed(d, { expr(1, -1, static_cast<int32_t>(kPressure), 0.4) });
	REQUIRE(d.output(kPressure) == doctest::Approx(0.4f));

	feed(d, { expr(1, -1, static_cast<int32_t>(kPressure),
	                std::numeric_limits<double>::quiet_NaN()) });
	CHECK(d.output(kPressure) == doctest::Approx(0.4f));

	// AN UNKNOWN ID MUST NOT LAND ON THE LAST OUTPUT, which is where a clamp would put it.
	feed(d, { expr(1, -1, 99, 0.9) });
	CHECK(d.output(kPressure) == doctest::Approx(0.4f));
}

TEST_CASE("bypassed, it publishes the neutral rather than zero") {
	// The enable square means "not contributing". For pan that is 0.5 — a bypassed device that
	// published 0 would swing a routed pan hard left, which is the opposite of not contributing.
	nx::NoteExpressions d;
	d.prepare(48000.0);
	feed(d, { on(1, 60) });
	feed(d, { expr(1, -1, static_cast<int32_t>(kPan), 0.9) });
	REQUIRE(d.output(kPan) == doctest::Approx(0.9f));

	const std::array<float, 2> offFields { 0.0f, 0.0f }; // Rule = Last, Enabled = off
	d.setFields(offFields.data(), offFields.size());
	CHECK(d.output(kPan) == doctest::Approx(0.5f));
	CHECK(d.output(kVolume) == doctest::Approx(1.0f));
}
