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

#pragma once

#include "device/device.h"
#include "device/registry.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// HARM — STRIP THE FUNDAMENTAL, AND PUT A CLEAN HARMONIC BACK.
//
// A note-tracking NOTCH and a sine sub-oscillator, in one device. Ported from the reference fork's
// `dsp/harm.h` rather than lifted: that is Q31 fixed-point built on the firmware's own sine table,
// and this plugin never calls that render layer.
//
// WHY IT IS ONE DEVICE RATHER THAN A FILTER PLUS AN OSCILLATOR IN A RACK. HARM is its own device
// because it does something specific: strip the fundamental. The filter could do it, but only with
// more filter instances in a rack — HARM is a convenience.
//
// THE TIEBREAKER IS WORTH MORE THAN THE DECISION, because "could this be built from parts we already
// have?" is the wrong question — once the rack holds heterogeneous devices the answer is almost
// always yes, and answering it that way ships a parts bin and calls it an instrument. The test is
// whether the job has a name a musician would use. HARM's is *strip the fundamental*.
//
// AND THE TWO HALVES KNOW ABOUT EACH OTHER, which is what a hand-assembled rack could not express:
// THE NOTCH CENTRE TRACKS THE OSCILLATOR'S HARMONIC. Turn the sub-oscillator to a fifth and the
// notch moves to that fifth. There is no routing anywhere in this plugin that would let two separate
// devices do that, and inventing one would be a worse answer than the packaging.
//
//=================================================================================================//
// IT IS A NOTCH. THE FIRMWARE CALLS IT AN HPF IN THIRTEEN PLACES AND IS WRONG IN ALL OF THEM.       //
//=================================================================================================//
//
// The reference fork replaced the cascaded HPF with a biquad notch on 2026-04-11 (a37c8fd), renamed it
// (3644aef), and made it track the oscillator's pitch (9847046) — and THE RENAME NEVER FINISHED. The
// field is still `hpf`, its comment still says "Note-tracking HPF cutoff", the predicate is still
// `isHpfEnabled()`, and the function that renders the notch is still called `renderHpf`. The design
// doc beside it says HPF thirty-six times and notch zero.
//
// WHEN A RENAME IS PARTIAL, THE OLD NAME BECOMES A LIE THAT GREPS — worse than a stale comment,
// because a comment is checked against the code beside it while a name is checked against nothing
// and travels into every call site. Both facts it hid would have produced a plausible number doing
// the wrong job:
//
//   - `hpf` 1..127 IS WIDTH, NOT CUTOFF, and inverted: 1 is surgical, 127 is wide. The firmware
//     computes Q = 30 - ((hpf-1)/126) * 29.5, so the knob runs Q=30 down to Q=0.5.
//   - THE CENTRE IS NOT A PARAMETER AT ALL. It is derived per buffer from the note, the harmonic
//     ratio, fine tune and pitch bend.
//
// So this device's control is called Width and means width, and the centre is not a control.
//
//=================================================================================================//
// WHAT WAS DELIBERATELY NOT PORTED                                                                 //
//=================================================================================================//
//
// THE SPLIT RENDER. The firmware runs the notch PRE-reverb and mixes the oscillator in POST-reverb,
// so the fundamental is stripped before the reverb smears it and the clean harmonic is added back
// dry. That structure exists entirely to straddle a reverb. THIS PLUGIN HAS NO REVERB. Copying it
// faithfully would produce something that looks like the original and means nothing; the two signal
// paths were independent anyway.
//
// PHASE AND SPREAD ARE TWO CONTROLS HERE AND ONE THERE. The firmware packs them into a single 0..127
// knob — 0..64 is start phase, 65..127 is stereo spread — which is a hardware front-panel constraint
// rather than a design. A packed control is one control that cannot say what it is, and we have room
// for two.
//
// DEFERRED, DO NOT FORECLOSE: a rack inside HARM for send effects, post-notch and
// pre-oscillator. It is suddenly cheap — the rack holds heterogeneous devices and device_row.h moves
// a device's settings without caring what it is — so the open question is whether a device can
// CONTAIN a slot space rather than only sit in one. Nothing here should make that harder.

