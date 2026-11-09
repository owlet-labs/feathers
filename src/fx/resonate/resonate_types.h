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

#pragma once

#include "device/device.h"
#include "device/registry.h"

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <type_traits>

// RESONATE — the scanned-string effect: one ring of masses the input bows and plucks, scanned at held
// notes for a pitched tone, and used as an FIR kernel on the input for a filtered one, blended.
//
// A port of the Weave synth engine, made into a plugin effect as well.
//
// **THE EFFECT IS "RESONATE"; THE PHYSICS KEEPS THE FIRMWARE'S NAME**. The
// string it runs is `WeaveString` with `PhiWeaveParams` and `WeaveModel` — the phi-weave model's own
// names — and only the device, its tag and its keys say Resonate.
//
// **THE PHYSICS RUNS AT HAPTIC RATE AND PITCH IS THE SCAN RATE.** The string moves a few hundred times a
// second; what a player hears as pitch is how fast the heads read round it. So the Model and every law
// control change how the scanned WAVEFORM MOVES, never which note sounds — the tooltips say so, and none
// of them claims the device imitates an instrument.
//
// **THIS HEADER IS THE DESCRIPTOR; THE DSP IS `resonate_dsp.h`**, driven through the ops in `resonate.cpp`.

namespace feathers::fx::resonate {

/// THE PLUGIN'S ID BLOCK IS ASSIGNED IN THIS ORDER, so a device row is index-aligned with this enum.
/// A PATCH STORES EACH BY ITS KEY (`kParamDescs[i].key`), so reordering this is free for saved state.
enum class Param : int {
	Enabled,
	/// WHICH PHYSICS MOVES THE STRING — a value box with a menu, so all five are visible before one is
	/// applied (the multivalue-toggle rule).
	Model,
	/// WHERE ON THE LAWS' PHASE THE STRING SITS, 0..1 — the synth's zone, as one continuous control.
	Character,
	/// THE A↔B CROSSFADE TOWARD `Character` plus a fixed φ offset. Its MOTION bows the string.
	Morph,
	/// THE ZONE'S OWN BOW, under the input. 0: the string moves only when the input plays it.
	Bow,
	/// HOW HARD THE INPUT'S LEVEL BOWS the string.
	Drive,
	/// PLUCK (v13; Pluck Sens until then): how hard a note-on plucks the ring (v19: the input no longer does; v20: the Cycle's is
	/// Drone Pluck). 0 is none.
	Pluck,
	/// POLY OR MONO heads on the one shared string.
	Voicing,
	/// log2(s) of the Mono glide. MONO ONLY (gated).
	Porta,
	/// OFFSET APPLIED TO EVERY HEAD: octaves, semitones, cents.
	Octave,
	Semitone,
	Fine,
	/// THE SYMPATHETIC DRONE'S OUTPUT LEVEL (drone should have its own output… output level knob"): a
	/// fader like Level, unity at the centre, on the drone alone — added to the wet AFTER Level and Blend, which no
	/// longer touch it. The drone is a head of its own at `Root`, sounded by an envelope follower on the INPUT, so
	/// the input plays it as a sympathetic string is played by the strings around it. 0 is silent.
	DroneOut,
	/// HOW THE INPUT REACHES EACH STRING: from TUNED (0: through the string's own harmonic ear, so a note lights up
	/// the strings it matches) to INTEGRATED (1: the whole input's envelope, the same for every string).
	DroneExcite,
	/// HOW FAST THE DRONE SWELLS as the input arrives: the follower's attack TIME CONSTANT.
	DroneAttack,
	/// HOW LONG IT RINGS ON after the input stops: the follower's decay as a T60 (the time to fall 60 dB).
	DroneDecay,
	/// THE DRONE'S STRINGS, A CHOICE (the dropdown stays the chord): which chord above Root they
	/// are tuned to (`kDroneChords`), stored by its label.
	DroneChord,
	/// THE DRONE'S CHARACTER (should be called character and should be a standard infinite zone knob):
	/// the house zoned coordinate — one unbounded number, eight zones a lap — driving every drone string's φ landscape
	/// (its ear, times, level and brightness) and the gourd body's peaks. It never changes the chord.
	DroneCharacter,
	/// HOW WIDE THE DRONE'S STRINGS SIT: 0 is mono; toward 1 each string moves to its own φ-placed side.
	DroneWidth,
	/// THE DRONE'S NOTE.
	Root,
	/// THE FILTER PATH'S KERNEL LENGTH, in taps — continuous and modulatable (2026-09-26).
	Size,
	/// HOW FAR ROUND THE RING THE RIGHT CHANNEL'S PICKUP SITS — both paths.
	Width,
	/// TONE (0) TO FILTER (1).
	Blend,
	/// A FADER ON THE WET, unity at the centre.
	Level,
	/// DRY/WET, COMP's form: `(1 - m) x + m wet`.
	Mix,
	/// HOW MUCH THE NOTE HEADS RING WITHOUT MOTION, 0..1 — appended. 0: they sound only while
	/// the input or Bow moves the string; 1: steadily whenever a note is held. NOT the drone, whose level is
	/// its follower's alone (2026-09-26).
	Sustain,
	/// THE JAWARI BRIDGE (v9 2026-09-28): how close the bridge sits, 0..1 against each string's own swing —
	/// 0 is no bridge. The ring meets a LINE along its length (WeaveString::Bridge), and so do the notes' and (v11) the
	/// drone's node strings (NodeString).
	Jawari,
	/// THE BRIDGE'S SHAPE, 0..1: on the ring, a straight line (0) to one that curves away everywhere but under the bow (1);
	/// on the notes' and drone's node strings (v11), from its middle up, a line lifted from all but the bridge end.
	BridgeCurve,
	/// THE BRIDGE AS A SHARED NODE : how much of the drone strings' own vibration each string's ear
	/// hears beside the input, so strings whose harmonics line up ring each other. 0: the input alone.
	Sympathy,
	/// THE DRONE'S VOICE (2026-09-27): from its own strings (0; node strings since v11) to the ring-scanned drone (1).
	DroneVoice,
	/// THE COMB STRINGS' STIFFNESS: 0 harmonic; up, the partials stretch sharp as a thick string's do.
	DroneStiffness,
	/// THE GOURD BODY'S AMOUNT, 0 (dry) .. 1; its peaks are the Character's.
	DroneBody,
	/// THE TWIN GOURDS (a veena's two tumba), 0 .. 1: two low resonances, one each side.
	DroneTwin,
	/// THE SELF-PLAYING DRONE'S PATTERN, a zoned coordinate: its zone the order the strings are plucked in, its
	/// position each step's strength.
	DroneCycle,
	/// HOW OFTEN IT PLUCKS: Off, or a note division on the host's grid.
	DroneCycleSync,
	/// WHAT A NOTE-OFF DOES TO ITS HEAD: Gate releases it; Ring lets it sound until the string has rung down.
	HeadRelease,
	/// THE DRONE'S TUNING: Equal (12-TET) or Just (5-limit ratios).
	DroneTuning,
	/// HOW LONG THE RING RINGS (some way to control how long the played note rings): log2 of a scaler on
	/// the ring's damping, the synth's Explore Decay law (damping / 2^value, clamped); up is longer. 0 is the laws as they are.
	RingDecay,
	/// HOW HARD THE CYCLE PLUCKS : a fader on the pluck's strength, unity at the centre, where a pluck
	/// sounds the drone as loud as a played note does. 0 is no plucks.
	DroneCycleLevel,
	/// HOW HARD THE STRINGS BOUNCE OFF THE BRIDGE : the contact's restitution. 1 an elastic hit that
	/// throws the string back and moves its energy into the upper partials; lower a softer hit that also soaks it up.
	Bounce,
	/// HOW TAUT THE RING IS (most of the string vibrating settings vibrate too fast): log2 of a scaler on the
	/// frequency of the ring's own motion — its springs and couplings scaled by its square, so every mode moves by it and
	/// the rest pose does not. 0 is the laws as they are.
	Tension,
	/// HOW HARD THE INPUT PLAYS THE DRONE ("the played notes always blow out the drone ... if level was
	/// pre excitation, this would probably fix it"): a fader on the input the drone's strings hear, before their ears and
	/// follower. Not on the Cycle's plucks, nor on what the strings hear of each other on the bridge.
	DroneInput,
	/// THE NOTES' OWN STRINGS (v10, node strings per voice): how much of each head is its own
	/// finite-difference string (resonate_node_string.h) rather than the ring's scanned voice, 0..1. 0 is v9's tone, bit for bit.
	StringBlend,
	/// WHAT PLAYS THEM: 0 the head's scanned voice, 1 the dry input, 0..1.
	StringExcite,
	/// THE CYCLE ONLY WHILE THE TRANSPORT PLAYS ("add a button to play the pluck arp on transport on
	/// only"): Off (it also runs free when stopped, v7-v10) or On.
	DroneCycleTransport,
	/// WHERE THE STRINGS ARE HEARD (v11, is there a way to get 'more'): 0 the v10 pickup at 0.3 of the length, 1 by
	/// the bridge end at 0.1, held to the same fundamental level — brighter, the bite past Bounce's elastic top.
	Pickup,
	/// SCALED TO FIXED (v13): where the strings' bridge line stands — 0 a fraction of each string's own swing (a harder
	/// pluck buzzes alike, only louder), 1 a fixed height (a harder pluck presses further into the bridge). NodeString::setFixed.
	BridgeFixed,
	/// HOW HARD THE CYCLE PLUCKS THE MAIN STRING (v20): the Cycle's jolt to the ring, on Pluck's law, apart from Pluck, which
	/// is now the played notes' alone.
	DronePluck,
	/// THE DRONE STRINGS' OWN JAWARI (v20): their point's gap, on Jawari's law (NodeString::gapOf), apart from Jawari, which
	/// is now the notes' strings' and the ring's alone.
	DroneJawari,
	/// HOW HARD THE DRONE STRINGS COME OFF THEIR BRIDGE (v21): their contact's restitution, on Bounce's law, apart from
	/// Bounce, which is now the notes' strings' and the ring's alone.
	DroneBounce,
	/// HOW FREELY THE BRIDGE MOVES UNDER THE DRONE STRINGS (v21, a new sympathy control for drones only): the mass the drone
	/// strings see on the shared bridge, on Sympathy's law, apart from Sympathy, which is now the notes' strings'.
	DroneSympathy,
	/// WHERE THE DRONE FX LANE GOES (can't decide if drone fx should go through wet fx also): On, the
	/// drone (after DRONE FX) joins the tone BEFORE the WET FX lane, so WET FX is a bus over everything resonated; Off, it
	/// joins AFTER, so the two lanes run in parallel. With WET FX empty the two are the same sound, bit for bit.
	DroneToWetFx,
	/// WHERE THE DRONE STRINGS FIRST MEET THEIR JAWARI (v21, Requirement: the THREAD, as a tanpura's): the point's place along the length
	/// from the bridge end (NodeString::setPointPos), 3% by default. The notes' point stays at 3%.
	DroneThread,
	/// WHETHER THE DRONE'S SCANNED VOICES FOLLOW THE MAIN STRING (v21, a chip to totally disable the main string env into the
	/// drones): On, a scanned voice's gain is the larger of its string's own level and the main string's motion (v14); Off, its
	/// string's own level alone, so played notes moving the main string no longer swell the drone.
	DroneFollow,
	/// HOW MANY NODES A DRONE STRING MAY HAVE (backlog PS: can we make it a switch at least so i can ab): Full is
	/// NodeString::kMaxNodes (128), today's string to the bit; 96, 64 and 48 cap it, which cuts the drone's cost (its strings were ~45%
	/// of the device) at the price of the strings' top modes. Not modulatable: a change regrids the ringing strings (NodeString::regrid).
	DroneDetail,

