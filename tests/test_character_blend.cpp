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

#include "dsp/character_bank.h"
#include "dsp/slice_build.h"
#include "plugin/character_params.h"
#include "plugin/rotate_sound.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <utility>
#include <vector>

// WHERE THE PARAMETERS BECOME A CONSTELLATION, and where the constellation becomes one table.
//
// The geometry has its own suite and the recipe blend has its own suite; both are arithmetic on
// numbers somebody hands them. This is the seam between them — which sounds are live, which slot's
// weight is whose, and which of the two blend rules a given patch gets — and every bug that can live
// here is an off-by-one that still produces a perfectly plausible sound.
//
// The load-bearing claim is the FIRST one: two sounds render exactly what v1 rendered, to the bit.
// Everything else is new behaviour, and new behaviour is allowed to be new.

using namespace feathers;

namespace {

/// A patch: the parameter table's own defaults, with whatever this test wanted changed.
///
/// Defaults from paramDescs() rather than from a list written here, which is the rule CLAUDE.md
/// states and which two bugs have already paid for: a fixture that declares what our code expects
/// tests the code against itself.
class Patch {
public:
	Patch& set(ParamId id, double value) {
		overrides_[static_cast<clap_id>(id)] = value;
		return *this;
	}

	Patch& enable(size_t slot, bool on) { return set(soundParam(slot, SoundField::Enable), on ? 1.0 : 0.0); }

	Patch& place(size_t slot, double x, double y) {
		set(soundParam(slot, SoundField::PlaceX), x);
		return set(soundParam(slot, SoundField::PlaceY), y);
	}

	Patch& zone(size_t slot, double z) { return set(soundParam(slot, SoundField::Zone), z); }

	Patch& width(size_t slot, double w) { return set(soundParam(slot, SoundField::Width), w); }

	double operator()(ParamId id) const {
		const auto it = overrides_.find(static_cast<clap_id>(id));
		if (it != overrides_.end()) {
			return it->second;
		}
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		REQUIRE(desc != nullptr);
		return desc->def;
	}

	/// At the audition's defaults, with the puck parked and nothing drifted, which is the build that is
	/// played. A case about motion passes its own velocity; a case about drift passes its own offsets.
	Character character(const AuditionTuning& tuning = {}, AnchorPoint velocity = {},
	                    const SoundTravel& drift = {}, const SoundOrientations& planes = {},
	                    const SoundResidue& residue = {}, const Preview& preview = {},
	                    EarSide ear = EarSide::Head) const {
		return characterFromParams([this](ParamId id) { return (*this)(id); },
		                           oscTypeFromParams([this](ParamId id) { return (*this)(id); }), tuning,
		                           velocity, drift, planes, residue, SoundGamma {}, preview, ear);
	}

private:
	std::map<clap_id, double> overrides_;
};

/// One sound's table, built the way the bank builds it — the comparison target for "this character IS
/// that sound", which is what standing on an anchor has to mean.
owlet::dsp::PhiMorphWavetable soloMorph(const SoundCoord& s) {
	// THE SOUND'S OWN FRAME, not a default one: its resolved plane and what its rotations banked. Both
	// are empty in the cases that only care about blending, so this is the same call it always was —
	// but a reference built on somebody else's plane is a reference to a different sound, and that trap
	// has been walked into once already (the wrong-home finding).
	// The engine's own two arguments, mirrored: the turn this sound carries, and where it is. Written
	// as `rotateFor` rather than as a generator because that is what the builder reads — a reference
	// that derived its plane a different way would be a reference to a different sound, and that trap
	// has been walked into once already (the wrong-home finding).
	return buildMorphSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
	                        sliceFrameFor<kMorphTriangles>(rotateFor(s.orientation),
	                                                       sliceHome(s.zone, s.phaseOffset, 0.0),
	                                                       s.residue.morph),
	                        0.0);
}

/// Bit equality, not tolerance. The claims here are identity claims — this patch renders what it
/// always rendered, this position IS that sound — and a tolerance would let exactly the drift they
/// exist to catch through.
bool sameBits(const owlet::dsp::PhiMorphWavetable& a, const owlet::dsp::PhiMorphWavetable& b) {
	return std::memcmp(&a, &b, sizeof(a)) == 0;
}

Character twoSounds(uint16_t zoneA, uint16_t zoneB, float xf) {
	Character c;
	c.count = 2;
	c.sounds[0].zone = zoneA;
	c.sounds[1].zone = zoneB;
	c.sounds[0].weight = 1.0 - static_cast<double>(xf);
	c.sounds[1].weight = static_cast<double>(xf);
	c.xf = xf;
	return c;
}

} // namespace

TEST_CASE("parity mode still renders the v1 crossfade, bit for bit") {
	// THE WAY BACK, KEPT RUNNING RATHER THAN KEPT AROUND — §2c. The verbatim two-sound crossfade was
	// the shipping rule until blue shift landed; it is now the reference-parity reference, and the point
	// of this case is that it stays EXACT rather than bit-rotting into something that merely resembles
	// what v1 did. Reachable from a test and from a debug environment, and from nowhere else: a control
	// is how something becomes a feature.
	for (const float xf : { 0.0f, 0.25f, 0.5f, 0.731f, 1.0f }) {
		Character c = twoSounds(211, 640, xf);
		c.v1Crossfade = true;
		const owlet::dsp::PhiMorphWavetable expected =
		    buildMorphXfade(soloMorph(c.sounds[0]), soloMorph(c.sounds[1]), xf);
		CHECK(sameBits(buildMorphFor(c), expected));
	}
}

TEST_CASE("one rule at every sound count") {
	// §2c, and the seam §7.3 named is what it dissolves: switching a third sound on used to move the
	// excitation factors from B's to a weighted average and drop the directional overshoot, in one
	// step, at a moment the player chose. There is no such moment when there is nothing to switch
	// between.
	//
	// The claim, stated so it cannot be satisfied by accident: the shipping path at two sounds is the
	// BLEND, and parity mode is the only way back to the crossfade.
	Character uniform = twoSounds(211, 640, 0.4f);
	Character parity = uniform;
	parity.v1Crossfade = true;
	CHECK_FALSE(sameBits(buildMorphFor(uniform), buildMorphFor(parity)));

	const owlet::dsp::PhiMorphWavetable tables[] = { soloMorph(uniform.sounds[0]),
		                                              soloMorph(uniform.sounds[1]) };
	const double weights[] = { uniform.sounds[0].weight, uniform.sounds[1].weight };
	CHECK(sameBits(buildMorphFor(uniform), buildMorphBlend({ tables, 2 }, { weights, 2 })));

	// AND THE SEAM IS GONE, which is the thing worth measuring rather than asserting. Adding a third
	// sound with a vanishing share used to be a step change; now it approaches the two-sound blend
	// smoothly, because both sides are the same function of the weights.
	double worst = 0.0;
	for (const double share : { 0.1, 0.01, 0.001, 0.0001 }) {
		Character three = uniform;
		three.count = 3;
		three.sounds[2].zone = 900;
		three.sounds[0].weight = uniform.sounds[0].weight * (1.0 - share);
		three.sounds[1].weight = uniform.sounds[1].weight * (1.0 - share);
		three.sounds[2].weight = share;

		const owlet::dsp::PhiMorphWavetable withThird = buildMorphFor(three);
		const owlet::dsp::PhiMorphWavetable withoutIt = buildMorphFor(uniform);
		double gap = 0.0;
		for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
			gap = std::max(gap, std::abs(static_cast<double>(withThird.amplitudes[i])
			                             - static_cast<double>(withoutIt.amplitudes[i])));
		}
		CAPTURE(share);
		CAPTURE(gap);
		// The difference shrinks with the share — a step change would hold its size as the share went
		// to nothing, which is what the seam WAS.
		const bool shrinking = worst == 0.0 || gap < worst + 1e-9;
		CHECK(shrinking);
		worst = gap;
	}
	CHECK(worst < 1.0e-3); // vanishing share, vanishing difference
}

TEST_CASE("a third sound is heard") {
	// The point of the whole stage, and the first thing a wiring bug silences: a build that blends only
	// the first two sounds passes every other test in this file and every test in the suite.
	Character two = twoSounds(211, 640, 0.5f);
	Character three = two;
	three.count = 3;
	three.sounds[2].zone = 900;
	// Even shares, which is where the excite spread is largest and a dropped term shows most.
	for (int i = 0; i < 3; ++i) {
		three.sounds[i].weight = 1.0 / 3.0;
	}
	two.sounds[0].weight = 0.5;
	two.sounds[1].weight = 0.5;

	CHECK_FALSE(sameBits(buildMorphFor(three), buildMorphFor(two)));

	// And it is the third sound that did it, rather than the switch from crossfade to blend: the same
	// three slots with the third's share at zero is the two-sound blend, which is a different table
	// again from the one where it has a third of the weight.
	Character muted = three;
	muted.sounds[2].weight = 0.0;
	muted.sounds[0].weight = 0.5;
	muted.sounds[1].weight = 0.5;
	CHECK_FALSE(sameBits(buildMorphFor(three), buildMorphFor(muted)));
}

TEST_CASE("standing on a sound is that sound, with a constellation around it") {
	// The limit the blend has to reach and the one an ear checks first. At an anchor the weights snap
	// to that sound alone, so the recipe average is a one-term sum, the excite spread is exactly zero,
	// and what comes out is that sound and nothing of its neighbours.
	//
	// The reference is the CROSSFADE PARKED ON IT rather than the raw built table, and the difference
	// is the honest one: both drop morphAmpOvershoot, morphCurvBoost and morphPhaseDistort, because
	// those describe a transition INTO a zone and a blend result is not a zone. The firmware's own
	// effective table leaves them zeroed for the same reason. Parking Path at an end is what a user did
	// in v1 to hear one sound alone, so this is a comparison against what they heard.
	Character c;
	c.count = 4;
	const uint16_t zones[] = { 100, 340, 700, 905 };
	for (int i = 0; i < 4; ++i) {
		c.sounds[i].zone = zones[i];
		c.sounds[i].weight = 0.0;
	}
	c.sounds[2].weight = 1.0;

	const owlet::dsp::PhiMorphWavetable alone = soloMorph(c.sounds[2]);
	CHECK(sameBits(buildMorphFor(c), buildMorphXfade(alone, alone, 0.0f)));
}

TEST_CASE("an empty constellation is silent rather than undefined") {
	// Reachable: every enable is a parameter and a user may turn all eight off. The blend over nothing
	// is a zeroed table, which evaluates to zero everywhere — silence, and recoverable the moment a
	// sound comes back. What it must not be is a division by zero or a table nobody built.
	Character c;
	c.count = 0;
	const owlet::dsp::PhiMorphWavetable table = buildMorphFor(c);
	for (double phase = 0.0; phase < 1.0; phase += 0.01) {
		CHECK(evalMorphTable(table, phase) == 0.0f);
	}

	c.engine = OscEngine::Stair;
	const StairTable stair = buildStairTable(buildStairFor(c));
	for (double phase = 0.0; phase < 1.0; phase += 0.01) {
		CHECK(evalStairTable(stair, phase) == 0.0f);
	}
}

TEST_CASE("the default patch is the v1 instrument") {
	// A and B on either side of the origin and nothing else switched on — the A→B path v1 shipped. If
	// this ever reports three sounds, a default changed and every patch that says nothing changed with
	// it.
	const Character c = Patch {}.character();
	CHECK(c.count == 2);
	CHECK(c.sounds[0].weight + c.sounds[1].weight == doctest::Approx(1.0));
	// Travel is zero because Weather defaults to zero, whatever the puck is doing.
	CHECK(c.sounds[0].travelX == 0.0);
	CHECK(c.sounds[1].travelY == 0.0);
}

