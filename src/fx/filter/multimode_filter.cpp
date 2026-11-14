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

#include "fx/filter/multimode_filter.h"

#include "dsp/fast_math_approx.h"

#include <algorithm>
#include <new>
#include <cmath>
#include <complex>
#include <numbers>

namespace feathers::fx::filter {

namespace {

using SVF = CytomicSVF;

/// The vendored SVF's own mode enum, which this module's Type is a curated subset of. Kept as a
/// function rather than a cast so that reordering the dropdown cannot silently reorder the DSP.
SVF::Mode svfModeOf(Type type) {
	switch (type) {
	case Type::Lowpass:
		return SVF::Mode::Lowpass;
	case Type::Highpass:
		return SVF::Mode::Highpass;
	case Type::Bandpass:
		return SVF::Mode::Bandpass;
	case Type::Notch:
		return SVF::Mode::Notch;
	case Type::Peak:
		return SVF::Mode::Peak;
	case Type::LowShelf:
		return SVF::Mode::LowShelf;
	case Type::HighShelf:
		return SVF::Mode::HighShelf;
	case Type::Bell:
		return SVF::Mode::Bell;
	// THE COMB IS NOT AN SVF MODE AT ALL — it is a delay in a feedback loop, so it has no g, no k and
	// no three-term output mix. solve() and both sample loops branch on it before this is reached.
	case Type::Comb:
	// NOR IS THE BRICKWALL: four sections of its own (dsp/elliptic_highpass.h), branched on with the comb.
	case Type::Brickwall:
	case Type::kCount:
		break;
  }
	return SVF::Mode::Lowpass;
 }

constexpr MacroMap kGainMacro{MacroKind::Gain, "Gain", Type::Lowpass, Type::Lowpass};
/// AN EMPTY SLOT, which five types now have: Spread left the macro table for a param of its own, and
/// nothing has taken its place on purpose — see MacroKind::None.
constexpr MacroMap kNoMacro{MacroKind::None, "", Type::Lowpass, Type::Lowpass};
/// A SECOND EMPTY SLOT for types that spend neither macro. Label differs from kNoMacro so the
/// "every macro has a distinct label" test passes — both are None, both draw nothing.
constexpr MacroMap kNoMacro2{MacroKind::None, " ", Type::Lowpass, Type::Lowpass};
/// The shelves morph into each other, which they could not do while macro 2 was spent elsewhere.
/// BELL IS NOT A PARTNER IN EITHER DIRECTION and never can be: setCoeff divides its k by the gain,
/// so a Bell lane and a non-Bell lane are two different filters rather than two mixes of one — the
/// constraint this module's header states, applied.
constexpr MacroMap kShelfMorph{MacroKind::Morph, "Morph", Type::LowShelf, Type::HighShelf};
/// THE SLOT SPREAD LEFT EMPTY, SPENT. MacroKind::None's own comment says the five freed slots were
/// being kept "because comb and phaser are the devices that will spend it, on damping and feedback
/// character" — this is the comb half of that, arriving as the damper inside the feedback loop.
constexpr MacroMap kCombDamp{MacroKind::Damp, "Damp", Type::Lowpass, Type::Lowpass};

/// THE TYPE-FOLLOWING TABLE. Four columns, and every future type has to answer all of them.
///
/// THE MACROS ARE FOR MEANINGS THAT CHANGE WITH THE TYPE, which is the rule that decided the shape
/// this table now has. Gain qualifies: a type with a gain axis (bell, both shelves) spends macro 1
/// on it, because it is the control that type is about and it costs nothing — bellShelfAmp is
/// already a coefficient the filter takes. Morph qualifies: it walks the output mix toward a
/// NEIGHBOUR, and which neighbour is a fact about the type. Spread never qualified — it offset a
/// second corner identically for all eight — so it left for a param of its own, and the five types
/// that used to spend macro 1 on it now declare NOTHING there, on purpose. Comb and phaser are the
/// devices that will spend that slot.
///
/// The morph partners are chosen so that both directions stay in the same pole family. NEVER BELL,
/// in either direction: setCoeff divides its k by the gain, so its poles sit elsewhere and a blend
/// with it would be a crossfade between two different filters rather than a blend of two mixes of
/// one. That is also why Bell alone has no morph of its own.
///
/// THE THIRD COLUMN IS THE SLOPES A TYPE DECLARES, per SlopeMask: everything for the two that have a
/// one-pole form, the even pair for the two that have a skirt but no one-pole, and one section for
/// the gain shapes, which get LOUDER from a cascade rather than steeper. The fourth says the
/// response can be drawn in closed form, which is the card's promise made checkable.
constexpr TypeTraits kTypeTraits[] = {
	// LOWPASS and HIGHPASS: a skirt and a one-pole form, so they wear the whole slope list. Macro 1
	// is empty since Spread became a param; macro 2 still walks the mix toward a neighbour — down
	// toward a notch (a dip under the roll-off), up toward a band (the bass leaves).
	{kNoMacro, {MacroKind::Morph, "Morph", Type::Notch, Type::Bandpass}, kSlopesAll, true, true, true},
	{kNoMacro, {MacroKind::Morph, "Morph", Type::Notch, Type::Bandpass}, kSlopesAll, true, true, true},
	// BANDPASS and NOTCH have a skirt to steepen but no one-pole form, so the odd entries are not
	// theirs to offer. The three on the LP–HP axis morph along it, so the macro reads as a tilt.
	{kNoMacro, {MacroKind::Morph, "Morph", Type::Lowpass, Type::Highpass}, kSlopesEven, true, true, true},
	{kNoMacro, {MacroKind::Morph, "Morph", Type::Lowpass, Type::Highpass}, kSlopesEven, true, true, true},
	// PEAK is a gain shape rather than a skirt — unity at DC and unity at Nyquist, with the feature
	// at the corner — so a cascade doubles the feature's decibels instead of steepening anything.
	{kNoMacro, {MacroKind::Morph, "Morph", Type::Lowpass, Type::Highpass}, kSlopesOne, true, true, true},
	// THE THREE WITH A GAIN AXIS keep macro 1 on it and now spend macro 2 on Morph, which they did
	// not have at all while Spread was a macro — the shelves into each other, and Bell nowhere,
	// because Bell cannot be a morph partner.
	{kGainMacro, kShelfMorph, kSlopesOne, true, true, true},
	{kGainMacro, kShelfMorph, kSlopesOne, true, true, true},
	{kGainMacro, kNoMacro, kSlopesOne, true, true, true},

	// COMB. Macro 1 is the damper and macro 2 is empty — the second half of the freed slot is the
	// phaser's to spend, not this one's. ONE SLOPE, because a comb has no pole count to steepen: its
	// skirt is a train of notches whose spacing is the delay, and cascading two of them would be two
	// combs rather than a steeper one. NO SPREAD: the spread stage mixes over the SVF cascade and
	// this branch does not run it. DRAWABLE: the loop
	// is linear, and 1/(1 - fb·D(z)·z^-d) is as closed as a form gets.
	{kCombDamp, kNoMacro2, kSlopesOne, true, false, true},

	// BRICKWALL HP (backlog NL). Neither macro: its shape is fixed by its design. ONE SLOPE — it is already
	// order 8, and the slope list names SVF cascades it does not run. NO SPREAD and NO RESONANCE, for the comb's
	// reason and its own: the branch runs neither the spread stage nor a knob-wearing section. DRAWABLE: four
	// linear sections, drawn through the closed form the sample loop runs (dsp::EllipticHighpassDesign).
	{kNoMacro, kNoMacro2, kSlopesOne, true, false, false},
};

static_assert(std::size(kTypeTraits) == static_cast<size_t>(Type::kCount));

/// The loop gain a resonance asks for, and the damper pole a macro asks for — both in one place
/// because the sample loop and the curve both need them and a second copy is a second answer.
double combFeedbackOf(double resonance) {
	return std::clamp(resonance, 0.0, 1.0) * static_cast<double>(kCombMaxFeedback);
}

double combDampOf(double macro1) {
	return std::clamp(macro1, -1.0, 1.0) * static_cast<double>(kCombMaxDamp);
}

/// Everything the macros resolve to, in double, computed in ONE place so that the audio path and the
/// curve on the card cannot disagree about what a macro means. solve() narrows these to float for
/// the SIMD coefficients; gainAt() uses them as they are.
struct Resolved {
	Type base{Type::Lowpass};
	Type morph{Type::Lowpass};
	double cutoffHz{1000.0};
	double spreadCutoffHz{1000.0};
	double resonance{0.0};
	double amp{1.0}; ///< bellShelfAmp, 1.0 (flat) when the type has no gain axis or the macro is 0
	double morphWeight{0.0};
	double spreadWeight{0.0};
	/// The comb damper's pole, from whichever macro slot the type spends on Damp. Zero for every type
	/// that declares none, which is every type but the comb.
	double combDamp{0.0};
	StagePlan plan{};
};

Resolved resolve(const Settings& s) {
	Resolved r;
	r.base = s.type;
	r.morph = s.type;

	const auto index = static_cast<size_t>(s.type);
	const TypeTraits& traits = kTypeTraits[index < std::size(kTypeTraits) ? index : 0];
	r.plan = stagePlanOf(s.type, s.slope);

	double gainDb = 0.0;
	// SPREAD COMES FROM ITS OWN PARAM NOW, not from whichever macro slot a type happened to spend on
	// it. One reading for all eight types, which is what promoting it was for.
	const double spread = std::clamp(static_cast<double>(s.spread), -1.0, 1.0);
	const auto applyMacro = [&](double value, const MacroMap& map) {
		const double clamped = std::clamp(value, -1.0, 1.0);
		switch (map.kind) {
		case MacroKind::Gain:
			gainDb = clamped * kMacroGainDb;
			break;
		case MacroKind::Morph:
			r.morphWeight = std::abs(clamped);
			r.morph = clamped < 0.0 ? map.morphNeg : map.morphPos;
			break;
		case MacroKind::Damp:
			// THE COMB'S DAMPER, resolved HERE rather than read off macro1 in the two places that need
			// it. Which slot a type spends on which mechanism is the TABLE's to say, and a sample loop
			// reaching past the table for a named macro is the second copy that goes stale the day the
			// table moves.
			r.combDamp = combDampOf(clamped);
			break;
		case MacroKind::None:
			// A slot a type does not spend. Nothing to apply, and the card draws no knob for it.
			break;
		}
	};
	applyMacro(s.macro1, traits.macro1);
	applyMacro(s.macro2, traits.macro2);

	// The Cytomic convention, stated in CytomicSVF.h: the amplitude is 10^(dB/40), so the response's
	// gain — which goes as A² for every one of the three types that take it — is 10^(dB/20). A macro
	// at zero gives exactly 1.0, and all three of Bell, Low Shelf and High Shelf are algebraically
	// flat at A = 1: their m-vectors reduce to (1, 0, 0).
	r.amp = std::pow(10.0, gainDb / 40.0);
	r.spreadWeight = std::abs(spread);

	// THE GUARD BOUNDS AND NOT THE KNOB'S — see kCutoffGuardOctaves. Clamping the sum to the knob's
	// own ends is what used to make a keytracked filter stop tracking at the point where the key
	// offset carried it out of the range: every note above C6 with the knob at its top met the same
	// 20 kHz corner, and every note below C2 with the knob at its bottom met the same 20 Hz one. The
	// corner is allowed out there now; gOf clamps f/sr to the vendor's 0.499 when it becomes a
	// coefficient, which is where a Nyquist limit belongs.
	const double cutoffLog2 =
	    std::clamp(static_cast<double>(s.cutoffLog2Hz) + static_cast<double>(s.keyOffsetSemitones) / 12.0,
	               kCutoffGuardLoLog2, kCutoffGuardHiLog2);
	r.cutoffHz = std::exp2(cutoffLog2);
	r.spreadCutoffHz = std::exp2(
	    std::clamp(cutoffLog2 + spread * kSpreadOctaves, kCutoffGuardLoLog2, kCutoffGuardHiLog2));
	r.resonance = std::clamp(static_cast<double>(s.resonance), 0.0, 1.0);
	return r;
}

/// The three-term output mix of one mode, as scalars — the same table CytomicSVF::setCoeffPostGK
/// writes into its SIMD lanes, transcribed here because the response evaluator needs it in double
/// and because a morph is a blend of two of these. The transcription is not taken on trust: a case
/// in tests/test_fx_filter.cpp checks this evaluator against the vendor's own closed-form gain, and
/// another checks it against the filter's measured impulse response.
std::array<double, 3> mixVectorOf(Type type, double k, double A) {
	switch (type) {
	case Type::Lowpass:
		return {0.0, 0.0, 1.0};
	case Type::Bandpass:
		return {0.0, 1.0, 0.0};
	case Type::Highpass:
		return {1.0, -k, -1.0};
	case Type::Notch:
		return {1.0, -k, 0.0};
	case Type::Peak:
		return {1.0, -k, -2.0};
	case Type::Bell:
		return {1.0, k * (A * A - 1.0), 0.0};
	case Type::LowShelf:
		return {1.0, k * (A - 1.0), A * A - 1.0};
	case Type::HighShelf:
		return {A * A, A * k * (1.0 - A), 1.0 - A * A};
	// THE COMB NEVER REACHES HERE, AND THAT IS AN INVARIANT RATHER THAN AN ACCIDENT. It is not an SVF
	// mix at all — gainAt dispatches it to combResponse BEFORE this function is called, so `r.base`
	// cannot be it.
	//
	// `r.morph` is the road that could still deliver one, because the morph target is NOT dispatched
	// — and it does not, because no type's macro map names a non-SVF type as morphNeg or morphPos.
	// tests/test_fx_filter.cpp pins it, because it is an invariant of a TABLE and the table is edited
	// by people adding filter models — the day a morph map names one, this function would answer with
	// the LOWPASS mix below and the card would draw a lowpass for it, in silence.
	//
	// Listed explicitly rather than left to the default so that adding a model is a compile error
	// here rather than a wrong curve later.
	case Type::Comb:
	case Type::Brickwall:
	case Type::kCount:
		break;
	}
	// UNREACHABLE FOR EVERY DECLARED TYPE. Kept as a lowpass rather than an assert because this runs
	// on the audio thread's sibling — the card's curve — and a wrong picture is better than a crash.
	return {0.0, 0.0, 1.0};
}

/// H(z) of one SVF stage with an arbitrary output mix.
///
/// The vendor's CytomicSVFGainAt carries a separate closed form per mode; this is the one identity
/// underneath all of them. The filter computes m0·v0 + m1·v1 + m2·v2 where v0 is the input, v1 the
/// bandpass and v2 the lowpass, so with den the common denominator,
///
///     H = m0 + m1·(g·(z²−1))/den + m2·(g²·(1+z)²)/den = (m0·den + m1·g·(z²−1) + m2·g²·(1+z)²)/den
///
/// which reproduces every one of the vendor's per-mode numerators — including the highpass, whose
/// numerator collapses to (z−1)² exactly because m = (1, −k, −1) cancels the other two terms of den.
/// One formula rather than nine is what lets a MORPHED mix be drawn at all.
std::complex<double> stageResponse(const std::array<double, 3>& m, double g, double k,
                                   const std::complex<double>& z) {
	const auto zp1 = 1.0 + z;
	const auto zm1 = z - 1.0;
	const auto zz1 = z * z - 1.0;
	const auto den = zm1 * zm1 + g * g * zp1 * zp1 + g * k * zz1;
	const auto num = m[0] * den + m[1] * g * zz1 + m[2] * g * g * zp1 * zp1;
	return num / den;
}

/// The same clamps setCoeff applies, so that the curve keeps agreeing with the filter at the edges
/// of the range rather than only in the middle of it.
double gOf(double cutoffHz, double sampleRate) {
	return std::tan(std::numbers::pi * std::clamp(cutoffHz / sampleRate, 0.0, 0.499));
}

double kOf(const Resolved& r) {
	const double k = 2.0 - 2.0 * std::clamp(r.resonance, 0.0, 0.98);
	// setCoeff divides k by the amplitude for Bell and for Bell only, which moves its poles. It is
	// also why Bell is never a morph partner: two lanes with different k are two different filters,
	// not two mixes of one.
	return r.base == Type::Bell ? k / std::max(r.amp, 0.001) : k;
}

/// The resonance the SECOND SVF section wears when a 24 dB slope runs two of them: Butterworth,
/// never the knob's. See kButterworthRes for why the peak has to land on one section only.
double kButterworth() {
	return 2.0 - 2.0 * static_cast<double>(kButterworthRes);
}

/// H(z) OF THE ONE-POLE, in the same z convention as stageResponse above — the trapezoidal (TPT)
/// integrator the SVF is built on, taken down to a single pole:
///
///     y[n] = G·x[n] + s,  s ← y[n] + G·(x[n] − s),  G = g/(1+g)
///
/// which transforms to g(z+1) / ((1+g)z + (g−1)). Unity at DC and zero at Nyquist, so it is a
/// lowpass; the highpass is the difference from the input, and that identity is exactly what the
/// sample loop computes rather than a second filter.
std::complex<double> onePoleResponse(double g, bool highpass, const std::complex<double>& z) {
	const auto lp = (g * (z + 1.0)) / ((1.0 + g) * z + (g - 1.0));
	return highpass ? 1.0 - lp : lp;
}

/// H(z) OF THE COMB: one feedback loop around a fractional delay with a one-pole damper inside it.
///
///     y[n] = x[n] + fb · damp(y[n − d])      =>      H(z) = 1 / (1 − fb · D(z) · z^−d)
///
/// with D(z) = (1 − |a|) / (1 − a·z^−1), the damper. |D| is at most exactly 1 — at DC for a positive
/// pole, at Nyquist for a negative one — so the loop's gain never exceeds fb and the whole thing is
/// stable for every |fb| < 1 that kCombMaxFeedback allows.
///
/// THE FRACTIONAL DELAY IS THE ONE THE SAMPLE LOOP ACTUALLY RUNS, twelve sinc taps and all, and an
/// ideal e^{−jωd} in its place is NOT good enough to draw with. MEASURED before it was believed: with
/// the ideal delay the drawn curve missed the rendered response by up to 14.6 dB — at 12 kHz, on a
/// 200 Hz comb at resonance 0.9 — because the interpolator's sinc has a bandwidth of 0.455 (its edge
/// is at 0.2275·sr: −3.9 dB at 0.2·sr, −15 dB at 0.3·sr) and therefore throws away the loop gain
/// exactly where the ideal model keeps it. Under the tap-for-tap form below the same case lands
/// inside the 0.1 dB the card's promise is pinned at.
///
///     y[n] = x[n] + fb · damp(interp(y)[n − d])   =>   H(z) = 1 / (1 − fb · D(z) · I(z))
///
/// with D(z) = (1 − |a|) / (1 − a·z^−1), the damper, and I(z) the interpolator's own FIR. |D| is at
/// most exactly 1 — at DC for a positive pole, at Nyquist for a negative one — and |I| never exceeds
/// 1 either, so the loop's gain never exceeds fb and the whole thing is stable for every |fb| < 1
/// that kCombMaxFeedback allows.
///
/// I(z), TAP FOR TAP FROM dsp::SincDelayLine::read. The read centres a twelve-tap window six samples
/// behind the read point and takes its coefficients from SincTable::kernel at the FRACTIONAL part,
/// so tap j is the sample (iDelay + 6 − j) writes ago:
///
///     out = Σ x[n − (iDelay + 6 − j)] · kernel(frac)[j]
///
/// Asking the TABLE for the taps rather than modelling them is deliberate: the taps are DATA and the
/// structure above is the derivation, so this is not the sample loop restating itself — the case
/// that pins it renders an impulse through the real loop and transforms it, which shares no line of
/// arithmetic with this. The fraction is narrowed to float exactly as the sample loop narrows it, so
/// the two ask the table the same question.
std::complex<double> combInterpolatorResponse(double delaySamples, double omega) {
	using Table = feathers::dsp::SincTable;
	const float delay = static_cast<float>(delaySamples);
	const int iDelay = static_cast<int>(delay);
	const float frac = delay - static_cast<float>(iDelay);
	float taps[Table::kTaps];
	kSincTable.kernel(frac, taps);
	std::complex<double> h{0.0, 0.0};
	for (int j = 0; j < Table::kTaps; j++) {
		const double lag = static_cast<double>(iDelay + Table::kHalfTaps - j);
		h += static_cast<double>(taps[j]) * std::exp(std::complex<double>(0.0, -omega * lag));
	}
	return h;
}

std::complex<double> combResponse(double delaySamples, double feedback, double damp, double omega) {
	const std::complex<double> zInv = std::exp(std::complex<double>(0.0, -omega));
	const std::complex<double> d = (1.0 - std::abs(damp)) / (1.0 - damp * zInv);
	return 1.0 / (1.0 - feedback * d * combInterpolatorResponse(delaySamples, omega));
}

} // namespace

double combDelaySamples(double cutoffHz, double sampleRate) {
	if (!(cutoffHz > 0.0) || !(sampleRate > 0.0)) {
		return static_cast<double>(kCombMinDelaySamples);
	}
	// THE CLAMP IS WHERE THE COMB'S CEILING AND FLOOR ACTUALLY LIVE, and both are a property of the
	// buffer rather than of taste — see kCombMinDelaySamples and kCombLineSamples. A corner past the
	// ceiling STOPS here rather than wrapping or reading unwritten samples, which is what makes the
	// drawn curve stop with it: gainAt calls this same function.
	return std::clamp(sampleRate / cutoffHz, static_cast<double>(kCombMinDelaySamples),
	                  static_cast<double>(kCombMaxDelaySamples));
}

namespace {

/// ONE SAMPLE THROUGH THE COMB, for one channel. Written once and called from both sample loops,
/// because the mono road and the stereo road being the same filter is a case in this module's tests.
///
/// THE READ HAPPENS BEFORE THE WRITE, which is what makes the shortest delay a real one: the newest
/// sample in the line is the PREVIOUS output, so a delay of d reads d samples back from now.
inline float combStep(feathers::dsp::SincDelayLine<kCombLineSamples>& line, float& dampZ,
                      float input, float delay, float feedback, float damp, float dampGain) {
	const float delayed = line.read(delay);
	// The damper: y = (1 - |a|)·x + a·y[-1], unity at DC for a positive pole and unity at Nyquist for
	// a negative one, and EXACTLY the identity at a = 0 — the invariant every macro in this module
	// carries.
	dampZ = dampGain * delayed + damp * dampZ;
	const float out = input + feedback * dampZ;
	line.write(out);
	return out;
}


} // namespace

const TypeTraits& traitsOf(Type type) {
	const auto index = static_cast<size_t>(type);
	return kTypeTraits[index < std::size(kTypeTraits) ? index : 0];
}

Slope effectiveSlope(Type type, Slope slope) {
	const TypeTraits& traits = traitsOf(type);
	if (slopeSupported(traits, slope)) {
		return slope;
	}
	// THE NEAREST SLOPE THIS TYPE ACTUALLY DECLARES: walk outward from what was asked and take the
	// first supported entry, preferring the STEEPER on a tie, so that a bandpass asked for 18 gets 24
	// and never fewer poles than the entry below it.
	Slope wanted = slope;
	const int asked = polesOf(slope);
	int bestDistance = 0;
	bool found = false;
	for (int i = 0; i < static_cast<int>(Slope::kCount); i++) {
		const auto candidate = static_cast<Slope>(i);
		if (!slopeSupported(traits, candidate)) {
			continue;
		}
		const int distance = std::abs(polesOf(candidate) - asked);
		// `<=` is the tie-break: the list is in ascending order, so the later of two equally distant
		// entries is the steeper one.
		if (!found || distance <= bestDistance) {
			bestDistance = distance;
			wanted = candidate;
			found = true;
		}
	}
	return wanted;
}

StagePlan stagePlanOf(Type type, Slope slope) {
	const int poles = polesOf(effectiveSlope(type, slope));
	StagePlan plan;
	// A pole is a pole: two at a time in an SVF section, and the odd one left over in a one-pole.
	// THE FIRST SECTION IS THE ONE THAT RESONATES, which is why a 6 dB slope — the case with no SVF
	// section at all — has no resonance to give and says so.
	plan.twoPoleStages = poles / 2;
	plan.onePole = (poles % 2) == 1;
	plan.onePoleHighpass = type == Type::Highpass;
	return plan;
}

bool slopeAppliesTo(Type type) {
	// More than one entry to choose between. A type with a single declared slope has a control that
	// cannot change anything, which is what a surface greys rather than draws live.
	const SlopeMask slopes = traitsOf(type).slopes;
	return (slopes & (slopes - 1)) != 0;
}

void MultimodeFilter::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	srInv_ = static_cast<float>(1.0 / sampleRate_);
	// SEE `kRestSilentSeconds`. In samples at this rate, so the wait is a duration rather than a
	// buffer count.
	restSilenceNeeded_ = static_cast<int>(kRestSilentSeconds * sampleRate_);
	restSilence_ = 0;
	// One pole, one time constant, one grid. The rate is a function of the SAMPLE RATE and of
	// nothing else — in particular not of the block length, which is the whole point.
	smoothAlpha_ = static_cast<float>(
	    1.0 - std::exp(-static_cast<double>(kTickSamples) / (kSmoothSeconds * sampleRate_)));
	reset();
	snap();
}