	kCount,
};

/// **INDEX-ALIGNED WITH THE STRING'S OWN MODEL ENUM** (the plan's Task 2b, `WeaveString`), which the DSP
/// asserts where it can see both. A Choice is stored by its LABEL, so a renamed entry needs a
/// `ChoiceAlias` and a reordered one is free.
enum class Model : int { Ring, Bar, Tube, Membrane, Stiffening, kCount };
inline constexpr const char* kModelNames[] = { "Ring", "Bar", "Tube", "Membrane", "Stiffening" };
static_assert(std::size(kModelNames) == static_cast<size_t>(Model::kCount));

enum class Voicing : int { Poly, Mono, kCount };
inline constexpr const char* kVoicingNames[] = { "Poly", "Mono" };
static_assert(std::size(kVoicingNames) == static_cast<size_t>(Voicing::kCount));

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// THE GLIDE RANGE — HARM's numbers, named here so the descriptor and `stateOf` read the same ones. The
/// minimum means "no time at all" (`Unit::LogSeconds`' convention): Mono snaps.
inline constexpr double kPortaLog2Min = -11.0;              // 0.49 ms
inline constexpr double kPortaLog2Max = 3.3219280948873626; // 10 s

/// THE DRONE FOLLOWER'S TIMES — HARM's attack/release range (log2 seconds, 0.49 ms .. 10 s). The attack is a
/// time constant; the decay is a T60, the follower's own time constant being T60 / ln(1000) (~T60 / 6.91).
/// DEFAULTS ARE BY-EAR FIRST CUTS, chosen as exact powers of two so the declared default is representable:
/// attack 2^-4 = 62.5 ms, decay T60 2^1 = 2 s.
inline constexpr double kDroneTimeLog2Min = -11.0;              // 0.49 ms
inline constexpr double kDroneTimeLog2Max = 3.3219280948873626; // 10 s
/// DRONE DECAY REACHES 20 s (2026-09-28: a tanpura rings for many seconds) and starts at 12 s: at 8 s the Tanpura's
/// shorter-scaled strings fell 40 dB in 5.7 s (MEASURED), under the 6 s a plucked string should still be heard.
inline constexpr double kDroneDecayLog2Max = 5.906890595608519; // 60 s (v21, increase the range on attack to 60s but log control (probably decay as well); 20 s until v21)
/// DRONE ATTACK REACHES 60 s TOO (v21; 10 s, kDroneTimeLog2Max, until then). The stored value is absolute log2 seconds, so
/// only the bound moved: every value a patch holds means what it did.
inline constexpr double kDroneAttackLog2Max = 5.906890595608519; // 60 s
/// THE EVERY-MAX CASES' DECAY (test_fx_resonate): 20 s, v20's top, so their ring-down stays inside a test's time; the 60 s top
/// has its own exact-zero case.
inline constexpr double kDroneDecayLog2V20Max = 4.321928094887363; // 20 s
/// THE DRONE THREAD (v21): the drone strings' point, a fraction of the length (NodeString::kPointPos by default); its travel is
/// NodeString's kPointPosMin..kPointPosMax.
inline constexpr double kDroneThreadDefault = 0.03;
inline constexpr double kDroneThreadMin = 0.01;
inline constexpr double kDroneThreadMax = 0.08;
inline constexpr double kDroneAttackLog2Default = -4.0;
inline constexpr double kDroneDecayLog2Default = 3.584962500721156; // 12 s
/// ln(1000): a one-pole falls 60 dB in this many of its time constants.
inline constexpr double kT60PerTau = 6.907755278982137;

/// THE DRONE'S CHORDS — the `DroneChord` Choice, as intervals in semitones above Root, lowest first, at most
/// kMaxDroneStrings. Each is a string of its own: a drone head at its pitch, played by the input through a
/// harmonic ear at that pitch (resonate_ear.h), so a note you play lights up the strings it matches (a sympathetic string's tuned
/// response). A Choice is stored by its LABEL, so reordering or appending is free; renaming needs a ChoiceAlias.
///
/// **SITAR** (Sa, Pa, Sa', Re', Ga', Pa' — 0, 7, 12, 14, 16, 19, about an octave and a half): a sitar's chikari
/// strings frame the drone on Sa and Pa, and its taraf (sympathetic) strings are tuned to the raga's notes above
/// the upper Sa. Re and Ga are the ones the commonest ragas share (Yaman, Bilawal, Bhupali, Kalyan), so the
/// upper three ring for most melodies without naming one raga; a raga-specific tuning is a later chord.
///
/// **TANPURA** : its four open strings are Pa, Sa, Sa and low Sa — as intervals above Root (the
/// low Sa) 0, 7, 12, 12; TANPURA MA puts Ma where Pa was (0, 5, 12, 12). The two unison Sa strings are detuned
/// apart by a hair (kDroneUnisonDetuneCents, each way) so they beat as two real strings do rather than summing.
/// **THE RAGAS** (maybe we should add the ragas), after the chords: each is its thaat's or scale's
/// notes above Sa (Root) plus the upper Sa, so a raga's strings ring when a melody in it is played. They are the
/// SCALE only — a raga's vakra (crooked) movements, and notes varjit (omitted) in one direction only, are not
/// modelled: a sympathetic string has no direction. Seven-note ragas are eight strings, hence kMaxDroneStrings.
inline constexpr size_t kMaxDroneStrings = 8;
/// A JUST INTERVAL, as a ratio of whole numbers above Root.
struct Ratio {
	uint16_t num = 1;
	uint16_t den = 1;
};
/// THE 5-LIMIT RATIO OF A SEMITONE COUNT (Indian instruments are tuned close to just intonation):
/// Sa 1/1, komal Re 16/15, Re 9/8, komal Ga 6/5, Ga 5/4, Ma 4/3, tivra Ma 45/32, Pa 3/2, komal Dha 8/5, Dha 5/3,
/// komal Ni 9/5, Ni 15/8, Sa' 2/1 — and an octave up, twice that. What most chords use; a chord whose just interval
/// is another (the harmonic seventh, 7/4; the quartal 16/9) spells its own.
constexpr Ratio justOf(int semis) {
	constexpr Ratio kOctave[12] = { { 1, 1 }, { 16, 15 }, { 9, 8 }, { 6, 5 }, { 5, 4 }, { 4, 3 },
		                            { 45, 32 }, { 3, 2 }, { 8, 5 }, { 5, 3 }, { 9, 5 }, { 15, 8 } };
	Ratio r = kOctave[((semis % 12) + 12) % 12];
	for (int o = semis / 12; o > 0; --o) {
		r.num = static_cast<uint16_t>(r.num * 2);
	}
	return r;
}
struct DroneChord {
	const char* name;
	uint8_t count;
	/// EQUAL: semitones above Root (the φ landscapes are placed by these in either tuning).
	std::array<int8_t, kMaxDroneStrings> intervals;
	/// JUST: each string's ratio above Root.
	std::array<Ratio, kMaxDroneStrings> just;
};
/// A CHORD WHOSE JUST TUNING IS THE 5-LIMIT TABLE'S, string by string.
constexpr DroneChord chordOf(const char* name, std::initializer_list<int> semis) {
	DroneChord c { name, 0, {}, {} };
	for (const int s : semis) {
		c.intervals[c.count] = static_cast<int8_t>(s);
		c.just[c.count] = justOf(s);
		++c.count;
	}
	return c;
}
/// A CHORD WITH ITS OWN JUST RATIOS (the semitones its Equal tuning rounds them to, beside them).
constexpr DroneChord chordOf(const char* name, std::initializer_list<int> semis, std::initializer_list<Ratio> just) {
	DroneChord c = chordOf(name, semis);
	size_t i = 0;
	for (const Ratio r : just) {
		c.just[i++] = r;
	}
	return c;
}
/// THE RAGAS' MENU LABELS carry a Western parenthetical; a patch stores the Choice by label, so each
/// old label is aliased to its new one (kChordAliases). NO SHRUTI OVERRIDES: every raga here is its thaat's or scale's
/// notes in the 5-limit table above; a raga that traditionally lowers a note off that table (Darbari's ati-komal Ga)
/// is not in the list.
/// THE ADDED CHORDS (after the chords, before the ragas): Sus2, Add9, Maj7, Min7, Dom7 — its seventh the HARMONIC 7/4
/// in Just (969 cents), 10 semitones in Equal — Min9, Quartal (4/3, 16/9: stacked just fourths), Dim (its fifth
/// 64/45, the diminished fifth, 610 cents; not 36/25) and Aug (25/16, two just major thirds).
inline constexpr DroneChord kDroneChords[] = {
	chordOf("Unison", { 0 }),
	chordOf("Fifth", { 0, 7 }),
	chordOf("Octave", { 0, 12 }),
	chordOf("Fifth + Octave", { 0, 7, 12 }),
	chordOf("Major", { 0, 4, 7, 12 }),
	chordOf("Minor", { 0, 3, 7, 12 }),
	chordOf("Sus4", { 0, 5, 7, 12 }),
	chordOf("Sitar", { 0, 7, 12, 14, 16, 19 }),
	chordOf("Tanpura", { 0, 7, 12, 12 }),
	chordOf("Tanpura Ma", { 0, 5, 12, 12 }),
	chordOf("Sus2", { 0, 2, 7, 12 }),
	chordOf("Add9", { 0, 4, 7, 14 }),
	chordOf("Maj7", { 0, 4, 7, 11 }),
	chordOf("Min7", { 0, 3, 7, 10 }),
	chordOf("Dom7", { 0, 4, 7, 10 }, { { 1, 1 }, { 5, 4 }, { 3, 2 }, { 7, 4 } }),
	chordOf("Min9", { 0, 3, 7, 10, 14 }),
	chordOf("Quartal", { 0, 5, 10, 12 }, { { 1, 1 }, { 4, 3 }, { 16, 9 }, { 2, 1 } }),
	chordOf("Dim", { 0, 3, 6, 12 }, { { 1, 1 }, { 6, 5 }, { 64, 45 }, { 2, 1 } }),
	chordOf("Aug", { 0, 4, 8, 12 }, { { 1, 1 }, { 5, 4 }, { 25, 16 }, { 2, 1 } }),
	chordOf("Bilawal (Ionian)", { 0, 2, 4, 5, 7, 9, 11, 12 }),
	chordOf("Yaman (Lydian)", { 0, 2, 4, 6, 7, 9, 11, 12 }),
	chordOf("Khamaj (Mixolydian)", { 0, 2, 4, 5, 7, 9, 10, 12 }),
	chordOf("Kafi (Dorian)", { 0, 2, 3, 5, 7, 9, 10, 12 }),
	chordOf("Asavari (Aeolian)", { 0, 2, 3, 5, 7, 8, 10, 12 }),
	chordOf("Bhairavi (Phrygian)", { 0, 1, 3, 5, 7, 8, 10, 12 }),
	chordOf("Bhairav (Double Harmonic)", { 0, 1, 4, 5, 7, 8, 11, 12 }),
	chordOf("Todi (Phrygian \342\231\257" "4 \342\231\256" "7)", { 0, 1, 3, 6, 7, 8, 11, 12 }),
	chordOf("Purvi (Lydian \342\231\255" "2 \342\231\255" "6)", { 0, 1, 4, 6, 7, 8, 11, 12 }),
	chordOf("Marwa (Lydian \342\231\255" "2, no 5th)", { 0, 1, 4, 6, 9, 11, 12 }),
	chordOf("Bhupali (Major Pentatonic)", { 0, 2, 4, 7, 9, 12 }),
	chordOf("Malkauns (Pentatonic, no 5th)", { 0, 3, 5, 8, 10, 12 }),
	chordOf("Durga (Suspended Pentatonic)", { 0, 2, 5, 7, 9, 12 }),
};
/// THE CHOICE'S LABELS, index-aligned with `kDroneChords` (asserted below).
inline constexpr const char* kChordNames[] = {
	"Unison", "Fifth", "Octave", "Fifth + Octave", "Major", "Minor", "Sus4", "Sitar", "Tanpura", "Tanpura Ma",
	"Sus2", "Add9", "Maj7", "Min7", "Dom7", "Min9", "Quartal", "Dim", "Aug",
	"Bilawal (Ionian)", "Yaman (Lydian)", "Khamaj (Mixolydian)", "Kafi (Dorian)", "Asavari (Aeolian)",
	"Bhairavi (Phrygian)", "Bhairav (Double Harmonic)", "Todi (Phrygian \342\231\2574 \342\231\2567)",
	"Purvi (Lydian \342\231\2552 \342\231\2556)", "Marwa (Lydian \342\231\2552, no 5th)",
	"Bhupali (Major Pentatonic)", "Malkauns (Pentatonic, no 5th)", "Durga (Suspended Pentatonic)" };
/// THE RAGAS' OLD LABELS (before their parentheticals), so a saved patch still finds its raga.
inline constexpr device::ChoiceAlias kChordAliases[] = {
	{ "Bilawal", "Bilawal (Ionian)" },
	{ "Yaman", "Yaman (Lydian)" },
	{ "Khamaj", "Khamaj (Mixolydian)" },
	{ "Kafi", "Kafi (Dorian)" },
	{ "Asavari", "Asavari (Aeolian)" },
	{ "Bhairavi", "Bhairavi (Phrygian)" },
	{ "Bhairav", "Bhairav (Double Harmonic)" },
	{ "Todi", "Todi (Phrygian \342\231\2574 \342\231\2567)" },
	{ "Purvi", "Purvi (Lydian \342\231\2552 \342\231\2556)" },
	{ "Marwa", "Marwa (Lydian \342\231\2552, no 5th)" },
	{ "Bhupali", "Bhupali (Major Pentatonic)" },
	{ "Malkauns", "Malkauns (Pentatonic, no 5th)" },
	{ "Durga", "Durga (Suspended Pentatonic)" },
};
/// THE CHORD INDEX OF THE FIRST RAGA (the tests and the bench name ragas by it).
inline constexpr int kFirstRaga = 19;

static_assert(std::size(kChordNames) == std::size(kDroneChords));
inline constexpr bool chordNamesMatch() {
	for (size_t i = 0; i < std::size(kDroneChords); ++i) {
		const char* a = kChordNames[i];
		const char* b = kDroneChords[i].name;
		while (*a != '\0' && *a == *b) {
			++a;
			++b;
		}
		if (*a != *b) {
			return false;
		}
	}
	return true;
}
static_assert(chordNamesMatch(), "kChordNames must be kDroneChords' names, in order");
/// THE TWO UNISON Sa STRINGS OF A TANPURA, each this far off in cents, one flat and one sharp: 3 x 1/φ = 1.854,
/// so the pair sits 3.7 cents apart and beats at about half a hertz on a C3 Sa. By ear a first cut.
inline constexpr double kDroneUnisonDetuneCents = 3.0 * 0.6180339887498949;
/// THE ZONED COORDINATES (Character, Cycle) — the house convention (Eroder's Character, Topo): an unbounded number
/// whose range is a drag window, not a domain; eight zones a lap of 1.0 (ZoneSelect::kCoordinatePerLap); a full-depth
/// route moves one zone.
inline constexpr uint32_t kDroneZones = 8;
inline constexpr double kDroneOneZone = 1.0 / static_cast<double>(kDroneZones);
inline constexpr double kDroneCoordMax = 8388608.0;
/// CHARACTER'S DEFAULT: 0.5, where the old Drone Phase sat and v3's default Chord put every landscape.
inline constexpr double kDroneCharacterDefault = 0.5;
/// THE CYCLE'S PATTERNS, one per zone (resonate_dsp.cpp, cyclePlan). Each is an order over the chord's strings,
/// string 0 being the lowest; the position within the zone gives each step's strength from its φ-triangle.
enum class CyclePattern : int { Tanpura, Up, Down, UpDown, OutsideIn, InsideOut, Alternate, PhiWalk, kCount };
inline constexpr device::ZoneName kCycleZoneNames[] = {
	{ "Tanpura", "Every string above the lowest in order, then the lowest: a tanpura's Pa, Sa, Sa, low Sa" },
	{ "Up", "Lowest to highest, over and over" },
	{ "Down", "Highest to lowest, over and over" },
	{ "Up-Down", "Up and back down, the ends once each" },
	{ "Outside-In", "Lowest, highest, next lowest, next highest, toward the middle" },
	{ "Inside-Out", "From the middle outward, alternating sides" },
	{ "Alternate", "Every other string going up, then the ones between" },
	{ "φ-walk", "A golden-ratio walk: each step the string at the next multiple of φ round the chord" },
};
static_assert(std::size(kCycleZoneNames) == static_cast<size_t>(CyclePattern::kCount));
static_assert(std::size(kCycleZoneNames) == kDroneZones);
/// THE CYCLE'S RATE, TEMPO-SYNCED (the plucks should be quantized): Off, then the house's note
/// divisions (`device::kDivisionNames`) from 1/16 to 4 bars, each straight, dotted and triplet — the automod and delay
/// lists' spellings, read from the same table. Stored by label.
inline constexpr size_t kCycleFirstDivision = 6;  // 1/16
inline constexpr size_t kCycleLastDivision = 26;  // 4 barT
inline constexpr size_t kCycleSyncCount = 1 + kCycleLastDivision - kCycleFirstDivision + 1;
static_assert(std::string_view(device::kDivisionNames[kCycleFirstDivision]) == "1/16");
static_assert(std::string_view(device::kDivisionNames[kCycleLastDivision]) == "4 barT");
constexpr std::array<const char*, kCycleSyncCount> makeCycleSyncNames() {
	std::array<const char*, kCycleSyncCount> out {};
	out[0] = "Off";
	for (size_t i = kCycleFirstDivision; i <= kCycleLastDivision; ++i) {
		out[1 + i - kCycleFirstDivision] = device::kDivisionNames[i];
	}
	return out;
}
inline constexpr auto kCycleSyncNames = makeCycleSyncNames();
/// A CHOICE'S PERIOD IN BEATS, or 0 for Off.
[[nodiscard]] inline double cyclePeriodBeats(int choice) {
	if (choice <= 0 || static_cast<size_t>(choice) >= kCycleSyncCount) {
		return 0.0;
	}
	return device::kDivisionBeats[kCycleFirstDivision + static_cast<size_t>(choice) - 1];
}
/// THE CHOICE A HAND FINDS FIRST ON TURNING IT ON: one bar, a tanpura's slow round at most tempos.
inline constexpr int kCycleOneBar = 1 + 18 - static_cast<int>(kCycleFirstDivision);
static_assert(device::kDivisionBeats[18] == 4.0);

inline constexpr const char* kHeadReleaseNames[] = { "Gate", "Ring" };
enum class HeadRelease : int { Gate, Ring, kCount };
inline constexpr const char* kTuningNames[] = { "Equal", "Just" };
/// DRONE DETAIL'S CHOICES, stored by label, and the node cap each names (Full: NodeString::kMaxNodes).
inline constexpr const char* kDroneDetailNames[] = { "Full", "96", "64", "48" };
inline constexpr int kDroneDetailNodes[] = { 128, 96, 64, 48 };
static_assert(std::size(kDroneDetailNames) == std::size(kDroneDetailNodes));
enum class Tuning : int { Equal, Just, kCount };
/// THE DRONE VOICE'S DEFAULT: mostly the strings, a fifth of the ring-scanned drone under them.
inline constexpr double kDroneVoiceDefault = 0.2;
/// SYMPATHY'S DEFAULT (v20): where the bridge's log law (ResonateDsp::bridgeMass, mass 843 at 0+ to 63 at the top) puts mass 150,
/// v18's top: log(843 / 150) / log(843 / 63).
inline constexpr double kSympathyDefault = 0.6655525530;
/// THE NOTES' SYMPATHY DEFAULT (v21): mass 150 again on the notes' longer law (ResonateDsp::bridgeMassNotes, 843 to 35):
/// log(843 / 150) / log(843 / 35).
inline constexpr double kSympathyNotesDefault = 0.5425953642;
/// THE BODY'S DEFAULT: half — a gourd is part of the instrument, and at half it boosts at most +4.6 dB (MEASURED, v11).
inline constexpr double kDroneBodyDefault = 0.5;
/// BOUNCE'S DEFAULT: elastic — the string is thrown back whole (NodeString::contact).
inline constexpr double kBounceDefault = 1.0;
/// TENSION'S DEFAULT: about a third of the laws' own speed (2^-1.5) — a slow wander of a few hertz where the laws rang at
/// 2..20 Hz (MEASURED 2026-09-28, a plucked ring, Bow 0: Ring 8.7, Bar 2.2, Tube 7.3, Membrane 20.6, Stiffening 7.5 Hz).
/// THIS CHANGES THE DEFAULT SOUND: the string moves about a third as fast as it did.
inline constexpr double kTensionDefault = -1.5;
/// THE NOTES' STRINGS BY DEFAULT: half of each note is its string, played half by the ring's voice and half by the input —
/// audible from the first note. THIS CHANGES THE DEFAULT SOUND (v9 is String 0).
inline constexpr double kStringBlendDefault = 0.5;
inline constexpr double kStringExciteDefault = 0.5;

/// DRONE WIDTH'S DEFAULT: half, so the strings spread out of the box; 0 is mono.
inline constexpr double kDroneWidthDefault = 0.5;
/// DRONE EXCITE'S DEFAULT — set by measurement (v3, through the follower's makeup that v12 moved onto the feed:
/// resonate_dsp.h, kNodeMakeup).
inline constexpr double kDroneExciteDefault = 0.3;

/// SIZE, IN TAPS: the shortest and longest kernel the filter runs.
inline constexpr double kSizeMinTaps = 32.0;
inline constexpr double kSizeMaxTaps = 256.0;

/// THE FADER'S CENTRE IS UNITY: gain `(2v)^2` (`device::Unit::Fader`).
inline constexpr double kFaderUnity = 0.5;

/// HEADS ON THE STRING IN POLY — the design's eight, oldest stolen. Named for Task 7's pool.
inline constexpr size_t kMaxHeads = 8;

struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — every effect defaults to on when it is added.
	/// Pinned to the descriptor below.
	bool enabled = true;
	int model = static_cast<int>(Model::Ring);
	float character = 0.5f;
	float morph = 0.0f;
	float bow = 0.25f;
	float drive = 0.5f;
	float pluck = 0.5f;
	int voicing = static_cast<int>(Voicing::Poly);
	/// log2(s), as the control stores it.
	float porta = static_cast<float>(kPortaLog2Min);
	int octave = 0;
	int semitone = 0;
	/// CENTS.
	float fine = 0.0f;
	/// A FADER POSITION (unity at kFaderUnity), so the effect sounds out of the box on an audio track with no MIDI.
	float droneOut = static_cast<float>(kFaderUnity);
	float droneExcite = static_cast<float>(kDroneExciteDefault);
	/// log2(s), as the controls store them: the attack's time constant, the decay's T60.
	float droneAttack = static_cast<float>(kDroneAttackLog2Default);
	float droneDecay = static_cast<float>(kDroneDecayLog2Default);
	/// THE CHORD, an index into kDroneChords (the Choice); the CHARACTER that drives its strings' landscapes; the WIDTH.
	int droneChord = 0;
	float droneCharacter = static_cast<float>(kDroneCharacterDefault);
	float droneWidth = static_cast<float>(kDroneWidthDefault);
	/// A SEMITONE NUMBER, hardware naming (60 is C3), so 48 is C2.
	int root = 48;
	/// TAPS, continuous: the kernel's length follows it without a step.
	float size = 64.0f;
	float width = 0.3f;
	float blend = 0.5f;
	/// A FADER POSITION, not a gain — see `kFaderUnity`.
	float level = static_cast<float>(kFaderUnity);
	float mix = 1.0f;
	float sustain = 0.0f;
	float jawari = 0.0f;
	float bridgeCurve = 0.5f;
	float sympathy = static_cast<float>(kSympathyNotesDefault);
	float droneVoice = static_cast<float>(kDroneVoiceDefault);
	float droneStiffness = 0.0f;
	float droneBody = static_cast<float>(kDroneBodyDefault);
	float droneTwin = 0.0f;
	/// THE CYCLE'S PATTERN COORDINATE (zoned) and its RATE (0 off).
	float droneCycle = 0.0f;
	int droneCycleSync = 0;
	int headRelease = static_cast<int>(HeadRelease::Gate);
	int droneTuning = static_cast<int>(Tuning::Just);
	/// log2 of how many times LONGER the ring rings.
	float ringDecay = 0.0f;
	float droneCycleLevel = static_cast<float>(kFaderUnity);
	float bounce = static_cast<float>(kBounceDefault);
	/// log2 of how many times FASTER the ring moves.
	float tension = static_cast<float>(kTensionDefault);
	float droneInput = static_cast<float>(kFaderUnity);
	float stringBlend = static_cast<float>(kStringBlendDefault);
	float stringExcite = static_cast<float>(kStringExciteDefault);
	/// 1: the Cycle plucks only while the host's transport plays (v11).
	int droneCycleTransport = 0;
	/// 0..1: where the strings are heard (v11).
	float pickup = 0.0f;
	/// 0..1: the strings' bridge line from scaled to the swing (0) to a fixed height (1) (v13).
	float bridgeFixed = 0.0f;
	/// 0..1: the Cycle's jolt to the main string (v20), Pluck's law; the middle is the old amount.
	float dronePluck = 0.5f;
	/// 0..1: the drone strings' jawari (v20), Jawari's law; 0 no bridge.
	float droneJawari = 0.0f;
	/// 0..1: the drone strings' bounce off their bridge (v21), Bounce's law.
	float droneBounce = static_cast<float>(kBounceDefault);
	/// 0..1: the bridge's lightness as the drone strings see it (v21), Sympathy's law.
	float droneSympathy = static_cast<float>(kSympathyDefault);
	/// 1: the drone goes through the WET FX lane after its own (2026-10-05). Read by the lanes' holder, not the DSP.
	int droneToWetFx = 1;
	/// The drone strings' point, a fraction of the length from the bridge end (v21).
	float droneThread = static_cast<float>(kDroneThreadDefault);
	/// 1: the drone's scanned voices follow the main string's motion (v21).
	int droneFollow = 1;
	/// The drone strings' node cap, an index into kDroneDetailNodes (0: Full, today's strings).
	int droneDetail = 0;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// Row: the serialized form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "resonator", "fx" };

/// **THE HEADINGS.** The top row (Model, Character, Morph) carries none: it is the string itself, named
/// by the card's title. Keyed by the control each section opens at, as the delay's are.
inline constexpr device::FaceSection kFaceSections[] = {
	{ "EXCITE", static_cast<uint8_t>(Param::Bow) },
	{ "NOTES", static_cast<uint8_t>(Param::Voicing) },
	{ "DRONE", static_cast<uint8_t>(Param::DroneOut) },
	{ "OUTPUT", static_cast<uint8_t>(Param::Size) },
	{ "BRIDGE", static_cast<uint8_t>(Param::Jawari) },
};

/// **KEYS THIS DEVICE NO LONGER HAS** (backlog JF): a patch holding one drops it with the reason said, and the key
/// can never name a control again (`device::RetiredKey`).
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "resonate_drone", "2026-09-26", "the Drone switch became a level: a sympathetic drone the input plays (resonate_drone_level)" },
	{ "resonate_size", "2026-09-26",
	  "Size became continuous and modulatable (resonate_size_taps); still taps, but a stepped control turned continuous is a new meaning to the ledger" },
	{ "resonate_drone_level", "2026-09-26", "the drone's level became its own output fader after Level and Blend (resonate_drone_out)" },
	{ "resonate_drone_chord", "2026-09-26", "the Chord became one unbounded coordinate through the φ landscape, the house zone-knob convention (resonate_drone_chord_coord)" },
	{ "resonate_drone_chord_coord", "2026-09-26", "the Chord split in two: the chord a Choice (resonate_drone_chord_choice), its φ driver a plain number (resonate_drone_phase)" },
	{ "resonate_drone_phase", "2026-09-27", "the Drone Phase (a plain number) became the Character, the house zoned coordinate (resonate_drone_character)" },
	{ "resonate_drone_cycle_rate", "2026-09-28", "the Cycle's rate became tempo-synced note divisions, Off first (resonate_drone_cycle_sync)" },
	{ "resonate_bridge_loss", "2026-09-28",
	  "the all-pass contact and its Loss were replaced; how hard the string comes off the bridge is Bounce (resonate_bounce), 1 elastic: the opposite sense" },
	{ "resonate_pluck", "2026-10-04",
	  "Pluck Sens (the onset detector's threshold) became Pluck, how hard every pluck deforms the ring, onsets and the Cycle's alike (resonate_pluck_strength); the threshold is fixed at the old default" },
	{ "resonate_sympathy", "2026-10-05",
	  "Sympathy's reach grew and its law bent (resonate_sympathy_reach): v15's full Sympathy is now mid-travel, the top 1.8x it, and the played notes ride the bridge too" },
	{ "resonate_sympathy_reach", "2026-10-05",
	  "Sympathy became the yielding bridge's lightness on a log law (resonate_sympathy_bridge): every string ends on one bridge that conserves energy; the coupling through the ears is gone" },
	{ "resonate_sympathy_bridge", "2026-10-05",
	  "Sympathy's range widened (resonate_sympathy_bridge_wide): the bridge mass now runs 843 to 63 (was 843 to 150), the old top is the new default; a rescale, so a new key" },
	{ "resonate_sympathy_bridge_wide", "2026-10-05",
	  "Sympathy became the notes' strings' alone (resonate_sympathy_notes); the drone's strings have Drone Sympathy (resonate_drone_sympathy)" },
};