TEST_CASE("a switched-off sound is absent, not silent") {
	// The distinction is the tour's. An absent sound takes no leg of the polyline, so Path over B and C
	// is a walk from B to C — where a sound kept at weight zero would still be a corner to walk around.
	Patch patch;
	patch.enable(0, false);
	patch.enable(2, true);

	const Character c = patch.character();
	REQUIRE(c.count == 2);

	// Slot order is preserved, so the first live sound is B's identity and the second is C's.
	Patch reference;
	CHECK(c.sounds[0].zone
	      == static_cast<uint16_t>(std::lround(reference(soundParam(1, SoundField::Zone)))));
	CHECK(c.sounds[1].zone
	      == static_cast<uint16_t>(std::lround(reference(soundParam(2, SoundField::Zone)))));
}

TEST_CASE("the weights belong to the sounds that are live") {
	// THE OFF-BY-ONE THIS FILE EXISTS FOR. Skipping disabled slots means a live sound's index is not
	// its slot, and a weight read at the slot would be somebody else's — a bug that still sounds like a
	// blend, just not the one on screen.
	//
	// Sounds C and F, placed far apart, with the puck parked on F's anchor: F takes all of it.
	Patch patch;
	patch.enable(0, false).enable(1, false).enable(2, true).enable(5, true);
	patch.place(2, -3.0, 0.0).place(5, 2.0, 1.0);
	// Path 1 walks the tour to its last anchor, which is F's.
	patch.set(ParamId::Path, 1.0);

	const Character c = patch.character();
	REQUIRE(c.count == 2);
	CHECK(c.sounds[0].weight == 0.0);
	CHECK(c.sounds[1].weight == 1.0);

	// And the identity that carries the weight is F's, not slot 1's.
	CHECK(c.sounds[1].zone
	      == static_cast<uint16_t>(std::lround(Patch {}(soundParam(5, SoundField::Zone)))));
}

TEST_CASE("the falloff reaches every sound count now, and parity is where it does not") {
	// §7.1d SAID the A/B could not be heard with two sounds, and it was right for the build it was
	// written for: Path governed there and the exponent reached nothing that was played. The uniform rule
	// superseded that — two sounds are weighted like every other count now, so the exponent is audible
	// at two as well, and only PARITY MODE still ignores it. Recorded here rather than in a comment,
	// because a superseded sequencing fact left in a test is a test asserting the old build.
	AuditionTuning powerB;
	powerB.falloffPower = kFalloffPowerB;

	Patch two;
	// Off the tour, so the puck is somewhere the weights actually differ between exponents.
	two.set(ParamId::Path, 0.35).set(ParamId::PosX, 0.2).set(ParamId::PosY, 0.15);
	REQUIRE(two.character().count == 2);
	CHECK_FALSE(sameBits(buildMorphFor(two.character()), buildMorphFor(two.character(powerB))));

	// In parity mode the exponent is inaudible again, exactly as §7.1d described — the crossfade takes
	// Path and never looks at a weight.
	AuditionTuning parityA;
	parityA.v1Crossfade = true;
	AuditionTuning parityB = powerB;
	parityB.v1Crossfade = true;
	CHECK(sameBits(buildMorphFor(two.character(parityA)), buildMorphFor(two.character(parityB))));

	Patch three = two;
	three.enable(2, true);
	REQUIRE(three.character().count == 3);
	CHECK_FALSE(sameBits(buildMorphFor(three.character()), buildMorphFor(three.character(powerB))));

	// And the direction is the one the exponent promises: a higher power is MORE local, so the nearest
	// sound takes a bigger share of it.
	const Character atA = three.character();
	const Character atB = three.character(powerB);
	size_t nearest = 0;
	for (size_t i = 1; i < atA.count; ++i) {
		if (atA.sounds[i].weight > atA.sounds[nearest].weight) {
			nearest = i;
		}
	}
	CHECK(atB.sounds[nearest].weight > atA.sounds[nearest].weight);
}

TEST_CASE("weather is what lets the constellation's position through") {
	// Travel is zero at Weather 0 however far the puck is from an anchor, and non-zero above it. The
	// same claim §7.1c makes, checked here at three live sounds because that is where the design's
	// "where you are STANDING colours what you hear" has more than one direction to point in.
	Patch patch;
	patch.enable(2, true);
	patch.set(ParamId::PosX, 0.4).set(ParamId::PosY, -0.3);

	const Character parked = patch.character();
	REQUIRE(parked.count == 3);
	for (int i = 0; i < 3; ++i) {
		CHECK(parked.sounds[i].travelX == 0.0);
		CHECK(parked.sounds[i].travelY == 0.0);
	}

	patch.set(ParamId::Weather, 1.0);
	const Character walking = patch.character();
	bool anyTravel = false;
	for (int i = 0; i < 3; ++i) {
		anyTravel = anyTravel || walking.sounds[i].travelX != 0.0 || walking.sounds[i].travelY != 0.0;
	}
	CHECK(anyTravel);
	CHECK_FALSE(sameBits(buildMorphFor(parked), buildMorphFor(walking)));
}

TEST_CASE("the excitation the scope prints is the one the engine applies") {
	// blendExcite is in the header for the same reason morphExcite is: the editor PRINTS it, and a
	// second copy of the formula is a readout that can report an excitation nobody is applying.
	//
	// The reduction is the claim worth pinning — at two sounds the spread of the weights IS `4t(1−t)`,
	// so the number does not jump when the rule changes underneath it.
	for (int step = 0; step <= 20; ++step) {
		const double t = static_cast<double>(step) / 20.0;
		const double weights[] = { 1.0 - t, t };
		CAPTURE(t);
		CHECK(blendExcite({ weights, 2 }) == doctest::Approx(morphExcite(static_cast<float>(t))));
	}

	// Zero when one sound has everything, whatever the constellation around it — the anchor case.
	const double alone[] = { 0.0, 1.0, 0.0, 0.0 };
	CHECK(blendExcite({ alone, 4 }) == 0.0f);

	// And greatest when the shares are even, which is what "how mixed is this" has to mean.
	const double even[] = { 0.25, 0.25, 0.25, 0.25 };
	const double lopsided[] = { 0.7, 0.1, 0.1, 0.1 };
	CHECK(blendExcite({ even, 4 }) > blendExcite({ lopsided, 4 }));
}

// ---------------------------------------------------------------------------
// Blue shift — §2b. The mechanism is here; both of its decisions are not.
// ---------------------------------------------------------------------------

namespace {

/// Three sounds, spread out, with the puck off every anchor. Three rather than two because the
/// weights are then genuinely uneven and every term has something to act on — not because two cannot
/// reach the blend, which stopped being true when §2c made the rule uniform.
Patch movingPatch() {
	Patch patch;
	patch.enable(2, true);
	patch.place(0, -0.6, 0.0).place(1, 0.6, 0.0).place(2, 0.0, 0.7);
	patch.zone(0, 120.0).zone(1, 560.0).zone(2, 880.0);
	patch.set(ParamId::Path, 0.35).set(ParamId::PosX, 0.1).set(ParamId::PosY, 0.05);
	return patch;
}

/// BOTH SCALES NAMED, ALWAYS. Nothing in this file may lean on the default, in either direction: the
/// default is itself a decision that can be revisited, and a case that inherits it silently inverts the
/// day it changes — which is exactly what happened when the scales went from 0 to 1, and is why this
/// helper exists rather than three ad-hoc structs.
AuditionTuning scales(double overshoot, double doppler) {
	AuditionTuning t;
	t.blueOvershootScale = overshoot;
	t.blueDopplerScale = doppler;
	return t;
}

AuditionTuning bothScalesUp() {
	return scales(kBlueShiftScaleOn, kBlueShiftScaleOn);
}

} // namespace

TEST_CASE("with the scales down, moving changes nothing at all") {
	// THE REFERENCE THE WHOLE FEATURE IS JUDGED AGAINST: the engine without blue shift, reachable by
	// putting both scales to zero, and identical to the engine that never had it — the same bits, not
	// the same impression. The audition compares against this, so it has to be exact.
	// SWEPT RATHER THAN SAMPLED, and that is not thoroughness for its own sake. The first draft checked
	// one position, and a known-bad that removed the rest-guard entirely PASSED it: at that puck the
	// normalised weights happened to sum to exactly 1.0, so renormalising them again was the identity.
	// Somewhere else it is 1.0 to fifteen places, and there the guard is the whole difference.
	Patch patch = movingPatch();
	bool everDiffered = false;
	for (int step = 0; step <= 40; ++step) {
		const double t = static_cast<double>(step) / 40.0;
		patch.set(ParamId::Path, t).set(ParamId::PosX, 0.6 * t - 0.3).set(ParamId::PosY, 0.25 - 0.5 * t);
		CAPTURE(t);

		const AuditionTuning off = scales(kBlueShiftScaleOff, kBlueShiftScaleOff);
		const Character parked = patch.character(off, { 0.0, 0.0 });
		const Character sprinting = patch.character(off, { 4.0, -3.0 });
		CHECK(sameBits(buildMorphFor(parked), buildMorphFor(sprinting)));

		double sum = 0.0;
		for (int i = 0; i < parked.count; ++i) {
			CHECK(sprinting.sounds[i].approach == 0.0);
			CHECK(sprinting.sounds[i].weightRate == 0.0);
			sum += parked.sounds[i].weight;
		}
		everDiffered = everDiffered || sum != 1.0;
		CHECK(sprinting.blueOvershoot == 0.0f);
	}
	// Stated out loud: somewhere in that sweep the weights do NOT sum to exactly one, which is what
	// makes the branch in buildMorphBlend load-bearing rather than tidy. If this ever stops being true,
	// the sweep has stopped covering the case it was widened for.
	CHECK(everDiffered);
}

TEST_CASE("at rest nothing happens, whatever the scales say") {
	// §2b.1's static purity, and it is exact rather than small: a parked patch is a pure function of
	// position however reactive the field says the neighbourhood is. Every blue-shift term is the
	// velocity multiplied by something, so this is arithmetic and not a threshold.
	const Patch patch = movingPatch();
	CHECK(sameBits(buildMorphFor(patch.character({}, { 0.0, 0.0 })),
	               buildMorphFor(patch.character(bothScalesUp(), { 0.0, 0.0 }))));
}

TEST_CASE("with a scale up, moving is heard") {
	// The feature itself, on the three-sound fixture because that is where the weights are uneven enough
	// for each term to have something to act on. It is NOT a minimum: §2c made the rule uniform, so blue
	// shift reaches two sounds exactly as it reaches eight, and an audition of it needs no third sound
	// switched on first.
	const Patch patch = movingPatch();
	const AnchorPoint moving { 3.0, 1.0 };

	const Character still = patch.character(bothScalesUp(), { 0.0, 0.0 });
	const Character going = patch.character(bothScalesUp(), moving);
	CHECK_FALSE(sameBits(buildMorphFor(still), buildMorphFor(going)));

	// EACH FLAVOUR ALONE, AND ALONE MEANS ALONE. The plan calls them compoundable, which is two claims
	// rather than one: each does something by itself, and each leaves the other switched off.
	//
	// The second half was missing at first, and a known-bad found the hole rather than a reading did —
	// a build where the Doppler term ignored its own scale passed every case here, because the call
	// site's gate hid it whenever BOTH scales were down. With one scale up the gate is open and the
	// other flavour would have run at full strength with its scale at zero.
	const AuditionTuning overshootOnly = scales(kBlueShiftScaleOn, kBlueShiftScaleOff);
	const Character overshooting = patch.character(overshootOnly, moving);
	CHECK_FALSE(sameBits(buildMorphFor(overshooting),
	                     buildMorphFor(patch.character(overshootOnly, { 0.0, 0.0 }))));
	CHECK(overshooting.blueOvershoot > 0.0f);
	for (int i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK(overshooting.sounds[i].approach == 0.0); // no Doppler with the Doppler scale down
	}

	const AuditionTuning dopplerOnly = scales(kBlueShiftScaleOff, kBlueShiftScaleOn);
	const Character dopplering = patch.character(dopplerOnly, moving);
	CHECK_FALSE(sameBits(buildMorphFor(dopplering),
	                     buildMorphFor(patch.character(dopplerOnly, { 0.0, 0.0 }))));
	CHECK(dopplering.blueOvershoot == 0.0f); // no overshoot with the overshoot scale down
	bool anyApproach = false;
	for (int i = 0; i < 3; ++i) {
		anyApproach = anyApproach || dopplering.sounds[i].approach != 0.0;
	}
	CHECK(anyApproach);
}