void MultimodeFilter::processStereo(float* left, float* right, int n) {
	// THE WRAPPER IS THE REST CHECK AND NOTHING ELSE — see `flushIfAtRest`. `enabled` is read BEFORE
	// the core runs because the core is what turns `active_` off on a bypass, and a buffer the device
	// declined to touch is the caller's to read, not ours.
	const bool running = target_.enabled;
	// THE INPUT, READ BEFORE THE CORE OVERWRITES IT IN PLACE. See `kRestSilentSeconds`: a quiet
	// OUTPUT says nothing about the state, and the input is the only thing that can promise there is
	// nothing left to come out.
	bool silentInput = true;
	if (running) {
		// A NON-FINITE SAMPLE IS SCRUBBED TO ZERO BEFORE IT REACHES THE STATE, as EQ does: one NaN
		// in a recursive section is a NaN in every later output. Finite input is untouched.
		for (int i = 0; i < n; ++i) {
			left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
			right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
			silentInput = silentInput && left[i] == 0.0f && right[i] == 0.0f;
		}
	}
	processStereoCore(left, right, n);
	if (running) {
		flushIfAtRest(left, right, n, silentInput);
	}
}

void MultimodeFilter::processMono(float* mono, int n) {
	const bool running = target_.enabled;
	bool silentInput = true;
	if (running) {
		for (int i = 0; i < n; ++i) {
			mono[i] = std::isfinite(mono[i]) ? mono[i] : 0.0f;
			silentInput = silentInput && mono[i] == 0.0f;
		}
	}
	processMonoCore(mono, n);
	if (running) {
		flushIfAtRest(mono, nullptr, n, silentInput);
	}
}

