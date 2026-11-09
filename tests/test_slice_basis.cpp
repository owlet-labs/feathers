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

#include "dsp/slice_basis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <cstring>

using namespace feathers;

namespace {

/// A PLACE TO STAND while the plane is examined.
///
/// M is a function of the turn AND the home, so every case in this file has to say where it is — a
/// basis with no place is not a thing any more. Named once rather than spelled at twenty sites, so
/// that "the same plane" means the same plane: a case comparing two derivations must not be
/// accidentally comparing two places, which is exactly the mistake a bare literal invites.
constexpr double kSomewhere = 0.37;

} // namespace

TEST_CASE("a sound that has not travelled is exactly where it was, whatever the orientation") {
	// THE DESIGN RULE, and the one the whole shape of this file exists to satisfy: a rotation that
	// changed the sound would be redundant with gamma, so the sound stays the same unless you move in
	// xy or gamma. Home and travel are separate coordinates precisely so that this holds by
	// construction — both travel products are exactly ±0.0, and adding either to a finite value is
	// exact — but the arithmetic that makes it true is easy to break, so it is checked rather than
	// asserted.
	//
	// Note what is NOT quantified over: the orientation. Every one of them, however hard the plane has
	// been turned, returns home to the bit.
	for (float rotate : { -2.0f, -0.35f, 0.0f, 0.45f, 0.9f, 1.2f }) {
		for (int step = 0; step < 9; ++step) {
			const double home = step * 0.237 - 0.5;
			// DERIVED AT THE PLACE IT IS READ AT, which M made possible to get wrong: the plane a sound
			// plays on is the plane its own home gives it, so a case that built one basis and then
			// evaluated it at nine different homes would be asserting the guarantee for eight sounds
			// playing somebody else's plane. It would still pass — the guarantee is about the travel
			// products being zero, whatever the plane — and it would have stopped describing the engine.
			const SliceBasis<kMorphTriangles> basis =
			    sliceBasisFor<kMorphTriangles>(rotate, home);
			for (size_t i = 0; i < kMorphTriangles; ++i) {
				const double got = basis.phaseFor(i, home, 0.0, 0.0);
				CHECK(std::memcmp(&got, &home, sizeof(double)) == 0);
				CHECK(basis.rateFor(i, 0.0, 0.0) == 1.0f);
			}
		}
	}
}

TEST_CASE("with the plane closed, both travel directions are just more gamma") {
	// The degenerate basis is the one-dimensional world v1 lived in, stated exactly: every coefficient
	// 1, so a step in Pos X and a step in Pos Y do the same thing to every triangle, and both of them
	// do what gamma already did. This is the arithmetic the decode has to reduce to, and it is a
	// CONSTRUCTED special case rather than what anything ships with — by design, hardware parity is a
	// test case here, not a default.
	const SliceBasis<kMorphTriangles> basis = degenerateSliceBasis<kMorphTriangles>();

	for (int step = 0; step < 9; ++step) {
		const double home = step * 0.237 - 0.5;
		// Dyadic, so the sum is exact in both float and double and the comparison can be about the
		// decode rather than about rounding.
		const double expected = home + 0.375;
		for (size_t i = 0; i < kMorphTriangles; ++i) {
			const double both = basis.phaseFor(i, home, 0.25, 0.125);
			const double xOnly = basis.phaseFor(i, home, 0.375, 0.0);
			const double yOnly = basis.phaseFor(i, home, 0.0, 0.375);
			CHECK(std::memcmp(&both, &expected, sizeof(double)) == 0);
			CHECK(std::memcmp(&xOnly, &expected, sizeof(double)) == 0);
			CHECK(std::memcmp(&yOnly, &expected, sizeof(double)) == 0);
			CHECK(basis.rateFor(i, 0.25, 0.125) == 1.0f);
		}
	}
}