namespace feathers::fx::harm {

/// THE HARMONIC RATIOS, and the table IS the device rather than an implementation detail: these are
/// chosen musical intervals — eighths and thirds and quarters low down, whole numbers high up —
/// rather than a linear sweep, which is what makes the knob land on things worth hearing.
///
/// Carried across verbatim from the fork because the choice of ratios is the port's whole content on
/// this control; deriving them would be inventing a different device.
struct HarmonicRatio {
	uint8_t num;
	uint8_t den;
	float ratio;
};

inline constexpr HarmonicRatio kHarmonics[] = {
    {1, 8, 0.125f},      {1, 4, 0.25f},        {1, 3, 0.33333333f},  {3, 8, 0.375f},
    {1, 2, 0.5f},        {5, 8, 0.625f},       {2, 3, 0.66666667f},  {3, 4, 0.75f},
    {7, 8, 0.875f},      {1, 1, 1.0f},         {9, 8, 1.125f},       {5, 4, 1.25f},
    {4, 3, 1.33333333f}, {11, 8, 1.375f},      {3, 2, 1.5f},         {13, 8, 1.625f},
    {5, 3, 1.66666667f}, {7, 4, 1.75f},        {15, 8, 1.875f},      {2, 1, 2.0f},
    {17, 8, 2.125f},     {9, 4, 2.25f},        {7, 3, 2.33333333f},  {19, 8, 2.375f},
    {5, 2, 2.5f},        {21, 8, 2.625f},      {8, 3, 2.66666667f},  {11, 4, 2.75f},
    {23, 8, 2.875f},     {3, 1, 3.0f},         {25, 8, 3.125f},      {13, 4, 3.25f},
    {10, 3, 3.33333333f},{27, 8, 3.375f},      {7, 2, 3.5f},         {29, 8, 3.625f},
    {11, 3, 3.66666667f},{15, 4, 3.75f},       {31, 8, 3.875f},      {4, 1, 4.0f},
    {17, 4, 4.25f},      {13, 3, 4.33333333f}, {9, 2, 4.5f},         {14, 3, 4.66666667f},
    {19, 4, 4.75f},      {5, 1, 5.0f},         {21, 4, 5.25f},       {16, 3, 5.33333333f},
    {11, 2, 5.5f},       {17, 3, 5.66666667f}, {23, 4, 5.75f},       {6, 1, 6.0f},
    {25, 4, 6.25f},      {19, 3, 6.33333333f}, {13, 2, 6.5f},        {20, 3, 6.66666667f},
    {27, 4, 6.75f},      {7, 1, 7.0f},         {29, 4, 7.25f},       {22, 3, 7.33333333f},
    {15, 2, 7.5f},       {23, 3, 7.66666667f}, {31, 4, 7.75f},       {8, 1, 8.0f},
    {25, 3, 8.33333333f},{17, 2, 8.5f},        {26, 3, 8.66666667f}, {9, 1, 9.0f},
    {28, 3, 9.33333333f},{19, 2, 9.5f},        {29, 3, 9.66666667f}, {10, 1, 10.0f},
    {31, 3, 10.3333333f},{21, 2, 10.5f},       {32, 3, 10.6666667f}, {11, 1, 11.0f},
    {23, 2, 11.5f},      {12, 1, 12.0f},       {25, 2, 12.5f},       {13, 1, 13.0f},
    {27, 2, 13.5f},      {14, 1, 14.0f},       {29, 2, 14.5f},       {15, 1, 15.0f},
    {31, 2, 15.5f},      {16, 1, 16.0f},       {17, 1, 17.0f},       {18, 1, 18.0f},
    {19, 1, 19.0f},      {20, 1, 20.0f},       {21, 1, 21.0f},       {22, 1, 22.0f},
    {23, 1, 23.0f},      {24, 1, 24.0f},       {25, 1, 25.0f},       {26, 1, 26.0f},
    {27, 1, 27.0f},      {28, 1, 28.0f},       {29, 1, 29.0f},       {30, 1, 30.0f},
    {31, 1, 31.0f},      {32, 1, 32.0f},
};

inline constexpr size_t kHarmonicCount = std::size(kHarmonics);

/// WHAT A RATIO IS CALLED, as a musician would say it.
///
/// lets give some helpful color text in the dropdown (eg 1 oct, perfect 5th). A ratio of
/// 3:2 tells a synthesist nothing at a glance and "perfect 5th" tells them what they will hear — so
/// the name is COLOUR TEXT BESIDE the ratio rather than a replacement for it. The ratio is what the
/// DSP multiplies by; the name is what the hand is reaching for. Both, always.
///
/// NAMED ONLY WHERE THE RATIO REALLY IS THE INTERVAL, and that is the whole design of this. The
/// table is JUST — 3:2 is a perfect fifth exactly, 5:4 a major third exactly — but it also holds
/// ratios like 11:8 and 13:8 that sit roughly HALF A SEMITONE from anything with a name. Calling
/// 11:8 "a fourth" would be 51 cents wrong, which is not colour text, it is a lie a player would
/// tune against. Those entries answer nullptr and the card shows the ratio alone.
///
/// MATCHED BY EXACT INTEGER ARITHMETIC, never by comparing floats against a tolerance: the ratio is
/// stored as num/den, so the octave folding and the comparison are both exact and there is no
/// threshold to argue about.
struct Interval {
	/// How many octaves the ratio is above (positive) or below (negative) its named interval.
	int octaves = 0;
	/// The interval inside one octave, or nullptr where the ratio does not name one.
	const char* name = nullptr;
};

/// The interval a bare num/den names. Takes the PAIR rather than a table index so a caller can ask
/// about a ratio the table does not hold — which is how a descending entry is named by its
/// reciprocal even when the reciprocal has no row of its own (5:8 is in the table; 8:5 is not).
constexpr Interval intervalOfRatio(int num, int den) {
	if (num <= 0 || den <= 0) {
		return {};
	}
	int octaves = 0;
	// FOLD INTO ONE OCTAVE, counting how many it took. Integer doubling of the denominator or the
	// numerator, so nothing is approximated on the way.
	while (num >= den * 2) {
		den *= 2;
		++octaves;
	}
	while (num < den) {
		num *= 2;
		--octaves;
	}
	// Lowest terms, so the comparison below is a plain pair match.
	int a = num;
	int b = den;
	while (b != 0) {
		const int t = a % b;
		a = b;
		b = t;
	}
	if (a > 0) {
		num /= a;
		den /= a;
	}

	struct Named {
		int num;
		int den;
		const char* name;
	};
	// THE INTERVALS THIS TABLE ACTUALLY CONTAINS, in lowest terms inside one octave. Anything not
	// here has no honest name — see the note above about 11:8.
	constexpr Named kNames[] = {
	    {1, 1, "unison"},     {9, 8, "major 2nd"},  {6, 5, "minor 3rd"},    {5, 4, "major 3rd"},
	    {4, 3, "perfect 4th"}, {3, 2, "perfect 5th"}, {8, 5, "minor 6th"},  {5, 3, "major 6th"},
	    {7, 4, "harmonic 7th"}, {15, 8, "major 7th"},
	};
	for (const Named& n : kNames) {
		if (n.num == num && n.den == den) {
			return {octaves, n.name};
		}
	}
	return {octaves, nullptr};
}

constexpr Interval intervalOf(size_t index) {
	return index < kHarmonicCount ? intervalOfRatio(static_cast<int>(kHarmonics[index].num),
	                                                static_cast<int>(kHarmonics[index].den))
	                              : Interval {};
}

/// The whole label a control shows for a harmonic: the ratio, and the interval beside it where the
/// ratio names one. "3:2 · perfect 5th", "4:1 · 2 oct", "11:8" — see intervalOf.
std::string harmonicLabel(size_t index);
/// The index that means 1:1, which is the default — a sub-oscillator at the fundamental.
inline constexpr size_t kUnisonHarmonic = 9;

/// The ratio at `index`, clamped. Index kHarmonicCount and above is the caller's error and answers
/// unison rather than reading past the table.
constexpr float ratioAt(size_t index) {
	return index < kHarmonicCount ? kHarmonics[index].ratio : 1.0f;
}

//---------------------------------------------------------------------------------//
// The controls — index-aligned with kParamDescs and with Row below.                 //
//---------------------------------------------------------------------------------//

enum class Param : int {
	/// IS THIS DEVICE IN THE SIGNAL — the same switch every other effect has, and FIRST as the
	/// filter's is.
	///
	/// REVERSING THE CARD'S ORIGINAL READING: HARM has the same active/inactive switch as every other
	/// effect, and the "level=0 means off" semantics, which were hardware-specific, are gone.
	///
	/// THE CARD DECLINED TO INVENT ONE, on the grounds that a control a device does not declare is a
	/// control the honesty rule forbids drawing — so it dimmed on `Level > 0 || Width > 0` instead.
	/// That was the right call to make from the device as it stood; the answer is that the DEVICE was
	/// wrong, not the card. A convention where "turn it down to nothing" doubles as "switch it off"
	/// came from a machine with a fixed front panel and no room for a switch, and this is not that
	/// machine.
	///
	/// IT IS NOT THE IDENTITY GUARANTEE, and the two must not be confused now that both exist. Level
	/// at zero adds nothing and Width at zero is NO notch rather than a very sharp one; those are
	/// bit-exact promises about what an ENABLED device does with its controls down, and they survive
	/// this unchanged. What goes is the idea that they also mean "off".
	///
	/// INSERTED AT THE FRONT rather than appended, which moves every other control's index. That is
	/// allowed and cheap here — CLAUDE.md, on patch compatibility: *"Insert where it belongs."* No
	/// saved patch and no automation names these. The modulation ORDINALS are unmoved regardless,
	/// because a switch is not a modulation destination and ordinals count only the controls that
	/// are.
	Enabled,
	/// Which harmonic the sub-oscillator sits at. Stepped, an index into kHarmonics.
	Harmonic,
	/// How loud it is. Zero is silence and is the DEFAULT, so a HARM added to a chain is inaudible
	/// until a hand asks for it — the same identity guarantee FM Depth carries.
	Level,
	/// ±12 semitones on the sub-oscillator, and therefore on the notch centre with it.
	Fine,
	/// Where the oscillator's cycle starts, 0..1 of a turn.
	Phase,
	/// Stereo spread, 0..1 of a half turn. SEPARATE FROM PHASE, unlike the firmware — see the header.
	Spread,
	Attack,
	Release,
	/// HOW WIDE THE NOTCH IS. Not a cutoff, whatever the firmware calls it: 0 is off entirely, and
	/// above that the notch runs from surgical to wide.
	Width,
	/// HOW DEEP THE NOTCH CUTS — 0 is flat, 1 removes the centre outright.
	///
	/// WHETHER ONE EXISTED WAS ASKED and the answer was no, and no partial one COULD exist as the
	/// notch was built: RBJ's form puts the zero pair exactly on the unit circle, so the centre is
	/// removed by construction and Width only says how WIDE the cut is. A notch you cannot make
	/// shallower is a strong effect with no dial — at unison it deletes the fundamental outright,
	/// which is the device's job, and at other harmonics it is often more than anyone wanted.
	///
	/// IT WORKS BY PULLING THE ZEROS INSIDE THE CIRCLE, giving the numerator its own alpha:
	/// `alphaZ = alpha * (1 - depth)`. At depth 1 that is alphaZ = 0 and the coefficients are
	/// exactly what they were; at depth 0 the numerator equals the denominator termwise and the
	/// filter is a straight wire.
	///
	/// AND THE ATTENUATION AT THE CENTRE IS EXACTLY `1 - depth`, LINEAR — independent of the centre
	/// frequency, the Q and the sample rate, because both numerator and denominator collapse to the
	/// same form at w0 and everything but the two alphas cancels. That is what makes it assertable
	/// as a VALUE rather than as a shape, which for a partial effect is the difference between a
	/// test and a decoration.
	Depth,
	/// Portamento on the tracked frequency.
	///
	/// IT IS LOAD-BEARING IN MONO AND HAS ALMOST NOTHING TO DO IN POLY, which is the opposite way
	/// round from how this comment first read.
	///
	/// IT SAID THE GLIDE WAS NEARLY VESTIGIAL, and that was true while the bus followed the LOWEST
	/// held note — a target that barely moves, because the bottom of a chord is the note a hand
	/// changes least. that rule was replaced on 2026-08-24 with LAST-note priority, and the target
	/// now jumps on every note-on and on every release that falls back. **The example given for the new
	/// rule is literally a porta up and a porta down**: with D0 held and F0 played and released, the sub
	/// glides up to F0 and back down to D0. The glide is what makes that musical instead of a click.
	///
	/// IN POLY IT STILL HAS LITTLE TO DO, and that half stands: a per-voice HARM tracks one voice,
	/// and a voice's target pitch does not jump — it is given its note at voice start and only ever
	/// glides, in mono and legato, where the voice's OWN glide is already doing the smoothing.
	///
	/// SO IT IS THE SAME SHAPE AS KEYTRACK, one device over — a control whose job exists in one of
	/// two architectures — with the architectures the other way about. Written down because a knob
	/// that appears to do nothing in the arrangement a reader happens to be in is a knob somebody
	/// files as broken.
	Porta,
	/// ONE HARM PER VOICE, OR ONE ON THE SUMMED BUS — the same architectural switch the filter grew
	/// on 2026-08-23, and for the same reason: it decides how many of the device there are rather
	/// than what one of them does. That was decided, and so was this: with poly OFF the device tracks the
	/// LOWEST SOUNDING NOTE.
	Poly,
	/// THE NOTCH'S DEPTH FOLLOWS THE NOTE ENVELOPE — filter depth follows env, as an
	/// OPTION. Off, the notch sits at its Depth whenever Width is up, notes or none, holding the last note's
	/// centre between them. On, Depth is scaled by the same attack/release envelope that gates the
	/// oscillator: no note, no notch; it fades in over Attack and out over Release. Per voice when Poly,
	/// because each voice's HARM has its own envelope.
	DepthFollowsEnv,
	/// NOTCH OR HIGH-PASS — backlog NL (a switch on HARM selecting notch or high-pass). Notch strips the tracked harmonic alone; High-pass is the keytracked
	/// elliptic brickwall (dsp/elliptic_highpass.h, the Filter device's "Brickwall HP" too) that removes the
	/// tracked harmonic AND EVERYTHING BELOW IT, at least 60 dB down, and keeps the octave above within 0.1 dB.
	/// TWO STATES, so a switch rather than a multivalue toggle. Width, Depth and Depth Follows Env shape the
	/// notch and nothing else: in High-pass they are greyed, because the brickwall has one fixed shape.
	Mode,
	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog OW), supplied and applied by the slot: the device
	/// never reads either, and the card does not draw them (the site's head row does). Mix is a crossfade (100% the
	/// wet), the wet Gain a fader position (unity at 0.5) on the wet only. Both defaults are HARM as it was, to the bit.
	/// Mix is NOT Level: Level is the oscillator's loudness inside the wet; Mix is the slot's dry against the whole wet.
	Mix,
	WetGain,
	/// **THE FIRST PARTIAL'S SWITCH** — see `kPartialCount`. On by default, so a patch from before partials existed (which
	/// stores no such key) plays its one partial exactly as it did. The device's own enable stays `Enabled`'s.
	Partial1Active,
	/// **THE ENVELOPE'S OTHER FOUR STAGES.** Attack and Release above are the A and R of a full DAHDSR; these are its D, H, D
	/// and S. Their defaults (no delay, no hold, decay off, sustain at the top) make the envelope the attack-and-release
	/// one it always was, to the bit. See `Envelope` in harm.h.
	Delay,
	Hold,
	Decay,
	Sustain,
	/// **PARTIALS 2 TO 4**, nine controls each in `PartialField` order, so a partial's control is `partialParam`. Generated from
	/// one macro, below, so the four cannot drift.
	Partial2Harmonic,
	kCount = Partial2Harmonic + 3 * 9,
};

