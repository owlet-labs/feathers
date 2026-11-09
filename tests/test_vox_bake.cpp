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

// THE NOTE, BAKED. What a position cannot answer — see dsp/vox_bake.h for why a vowel is the one
// engine here that is not position-pure, and for the parity boundary these cases sit inside.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/vox_bake.h"

#include <cmath>
#include <vector>

using namespace feathers;

namespace {

/// A hand-built vowel: one formant, `pulses` raised cosines of equal gain, at `hz`.
///
/// BY HAND RATHER THAN FROM THE TERRAIN, deliberately, for the cases that make an arithmetic claim:
/// a fixture read out of a φ field is a fixture whose numbers move the day the field is retuned,
/// and a case that says "the mean is zero" should fail when the mean stops being zero rather than
/// when a triangle's duty is adjusted. The terrain-fed cases below are marked as such and ask
/// different questions.
VoxParams oneFormant(float hz, int pulses, float gain = 0.62f) {
	VoxParams p;
	p.formant[0].frequencyHz = hz;
	for (int k = 0; k < pulses && k < static_cast<int>(kVoxMaxPulses); ++k) {
		p.formant[0].pulseGain[static_cast<size_t>(k)] = gain;
	}
	return p;
}

double meanOf(const VoxCycle& c) {
	double sum = 0.0;
	// Slot kVoxSlots is the wrap guard, a repeat of slot 0 — counting it would weight one sample
	// twice in a mean over one cycle.
	for (int i = 0; i < kVoxSlots; ++i) {
		sum += static_cast<double>(c.slots[static_cast<size_t>(i)]);
	}
	return sum / static_cast<double>(kVoxSlots);
}

/// How many separate humps the cycle has — the audible count of glottal pulses, read off the baked
/// waveform rather than off the gain table, which is the point: this is what a listener hears.
int humpsIn(const VoxCycle& c) {
	int humps = 0;
	for (int i = 1; i < kVoxSlots - 1; ++i) {
		const float prev = c.slots[static_cast<size_t>(i - 1)];
		const float here = c.slots[static_cast<size_t>(i)];
		const float next = c.slots[static_cast<size_t>(i + 1)];
		if (here > prev && here >= next && here > 0.02f) {
			humps++;
		}
	}
	return humps;
}

} // namespace

TEST_CASE("the bake is a function of the RATIO, so the same note bakes the same cycle at any scale") {
	// THE STRUCTURAL CLAIM, and it is stronger than V22a asked for. "The voice bakes the note" could
	// have meant a bake that takes a sample rate and an absolute pitch; it does not. The only thing
	// the note contributes is formantHz/fundamentalHz — how many formant periods fit in a cycle — so
	// a formant at 900 Hz over a 150 Hz note and one at 1800 over 300 are THE SAME CYCLE, bit for
	// bit. That is what makes the engine sample-rate independent without a single line about sample
	// rates, and what lets every case below name a ratio instead of a rig.
	VoxCycle low;
	VoxCycle high;
	bakeVoxCycle(oneFormant(900.0f, 5), 150.0, low);
	bakeVoxCycle(oneFormant(1800.0f, 5), 300.0, high);
	REQUIRE(low.voiced);
	REQUIRE(high.voiced);
	CHECK(low.slots == high.slots); // bit for bit, not approximately

	// And a DIFFERENT ratio is a different cycle, or the claim above would be vacuous — an
	// implementation that ignored the note entirely would pass the equality and fail here.
	VoxCycle other;
	bakeVoxCycle(oneFormant(900.0f, 5), 300.0, other);
	CHECK_FALSE(other.slots == low.slots);
}