inline constexpr device::Traits kTraits {
	.tag = "RESONATE",
	.name = "Resonate",
	.hasCard = true,
	/// THE RING, LARGE — `ResonateExpanded` (Task 8), beside `ResonateCard` on the face.
	.hasExpanded = true,
	/// **ONE STRING ON THE BUS, AND THAT IS THE DESIGN.** A per-voice instance would give each voice its
	/// own string, and the point of the device is that every note is a head on the SAME one.
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.faceColumns = 4,
	.tags = kTags,
	.faceSections = kFaceSections,
	.retiredKeys = kRetiredKeys,
	/// **NO SITE: ITS LANES ARE ITS OWN** (D3, kept for this device when it was lifted for the Rack and the Chain, 2026-10-29).
	.siteLanes = device::Traits::SiteLanes::None,
};

inline constexpr int16_t kVoicingControl = static_cast<int16_t>(Param::Voicing);
/// PORTA IS LIVE IN MONO — bit i is "live while the gate rounds to i". ROOT IS NO LONGER GATED: the drone is
/// always there, and a gate can only say "the control rounds to i", which a 0..1 level cannot express as "above 0".
inline constexpr uint32_t kMonoOnly = 1u << static_cast<uint32_t>(Voicing::Mono);

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    {.key = "resonate_enabled", .name = "Resonate Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the string is in the chain"},
    {.key = "resonate_model", .name = "Resonate Model", .min = 0.0, .max = static_cast<double>(std::size(kModelNames) - 1),
     .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kModelNames,
     .tooltip = "The physics that moves the string, which changes how the scanned waveform moves and "
                "evolves — not the pitch, which is how fast the string is read. Ring is a closed loop of "
                "masses; Bar is stiff, so its shape bends in broad curves; Tube is pinned at both ends; "
                "Membrane is a sheet fixed at its rim and read round a circle, so a disturbance travels before it is "
                "heard; Stiffening: the springs stiffen as they stretch, so big plucks and hard bowing move "
                "tighter and faster than gentle ones"},
    {.key = "resonate_character", .name = "Resonate Character", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Which set of laws the string follows — its stiffness, damping and rest shape — so the "
                "timbre of the scanned tone and the colour of the filter both change"},
    {.key = "resonate_morph", .name = "Resonate Morph", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Crossfades the string's laws toward a second character. Moving it also bows the string, "
                "so sweeping it makes the waveform churn"},
    {.key = "resonate_bow", .name = "Resonate Bow", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "A steady bow that keeps the string moving on its own. At 0 the string moves only when "
                "the input plays it, and falls still in silence"},
    {.key = "resonate_drive", .name = "Resonate Drive", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How hard the input's loudness bows the string — louder playing makes the waveform move "
                "more"},
    {.key = "resonate_pluck_strength", .name = "Resonate Pluck", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How hard a played note plucks the main string, jolting its shape and its sound — a note's velocity scales "
                "its pluck. The input alone never plucks it, and the Cycle's pluck is Drone Pluck. At 0 nothing you play plucks "
                "it; the middle is the old amount; toward 100% well past it",
     .shortName = "Pluck"},
    {.key = "resonate_voicing", .name = "Resonate Voicing", .min = 0.0,
     .max = static_cast<double>(std::size(kVoicingNames) - 1), .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kVoicingNames,
     .tooltip = "Poly plays every held note as its own reading of the one string; Mono plays the last "
                "note held, gliding by Porta"},
    {.key = "resonate_porta", .name = "Resonate Porta", .min = kPortaLog2Min, .max = kPortaLog2Max, .def = kPortaLog2Min,
     .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = false,
     .tooltip = "Mono: how long the pitch takes to glide to a new note. At the minimum it jumps",
     .gateControl = kVoicingControl, .gateMask = kMonoOnly},
    {.key = "resonate_octave", .name = "Resonate Octave", .min = -3.0, .max = 3.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Number, .names = {}, .tooltip = "Shifts every note the tone plays by whole octaves",
     .centred = true},
    {.key = "resonate_semitone", .name = "Resonate Semitone", .min = -12.0, .max = 12.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Number, .names = {}, .tooltip = "Shifts every note the tone plays by semitones",
     .centred = true, .shortName = "Semi"},
    {.key = "resonate_fine", .name = "Resonate Fine", .min = -100.0, .max = 100.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "Detunes every note the tone plays, in cents", .centred = true},
    // UNITY BY DEFAULT, so the effect sounds out of the box on an audio track with no MIDI.
    {.key = "resonate_drone_out", .name = "Resonate Drone Out", .min = 0.0, .max = 1.0, .def = kFaderUnity,
     .stepped = false, .unit = device::Unit::Fader, .names = {}, .modulatable = true,
     .tooltip = "The drone's own output level: sympathetic strings at the Root note, always sounding beside any "
                "notes you play. The input plays them, so they swell as it arrives and ring on after it stops. "
                "Level and Blend do not touch the drone; 0 is silent",
     .shortName = "Drone"},
    {.key = "resonate_drone_excite", .name = "Resonate Drone Excite", .min = 0.0, .max = 1.0, .def = kDroneExciteDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How the input reaches the drone's strings: 0 = tuned through each string's ear (a note lights up the strings "
                "it matches), 1 = direct (every string hears the whole input); acts only when Drone Drive is up",
     .shortName = "Excite"},
    {.key = "resonate_drone_attack", .name = "Resonate Drone Attack", .min = kDroneTimeLog2Min,
     .max = kDroneAttackLog2Max, .def = kDroneAttackLog2Default, .stepped = false, .unit = device::Unit::LogSeconds,
     .names = {}, .modulatable = true,
     .tooltip = "How quickly the input swells into the drone's strings when it arrives — the time it takes to cover "
                "most of the way. It shapes what plays the strings, never a string already ringing",
     .shortName = "Attack"},
    {.key = "resonate_drone_decay", .name = "Resonate Drone Decay", .min = kDroneTimeLog2Min,
     .max = kDroneDecayLog2Max, .def = kDroneDecayLog2Default, .stepped = false, .unit = device::Unit::LogSeconds,
     .names = {}, .modulatable = true,
     .tooltip = "How long the drone's strings ring — the time a string takes to fall 60 dB once nothing plays it",
     .shortName = "Decay"},
    // THE CHORD, A CHOICE seated as a value box with a menu, so every chord is seen before one is applied.
    {.key = "resonate_drone_chord_choice", .name = "Resonate Chord", .min = 0.0,
     .max = static_cast<double>(std::size(kChordNames) - 1), .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kChordNames,
     .tooltip = "The chord the drone's strings are tuned to, above Root: Unison is Root alone; Fifth, Octave and Fifth + "
                "Octave add those; Major, Minor and Sus4 are the triads with the octave; Sitar is Sa, Pa and the upper Sa "
                "with Re, Ga and Pa above it; Tanpura is its four open strings, Pa, Sa, Sa and low Sa, the two Sa "
                "beating; Tanpura Ma puts Ma where Pa was. Then Sus2, Add9, Maj7, Min7, Dom7 (its seventh the harmonic "
                "7/4 in Just, a flat seventh in Equal), Min9, Quartal, Dim and Aug. Then the ragas, Bilawal to Durga, "
                "each with its nearest Western scale: its notes above Sa with the upper Sa. Each string rings when the "
                "input holds its own pitch or one of its harmonics",
     .shortName = "Chord", .choiceAliases = kChordAliases},
    // THE CHARACTER THAT DRIVES THEM: the house zoned coordinate, unbounded, eight zones a lap.
    {.key = "resonate_drone_character", .name = "Resonate Drone Character", .min = -kDroneCoordMax,
     .max = kDroneCoordMax, .def = kDroneCharacterDefault, .stepped = false, .unit = device::Unit::Number, .names = {},
     .modulatable = true,
     .tooltip = "Moves every drone string through its own landscape — how sharply it hears the input, how slowly it "
                "swells and how long it rings, how bright and how loud it is — each string differently, and shapes "
                "the gourd body's resonances. One continuous turn: it keeps turning. It never changes the chord",
     .zones = kDroneZones, .modSpan = kDroneOneZone, .shortName = "Drone Character"},

    {.key = "resonate_drone_width", .name = "Resonate Drone Width", .min = 0.0, .max = 1.0, .def = kDroneWidthDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How wide the drone's strings sit: at 0 they are all in the middle; turning it up moves each to its "
                "own place between left and right",
     .shortName = "Width"},
    {.key = "resonate_root", .name = "Resonate Root", .min = 24.0, .max = 96.0, .def = 48.0, .stepped = true,
     .unit = device::Unit::Note, .names = {}, .modulatable = true,
     .tooltip = "The note the drone sounds. A macro or LFO moves it in semitones, an octave for a full-depth route; the "
                "ringing strings take each new note through a short fade",
     .modSpan = 12.0},
    {.key = "resonate_size_taps", .name = "Resonate Size", .min = kSizeMinTaps, .max = kSizeMaxTaps, .def = 64.0,
     .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How long the string's shape is stretched when it filters the input — longer reaches "
                "lower and rings darker",
     .wholeReadout = true},
    {.key = "resonate_width", .name = "Resonate Width", .min = 0.0, .max = 1.0, .def = 0.3, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Stereo width: the right channel reads the string from a point further round it, so the "
                "two sides move differently"},
    {.key = "resonate_blend", .name = "Resonate Blend", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "From the pitched tone played at the held notes (0%) to the input filtered through the "
                "string's shape (100%)"},
    {.key = "resonate_level", .name = "Resonate Level", .min = 0.0, .max = 1.0, .def = kFaderUnity, .stepped = false,
     .unit = device::Unit::Fader, .names = {}, .modulatable = true,
     .tooltip = "The level of the wet sound, before it is mixed with the dry"},
    {.key = "resonate_mix", .name = "Resonate Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How much of the output is the string's sound rather than the untouched input"},
    {.key = "resonate_sustain", .name = "Resonate Sustain", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How much the notes you play ring without motion. At 0 they sound only while the input or Bow "
                "moves the string, and fade as the string settles; at full they sound steadily while held, and "
                "the input shapes their timbre. The drone is not affected: the input's level sounds it"},
    {.key = "resonate_jawari", .name = "Resonate Jawari", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "How close the bridge sits to the strings. The string swings onto a line along its length and bounces "
                "off, passing the push to its neighbours, so its motion kinks and the tone buzzes in bursts that come "
                "and go with the swing, on the ring and the notes' strings (the drone's have Drone Jawari), brightest just after "
                "each pluck. At 0 there is no bridge; up, the bridge sits closer to the swing and buzzes more",
     .shortName = "Jawari"},
    {.key = "resonate_bridge_curve", .name = "Resonate Bridge Curve", .min = 0.0, .max = 1.0, .def = 0.5,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "The main string's bridge shape: low, a straight line the whole string meets; high, it curves away "
                "everywhere but under the bow, so only that stretch buzzes. The notes' and the drone's strings buzz on a point, "
                "which has no shape. Does nothing while Jawari is 0",
     .shortName = "Curve"},
    {.key = "resonate_sympathy_notes", .name = "Resonate Sympathy", .min = 0.0, .max = 1.0, .def = kSympathyNotesDefault, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How freely the bridge moves under the notes you play: every string ends on one bridge, so a ringing string "
                "rocks it and sets ringing the strings whose harmonics line up with its own — a note in tune with a drone string "
                "sets it singing, a detuned one hardly. Drone Sympathy is the same for the drone's strings, so the two set how "
                "the notes and the drone ring each other. Energy only moves between them; nothing is added. At 0 the notes ring alone",
     .shortName = "Sympathy"},
    {.key = "resonate_drone_voice", .name = "Resonate Drone Voice", .min = 0.0, .max = 1.0, .def = kDroneVoiceDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "What the drone's strings sound like: at 0 each is a string of its own, ringing only at the harmonics "
                "the input (or the other strings) set going, buzzing on its bridge; toward 100% each is a reading of the "
                "big string at its pitch, swelling with the input",
     .shortName = "Voice"},
    {.key = "resonate_drone_stiffness", .name = "Resonate String Stiffness", .min = 0.0, .max = 1.0, .def = 0.0,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How stiff the drone's strings are: at 0 their overtones are exact multiples of the note; turning it "
                "up stretches them sharp, the higher the more, as a thick string's are — the veena's metallic shimmer",
     .shortName = "Stiffness"},
    {.key = "resonate_drone_body", .name = "Resonate Body", .min = 0.0, .max = 1.0, .def = kDroneBodyDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How much of a gourd body colours the sound — the drone and the notes' strings both: a low body resonance, a "
                "warm band and an upper one, whose "
                "places the Drone Character moves, held to the same loudness. At 0 both are dry",
     .shortName = "Body"},
    {.key = "resonate_drone_twin", .name = "Resonate Twin Body", .min = 0.0, .max = 1.0, .def = 0.0,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "A veena's two gourds: the body split into two, one on each side, its resonances pulled apart — lower "
                "on the left, higher on the right, by an amount the Drone Character sets — so the body widens the "
                "sound. Needs some Body. At 0 there is only the one body",
     .shortName = "Twin"},
    {.key = "resonate_drone_cycle", .name = "Resonate Cycle", .min = -kDroneCoordMax, .max = kDroneCoordMax, .def = 0.0,
     .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "The self-playing drone's pattern: its zone is the order the strings are plucked in — Tanpura, Up, "
                "Down, Up-Down, Outside-In, Inside-Out, Alternate, a φ-walk — and turning within it changes how hard "
                "each step plucks. It keeps turning. Cycle Rate sets it going",
     .zones = kDroneZones, .modSpan = kDroneOneZone, .zoneNames = kCycleZoneNames, .shortName = "Cycle"},
    {.key = "resonate_drone_cycle_sync", .name = "Resonate Cycle Rate", .min = 0.0,
     .max = static_cast<double>(kCycleSyncCount - 1), .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kCycleSyncNames,
     .tooltip = "Plucks the drone's strings in turn, on their own, so it drones with no input: Off, or one pluck every "
                "note division, on the song's grid while the transport plays. Stopped, it keeps the same rate at the "
                "host's tempo. While it runs the device never falls silent",
     .shortName = "Rate"},
    {.key = "resonate_head_release", .name = "Resonate Note Release", .min = 0.0,
     .max = static_cast<double>(std::size(kHeadReleaseNames) - 1), .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kHeadReleaseNames,
     .tooltip = "What letting go of a key does: Gate stops its note at once; Ring lets it sound on until the string "
                "has rung down, so how long you held the key stops mattering",
     .shortName = "Ring"},
    {.key = "resonate_drone_tuning", .name = "Resonate Tuning", .min = 0.0,
     .max = static_cast<double>(std::size(kTuningNames) - 1), .def = static_cast<double>(Tuning::Just), .stepped = true,
     .unit = device::Unit::Choice, .names = kTuningNames,
     .tooltip = "How the drone's strings are tuned: Just puts each on a whole-number ratio of Root (Pa exactly 3:2), "
                "so their overtones coincide and they ring each other; Equal is the piano's tuning, where most of them "
                "beat instead",
     .shortName = "Just"},
    {.key = "resonate_ring_decay", .name = "Resonate Ring Decay", .min = -2.0, .max = 2.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,
     .tooltip = "How long the string rings once something has set it moving: up, it loses less of its motion to damping "
                "and rings longer (to four times), down it settles sooner (to a quarter). The played notes ring as long "
                "as the string does",
     .centred = true, .shortName = "Ring Decay"},
    {.key = "resonate_drone_cycle_level", .name = "Resonate Cycle Level", .min = 0.0, .max = 1.0, .def = kFaderUnity,
     .stepped = false, .unit = device::Unit::Fader, .names = {}, .modulatable = true,
     .tooltip = "How hard the Cycle plucks the drone's strings: at the centre as loud as a note played into it; 0 plucks "
                "none of them. How hard it plucks the main string is Drone Pluck",
     .shortName = "Cycle Level"},
    {.key = "resonate_bounce", .name = "Resonate Bounce", .min = 0.0, .max = 1.0, .def = kBounceDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "How hard the strings come back off the bridge. At full the hit is elastic and the string buzzes "
                "brightly; lower, the hit is deader and the buzz duller, down to a third as strong at 0, while the "
                "note rings about as long. The notes' strings and the main string; the drone's is Drone Bounce. Does nothing while "
                "Jawari is 0",
     .shortName = "Bounce"},
    {.key = "resonate_tension", .name = "Resonate Tension", .min = -4.0, .max = 1.0, .def = kTensionDefault,
     .stepped = false, .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,
     .tooltip = "How taut the string is, which sets how fast it moves: down, its motion slows to a lazy wander (to a "
                "sixteenth); up, it quivers (to twice as fast). The pitch of the notes does not change, only how fast "
                "their timbre moves. Stiffening only slows: its springs already stiffen as it stretches",
     .shortName = "Tension"},
    {.key = "resonate_drone_input", .name = "Resonate Drone Drive", .min = 0.0, .max = 1.0, .def = kFaderUnity,
     .stepped = false, .unit = device::Unit::Fader, .names = {}, .modulatable = true,
     .tooltip = "Drone Drive: how hard the input plays the drone's strings — at the centre as it arrives, down to nothing, up "
                "to +12 dB. The Cycle's plucks and the strings' sympathy with each other are not affected, so turning it down "
                "lets the Cycle play the drone while the notes you play leave it alone",
     .shortName = "Drive"},
    {.key = "resonate_string", .name = "Resonate String", .min = 0.0, .max = 1.0, .def = kStringBlendDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How much of each note is its own string: every note you play has a string of its own at its pitch, "
                "played by the ring's voice and by the input and buzzing on its bridge as the ring swings toward it. At "
                "0 you hear the ring's voice alone",
     .shortName = "String"},
    {.key = "resonate_string_excite", .name = "Resonate String Excite", .min = 0.0, .max = 1.0,
     .def = kStringExciteDefault, .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "What plays the notes' strings: low, the ring's voice, so the main string's timbre colours them; high, the "
                "input itself, so its own spectrum rings in them",
     .shortName = "Excite"},
    {.key = "resonate_drone_cycle_transport", .name = "Resonate Cycle Transport", .min = 0.0, .max = 1.0, .def = 0.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "On, the Cycle plucks only while the host's transport plays, on the song's grid; off, it also plucks when the "
                "transport is stopped, at the host's tempo",
     .shortName = "Transport"},
    {.key = "resonate_pickup", .name = "Resonate Pickup", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "Where the strings are heard along their length. At 0 a third of the way along, the round, even tone; "
                "up, nearer the bridge, where the upper partials are strongest: brighter, more bite and more of the "
                "buzz, at the same loudness",
     .shortName = "Pickup"},
    {.key = "resonate_bridge_fixed", .name = "Resonate Bridge Fixed", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "Whether the notes' bridge follows how hard their strings are played. At 0 it stands at a share of each string's "
                "own swing, so a soft pluck and a hard one buzz alike, one louder; toward 100% it stands at a fixed height, so "
                "a hard pluck presses the string into it and buzzes, and a soft one only grazes it. The drone's strings always "
                "stand on a fixed bridge. Does nothing while Jawari is 0",
     .shortName = "Fixed"},
    {.key = "resonate_drone_pluck", .name = "Resonate Drone Pluck", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "How hard the Cycle plucks the main string at each of its steps, jolting its shape and its sound — as Pluck "
                "does for the notes you play, which it leaves alone. At 0 the Cycle plays only the drone's strings; the middle is "
                "the old amount. Does nothing while the Cycle is off",
     .shortName = "D.Pluck"},
    {.key = "resonate_drone_jawari", .name = "Resonate Drone Jawari", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "How close the drone strings' bridge sits, so they buzz — the tanpura's jawari, apart from Jawari, which is the "
                "main string's and the notes'. At 0 they have no bridge; from just above 0 the bridge comes in gradually",
     .shortName = "D.Jawari"},
    {.key = "resonate_drone_bounce", .name = "Resonate Drone Bounce", .min = 0.0, .max = 1.0, .def = kBounceDefault, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "How hard the drone's strings come back off their bridge, as Bounce does for the notes: at full the hit is "
                "elastic and the buzz bright; lower, deader and duller. Does nothing while Drone Jawari is 0",
     .shortName = "D.Bounce"},
    {.key = "resonate_drone_sympathy", .name = "Resonate Drone Sympathy", .min = 0.0, .max = 1.0, .def = kSympathyDefault,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "How freely the bridge moves under the drone's strings, as Sympathy does for the notes: up, the drone's "
                "strings ring each other (and, with Sympathy, the notes); at 0 they ring alone",
     .shortName = "D.Sympathy"},
    {.key = "resonate_drone_to_wet_fx", .name = "Resonate Drone to Wet FX", .min = 0.0, .max = 1.0, .def = 1.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
     .tooltip = "On, the drone (after its own DRONE FX) goes through the WET FX lane too, which then acts on everything the "
                "strings sound; off, it joins after WET FX, so the two lanes run side by side. Changes nothing while WET FX "
                "is empty",
     .shortName = "Drone to Wet FX"},
    {.key = "resonate_drone_thread", .name = "Resonate Drone Thread", .min = kDroneThreadMin, .max = kDroneThreadMax,
     .def = kDroneThreadDefault, .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = false,
     .tooltip = "Where along the drone strings their jawari first touches, as the thread under a tanpura's strings sets it: "
                "close to the bridge end a darker buzz; further along a brighter one, and more of it. Does nothing while Drone "
                "Jawari is 0",
     .shortName = "Thread"},
    {.key = "resonate_drone_follow", .name = "Resonate Drone Follow", .min = 0.0, .max = 1.0, .def = 1.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
     .tooltip = "On, the drone's scanned voices swell with the main string's motion, so the notes you play move them; off, "
                "they follow only their own strings",
     .shortName = "Follow"},
    {.key = "resonate_drone_detail", .name = "Resonate Drone Detail", .min = 0.0,
     .max = static_cast<double>(std::size(kDroneDetailNames) - 1), .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kDroneDetailNames, .modulatable = false,
     .tooltip = "How many points each drone string is built from: Full is the whole string; 96, 64 and 48 are lighter on "
                "the CPU and lose the strings' highest overtones first",
     .shortName = "Detail"},

};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// EVERY DEFAULT PINNED TO THE DESCRIPTOR, so a fresh state cannot depend on which road it arrived by.
namespace detail {
[[nodiscard]] constexpr double defOf(Param p) {
	return kParamDescs[static_cast<size_t>(p)].def;
}
} // namespace detail
static_assert(Settings {}.enabled == (detail::defOf(Param::Enabled) >= 0.5));
static_assert(Settings {}.model == static_cast<int>(detail::defOf(Param::Model)));
static_assert(Settings {}.character == static_cast<float>(detail::defOf(Param::Character)));
static_assert(Settings {}.morph == static_cast<float>(detail::defOf(Param::Morph)));
static_assert(Settings {}.bow == static_cast<float>(detail::defOf(Param::Bow)));
static_assert(Settings {}.drive == static_cast<float>(detail::defOf(Param::Drive)));
static_assert(Settings {}.pluck == static_cast<float>(detail::defOf(Param::Pluck)));
static_assert(Settings {}.voicing == static_cast<int>(detail::defOf(Param::Voicing)));
static_assert(Settings {}.porta == static_cast<float>(detail::defOf(Param::Porta)));
static_assert(Settings {}.octave == static_cast<int>(detail::defOf(Param::Octave)));
static_assert(Settings {}.semitone == static_cast<int>(detail::defOf(Param::Semitone)));
static_assert(Settings {}.fine == static_cast<float>(detail::defOf(Param::Fine)));
static_assert(Settings {}.droneOut == static_cast<float>(detail::defOf(Param::DroneOut)));
static_assert(Settings {}.droneExcite == static_cast<float>(detail::defOf(Param::DroneExcite)));
static_assert(Settings {}.droneAttack == static_cast<float>(detail::defOf(Param::DroneAttack)));
static_assert(Settings {}.droneDecay == static_cast<float>(detail::defOf(Param::DroneDecay)));
static_assert(Settings {}.droneChord == static_cast<int>(detail::defOf(Param::DroneChord)));
static_assert(Settings {}.droneCharacter == static_cast<float>(detail::defOf(Param::DroneCharacter)));
static_assert(Settings {}.droneVoice == static_cast<float>(detail::defOf(Param::DroneVoice)));
static_assert(Settings {}.droneStiffness == static_cast<float>(detail::defOf(Param::DroneStiffness)));
static_assert(Settings {}.droneBody == static_cast<float>(detail::defOf(Param::DroneBody)));
static_assert(Settings {}.droneTwin == static_cast<float>(detail::defOf(Param::DroneTwin)));
static_assert(Settings {}.droneCycle == static_cast<float>(detail::defOf(Param::DroneCycle)));
static_assert(Settings {}.droneCycleSync == static_cast<int>(detail::defOf(Param::DroneCycleSync)));
static_assert(Settings {}.headRelease == static_cast<int>(detail::defOf(Param::HeadRelease)));
static_assert(Settings {}.droneTuning == static_cast<int>(detail::defOf(Param::DroneTuning)));
static_assert(Settings {}.ringDecay == static_cast<float>(detail::defOf(Param::RingDecay)));
static_assert(Settings {}.droneCycleLevel == static_cast<float>(detail::defOf(Param::DroneCycleLevel)));
static_assert(Settings {}.bounce == static_cast<float>(detail::defOf(Param::Bounce)));
static_assert(Settings {}.tension == static_cast<float>(detail::defOf(Param::Tension)));
static_assert(Settings {}.droneInput == static_cast<float>(detail::defOf(Param::DroneInput)));
static_assert(Settings {}.stringBlend == static_cast<float>(detail::defOf(Param::StringBlend)));
static_assert(Settings {}.stringExcite == static_cast<float>(detail::defOf(Param::StringExcite)));
static_assert(Settings {}.droneCycleTransport == static_cast<int>(detail::defOf(Param::DroneCycleTransport)));
static_assert(Settings {}.pickup == static_cast<float>(detail::defOf(Param::Pickup)));
static_assert(Settings {}.bridgeFixed == static_cast<float>(detail::defOf(Param::BridgeFixed)));
static_assert(Settings {}.dronePluck == static_cast<float>(detail::defOf(Param::DronePluck)));
static_assert(Settings {}.droneJawari == static_cast<float>(detail::defOf(Param::DroneJawari)));
static_assert(Settings {}.droneBounce == static_cast<float>(detail::defOf(Param::DroneBounce)));
static_assert(Settings {}.droneSympathy == static_cast<float>(detail::defOf(Param::DroneSympathy)));
static_assert(Settings {}.droneThread == static_cast<float>(detail::defOf(Param::DroneThread)));
static_assert(Settings {}.droneFollow == static_cast<int>(detail::defOf(Param::DroneFollow)));
static_assert(Settings {}.droneDetail == static_cast<int>(detail::defOf(Param::DroneDetail)));
static_assert(Settings {}.droneToWetFx == static_cast<int>(detail::defOf(Param::DroneToWetFx)));
static_assert(Settings {}.droneWidth == static_cast<float>(detail::defOf(Param::DroneWidth)));
static_assert(Settings {}.root == static_cast<int>(detail::defOf(Param::Root)));
static_assert(Settings {}.size == static_cast<float>(detail::defOf(Param::Size)));
static_assert(Settings {}.width == static_cast<float>(detail::defOf(Param::Width)));
static_assert(Settings {}.blend == static_cast<float>(detail::defOf(Param::Blend)));
static_assert(Settings {}.level == static_cast<float>(detail::defOf(Param::Level)));
static_assert(Settings {}.mix == static_cast<float>(detail::defOf(Param::Mix)));
static_assert(Settings {}.sustain == static_cast<float>(detail::defOf(Param::Sustain)));
static_assert(Settings {}.jawari == static_cast<float>(detail::defOf(Param::Jawari)));
static_assert(Settings {}.bridgeCurve == static_cast<float>(detail::defOf(Param::BridgeCurve)));
static_assert(Settings {}.sympathy == static_cast<float>(detail::defOf(Param::Sympathy)));