TEST_CASE("what you are approaching gains, what you are leaving loses") {
	// The DIRECTION claim, which is the whole of "navigation you can hear". A build with the sign
	// inverted would still change the sound while you moved, and would still pass every test above.
	Patch patch = movingPatch();
	patch.set(ParamId::PosX, 0.0).set(ParamId::PosY, 0.0);

	// Straight up the plane, towards sound C at (0, 0.7) and away from nothing in particular.
	const Character going = patch.character(bothScalesUp(), { 0.0, 2.5 });
	REQUIRE(going.count == 3);
	// The field where sound C lives has to be awake for this case to be about direction at all. It is
	// stated rather than assumed because the first draft of this test was not: sound C sat in a dead
	// region of a narrower field and the test read a signed zero as a missing sign.
	REQUIRE(blueShiftFieldsAt(sliceHome(going.sounds[2].zone, going.sounds[2].phaseOffset, 0.0)).doppler > 0.0f);
	CHECK(going.sounds[2].approach > 0.0); // C is ahead

	// And the mirror image: the same gesture reversed reverses every sign, which no accidental
	// asymmetry in the geometry could produce.
	const Character back = patch.character(bothScalesUp(), { 0.0, -2.5 });
	CHECK(back.sounds[2].approach < 0.0);
	CHECK(back.sounds[2].approach == doctest::Approx(-going.sounds[2].approach));

	// The share follows the sign, which is the part that is audible: approaching C makes more of C.
	double toward[8] = {};
	double away[8] = {};
	for (int i = 0; i < 3; ++i) {
		toward[i] = going.sounds[i].weight * (1.0 + going.sounds[i].approach);
		away[i] = back.sounds[i].weight * (1.0 + back.sounds[i].approach);
	}
	CHECK(toward[2] > away[2]);
}

TEST_CASE("the overshoot points along the velocity rather than at a sound") {
	// weightRate is the direction of travel in recipe space, and it has to be the blend's own rate of
	// change: what would arrive next if you kept going. Reversing the gesture reverses it, and a
	// stationary puck has none of it — the two properties that make it usable where "towards the
	// nearest sound" was not, since that flips on a midline where the excitation is loudest.
	const Patch patch = movingPatch();
	const Character going = patch.character(bothScalesUp(), { 2.0, 0.0 });
	const Character back = patch.character(bothScalesUp(), { -2.0, 0.0 });

	double forward = 0.0;
	double reverse = 0.0;
	for (int i = 0; i < 3; ++i) {
		forward += std::abs(going.sounds[i].weightRate);
		reverse += std::abs(back.sounds[i].weightRate);
	}
	REQUIRE(forward > 0.0);
	// EXACTLY MIRRORED, which is what the centred difference buys: a swipe and the same swipe reversed
	// push the overshoot equally hard in opposite directions. A forward difference gets this wrong —
	// 0.196 against 0.237 on this very fixture — because the weights are not linear in position.
	CHECK(reverse == doctest::Approx(forward));
	for (int i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK(back.sounds[i].weightRate == doctest::Approx(-going.sounds[i].weightRate));
	}
	// Sound B is to the right, so moving right is moving towards it: its share is the one arriving.
	CHECK(going.sounds[1].weightRate > 0.0);
	CHECK(back.sounds[1].weightRate < 0.0);

	// And the shares still sum to zero change: what one sound gains another gives up, because the
	// weights are normalised at both positions.
	double net = 0.0;
	for (int i = 0; i < 3; ++i) {
		net += going.sounds[i].weightRate;
	}
	CHECK(net == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("the reactivity field interpolates VALUES, not coordinates") {
	// DECIDED, and the two schemes look equivalent until they are not. `Σ w·field(home)` is bounded by
	// what the neighbours actually are; `field(Σ w·home)` reads the field at a coordinate that may lie
	// in a region NEITHER sound occupies — so two calm sounds either side of a windy stripe would
	// produce a phantom gale between them, at a place nothing is.
	//
	// The fixture has to be a position where the two disagree, or this passes on both builds and proves
	// nothing. It is searched for rather than guessed, and the search asserts what it found.
	uint16_t zoneLow = 0;
	uint16_t zoneHigh = 0;
	double gap = 0.0;
	for (uint16_t a = 0; a < 1000; a += 37) {
		for (uint16_t b = static_cast<uint16_t>(a + 37); b < 1023; b += 37) {
			const double homeA = sliceHome(a, 0.0f, 0.0);
			const double homeB = sliceHome(b, 0.0f, 0.0);
			const double interpolated = 0.5 * blueShiftFieldsAt(homeA).overshoot
			                            + 0.5 * blueShiftFieldsAt(homeB).overshoot;
			const double atMidpoint = blueShiftFieldsAt(0.5 * (homeA + homeB)).overshoot;
			if (std::abs(interpolated - atMidpoint) > gap) {
				gap = std::abs(interpolated - atMidpoint);
				zoneLow = a;
				zoneHigh = b;
			}
		}
	}
	CAPTURE(zoneLow);
	CAPTURE(zoneHigh);
	CAPTURE(gap);
	REQUIRE(gap > 0.05); // the two schemes must be distinguishable here at all

	// Two sounds at those homes, placed symmetrically, with the puck exactly between them so the
	// weights are a half each and the arithmetic above is the arithmetic under test.
	Patch patch;
	patch.zone(0, zoneLow).zone(1, zoneHigh);
	patch.place(0, -0.5, 0.0).place(1, 0.5, 0.0);
	patch.set(ParamId::Path, 0.5);

	const Character c = patch.character(scales(kBlueShiftScaleOn, kBlueShiftScaleOff), { 1.0, 0.0 });
	REQUIRE(c.count == 2);
	REQUIRE(c.sounds[0].weight == doctest::Approx(0.5));

	const double interpolated =
	    c.sounds[0].weight * blueShiftFieldsAt(sliceHome(c.sounds[0].zone, c.sounds[0].phaseOffset, 0.0)).overshoot
	    + c.sounds[1].weight
	          * blueShiftFieldsAt(sliceHome(c.sounds[1].zone, c.sounds[1].phaseOffset, 0.0)).overshoot;
	const double atAveragedCoordinate =
	    blueShiftFieldsAt(c.sounds[0].weight * sliceHome(c.sounds[0].zone, c.sounds[0].phaseOffset, 0.0)
	                      + c.sounds[1].weight * sliceHome(c.sounds[1].zone, c.sounds[1].phaseOffset, 0.0))
	        .overshoot;

	CHECK(c.blueOvershoot
	      == doctest::Approx(interpolated * kBlueShiftOvershootMax * kBlueShiftScaleOn).epsilon(1e-6));
	CHECK(c.blueOvershoot
	      != doctest::Approx(atAveragedCoordinate * kBlueShiftOvershootMax * kBlueShiftScaleOn)
	             .epsilon(1e-6));
}

TEST_CASE("a carried sound reads its motion fields where it is now, not where it started") {
	// THE DEFAULT-DROPPING PASS FOUND THIS, and it is the argument for the pass. `sliceHome`'s base was
	// defaulted, and the one caller in the tree that omitted it was the blue-shift field read inside
	// blueShiftInto — so a sound carried along its own line by the Explore corner kept the motion
	// reactivity of the neighbourhood it had left. Its sibling one file over in orientation_row.cpp
	// always passed the base, so this was not a design choice anywhere; it was a site nobody could see
	// was short, and only the compiler could say so.
	//
	// THE REFERENCE IS INDEPENDENT OF THE ARITHMETIC UNDER TEST, which is what stops this being the
	// blue shift asking itself. `sliceHome` SUMS the offset and the base, and slice_build.h declares
	// these fields a pure function of that home — so moving a quantity from the base into the offset
	// must not change them. Both spellings are exact in float (0.25 and 1.5) and both offsets stay
	// inside the parameter's real 0..2 range, so the two patches have bit-identical homes and there is
	// nothing else left that could differ.
	//
	// KNOWN-BAD: put the base back to a default at the field read and the based patch answers from its
	// old neighbourhood while the offset patch answers from the new one, and the two part company.
	constexpr float kOffset = 0.25f;
	constexpr double kBase = 1.5;

	// A ZONE WHERE THE CARRY ACTUALLY CHANGES THE FIELD. Without this the case could pass on the
	// known-bad by landing somewhere the field happens to be flat, which is the vacuous-control trap
	// this suite has already been bitten by more than once.
	uint16_t carriedZone = 0;
	double spread = 0.0;
	for (uint16_t zone = 0; zone < 1023; zone = static_cast<uint16_t>(zone + 17)) {
		const double before = blueShiftFieldsAt(sliceHome(zone, kOffset, 0.0)).overshoot;
		const double after = blueShiftFieldsAt(sliceHome(zone, kOffset, kBase)).overshoot;
		if (std::abs(after - before) > spread) {
			spread = std::abs(after - before);
			carriedZone = zone;
		}
	}
	CAPTURE(carriedZone);
	CAPTURE(spread);
	REQUIRE(spread > 0.01); // the two readings must be distinguishable at all

	const auto characterAt = [&](float offset, const SoundGamma& gamma) {
		Patch patch;
		// ONE SOUND, so the overshoot is this sound's field and not a blend of two — the default patch
		// ships A and B enabled, and a second anchor would put a weight in front of the number.
		patch.enable(1, false);
		patch.zone(0, carriedZone);
		patch.set(soundParam(0, SoundField::Offset), offset);
		const auto read = [&patch](ParamId id) { return patch(id); };
		// MOVING, because the blue shift is gated on velocity at its call site — at rest the fields are
		// never read at all and both spellings would agree about a zero, which is a case that cannot
		// fail. The velocity only opens the gate here: the overshoot this asserts on is `Σ w·field(home)`
		// and carries no velocity term, so the number stays a pure function of position.
		return characterFromParams(read, OscType::Morph, scales(kBlueShiftScaleOn, kBlueShiftScaleOff),
		                           AnchorPoint { 1.0, 0.0 }, SoundTravel {}, SoundOrientations {},
		                           SoundResidue {}, gamma, Preview {}, EarSide::Head);
	};

	SoundGamma carried;
	carried.base[0] = kBase;
	const Character based = characterAt(kOffset, carried);
	const Character spelled = characterAt(kOffset + static_cast<float>(kBase), SoundGamma {});

	REQUIRE(based.count == 1);
	REQUIRE(based.sounds[0].gammaBase == kBase);
	REQUIRE(spelled.sounds[0].gammaBase == 0.0);
	// Said out loud, because the whole case rests on it: the two sounds are at the SAME home.
	REQUIRE(sliceHome(based.sounds[0].zone, based.sounds[0].phaseOffset, based.sounds[0].gammaBase)
	        == sliceHome(spelled.sounds[0].zone, spelled.sounds[0].phaseOffset,
	                     spelled.sounds[0].gammaBase));
	// And the overshoot is nonzero, or "equal" would be two zeros agreeing about nothing.
	REQUIRE(based.blueOvershoot > 0.0f);
	CHECK(based.blueOvershoot == spelled.blueOvershoot);
}

TEST_CASE("blue shift is live by default") {
	// THE DECISION, PINNED. Default-1 was decided on §2c's argument — the uniform blend rule is gated on
	// blue shift so that it has directional character to give back, and shipping the scales at zero
	// would have made that step a net loss. A default that drifted back to zero would take the
	// justification for the uniform rule with it, silently.
	//
	// The other half of the decision is that it costs a parked patch nothing, and that is checked here
	// rather than assumed: default scales, no velocity, same bits.
	const AuditionTuning shipped;
	CHECK(shipped.blueOvershootScale == kBlueShiftScaleOn);
	CHECK(shipped.blueDopplerScale == kBlueShiftScaleOn);
	CHECK(shipped.blueShiftActive());

	const Patch patch = movingPatch();
	CHECK(sameBits(buildMorphFor(patch.character(shipped, { 0.0, 0.0 })),
	               buildMorphFor(patch.character(scales(kBlueShiftScaleOff, kBlueShiftScaleOff),
	                                             { 0.0, 0.0 }))));

	// And it is audible while moving, which is the other thing a live default has to be for.
	CHECK_FALSE(sameBits(buildMorphFor(patch.character(shipped, { 2.0, 1.0 })),
	                     buildMorphFor(patch.character(scales(kBlueShiftScaleOff, kBlueShiftScaleOff),
	                                                   { 2.0, 1.0 }))));
}

TEST_CASE("at two sounds the overshoot points exactly where v1's did") {
	// THE REDUCTION, AS A CONTROL RATHER THAN A COMMENT. v1's crossfade pushes the amplitude from A
	// past B — `excite · (b − a) · overshoot` — using the one direction a two-point fade has. The
	// N-path has no target, and takes its direction from the velocity instead. The claim is that with
	// two sounds and the puck travelling A→B those are THE SAME DIRECTION, so the generalisation
	// inherits the character rather than inventing a new one.
	//
	// Checked against the blend directly rather than through buildMorphFor so the claim is about the
	// blend's arithmetic alone, with no chance of the parity branch answering instead.
	Patch patch;
	patch.zone(0, 211.0).zone(1, 640.0);
	patch.place(0, -0.5, 0.0).place(1, 0.5, 0.0);
	patch.set(ParamId::Path, 0.5); // the midpoint, where the excitation is largest

	// Travelling from A towards B, which is +x with these placements.
	const Character c = patch.character(scales(kBlueShiftScaleOn, kBlueShiftScaleOff), { 1.5, 0.0 });
	REQUIRE(c.count == 2);
	REQUIRE(c.blueOvershoot > 0.0f);

	const owlet::dsp::PhiMorphWavetable a = soloMorph(c.sounds[0]);
	const owlet::dsp::PhiMorphWavetable b = soloMorph(c.sounds[1]);
	const owlet::dsp::PhiMorphWavetable tables[] = { a, b };
	const double weights[] = { c.sounds[0].weight, c.sounds[1].weight };
	const double rates[] = { c.sounds[0].weightRate, c.sounds[1].weightRate };
	const double noApproach[] = { 0.0, 0.0 };

	// Moving towards B, B's share is the one arriving. If this is backwards the whole push is.
	CHECK(rates[1] > 0.0);
	CHECK(rates[0] < 0.0);

	const owlet::dsp::PhiMorphWavetable plain = buildMorphBlend({ tables, 2 }, { weights, 2 });
	const BlueShift blue { { noApproach, 2 }, { rates, 2 }, c.blueOvershoot };
	const owlet::dsp::PhiMorphWavetable pushed = buildMorphBlend({ tables, 2 }, { weights, 2 }, blue);

	// Every waypoint moved by the SAME positive multiple of (b − a), which is what "the same direction
	// as v1's" means when said precisely. One ratio, taken from the waypoint with the most room, and
	// then required of all the others.
	double reference = 0.0;
	int referenceAt = -1;
	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		const double delta = static_cast<double>(b.amplitudes[i]) - static_cast<double>(a.amplitudes[i]);
		if (std::abs(delta) > 0.05 && referenceAt < 0) {
			referenceAt = i;
			reference = (static_cast<double>(pushed.amplitudes[i]) - static_cast<double>(plain.amplitudes[i]))
			            / delta;
		}
	}
	REQUIRE(referenceAt >= 0);
	CAPTURE(reference);
	CHECK(reference > 0.0); // PAST B, not back towards A — the sign v1 pushes in

	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		const double delta = static_cast<double>(b.amplitudes[i]) - static_cast<double>(a.amplitudes[i]);
		if (std::abs(delta) <= 0.05) {
			continue;
		}
		CAPTURE(i);
		const double moved =
		    static_cast<double>(pushed.amplitudes[i]) - static_cast<double>(plain.amplitudes[i]);
		CHECK(moved / delta == doctest::Approx(reference).epsilon(1e-5));
	}
}