TEST_CASE("a baked cycle carries no DC, which is what the compensation is for") {
	// A burst of raised cosines is STRICTLY ONE-SIGNED until the polarities alternate, so its mean
	// is emphatically not zero on its own: without compensation every cycle sits above the axis, and
	// the amp edges turn that standing offset into a click at note-on and note-off. The firmware
	// carries a DC term for exactly this and so does the bake.
	//
	// TWO KNOWN-BADS, both arithmetic rather than a second build. An UNCOMPENSATED burst's mean is
	// 0.5 * sum(gains) * min(Tformant/Tnote, 1) — computed below and required to be far outside the
	// tolerance, so a bake that dropped the correction entirely could not pass by accident. And the
	// FIRMWARE'S OWN FORMULA is the second: at eight and twelve pulses the burst outruns the cycle,
	// the formula subtracts pulses that never sound, and the measured means were -0.10 and -0.31
	// before this bake started measuring instead. Those two counts are in the list deliberately.
	for (const int pulses : { 1, 3, 8, 12 }) {
		CAPTURE(pulses);
		VoxCycle c;
		bakeVoxCycle(oneFormant(1200.0f, pulses), 200.0, c);
		REQUIRE(c.voiced);
		const double uncompensated = 0.5 * 0.62 * static_cast<double>(pulses) * (200.0 / 1200.0);
		REQUIRE(uncompensated > 0.05); // the term this case exists to catch is worth catching
		CAPTURE(uncompensated);
		CAPTURE(meanOf(c));
		CHECK(std::abs(meanOf(c)) < 0.01);
	}
}

TEST_CASE("the cycle meets itself at the seam, even when the burst is amputated by the wrap") {
	// THE FIRMWARE'S HARD-WON FIX, and the case that says why it is not optional. When the burst is
	// LONGER than the note period the cycle wrap cuts a pulse off mid-flight: a step discontinuity
	// every single cycle, which is a buzz rather than a click because it happens at the fundamental.
	// The release fades the last 1/32 of the cycle to zero so every truncation lands softly.
	//
	// Twelve pulses at 300 Hz over a 200 Hz note needs twelve formant periods where one and a half
	// cycles' worth exist — the burst is cut hard, which is the country this case is about.
	VoxCycle c;
	bakeVoxCycle(oneFormant(300.0f, 12), 200.0, c);
	REQUIRE(c.voiced);

	// The seam step, measured against the cycle's own SWING rather than against its steepest
	// interior step — and the first draft of this case got that frame wrong, which is worth
	// recording. "The seam is no worse than anywhere else" sounds like the right claim and is not
	// achievable: the release ramp is itself the steepest slope in the waveform, by construction,
	// because it takes the signal to zero in a thirty-second of a cycle. Measured, the two came out
	// at 0.0775 against 0.0773 — a hair apart, and the case failed for a reason that was about the
	// release working rather than failing.
	//
	// What the release actually promises is that the seam is a SMALL FRACTION OF THE WAVEFORM
	// instead of a full-height cliff. Without it this fixture would step by the amputated pulse's
	// own height, near the peak of the burst.
	const double seam = std::abs(static_cast<double>(c.slots[kVoxSlots - 1])
	                             - static_cast<double>(c.slots[0]));
	double lowest = c.slots[0];
	double highest = c.slots[0];
	for (int i = 0; i < kVoxSlots; ++i) {
		lowest = std::min(lowest, static_cast<double>(c.slots[static_cast<size_t>(i)]));
		highest = std::max(highest, static_cast<double>(c.slots[static_cast<size_t>(i)]));
	}
	const double swing = highest - lowest;
	CAPTURE(seam);
	CAPTURE(swing);
	REQUIRE(swing > 0.3); // a fixture with a real burst in it, or the ratio below means nothing
	CHECK(seam < 0.2 * swing);

	// The wrap guard is the seam, not a thirteenth value: slot 256 IS slot 0, because a cycle is
	// periodic and an evaluator reading across the end must not fall off a cliff of its own making.
	CHECK(c.slots[kVoxSlots] == c.slots[0]);
}