/// HOW MANY HARMONICS ONE HARM PLAYS: four identical partials, each its own notch and its own sine. The first keeps the
/// device's original keys and meanings; the other three are the same controls under `harm_p2_`..`harm_p4_`.
inline constexpr int kPartialCount = 4;

/// The nine controls one partial is, in the order the extra partials lay them out. The first partial's are the
/// device's original controls (and `Partial1Active`), found by `partialParam`.
enum class PartialField : int { Harmonic, Level, Fine, Phase, Spread, Width, Depth, Mode, Active, kCount };
inline constexpr int kPartialFieldCount = static_cast<int>(PartialField::kCount);
static_assert(kPartialFieldCount == 9);

/// THE PARAMETER A PARTIAL'S FIELD LIVES AT. `partial` is 0..3.
constexpr Param partialParam(int partial, PartialField field) {
	if (partial <= 0) {
		switch (field) {
		case PartialField::Harmonic: return Param::Harmonic;
		case PartialField::Level: return Param::Level;
		case PartialField::Fine: return Param::Fine;
		case PartialField::Phase: return Param::Phase;
		case PartialField::Spread: return Param::Spread;
		case PartialField::Width: return Param::Width;
		case PartialField::Depth: return Param::Depth;
		case PartialField::Mode: return Param::Mode;
		case PartialField::Active:
		case PartialField::kCount: break;
		}
		return Param::Partial1Active;
	}
	return static_cast<Param>(static_cast<int>(Param::Partial2Harmonic) + (partial - 1) * kPartialFieldCount
	                          + static_cast<int>(field));
}