//------------------------------------------------------------------------------------------------//
// THE PICTURE — what the DSP publishes for the card's live ring, once per physics tick.            //
//------------------------------------------------------------------------------------------------//

/// NODES IN THE PICTURE: 32 for the chain models (Ring, Bar, Tube, Stiffening use the first 32) and
/// 64 for Membrane, an 8x8 grid ROW-MAJOR.
inline constexpr size_t kPictureNodes = 64;
inline constexpr size_t kChainNodes = 32;

/// **ONE TICK OF THE STRING, AS THE CARD SEES IT.** Floats only, because the road it travels is
/// `device::ReadingsSink` — a span of floats, `count` wide — and the host stores it in
/// `RackEngine::kMaxReadings` / `GuiBridge::kMaxDeviceReadings` (224 each) per slot; `kReadingCount`
/// below is 161, checked against the rack's store in `device_registry.h`. The struct is the DEVICE'S
/// statement of the layout; `packReading` / `unpackReading` are the one translation to and from the flat
/// floats, so the card never hand-counts an offset.
struct Reading {
	/// THE MODEL'S INDEX (`Model`, and so `WeaveModel`), as a float — exact for 0..4.
	float model = 0.0f;
	/// THE SMOOTHED STATE — the shape the tone path scans and the filter path convolves with.
	std::array<float, kPictureNodes> nodes {};
	/// THE REST POSE the laws pull toward, same layout.
	std::array<float, kPictureNodes> rest {};
	/// EACH HEAD'S SCAN POSITION along the model's scan path, in [0, 1).
	std::array<float, kMaxHeads> heads {};
	/// EACH HEAD'S GAIN, 0 for an inactive head — so the card draws exactly the heads that sound.
	std::array<float, kMaxHeads> headGains {};
	/// WHERE THE LAST PLUCK LANDED along the scan path, [0, 1).
	float pluckPos = 0.0f;
	/// TICKS SINCE THE LAST PLUCK, saturating at `kPluckAgeMax` (the card fades a flash with it).
	float pluckAge = 0.0f;
	/// THE STRING'S ENERGY — the same quantity the DSP's tests read.
	float energy = 0.0f;
	// THE INPUT, FOR THE CARD'S METER (Task 8) — APPENDED, so every offset above is unchanged.
	/// THE INPUT'S LEVEL AS THE EXCITATION HEARS IT — the envelope follower's, linear.
	float inLevel = 0.0f;
	// THE SCAN, FOR THE CARD'S HEAD MARKS (Task 8 review) — APPENDED. A head reads the whole path once per pitch
	// cycle, so its phase strobes at audio rate; what a picture can show is the slow part.
	/// THE TRAVEL PHASE, [0, 1): how far the ring has rotated under the scan origin — the phase every head
	/// reads from, moving at a readable rate.
	float travel = 0.0f;
	/// THE PITCH EACH HEAD SOUNDS, in semitones (a MIDI note number, the offset and any glide included) —
	/// meaningful only where `headGains` is above 0.
	std::array<float, kMaxHeads> headNotes {};
	/// 1 WHILE THE DEVICE IS PARKED, else 0 — the rest logic's own state (`ResonateDsp`'s `resting_`). A parked
	/// string that was still moving is FROZEN where it was, not reset, so without this a card cannot tell a
	/// string held still from a picture that stopped. APPENDED (the card's review, 2026-09-26).
	float parked = 0.0f;
	// THE DRONE'S STRINGS (the chord, 2026-09-26) — APPENDED. `heads` above are NOTE heads only now; each drone
	// string is a head of its own here, in the chord's order (lowest first), up to kMaxDroneStrings.
	/// EACH DRONE STRING'S SCAN POSITION, [0, 1), as `heads`.
	std::array<float, kMaxDroneStrings> droneHeads {};
	/// EACH DRONE STRING'S GAIN — Drone x its follower x its level; 0 for a silent or unused string.
	std::array<float, kMaxDroneStrings> droneGains {};
	/// EACH DRONE STRING'S PITCH in semitones (Root + its interval + the offset), for every string the chord has,
	/// sounding or not — so a card can draw the chord's strings before the input plays them.
	std::array<float, kMaxDroneStrings> droneNotes {};
	/// HOW MANY STRINGS THE CHORD HAS (1..kMaxDroneStrings), as a float.
	float droneCount = 0.0f;