TEST_CASE("the vowel flattens as the note climbs past its own formants") {
	// THE AUDIBLE CONSEQUENCE OF FIXED-Hz FORMANTS, and the reason the engine is worth its
	// note-dependence. At a low fundamental many formant periods fit inside one cycle, so the burst
	// is long and the resonance is well resolved — a vowel. Climb until a formant period is longer
	// than the whole cycle and less than one pulse fits: the vowel flattens into the fundamental,
	// exactly as a sung note does when it passes above its own formants. A position-pure engine
	// CANNOT do this, which is the whole argument for V22a.
	const VoxParams p = oneFormant(900.0f, 6);
	int previousHumps = 1000;
	for (const double note : { 100.0, 200.0, 400.0, 900.0 }) {
		CAPTURE(note);
		VoxCycle c;
		bakeVoxCycle(p, note, c);
		REQUIRE(c.voiced);
		const int humps = humpsIn(c);
		CHECK(humps <= previousHumps); // never MORE resolved as the note rises
		previousHumps = humps;
	}
	// And the ends of that sweep really are different, or the monotone check above would hold
	// trivially for a bake that ignored the note.
	VoxCycle low;
	VoxCycle high;
	bakeVoxCycle(p, 100.0, low);
	bakeVoxCycle(p, 900.0, high);
	CHECK(humpsIn(low) > humpsIn(high));
}

TEST_CASE("a burst shorter than the cycle leaves silence behind it, not a repeated pulse") {
	// THE INDEX CLAMP'S REAL MEANING. The firmware sticks the pulse index at the last slot rather
	// than letting it run off the end of the gain table, which is a bounds fix — but what makes it
	// INAUDIBLE is the builder's zero padding: past the burst the gain is 0, so the stuck index
	// reads silence. If either half of that pairing broke, the tail of every short burst would
	// repeat its final pulse forever, which is a buzz where a glottal gap belongs.
	VoxCycle c;
	bakeVoxCycle(oneFormant(1600.0f, 2), 200.0, c); // eight periods fit; two are voiced
	REQUIRE(c.voiced);

	// The burst occupies the first quarter of the cycle (2 of 8 periods). The rest must be flat —
	// at the DC level, since compensation is subtracted from every slot including the silent ones.
	const double floorLevel = static_cast<double>(c.slots[static_cast<size_t>(kVoxSlots / 2)]);
	for (int i = kVoxSlots * 3 / 8; i < kVoxSlots * 7 / 8; ++i) {
		CAPTURE(i);
		CHECK(std::abs(static_cast<double>(c.slots[static_cast<size_t>(i)]) - floorLevel) < 1e-6);
	}
	// ...and the voiced quarter is emphatically not flat, or the check above is about a silent bake.
	double burstSwing = 0.0;
	for (int i = 0; i < kVoxSlots / 4; ++i) {
		burstSwing = std::max(burstSwing,
		                      std::abs(static_cast<double>(c.slots[static_cast<size_t>(i)]) - floorLevel));
	}
	CHECK(burstSwing > 0.1);
}