TEST_CASE("a zero generator is born open — the degenerate plane is a thing you construct on purpose") {
	// RESTATED WHEN M ARRIVED, and by a decision rather than by a convenience. This case used to require
	// that a zero generator reproduce the degenerate basis bit for bit, because the old decode TURNED a
	// degenerate basis by an angle and a zero angle had to be exactly the identity. M has no angle to
	// be zero: every coordinate is a φ triangle read AT the rotate, and rotate 0 is simply one place to
	// read it.
	//
	// THE DECISION IS BORN-OPEN. The alternative — an anchor term making M(0) exactly degenerate —
	// was arithmetically prettier and audibly worse: a sound would have had no second dimension until
	// somebody turned something, so Pos X and Pos Y would both have been more gamma for every patch
	// that had never been rotated. That is the one-dimensional world the slice exists to leave.
	//
	// WHAT THE DEGENERATE PLANE IS NOW: a thing the firmware-parity cases construct deliberately, which
	// is where it always belonged — the record already rules hardware parity a test case here rather
	// than the shipping default.
	// Rotate 0 IS what an unpinned sound plays now — rotateFor gives every pinless sound exactly this.
	const SliceBasis<kMorphTriangles> derived = sliceBasisFor<kMorphTriangles>(0.0, kSomewhere);
	const SliceBasis<kMorphTriangles> closed = degenerateSliceBasis<kMorphTriangles>();

	CHECK(std::memcmp(derived.x.data(), closed.x.data(), sizeof(closed.x)) != 0);
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		INFO("coordinate " << i);
		// The two travel directions lead somewhere different, at every coordinate...
		CHECK(derived.x[i] != derived.y[i]);
		// ...and the rate half is reached, so one region of the plane can be denser than another.
		CHECK(derived.x[kMorphTriangles + i] != 0.0f);
	}
}

TEST_CASE("deriving a basis is pure, so a retrace is exact") {
	// Rewindability lives on this. The generator is stored and the basis recomputed; if derivation
	// were not a pure function of the rotate — if it drifted, or depended on how many times it had
	// been called — then returning to a breadcrumb would land near where you were rather than on it.
	const SliceGenerator generator { 0.37f };

	const SliceBasis<kMorphTriangles> first =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere);
	for (int repeat = 0; repeat < 5; ++repeat) {
		const SliceBasis<kMorphTriangles> again =
		    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere);
		CHECK(std::memcmp(again.x.data(), first.x.data(), sizeof(first.x)) == 0);
		CHECK(std::memcmp(again.y.data(), first.y.data(), sizeof(first.y)) == 0);
	}
}

TEST_CASE("there is no closed plane to escape from: every rotate is already a plane") {
	// THE INVERSE OF THE CONTROL THIS REPLACES, and the inversion is the design change rather than a
	// weakening. The old case pinned down a real trap: a rotation applies the same linear map to both
	// rows, and the degenerate basis has both rows IDENTICAL, so two identical vectors stayed identical
	// however hard they were turned. Something had to prise the plane open first, and that something
	// was the aperture — a second mechanism, with its own weights, existing only because the first one
	// could not do the job.
	//
	// M DOES NOT HAVE THE PROBLEM TO SOLVE. It does not turn a basis; it WRITES one, both rows from
	// independent readings of a φ field, so being open is what the plane is rather than a state it has
	// to be put into. The trap can therefore no longer be sprung, and the case that guarded it becomes
	// the case that says so — checked over a spread of turns, including the ones the old case used.
	// (The old second half — proving the aperture and the extra angles never reach the decode — is now
	// enforced by the type: there is nothing else in a SliceGenerator to leak.)
	for (const float rotate : { -2.3f, -0.8f, 0.0f, 1.1f, 4.7f }) {
		const SliceBasis<kMorphTriangles> basis = sliceBasisFor<kMorphTriangles>(rotate, kSomewhere);

		size_t same = 0;
		for (size_t i = 0; i < 2 * kMorphTriangles; ++i) {
			same += basis.x[i] == basis.y[i] ? 1u : 0u;
		}
		INFO("rotate " << rotate << ": " << same << " of " << 2 * kMorphTriangles
		               << " coordinates where the two travel directions coincide");
		CHECK(same == 0);
	}
}