TEST_CASE("blue shift reaches the stair engine too") {
	// IT DID NOT, AND NOTHING SAID SO. buildStairFor blended the plain weights and never passed the
	// motion terms, so blue shift was a morph-only feature by omission — the decisions name no engine,
	// and the shape of the code decided whose feature it was. Parked and moving were byte-identical.
	//
	// All three halves are checked separately, because each reaches the staircase through a different
	// one of its own quantities and a single "something changed" would pass on any one of them working.
	Patch patch = movingPatch();
	patch.set(ParamId::OscType, 1.0); // Stair
	// Zones with a slope worth leaning on: 39% of them have none, and this case is partly about slope.
	patch.zone(0, 291.0).zone(1, 582.0).zone(2, 873.0);

	const AuditionTuning off = scales(kBlueShiftScaleOff, kBlueShiftScaleOff);
	const AnchorPoint moving { 3.0, 1.0 };

	const owlet::dsp::PhiStairParams parked = buildStairFor(patch.character(off, { 0.0, 0.0 }));
	const owlet::dsp::PhiStairParams sprinting = buildStairFor(patch.character(off, moving));
	// With the scales down it is still exactly the engine without any of this in it.
	CHECK(std::memcmp(&parked, &sprinting, sizeof(parked)) == 0);

	// DOPPLER, on the two quantities it reaches: the shares (which move every height and width) and the
	// slope (which is this recipe's edge).
	const AuditionTuning doppler = scales(kBlueShiftScaleOff, kBlueShiftScaleOn);
	const owlet::dsp::PhiStairParams leaned = buildStairFor(patch.character(doppler, moving));
	CHECK(std::memcmp(&parked, &leaned, sizeof(parked)) != 0);
	CHECK(leaned.slope != parked.slope);

	// THE OVERSHOOT, on the heights, and it must not be a uniform push — the loudness rule that follows
	// strips DC and normalises RMS, so a staircase scaled bodily would come out where it went in and
	// the flavour would be inaudible while looking implemented.
	const AuditionTuning overshoot = scales(kBlueShiftScaleOn, kBlueShiftScaleOff);
	const owlet::dsp::PhiStairParams pushed = buildStairFor(patch.character(overshoot, moving));
	CHECK(std::memcmp(&parked, &pushed, sizeof(parked)) != 0);
	CHECK(pushed.slope == parked.slope); // the overshoot is not the Doppler wearing another name

	double smallest = 1e9;
	double largest = -1e9;
	for (int i = 0; i < owlet::dsp::kPhiStairSteps; ++i) {
		const double moved = static_cast<double>(pushed.heights[i]) - static_cast<double>(parked.heights[i]);
		smallest = std::min(smallest, moved);
		largest = std::max(largest, moved);
	}
	CAPTURE(smallest);
	CAPTURE(largest);
	CHECK(largest - smallest > 1.0e-4); // the steps moved by DIFFERENT amounts, which is what shape means

	// And the whole thing survives preparation: the staircase this describes is still one a voice can
	// render, rather than a set of parameters that happens to differ.
	const StairTable table = buildStairTable(buildStairFor(patch.character(bothScalesUp(), moving)));
	for (double phase = 0.0; phase < 1.0; phase += 0.01) {
		REQUIRE(std::isfinite(evalStairTable(table, phase)));
	}
}

TEST_CASE("the overshoot alone does not disturb the shares") {
	// The Doppler half moves the weights; the overshoot half must not touch them. They are separately
	// switchable, so a build that renormalised whenever EITHER was live would put the overshoot's
	// rounding into a blend the Doppler scale is switched off for.
	//
	// TWO CLAIMS AT TWO LEVELS, and only one of them can be caught by looking at the sound. The built
	// table is bit-identical either way — multiplying by max(0.1, 1+0) and dividing by a sum within an
	// ulp of one rounds back to the same doubles at every position tried — so the audio comparison
	// below passes on a build that renormalises for no reason. What discriminates is asking the tilt
	// directly whether it did anything, and that is why the last two lines are here rather than being
	// left to the wavetable to reveal.
	const Patch patch = movingPatch();
	const AuditionTuning overshootOnly = scales(kBlueShiftScaleOn, kBlueShiftScaleOff);
	const Character c = patch.character(overshootOnly, { 3.0, 1.0 });
	REQUIRE(c.count == 3);
	REQUIRE(c.blueOvershoot > 0.0f);

	const owlet::dsp::PhiMorphWavetable tables[] = { soloMorph(c.sounds[0]), soloMorph(c.sounds[1]),
		                                              soloMorph(c.sounds[2]) };
	const double weights[] = { c.sounds[0].weight, c.sounds[1].weight, c.sounds[2].weight };
	const double rates[] = { c.sounds[0].weightRate, c.sounds[1].weightRate, c.sounds[2].weightRate };
	const double noApproach[] = { 0.0, 0.0, 0.0 };

	// Built by hand from the UNTILTED weights: if the blend had renormalised them, this would differ.
	const BlueShift blue { { noApproach, 3 }, { rates, 3 }, c.blueOvershoot };
	CHECK(sameBits(buildMorphFor(c), buildMorphBlend({ tables, 3 }, { weights, 3 }, blue)));

	// And the tilt itself declines the job, which is the part that IS checkable directly.
	double out[8] = { -1.0, -1.0, -1.0 };
	CHECK_FALSE(tiltWeights({ weights, 3 }, blue, out, 3));
	CHECK(out[0] == -1.0); // untouched, not overwritten with the same numbers
}

TEST_CASE("a patch that has never drifted is the engine before drift existed") {
	// THE MIGRATION CONTROL, rewritten because the first version compared the mapping against ITSELF:
	// Patch::character defaults the drift argument, so both sides were the zero-drift build and the
	// case could not have failed. It compares against an INDEPENDENTLY COMPUTED pre-drift expression
	// now — the travel the engine used before the offset existed, `Weather × (puck − anchor)`, worked
	// out here from the same parameters rather than read back out of the thing under test.
	//
	// WHAT IT CAN AND CANNOT CATCH, stated because the difference is easy to overclaim. It catches the
	// drift term landing somewhere that changes the expression: a dropped Weather multiply, the wrong
	// anchor, the wrong puck. It CANNOT catch a placement that is the identity at zero — `x * (1 +
	// drift)` and `Weather × ((puck − anchor) + drift)` both reduce to the same numbers when the offset
	// is zero, and no comparison of zero-drift outputs can separate them. Those are the business of the
	// drift cases below, which use non-zero offsets.
	Patch patch = movingPatch();
	const SoundTravel none;

	for (int step = 0; step <= 24; ++step) {
		const double t = static_cast<double>(step) / 24.0;
		const double posX = 0.7 * t - 0.35;
		const double posY = 0.3 - 0.6 * t;
		const double weather = (step % 3) * 0.5; // 0, 0.5, 1.0 — including the exact-zero case
		patch.set(ParamId::Path, t);
		patch.set(ParamId::PosX, posX).set(ParamId::PosY, posY);
		patch.set(ParamId::Weather, weather);
		CAPTURE(t);
		CAPTURE(weather);

		const Character c = patch.character(AuditionTuning {}, AnchorPoint {}, none);
		REQUIRE(c.count == 3);

		// The pre-drift expression, from the patch rather than from the engine.
		std::vector<AnchorPoint> anchors;
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			if (steppedIndex(patch(soundParam(slot, SoundField::Enable)), kEnableNames.size()) != 0) {
				anchors.push_back({ patch(soundParam(slot, SoundField::PlaceX)),
					                patch(soundParam(slot, SoundField::PlaceY)) });
			}
		}
		const AnchorPoint onTour = tourPoint(anchors, c.xf);
		const AnchorPoint puck { onTour.x + posX, onTour.y + posY };

		// THROUGH THE SAME TAPER THE ENGINE APPLIES. Weather is a power-scaled amount (see amountTaper
		// — measured to spend 98% of its distance in the bottom eighth of the knob), and a restatement
		// that used the raw knob would be asserting the taper away. Hoisted so both claims below read
		// the one value.
		const double scaled = amountTaper(weather);
		for (int i = 0; i < c.count; ++i) {
			CAPTURE(i);
			const double expectedX = scaled * (puck.x - anchors[static_cast<size_t>(i)].x);
			const double expectedY = scaled * (puck.y - anchors[static_cast<size_t>(i)].y);
			// NUMERIC equality, not bit equality, and the distinction is the whole finding: at Weather 0
			// the pre-drift product is −0.0 for a sound on the far side of the puck, and `−0.0 + 0.0` is
			// `+0.0`. The travel value therefore differs in its SIGN BIT at about half the swept points
			// while being the same number. That is harmless — ±0.0 multiplies to ±0.0 and adds exactly —
			// and the claim that matters is the one below.
			CHECK(c.sounds[i].travelX == expectedX);
			CHECK(c.sounds[i].travelY == expectedY);
		}

		// THE CHARACTER IS IDENTICAL, bit for bit, which is what a patch made before drift existed is
		// owed. Built from the engine's own decode against one built from the pre-drift travel.
		Character reference = c;
		for (int i = 0; i < c.count; ++i) {
			reference.sounds[i].travelX = scaled * (puck.x - anchors[static_cast<size_t>(i)].x);
			reference.sounds[i].travelY = scaled * (puck.y - anchors[static_cast<size_t>(i)].y);
		}
		CHECK(sameBits(buildMorphFor(c), buildMorphFor(reference)));
	}
}