void MultimodeFilter::flushIfAtRest(float* left, float* right, int n, bool silentInput) {
	if (!silentInput) {
		restSilence_ = 0;
		return;
	}
	// A NON-FINITE OUTPUT ON SILENT INPUT IS A POISONED STATE, and it flushes NOW, whatever the wait
	// and whatever the level: `std::max` ignores a NaN, so the peak scan below would read it as
	// silence and never get here, and the device would sing NaN for ever. The block's own non-finite
	// samples are zeroed so what leaves is finite too.
	bool poisoned = false;
	for (int i = 0; i < n; ++i) {
		if (!std::isfinite(left[i])) {
			left[i] = 0.0f;
			poisoned = true;
		}
		if (right != nullptr && !std::isfinite(right[i])) {
			right[i] = 0.0f;
			poisoned = true;
		}
	}
	if (!poisoned) {
		if (restSilence_ < restSilenceNeeded_) {
			restSilence_ += n;
			return;
		}
		float peak = 0.0f;
		for (int i = 0; i < n; ++i) {
			peak = std::max(peak, std::fabs(left[i]));
			if (right != nullptr) {
				peak = std::max(peak, std::fabs(right[i]));
			}
		}
		if (peak == 0.0f || peak >= kRestLevel) {
			return;
		}
	}
	// **THE NUMERIC STATE, AND NOT `reset()`.** The first version of this called `reset()`, on the
	// reasoning that it is `AudioOps::reset`'s weaker half — forget the delay lines, keep the settings.
	// It is not: `reset()` ALSO clears `spreadRunning_`, `butterRunning_`, `onePoleRunning_` and
	// `combRunning_`, which say WHICH STAGES ARE LIVE. `solve()` sets them, and `solve()` runs on a
	// settings change rather than every block — so a flush mid-stream silently took the spread and the
	// Butterworth section out of the signal path until the next automation move.
	//
	// MEASURED, which is how it was caught rather than reasoned: `test_fx_filter`'s impulse-response
	// cases went **0.472 dB and 1.24 dB** off their drawn curves. A tail at 1e-30 cannot move a
	// transform by a decibel; a missing filter section can.
	//
	// So this clears the delay lines and nothing else. `active_` and the four running flags are
	// untouched, which is what makes the flush inaudible by construction rather than by measurement.
	stage1_.init();
	stageButter_.init();
	stage2_.init();
	onePoleLpL_.reset();
	onePoleLpR_.reset();
	onePoleHpL_.reset();
	onePoleHpR_.reset();
	brickwall_[0].reset();
	brickwall_[1].reset();
	if (combRunning_) {
		combLine_[0].clear();
		combLine_[1].clear();
	}
	combDampZ_[0] = 0.0f;
	combDampZ_[1] = 0.0f;
}