inline constexpr const char* kOffOnNames[] = {"Off", "On"};
/// The Mode switch's two states, in stored order — a Choice is stored by LABEL, so these words are the patch's.
inline constexpr const char* kModeNames[] = {"Notch", "High-pass"};

/// log2(seconds) for the time controls, exactly as the DAHDSR's stages are declared — a time knob is
/// even in octaves, and the minimum means "no time at all" because a logarithmic domain has no zero.
inline constexpr double kTimeLog2Min = -11.0; // 0.49 ms
inline constexpr double kTimeLog2Max = 3.3219280948873626; // 10 s

/// ONE PARTIAL'S NINE ROWS, for partials 2 to 4. `HARMONIC` is the default ratio's index in `kHarmonics` (19 is 2:1, 29 is
/// 3:1, 39 is 4:1: the octave, the fifth above it, two octaves), and the default of Active is OFF, so switching one on adds
/// a harmonic worth hearing. Every other number is the first partial's own.
#define FEATHERS_HARM_PARTIAL_DESCS(KEY, NAME, HARMONIC, ORDINAL) \
    {KEY "harmonic", NAME "Harmonic", 0.0, static_cast<double>(kHarmonicCount) - 1.0, HARMONIC, true, device::Unit::Number, {}, \
     false, false, "Which harmonic the " ORDINAL " partial's notch targets and its sine plays"}, \
    {KEY "level", NAME "Level", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false, \
     "Output level of the " ORDINAL " partial's sine"}, \
    {KEY "fine", NAME "Fine", -12.0, 12.0, 0.0, false, device::Unit::Number, {}, true, false, \
     "Fine pitch adjustment of the " ORDINAL " partial in semitones — tunes its notch target precisely"}, \
    {KEY "phase", NAME "Phase", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false, \
     "Where in its cycle the " ORDINAL " partial's sine starts"}, \
    {KEY "spread", NAME "Spread", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false, \
     "Stereo spread of the " ORDINAL " partial's sine"}, \
    {KEY "width", NAME "Width", 0.0, 1.0, 0.3, false, device::Unit::Percent, {}, true, false, \
     "How wide the " ORDINAL " partial's notch is — 0 is a wire, higher is a broader cut. Notch mode only"}, \
    {KEY "depth", NAME "Depth", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false, \
     "How much of the centre the " ORDINAL " partial's notch removes — 1 is a full null, 0 is nothing. Notch mode only"}, \
    {KEY "mode", NAME "Mode", 0.0, 1.0, 0.0, true, device::Unit::Choice, kModeNames, false, false, \
     "The " ORDINAL " partial strips its harmonic with a notch, or with a steep high-pass that also removes everything below it"}, \
    {KEY "active", NAME "Active", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false, \
     "Whether the " ORDINAL " partial — its notch and its sine — is in the sound"},