TEST_CASE("RASP COUNTRY: where the terrain grows its bursts, the baked cycle grows too") {
	// THE MEASUREMENT V-C3 OWES, and it is deliberately taken from the TERRAIN rather than by hand.
	// C1 pinned that growing-decay country exists in the φ field and that the GAIN TABLE grows
	// across it; that is a claim about a recipe. This is the claim about a SOUND: bake a cycle at
	// such a position and the burst must actually get louder as it goes. A recipe that grew while
	// its waveform did not would have satisfied C1 and been inaudible.
	//
	// BOTH FORMANTS ARE REQUIRED TO BE GROWING, which the first draft did not ask for and should
	// have. The waveform is a SUM: with only F1 selected for growth, F2's own decay is whatever the
	// field says there, and its faster ripples sit on top of F1's humps. Measured, the first draft
	// read peaks of 0.53 falling to 0.18 at a position whose F1 gains were genuinely growing — it
	// was measuring the wrong formant's contribution. A position where both grow is unambiguous.
	using owlet::dsp::phi::evalTriangle;
	uint16_t foundZone = 0;
	double foundGamma = 0.0;
	bool found = false;
	// A WIDE GAMMA SWEEP, and the width is a measurement rather than a habit: the two decay fields
	// are independent φ triangles with long periods, so the country where BOTH are in their top
	// sixth is rare and is not reachable at all within the first few units of home. Searching
	// gamma 0..20 finds it; searching 0..2, as the first draft did, finds nothing and would have
	// argued the claim away.
	for (int step = 0; step < 81 && !found; ++step) {
		const double gamma = static_cast<double>(step) * 0.25;
		for (int zone = 0; zone <= 1023 && !found; ++zone) {
			const double home = sliceHome(static_cast<uint16_t>(zone), 0.0f, gamma);
			const float b1 = 0.30f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxDecay1) * 0.88f;
			const float b2 = 0.30f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxDecay2) * 0.88f;
			const float n1 = 1.0f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxN1) * 11.0f;
			const float n2 = 1.0f + evalTriangle(home, 1.0f, owlet::dsp::kPhiVoxN2) * 11.0f;
			if (b1 > 1.05f && b2 > 1.05f && n1 >= 4.0f && n2 >= 4.0f) {
				foundZone = static_cast<uint16_t>(zone);
				foundGamma = gamma;
				found = true;
			}
		}
	}
	REQUIRE(found); // country where the whole character rasps must exist, or the map has drifted

	const VoxParams p = buildVoxSliced(foundZone, 0.0f, 0.0, 0.0,
	                                   degenerateSliceBasis<kVoxTriangles>(), foundGamma);

	// THE NOTE IS CHOSEN SO THE BURST EXACTLY FILLS THE CYCLE, which is what makes halves
	// comparable: pick the fundamental at F1 divided by its own pulse count and exactly that many
	// formant periods fit. Any lower and the cycle ends in silence, so the second half would be
	// quieter for a reason that has nothing to do with decay; any higher and the burst is amputated
	// and the release is what the second half measures.
	size_t voicedPulses = 0;
	for (const float g : p.formant[0].pulseGain) {
		voicedPulses += g != 0.0f ? 1 : 0;
	}
	REQUIRE(voicedPulses >= 4);
	const double note = static_cast<double>(p.formant[0].frequencyHz)
	                    / static_cast<double>(voicedPulses);
	VoxCycle c;
	bakeVoxCycle(p, note, c);
	REQUIRE(c.voiced);

	// ENERGY IN HALVES rather than peak-to-peak, because the sum of two formants beats: an
	// individual hump can dip without the burst stopping being a growing one. Energy over a half
	// cannot, and it is also closer to what "gets louder" means to an ear.
	const int lastMeasurable = kVoxSlots - kVoxSlots / 32; // the release tail is not the rasp
	double firstHalf = 0.0;
	double secondHalf = 0.0;
	for (int i = 0; i < lastMeasurable; ++i) {
		const double v = c.slots[static_cast<size_t>(i)];
		(i < lastMeasurable / 2 ? firstHalf : secondHalf) += v * v;
	}
	CAPTURE(voicedPulses);
	CAPTURE(note);
	CAPTURE(firstHalf);
	CAPTURE(secondHalf);
	REQUIRE(firstHalf > 0.0); // a burst with something in it, or this measures two silences
	CHECK(secondHalf > firstHalf);
}