void MultimodeFilter::reset() {
	stage1_.init();
	stageButter_.init();
	stage2_.init();
	onePoleLpL_.reset();
	onePoleLpR_.reset();
	onePoleHpL_.reset();
	onePoleHpR_.reset();
	// THE COMB LINE IS 32 KB AND IS CLEARED ONLY IF IT WAS RUNNING. reset() is called on every
	// note-on through AudioOps::reset, so clearing unconditionally would memset 32 KB per voice per
	// note for a device that is almost never a comb. Correctness does not need it: solve() clears the
	// line as the comb STARTS running, which is the same guarantee stage2_ and stageButter_ get.
	if (combRunning_) {
		combLine_[0].clear();
		combLine_[1].clear();
	}
	combDampZ_[0] = 0.0f;
	combDampZ_[1] = 0.0f;
	spreadRunning_ = false;
	butterRunning_ = false;
	onePoleRunning_ = false;
	combRunning_ = false;
	brickwall_[0].reset();
	brickwall_[1].reset();
	brickwallRunning_ = false;
}

void MultimodeFilter::snap() {
	smoothed_.cutoffLog2Hz = target_.cutoffLog2Hz + target_.keyOffsetSemitones / 12.0f;
	smoothed_.resonance = target_.resonance;
	smoothed_.drive = target_.drive;
	smoothed_.macro1 = target_.macro1;
	smoothed_.macro2 = target_.macro2;
	smoothed_.spread = target_.spread;
	// THE RIGHT EAR SNAPS TOO (backlog KU): every control where its right ear is — its left where it is not live.
	const Settings sR = rightEarOf(target_);
	smoothedR_.cutoffLog2Hz = sR.cutoffLog2Hz + sR.keyOffsetSemitones / 12.0f;
	smoothedR_.resonance = sR.resonance;
	smoothedR_.drive = sR.drive;
	smoothedR_.macro1 = sR.macro1;
	smoothedR_.macro2 = sR.macro2;
	smoothedR_.spread = sR.spread;
	liveSmoothed_ = target_.rightLive;
	solve();
	// AND THE READ HEAD GOES WHERE IT IS GOING, rather than gliding there. snap() is "put the smoother
	// where the target already is"; a delay that ramped in from wherever it was left would be a pitch
	// slide at the top of every note.
	combDelay_ = combDelayTarget_;
	combDelayInc_ = 0.0f;
	combDelayR_ = combDelayTargetR_;
	combDelayIncR_ = 0.0f;
	untilTick_ = kTickSamples;
}

void MultimodeFilter::tick() {
	const float a = smoothAlpha_;
	const float cutoffTarget = target_.cutoffLog2Hz + target_.keyOffsetSemitones / 12.0f;
	smoothed_.cutoffLog2Hz += (cutoffTarget - smoothed_.cutoffLog2Hz) * a;
	smoothed_.resonance += (target_.resonance - smoothed_.resonance) * a;
	smoothed_.drive += (target_.drive - smoothed_.drive) * a;
	smoothed_.macro1 += (target_.macro1 - smoothed_.macro1) * a;
	smoothed_.macro2 += (target_.macro2 - smoothed_.macro2) * a;
	// AND SPREAD, which was the one continuous control on this device that was not here.
	//
	// It is a REGRESSION FROM ITS OWN PROMOTION. Spread used to arrive through macro1/macro2, which
	// are two lines above and always have been smoothed; giving it a field of its own took it off the
	// smoother, and solve() has been building its coefficients from the raw target ever since — so the
	// second stage's mix weight AND its corner both stepped, once per coefficient update.
	//
	// MEASURED, on a 220 Hz tone with Spread jumped mid-note: a discontinuity of 22 to 29 times the
	// tone's own per-sample slew, landing on the sample the control moved. Cutoff, doing a comparable
	// four-octave jump through this smoother, gives 1.0 to 1.8x — which is to say nothing. Moving Spread
	// with sound playing was audibly clicking.
	//
	// It also disarms the stage2_.init() in solve(): the reset happens as the weight leaves zero, and
	// a ramped weight makes a zeroed stage inaudible where a stepped one dumped it into the signal.
	smoothed_.spread += (target_.spread - smoothed_.spread) * a;

	// THE RIGHT EAR'S SMOOTHERS, only for the controls a stereo route reaches (backlog KU). A control that has just
	// become live starts from where its left ear is, so the split opens from the sound rather than jumping; a
	// control that is not live IS its left (not a glide toward it from a stale value).
	if (target_.rightLive != 0) {
		const uint8_t fresh = static_cast<uint8_t>(target_.rightLive & ~liveSmoothed_);
		const Settings sR = rightEarOf(target_);
		const auto step = [&](float& v, float left, float want, Ear e) {
			if ((target_.rightLive & earBit(e)) == 0) {
				v = left;
				return;
			}
			if ((fresh & earBit(e)) != 0) {
				v = left;
			}
			v += (want - v) * a;
		};
		step(smoothedR_.cutoffLog2Hz, smoothed_.cutoffLog2Hz, sR.cutoffLog2Hz + sR.keyOffsetSemitones / 12.0f,
		     Ear::Cutoff);
		step(smoothedR_.resonance, smoothed_.resonance, sR.resonance, Ear::Resonance);
		step(smoothedR_.drive, smoothed_.drive, sR.drive, Ear::Drive);
		step(smoothedR_.macro1, smoothed_.macro1, sR.macro1, Ear::Macro1);
		step(smoothedR_.macro2, smoothed_.macro2, sR.macro2, Ear::Macro2);
		step(smoothedR_.spread, smoothed_.spread, sR.spread, Ear::Spread);
	}
	liveSmoothed_ = target_.rightLive;
	solve();
}