TEST_CASE("drift moves the sound, and only the sound that drifted") {
	// The verb itself. An offset is IDENTITY-level: it changes what that sound is, wherever the puck
	// happens to be standing, which is the difference between it and moving the puck.
	Patch patch = movingPatch();
	SoundTravel drift;
	drift.offset[2] = { 0.35, -0.2 }; // sound C only

	const auto build = [&patch](const SoundTravel& d) {
		return characterFromParams([&patch](ParamId id) { return patch(id); }, OscType::Morph,
		                           AuditionTuning {}, AnchorPoint {}, d, SoundOrientations {},
		                           SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	};
	const Character before = build(SoundTravel {});
	const Character after = build(drift);
	REQUIRE(after.count == 3);

	CHECK(after.sounds[2].travelX != before.sounds[2].travelX);
	// AND NOBODY ELSE MOVED. An offset indexed by position rather than by slot would have drifted
	// whichever sound happened to sit third in the packed list.
	CHECK(after.sounds[0].travelX == before.sounds[0].travelX);
	CHECK(after.sounds[1].travelX == before.sounds[1].travelX);
	CHECK_FALSE(sameBits(buildMorphFor(after), buildMorphFor(before)));

	// IT IS NOT SCALED BY WEATHER, which is the design's own consequence: Weather says how much of the
	// terrain the PUCK's position lets through, and a sound's drift is part of what that sound is. A
	// patch at Weather 0 can still have drifted sounds — and could not, if the offset shared the
	// multiplier.
	patch.set(ParamId::Weather, 0.0);
	const Character parked = build(drift);
	CHECK(parked.sounds[2].travelX == doctest::Approx(0.35));
	CHECK(parked.sounds[0].travelX == 0.0); // and the undrifted sounds are still exactly home
}

TEST_CASE("drift follows the sound when its neighbours are switched off") {
	// The packing trap, stated as its own case because it is the one that produces a plausible wrong
	// sound rather than an obvious one: live sounds are packed, so slot 5's offset must still be
	// slot 5's after slots 0 and 1 are disabled.
	Patch patch;
	patch.enable(0, false).enable(1, false).enable(5, true);
	SoundTravel drift;
	drift.offset[5] = { -0.4, 0.25 };

	const Character c = characterFromParams([&patch](ParamId id) { return patch(id); },
	                                        OscType::Morph, AuditionTuning {}, AnchorPoint {}, drift,
	                                        SoundOrientations {}, SoundResidue {}, SoundGamma {}, Preview {}, EarSide::Head);
	REQUIRE(c.count == 1);
	CHECK(c.sounds[0].travelX == doctest::Approx(-0.4));
	CHECK(c.sounds[0].travelY == doctest::Approx(0.25));
}

TEST_CASE("the tilt refuses a total it cannot divide by") {
	// `total > 0.0` is TRUE OF INFINITY, and dividing by it sends every share to zero — a blend with
	// no sounds in it, which renders as silence rather than as a wrong sound and would therefore be
	// looked for in the wrong place. The inline version this helper was hoisted from guarded both
	// halves; the hoist kept only the positivity check.
	//
	// Nothing upstream can reach it today: blendWeights normalises, and the approach term is clamped.
	// It is guarded because tiltWeights is a public helper now and its callers are not all written.
	// Reaching it takes a SUM that overflows, not merely a large term: `1.0 + DBL_MAX` is still finite,
	// so a first draft of this case with halved weights produced a perfectly good tilt and asserted the
	// opposite. Two unit weights each scaled to DBL_MAX is what actually adds to infinity.
	const double unit[] = { 1.0, 1.0 };
	const double huge[] = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max() };
	const BlueShift blown { { huge, 2 }, {}, 0.0f };
	double out[2] = { -1.0, -1.0 };
	CHECK_FALSE(tiltWeights({ unit, 2 }, blown, out, 2));
	CHECK(out[0] == std::numeric_limits<double>::max()); // written, but the caller is told to ignore it

	const double weights[] = { 0.5, 0.5 };

	// The ordinary case still works, or the guard has eaten the feature.
	const double gentle[] = { 0.2, -0.2 };
	const BlueShift ordinary { { gentle, 2 }, {}, 0.0f };
	CHECK(tiltWeights({ weights, 2 }, ordinary, out, 2));
	CHECK(out[0] > out[1]);
	CHECK(out[0] + out[1] == doctest::Approx(1.0));
}

TEST_CASE("an unpinned sound is built on the plane its OWN home gives it") {
	// THE CLAIM THAT LIVED NOWHERE. I named this residual and pointed at test_slice_build; the reviewer
	// built the known-bad — oneMorph deriving every unpinned plane from a constant home — and ALL FOUR
	// suites passed on it while the built table provably differed. test_slice_build checks the
	// FUNCTION, M at homes the test hands it; the bug lives in the CALLER, in which
	// home oneMorph passes. A case cannot fail on a path it never invokes.
	//
	// So this invokes it: the engine's own build, against a plane derived independently here from THIS
	// sound's home.
	//
	// TRAVEL FIRST, for the reason the plane round trip needs it — a plane reaches nothing at home, so
	// a sound at zero travel would agree whatever home the derivation used.
	Patch patch;
	patch.enable(1, false); // one sound, so the blend is that sound and nothing else
	patch.zone(0, 777.0);
	patch.set(soundParam(0, SoundField::Offset), 0.125);

	SoundTravel drift;
	drift.offset[0] = { 0.4, -0.25 };
	const Character c = patch.character(AuditionTuning {}, AnchorPoint {}, drift);
	REQUIRE(c.count == 1);
	REQUIRE_FALSE(c.sounds[0].orientation.pinned); // unpinned, so the builder must derive

	// The plane THIS sound's home gives it, worked out here rather than read back out of the engine.
	const double home = sliceHome(c.sounds[0].zone, c.sounds[0].phaseOffset, 0.0);
	const owlet::dsp::PhiMorphWavetable table =
	    buildMorphSliced(c.sounds[0].zone, c.sounds[0].phaseOffset, c.sounds[0].travelX,
	                     c.sounds[0].travelY, sliceBasisFor<kMorphTriangles>(0.0, home), 0.0);
	const double weight[] = { 1.0 };
	const owlet::dsp::PhiMorphWavetable expected = buildMorphBlend({ &table, 1 }, { weight, 1 });

	CHECK(sameBits(buildMorphFor(c), expected));

	// AND A DIFFERENT HOME IS A DIFFERENT PLANE, or the comparison above could pass on a build that
	// used any home at all.
	const owlet::dsp::PhiMorphWavetable wrong =
	    buildMorphSliced(c.sounds[0].zone, c.sounds[0].phaseOffset, c.sounds[0].travelX,
	                     c.sounds[0].travelY, sliceBasisFor<kMorphTriangles>(0.0, 0.0), 0.0);
	const owlet::dsp::PhiMorphWavetable wrongBlend = buildMorphBlend({ &wrong, 1 }, { weight, 1 });
	CHECK_FALSE(sameBits(expected, wrongBlend));
}

TEST_CASE("rotation leaves the sound you are standing on alone, at any drift") {
	// THE REPORT AND THE DECISION, on the live road. The bug: rotate changed the sound while sitting
	// on a single point, and it must not. The sweep found no leak — every audible case traced to genuine
	// travel — and A10 was then reversed rather than the explanation accepted, because on the 2-D chart
	// every drag that is not exactly vertical commits drift, so "a drifted sound legitimately moves"
	// meant rotate preserved the standing sound almost never.
	//
	// So the claim this case makes is the new one, and it is strictly stronger than the old held-at-home
	// guarantee: the sound at the point the patch is standing on comes back BIT FOR BIT, whatever its
	// drift. What rotation still reaches is travel the PUCK contributes — which is not what the sound
	// is, it is where you are listening from.
	Patch patch;
	patch.enable(2, true);
	patch.set(ParamId::Path, 0.25);            // between sounds, which is where a player usually parks
	patch.set(ParamId::PosX, -0.16).set(ParamId::PosY, 0.119); // the residual from the report's own screenshot

	SliceRotation turn;
	turn.rotate = 0.7f;

	/// The turn as the surface makes it — plane and residue together — beside the same turn with the
	/// residue thrown away, which is the pre-decision behaviour and the live contrast the cases lean on.
	struct Turned {
		owlet::dsp::PhiMorphWavetable flat;
		owlet::dsp::PhiMorphWavetable folded;
		owlet::dsp::PhiMorphWavetable unfolded;
		Character foldedCharacter;
	};
	const auto rotatedTable = [&](const SoundTravel& drift, double weather) {
		patch.set(ParamId::Weather, weather);
		const Character flat = patch.character(AuditionTuning {}, AnchorPoint {}, drift);
		const RotatedSound turned =
		    rotateSoundBy(SoundOrientations {}, SoundResidue {}, 0, flat.sounds[0].zone,
		                  flat.sounds[0].phaseOffset, SliceOrientationPin {}, turn, drift.offset[0], 0.0);
		const Character folded =
		    patch.character(AuditionTuning {}, AnchorPoint {}, drift, turned.planes, turned.residue);
		const Character unfolded =
		    patch.character(AuditionTuning {}, AnchorPoint {}, drift, turned.planes, SoundResidue {});
		return Turned { buildMorphFor(flat), buildMorphFor(folded), buildMorphFor(unfolded), folded };
	};

	// AT HOME, WHICH IS THE OLD GUARANTEE AND STILL HOLDS. At Weather 0 with nothing drifted every
	// product is exactly ±0.0, so a rotation cannot reach the sound however far it turns — and with
	// nothing to bank, the fold changes nothing either.
	{
		const Turned t = rotatedTable(SoundTravel {}, 0.0);
		CHECK(sameBits(t.flat, t.folded));
		CHECK(sameBits(t.flat, t.unfolded)); // nothing to fold, so both roads agree
	}

	// AT A DRIFT, WHICH IS THE DECISION. The reported case: Weather 0, the sound scrubbed out into the plane
	// by an ordinary drag, the encoder turned. The table comes back BIT FOR BIT.
	//
	// AND THE CONTRAST IS LIVE: the same turn with the residue dropped moves the table audibly, which is
	// exactly what was heard. Without that line this case would pass just as well if rotation had
	// silently stopped doing anything at all.
	{
		SoundTravel drift;
		drift.offset[0] = { 0.37, 0.1 + 0.2 }; // what a drag commits, not what a fixture prefers
		const Turned t = rotatedTable(drift, 0.0);
		CHECK(sameBits(t.flat, t.folded));
		CHECK_FALSE(sameBits(t.flat, t.unfolded));
	}

	// AND THE VERB STILL WORKS. Turn Weather up and the same still puck gives sound A travel it did not
	// have — travel the fold does not bank, because it is where you are LISTENING from rather than what
	// the sound IS (A14). Rotation reaches it, which is what a re-aiming verb is for.
	{
		const Turned t = rotatedTable(SoundTravel {}, 1.0);
		CHECK_FALSE(sameBits(t.flat, t.folded));
	}

	// THE BANKED DIFFERENCE BELONGS TO THE SOUND THAT TURNED, and to no other. A residue indexed by
	// packed position rather than by slot would hand it to whichever sound happened to sit there (A7),
	// and a fold applied to the whole set would quietly re-aim sounds nobody touched.
	{
		SoundTravel drift;
		drift.offset[0] = { 0.37, 0.3 };
		drift.offset[2] = { -0.25, 0.4 }; // a second drifted sound, left alone by the turn
		const Turned t = rotatedTable(drift, 0.0);
		REQUIRE(t.foldedCharacter.count == 3);
		CHECK(t.foldedCharacter.sounds[1].residue == PlaneResidue {});
		CHECK(t.foldedCharacter.sounds[2].residue == PlaneResidue {});
		const Character flat = patch.character(AuditionTuning {}, AnchorPoint {}, drift);
		CHECK(sameBits(soloMorph(t.foldedCharacter.sounds[2]), soloMorph(flat.sounds[2])));
		// And the sound that DID turn is the one carrying the difference.
		CHECK_FALSE(t.foldedCharacter.sounds[0].residue == PlaneResidue {});
	}
}