inline constexpr device::ParamDesc kParamDescs[] = {
    // **ON BY DEFAULT — all effects should default to on when they are added.**
    // This said OFF so that "an fx chain holding a HARM renders the same bits it
    // rendered before the device was added" — true, and no longer the behaviour asked for: a device
    // only gets into a chain by a hand pressing (+), and that press is the intent to use it. See
    // `fx::filter::kParamDescs` for the full statement of the decision.
    //
    // The bypass is still HARD rather than a unity-gain pass: processStereo returns without touching
    // the buffer. That half was never about the default.
    {"harm_enabled", "Harm Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Bypass switch — off removes the device from the chain"},
    {"harm_harmonic", "Harm Harmonic", 0.0, static_cast<double>(kHarmonicCount) - 1.0,
     static_cast<double>(kUnisonHarmonic), true, device::Unit::Number, {}, false, false,
     "Which harmonic the notch targets — 1 is the fundamental, 2 the octave, and so on"},
    // FULL BY DECISION, 2026-08-24, and the number is the decided one rather than the one that was
    // proposed. A HARM you switch on was SILENT — Level 0 meant no oscillator — which read as
    // unresponsive knobs: every control turned and nothing was audible.
    //
    // AND IT FIXES THE PICTURE AS WELL AS THE SOUND. `notchCentreHz` falls back to the fundamental
    // only while the oscillator is silent, so at Level 1 that branch never runs on a fresh device
    // and the notch sits at the SELECTED harmonic from the first frame. The fallback is still real
    // and still asserted — it is a state you reach by turning Level down, rather than the state you
    // start in.
    //
    // HALF WAS PROPOSED AND OVERRIDDEN UPWARD. A harmonic you have to go looking for is nearer
    // the problem reported than its fix.
    {"harm_level", "Harm Level", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Output level of the oscillator that drives the notch"},
    {"harm_fine", "Harm Fine", -12.0, 12.0, 0.0, false, device::Unit::Number, {}, true, false,
     "Fine pitch adjustment in semitones — tunes the notch target precisely"},
    {"harm_phase", "Harm Phase", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Where in its cycle the oscillator starts — shifts the notch's phase"},
    {"harm_spread", "Harm Spread", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Offsets a second notch by ±octaves — widens the image"},
    {"harm_attack", "Harm Attack", kTimeLog2Min, kTimeLog2Max, kTimeLog2Min, false, device::Unit::LogSeconds, {},
     true, false, "How long the notch takes to engage after a note"},
    // THE FIRMWARE'S DEFAULT OF 64/127 ON ITS RELEASE CURVE IS A SECOND, NOT A QUARTER: `1 + 64 *
    // (2 * 44100 / 127)` = 44448 samples of time constant at 44.1 kHz, 1.0079 s. This row said "a
    // quarter second, the firmware's own default" and was four times too fast (HARM parity sweep,
    // 2026-09-16). Declared as ONE second — log2 0 — because a log2 of 1.0079 is not exactly
    // representable and the fresh-state-equals-declared case compares rows bit for bit; 0.8 % is
    // below anything a release time can be heard to.
    {"harm_release", "Harm Release", kTimeLog2Min, kTimeLog2Max, 0.0, false, device::Unit::LogSeconds, {}, true,
     false, "How long the notch takes to fade out after the note ends"},
    // A NOTCH THAT EXISTS. Width 0 is a wire — `refreshCoefficients`' own rule — so a fresh HARM
    // had no notch at all, and the ui doesnt draw any notch filter? was literally
    // correct. MEASURED before choosing: 0.3 gives Q = 21.2 and about 5 px of visible dip on a
    // 284 px eight-octave plot, full height at the default Depth of 1.
    {"harm_width", "Harm Width", 0.0, 1.0, 0.3, false, device::Unit::Percent, {}, true, false,
     "How wide the notch is — 0 is a wire, higher is a broader cut. Notch mode only: the High-pass has one fixed shape"},
    // FULL DEPTH BY DEFAULT, AND THAT IS THE RIGHT DEFAULT RATHER THAN A COMPATIBLE ONE. A device
    // whose one-phrase job is *strip the fundamental* should strip it when you turn it on; that
    // depth 1 also reproduces the behaviour that shipped is a convenience, not the argument.
    {"harm_depth", "Harm Depth", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "How much of the centre the notch removes — 1 is a full null, 0 is nothing. Notch mode only"},
    {"harm_porta", "Harm Porta", kTimeLog2Min, kTimeLog2Max, kTimeLog2Min, false, device::Unit::LogSeconds, {},
     true, false, "Glide time between notes — the notch slides instead of jumping"},
    {"harm_poly", "Harm Poly", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "One notch per voice, or one on the summed bus"},
    // OFF BY DEFAULT, so a HARM renders the bits it rendered before the option existed.
    {"harm_depth_env", "Harm Depth Follows Env", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Depth follows the note envelope — no note, no notch; it fades in over Attack and out over Release. Notch mode only"},
    // NOTCH BY DEFAULT: the device that shipped, and the gentler of the two.
    {"harm_mode", "Harm Mode", 0.0, 1.0, 0.0, true, device::Unit::Choice, kModeNames, false, false,
     "Notch removes the tracked harmonic alone. High-pass is a steep brickwall that removes it and everything "
     "below, keeping the octave above within 0.1 dB"},
    {"harm_mix", "Harm Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry against processed, around the whole device"},
    {"harm_wet_gain", "Harm Wet Gain", 0.0, 1.0, 0.5, false, device::Unit::Fader, {}, true, false,
     "Level of the processed signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
    // THE FIRST PARTIAL'S SWITCH. ON BY DEFAULT, which is what a patch that never stored it means: one partial, playing.
    {"harm_p1_active", "Harm 1 Active", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Whether the first partial — its notch and its sine — is in the sound"},
    // THE ENVELOPE'S D, H, D AND S, beside the Attack and Release above. Times are the same log2 seconds, and the minimum of
    // a time means the stage does not happen (the DAHDSR's own convention). The defaults — no delay, no hold, decay
    // off, sustain full — leave the envelope exactly the attack/release one the device had.
    {"harm_delay", "Harm Delay", kTimeLog2Min, kTimeLog2Max, kTimeLog2Min, false, device::Unit::LogSeconds, {}, true, false,
     "A silence between the note starting and the envelope's attack beginning"},
    {"harm_hold", "Harm Hold", kTimeLog2Min, kTimeLog2Max, kTimeLog2Min, false, device::Unit::LogSeconds, {}, true, false,
     "How long the envelope stays at its peak before the decay begins"},
    {"harm_decay", "Harm Decay", kTimeLog2Min, kTimeLog2Max, kTimeLog2Min, false, device::Unit::LogSeconds, {}, true, false,
     "How long the envelope takes to fall from its peak to the sustain level; at the bottom of its travel the fall is instant"},
    {"harm_sustain", "Harm Sustain", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "The level the envelope holds at while the note is pressed"},
    // PARTIALS 2, 3 AND 4 — ONE MACRO, SO THE THREE CANNOT DRIFT FROM EACH OTHER, and `static_assert`s below hold them to
    // the first partial's rows. Each differs from the first only in key, name, default harmonic and a default of OFF.
    FEATHERS_HARM_PARTIAL_DESCS("harm_p2_", "Harm 2 ", 19, "second")
    FEATHERS_HARM_PARTIAL_DESCS("harm_p3_", "Harm 3 ", 29, "third")
    FEATHERS_HARM_PARTIAL_DESCS("harm_p4_", "Harm 4 ", 39, "fourth")
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE FOUR PARTIALS' DESCRIPTORS ARE ONE DESCRIPTOR, checked rather than hoped. Everything but key, name and tooltip must
/// match the first partial's row — and the two defaults that are the point of having partials to switch on (Active, and
/// the ratio) are held to their own values.
constexpr bool partialsAgree() {
	for (int k = 1; k < kPartialCount; ++k) {
		for (int f = 0; f < kPartialFieldCount; ++f) {
			const auto field = static_cast<PartialField>(f);
			const device::ParamDesc& a = kParamDescs[static_cast<size_t>(partialParam(0, field))];
			const device::ParamDesc& b = kParamDescs[static_cast<size_t>(partialParam(k, field))];
			const bool defOk = field == PartialField::Active ? b.def == 0.0
			                   : field == PartialField::Harmonic ? b.def > a.def
			                                                     : b.def == a.def;
			if (a.min != b.min || a.max != b.max || !defOk || a.stepped != b.stepped || a.unit != b.unit
			    || a.names.data() != b.names.data() || a.names.size() != b.names.size()
			    || a.modulatable != b.modulatable || a.offsetsMayLeaveRange != b.offsetsMayLeaveRange) {
				return false;
			}
		}
	}
	return kParamDescs[static_cast<size_t>(Param::Partial1Active)].def == 1.0
	       && kHarmonics[static_cast<size_t>(kParamDescs[static_cast<size_t>(partialParam(1, PartialField::Harmonic))].def)].ratio == 2.0f
	       && kHarmonics[static_cast<size_t>(kParamDescs[static_cast<size_t>(partialParam(2, PartialField::Harmonic))].def)].ratio == 3.0f
	       && kHarmonics[static_cast<size_t>(kParamDescs[static_cast<size_t>(partialParam(3, PartialField::Harmonic))].def)].ratio == 4.0f;
}
static_assert(partialsAgree(), "the four partials' descriptors differ in more than key, name, ratio and Active");


/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "pitch", "fx" };

inline constexpr device::Traits kTraits {
    .tag = "HARM",
    .name = "Harm",
    .hasCard = true,
    // Ten controls across two halves that need telling apart — the notch and the oscillator — and a
    // harmonic knob whose 102 ratios want more than a number. Whether that fits one card is a layout
    // question for when the card is built, not a promise to make here.
    // FLIPPED 2026-08-24, WITH ITS READER IN THE SAME CHANGE. It was asked for the dual view — the
    // controls and a filter-style visualizer in an expanded panel, four controls on the card — and
    // `ui::hasExpandedView` is what finally consults this. The flag was declared and read by nothing
    // for a fortnight (backlog 16); a fifth inert flag was the one outcome this work must not add.
    .hasExpanded = true,

    // ONE HARM PER VOICE, AND THE DECLARATION COMES AFTER THE ROAD RATHER THAN BEFORE IT. This was -1
    // until the per-voice runner existed, which was the device correctly saying it had not implemented
    // one — and the card read that declaration and drew its Poly switch DEAD, captioned with why.
    // Flipping this is what brings the switch alive, and it must not be flipped a moment earlier: the
    // host's answer to "poly is on" is to stop running the device on the bus, so a device that
    // declares a road it does not have goes SILENT rather than wrong.
    //
    // WHY PER-VOICE IS THE RIGHT ARRANGEMENT FOR THIS DEVICE, which was a real question rather than a
    // formality. HARM's job is *strip the fundamental*, and ON THE SUMMED BUS IT STRIPS EXACTLY ONE —
    // the lowest sounding note's — so on a triad the third and the fifth keep theirs. Per-voice is the
    // arrangement in which the device does the thing it is named for; the bus arrangement is a bass
    // reinforcement that happens to share the code, and both are worth having.
    //
    // AND THE COST OBJECTION DID NOT SURVIVE THE INSTRUMENT. Sixteen of these MEASURED at 1.98% of one
    // core with both halves running and 0.68% with the oscillator silent, which is the notch-only case
    // a player wanting per-voice stripping would actually run. HARM is a biquad and a sine; 176 bytes
    // of state against the filter's 1440.
    .perVoiceControl = static_cast<int>(Param::Poly),

    /// THE SQUARE IN THE SHARED HEAD, named by index rather than assumed — every fx device has
    /// exactly one enable and no modulator has any, so the head draws it conditionally and the
    /// condition comes from here. See device.h.
    .enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// HARM OFFERS PRE AND POST : its notch and brickwall high-pass PROCESS the passing signal, so a PRE chain
	// feeding them is meaningful (backlog NR D5 had classed it a generator, POST only; Noise, whose input passes untouched, stays so).
	// THE SLOT BLENDS THESE (backlog OW): a plain crossfade around the device, which hands back the wet.
	// HARM's notch and brickwall are IIR with no lookahead, so it declares no latency and the slot's dry needs none.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
	.siteLanes = device::Traits::SiteLanes::PreAndPost,
};

/// MONO IS EXACTLY ONE OSCILLATOR, which is stated as a requirement — in mono mode there is only
/// one oscillator — and which was already true rather than a defect: the bus FxRunner is prepared
/// with the default `instances = 1`, and a Harm holds one phase accumulator. Measured before
/// answering, and pinned by a case rather than left to the structure.
///
/// N VOICES MEANS N SUB-OSCILLATORS, so a six-note chord is roughly six times the sub level of the
/// bus arrangement. FLAGGED RATHER THAN FIXED, and it is the ear that owns it.
///
/// The arithmetic is the one the main oscillators already have — six notes is six oscillators — so
/// this is not a new surprise so much as the same one arriving in a place a player might not expect
/// it, because the bus arrangement they just switched away from summed to one.
///
/// THE OBVIOUS FIX IS WORSE. Scaling the sub by the number of sounding voices would make a level that
/// MOVES AS KEYS GO DOWN: hold a chord and the sub of the first note quietens as the others arrive,
/// which is a compressor nobody asked for and cannot switch off. A loud but stable level is the
/// lesser surprise, and Level is right there. Revisit by ear, not by argument.

/// WHAT IT PUBLISHES TO THE MODULATION BUS: NOTHING.
///
/// A declaration rather than an omission, and the same answer the filter gives. HARM makes sound; it
/// does not make control signals. An envelope follower on its own oscillator would be a real second
/// output and is not being invented here.
/// THE INDEX OF A RATIO IN `kHarmonics`, for the extra partials' defaults — found by value, so nobody counts rows by hand.
constexpr size_t harmonicIndexOf(int num, int den) {
	for (size_t i = 0; i < kHarmonicCount; ++i) {
		if (kHarmonics[i].num * den == kHarmonics[i].den * num) {
			return i;
		}
	}
	return kUnisonHarmonic;
}

/// **PARTIALS 2 TO 4, as the DSP takes them** — the first partial's own fields, without the per-render note (which is the
/// whole device's). The first partial's live in `Settings` itself, under the names the device has always had, so every
/// existing reader of them is unmoved; `partialSlice` makes any partial look like that one.
struct PartialSettings {
	bool active { false };
	size_t harmonic { kUnisonHarmonic };
	float level { 1.0f };
	float fineSemitones { 0.0f };
	float phase { 0.0f };
	float spread { 0.0f };
	float width { 0.3f };
	float depth { 1.0f };
	bool highpass { false };

	friend bool operator==(const PartialSettings&, const PartialSettings&) = default;
};

constexpr std::array<PartialSettings, 3> defaultExtraPartials() {
	std::array<PartialSettings, 3> out {};
	out[0].harmonic = harmonicIndexOf(2, 1);
	out[1].harmonic = harmonicIndexOf(3, 1);
	out[2].harmonic = harmonicIndexOf(4, 1);
	return out;
}

/// WHAT THE DSP TAKES. Every control that feeds the sample loop, in its own units — times in
/// SECONDS here rather than in the log2 the knob declares, because the loop should not carry a taper.
struct Settings {
	/// WHETHER THE DEVICE IS IN THE SIGNAL — in Settings and not beside `poly` in State, because the
	/// SAMPLE LOOP READS IT: the bypass is the DSP's, exactly as fx::filter::Settings::enabled is.
	/// `poly` stays in State because the loop never reads it — it decides how many loops there are.
	bool enabled { true };
	size_t harmonic { kUnisonHarmonic };
	/// THE DEFAULTS, 2026-08-24 — see `kParamDescs`, which is where the reasoning lives.
	///
	/// THESE MUST AGREE WITH `kParamDescs[].def` AND NOTHING MAKES THEM. Two places declare one
	/// fact: `stateOf` uses the descriptor's `def` for a field a patch omits, and this struct's own
	/// initialisers are what a caller gets from `Settings {}`. A static_assert below is what stops
	/// them drifting — the first draft of this change moved one and not the other, and five cases
	/// went red saying `1 == Approx(0)` before anything noticed the two numbers were separate.
	float level { 1.0f };
	float fineSemitones { 0.0f };
	float phase { 0.0f };
	float spread { 0.0f };
	float attackSeconds { 0.0f };
	float releaseSeconds { 1.0f }; ///< the firmware's 64/127 is 1.0079 s — see the descriptor row for the rounding
	/// 0 means the notch is off entirely; above that, 0..1 runs surgical to wide.
	float width { 0.3f };
	/// How much of the centre the notch removes: 1 all of it, 0 none. See Param::Depth.
	float depth { 1.0f };
	float portaSeconds { 0.0f };
	/// SCALE THE NOTCH'S DEPTH BY THE NOTE ENVELOPE — see `Param::DepthFollowsEnv`. The sample loop reads it,
	/// so it is a Setting, unlike `poly`.
	bool depthFollowsEnv { false };
	/// THE BRICKWALL INSTEAD OF THE NOTCH — see `Param::Mode`. The sample loop reads it.
	bool highpass { false };
	/// THE FIRST PARTIAL'S SWITCH (`Param::Partial1Active`). The fields above are the first partial's; partials 2 to 4 are `more`.
	bool active { true };
	/// THE ENVELOPE'S DELAY, HOLD, DECAY AND SUSTAIN, beside `attackSeconds` and `releaseSeconds`. Zero seconds is a stage that
	/// does not happen, and a full sustain leaves nothing for the decay to do, so the defaults are the attack/release envelope.
	float delaySeconds { 0.0f };
	float holdSeconds { 0.0f };
	float decaySeconds { 0.0f };
	float sustain { 1.0f };
	/// PARTIALS 2, 3 AND 4, in that order.
	std::array<PartialSettings, 3> more { defaultExtraPartials() };

	/// THE NOTE THE DEVICE IS TRACKING, and NOT patch state — supplied per render by whoever runs
	/// HARM, exactly as fx::filter::Settings::keyOffsetSemitones is.
	///
	/// That precedent is what makes HARM a chain device at all. Its firmware doc says "sound-only
	/// (needs voice/note info)", which reads as "cannot be an effect" until you notice the filter
	/// already receives note-derived context through its own settings.
	float noteSemitones { 60.0f };
	/// Pitch bend, folded in with fine tune. Also per-render.
	float bendSemitones { 0.0f };
	/// Whether a note is being held. Derived by the host FROM THE VOICES' OWN STAGES rather than
	/// counted here — the rack learned that lesson and it is what makes the sustain pedal, mono,
	/// legato and all-notes-off answer once instead of five times.
	bool gate { false };

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// WHAT A PATCH STORES — every declared control, which is not the same as what the DSP takes.
///
/// `poly` decides how many HARMs there are and where they sit; the sample loop never reads it, in
/// exactly the way fx::filter's keytrack and poly are absent from its Settings. Same distinction,
/// second device — which is what says it was a distinction rather than a special case.
/// THE TWO DECLARATIONS OF ONE DEFAULT, TIED TOGETHER.
///
/// `Settings`'s member initialisers and `kParamDescs[].def` are separate statements of the same
/// number: the first is what `Settings {}` gives a caller, the second is what `stateOf` writes for a
/// field a patch omits. NOTHING MADE THEM AGREE until this, and moving one without the other is a
/// device whose fresh state depends on which road it arrived by.
///
/// FOUND BY DOING EXACTLY THAT. The new defaults went into `kParamDescs` and five cases failed
/// reading `1 == Approx(0)` before it was clear the numbers were declared twice.
static_assert(Settings {}.level
              == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Level)].def));
static_assert(Settings {}.width
              == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Width)].def));
static_assert(Settings {}.depth
              == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Depth)].def));