TEST_CASE("a position with no vox in it bakes nothing, and says so") {
	// THE SKIP FLAG, which is what keeps a vox-free patch costing exactly what it always did: every
	// gain zero means the bake is a flat line, and the consumer is told so rather than being handed
	// 257 zeros to blend. Nothing downstream should ever have to inspect a grid to discover it is
	// silent.
	VoxParams silent;
	silent.formant[0].frequencyHz = 700.0f;
	silent.formant[1].frequencyHz = 1200.0f; // real formants, no gains at all
	VoxCycle c;
	bakeVoxCycle(silent, 220.0, c);
	CHECK_FALSE(c.voiced);
	CHECK(c == VoxCycle {});

	// AND A NOTE THAT IS NOT A NOTE takes the same road rather than a division: a fundamental of
	// zero or worse reaches this bake only through a bug elsewhere, and an unvoiced cycle is the
	// answer that cannot make that bug louder.
	for (const double bad : { 0.0, -110.0, std::numeric_limits<double>::infinity() }) {
		CAPTURE(bad);
		VoxCycle degenerate;
		bakeVoxCycle(oneFormant(900.0f, 4), bad, degenerate);
		CHECK_FALSE(degenerate.voiced);
	}
}

TEST_CASE("the evaluator reads the grid it was given, wrapping rather than clamping") {
	// A cycle is periodic, so a phase of 1.25 means a quarter of the way in — not the end of the
	// table. Clamping instead would freeze the waveform at its last slot for any caller whose phase
	// ran past one, which is every caller that adds a modulation offset without normalising first.
	VoxCycle c;
	bakeVoxCycle(oneFormant(1100.0f, 4), 180.0, c);
	REQUIRE(c.voiced);

	// Slot centres come back exactly, which is the interpolator's own identity.
	for (const int slot : { 0, 1, 64, 200, 255 }) {
		CAPTURE(slot);
		const double phase = static_cast<double>(slot) / static_cast<double>(kVoxSlots);
		CHECK(evalVoxCycle(c, phase) == doctest::Approx(c.slots[static_cast<size_t>(slot)]));
	}
	// And the wrap: one turn on, and one turn back, are the same point.
	for (const double phase : { 0.0, 0.13, 0.5, 0.99 }) {
		CAPTURE(phase);
		CHECK(evalVoxCycle(c, phase + 1.0) == doctest::Approx(evalVoxCycle(c, phase)));
		CHECK(evalVoxCycle(c, phase - 1.0) == doctest::Approx(evalVoxCycle(c, phase)));
	}
}

//----------------------------------------------------------------------------------------------//
// THE SPECIES FIELD                                                                              //
//----------------------------------------------------------------------------------------------//
//
// How much vowel is in a place. The decision fixed the SHAPE rather than the number — terrain-derived,
// without hard edges — so these cases are about shape: no cliffs, a real
// zero-presence country for the identity to stand on, and a period longer than anything else in
// the engine so a species reads as terrain rather than as a modulation.

TEST_CASE("the species field has no hard edges anywhere, including where it leaves silence") {
	// THE DECISION, MEASURED. "No hard edges" is not a preference about niceness: a cliff in this
	// field would be audible as a click when a sound drifts across it, and the drift road moves a
	// sound continuously by design. The feedback field it sits under DOES have a threshold — a
	// max(0, tri - 0.75) join — because feedback is dangerous and should be rare; vox is not, so its
	// triangle's own shoulders are the whole fade.
	//
	// Measured as a Lipschitz bound over a fine sweep: no step between neighbouring positions may
	// exceed a small multiple of the average step. A threshold cliff of the feedback kind fails this
	// immediately, which is what makes it the known-bad rather than a hypothetical.
	constexpr int kSteps = 200000;
	constexpr double kSpan = 4.0; // several periods of the longest field in the engine
	double worst = 0.0;
	double total = 0.0;
	double previous = static_cast<double>(voxPresenceAt(0.0));
	for (int i = 1; i <= kSteps; ++i) {
		const double at = kSpan * static_cast<double>(i) / static_cast<double>(kSteps);
		const double here = static_cast<double>(voxPresenceAt(at));
		const double step = std::abs(here - previous);
		worst = std::max(worst, step);
		total += step;
		previous = here;
	}
	CAPTURE(worst);
	CAPTURE(total);
	// The whole sweep's travel is finite and the worst single step is a rounding-scale fraction of
	// it: a continuous landscape. A cliff would put a step of order 0.25 in `worst` — the height the
	// feedback field's threshold discards — while leaving `total` almost unchanged.
	REQUIRE(total > 1.0); // the field actually moves, or continuity is trivial
	CHECK(worst < 0.001);
}