	friend bool operator==(const Reading&, const Reading&) = default;
};
static_assert(std::is_trivially_copyable_v<Reading>);

/// WHERE `pluckAge` STOPS COUNTING — about three seconds of ticks at the string's ~344 Hz.
inline constexpr float kPluckAgeMax = 1024.0f;

/// THE FLAT LAYOUT, in `Reading`'s field order.
inline constexpr size_t kReadModel = 0;
inline constexpr size_t kReadNodes = kReadModel + 1;
inline constexpr size_t kReadRest = kReadNodes + kPictureNodes;
inline constexpr size_t kReadHeads = kReadRest + kPictureNodes;
inline constexpr size_t kReadHeadGains = kReadHeads + kMaxHeads;
inline constexpr size_t kReadPluckPos = kReadHeadGains + kMaxHeads;
inline constexpr size_t kReadPluckAge = kReadPluckPos + 1;
inline constexpr size_t kReadEnergy = kReadPluckAge + 1;
inline constexpr size_t kReadInLevel = kReadEnergy + 1;
inline constexpr size_t kReadTravel = kReadInLevel + 1;
inline constexpr size_t kReadHeadNotes = kReadTravel + 1;
inline constexpr size_t kReadParked = kReadHeadNotes + kMaxHeads;
inline constexpr size_t kReadDroneHeads = kReadParked + 1;
inline constexpr size_t kReadDroneGains = kReadDroneHeads + kMaxDroneStrings;
inline constexpr size_t kReadDroneNotes = kReadDroneGains + kMaxDroneStrings;
inline constexpr size_t kReadDroneCount = kReadDroneNotes + kMaxDroneStrings;
inline constexpr size_t kReadingCount = kReadDroneCount + 1;
static_assert(kReadingCount == 184);