TEST_CASE("an opened plane is a real plane, not the same direction twice") {
	// The whole point of stage 1. With the plane closed, Pos X and Pos Y are one control wearing two
	// hats — which is the finding that deferred Explore, in its new clothes. Once opened, the two must
	// lead somewhere different, and they must differ in the RATE half too or there is no local
	// density.
	const SliceBasis<kMorphTriangles> basis = sliceBasisFor<kMorphTriangles>(0.6, kSomewhere);

	bool directionsDiffer = false;
	bool rateReached = false;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		if (basis.x[i] != basis.y[i]) {
			directionsDiffer = true;
		}
		if (basis.x[kMorphTriangles + i] != 0.0f || basis.y[kMorphTriangles + i] != 0.0f) {
			rateReached = true;
		}
	}
	CHECK(directionsDiffer);
	CHECK(rateReached);

	// And travelling in Y must not arrive where travelling in X arrives — for SOME triangle. (Under M
	// it is in fact every one — the case above pins that — but this claim is the one stage 1 needs.)
	bool travelDiffers = false;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		if (basis.phaseFor(i, 0.0, 1.0, 0.0) != basis.phaseFor(i, 0.0, 0.0, 1.0)) {
			travelDiffers = true;
		}
	}
	CHECK(travelDiffers);
}

TEST_CASE("a rotation cannot reach a sound at its home") {
	// THE GUARANTEE THAT MAKES ROTATE AN HONEST VERB, and it is the same structure as the home/travel
	// separation rather than a second mechanism: the decode multiplies every basis component by the
	// TRAVEL, so a sound that has not travelled returns its home bit for bit however far the plane is
	// turned. "The held sound sits on the rotation axis" is arithmetic, not arrangement.
	const SliceGenerator generator { 0.7f };

	SliceRotation delta;
	delta.rotate = -0.9f;
	const SliceGenerator turned = rotateBy(generator, delta);

	const SliceBasis<kMorphTriangles> before =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere);
	const SliceBasis<kMorphTriangles> after =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(turned), kSomewhere);

	const double home = 0.371;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		CAPTURE(i);
		// At zero travel, to the bit — not nearly.
		CHECK(before.phaseFor(i, home, 0.0, 0.0) == after.phaseFor(i, home, 0.0, 0.0));
		CHECK(before.rateFor(i, 0.0, 0.0) == after.rateFor(i, 0.0, 0.0));
	}

	// AND IT REACHES EVERYTHING ELSE, or the plane is decoration. A sound that has travelled is out in
	// the plane, and re-aiming the plane is what moves it.
	//
	// NOTE WHAT THIS DOES NOT SAY, since A10 was superseded: a turn of the RAW BASIS moves a drifted
	// sound, and that is exactly the behaviour the fold exists to cancel. The surface never turns a raw
	// basis — see the residue cases below and rotateSoundBy's folding form.
	bool anyMoved = false;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		anyMoved = anyMoved || before.phaseFor(i, home, 0.3, -0.2) != after.phaseFor(i, home, 0.3, -0.2);
	}
	CHECK(anyMoved);
}

TEST_CASE("the residue is exactly nothing until something banks into it") {
	// THE MIGRATION GUARANTEE, at the layer it is made: an empty residue must add no bit anywhere, or
	// every patch in existence changes the day this lands. Not "close" — a patch that has never been
	// rotated at a drift has to build the character it always built.
	const SliceGenerator generator { -1.1f };
	const SliceBasis<kMorphTriangles> plain =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere);
	const SliceBasis<kMorphTriangles> framed = sliceFrameFor<kMorphTriangles>(
	    sliceRotateOf(generator), kSomewhere, SliceResidue<kMorphTriangles> {});

	const double home = 0.6180339887;
	const std::pair<double, double> spots[] = { { 0.0, 0.0 }, { 0.37, -0.125 }, { -1.0, 0.9 } };
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		CAPTURE(i);
		for (const auto& [tx, ty] : spots) {
			CHECK(plain.phaseFor(i, home, tx, ty) == framed.phaseFor(i, home, tx, ty));
			CHECK(plain.rateFor(i, tx, ty) == framed.rateFor(i, tx, ty));
		}
	}

	// And a fold with nothing to fold is the identity: at zero drift both travel terms are exactly ±0.0,
	// so the difference is zero and no bit of a banked residue is disturbed by a turn made at home.
	SliceResidue<kMorphTriangles> banked;
	for (size_t i = 0; i < 2 * kMorphTriangles; ++i) {
		banked.offset[i] = 0.001 * static_cast<double>(i + 1);
	}
	SliceGenerator elsewhere = generator;
	elsewhere.rotate += 2.3f;
	CHECK(foldResidue<kMorphTriangles>(banked, sliceRotateOf(generator), sliceRotateOf(elsewhere), 0.0,
	                                   0.0, kSomewhere)
	      == banked);
}