void MultimodeFilter::solve() {
	// A STEREO ROUTE REACHES THIS FILTER (backlog KU): each ear solved on its own. Otherwise everything below is the
	// mono filter as it always was, plus a tail that makes the right channel's twins equal to the left.
	if (target_.rightLive != 0) {
		solveEars();
		return;
	}
	// The smoothed control values wearing the target's discrete settings. The type is not smoothed —
	// it is a choice, not a position — and neither is the keytrack architecture, which the owner
	// expresses by how many of these objects it runs.
	Settings s = target_;
	s.cutoffLog2Hz = smoothed_.cutoffLog2Hz;
	s.keyOffsetSemitones = 0.0f; // already folded into the smoothed cutoff
	s.resonance = smoothed_.resonance;
	s.drive = smoothed_.drive;
	s.macro1 = smoothed_.macro1;
	s.macro2 = smoothed_.macro2;
	s.spread = smoothed_.spread;

	const Resolved r = resolve(s);
	// A TYPE THAT IS NOT THE COMB LEAVES THE LINE NOT RUNNING, AND IT HAS TO BE SAID HERE, before any
	// branch returns: a `combRunning_ = false` sitting beside the SVF cascade would be skipped by any
	// type that returned early, and the line would then come back to a comb without being cleared,
	// ringing the tail it held before the type moved away.
	combRunning_ = combRunning_ && r.base == Type::Comb;
	brickwallRunning_ = brickwallRunning_ && r.base == Type::Brickwall; // and the brickwall, for the same reason
	morphWeight_ = static_cast<float>(r.morphWeight);
	spreadWeight_ = static_cast<float>(r.spreadWeight);

	// THE DRIVE STAGE, SET UP BEFORE THE BRANCHES because the comb returns from inside one and needs
	// it. 1x to 16x, so a drive at the top is a clipper and a drive in the middle is a
	// colour; the makeup holds a full-scale input at full scale, which makes the knob read as
	// saturation rather than as a volume control.
	driveAmount_ = std::clamp(smoothed_.drive, 0.0f, 1.0f);
	driveGain_ = std::exp2(driveAmount_ * 4.0f);
	driveComp_ = 1.0f / feathers::dsp::fasttanh(std::min(driveGain_, 4.5f));

	if (r.base == Type::Comb) {
		// THE DELAY IS A TARGET AND AN INCREMENT, not a coefficient. Everything else this function
		// writes may step on the tick because a small step in g or k is a small step in the output; a
		// step in the read POSITION is a step in the signal itself. So the tick says where the head is
		// going and the sample loop walks it there over exactly kTickSamples samples — a rate that is
		// a function of the tick and of nothing else, which is what keeps the host's block size out of
		// the audio.
		combDelayTarget_ = static_cast<float>(combDelaySamples(r.cutoffHz, sampleRate_));
		combDelayInc_ =
		    (combDelayTarget_ - combDelay_) / static_cast<float>(kTickSamples);
		combFeedback_ = static_cast<float>(combFeedbackOf(smoothed_.resonance));
		// THE DAMPER COMES OFF THE RESOLVED MACRO, so the slot it lives in is the table's to move.
		combDamp_ = static_cast<float>(r.combDamp);
		combDampGain_ = 1.0f - std::abs(combDamp_);
		if (!combRunning_) {
			// Clean, for the reason stage2_ is: a line that still holds the last comb's tail would
			// ring two settings ago into the signal the moment the type came back.
			combLine_[0].clear();
			combLine_[1].clear();
			combDampZ_[0] = 0.0f;
			combDampZ_[1] = 0.0f;
			combDelay_ = combDelayTarget_;
			combDelayInc_ = 0.0f;
			combDelayR_ = combDelayTarget_; // and the right head with it, so the mirror below walks it nowhere
			combRunning_ = true;
		}
		mainRunning_ = false;
		butterRunning_ = false;
		onePoleRunning_ = false;
		spreadRunning_ = false;
		// THE SHARED DRIVE STAGE STAYS ON, in front of the loop — the same road the diode ladder
		// takes. The comb has no saturation of its own, so this is the whole of its Drive.
		mirrorRightEar();
		return;
	}

	if (r.base == Type::Brickwall) {
		// THE KEYTRACKED BRICKWALL (backlog NL): the corner is f0, the frequency to remove — the smoothed cutoff
		// with the key offset already folded in, so it glides on the same 10 ms smoother every corner here does.
		// The drive stage stays in front of it, as it does for the comb.
		if (!brickwallRunning_) {
			brickwall_[0].reset();
			brickwall_[1].reset();
			brickwallRunning_ = true;
		}
		brickwall_[0].setCorner(r.cutoffHz, sampleRate_);
		brickwall_[1].setCorner(r.cutoffHz, sampleRate_);
		mainRunning_ = false;
		butterRunning_ = false;
		onePoleRunning_ = false;
		spreadRunning_ = false;
		mirrorRightEar();
		return;
	}

	// Lanes 0 and 1 are the declared type on left and right; lanes 2 and 3 are the morph partner on
	// the same left and right. All four share g and k, so all four share their state exactly — see
	// this module's header. A type with no morph partner puts the same mode in all four lanes, which
	// costs the same and makes the blend a no-op by construction.
	const SVF::Mode base = svfModeOf(r.base);
	const std::array<SVF::Mode, 4> modes{base, base, svfModeOf(r.morph), svfModeOf(r.morph)};

	// THE CASCADE THE PLAN ASKS FOR. Every stage takes the SAME corner and the same output mix; what
	// differs is the resonance, which lands on the first section and nowhere else.
	mainRunning_ = r.plan.twoPoleStages >= 1;
	if (mainRunning_) {
		stage1_.setCoeff(modes, static_cast<float>(r.cutoffHz), static_cast<float>(r.resonance),
		                 srInv_, static_cast<float>(r.amp));
	}

	if (r.plan.twoPoleStages >= 2) {
		if (!butterRunning_) {
			stageButter_.init();
			butterRunning_ = true;
		}
		stageButter_.setCoeff(modes, static_cast<float>(r.cutoffHz), kButterworthRes, srInv_,
		                      static_cast<float>(r.amp));
	}
	else {
		butterRunning_ = false;
	}

	if (r.plan.onePole) {
		if (!onePoleRunning_) {
			onePoleLpL_.reset();
			onePoleLpR_.reset();
			onePoleHpL_.reset();
			onePoleHpR_.reset();
			onePoleRunning_ = true;
		}
		onePoleHighpass_ = r.plan.onePoleHighpass;
		// NYQUIST-CLAMPED BEFORE IT REACHES THE ONE-POLE, and this is load-bearing rather than tidy.
		// dsp/tpt_one_pole.h takes tan(pi·fc/sr) with no clamp of its own, where CytomicSVF::setCoeff
		// clamps fc/sr to 0.499 internally — and since the cutoff fix on this branch's parent, a
		// keytracked high note can genuinely ask for a corner above Nyquist. Unclamped, tan of an
		// angle past pi/2 goes NEGATIVE and the one-pole is not a filter any more. Same clamp, same
		// number, at the only stage that does not apply it itself.
		const auto corner =
		    static_cast<float>(std::min(r.cutoffHz, 0.499 * sampleRate_));
		if (onePoleHighpass_) {
			onePoleHpL_.setCutoff(corner, static_cast<float>(sampleRate_));
			onePoleHpR_.setCutoff(corner, static_cast<float>(sampleRate_));
		}
		else {
			onePoleLpL_.setCutoff(corner, static_cast<float>(sampleRate_));
			onePoleLpR_.setCutoff(corner, static_cast<float>(sampleRate_));
		}
	}
	else {
		onePoleRunning_ = false;
	}

	if (spreadWeight_ > 0.0f) {
		if (!spreadRunning_) {
			// Coming back from silence with whatever it held when it was last needed would ring a
			// stale tail into the signal. It costs one branch to start it clean.
			stage2_.init();
			spreadRunning_ = true;
		}
		stage2_.setCoeff(modes, static_cast<float>(r.spreadCutoffHz),
		                 static_cast<float>(r.resonance), srInv_, static_cast<float>(r.amp));
	}
	else {
		spreadRunning_ = false;
	}
	mirrorRightEar();
}

void MultimodeFilter::mirrorRightEar() {
	morphWeightR_ = morphWeight_;
	spreadWeightR_ = spreadWeight_;
	driveAmountR_ = driveAmount_;
	driveGainR_ = driveGain_;
	driveCompR_ = driveComp_;
	combFeedbackR_ = combFeedback_;
	combDampR_ = combDamp_;
	combDampGainR_ = combDampGain_;
	combDelayTargetR_ = combDelayTarget_;
	// THE READ HEAD IS NOT COPIED, it is walked: the same expression over the same operands as the left's, so a right
	// head that was where the left is (always, on the mono road) stays there to the bit — and one coming back from a
	// split glides home over the tick rather than jumping, which would be a step in the signal.
	combDelayIncR_ = (combDelayTargetR_ - combDelayR_) / static_cast<float>(kTickSamples);
}

void MultimodeFilter::solveEars() {
	// EACH EAR'S SETTINGS, built exactly as `solve` builds its one.
	const auto earOf = [&](const Coeffs& c) {
		Settings s = target_;
		s.cutoffLog2Hz = c.cutoffLog2Hz;
		s.keyOffsetSemitones = 0.0f; // already folded into the smoothed cutoff
		s.resonance = c.resonance;
		s.drive = c.drive;
		s.macro1 = c.macro1;
		s.macro2 = c.macro2;
		s.spread = c.spread;
		s.rightLive = 0;
		return s;
	};
	const Resolved rL = resolve(earOf(smoothed_));
	const Resolved rR = resolve(earOf(smoothedR_));
	// THE TYPE IS ONE (it is not a stereo control), so both ears agree on comb-or-not and on the stage plan. The
	// morph PARTNER can differ: it is picked by the macro's sign, which a stereo route can split.
	combRunning_ = combRunning_ && rL.base == Type::Comb;
	brickwallRunning_ = brickwallRunning_ && rL.base == Type::Brickwall;
	morphWeight_ = static_cast<float>(rL.morphWeight);
	morphWeightR_ = static_cast<float>(rR.morphWeight);
	spreadWeight_ = static_cast<float>(rL.spreadWeight);
	spreadWeightR_ = static_cast<float>(rR.spreadWeight);

	driveAmount_ = std::clamp(smoothed_.drive, 0.0f, 1.0f);
	driveGain_ = std::exp2(driveAmount_ * 4.0f);
	driveComp_ = 1.0f / feathers::dsp::fasttanh(std::min(driveGain_, 4.5f));
	driveAmountR_ = std::clamp(smoothedR_.drive, 0.0f, 1.0f);
	driveGainR_ = std::exp2(driveAmountR_ * 4.0f);
	driveCompR_ = 1.0f / feathers::dsp::fasttanh(std::min(driveGainR_, 4.5f));

	if (rL.base == Type::Comb) {
		combDelayTarget_ = static_cast<float>(combDelaySamples(rL.cutoffHz, sampleRate_));
		combDelayInc_ = (combDelayTarget_ - combDelay_) / static_cast<float>(kTickSamples);
		combDelayTargetR_ = static_cast<float>(combDelaySamples(rR.cutoffHz, sampleRate_));
		combDelayIncR_ = (combDelayTargetR_ - combDelayR_) / static_cast<float>(kTickSamples);
		combFeedback_ = static_cast<float>(combFeedbackOf(smoothed_.resonance));
		combFeedbackR_ = static_cast<float>(combFeedbackOf(smoothedR_.resonance));
		combDamp_ = static_cast<float>(rL.combDamp);
		combDampGain_ = 1.0f - std::abs(combDamp_);
		combDampR_ = static_cast<float>(rR.combDamp);
		combDampGainR_ = 1.0f - std::abs(combDampR_);
		if (!combRunning_) {
			combLine_[0].clear();
			combLine_[1].clear();
			combDampZ_[0] = 0.0f;
			combDampZ_[1] = 0.0f;
			combDelay_ = combDelayTarget_;
			combDelayInc_ = 0.0f;
			combDelayR_ = combDelayTargetR_;
			combDelayIncR_ = 0.0f;
			combRunning_ = true;
		}
		mainRunning_ = false;
		butterRunning_ = false;
		onePoleRunning_ = false;
		spreadRunning_ = false;
		return;
	}

	if (rL.base == Type::Brickwall) {
		// EACH EAR ITS OWN CORNER — the brickwall is one channel per object, so a stereo cutoff route splits it.
		if (!brickwallRunning_) {
			brickwall_[0].reset();
			brickwall_[1].reset();
			brickwallRunning_ = true;
		}
		brickwall_[0].setCorner(rL.cutoffHz, sampleRate_);
		brickwall_[1].setCorner(rR.cutoffHz, sampleRate_);
		mainRunning_ = false;
		butterRunning_ = false;
		onePoleRunning_ = false;
		spreadRunning_ = false;
		return;
	}

	// LANES {L, R, L, R}: 0 and 2 the left ear (type, partner), 1 and 3 the right — `step(stage, l, r, lm, rm)`'s
	// order.
	const std::array<SVF::Mode, 4> modes{svfModeOf(rL.base), svfModeOf(rR.base), svfModeOf(rL.morph),
	                                     svfModeOf(rR.morph)};
	const auto ears = [](double l, double r) {
		return std::array<float, 4>{static_cast<float>(l), static_cast<float>(r), static_cast<float>(l),
		                            static_cast<float>(r)};
	};
	const std::array<float, 4> corner = ears(rL.cutoffHz, rR.cutoffHz);
	const std::array<float, 4> res = ears(rL.resonance, rR.resonance);
	const std::array<float, 4> amp = ears(rL.amp, rR.amp);

	mainRunning_ = rL.plan.twoPoleStages >= 1;
	if (mainRunning_) {
		stage1_.setCoeff(modes, corner, res, srInv_, amp);
	}

	if (rL.plan.twoPoleStages >= 2) {
		if (!butterRunning_) {
			stageButter_.init();
			butterRunning_ = true;
		}
		stageButter_.setCoeff(modes, corner, std::array<float, 4>{kButterworthRes, kButterworthRes, kButterworthRes,
		                                                          kButterworthRes},
		                      srInv_, amp);
	}
	else {
		butterRunning_ = false;
	}

	if (rL.plan.onePole) {
		if (!onePoleRunning_) {
			onePoleLpL_.reset();
			onePoleLpR_.reset();
			onePoleHpL_.reset();
			onePoleHpR_.reset();
			onePoleRunning_ = true;
		}
		onePoleHighpass_ = rL.plan.onePoleHighpass;
		// NYQUIST-CLAMPED, as `solve` does it and for its reason.
		const auto cornerL = static_cast<float>(std::min(rL.cutoffHz, 0.499 * sampleRate_));
		const auto cornerR = static_cast<float>(std::min(rR.cutoffHz, 0.499 * sampleRate_));
		if (onePoleHighpass_) {
			onePoleHpL_.setCutoff(cornerL, static_cast<float>(sampleRate_));
			onePoleHpR_.setCutoff(cornerR, static_cast<float>(sampleRate_));
		}
		else {
			onePoleLpL_.setCutoff(cornerL, static_cast<float>(sampleRate_));
			onePoleLpR_.setCutoff(cornerR, static_cast<float>(sampleRate_));
		}
	}
	else {
		onePoleRunning_ = false;
	}

	if (spreadWeight_ > 0.0f || spreadWeightR_ > 0.0f) {
		if (!spreadRunning_) {
			stage2_.init();
			spreadRunning_ = true;
		}
		stage2_.setCoeff(modes, ears(rL.spreadCutoffHz, rR.spreadCutoffHz), res, srInv_, amp);
	}
	else {
		spreadRunning_ = false;
	}
}