TEST_CASE("three quarters of the terrain has no vowel in it, which is where the identity stands") {
	// THE DUTY DOES THE RATIONING, and this is the case that makes the zero-presence identity pin
	// meaningful rather than a statement about one point: "no vox here" has to be a COUNTRY a sound
	// can sit in and stay in, not a knife edge it balances on. If this fraction ever collapsed,
	// every existing patch would acquire a throat.
	//
	// THREE QUARTERS, not the half this case was written for. A later decision took the duty from 0.5
	// to 0.25 — vox should cover well under half the field, which puts it between sync's 0.50 and
	// feedback's effective 10% rather than making a vowel as common as a sync ridge.
	constexpr int kSamples = 100000;
	constexpr double kSpan = 12.0; // many periods, so the fraction is the field's and not a window's
	int silent = 0;
	int loud = 0;
	for (int i = 0; i < kSamples; ++i) {
		const double at = kSpan * static_cast<double>(i) / static_cast<double>(kSamples);
		const float presence = voxPresenceAt(at);
		// The range is the contract: a presence outside 0..1 would be a gain, not a blend weight.
		REQUIRE(presence >= 0.0f);
		REQUIRE(presence <= 1.0f);
		if (presence == 0.0f) {
			silent++;
		}
		if (presence > 0.5f) {
			loud++;
		}
	}
	const double silentFraction = static_cast<double>(silent) / static_cast<double>(kSamples);
	CAPTURE(silentFraction);
	// About three quarters, with room for the sweep's own windowing — the claim is "a real country",
	// not a number to the third place. Same tolerance philosophy the half-duty version carried.
	CHECK(silentFraction > 0.65);
	CHECK(silentFraction < 0.85);
	// And the field genuinely reaches strong presence somewhere, or a vox-free engine would pass
	// every case in this file.
	CHECK(loud > 0);
}

TEST_CASE("the species is the longest-range field in the engine, and it says so in its own rate") {
	// A SPECIES SHOULD BE SLOWER THAN WEATHER. The operator fields are countries (quantize, sync,
	// feedback, at φ^-2.25/-2.75/-3.25); a vowel character that changed as fast as one of those
	// would read as a modulation effect rather than as somewhere you have travelled to.
	//
	// The rate is DERIVED from feedback's rather than transcribed, so this case is really checking
	// that the derivation still means what it says — a later edit that pasted a decimal in would
	// break the relationship silently, and this is what would notice.
	CHECK(kVoxSpeciesRate < kFlagshipFeedbackRate); // a lower rate is a longer period
	CHECK(static_cast<double>(kFlagshipFeedbackRate / kVoxSpeciesRate)
	      == doctest::Approx(static_cast<double>(owlet::dsp::phi::kPhi050)).epsilon(1e-6));
	// And it is below every operator, not merely below the slowest one.
	CHECK(kVoxSpeciesRate < kFlagshipQuantizeRate);
	CHECK(kVoxSpeciesRate < kFlagshipSyncRate);

	// THE FLAGSHIP'S HOME HAS NO THROAT IN IT, exactly — not "not much". The offset is written as
	// 0.75/rate precisely so the origin lands mid-silence, and this is the case that holds the two
	// together: a rate change that left a hand-written offset behind would move the origin back onto
	// the triangle, which is what the first spelling of the constant did (measured: 0.62).
	CHECK(voxPresenceAt(0.0) == 0.0f);
	// And it is silent for a WHILE either side, which is what makes it a home rather than a null.
	CHECK(voxPresenceAt(0.2) == 0.0f);
	CHECK(voxPresenceAt(-0.2) == 0.0f);
}