TEST_CASE("a rotation cannot reach the point a sound is standing on") {
	// THE DECISION THAT SUPERSEDED A10: rotating must not change the sound while it sits on a single
	// point. The old guarantee only covered a sound at its home, and on the 2-D chart
	// every drag that is not exactly vertical commits drift — so under A10 the standing sound was
	// preserved almost never. The fold is what makes the guarantee cover where the sound ACTUALLY is.
	const SliceGenerator generator { 0.7f };
	SliceRotation delta;
	delta.rotate = -0.9f;
	const SliceGenerator turned = rotateBy(generator, delta);

	// A DRIFT WORTH THE NAME. Not 0.25 and 0.5 — the drag commits whatever the pointer lands on, and a
	// fixture built from exactly representable numbers is a fixture that cannot see a rounding bug.
	const double driftX = 0.37;
	const double driftY = 0.1 + 0.2;
	const double home = 0.371;

	const SliceBasis<kMorphTriangles> before =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere);
	const SliceResidue<kMorphTriangles> banked =
	    foldResidue<kMorphTriangles>(SliceResidue<kMorphTriangles> {}, sliceRotateOf(generator),
	                                 sliceRotateOf(turned), driftX, driftY, kSomewhere);
	const SliceBasis<kMorphTriangles> after =
	    sliceFrameFor<kMorphTriangles>(sliceRotateOf(turned), kSomewhere, banked);
	// The same turn WITHOUT the fold — the A10 behaviour, kept here as the live contrast. A case that
	// only showed the folded plane agreeing would pass just as well if the rotation had done nothing.
	const SliceBasis<kMorphTriangles> unfolded =
	    sliceBasisFor<kMorphTriangles>(sliceRotateOf(turned), kSomewhere);

	double worstFolded = 0.0;
	double worstUnfolded = 0.0;
	double worstRate = 0.0;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		const double was = before.phaseFor(i, home, driftX, driftY);
		worstFolded = std::max(worstFolded, std::abs(after.phaseFor(i, home, driftX, driftY) - was));
		worstUnfolded =
		    std::max(worstUnfolded, std::abs(unfolded.phaseFor(i, home, driftX, driftY) - was));
		// THE RATE ROWS TOO, and they are half the mechanism: rotation mixes phase coordinates with rate
		// coordinates, so a fold that banked only the phase rows would leave every triangle reading its
		// landscape at a different speed — the standing sound preserved in position and not in busyness.
		worstRate = std::max(worstRate,
		                     std::abs(static_cast<double>(after.rateFor(i, driftX, driftY))
		                              - static_cast<double>(before.rateFor(i, driftX, driftY))));
	}
	CAPTURE(worstFolded);
	CAPTURE(worstUnfolded);
	CAPTURE(worstRate);

	// WHAT THE FOLD CAN AND CANNOT PROMISE. At nonzero drift the banked difference has to land in a
	// finite double, so what is guaranteed is a rounding of the inner sum rather than the bit — one ULP
	// or so per turn, which is what these floors are, and it is arithmetic rather than a design choice.
	//
	// MEASURED ON THIS TOOLCHAIN IT IS BETTER THAN THAT: every one of the 35 coordinates comes back bit
	// for bit, at one turn and at a hundred, because `(S + old) - new` and the decode's `+ new` are the
	// same association and the subtraction lands exactly. That is an observation, not a promise — it is
	// not asserted here, because a case that failed on another compiler would be noise rather than news.
	// The claim that has to hold everywhere is the floor; the claim a listener cares about is the built
	// FLOAT table, and that one is checked for bit-identity in the blend suite where it is observable.
	CHECK(worstFolded < 1e-14);
	CHECK(worstRate < 1e-7);
	// And the contrast, which is what makes the numbers above mean anything: without the fold the same
	// turn moves the same sound by something a listener hears.
	CHECK(worstUnfolded > 1e-3);

	// THE OTHER HALF OF THE VERB STILL WORKS. Preserving the point must not freeze the plane: travel
	// away from where the sound is standing has to follow the NEW orientation, or rotate has become an
	// expensive way of doing nothing.
	bool departureMoved = false;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		departureMoved = departureMoved
		                 || after.phaseFor(i, home, driftX + 0.2, driftY - 0.15)
		                        != before.phaseFor(i, home, driftX + 0.2, driftY - 0.15);
	}
	CHECK(departureMoved);
}