float MultimodeFilter::driveShape(float x) const {
	// fasttanh is a Padé approximant, within 4.3e-5 of tanh on ±4.5 and past 1 beyond ±5. Clamping
	// the ARGUMENT is what makes a hot input at high drive a clip rather than an explosion.
	const float t = std::clamp(x * driveGain_, -4.5f, 4.5f);
	return feathers::dsp::fasttanh(t) * driveComp_;
}

float MultimodeFilter::driveShapeR(float x) const {
	const float t = std::clamp(x * driveGainR_, -4.5f, 4.5f);
	return feathers::dsp::fasttanh(t) * driveCompR_;
}

void MultimodeFilter::processStereoCore(float* left, float* right, int n) {
	if (!target_.enabled) {
		if (active_) {
			active_ = false;
			reset();
		}
		return; // THE BYPASS IS A RETURN, not a pass through a unity filter. Not one sample is read.
	}
	if (!active_) {
		active_ = true;
		reset();
		snap();
	}

	if (target_.type == Type::Comb) {
		for (int i = 0; i < n; i++) {
			if (untilTick_ <= 0) {
				tick();
				untilTick_ = kTickSamples;
			}
			untilTick_--;
			combDelay_ += combDelayInc_;
			combDelayR_ += combDelayIncR_;
			const float delay = std::clamp(combDelay_, kCombMinDelaySamples, kCombMaxDelaySamples);
			const float delayR = std::clamp(combDelayR_, kCombMinDelaySamples, kCombMaxDelaySamples);
			float l = left[i];
			float r = right[i];
			// THE RIGHT CHANNEL READS THE RIGHT EAR'S TWINS (backlog KU), equal to the left's on the mono road.
			if (driveAmount_ > 0.0f || driveAmountR_ > 0.0f) {
				l += driveAmount_ * (driveShape(l) - l);
				r += driveAmountR_ * (driveShapeR(r) - r);
			}
			left[i] = combStep(combLine_[0], combDampZ_[0], l, delay, combFeedback_,
			                           combDamp_, combDampGain_);
			right[i] = combStep(combLine_[1], combDampZ_[1], r, delayR, combFeedbackR_,
			                            combDampR_, combDampGainR_);
		}
		return;
	}

	if (target_.type == Type::Brickwall) {
		// SOLVED BEFORE THE FIRST SAMPLE when the type has just arrived, so no sample runs on coefficients the
		// brickwall never had. `solve` does not step the smoothers, so this moves no timing.
		if (!brickwallRunning_) {
			solve();
		}
		for (int i = 0; i < n; i++) {
			if (untilTick_ <= 0) {
				tick();
				untilTick_ = kTickSamples;
			}
			untilTick_--;
			float l = left[i];
			float r = right[i];
			if (driveAmount_ > 0.0f || driveAmountR_ > 0.0f) {
				l += driveAmount_ * (driveShape(l) - l);
				r += driveAmountR_ * (driveShapeR(r) - r);
			}
			left[i] = brickwall_[0].process(l);
			right[i] = brickwall_[1].process(r);
		}
		return;
	}

	for (int i = 0; i < n; i++) {
		if (untilTick_ <= 0) {
			tick();
			untilTick_ = kTickSamples;
		}
		untilTick_--;

		float l = left[i];
		float r = right[i];
		if (driveAmount_ > 0.0f || driveAmountR_ > 0.0f) {
			l += driveAmount_ * (driveShape(l) - l);
			r += driveAmountR_ * (driveShapeR(r) - r);
		}

		float outL = l;
		float outR = r;
		if (mainRunning_) {
			float lm = l;
			float rm = r;
			SVF::step(stage1_, l, r, lm, rm);
			outL = l + morphWeight_ * (lm - l);
			outR = r + morphWeightR_ * (rm - r);
		}
		if (butterRunning_) {
			float bl = outL;
			float br = outR;
			float blm = outL;
			float brm = outR;
			SVF::step(stageButter_, bl, br, blm, brm);
			outL = bl + morphWeight_ * (blm - bl);
			outR = br + morphWeightR_ * (brm - br);
		}
		if (onePoleRunning_) {
			outL = onePoleHighpass_ ? onePoleHpL_.step(outL) : onePoleLpL_.step(outL);
			outR = onePoleHighpass_ ? onePoleHpR_.step(outR) : onePoleLpR_.step(outR);
		}

		// `spreadRunning_` is `spreadWeight_ > 0` on the mono road (solve sets it so); with a right ear either ear's
		// weight runs the stage.
		if (spreadRunning_) {
			float sl = outL;
			float sr = outR;
			float slm = outL;
			float srm = outR;
			SVF::step(stage2_, sl, sr, slm, srm);
			const float spreadL = sl + morphWeight_ * (slm - sl);
			const float spreadR = sr + morphWeightR_ * (srm - sr);
			outL += spreadWeight_ * (spreadL - outL);
			outR += spreadWeightR_ * (spreadR - outR);
		}

		left[i] = outL;
		right[i] = outR;
	}
}

void MultimodeFilter::processMonoCore(float* mono, int n) {
	if (!target_.enabled) {
		if (active_) {
			active_ = false;
			reset();
		}
		return;
	}
	if (!active_) {
		active_ = true;
		reset();
		snap();
	}

	if (target_.type == Type::Comb) {
		for (int i = 0; i < n; i++) {
			if (untilTick_ <= 0) {
				tick();
				untilTick_ = kTickSamples;
			}
			untilTick_--;
			combDelay_ += combDelayInc_;
			const float delay = std::clamp(combDelay_, kCombMinDelaySamples, kCombMaxDelaySamples);
			float x = mono[i];
			if (driveAmount_ > 0.0f) {
				x += driveAmount_ * (driveShape(x) - x);
			}
			// CHANNEL 0, which is the same channel the mono road takes everywhere else in this file. A
			// mono buffer is one signal, and running it through both lines would be the same arithmetic
			// twice.
			mono[i] = combStep(combLine_[0], combDampZ_[0], x, delay, combFeedback_,
			                           combDamp_, combDampGain_);
		}
		return;
	}

	if (target_.type == Type::Brickwall) {
		if (!brickwallRunning_) {
			solve(); // see the stereo road
		}
		for (int i = 0; i < n; i++) {
			if (untilTick_ <= 0) {
				tick();
				untilTick_ = kTickSamples;
			}
			untilTick_--;
			float x = mono[i];
			if (driveAmount_ > 0.0f) {
				x += driveAmount_ * (driveShape(x) - x);
			}
			mono[i] = brickwall_[0].process(x); // channel 0, the mono road's channel everywhere in this file
		}
		return;
	}

	for (int i = 0; i < n; i++) {
		if (untilTick_ <= 0) {
			tick();
			untilTick_ = kTickSamples;
		}
		untilTick_--;

		float x = mono[i];
		if (driveAmount_ > 0.0f) {
			x += driveAmount_ * (driveShape(x) - x);
		}

		// The same four lanes as the stereo path, carrying the same sample twice rather than a
		// channel each: lanes 0/1 the declared type, lanes 2/3 the morph partner. Feeding zeros into
		// the spare lanes would work too and would say something untrue about what the object is.
		float out = x;
		if (mainRunning_) {
			float a = x;
			float b = x;
			float c = x;
			float d = x;
			SVF::step(stage1_, a, b, c, d);
			out = a + morphWeight_ * (c - a);
		}
		if (butterRunning_) {
			float ba = out;
			float bb = out;
			float bc = out;
			float bd = out;
			SVF::step(stageButter_, ba, bb, bc, bd);
			out = ba + morphWeight_ * (bc - ba);
		}
		if (onePoleRunning_) {
			out = onePoleHighpass_ ? onePoleHpL_.step(out) : onePoleLpL_.step(out);
		}

		if (spreadWeight_ > 0.0f) {
			float sa = out;
			float sb = out;
			float sc = out;
			float sd = out;
			SVF::step(stage2_, sa, sb, sc, sd);
			const float spread = sa + morphWeight_ * (sc - sa);
			out += spreadWeight_ * (spread - out);
		}

		mono[i] = out;
	}
}