/// `Reading` TO THE FLOATS THE SINK CARRIES.
void packReading(const Reading& r, std::array<float, kReadingCount>& out);
/// AND BACK — `count` is what the host holds; a field it does not cover reads as its default, so a short
/// store draws an empty picture rather than reading past its end.
[[nodiscard]] Reading unpackReading(const float* values, size_t count);

/// THE RENDER PATH AND THE NOTE ROAD, over one shared storage (resonate.cpp, resonate_dsp.h).
extern const device::AudioOps kAudioOps;
extern const device::NoteOps kNoteOps;
/// THE TWO FX LANES (resonate.cpp, resonate_lanes.h): `rack::laneContainerOps` over DRONE FX and WET FX.
extern const device::ContainerOps kContainerOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: an effect, not a modulator. Spelled because every field of this struct is a decision.
	.outputs = {},
	.audio = &kAudioOps,
	// **THE NOTE ROAD SHARES `.audio`'s STORAGE**: held notes become heads on the string. `kNoteOps`'
	// stateBytes/stateAlign EQUAL `kAudioOps`', because `RackEngine` takes the first non-null ops and
	// allocates once (`Descriptor::notes`).
	.notes = &kNoteOps,
	// **AND A CONTAINER: TWO FX LANES** (2026-10-05), DRONE FX and WET FX, both before the Mix — resonate.cpp. Like
	// SKLITTER, a device that holds chains lives above the rack (`feathers-fx-sklitter`).
	.container = &kContainerOps,
	// FILTER & EQ: a resonator is a filter — the input is convolved with the string's shape, and the
	// tone path is that string read at pitch. Display metadata only (`Category`).
	.category = device::Category::FilterEq,
};

} // namespace feathers::fx::resonate