TEST_CASE("the standing point survives a hundred turns, and a crumb restores it exactly") {
	// ACCUMULATION IS THE QUESTION A SINGLE TURN CANNOT ANSWER. Each fold banks the difference the turn
	// it is part of would have made, so the errors are per-turn roundings rather than a growing drift —
	// but that is a claim about a hundred turns, so a hundred turns is what checks it.
	const double driftX = 0.41;
	const double driftY = -0.23;
	const double home = 0.271828;

	SliceGenerator generator { 0.15f };
	const SliceGenerator crumbGenerator = generator;
	const SliceResidue<kMorphTriangles> crumbResidue {};

	SliceResidue<kMorphTriangles> banked = crumbResidue;
	const SliceBasis<kMorphTriangles> start =
	    sliceFrameFor<kMorphTriangles>(sliceRotateOf(crumbGenerator), kSomewhere, crumbResidue);

	uint32_t dice = 0x9e3779b9u;
	for (int turn = 0; turn < 100; ++turn) {
		dice ^= dice << 13;
		dice ^= dice >> 17;
		dice ^= dice << 5;
		SliceRotation step;
		// A real encoder's clicks: small, signed, and never the same twice.
		step.rotate = (static_cast<float>(dice % 2000u) / 1000.0f - 1.0f) * 0.25f;
		const SliceGenerator next = rotateBy(generator, step);
		banked = foldResidue<kMorphTriangles>(banked, sliceRotateOf(generator), sliceRotateOf(next),
		                                      driftX, driftY, kSomewhere);
		generator = next;
	}

	const SliceBasis<kMorphTriangles> walked =
	    sliceFrameFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere, banked);
	double worst = 0.0;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		worst = std::max(worst, std::abs(walked.phaseFor(i, home, driftX, driftY)
		                                 - start.phaseFor(i, home, driftX, driftY)));
	}
	CAPTURE(worst);
	// Below what a float character table can resolve by orders, which is the property that matters: a
	// hundred turns of the encoder leave the standing sound where it was.
	CHECK(worst < 1e-12);
	// The plane really did travel — otherwise the line above is a tautology about a generator nobody
	// turned.
	CHECK_FALSE(generator == crumbGenerator);

	// AND THE REWIND IS THE STORED PAIR, restored, which is exact because it was never arithmetic. The
	// residue joins the generator as identity: a crumb that carried the plane and not the residue would
	// land on the right orientation with the wrong sound standing on it.
	const SliceBasis<kMorphTriangles> retraced =
	    sliceFrameFor<kMorphTriangles>(sliceRotateOf(crumbGenerator), kSomewhere, crumbResidue);
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		CHECK(retraced.phaseFor(i, home, driftX, driftY) == start.phaseFor(i, home, driftX, driftY));
	}
	// The crumb WITHOUT its residue is the thing that would look fine and be wrong — the plane restored,
	// the sound standing somewhere else.
	const SliceBasis<kMorphTriangles> halfCrumb =
	    sliceFrameFor<kMorphTriangles>(sliceRotateOf(generator), kSomewhere, crumbResidue);
	bool halfCrumbMoved = false;
	for (size_t i = 0; i < kMorphTriangles; ++i) {
		halfCrumbMoved = halfCrumbMoved
		                 || halfCrumb.phaseFor(i, home, driftX, driftY)
		                        != start.phaseFor(i, home, driftX, driftY);
	}
	CHECK(halfCrumbMoved);
}