double effectiveCutoffHz(const Settings& s) {
	return resolve(s).cutoffHz;
}

double gainAt(const Settings& s, double sampleRate, double freqHz) {
	if (!s.enabled) {
		return 1.0; // a bypass is flat, and the curve should say so rather than draw a filter
	}
	const Resolved r = resolve(s);
	// THE COMB, IN CLOSED FORM, through the SAME delay the sample loop runs — combDelaySamples is
	// called by both, so the curve stops where the comb stops rather than drawing a corner the buffer
	// cannot reach.
	if (r.base == Type::Comb) {
		const double omega = 2.0 * std::numbers::pi * freqHz / sampleRate;
		return std::abs(combResponse(combDelaySamples(r.cutoffHz, sampleRate),
		                             combFeedbackOf(r.resonance), r.combDamp, omega));
	}
	// THE BRICKWALL, through the closed form of the sections the sample loop runs (backlog NL).
	if (r.base == Type::Brickwall) {
		return feathers::dsp::kEllipticHighpass.digitalMagnitude(r.cutoffHz, sampleRate, freqHz);
	}
	const double k = kOf(r);
	const std::complex<double> z =
	    std::exp(std::complex<double>(0.0, 2.0 * std::numbers::pi * freqHz / sampleRate));

	const auto mBase = mixVectorOf(r.base, k, r.amp);
	const auto mMorph = mixVectorOf(r.morph, k, r.amp);
	std::array<double, 3> m{};
	for (size_t i = 0; i < m.size(); i++) {
		m[i] = mBase[i] + r.morphWeight * (mMorph[i] - mBase[i]);
	}

	// THE CASCADE, IN THE ORDER THE SAMPLE LOOP RUNS IT, because a product of linear systems is
	// their series connection exactly. Each factor below is a stage the plan asked for, and a plan
	// that asks for one SVF section and nothing else — which is what a 12 dB slope is — leaves this
	// arithmetic identical to what it was before slope existed.
	const double g = gOf(r.cutoffHz, sampleRate);
	std::complex<double> h1{1.0, 0.0};
	if (r.plan.twoPoleStages >= 1) {
		h1 *= stageResponse(m, g, k, z);
	}
	if (r.plan.twoPoleStages >= 2) {
		// The second section is Butterworth and its OUTPUT MIX IS THE SAME as the first's, so a
		// morphed 24 dB is a morphed filter twice over rather than a morph in front of a lowpass.
		const auto mButter = mixVectorOf(r.base, kButterworth(), r.amp);
		const auto mButterMorph = mixVectorOf(r.morph, kButterworth(), r.amp);
		std::array<double, 3> mb{};
		for (size_t i = 0; i < mb.size(); i++) {
			mb[i] = mButter[i] + r.morphWeight * (mButterMorph[i] - mButter[i]);
		}
		h1 *= stageResponse(mb, g, kButterworth(), z);
	}
	if (r.plan.onePole) {
		h1 *= onePoleResponse(g, r.plan.onePoleHighpass, z);
	}

	if (r.spreadWeight <= 0.0) {
		return std::abs(h1);
	}
	// Series, blended: out = h1·((1−w) + w·h2), which is exactly what the sample loop computes when
	// it mixes the spread stage's output back over the cascade's.
	const auto h2 = stageResponse(m, gOf(r.spreadCutoffHz, sampleRate), k, z);
	return std::abs(h1 * ((1.0 - r.spreadWeight) + r.spreadWeight * h2));
}

void gainProfile(const Settings& s, double sampleRate, float* freqOut, float* gainOut, size_t n) {
	if (n == 0) {
		return;
	}
	if (n == 1) {
		freqOut[0] = static_cast<float>(kProfileLoHz);
		gainOut[0] = static_cast<float>(gainAt(s, sampleRate, kProfileLoHz));
		return;
	}
	// Log-spaced for real: each step is the same ratio, so an octave is the same width everywhere on
	// the card. (The vendor's own profile helper spaces by 2^(i/N) scaled onto the range, which is
	// not that.)
	const double ratio = std::pow(kProfileHiHz / kProfileLoHz, 1.0 / static_cast<double>(n - 1));

	double freq = kProfileLoHz;
	for (size_t i = 0; i < n; i++) {
		freqOut[i] = static_cast<float>(freq);
		gainOut[i] = static_cast<float>(gainAt(s, sampleRate, freq));
		freq *= ratio;
	}
}

//--------------------------------------------------------------------------------//
// THE DEVICE'S INDEXED VIEW OF ITSELF — see filter_types.h for why it exists.      //
//--------------------------------------------------------------------------------//

namespace {

/// One control's declaration, or nullptr past the end.
const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	return index < std::size(kParamDescs) ? &kParamDescs[index] : nullptr;
}

/// CLAMPED AND ROUNDED BY THE CONTROL'S OWN DECLARATION — the single rule every write here obeys,
/// and the same one ParamSet::setBase applies on the parameter road. A stepped control rounds to an
/// integer index and a continuous one merely clamps; both are refused a non-finite value outright,
/// because this is the last gate before a number becomes something the DSP runs on.
float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	double v = value;
	if (desc.stepped) {
		v = std::round(v);
	}
	// A CONTROL WHOSE RANGE IS A KNOB'S TRAVEL IS NOT CLAMPED ON DECODE, because a row may carry a
	// MODULATED value and clamping it here would throw the modulation away on the way in — which is
	// the reported bug arriving through a different door. The descriptor is the one that says so,
	// and only the cutoff says it: its ten octaves are what fits a hand, while the corner is bounded
	// by Nyquist, which the DSP applies itself.
	//
	// THE KNOB ROAD STILL CLAMPS. setValueOf does it explicitly rather than relying on this, because
	// a hand on a control does stop at the end of its travel — see there.
	if (!desc.offsetsMayLeaveRange) {
		v = std::clamp(v, desc.min, desc.max);
	}
	return static_cast<float>(v);
}

} // namespace

Row rowOf(const State& state) {
	// THE FIELD ORDER, WRITTEN ONCE. Index-aligned with Param and therefore with kParamDescs, which
	// is what lets a host walk this row against the declarations without a second table.
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = state.settings.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Type)] = static_cast<float>(state.settings.type);
	row[static_cast<size_t>(Param::Cutoff)] = state.settings.cutoffLog2Hz;
	row[static_cast<size_t>(Param::Resonance)] = state.settings.resonance;
	row[static_cast<size_t>(Param::Drive)] = state.settings.drive;
	row[static_cast<size_t>(Param::Keytrack)] = state.keytrack ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Macro1)] = state.settings.macro1;
	row[static_cast<size_t>(Param::Macro2)] = state.settings.macro2;
	row[static_cast<size_t>(Param::Slope)] = static_cast<float>(state.settings.slope);
	row[static_cast<size_t>(Param::Spread)] = state.settings.spread;
	row[static_cast<size_t>(Param::Poly)] = state.poly ? 1.0f : 0.0f;
	// THE FURNITURE, carried for the slot: the device never reads either.
	row[static_cast<size_t>(Param::Mix)] = state.mix;
	row[static_cast<size_t>(Param::WetGain)] = state.wetGain;
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto present = [&](Param p) { return static_cast<size_t>(p) < fieldsPresent; };
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		// A FIELD THE PAYLOAD DOES NOT REACH KEEPS ITS DECLARED DEFAULT, which is the same road a
		// shorter record takes everywhere else in this stream: read what is there, default the rest.
		return present(p) ? coerce(*d, row[static_cast<size_t>(p)]) : static_cast<float>(d->def);
	};

	out.settings.enabled = read(Param::Enabled) >= 0.5f;
	// A STEPPED INDEX RATHER THAN A RAW CAST. A file is editable by hand, and a cast would hand the
	// DSP a Type or a Slope with no case in any switch. coerce has already clamped to the declared
	// range, so this only has to turn the number into the enum.
	out.settings.type = static_cast<Type>(static_cast<int>(read(Param::Type)));
	out.settings.cutoffLog2Hz = read(Param::Cutoff);
	out.settings.resonance = read(Param::Resonance);
	out.settings.drive = read(Param::Drive);
	out.keytrack = read(Param::Keytrack) >= 0.5f;
	out.settings.macro1 = read(Param::Macro1);
	out.settings.macro2 = read(Param::Macro2);
	out.settings.slope = static_cast<Slope>(static_cast<int>(read(Param::Slope)));
	out.settings.spread = read(Param::Spread);
	out.poly = read(Param::Poly) >= 0.5f;
	out.mix = read(Param::Mix);
	out.wetGain = read(Param::WetGain);
	// keyOffsetSemitones is deliberately untouched: it is PER-VOICE, set by whoever runs the filter,
	// and is not patch state at all. A row that carried it would be a row claiming to know something
	// only the caller knows.
	return out;
}