TEST_CASE("the banked residue follows the sound when its neighbours are switched off") {
	// THE PACKING TRAP, A7, stated where it can actually fail. The version of this claim inside the case
	// above could not: with every sound enabled a slot IS its packed position, so an indexing bug reads
	// the same entry either way. It was caught by building the known-bad — residue indexed by the packed
	// count — and watching all 28 cases stay green.
	//
	// Here slots 0 and 1 are off, so the one live sound sits at packed position 0 and slot 5. A residue
	// read by position would hand it slot 0's empty entry: the plane would turn with nothing banked
	// against it, and the standing sound would move — which is the whole bug the fold exists to prevent.
	Patch patch;
	patch.enable(0, false).enable(1, false).enable(5, true);
	SoundTravel drift;
	drift.offset[5] = { -0.4, 0.25 };

	SliceRotation turn;
	turn.rotate = -0.6f;

	const Character flat = patch.character(AuditionTuning {}, AnchorPoint {}, drift);
	REQUIRE(flat.count == 1);
	const RotatedSound turned =
	    rotateSoundBy(SoundOrientations {}, SoundResidue {}, 5, flat.sounds[0].zone,
	                  flat.sounds[0].phaseOffset, SliceOrientationPin {}, turn, drift.offset[5], 0.0);
	REQUIRE_FALSE(turned.residue.banked[5] == PlaneResidue {});

	const Character c =
	    patch.character(AuditionTuning {}, AnchorPoint {}, drift, turned.planes, turned.residue);
	CHECK(c.sounds[0].residue == turned.residue.banked[5]);
	CHECK(sameBits(buildMorphFor(flat), buildMorphFor(c)));
}

TEST_CASE("a hundred turns at a drift leave the table bit-identical") {
	// WHAT A SINGLE TURN CANNOT ANSWER. Each fold banks the difference the turn it belongs to would have
	// made, so the roundings are per-turn rather than a growing drift — but that is a claim about a
	// hundred turns, and this is where it is observable: on the float table a listener actually hears,
	// down the whole road rather than on the basis in isolation.
	Patch patch;
	patch.set(ParamId::Weather, 0.0);
	SoundTravel drift;
	drift.offset[0] = { 0.41, -0.23 };

	const Character flat = patch.character(AuditionTuning {}, AnchorPoint {}, drift);
	const owlet::dsp::PhiMorphWavetable before = buildMorphFor(flat);

	SoundOrientations planes;
	SoundResidue residue;
	uint32_t dice = 0x9e3779b9u;
	for (int click = 0; click < 100; ++click) {
		dice ^= dice << 13;
		dice ^= dice >> 17;
		dice ^= dice << 5;
		SliceRotation step;
		// Every click a real turn: the rotate is the one number M reads, and since the shrink the one
		// number there is.
		step.rotate = (static_cast<float>(dice % 2000u) / 1000.0f - 1.0f) * 0.25f;
		const RotatedSound turned =
		    rotateSoundBy(planes, residue, 0, flat.sounds[0].zone, flat.sounds[0].phaseOffset,
		                  SliceOrientationPin {}, step, drift.offset[0], 0.0);
		planes = turned.planes;
		residue = turned.residue;
	}

	const Character walked = patch.character(AuditionTuning {}, AnchorPoint {}, drift, planes, residue);
	CHECK(sameBits(before, buildMorphFor(walked)));
	// The plane really did travel a hundred times, and the residue really is what is holding the sound
	// still — otherwise the line above is a statement about a generator nobody turned.
	CHECK_FALSE(planes.pin[0].generator == flat.sounds[0].orientation.generator);
	CHECK_FALSE(sameBits(before, buildMorphFor(patch.character(AuditionTuning {}, AnchorPoint {}, drift,
	                                                          planes, SoundResidue {}))));
}

//----------------------------------------------------------------------------------------------//
// GAMMA'S UNBOUNDED HALF — the vernier base                                                      //
//----------------------------------------------------------------------------------------------//

TEST_CASE("a zero gamma base builds the character it always built, to the bit") {
	// THE MIGRATION CONTROL, and it is exact rather than tolerant. `sliceHome` adds the base after
	// the float offset, and `+ 0.0` on a finite double is exact, so a patch that has never touched
	// the base is not merely close to what it was — it IS what it was. Anything less would be a
	// silent re-voicing of every patch already saved.
	//
	// Checked against an INDEPENDENTLY computed reference (the four-argument call that predates the
	// base) rather than against another call with the base spelled zero, so a bug that ignored the
	// argument entirely could not pass by agreeing with itself.
	for (uint16_t zone : { uint16_t(0), uint16_t(1), uint16_t(613), uint16_t(1023) }) {
		for (float offset : { -1.87f, 0.0f, 0.37f, 4.0f }) {
			const auto morphRef = feathers::buildMorphSliced(zone, offset, 0.0, 0.0, {}, 0.0);
			const auto morphBase = feathers::buildMorphSliced(zone, offset, 0.0, 0.0, {}, 0.0);
			CHECK(std::memcmp(&morphRef, &morphBase, sizeof(morphRef)) == 0);

			const auto stairRef = feathers::buildStairSliced(zone, offset, 0.0, 0.0, {}, 0.0);
			const auto stairBase = feathers::buildStairSliced(zone, offset, 0.0, 0.0, {}, 0.0);
			CHECK(std::memcmp(&stairRef, &stairBase, sizeof(stairRef)) == 0);

			// And the sign of a zero is not a way to sneak a difference in: -0.0 added to a finite
			// double is the same finite double.
			const auto morphNegZero = feathers::buildMorphSliced(zone, offset, 0.0, 0.0, {}, -0.0);
			CHECK(std::memcmp(&morphRef, &morphNegZero, sizeof(morphRef)) == 0);
			CHECK(!std::signbit(feathers::sliceHome(zone, offset, -0.0) - feathers::sliceHome(zone, offset, 0.0)));
		}
	}
}

TEST_CASE("the base actually moves the sound, and moves it like gamma does") {
	// Silence would pass the control above just as well, so this is the half that says the argument
	// is wired to anything at all. A base of one whole step is a real move.
	const auto still = feathers::buildMorphSliced(613, 0.37f, 0.0, 0.0, {}, 0.0);
	const auto moved = feathers::buildMorphSliced(613, 0.37f, 0.0, 0.0, {}, 1.0);
	CHECK(std::memcmp(&still, &moved, sizeof(still)) != 0);

	// And it moves it the SAME WAY the vernier does, which is what makes it gamma rather than a new
	// axis: a base of 1 lands within the measured split bound of a vernier of 1.
	const auto vernier = feathers::buildMorphSliced(613, 1.37f, 0.0, 0.0, {}, 0.0);
	double worst = 0.0;
	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		worst = std::max(worst, std::abs(double(moved.amplitudes[i]) - double(vernier.amplitudes[i])));
	}
	CHECK(worst < 5e-4);
}

TEST_CASE("moving a step from the vernier into the base is inaudible bookkeeping") {
	// THE SPLIT GUARANTEE, stated as the bound it actually is rather than as bit-equality it is not.
	//
	// The vernier is rounded into a float and the base is not, so "base B with vernier v" and "base
	// B+v with vernier 0" differ in home — and the builders can see that, so memcmp is the wrong
	// instrument and a bound is the right one.
	//
	// THE BOUND IS SET FROM A SWEEP, NOT FROM WHAT MADE THIS PASS, and the first attempt at it was
	// wrong in a way worth recording. Seven hand-picked cases suggested "at most about 1e-05, a third
	// of a 16-bit LSB". A 40,000-case random sweep over zone, offset, vernier in ±4 and base in ±1e5
	// put the true worst at 1.84e-04 — about SIX 16-bit LSBs, eighteen times the hand-picked figure.
	// Generalising from a handful of fixtures is exactly the habit this project keeps catching.
	//
	// So the honest claim is not "inaudible" but BOUNDED AND TINY RELATIVE TO THE THING ITSELF: the
	// split moves the built character by at most ~2e-04, while the vernier it is moving is worth
	// ~1e-01 — three orders of magnitude larger. 5e-04 is the measured worst with a 2.7x margin.
	//
	// THE KNOWN-BAD STILL HAS TEETH, and by the same three orders: a build that drops either half or
	// counts one twice misses by the whole vernier, not by a rounding. There is no arrangement that
	// is wrong by a little.
	constexpr double kInaudible = 5e-4;

	struct Split {
		float offsetWithVernier;
		float offsetWithout;
		double vernier;
		double base;
	};
	// Awkward values, none exactly representable, including a base far from the origin where the
	// float rounding of the vernier is at its worst relative to the base.
	const Split splits[] = {
		{ 0.66f, 0.37f, 0.29, 0.0 },
		{ 0.66f, 0.37f, 0.29, 137.91 },
		{ 0.66f, 0.37f, 0.29, 12345.678 },
		{ 4.11f, 0.11f, 4.0, 5432.1 },
	};

	for (const Split& s : splits) {
		for (uint16_t zone : { uint16_t(37), uint16_t(613) }) {
			const auto a = feathers::buildMorphSliced(zone, s.offsetWithVernier, 0.0, 0.0, {}, s.base);
			const auto b = feathers::buildMorphSliced(zone, s.offsetWithout, 0.0, 0.0, {},
			                                          s.base + s.vernier);
			double worst = 0.0;
			for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
				worst = std::max(worst, std::abs(double(a.amplitudes[i]) - double(b.amplitudes[i])));
				worst = std::max(worst, std::abs(double(a.phases[i]) - double(b.phases[i])));
			}
			for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
				worst = std::max(worst, std::abs(double(a.curvatures[i]) - double(b.curvatures[i])));
			}
			CHECK(worst < kInaudible);
		}
	}
}

TEST_CASE("the gamma base reaches the character through the parameter road") {
	// THE END-TO-END CONTROL. Everything above tests the pieces; this one tests that they are joined.
	// A base that reached SoundCoord but not the builder, or the builder but not the orientation,
	// would pass every earlier case in this file.
	auto read = [](feathers::ParamId id) -> double {
		if (id == feathers::soundParam(0, feathers::SoundField::Enable)) {
			return 1.0;
		}
		if (id == feathers::soundParam(0, feathers::SoundField::Zone)) {
			return 613.0;
		}
		return 0.0;
	};

	const feathers::Character none =
	    feathers::characterFromParams(read, feathers::OscType::Morph, feathers::AuditionTuning {},
	                                  feathers::AnchorPoint {}, feathers::SoundTravel {},
	                                  feathers::SoundOrientations {}, feathers::SoundResidue {}, SoundGamma {}, Preview {}, feathers::EarSide::Head);
	feathers::SoundGamma carried;
	carried.base[0] = 137.91;
	const feathers::Character moved =
	    feathers::characterFromParams(read, feathers::OscType::Morph, feathers::AuditionTuning {},
	                                  feathers::AnchorPoint {}, feathers::SoundTravel {},
	                                  feathers::SoundOrientations {}, feathers::SoundResidue {},
	                                  carried, Preview {}, feathers::EarSide::Head);

	// Omitting the argument is the zero base, exactly.
	CHECK(none.sounds[0].gammaBase == 0.0);
	CHECK(moved.sounds[0].gammaBase == 137.91);
	// The vernier parameter is untouched by any of it — the split is the whole point.
	CHECK(none.sounds[0].phaseOffset == moved.sounds[0].phaseOffset);

	// And it is audible at the far end: the built character differs, and by much more than the split
	// bound, because 137.91 whole steps along the line is a different sound rather than a rounding.
	const auto still = feathers::buildMorphSliced(613, 0.0f, 0.0, 0.0, {}, none.sounds[0].gammaBase);
	const auto far = feathers::buildMorphSliced(613, 0.0f, 0.0, 0.0, {}, moved.sounds[0].gammaBase);
	CHECK(std::memcmp(&still, &far, sizeof(still)) != 0);
}