static_assert(Settings {}.harmonic
              == static_cast<size_t>(kParamDescs[static_cast<size_t>(Param::Harmonic)].def));
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));
static_assert(Settings {}.depthFollowsEnv
              == (kParamDescs[static_cast<size_t>(Param::DepthFollowsEnv)].def >= 0.5));
static_assert(Settings {}.highpass == (kParamDescs[static_cast<size_t>(Param::Mode)].def >= 0.5));

static_assert(Settings {}.active == (kParamDescs[static_cast<size_t>(Param::Partial1Active)].def >= 0.5));
static_assert(Settings {}.sustain == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Sustain)].def));
static_assert(Settings {}.more[0].harmonic
              == static_cast<size_t>(kParamDescs[static_cast<size_t>(partialParam(1, PartialField::Harmonic))].def));
static_assert(Settings {}.more[1].harmonic
              == static_cast<size_t>(kParamDescs[static_cast<size_t>(partialParam(2, PartialField::Harmonic))].def));
static_assert(Settings {}.more[2].harmonic
              == static_cast<size_t>(kParamDescs[static_cast<size_t>(partialParam(3, PartialField::Harmonic))].def));
static_assert(Settings {}.more[0].width
              == static_cast<float>(kParamDescs[static_cast<size_t>(partialParam(1, PartialField::Width))].def));