TEST_CASE("turns accumulate in the generator, and a rewind is exact") {
	// §1.2: the generator is the state of record and the basis is derived. A basis turned IN PLACE
	// would carry each turn's float error into the next, so retracing a breadcrumb would land near
	// where it was recorded rather than on it. Restoring a STORED generator is exact by construction.
	const SliceGenerator start { 0.5f };

	SliceRotation step;
	step.rotate = 0.1f;

	SliceGenerator walked = start;
	for (int i = 0; i < 20; ++i) {
		walked = rotateBy(walked, step);
	}
	CHECK(walked.rotate != start.rotate);

	// THE CONTRAST IS THE POINT, and the first version of this case did not have one: it checked that a
	// copy equalled its source and that the same function twice gave the same answer, neither of which
	// can fail. What §1.2 actually claims is that RESTORING the stored generator is exact where
	// UN-TURNING it is not.
	const SliceGenerator rewound = start; // the breadcrumb, restored
	SliceGenerator unturned = walked;     // the same journey walked backwards
	SliceRotation back;
	back.rotate = -step.rotate;
	for (int i = 0; i < 20; ++i) {
		unturned = rotateBy(unturned, back);
	}

	CHECK(rewound == start);        // exact, because it was never arithmetic
	CHECK_FALSE(unturned == start); // and subtracting the increments back off is not
	CAPTURE(unturned.rotate - start.rotate);
	CHECK(std::abs(unturned.rotate - start.rotate) < 1e-5f); // close, which is the trap
	CHECK(sliceBasisFor<kMorphTriangles>(sliceRotateOf(rewound), kSomewhere).x
	      == sliceBasisFor<kMorphTriangles>(sliceRotateOf(start), kSomewhere).x);

	// A zero turn is exactly the identity, which is what lets an encoder send one every frame while
	// nobody is touching it.
	CHECK(rotateBy(start, SliceRotation {}) == start);
}

TEST_CASE("no direction in the plane is a disguised gamma") {
	// THE PROPERTY THE SECOND ROW EXISTS FOR, and the one an obvious statistic gets wrong.
	//
	// A phase coefficient of exactly 1 means travel moves that triangle exactly as far as home does —
	// gamma, by another name — so a travel direction whose phase coefficients are all EQUAL is a
	// direction that does nothing the old one-dimensional engine could not. #29 convicted the shipped
	// decode of exactly that: 29 of morph's 35 coordinates pinned at 1, every iso-line at −45°.
	//
	// IT IS NOT ENOUGH TO CHECK THE TWO ROWS. A plane is every combination of them, and a plane can be
	// open along X, open along Y, and still contain a degenerate direction between them — which is
	// precisely what a second row read a HALF period along would give: `y − 1 = −(x − 1)` exactly, so
	// x + y is 2 everywhere and moving both knobs together is pure gamma. That direction is reached by
	// any diagonal drag, so it is not a corner case.
	//
	// AND THE ANGLE BETWEEN THE ROWS CANNOT SEE IT: measured across the shifts tried, the half period
	// scores the LARGEST row angle of all of them (70.9° against 43.8° for the quarter that ships)
	// while being the one degenerate choice. A metric that ranks the known-bad first is not a metric.
	// So the statistic is the worst spread over EVERY direction: sweep the unit circle, take the mean
	// absolute deviation of the phase coefficients of αx + βy, keep the minimum.
	double worst = 1e9;
	int worstDegrees = 0;
	for (const double home : { 0.0, 0.37, 2.7 }) {
		for (int step = 0; step <= 24; ++step) {
			const double rotate = static_cast<double>(step) * 0.17;
			const SliceBasis<kMorphTriangles> basis = sliceBasisFor<kMorphTriangles>(rotate, home);
			for (int degrees = 0; degrees < 180; ++degrees) {
				const double radians = static_cast<double>(degrees) * 3.14159265358979 / 180.0;
				const double alpha = std::cos(radians);
				const double beta = std::sin(radians);
				double mean = 0.0;
				std::array<double, kMorphTriangles> along {};
				for (size_t i = 0; i < kMorphTriangles; ++i) {
					along[i] = alpha * static_cast<double>(basis.x[i]) + beta * static_cast<double>(basis.y[i]);
					mean += along[i];
				}
				mean /= static_cast<double>(kMorphTriangles);
				double spread = 0.0;
				for (const double v : along) {
					spread += std::abs(v - mean);
				}
				spread /= static_cast<double>(kMorphTriangles);
				if (spread < worst) {
					worst = spread;
					worstDegrees = degrees;
				}
			}
		}
	}
	INFO("the flattest travel direction anywhere in the sweep is at " << worstDegrees
	                                                                 << "° with spread " << worst);
	// Measured 0.391 at the quarter period that ships, against 0.225 at an eighth, 0.219 at φ⁻², and
	// exactly 0.000 at a half — where the flattest direction is 45°, the diagonal drag. The bar sits
	// well above the near-misses so that a shift chosen by ear cannot quietly reintroduce the diagonal.
	CHECK(worst > 0.3);
}