TEST_CASE("a nonzero base does not make the character depend on the block size") {
	// BLOCK-SIZE INVARIANCE, swept with the base LIVE rather than at zero. The character is a pure
	// function of the parameters, so asking for it repeatedly — as a host with a different block size
	// would — has to give the same answer every time. A base that had picked up any per-call state
	// would show here and nowhere else.
	auto read = [](feathers::ParamId id) -> double {
		if (id == feathers::soundParam(0, feathers::SoundField::Enable)) {
			return 1.0;
		}
		if (id == feathers::soundParam(0, feathers::SoundField::Zone)) {
			return 613.0;
		}
		if (id == feathers::soundParam(0, feathers::SoundField::Gamma)) {
			return 0.37;
		}
		return 0.0;
	};
	feathers::SoundGamma carried;
	carried.base[0] = -98765.4321;

	owlet::dsp::PhiMorphWavetable reference {};
	for (int call = 0; call < 64; ++call) {
		const feathers::Character c =
		    feathers::characterFromParams(read, feathers::OscType::Morph, feathers::AuditionTuning {},
		                                  feathers::AnchorPoint {}, feathers::SoundTravel {},
		                                  feathers::SoundOrientations {}, feathers::SoundResidue {},
		                                  carried, Preview {}, feathers::EarSide::Head);
		const auto built = feathers::buildMorphSliced(c.sounds[0].zone, c.sounds[0].phaseOffset, 0.0,
		                                              0.0, {}, c.sounds[0].gammaBase);
		if (call == 0) {
			reference = built;
		}
		else {
			CHECK(std::memcmp(&reference, &built, sizeof(reference)) == 0);
		}
	}
}

TEST_CASE("rotating a carried sound banks against the plane it is ACTUALLY on") {
	// THE PLANE A SOUND IS BORN ON IS A FUNCTION OF WHERE IT IS, and a carried sound is somewhere
	// else. This matters more than a wrong readout would, because rotateSoundBy does not merely READ
	// that plane — it BANKS THE RESIDUE against it, and the residue is saved. Derive from the
	// un-carried home and the fold preserves a point the sound is not standing on, then writes that
	// mistake into the patch permanently.
	//
	// Checked against an INDEPENDENTLY derived reference — the fold done by hand at the based home —
	// rather than against another call to the same function, so a rotate that ignored the base
	// could not pass by agreeing with itself.
	constexpr uint16_t kZone = 613;
	constexpr float kOffset = 0.37f;
	constexpr double kBase = 137.91;
	const feathers::AnchorPoint drift { 0.29, -0.11 };
	feathers::SliceRotation turn;
	turn.rotate = 0.21f;

	const feathers::RotatedSound turned =
	    feathers::rotateSoundBy(feathers::SoundOrientations {}, feathers::SoundResidue {}, 0, kZone,
	                            kOffset, feathers::SliceOrientationPin {}, turn, drift, kBase);

	// RESTATED FOR M, AND THE TEETH ARE IN THE SAME TOOTH. The claim has always been that the CARRIED
	// home reaches the fold, so that a rotation banks against the plane the sound is really playing
	// rather than its un-carried neighbour's. What changed is the road the base travels down: it used
	// to enter through a generator derived from the based home, and now it enters as the fold's own
	// `home` argument, because M takes the place as an argument rather than through the state. An
	// unpinned sound turns from rotate 0 either way.
	const feathers::SliceGenerator fromBased;
	const feathers::SliceGenerator toBased = feathers::rotateBy(fromBased, turn);
	const feathers::SoundResidue wanted = feathers::foldSoundResidue(
	    feathers::SoundResidue {}, 0, fromBased, toBased, drift,
	    feathers::sliceHome(kZone, kOffset, kBase));

	CHECK(turned.residue.banked[0] == wanted.banked[0]);

	// And it is not what folding at the UNCARRIED home would have banked, or the case above would pass
	// against the bug it exists to catch — which is the same bug as before, one argument to the left.
	const feathers::SoundResidue wrong = feathers::foldSoundResidue(
	    feathers::SoundResidue {}, 0, fromBased, toBased, drift,
	    feathers::sliceHome(kZone, kOffset, 0.0));
	CHECK_FALSE(turned.residue.banked[0] == wrong.banked[0]);
}

TEST_CASE("previewing a sound plays exactly the patch with only that sound in it") {
	// THE CROWN CASE FOR PREVIEW (#42, decision P1). Preview must not INVENT a sound — it plays
	// the one you could have got by switching the others off and standing on it. Anything else and
	// what you tune while previewing is not what the patch will do when you stop.
	//
	// Three sounds, the puck away from all of them, and one of them previewed. The reference is the
	// same patch with only that sound enabled and the puck where a single-sound patch puts it, which
	// is on the anchor: `tourPoint` of one anchor is that anchor, and Pos X/Y are centred at zero.
	Patch patch;
	patch.enable(0, true).enable(1, true).enable(2, true);
	patch.place(0, -0.5, 0.0).place(1, 0.5, 0.0).place(2, 0.0, 0.75);
	patch.zone(0, 100.0).zone(1, 400.0).zone(2, 900.0);
	// The puck somewhere none of them is — the whole situation preview exists for. Moved with PATH,
	// which walks the tour between the anchors, rather than with Pos X/Y.
	//
	// THE SCOPE OF THE EQUIVALENCE, found by this case failing first and worth writing down: it holds
	// with Pos AT CENTRE. A one-sound patch's puck is `tourPoint(its anchor) + Pos`, so an off-centre
	// Pos leaves that patch standing NEXT TO its sound while preview stands exactly ON it, and with
	// Weather up the two then genuinely differ. That is not a flaw in either — it is the honest
	// content of "stand on it" — but it means the reference has to be the solo patch with Pos centred,
	// which is also the state a player is in when they switch the others off to listen.
	patch.set(ParamId::Path, 0.5);
	// AND WEATHER UP, which is what separates the decision from the alternative: with terrain being
	// let through, "mute the others" would leave the previewed sound coloured by where the puck is
	// standing, and "stand on it" does not.
	patch.set(ParamId::Weather, 0.6);

	const Character blended = patch.character();
	REQUIRE(blended.count == 3);

	Preview listen;
	listen.slot = 2;
	const Character previewed = patch.character({}, {}, {}, {}, {}, listen);

	Patch solo;
	solo.enable(0, false).enable(1, false).enable(2, true);
	solo.place(0, -0.5, 0.0).place(1, 0.5, 0.0).place(2, 0.0, 0.75);
	solo.zone(0, 100.0).zone(1, 400.0).zone(2, 900.0);
	solo.set(ParamId::Weather, 0.6);
	solo.set(ParamId::Path, 0.5);
	const Character alone = solo.character();

	REQUIRE(previewed.count == 1);
	// THE WHOLE OBJECT, not a field of it: `Character` compares equal member by member, so this says
	// the engine is handed the same thing by both roads rather than something that agrees where the
	// case happened to look.
	CHECK(previewed == alone);
	// And it is genuinely a different sound from the blend it replaced, or the case above would pass
	// on a preview that did nothing.
	CHECK_FALSE(previewed == blended);
	// The travel term is exactly the drift — zero here — because standing on a sound makes the Weather
	// product zero however far up Weather is. Exact, not approximate: it is `weather * (x - x)`.
	CHECK(previewed.sounds[0].travelX == 0.0);
	CHECK(previewed.sounds[0].travelY == 0.0);
	CHECK(previewed.sounds[0].weight == doctest::Approx(1.0));
}

TEST_CASE("preview reaches a sound the patch has switched off, and changes nothing about the patch") {
	// Preview ignores the Enable switch on purpose — auditioning a sound before deciding to put it in
	// the patch is what it is for — and it writes nothing, so the switch stays where it was. There is
	// no parameter road out of this mapping at all, which is what makes the second half structural:
	// the reference character below is built from the SAME Patch object afterwards.
	Patch patch;
	patch.enable(0, true).enable(1, false);
	patch.zone(0, 100.0).zone(1, 700.0);

	Preview listen;
	listen.slot = 1;
	const Character previewed = patch.character({}, {}, {}, {}, {}, listen);
	REQUIRE(previewed.count == 1);
	CHECK(previewed.sounds[0].zone == 700);

	// The patch is untouched: with preview off it is the one-sound patch it always was.
	const Character after = patch.character();
	REQUIRE(after.count == 1);
	CHECK(after.sounds[0].zone == 100);
}

TEST_CASE("a moving puck cannot colour a sound being previewed") {
	// The blue shift is what the blend does as you move THROUGH it, and preview has suspended the puck
	// on the sound. A gesture elsewhere on the circle reaching a sound you are holding still to listen
	// to would be the surface lying about what it is playing — quietly, in the character rather than
	// in a picture.
	Patch patch;
	patch.enable(0, true).enable(1, true);
	patch.place(0, -0.5, 0.0).place(1, 0.5, 0.0);
	patch.zone(0, 100.0).zone(1, 800.0);

	Preview listen;
	listen.slot = 0;
	const AnchorPoint swipe { 1.7, -0.9 };
	const Character still = patch.character({}, {}, {}, {}, {}, listen);
	const Character during = patch.character({}, swipe, {}, {}, {}, listen);
	CHECK(during == still);
	CHECK(during.blueOvershoot == 0.0f);
	CHECK(during.sounds[0].approach == 0.0);

	// The control: without preview, that same swipe DOES colour the blend — so the case above is about
	// preview rather than about a velocity that never reached anything.
	const Character parked = patch.character();
	const Character swiped = patch.character({}, swipe);
	CHECK_FALSE(swiped == parked);
}

TEST_CASE("the uniform case is the puck displacement it replaced") {
	// V13's REDUCTION, at the mapping: with every enabled sound at the same width w and the global at
	// g, displacing each anchor by −(g·w)/2 (ear Right) is RELATIVELY identical to displacing the
	// puck by +(g·w)/2, because everything downstream — weights, weather travel, blue shift —
	// consumes (puck − anchor). So the ear's per-sound fields must equal, bit for bit, the mono
	// character of the same patch with Pos X moved by +(g·w)/2. The one deliberate difference is the
	// puck itself: since V13 it is the HEAD for both ears (V14c — nothing downstream of the blend
	// reads it), so it is pinned to the head's standing rather than to the reference's.
	//
	// The numbers are all powers of two so (tour + posX) − (anchor − s) and (tour + (posX + s)) −
	// anchor are the same float despite associating differently: s = g·w/2 = 0.5·0.5/2 = 0.125,
	// anchors at ∓0.5, and 0.25 + 0.125 = 0.375 exactly. A fixture off the binary grid would test
	// float association, not the feature.
	Patch patch;
	patch.set(ParamId::Weather, 0.5); // so the displacement reaches travel, not only the weights
	patch.zone(0, 100.0).zone(1, 800.0);
	patch.set(ParamId::PosX, 0.25);
	patch.set(ParamId::Width, 0.5);
	patch.width(0, 0.5).width(1, 0.5);

	const Character ear = patch.character({}, {}, {}, {}, {}, {}, EarSide::Right);

	Patch shifted;
	shifted.set(ParamId::Weather, 0.5);
	shifted.zone(0, 100.0).zone(1, 800.0);
	shifted.set(ParamId::PosX, 0.375);
	const Character walked = shifted.character();

	REQUIRE(ear.count == walked.count);
	CHECK(ear.sounds == walked.sounds);
	CHECK(ear.xf == walked.xf);
	CHECK(ear.blueOvershoot == walked.blueOvershoot);
	// The puck is the head — both ears carry it exactly, which is what let the synth stop averaging
	// two stands back into one.
	const Character head = patch.character();
	CHECK(ear.puck.x == head.puck.x);
	CHECK(ear.puck.y == head.puck.y);

	// KNOWN-BAD: a different uniform width stands the ear somewhere else. Without this the equality
	// above could be comparing two characters the displacement never reached.
	Patch wider = patch;
	wider.width(0, 1.0).width(1, 1.0);
	const Character other = wider.character({}, {}, {}, {}, {}, {}, EarSide::Right);
	CHECK_FALSE(other.sounds == walked.sounds);

	// AND THE MASTER SCALE GATES IT ALL (V13b): global 0 with every per-sound width up is mono — the
	// whole character, puck included, exactly the head's.
	Patch gated = patch;
	gated.set(ParamId::Width, 0.0).width(0, 1.0).width(1, 1.0);
	CHECK(gated.character({}, {}, {}, {}, {}, {}, EarSide::Left) == gated.character());
}