/// **ANY PARTIAL, SEEN AS THE FIRST.** Returns `settings` with partial `k`'s controls in the fields the first partial's
/// occupy (and the note, bend and gate untouched), so every function here that reads `Settings` — the oscillator's pitch,
/// the notch's centre, width and depth, its profile on the ratio axis — answers for partial `k` unchanged. A render and a
/// picture then ask the same question the same way.
inline Settings partialSlice(const Settings& settings, int k) {
	if (k <= 0) {
		return settings;
	}
	Settings out = settings;
	const PartialSettings& p = settings.more[static_cast<size_t>(std::min(k, kPartialCount - 1) - 1)];
	out.active = p.active;
	out.harmonic = p.harmonic;
	out.level = p.level;
	out.fineSemitones = p.fineSemitones;
	out.phase = p.phase;
	out.spread = p.spread;
	out.width = p.width;
	out.depth = p.depth;
	out.highpass = p.highpass;
	return out;
}

struct State {
	Settings settings {};
	bool poly { false };

	friend bool operator==(const State&, const State&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Row rowOf(const State& state);
State stateOf(const Row& row, size_t fieldsPresent);
float valueOf(const State& state, Param param);
void setValueOf(State* state, Param param, float value);

/// THE NOTCH'S CENTRE, IN HERTZ — derived, never a control.
///
/// note + fine + bend gives a pitch; the harmonic ratio multiplies it — at any Level, including zero
/// (the firmware's notch never reads level; a fallback to 1:1 at Level 0 was ours and is gone, HARM
/// parity sweep 2026-09-16). THAT COUPLING IS THE DEVICE: the notch follows what the oscillator is
/// doing, which is the thing a hand-assembled rack could not express and the reason HARM was made
/// one device.
double notchCentreHz(const Settings& s);

/// THE SUB-OSCILLATOR'S FREQUENCY, in hertz — the same pitch the notch is centred on, which is why
/// they are one function apart rather than two derivations that could drift.
double oscillatorHz(const Settings& s);

/// THE BIQUAD'S Q, from the width control. Inverted, because the firmware's knob is: width 0..1 runs
/// Q from 30 (surgical) down to 0.5 (wide), which is `30 - width * 29.5`.
double notchQ(const Settings& s);

/// THE DEPTH THE BIQUAD USES, clamped — 0..1, where 1 is a true notch and 0 is a wire. The
/// attenuation at the centre is exactly `1 - depth` in linear terms; see Param::Depth.
double notchDepth(const Settings& s);

//------------------------------------------------------------------------------------------------//
// THE HARMONIC AXIS — the device seen in RATIO rather than in hertz.                               //
//------------------------------------------------------------------------------------------------//
//
// WHY THIS EXISTS AND WHY IT IS NOT A COMPROMISE. `harm_card.h` recorded the objection to drawing
// this device at all: *"the notch's frequency depends on the NOTE BEING PLAYED, and a card has no
// note. Drawing it against a pretend middle C would be a curve that is wrong for every other note …
// What the card shows instead is the RATIO, which is the part that is actually a setting: '3:2' is
// true at every pitch."*
//
// THAT LAST SENTENCE IS THE ANSWER, and it was written before anybody asked the question. The notch
// centre is the note TIMES a ratio, so on a ratio axis it sits at the selected harmonic FOR EVERY
// NOTE. A ratio-space picture is not a pretend middle C — it is honest at every pitch, by exactly
// the argument that makes the "3:2" label honest.
//
// NO SAMPLE RATE LIVES ON THIS AXIS, deliberately. `refreshCoefficients` clamps to Nyquist because
// that is where the sample rate is known; a ratio has no sample rate and must not pretend to one.
// What is plotted here is the ANALOG prototype, whose magnitude is a function of ratio/centre and Q
// alone — see notchMagnitudeAtRatio for the measured size of the difference.

/// THE PLAYED NOTE, IN HERTZ — note plus bend, and NOT the oscillator's fine detune.
///
/// THIS IS THE AXIS'S ORIGIN, which is why fine is excluded. The ratio axis answers "how far above
/// the note is this", so the note itself must not move when a control that detunes the OSCILLATOR
/// moves. Fine belongs on the far side of the ratio and shows up there as a marker sitting slightly
/// off its exact harmonic, which is what it audibly does.
double playedNoteHz(const Settings& s);

/// WHERE THE NOTCH SITS ON THE RATIO AXIS.
///
/// DEFINED AS `notchCentreHz(s) / playedNoteHz(s)` LITERALLY, not as a second derivation that agrees
/// with it. The picture and the DSP then cannot drift apart by construction — including through the
/// level<=0 fallback, which this inherits rather than restates.
///
/// IT IS NOT EXACTLY THE SELECTED RATIO, and the difference is the fine detune: at fine 0 it is the
/// ratio exactly, and at fine f semitones it is `ratio * 2^(f/12)`. The same is true of the level<=0
/// fallback — the notch lands on 1:1 exactly only when fine is zero, because the fallback zeroes the
/// HARMONIC and leaves the detune alone. Both are what the DSP does; a picture that rounded them to
/// the tidy answer would be lying about a control the expanded view exists to show.
double notchRatio(const Settings& s);

/// THE NOTCH'S MAGNITUDE AT A POINT ON THE RATIO AXIS — the analog prototype, linear, 0..1.
///
/// WIDTH ZERO IS A WIRE, which is `refreshCoefficients`' own rule ("width zero is no notch at all,
/// which is the control's declared meaning and not a small-Q approximation of it") and not a
/// separate decision taken here.
///
/// AT THE CENTRE THIS IS EXACTLY `1 - depth`, the same number Param::Depth promises and the biquad
/// measures, reached from the other direction: the numerator carries alpha*(1-depth) and the
/// denominator carries alpha, and at the centre nothing else survives.
double notchMagnitudeAtRatio(const Settings& s, double ratio);

/// THE AXIS'S ENDS, which are the harmonic table's ends and not a range chosen for the picture.
/// 1:8 below the note to 32:1 above it — eight octaves, and every ratio a hand can select is inside
/// them by construction. A picture whose axis was narrower than its control would have positions it
/// could not draw.
inline constexpr double kProfileLoRatio = 0.125;
inline constexpr double kProfileHiRatio = 32.0;

/// `n` LOG-SPACED POINTS ACROSS THE HARMONIC AXIS, written into caller storage. Allocates nothing,
/// so it is safe on any thread — the filter's `gainProfile` contract, deliberately, since a second
/// convention for "give me a curve" is how two pictures in one editor stop matching.
///
/// `ratioOut` may be null when the caller places its points by index, which is what a log-spaced
/// curve of fixed width does.
void ratioProfile(const Settings& s, float* ratioOut, float* gainOut, size_t n);

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in harm.cpp, where the DSP class is.
///
/// DECLARED HERE AND DEFINED THERE so this header stays free of the DSP, and declared here rather
/// than in harm.h because kDescriptor below must point at it and harm.h includes this file.
extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
    .traits = kTraits,
    .params = kParamDescs,
    // EMPTY, spelled as `{}` rather than as a zero-length array — a zero-length array does not
    // convert to a span, which the filter already discovered.
    .outputs = {},
    // AND THE ENTRY POINT. Until 2026-08-23 this was absent and HARM was correctly REFUSED a seat in
    // the fx lane: it had controls, a harmonic table and its notch maths, and no way to render. The
    // null was the honest answer then and pointing at a real table is the honest answer now.
    .audio = &kAudioOps,
    .category = device::Category::Utility,
};

} // namespace feathers::fx::harm