TEST_CASE("MEASURE: how degenerate is the plane, really — the two axes against each other and against gamma") {
	// **THE CLAIM UNDER TEST WAS REPEATED WITHOUT BEING CHECKED** (2026-09-20). A backlog entry
	// records the plane as "~83% degenerate" — "29 of morph's 35 phase coefficients are exactly 1 …
	// six rotations cannot move 35 phase coordinates off the all-ones vector". The objection: how can
	// the axes be degenerate when rotate covers all possible decode matrices?
	//
	// THE OBJECTION IS RIGHT THAT THE MEASUREMENT IS STALE: `kSliceAngles` does not exist in this tree. The six
	// stored angles, the aperture and the stride pairing were replaced by `M(rotate)` — every one of
	// the 4N entries is `planeEntry(rotate, home, i, rateRow, second)` on phi-spaced ladders. So the
	// number describes code that is gone, and a stale number repeated is worse than no number.
	//
	// THE TWO QUESTIONS THAT ACTUALLY DECIDE IT, neither of which the old figure asked:
	//   1. ARE THE TWO AXES THE SAME DIRECTION? cos(x, y) over the phase row. That is what "degenerate"
	//      means for a plane — if the two basis vectors are parallel there is no second dimension,
	//      whatever the coefficients individually are.
	//   2. IS TRAVEL A SECOND GAMMA? cos(x, all-ones). Gamma shifts every phase together, so the
	//      all-ones vector IS the gamma direction; travel duplicating it is the specific failure
	//      `sliceBasisFor`'s "born open" note says must not happen.
	//
	// A coefficient near 1 is not degeneracy on its own — 35 coefficients can all sit near 1 and still
	// span a plane, provided the SPREAD is not parallel between the two axes. That distinction is the
	// whole of what I got wrong by quoting a summary.
	const auto stats = [](const std::array<float, 2 * kMorphTriangles>& row, size_t from) {
		double mean = 0.0;
		for (size_t i = 0; i < kMorphTriangles; ++i) {
			mean += static_cast<double>(row[from + i]);
		}
		mean /= static_cast<double>(kMorphTriangles);
		double lo = 1e9;
		double hi = -1e9;
		double var = 0.0;
		for (size_t i = 0; i < kMorphTriangles; ++i) {
			const double v = static_cast<double>(row[from + i]);
			lo = std::min(lo, v);
			hi = std::max(hi, v);
			var += (v - mean) * (v - mean);
		}
		return std::array<double, 4> { mean, std::sqrt(var / static_cast<double>(kMorphTriangles)), lo,
			                           hi };
	};
	const auto cosine = [](const float* a, const float* b, size_t n) {
		double dot = 0.0;
		double na = 0.0;
		double nb = 0.0;
		for (size_t i = 0; i < n; ++i) {
			dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
			na += static_cast<double>(a[i]) * static_cast<double>(a[i]);
			nb += static_cast<double>(b[i]) * static_cast<double>(b[i]);
		}
		return (na > 0.0 && nb > 0.0) ? dot / (std::sqrt(na) * std::sqrt(nb)) : 0.0;
	};
	const std::array<float, kMorphTriangles> ones = [] {
		std::array<float, kMorphTriangles> o {};
		o.fill(1.0f);
		return o;
	}();

	MESSAGE("PHASE ROW of morph's 35 triangles. cos(x,y)=1 means the two axes are ONE direction (no "
	        "plane at all); cos(x,ones)=1 means travel IS gamma.");
	for (const double home : { 0.0, 0.37, 1.5 }) {
		for (const double rotate : { 0.0, 0.05, 0.25, 1.0, 3.0 }) {
			const SliceBasis<kMorphTriangles> b = sliceBasisFor<kMorphTriangles>(rotate, home);
			const auto sx = stats(b.x, 0);
			const auto sy = stats(b.y, 0);
			MESSAGE("home " << home << " rotate " << rotate << " | x mean " << sx[0] << " sd " << sx[1]
			                << " [" << sx[2] << ".." << sx[3] << "]"
			                << " | y mean " << sy[0] << " sd " << sy[1] << " [" << sy[2] << ".."
			                << sy[3] << "]"
			                << " | cos(x,y) " << cosine(b.x.data(), b.y.data(), kMorphTriangles)
			                << " | cos(x,ones) " << cosine(b.x.data(), ones.data(), kMorphTriangles)
			                << " | cos(y,ones) " << cosine(b.y.data(), ones.data(), kMorphTriangles));
		}
	}
	CHECK(true);
}