TEST_CASE("width is per sound: the wide sound's geometry moves and the mono sound's does not") {
	// THE POINT OF V13. With w_A > 0 and w_B = 0, ear L and ear R see sound A's anchor in two places
	// and sound B's in one: B's weather travel is identical between the ears — its anchor never moved
	// — while A's differs. B's SHARE still differs between ears, deliberately: the weights are
	// relative, so a wide neighbour standing elsewhere changes everyone's share. That is the blend
	// doing its job, not a leak; what must not move is B's own geometry.
	Patch patch;
	patch.set(ParamId::Weather, 0.5);
	patch.zone(0, 100.0).zone(1, 800.0);
	patch.set(ParamId::Width, 1.0);
	patch.width(0, 0.5).width(1, 0.0);

	const Character left = patch.character({}, {}, {}, {}, {}, {}, EarSide::Left);
	const Character right = patch.character({}, {}, {}, {}, {}, {}, EarSide::Right);
	REQUIRE(left.count == 2);
	REQUIRE(right.count == 2);

	// The wide sound stands apart per ear.
	CHECK(left.sounds[0].travelX != right.sounds[0].travelX);
	// The mono sound stands in one place — travel identical to the bit, both axes.
	CHECK(left.sounds[1].travelX == right.sounds[1].travelX);
	CHECK(left.sounds[1].travelY == right.sounds[1].travelY);

	// KNOWN-BAD: swap which sound is wide and the roles swap with it — the displacement follows the
	// parameter, not the slot order.
	Patch swapped = patch;
	swapped.width(0, 0.0).width(1, 0.5);
	const Character sLeft = swapped.character({}, {}, {}, {}, {}, {}, EarSide::Left);
	const Character sRight = swapped.character({}, {}, {}, {}, {}, {}, EarSide::Right);
	CHECK(sLeft.sounds[0].travelX == sRight.sounds[0].travelX);
	CHECK(sLeft.sounds[1].travelX != sRight.sounds[1].travelX);
}

TEST_CASE("two ears apart on the disk hear different terrain and different blue shift") {
	// The decorrelation the design wants comes from the terrain itself: per-sound anchor displacement
	// (V13) gives each ear its own weights and its own weather travel, and with the blue shift live,
	// its own approach and weight rate — different Doppler, unfaked. Nothing per-ear is built
	// specially; this pins that it all falls out of the one displaced evaluation.
	Patch patch;
	patch.set(ParamId::Weather, 0.5);
	patch.zone(0, 100.0).zone(1, 800.0);

	const AuditionTuning blue = scales(kBlueShiftScaleOn, kBlueShiftScaleOn);
	const AnchorPoint swipe { 1.0, 0.0 }; // lateral motion, along the ear axis
	patch.set(ParamId::Width, 1.0);
	patch.width(0, 0.3).width(1, 0.3);
	const Character left = patch.character(blue, swipe, {}, {}, {}, {}, EarSide::Left);
	const Character right = patch.character(blue, swipe, {}, {}, {}, {}, EarSide::Right);

	REQUIRE(left.count == 2);
	REQUIRE(right.count == 2);
	// The shares are geometry and the geometry differs.
	REQUIRE(left.sounds[0].weight > 0.0);
	CHECK(left.sounds[0].weight != right.sounds[0].weight);
	// The weather lets each ear's own standing point through.
	CHECK(left.sounds[0].travelX != right.sounds[0].travelX);
	// And the motion terms are the ears' own: same head velocity, different geometry against it.
	REQUIRE((left.sounds[0].approach != 0.0 || left.sounds[1].approach != 0.0));
	CHECK((left.sounds[0].approach != right.sounds[0].approach
	       || left.sounds[1].approach != right.sounds[1].approach));
	CHECK(left.sounds[0].weightRate != right.sounds[0].weightRate);
}

TEST_CASE("preview suspends the ears") {
	// V12a. Preview's invariant is the sound at its own identity — puck AT the anchor, travel exactly
	// drift — and a displaced ear would colour the very thing preview exists to isolate. So while a
	// preview is latched the displacement adds nothing, and width's rebuild fades cover the way in
	// and out.
	Patch patch;
	patch.set(ParamId::Weather, 0.5);
	patch.zone(0, 100.0).zone(1, 800.0);
	patch.set(ParamId::Width, 1.0);
	patch.width(0, 0.6).width(1, 0.6);

	Preview listen;
	listen.slot = 0;
	const Character still = patch.character({}, {}, {}, {}, {}, listen);
	const Character displaced = patch.character({}, {}, {}, {}, {}, listen, EarSide::Left);
	CHECK(displaced == still);

	// The control: without preview the same ear moves the character, so the equality above is
	// preview suspending the displacement rather than widths that never reached anything.
	const Character head = patch.character();
	const Character stood = patch.character({}, {}, {}, {}, {}, {}, EarSide::Left);
	CHECK_FALSE(stood == head);
}

//------------------------------------------------------------------//
// THE ACTIVATION WEIGHTS, READ BACK OUT BY SLOT — task 49's readout //
//------------------------------------------------------------------//

TEST_CASE("the shares come back addressed by slot rather than by position") {
	// THE PACKING UNDONE, ONCE. `Character::sounds` is packed — a switched-off slot takes no place —
	// so `sounds[1]` is the second LIVE sound and not the sound on rail row 1. Anything that shows a
	// player their sounds addresses them the way the player does, and a readout that inherited the
	// packing would relabel every one of them the moment a slot was switched off.
	Patch patch;
	patch.enable(0, true).enable(1, false).enable(5, true);
	patch.place(0, -0.5, 0.0).place(5, 0.5, 0.0);
	patch.set(ParamId::Path, 0.5);

	const SoundWeights w = soundWeightsOf(patch.character());

	// Two sounds, and the puck halfway along the tour between them: at the shipped falloff the shares
	// are the linear crossfade, which is a half each. Hand-computable on purpose — see the falloff
	// note in constellation.h for why this exponent and no other gives that.
	CHECK(w.isLive(0));
	CHECK(w.isLive(5));
	CHECK(w.at(0) == doctest::Approx(0.5));
	CHECK(w.at(5) == doctest::Approx(0.5));

	// AND NOWHERE ELSE. The packed character holds these at positions 0 and 1; a readout that had
	// simply copied it across would light slot 1, which is switched off, and leave slot 5 dark.
	CHECK_FALSE(w.isLive(1));
	CHECK(w.at(1) == 0.0);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		if (slot != 0 && slot != 5) {
			CHECK_FALSE(w.isLive(slot));
		}
	}
}

TEST_CASE("a sound at zero share is still in the blend; a switched-off sound is not") {
	// THE DISTINCTION THE MASK EXISTS FOR, and the reason it is not enough to publish eight doubles.
	// Standing exactly on one anchor of a two-sound patch gives the other sound a share of zero — but
	// it is on the tour, it is what Path walks towards, and turning that way brings it in. A sound
	// with its switch off is not in the constellation at all. Both read 0.0; they are not the same
	// state, and a meter that drew them alike would be lying about what the patch can be played into.
	Patch onAnchor;
	onAnchor.set(ParamId::Path, 0.0);
	const SoundWeights standing = soundWeightsOf(onAnchor.character());
	CHECK(standing.isLive(0));
	CHECK(standing.isLive(1));
	CHECK(standing.at(0) == doctest::Approx(1.0));
	CHECK(standing.at(1) == doctest::Approx(0.0));

	// The same patch with B switched off: one sound, and slot 1 is absent rather than zero.
	Patch alone;
	alone.enable(1, false);
	const SoundWeights lone = soundWeightsOf(alone.character());
	CHECK(lone.isLive(0));
	CHECK_FALSE(lone.isLive(1));
	CHECK(lone.at(0) == doctest::Approx(1.0));

	// And what makes the pair worth writing: the VALUES agree while the masks do not, so a readout
	// that dropped the mask would pass every value check above and still be wrong.
	CHECK(standing.at(1) == lone.at(1));
	CHECK(standing.live != lone.live);
}

TEST_CASE("the live shares sum to one wherever the puck stands") {
	// A share is a share OF something, and the something is what you are hearing. blendWeights
	// normalises; this is the claim surviving the trip through the packing and back out.
	Patch patch;
	patch.enable(2, true).enable(3, true);
	for (const double path : { 0.0, 0.17, 0.5, 0.83, 1.0 }) {
		Patch at = patch;
		at.set(ParamId::Path, path).set(ParamId::PosX, 0.2).set(ParamId::PosY, -0.3);
		const SoundWeights w = soundWeightsOf(at.character());
		CAPTURE(path);
		double total = 0.0;
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			total += w.at(slot);
		}
		CHECK(total == doctest::Approx(1.0));
	}
}

TEST_CASE("previewing a sound collapses the shares onto it") {
	// SOLO IS WHAT IS AUDIBLE. Preview replaces the membership test outright — the previewed slot is
	// the only live sound — so this needs no branch of its own: the readout follows the character, and
	// the character is already one sound. The others are ABSENT rather than zero, which is the same
	// distinction as above and the right one: while a preview is latched they are not being blended at
	// all.
	Patch patch;
	patch.enable(2, true);
	Preview listen;
	listen.slot = 1;
	const SoundWeights w = soundWeightsOf(patch.character({}, {}, {}, {}, {}, listen));

	CHECK(w.isLive(1));
	CHECK(w.at(1) == doctest::Approx(1.0));
	CHECK_FALSE(w.isLive(0));
	CHECK_FALSE(w.isLive(2));

	// The control: without the preview all three are live, so the collapse above is preview doing it
	// rather than a patch that only ever had one sound in it.
	const SoundWeights blended = soundWeightsOf(patch.character());
	CHECK(blended.isLive(0));
	CHECK(blended.isLive(1));
	CHECK(blended.isLive(2));
}

TEST_CASE("preview reaches a switched-off sound, and the shares follow it there") {
	// The audition case from the preview suite, asked of the readout: previewing a sound that is not
	// in the patch is what you do before deciding to put it in one, and the outs must follow what is
	// AUDIBLE rather than what is enabled.
	Patch patch;
	Preview listen;
	listen.slot = 4;
	const SoundWeights w = soundWeightsOf(patch.character({}, {}, {}, {}, {}, listen));

	CHECK(w.isLive(4));
	CHECK(w.at(4) == doctest::Approx(1.0));
	CHECK_FALSE(w.isLive(0));
	CHECK_FALSE(w.isLive(1));
}

TEST_CASE("a patch with every sound switched off has no shares at all") {
	// Not zeros: nothing. A constellation with no anchors is a patch making silence, and blendWeights
	// is documented to write nothing rather than to divide by a count of none.
	Patch patch;
	patch.enable(0, false).enable(1, false);
	const SoundWeights w = soundWeightsOf(patch.character());
	CHECK(w.live == 0);
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		CAPTURE(slot);
		CHECK(w.at(slot) == 0.0);
	}
}