void sanitize(State* state) {
	if (state == nullptr) {
		return;
	}
	*state = stateOf(rowOf(*state), static_cast<size_t>(Param::kCount));
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
	// READ OFF THE ROW rather than off a second switch over the same fields — the row IS this
	// device's indexed view of itself, and a switch beside it would be a second copy of the order
	// rowOf exists to hold alone.
	return rowOf(state)[index];
}

// setModulatedValueOf lived here. It applied an offset without clamping, for the one control whose
// range is a knob's travel — and it is DELETED rather than kept, because the generic row walk that
// replaced it (Synth::modulatedRowAt) applies the same rule to every device from the descriptor, and
// stateOf now preserves what that walk produced. Its only remaining callers were its own tests, which
// is the shape the playbook names: a function whose only consumer is a test is not finished, it is
// half-installed. The claim it carried did not go anywhere — see test_fx_filter.cpp.
void setValueOf(State* state, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (state == nullptr || index >= static_cast<size_t>(Param::kCount)) {
		return;
	}
	const device::ParamDesc* desc = descOf(param);
	if (desc == nullptr || !std::isfinite(value)) {
		return;
	}
	// THROUGH THE ROW, so there is one field order and one coercion rule rather than a switch here
	// that could drift from rowOf's. A write past a control's end STOPS AT THE END rather than
	// falling through to the default — right for a hand on a knob, and the trap the DAHDSR's own
	// setValueOf records having fallen into.
	Row row = rowOf(*state);
	// CLAMPED HERE, EXPLICITLY, and not left to coerce. A hand on a knob stops at the end of the
	// travel whatever the control's range means to a modulation offset — the two roads differ and
	// this is the one that stops.
	row[index] = std::clamp(coerce(*desc, value), static_cast<float>(desc->min),
	                        static_cast<float>(desc->max));
	*state = stateOf(row, static_cast<size_t>(Param::kCount));
}

//--------------------------------------------------------------------------------//
// THE AUDIO ENTRY POINT — how a host runs this device without knowing it is a filter. //
//--------------------------------------------------------------------------------//

namespace {

/// THE STORAGE AN OPS-DRIVEN FILTER ACTUALLY NEEDS, and the reason it is not just a MultimodeFilter.
///
/// Two roads reach this device — its row and its note — and neither is enough on its own: the row says
/// WHETHER the corner follows the note (Param::Keytrack), the note says WHERE it would follow to. So
/// both are latched and the offset is recomputed whenever either moves. A host may call them in either
/// order and calls setFields far less often than setNoteContext, which is exactly why neither can be
/// allowed to depend on having seen the other first.
///
/// THE KEYTRACK DECISION LIVES HERE, WITH THE DECLARATION IT READS. Synth::renderVoices computes the
/// same offset per voice today; a device that owns a Keytrack control and cannot apply it is asking
/// every host to reimplement it, and the host that forgot is the bug this file just fixed.
struct OpsFilter {
	MultimodeFilter dsp;
	State state {};
	device::NoteContext note {};
	/// The right ear of the six (backlog KU), and which of them a stereo route reaches. `setFields` clears
	/// `rightLive` (a row alone means mono); `setFieldsR` sets both.
	Settings::Right right {};
	uint8_t rightLive = 0;

	void refresh() {
		Settings s = state.settings;
		s.right = right;
		s.rightLive = rightLive;
		// IDENTICAL TO THE PER-VOICE ROAD, deliberately: `pitch - kKeytrackReferenceKey` gated on the
		// flag, zero otherwise. Zero means every voice filters at the knob's own corner, which is a
		// real setting rather than a fallback — see Synth::renderVoices.
		// **KEYTRACK NO LONGER IMPLIES POLY**: in mono it follows the bus's collapsed note, by default
		// or from an upstream tracker card. It did — "a corner cannot follow
		// the note on a bus carrying six of them" — and in the rack plugin, which has no voices, that made
		// Keytrack do nothing at all. **THE BUS COLLAPSE IS NOW THE STANDARD ANSWER**: a host hands its bus
		// devices one note — the last key still held, kept after the release (`Synth::busNoteContext` in
		// Feathers, `RackEngine::updateNoteContext` in the rack) — or, when a Tracker stands upstream, that
		// Tracker's chosen note (`NoteOps::busNote`). So on the bus the corner follows that one note, in both
		// hosts, with no snap back to the reference key's corner on the release. Per voice nothing changed:
		// each voice's own note, as before.
		s.keyOffsetSemitones =
		    state.keytrack
		        ? static_cast<float>(static_cast<double>(note.pitchSemitones)
		                             - static_cast<double>(kKeytrackReferenceKey))
		        : 0.0f;
		dsp.setTarget(s);
	}
};

OpsFilter& asFilter(void* storage) { return *static_cast<OpsFilter*>(storage); }

} // namespace

Settings rightEarOf(const Settings& s) {
	Settings out = s;
	const auto take = [&](Ear e, float& field, float right) {
		if ((s.rightLive & earBit(e)) != 0) {
			field = right;
		}
	};
	take(Ear::Cutoff, out.cutoffLog2Hz, s.right.cutoffLog2Hz);
	take(Ear::Resonance, out.resonance, s.right.resonance);
	take(Ear::Drive, out.drive, s.right.drive);
	take(Ear::Macro1, out.macro1, s.right.macro1);
	take(Ear::Macro2, out.macro2, s.right.macro2);
	take(Ear::Spread, out.spread, s.right.spread);
	out.rightLive = 0;
	return out;
}

/// EVERY THUNK IS TRIVIAL ON PURPOSE. The ops table exists so a host can drive a device it does not
/// know the type of; it is not a place to put behaviour. Anything interesting here would be
/// behaviour the DSP class should have had.
const Settings& opsTargetForTest(const void* storage) {
	return static_cast<const OpsFilter*>(storage)->dsp.target();
}

double opsBrickwallCornerForTest(const void* storage) {
	return static_cast<const OpsFilter*>(storage)->dsp.brickwallCornerForTest();
}

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsFilter),
    .stateAlign = alignof(OpsFilter),
    // CONSTRUCTED INTO STORAGE THE HOST OWNS, because the host is the one under
    // no-allocation-after-activate. The device says how big it is; where it lives is not its
    // business.
    .construct = [](void* storage) { new (storage) OpsFilter(); },
    .destruct = [](void* storage) { asFilter(storage).~OpsFilter(); },
    .prepare = [](void* storage, double sampleRate, int) { asFilter(storage).dsp.prepare(sampleRate); },
    // RESTART, NOT reset. AudioOps::reset means "as if just switched on", and MultimodeFilter draws
    // that distinction itself: reset() forgets the delay line, restart() also forgets that the device
    // was ACTIVE, so the next block snaps its coefficients instead of ramping them in from the last
    // note's corner. This said reset() until the per-voice runner became the entry point's first
    // caller — the weaker meaning, chosen with nothing to interrogate it.
    //
    // FOUND BY READING AND NOT BY A FAILURE, and the first draft of this comment claimed otherwise.
    // The obvious guard — "a voice takes no resonant tail from the note before it" — replays THE SAME
    // KEY, so both notes want the same corner and a carried-in coefficient is a correct one; a
    // known-bad putting reset() back leaves it green. The case that does distinguish them plays a
    // note four octaves from the one before it: "a reclaimed voice does not open at the corner the
    // LAST note left behind".
    .reset = [](void* storage) { asFilter(storage).dsp.restart(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, which is what puts absent fields at their DECLARED
	        // defaults rather than at zero — the rule device_row.h states, arriving here as a bare
	        // pointer and a count because the device vocabulary cannot see the host's DeviceRow.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        // POLY IS NOT THE DSP'S — it decides how many filters there are, which is the host's
	        // architecture and cannot be answered from inside one of them. KEYTRACK IS, and that is the
	        // correction: it decides where THIS filter's corner sits, so the whole State is latched and
	        // refresh() folds the note in. Handing setTarget the decoded Settings directly is what
	        // zeroed keyOffsetSemitones on every call.
	        asFilter(storage).state = stateOf(row, take);
	        asFilter(storage).rightLive = 0; // setFields means mono until setFieldsR says otherwise
	        asFilter(storage).refresh();
        },
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) {
	        asFilter(storage).note = note;
	        asFilter(storage).refresh();
        },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asFilter(storage).dsp.processStereo(left, right, frames);
        },
    // THE MONO ROAD, which this device has always had and never published. The per-voice path renders
    // one buffer whenever the character bank's stereo window is shut, and processStereo here would
    // dereference a null right.
    .processMono =
        [](void* storage, float* mono, int frames) { asFilter(storage).dsp.processMono(mono, frames); },
    .setFieldsR =
        [](void* storage, const float* fieldsR, size_t fieldsPresent, const device::ControlMask& live) {
	        // THE SIX, each coerced as `stateOf` coerces the left, taken only where `live` marks it (backlog KU).
	        OpsFilter& f = asFilter(storage);
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fieldsR[i];
	        }
	        const Settings decoded = stateOf(row, take).settings;
	        f.right = { decoded.cutoffLog2Hz, decoded.resonance, decoded.drive,
		                decoded.macro1,       decoded.macro2,    decoded.spread };
	        f.rightLive = 0;
	        for (size_t e = 0; e < std::size(kEarParams); ++e) {
		        if (live.test(static_cast<size_t>(kEarParams[e]))) {
			        f.rightLive = static_cast<uint8_t>(f.rightLive | (1u << e));
		        }
	        }
	        f.refresh();
        },
    .holdsTail = [](const void* storage) { return static_cast<const OpsFilter*>(storage)->dsp.holdsTail(); },
};

} // namespace feathers::fx::filter
