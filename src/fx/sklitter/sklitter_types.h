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

#pragma once

#include "device/device.h"
#include "device/registry.h"
#include "fx/sklitter/groove.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator> // std::size, for kPerformOutputs
#include <string_view>

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// SKLITTER — the reference's stutterer, as a Feathers device.                                            //
//--------------------------------------------------------------------------------------------------//
//
// Seven modes over one ring buffer, driven by phi triangles and a deterministic per-slice hash.
// What follows is what a reader of the declarations needs.

/// WHAT THE BUFFER IS DOING TO THE AUDIO. Stored by label, so the order is free; the Choice list's names
/// come from this enum.
enum class Mode : uint8_t {
	Burst = 0,   ///< gated: play a grain of rate/2, then silence until the next trigger
	Repeat,      ///< beat repeat with a count
	Time,        ///< Zone A combines grains, Zone B holds one slice
	Shuffle,     ///< phi-driven segment reordering
	Grain,       ///< granular cloud: two voices crossfaded, Zone A spreads them
	Pattern,     ///< Zone A picks a slice pattern: seq / weave / skip / mirror / pairs
	Pitch,       ///< pitch manipulation against a scale
	kCount
};

inline constexpr const char* kModeNames[] = {
    "Burst", "Repeat", "Time", "Shuffle", "Grain", "Pattern", "Pitch",
};
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount),
              "a mode without a name would render as an empty row in the Choice list");

inline constexpr const char* kOffOnNames[] = {"Off", "On"};

/// **WHERE THE FEEDBACK IS TAPPED — the one thing about this circuit that is NOT decided.** See
/// `Param::FeedbackTap`. Two names, so the switch shows both states and the multivalue-toggle rule
/// is satisfied rather than argued around.
inline constexpr const char* kFeedbackTapNames[] = {"Grain FX", "Post FX"};

/// **THE TWO GRAIN BLEND ORDERS — an AUDITION control's states. See `Param::GrainOrder`.**
///
/// **NAMED ORDERS RATHER THAN Off/On, DELIBERATELY.** `device::isOffOnChoice` tests for literally
/// "Off" and "On", and only that pair gets a `SwitchChip`; anything else renders as a picker showing
/// both names. That is the right surface here and not a compromise: a chip captioned with one order's
/// name can never say what the OTHER order is, so a player would be choosing between the thing they
/// can read and a thing they cannot. The multivalue-toggle rule is about exactly that failure, and
/// two states only escape it when *next* and *which* are the same thing — which "Off/On" manages and
/// a pair of named orders does not.
inline constexpr const char* kGrainOrderNames[] = {"P Write First", "P Write Last"};

/// **THE SEQUENCE LADDER — how many bars before the arrangement repeats.** Four is the top because
/// four is all there is: `planFor` reads two bits of the bar counter and no more, so a fifth bar has
/// nothing new to be. See `Param::SeqLength`.
inline constexpr const char* kSeqLengthNames[] = {"1 bar", "2 bars", "3 bars", "4 bars"};

/// HOW MANY BARS EACH RUNG OF THE SEQUENCE LADDER IS WORTH.
inline constexpr int32_t kSeqLengthBars[] = {1, 2, 3, 4};
static_assert(std::size(kSeqLengthNames) == std::size(kSeqLengthBars),
              "a rung without a name would render as an empty row in the Choice list");

/// **THE BUFFER LADDER: `1/4 1/2 1 2 4`.** Sub-bar rungs are deliberate — a quarter-bar
/// buffer is a short loop the slice grid runs off the end of, and the offsets wrap inside it.
inline constexpr const char* kBufferLengthNames[] = {"1/4 bar", "1/2 bar", "1 bar", "2 bars",
                                                     "4 bars"};

/// EACH RUNG AS A FRACTION OF A BAR. Not an integer ladder: the bottom two rungs are shorter than the
/// bar the slice grid divides, which is the case a `size_t` count of bars cannot express at all.
inline constexpr double kBufferLengthBars[] = {0.25, 0.5, 1.0, 2.0, 4.0};
static_assert(std::size(kBufferLengthNames) == std::size(kBufferLengthBars),
              "a rung without a name would render as an empty row in the Choice list");

/// **THE DEFAULTS ARE THE BEHAVIOUR THAT SHIPPED**, named rather than left to be inferred: rung 3 of
/// the sequence ladder is 4 bars (the hard-wired period), rung 2 of the buffer ladder is one bar (the
/// only length the looper could capture). A device loaded without these fields sounds as it did.
inline constexpr double kSeqLengthDefault = 3.0;
inline constexpr double kBufferLengthDefault = 2.0;


/// The reference's 25 pitch targets: twelve scales, then thirteen fixed semitone shifts. Indexes
/// `kScaleSemitones` in `decisions.h`, which is why the count is pinned against it there.
inline constexpr const char* kPitchScaleNames[] = {
    "Chromatic", "Major", "Minor", "MajPent", "MinPent", "Blues", "Dorian", "Mixolyd",
    "MajTri",    "MinTri", "Sus4", "Dim",
    "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9", "+10", "+11", "+12", "+13",
};

/// THE CONTROLS. **The first six USED TO keep the reference's order and that WAS not cosmetic**: its
/// `processStutter` takes a `modulatedValues` array indexed exactly this way, so a reordering here was
/// a silent mis-mapping that every test in this repo would pass.
///
/// **TWO OF THOSE SIX ARE GONE — `MacroConfig` AND `Macro`, DELETED 2026-09-19** (we will most
/// likely get rid of the center macro and have it be zero). The perform circle's centre is a constant
/// zero push instead of the knobs' (design §2's boxed note), and a configuration with no macro to act on
/// goes with it: `zero × f(cfg) = zero`. **EVERY COLUMN AFTER THEM SHIFTED DOWN TWO**, which is the case
/// `kStateVersion` exists for and is the opposite of the eleven appends this device made two days
/// earlier — see `state_block_order.h`, where the version went to 15.
///
/// **NOTHING STILL INDEXES A `modulatedValues` ARRAY BY THIS ENUM.** The parity the note above protected
/// was a READING convenience — our `stateOf` names every column — and the reference's order survives
/// where it still means something: the four zoned coordinates the port reads (Zone A, Zone B and, now,
/// each waypoint's Cfg) are unchanged in meaning, and the plan functions take their arguments by name.
enum class Param : uint8_t {
	ZoneA = 0,   ///< structural zone — which slice, offset, length, skip
	ZoneB = 1,   ///< timbral zone — reverse, filter, delay, envelope shape
	PWrite = 2,  ///< buffer write-back probability. 0 freezes what was recorded
	Density = 3, ///< output dry/wet probability

	Rate = 4,
	Enabled = 5, ///< BYPASS. Defaults ON (all effects should default to on)
	Engage = 6,  ///< THE GESTURE. Defaults OFF. Not the same thing as Enabled — see below
	Mode = 7,

	Quantized = 8, ///< snaps Rate to a division. NOT quantised engagement — see below
	/// **LATCH STOOD HERE AND IS GONE WITH THE MECHANISM IT BELONGED TO.** It said "keep going after
	/// release", which is a question only a MOMENTARY gesture can ask. `Engage` is a parameter the
	/// host holds at 1 for as long as its automation says, so there is no release to survive — and
	/// honouring latch made the control ONE-WAY, which is what the first install hit: turning the enable
	/// switch off did not stop the device. See `engage.h`. A switch that
	/// cannot change the sound is one this project does not ship (the honesty rule).

	/// **ZONE A PHASE AND ZONE B PHASE STOOD HERE AND ARE GONE.** They were the reference's
	/// push-and-twist secrets, declared as real controls — and they were a SECOND ROAD to a position
	/// the coordinate already reaches, because an unbounded coordinate's WHOLE PART IS ITS LAP.
	/// `zone_coord_cell.h` states the rule after OWLTT paid for it: unbounding the coordinate made
	/// the phase reachable through it (the whole part IS the lap), so the phase params were deleted, and
	/// every zoned coordinate in the project is one control and one box. Zone A and Zone B are zoned
	/// coordinates; this port reintroduced exactly the arrangement that rule ended.
	///
	/// THE OFFSETS ARE STILL REACHED. `Sklitter::advanceSlice` splits each coordinate and hands the lap
	/// to the DSP as its phase, which is what it was already doing — these params only ever ADDED to
	/// that.
	///
	/// **AND MACRO CFG PHASE STOOD HERE TOO, EXCUSED BY A CLAIM THAT WAS SIMPLY FALSE.** This note
	/// once said the two that remain are not coordinates and keep their own controls. Macro Config is a
	/// coordinate: `ScatterMacroConfig` derives from the SAME `ZoneBasedDualParam` as the two zones,
	/// has eight named regions, and carries the same push-and-twist secret — `effectivePhaseOffset()`
	/// is written identically in all three. So its phase offset is folded into the Macro Config
	/// coordinate's value box like the zones'.
	///
	/// **AND HERE THE SECOND ROAD IS EVEN SHORTER THAN IT WAS FOR THE ZONES.** Their offsets at least
	/// reached laps the coordinate had to travel to; this one is consumed as `macroConfig +
	/// phase * 0.1`, clamped into the same 0..1 — a scaled duplicate of the control beside it, with
	/// no wrap and nowhere of its own to go. What it bought was a tenth of the range in finer steps,
	/// and a value box's per-digit drag gives that to the coordinate itself for nothing.
	Gamma = 9,

	PitchScale = 10, ///< Pitch mode only
	StereoWidth = 11, ///< Grain mode only

	/// **HOW MANY BARS THE ARRANGEMENT TAKES TO REPEAT** — 4 bars and lower.
	///
	/// THE PERIOD WAS FOUR AND IT WAS NOT A SETTING — `planFor` consumes the bar counter as two bits
	/// (`ofs.barIndex & 1`, `& 2`), which is the reference's multi-bar evolution and is hard-wired to
	/// a four-bar cycle. This chooses how many of those four the sequence actually uses, by taking the
	/// counter modulo the length before the bits are read: at 4 the behaviour is exactly what shipped,
	/// at 1 the bar-to-bar evolution is off entirely and every bar plays the same arrangement.
	SeqLength = 12,

	/// **HOW MUCH AUDIO THE LOOPER CAPTURES**, in bars — a quarter of one up to four.
	///
	/// **A SETTING, NOT A PERFORMANCE PARAM, AND NOT A MODULATION TARGET.** Changing it DISCARDS the
	/// buffer and starts the capture again, which is acceptable by design — the alternative is resampling a loop into a
	/// different length, which is a different effect wearing a setting's clothes.
	///
	/// **THE SLICE GRID IS UNAFFECTED: Rate still means slices per BAR** (chosen over
	/// slices-per-buffer). So a longer buffer does not stretch the rhythm — it gives the slice offsets
	/// more history to reach into, and a four-bar buffer scatters material from four bars ago on the
	/// same grid. The two settings are then genuinely independent, which is the point of there being
	/// two of them.
	BufferLength = 13,

	/// **WHETHER PLAYBACK BELONGS TO THE HOST'S TRANSPORT** — playback can stop with the transport, with
	/// the buffer frozen, and stays in phase on restart. Backlog GT.
	///
	/// ON does two things, and they are one idea: the device stops playing while the host is stopped,
	/// and the buffer FREEZES rather than being recorded over. While the host rolls, the read cursor
	/// also stops being a free-running counter and becomes a function of the song position — so a DAW
	/// loop or a scrub is followed, not only a stop. The decision is which phase "in phase" means — the
	/// TRANSPORT's, not the gesture's: a player who triggered on the downbeat resumes with the same
	/// buffer as if they had re-triggered on the downbeat. The engage point is wrapped against the
	/// sequence length to keep it in phase; it is not replayed at the same moment in the track.
	///
	/// **AND OFF IS A FEATURE RATHER THAN THE ABSENCE OF ONE (backlog HE).** A refinement after the
	/// first build collapsed the either/or it had been specified as: the device can be both free-running
	/// and transport-locked, re-phasing when the transport restarts, and with the toggle off the effect
	/// slips past a stopped transport and rejoins when it resumes. So **BOTH settings re-phase on a
	/// restart**; the toggle decides only whether audio keeps being produced while the transport is
	/// parked. The rejoin ducks the wet to the dry first, because unlocked the seek interrupts audio
	/// the player is hearing — see `Sklitter::mixAt` and `backlog HE: MEASURE` for the number.
	///
	/// **DEFAULT OFF, AND THAT IS THE SIDE THE MECHANISM FORCES RATHER THAN A TASTE.** `TransportClock`
	/// declares `bool playing = false` (`device.h`) — a host that publishes no transport at all reads
	/// as STOPPED, so an ON default would silence the device outright in any host or test harness that
	/// never sets the flag. The other half is the performance case: a free-running stutter over
	/// a parked DAW is how you audition a buffer, and a default that takes that away is a default that
	/// costs something. Both point the same way, and the mechanical one is decisive.
	///
	/// **GATED TO THE LOOPER MODES, verified by reading the engine rather than by grepping ids.** The
	/// freeze is a statement about the RING, and Burst captures one slice through its own play path
	/// (`playBurst`); the phase derivation is `playLooper`'s slice grid. In
	/// Grain the stop and the freeze are real and the phase derivation is not — Grain scatters over the
	/// bar instead of walking it and never calls `advanceSlice` — which is why the control is honest
	/// there on the strength of the first two.
	TransportLocked = 14,

	/// **A FLOOR UNDER EVERY GRAIN'S FADE-IN AND FADE-OUT.** It began as a minimum ATTACK time and became
	/// a minimum ENVELOPE affecting both attack and release, because a floor under one edge of a grain and
	/// none under the other guards half the problem — the END of a grain is where the length gate
	/// hard-zeroes the rest of the slot, which is as sharp a discontinuity as any start.
	///
	/// The anti-click attack today is a GATE, not a fade: the grain is silent until its first zero
	/// crossing and then at full level. That is the reference's mechanism and it works when a crossing
	/// arrives soon — but on material with a slow first cycle, or DC-ish content where the search
	/// finds nothing, the gate opens abruptly on whatever sample it lands on. This ramps the first N
	/// milliseconds of every grain from silence, so there is a floor under the edge whatever the
	/// crossing search does.
	///
	/// **DEFAULT IS THE BOTTOM, WHICH IS OFF.** `Unit::LogSeconds` declares that its minimum means NO
	/// TIME AT ALL — see `device.h` — so a device that never touches this control behaves exactly as it
	/// did.
	MinEnv = 15,

	/// **DRY AGAINST WET FOR THE WHOLE DEVICE** — a basic mix knob.
	///
	/// **DEFAULT IS FULLY WET, AND THAT IS CORRECTNESS RATHER THAN COMPATIBILITY.** A stutter that
	/// arrives half-mixed is a different effect: the gesture's whole point is that the device TAKES
	/// OVER, and every mode's dry behaviour is already expressed by Density and the plan's dry votes,
	/// which thin the grain stream deliberately. This is the blunt instrument on top of those.
	Mix = 16,

	/// **THE DELAY SEND'S OVERRIDE — ALWAYS, NEVER, OR PHI.** The override knob raises the probability
	/// as high as 1.0 at full strength or ties it to zero. Backlog EW.
	///
	/// **IT IS NOT A BIAS, AND THAT DISTINCTION IS THE WHOLE REASON IT IS A CONTROL AT ALL.** The macro
	/// already moves this probability — `delayProb + push.dDelay`, clamped — and backlog EW says why
	/// that cannot do this job: a term that is ADDED to a triangle and then clamped reaches somewhere
	/// that depends on where the triangle currently sits, so ALWAYS and NEVER are not places the player
	/// can steer to, only places they might land. This takes the finished probability and lerps it to an
	/// endpoint: `+1` is exactly 1.0 and `-1` exactly 0.0, for any phi value underneath.
	/// `slice_plan.h`'s `overriddenProbability` is the arithmetic and carries the exactness measurement.
	///
	/// **DEFAULT 0, WHICH IS THE IDENTITY RATHER THAN A COMPROMISE.** A device that has never had this
	/// knob touched draws exactly the probability it drew before it existed — bit for bit, no epsilon —
	/// which is what makes it an override of phi rather than a third opinion about it.
	///
	/// **INSERTED HERE RATHER THAN APPENDED, AND `kStateVersion` MOVED FOR IT.** It is a knob, and the
	/// knobs run to `Mix`; the seven after it are the perform circle, which is a block. Appending would
	/// have put a knob on the far side of the circle to protect stored rows that do not exist
	/// (CLAUDE.md, 2026-08-22). `state_block_order.h` has the entry.
	///
	/// **GATED TO THE MODES THE SEND IS ACTUALLY REACHED IN, WHICH IS NARROWER THAN "LOOPER".** The only
	/// reader of `shouldDelay` is `advanceSlice`, and `playGrain` never calls it — so this is
	/// `Min Env`'s gate, and for the same reason: Burst has its own play path, and Grain
	/// has one that has no delay send in it. The honesty rule, verified by reading the engine.
	SendOverride = 17,

	//----------------------------------------------------------------------------------------------//
	// THE TWO CONTINUOUS WRITERS — backlog GU/GV, decided 2026-09-21. A DRAFT.                   //
	//----------------------------------------------------------------------------------------------//
	//
	// **THE BUFFER HAS THREE WRITERS NOW AND THEY DO NOT INTERACT** (GU/GV addendum 2): every write
	// to the buffer is a blend, reducing the buffer content and adding the signal, in the order dry
	// write, then feedback, then `P Write`. Each is `old + (src − old)·amount`,
	// which is the crossfade `ring_.writeAt` already performed for `P Write` alone; what is new is
	// two more SOURCES and two dialled amounts. They compose, and composition is the whole of how
	// they relate — so what survives of the buffer's history after one sample is exactly
	// `(1 − a_dry)(1 − a_fb)(1 − a_pw)`, and a writer later in the order is attenuated less by the
	// ones after it. `P Write` therefore dominates and `Dry Write` is the most written over.
	//
	// **AND `P Write` WAS ALWAYS A FEEDBACK, which is why it is untouched here** — it is already as
	// much feedback as the device needs: it writes the played grain
	// back into the buffer it was read from, and only its TAP (the raw pre-envelope grain)
	// distinguishes it from the new one.
	//
	// **NEITHER IS A MODULATION DESTINATION, AND THAT `false` IS MEANT** (deferred; it may be added
	// later). A destination flag nothing routes to is the honesty
	// rule failing in the other direction, and a modulated gain on a regenerating loop is a runaway
	// question that is deferred rather than answered.

	/// **THE CLEAN WRITE FROM THE INPUT** — a clean buffer overwrite from the input, as a knob separate
	/// from feedback. GU's continuous in-phase write arriving as a control:
	/// fresh input blended into the ring at the linear write head, every sample, at a dialled depth.
	///
	/// **IT DOES NOT REPLACE OR GATE `P Write`.** The three-way question the previous addendum asked
	/// about was settled the other way — they blend, they do not interact.
	///
	/// **DEFAULT 0, WHICH IS THE IDENTITY.** A device that never has this knob touched writes exactly
	/// what it wrote before it existed, bit for bit — and that is not a compatibility concession but
	/// the same argument `P Write`'s own default rests on: a looper that overwrites as fast as it
	/// reads has nothing left to scatter.
	DryWrite = 18,

	/// **THE FEEDBACK CIRCUIT — THE WET, POST-EFFECTS, DAMPED, BLENDED INTO THE RING.** Backlog GV.
	///
	/// **WET ONLY — the dry does NOT go round.**
	/// The ring already records the INPUT, so a loop carrying dry would double it every pass and the
	/// accumulation would swamp the processed material long before the gain reached anything musical.
	///
	/// **A CONTINUOUS AMOUNT, NOT A PER-SLICE DRAW.** Every other routing decision in this device is
	/// a phi-driven probability — `shouldDelay`, the lane send, `pWriteThisSlice_`. This one is
	/// deliberately not: the loop is a LEVEL.
	///
	/// **GENTLE FIXED DAMPING IS IN THE PATH AND THE LANE STAYS IN THE LOOP.** A loop with no colour
	/// returns a spectrally identical copy each pass, so it accumulates and smears rather than
	/// decaying into anything; that is why every good delay has a tone control in its feedback. The
	/// decision is BOTH — built-in damping AND the player's lane still inside — so an empty lane is usable
	/// and a filter, pitch shifter or crusher in it still shapes each pass. See
	/// `kFeedbackDampHz`/`kFeedbackDcHz` for the numbers, which are a draft's, to be set by ear.
	Feedback = 19,

	/// **WHERE THE FEEDBACK IS TAPPED, AND IT IS DELIBERATELY UNDECIDED**: post grain fx or post master
	/// fx. The two are different instruments and
	/// the difference is to be decided by ear, so this ships as a switch that can be flipped while it plays
	/// rather than as a choice somebody made in advance.
	///
	/// - **Grain FX** — the loop carries the GRAIN lane's processing; the master lane is heard but
	///   not recorded. More controlled: a master-lane reverb colours the output without its tail
	///   feeding itself.
	/// - **Post FX** — the loop carries everything, so a master-lane device is INSIDE the feedback
	///   path and its output compounds each pass. Much more extreme, and it is where a device with
	///   its own feedback, placed in a lane that is itself inside a feedback loop, becomes real.
	///
	/// **TWO STATES, SO IT IS NOT THE MULTIVALUE TOGGLE THE PROJECT RULES OUT** — with two, *next*
	/// and *which* are the same thing, which is the `SwitchChip` exemption CLAUDE.md names.
	FeedbackTap = 20,

	/// **AN AUDITION CONTROL. IT EXISTS TO BE DECIDED BY EAR AND THEN DELETED — backlog HU, 2026-09-21.**
	///
	/// **KEPT, DELIBERATELY, UNTIL THE REVISIT:** the default (`P Write First`)
	/// ships, the switch STAYS, and the deletion described below waits for backlog HZ. Until then this
	/// is a setting chosen to keep open, not one nobody chose. The rest of this comment is the
	/// original argument and still describes what happens when HZ is decided.
	///
	/// **THIS IS SCAFFOLD, NOT FURNITURE, AND SAYING SO HERE IS THE POINT.** Which of the two blend
	/// orders Grain should use cannot be settled without hearing them, so both are built and the choice
	/// is made by ear, exactly as `FB Tap` was built for the
	/// question that could not be answered from a description. **When one is picked, one state is deleted, this
	/// control goes with it, and the surviving order becomes unconditional.** A control left standing
	/// after its question is answered is a setting nobody chose.
	///
	/// **WHAT THE TWO ORDERS ARE.** In the looper modes the ring's three writers run in the decided
	/// order — DRY, then FEEDBACK, then `P Write`. In GRAIN that order cannot be had for free, because
	/// `P Write` is performed per sample inside `playGrain` and deferring it to the flush is what gets
	/// it last:
	///
	/// - **`P Write First` (A, the DEFAULT and what shipped)** — `P Write` writes immediately, then
	///   DRY and FEEDBACK blend over it at the flush. FEEDBACK lands last and unattenuated.
	/// - **`P Write Last` (B, the decided order)** — `P Write` is recorded and performed at the flush
	///   behind the other two, so `P Write` lands last and unattenuated, as in every other mode.
	///
	/// **A IS THE DEFAULT SO THAT NOTHING ALREADY HEARD OR SAVED MOVES UNTIL THE SWITCH IS FLIPPED.** And B is
	/// reached ONLY while `writersActive()` — with both new knobs at zero the two states are
	/// bit-identical to each other and to the build before this control existed, which is asserted
	/// rather than argued.
	///
	/// **THE PRICE OF B, NAMED RATHER THAN DISCOVERED:** `P Write` changes from immediate to deferred
	/// at the instant `Feedback` or `Dry Write` leaves zero, which is a discontinuity in a control the
	/// player is not touching. That is exactly why A was shipped first. See `kGrainOrderNames` and
	/// `tests/test_fx_sklitter_grainorder.cpp` for what it measures at.
	///
	/// **NOT A MODULATION DESTINATION.** It restructures the write path rather than scaling anything,
	/// so a route into it is a ramp through a cliff — the same reason `Buffer Length` is not one.
	GrainOrder = 21,

	/// **WHEN A SLICE STARTS, SWUNG — backlog IG 2026-09-22; split in two by backlog IO.** The feel
	/// (`Straight`, `Swing 4/8/16`, `Groove 4/8/16`, `Free 4/8/16`) is a Choice, and how much of it is
	/// `GrooveAmount` beside it. `groove.h` carries the mechanism — a monotonic time warp of bar phase that
	/// the slice boundaries are cut on — and the numbers the ear will set.
	///
	/// **ONE ZONED CONTROL UNTIL 2026-09-23**, whose drag rolled Swing 16 at full into Groove 4 at nothing.
	/// A mode plus a phase offset is the right model — two numbers cannot roll into each
	/// other, so the fence is the shape rather than a clamp.
	///
	/// **INSERTED BEFORE THE PERFORM BLOCK, NOT APPENDED AFTER IT**, for the reason `SendOverride` and
	/// the two writers give: the seven below are a block a card lays out as one, and these are controls.
	/// Each of the seven moved down one column for each, and `kStateVersion` moved for both.
	///
	/// **GATED TO THE FIVE SLICE MODES** — `kGrooveModes`. Burst captures a division at the
	/// press and gates it on its own clock, not the bar's; Grain is out, at least at first.
	/// **NOT A DESTINATION**, as no Choice on this device is: a route through a list of feels would jump
	/// between them, which is the roll the split exists to remove.
	GrooveMode = 22,
	/// **HOW MUCH OF THE FEEL — a swing % for Swing, a share of the one φ template for Groove** (`groove.h`).
	/// A destination, moved by its whole range per unit of amount. Gated on `GrooveMode` being a Swing or a
	/// Groove, which is itself gated on the slice modes — the surfaces follow the chain
	/// (`GenericCard::relevant`).
	GrooveAmount = 23,
	/// **WHERE ALONG FREE'S WALK — a position, unbounded, that never wraps** (backlog IO: Free drives
	/// non-periodic phi groove mutations, so its position must not wrap and may go arbitrarily high).
	/// Its own control rather than `GrooveAmount` read another way, so every stored value is audible in the
	/// mode that shows it: a depth stops at 100% and a phase does not, and one control could not be both
	/// without dead drag travel in the modes that clamp. Gated on the three Free modes.
	FreePhase = 24,

	/// THE PERFORM CIRCLE. Four waypoints fixed at the DIAGONAL rim points, each a CONFIGURATION; NOT
	/// modulatable (by design, no individual waypoint is a modulation destination). The puck and its
	/// depth are.
	///
	/// **A WAYPOINT USED TO BE A (Macro, Macro Cfg) PAIR AND IS NOW A CONFIGURATION ALONE** — `MacroN`,
	/// `MacroE`, `MacroS` and `MacroW` stood immediately before these four and are DELETED (2026-09-19:
	/// the waypoints need no macro knobs; only the pad controls them). Each waypoint's push is built at
	/// **macro = 1.0**: the waypoint is its character at FULL strength, and the puck's RADIUS is the
	/// amount, which is exactly what the weights already do now that the centre is a zero push — centre 0,
	/// half-way 0.5, rim 1.0. The four values are implied rather than lost.
	///
	/// **APPENDED WHEN THEY ARRIVED, AND SHIFTED SIX DOWN AND ONE BACK UP SINCE** — two knob columns and
	/// four waypoint macros left from in front of them, and `SendOverride` was then inserted in front of
	/// them, so they are columns 19..25 of a row that is now 26 wide — `TransportLocked` (backlog GT)
	/// was inserted at 15 with the other two loop settings, which moved them one further down again.
	/// **THE ORDER INSIDE THE FOUR IS `Corner`'s** — 1, 2, 3, 4 clockwise from the top-left, exactly as
	/// `perform.h` declares it, so `Settings::cornerCfg[kCfg2]` and `Param::Cfg2` name the same waypoint
	/// without a translation table anywhere between them.
	///
	/// **THEY WERE `CfgN`…`CfgW` UNTIL 2026-09-19, AND THAT RENAME MOVED NO ID.** Removing the compass
	/// names was part of the point: the north/east/south/west labels go away. Only the
	/// names changed — `cornerCfg[0..3]` is the same array in the same order and each default stayed on
	/// its own slot. **The ids DID move, in a different change that landed the same day**: `SendOverride`
	/// was inserted at 17, so these four went 17..20 → 18..21 and the perform trio 21..23 → 22..24. Two
	/// unrelated edits to one row, and it is worth keeping them separate in the telling: one changed what
	/// these columns are CALLED, the other changed WHICH columns they are.
	/// **AND THEY MOVED AGAIN, THREE COLUMNS, 2026-09-21** — `Dry Write`, `Feedback` and `FB Tap` were
	/// inserted after `Send Override` for the same reason `Send Override` itself was: they are knobs
	/// and the knobs run to there, while the seven below are a BLOCK a card lays out as one.
	/// **AND ONE FURTHER DOWN AGAIN for `Grain Order` (2026-09-21)** — an AUDITION control, so these
	/// seven move back up the day it is deleted.
	/// **AND ONE FURTHER DOWN FOR `Groove` (2026-09-22, backlog IG)**, and one more when it split into a
	/// mode and an amount (2026-09-23, backlog IO), and one more for `FreePhase`.
	Cfg1 = 25, Cfg2 = 26, Cfg3 = 27, Cfg4 = 28,
	PerfX = 29, PerfY = 30, PerfDepth = 31,

	/// **INPUT GAIN — backlog KZ.** dB, −24..+24, 0 the identity;
	/// applied to what the device WRITES of its input into the ring — the capture, the pre-warm and `Dry Write` — and
	/// never to the input where it is HEARD (Mix's dry, a dry slice, a dry grain voice), so the dry stays honest.
	/// `P Write` writes back what a slice PLAYS, so a dry slice's write-back is the input as heard. Appended after
	/// the perform block: the stored row is keyed, so the column is where it lands, not what it means.
	InputGain = 32,

	/// **FREEZE — backlog KX** — a freeze-buffer button that overrides every write, keeping the current
	/// sound static. A switch: while it is on the ring takes NO writes at all — not the
	/// capture, the pre-warm, `Dry Write`, `Feedback`, `P Write`, a press's reset or the capture's seam fade — so
	/// playback keeps cutting the buffer as it stood. Everything else runs on its own clock (a capture still counts
	/// its frames, the write head still walks), so off resumes writing exactly where the song is.
	///
	/// **THE BOUNDARY IS A CROSSFADE, NOT A CUT**: every write is blended by a
	/// gain that ramps to 0 over the loop seam's own `wrapFade_` (5 ms) on the way in and back to 1 on the way out,
	/// so the material the read heads cross where writing stopped is a 5 ms crossfade from new to old — no harder
	/// than an ordinary loop point. **A patch loaded frozen over an empty buffer stays frozen and empty**: the
	/// buffer is not saved, and a press while frozen records nothing, so it plays silence until Freeze comes off.
	/// A switch at threshold 0.5, saved, and a modulation destination (Engage's precedent).
	Freeze = 33,

	/// **RECORD GATE — backlog MJ** — a record threshold (with a record light) that stops the device recording
	/// silence over the buffer. 0..1, 0 is the default and OFF, and
	/// bit-identical to a device without it. Above 0, the input's level (the larger of |L| and |R| as it ARRIVES, before
	/// Input Gain, so the two knobs do not move each other) is compared against `recordGateThreshold(value)`; while it
	/// stays below, KX's write gate shuts — the same per-sample gain and the same 5 ms seam crossfade Freeze uses — so
	/// silence never lands in the buffer. **A HARDCODED 500 ms HOLD** (`kRecordGateHoldSeconds`, deliberately no
	/// knob): any sample at or over the threshold re-arms it, so a decay is not chopped and the gate does not chatter.
	RecordGate = 34,

	kCount
};

//--------------------------------------------------------------------------------------------------//
// TWO PAIRS THAT LOOK ALIKE AND ARE NOT. BOTH WERE GOT WRONG ONCE WHILE DESIGNING THIS.              //
//--------------------------------------------------------------------------------------------------//
//
// **`Enabled` IS NOT `Engage`.** `Enabled` is the device's bypass and `Traits::enableControl` points
// at it, so it is what the card's enable square draws. `Engage` is the stutter gesture — what the
// hardware does by HOLDING an encoder, which a plugin has no way to express, so it becomes a 0/1
// control the DAW automates and MIDI drives. Point `enableControl` at `Engage` and the enable square
// silently becomes a stutter trigger.
//
// **`Quantized` IS NOT QUANTISED ENGAGEMENT.** It snaps `Rate` to a musical division — 4ths, 8ths,
// 16ths, 32nds, 64ths — at the moment the stutter begins, and restores the unsnapped value when it
// ends. Grid-ALIGNED triggering is a different mechanism entirely (the arm/standby machine), keyed on
// transport position rather than on this flag.
//
// **AND RATE IS ALWAYS TEMPO-RELATIVE, IN EVERY MODE, CLASSIC INCLUDED.** In the reference, the tempo
// scaling sits behind a test of a sync setting that looks like an on/off switch but is not: the setting
// starts at a fixed non-zero value and nothing ever changes it (the reference notes as much). So the
// scaling always applies, and the shift it uses is a constant minus the knob's magnitude.

/// **THE SHARED `device::ZoneName`, NOT A FIFTH COPY OF IT.** Four devices had each declared this
/// struct identically and each card converted its own into the widget's. It is one type on the
/// descriptor now, which is what lets a card that was not written for this device draw the names.

/// **WHERE THE BAR COUNTER WRAPS.** The reference's `kBarIndexWrap`, whose comment explains the
/// number: *"Bar counter wraps at 64 (supports phrases up to 32 bars)"*. Only the low two bits reach
/// `planFor`, but the modulus is what keeps the counter from growing without bound in a device that
/// may be left running.
inline constexpr int32_t kBarIndexWrap = 64;

/// **FOUR BEATS TO A BAR — the one number, not three copies of it.** `resolveBarFrames` multiplies by
/// it, `prewarm` waits for a beat index divisible by it, and the transport lock divides the song
/// position by it (`Param::TransportLocked`). Those three have to agree or ring frame 0 is not where
/// the reader thinks it is: the pre-warm's own comment records what a one-beat disagreement sounded
/// like — *"every slice the device then played was a beat displaced from the grid"*.
inline constexpr int32_t kBeatsPerBar = 4;

/// **TIME MODE'S PHRASE, IN BARS.** The reference's `kTimePhraseLength`: Time resets its phrase every
/// four bars, and takes the bar-end silence only on the last bar of one rather than on every bar.
inline constexpr int32_t kTimePhraseLength = 4;

/// **THE REFERENCE'S SAMPLE RATE, AND THE FOUR LENGTHS THAT WERE LEFT IN ITS SAMPLES.**
///
/// The anti-click windows were converted to TIME when they were ported — `prepare` computes them as
/// `0.015 * sampleRate_` and so on — and these four were not, so they stayed sample COUNTS that mean
/// 44.1 kHz. At 96 kHz every one of them is HALF its intended duration, which is the floor doing half
/// its job at exactly the rate where a short grain is most likely to click. Found by the parity sweep
/// (backlog EO, item 8).
///
/// Stated as the reference's own arithmetic rather than as a rounded millisecond count, so the
/// original number is still legible and the conversion is checkable.
inline constexpr double kReferenceRate = 44100.0;
/// Burst's cycle floor — the reference's 64 samples, 1.45 ms.
inline constexpr double kMinBurstCycleSeconds = 64.0 / kReferenceRate;
/// Burst's grain floor — the reference's 32 samples, 0.73 ms.
inline constexpr double kMinBurstGrainSeconds = 32.0 / kReferenceRate;
/// The shortest slice a schedule may produce — the reference's 256 samples, 5.8 ms.
inline constexpr double kMinSliceSeconds = 256.0 / kReferenceRate;
/// **A RATCHET SUB-SLICE IS NEVER SHORTER THAN 24 MS** — the reference's `kMinSubSliceMs = 24.0f`, as
/// `24 * 44.1` samples. A ratchet dividing a short slice finer than that plays 24 ms pieces and runs out at
/// the slice end instead. Ours had no floor, so a fast ratchet buzzed where the reference stutters.
inline constexpr double kMinSubSliceSeconds = 1058.0 / kReferenceRate;
/// Grain's length floor — the reference's 1024 samples. Its own comment already called this
/// *"a ~23 ms floor"*, which is the duration it was always meant to be.
inline constexpr double kMinGrainSeconds = 1024.0 / kReferenceRate;

/// **THE MINIMUM-ATTACK RANGE, IN log2 SECONDS** — `Unit::LogSeconds` holds the LOG, not the seconds.
/// The floor is 0.1 ms, which is shorter than any edge anyone can hear and therefore reads as OFF by
/// the unit's own convention; the ceiling is 20 ms, past which a "minimum attack" is an envelope
/// rather than a click guard.
inline constexpr double kMinEnvFloor = -13.2877;  // log2(0.0001)
inline constexpr double kMinEnvCeiling = -5.6439; // log2(0.02)

/// **THE FIRMWARE'S GRAIN FADE, IN SECONDS** — `prepareGrainEnvelopeQ31`'s two constants, converted
/// from its 44.1 kHz counts for the same reason every other length here was.
///
/// `kAntiClickSamples = 440` is its own comment's *"~10ms fade at 44.1kHz"*; `kMinAntiClickBase = 64`
/// is *"Absolute minimum for click-free audio"* — the floor that keeps a fade on both ends even when
/// `envShape` has scaled one of them to nothing.
inline constexpr double kAntiClickSeconds = 440.0 / kReferenceRate;
inline constexpr double kMinAntiClickSeconds = 64.0 / kReferenceRate;

/// **THE WRITE'S OWN FADE, INDEPENDENT OF THE OUTPUT'S** — the firmware's `kPWriteFade = 64`, its own
/// comment *"~1.5ms at 44.1kHz"*.
///
/// It is taken as a `min` AGAINST the grain envelope and applied only when the output's own fade is
/// SHORTER than this — so the written region is protected even where the grain is not, which is the
/// case a percussive `envShape` creates by design.
inline constexpr double kPWriteFadeSeconds = 64.0 / kReferenceRate;

/// **THE CROSSING DUCK — how long the wet dips either side of a read passing the live write head**
/// (backlog JC, 2026-09-24): the wet output ducks over a few ms around the moment a sped-up read crosses
/// the live write head, like the release zone. Ours, not the reference's: the firmware's
/// ring has no continuous writer, so nothing there crosses a head.
///
/// **1 ms EITHER SIDE, SET BY MEASUREMENT** (`test_fx_sklitter.cpp`, *"MEASURE: backlog JC — clicks with
/// Dry Write at 1 against the frozen ring's own"*). 113.3 Hz sine, Dry Write 1, Time/Shuffle/Pattern/Pitch
/// at the puck's rest and on E at 0.4, zones and rates swept; the clicks over the frozen ring's own, summed
/// over the eight rows: no duck 314 · 0.25 ms 39 · 0.5 ms 0 · 1 ms 0 · 2 ms 2 · 3 ms 5. Below 0.5 the dip
/// is too steep to hide the join; above 1 a read that JUMPS to within the width of the head, rather than
/// closing on it, gets there before the slewed gain does. 1 is the longer of the two clean widths, so the
/// dip is as gentle as the measurement allows: 2 ms of hole at each crossing.
inline constexpr double kCrossingDuckSeconds = 0.001;

/// **HOW FAR THE IDLE HEAD MAY DISAGREE WITH THE SONG BEFORE IT IS A DISCONTINUITY, IN FRAMES** (the
/// 2026-09-24 crush's guard — `Sklitter::seatPrewarmHead`). **SET BY MEASUREMENT** (`test_fx_sklitter`,
/// *"MEASURE: the idle head's drift…"*; 44.1/48/88.2/96 kHz × 60/97.3/120/173.71 BPM × blocks 1..8192 and
/// irregular, 60 s, positions quantised to CLAP's beat time, also from 10^6 beats):
///   - THE NOISE FLOOR IS 0. At every rate and block size the append head and the song head agree to the
///     frame wherever the bar is a whole number of frames (60 and 120 BPM).
///   - THE ONE REAL DRIFT IS THE BAR'S ROUNDING. The ring is a whole number of frames and the song's bar
///     is not (173.71 BPM at 48 kHz: 66317.43), so the two heads part by the remainder every pass — left
///     alone, 18 frames in 60 s. The old per-call seat followed it one frame at a time.
/// So 1: a disagreement of a frame is the rounding and is appended through; 2 is a drift the head follows
/// (MEASURED at this value: worst 2, at most 9 re-seats a minute, all of them 2-frame steps). And a
/// STALE host's disagreement grows by the call's length each call, so any call of 2 frames or more is
/// never mistaken for a discontinuity that holds still.
inline constexpr long long kPrewarmSeatToleranceFrames = 1;

/// **THE FEEDBACK LOOP'S GENTLE FIXED DAMPING — backlog GV, and its existence is a decision.**
///
/// Three options were weighed: built-in damping; no damping, with the player's device in the lane BEING
/// the damping; or both. The decision is damping, with the lane remaining in the loop. The
/// problem it answers, stated plainly: a loop at gain `g` with nothing colouring it returns a
/// spectrally IDENTICAL copy each pass, so the sound does not decay into anything — it accumulates
/// and smears. Every good delay has a tone control in its feedback path for this reason.
///
/// **TWO ONE-POLES, AND THEY GUARD DIFFERENT ENDS.** The lowpass is the damping proper: each pass
/// is darker than the last, so a regenerating figure erodes towards nothing instead of ringing.
/// The DC/rumble highpass is the half that is easy to forget and impossible to unhear — a blend
/// loop is an integrator at DC, so any offset in the wet (a bipolar grain window, an asymmetric
/// device in the lane) walks the buffer towards a rail with no lowpass able to stop it.
///
/// **"GENTLE" IS THE ORIGINAL WORD AND THESE NUMBERS ARE A DRAFT'S, NOT A MEASUREMENT'S.** They are
/// stated here as frequencies rather than as coefficients so that the thing to move is legible, and
/// they are to be set by ear — a model gives the shape and an instrument gives the number, and the
/// instrument for a damping curve is a person listening.
inline constexpr double kFeedbackDampHz = 6000.0;
inline constexpr double kFeedbackDcHz = 20.0;

/// **THE CEILING ON THE FEEDBACK SOURCE, AND IT IS A BOUND RATHER THAN A SOUND.**
///
/// The blend itself cannot diverge: `old + (src − old)·a` with `a` in [0, 1] is a convex combination,
/// so the buffer can never exceed the larger of what it held and what arrived. **What CAN diverge is
/// the loop through a lane with gain in it** — backlog GV names the case and it is allowed rather
/// than refused: a device with its own feedback, placed in a lane that is itself inside a feedback
/// loop, is not a reason to refuse the arrangement but a reason to bound it deliberately rather than
/// discover it. A unity-plus device on the `Post FX` tap multiplies every pass, and nothing in the
/// blend stops it reaching an infinity and then a NaN, which is the one outcome that is not a sound
/// at all.
///
/// **SO THE SOURCE IS CLAMPED AND NOTHING ELSE IS.** ±2.0 is +6 dBFS: above anything a mix contains,
/// so it is inaudible on material, and finite, so a runaway saturates loudly instead of silencing the
/// device with a NaN. It is the difference between an instrument that misbehaves and one that stops.
/// **A DRAFT'S NUMBER AND A DRAFT'S SHAPE** — a soft clip would be kinder and is a decision about
/// sound rather than about safety, so it is left to the ear.
inline constexpr float kFeedbackCeiling = 2.0f;

/// **WHICH MODES EACH CONTROL MEANS ANYTHING IN — the reference's `isRelevant()`, gathered.**
///
/// Many of the value boxes are mode-specific, and a control a mode does not use is hidden. The reference states it
/// per menu item and the statements are scattered across six headers; this is all of them in one
/// place, as a mask over `Mode`, so a card can ask without knowing what SKLITTER is.
///
/// `Mode` gates every one of them, which is why `ParamDesc::gateControl` takes an INDEX rather than
/// this being a Sklitter-shaped rule bolted onto the descriptor.
///
/// **FIVE OF THE REFERENCE'S STATEMENTS DO NOT MATCH ITS OWN ENGINE, AND THE ENGINE WINS HERE** — the
/// honesty rule: a control is shown iff it changes the sound. Found by MEASURING, not by reading
/// (`test_fx_sklitter.cpp`'s "MATRIX" case moves every control in every mode through the host's road):
/// - P Write is listed for Repeat, and `hasPWrite` excludes Repeat — hidden there now
/// - Density is hidden in Pitch, and density's draw runs in Pitch too — shown there now
/// - Quantized's snap sets Burst's captured length, so it is shown for Burst
/// - Gamma declared no gate at all, and only the looper modes' phi fields read it — looper modes now
inline constexpr uint32_t kModeBit(Mode m) {
	return 1u << static_cast<uint32_t>(m);
}

/// Every mode — for a control that is always live.
inline constexpr uint32_t kAllModes = 0x7Fu;
/// Burst: the one mode that plays one captured slice rather than a cut-up bar.
inline constexpr uint32_t kGatedModes = kModeBit(Mode::Burst);
/// The six looper modes — the complement, and what `Settings::isLooperMode` answers.
inline constexpr uint32_t kLooperModes = kAllModes & ~kGatedModes;

/// **WHERE THE TWO CONTINUOUS WRITERS ACT, AND IT IS NARROWER THAN "LOOPER" FOR TWO SEPARATE
/// REASONS** — backlog GU/GV. Named once here so the gate in the DSP and the gate on the descriptors
/// cannot drift, which is the shape the honesty rule keeps failing on.
///
/// **GRAIN WAS OUT AND IS NOW IN (backlog HU, 2026-09-21), AND THE SENTENCE THAT KEPT IT OUT WAS
/// WRONG.** Played, the circuit was missing controls one would expect to apply in Grain: Dry Write,
/// Feedback and FB Tap.
///
/// The original exclusion rested on backlog HN's hazard measurement — 24,570 reads of 96,000 in Grain
/// landing on positions written earlier in the same chunk, against 0 of ~86,600 in the looper modes,
/// because `playGrain` never consults the overlap duck (it lives in `advanceSlice`, which Grain does
/// not call) and Grain's reads are re-seeded anywhere in the buffer. From that, this comment once said
/// feedback in Grain was "unbounded by construction". **THAT PHRASE WAS FALSE AND NOTHING MEASURED IT.**
/// Every writer is a LERP — `old + (src − old)·amount`, amount in [0,1] — which is a convex
/// combination and cannot exceed `max(|old|, |src|)`; and the feedback source is damped and clamped
/// at ±`kFeedbackCeiling` before the blend sees it. The ring is BOUNDED in Grain by construction, not
/// unbounded.
///
/// **AND WHEN IT WAS MEASURED, GRAIN CAME OUT SAFER THAN THE LOOPER** (`test_fx_sklitter_grainfb.cpp`).
/// Ring peak per bar-long pass at feedback 1.0, six passes:
///
///     GRAIN   empty or unity lane   0.314  0.134  0.069  0.039  0.023  0.013
///     SHUFFLE empty or unity lane   0.603  0.279  0.236  0.212  0.190  0.175
///     GRAIN   x4 device in the loop 1.257  1.855  2.000  2.000  2.000  2.000   out peak 5.98
///     SHUFFLE x4 device in the loop 2.000  2.000  2.000  2.000  2.000  2.000   out peak 8.00
///
/// Grain decays THIRTEEN TIMES further than Shuffle where it is safe, and where it is not — a device
/// with gain inside the loop — it pegs LATER and QUIETER than the shipped looper behaviour does.
/// Invariant across host block sizes 64 / 512 / 2048 / 4096 and across Zone A's whole spread.
///
/// **SO THE DUCK IS NOT WHAT PROTECTS AGAINST THE FAILURE MODE, AND GRAIN GETS NO GUARD.** What the
/// duck prevents is a read seeing a write from the same chunk; what actually goes wrong is loop gain
/// above unity in the player's lane, which pegs at any delay and in any mode. A read/write proximity
/// guard would have cost feedback in Grain and bought nothing the numbers show.
///
/// **REPEAT STAYS OUT, FOR `P Write`'S OWN STATED REASON**, copied rather than invented: *"Repeat is
/// reading one figure over and over and writing into it would erase the figure."* A continuous
/// writer erases it faster than a per-slice draw does, not slower.
///
/// **THIS IS DECLARED AS WELL AS ENFORCED, which is the honesty rule's other half** — the two
/// controls' descriptions say where they act, and `hasFeedbackCircuit` below is what the engine
/// asks. Nothing silently does nothing.
inline constexpr uint32_t kWriterModes = kLooperModes & ~kModeBit(Mode::Repeat);

/// **WHERE GROOVE ACTS: THE FIVE MODES THAT CUT THE BAR INTO SLICES ON ITS OWN GRID** (backlog IG).
/// Repeat, Time, Shuffle, Pattern and Pitch all walk `advanceSlice`'s grid, which is where the warp is
/// applied. **Burst is out because it is not bar-derived**: it captures one division at
/// the press and replays it on a cycle of that division's length, so there is no bar phase to warp — a
/// swung Burst would be a different mechanism (a late trigger per cycle), and is not guessed at here.
/// **Grain is out by decision**, at least at first.
///
/// Named once, read by the descriptor's gate AND the engine's `Sklitter::grooveFeel`, for the reason
/// `kWriterModes` gives.
inline constexpr uint32_t kGrooveModes = kLooperModes & ~kModeBit(Mode::Grain);

static_assert(static_cast<uint32_t>(Mode::kCount) <= 32,
              "the relevance mask is a uint32_t; a mode enum past 32 needs a different shape");
static_assert(kAllModes == (1u << static_cast<uint32_t>(Mode::kCount)) - 1u,
              "kAllModes must name exactly the modes that exist");

/// ZONE COUNT. The reference's `ScatterParams::getZoneAInfo`/`getZoneBInfo` both default to eight.
inline constexpr int32_t kZones = 8;

/// **EVERY ENTRY IS EMPTY, AND THAT IS THE HONEST ANSWER RATHER THAN A GAP.**
/// Zone names mean nothing in most modes: Sklitter's zones are read by whichever
/// mode is running — Zone A picks a slice pattern in Pattern, a grain spread in Grain, a combine
/// length in Time — so one name per region would be right for at most one mode and misleading in the
/// other seven. `ParamDesc` carries ONE name list per control and there is no per-mode list, so the
/// choice is an invented name or none.
///
/// The rule this repo already keeps decides it: `owltt_types.h` leaves its zones 1 and 2 empty
/// because its DSP switch has no case for them, and `automod_types.h` states why — *"an evocative
/// name for an unmeasured region is a promise the sound may not keep."* Nothing here has been
/// measured per mode, so nothing here is named. The value box still counts `zone : phase`, which is
/// the firmware's display and is true regardless of mode.
///
/// FILL THESE IN WHEN A REGION HAS BEEN MEASURED IN A NAMED MODE, not before.
inline constexpr device::ZoneName kZoneAZoneNames[kZones] = {
	{ "", "" }, { "", "" }, { "", "" }, { "", "" },
	{ "", "" }, { "", "" }, { "", "" }, { "", "" },
};
inline constexpr device::ZoneName kZoneBZoneNames[kZones] = {
	{ "", "" }, { "", "" }, { "", "" }, { "", "" },
	{ "", "" }, { "", "" }, { "", "" }, { "", "" },
};

/// **MACRO CONFIG'S EIGHT, WHICH DO MEAN SOMETHING** — unlike Zone A's and Zone B's above. They are
/// the reference's own, verbatim, from `ScatterMacroConfig::getZoneName`: a weather ladder read in
/// one direction, so a region name is a claim about intensity rather than about a mode.
///
/// **KEPT AS A REFERENCE, NO LONGER SHOWN (backlog IW, 2026-09-23).** A ladder of intensity is a continuum, not a set
/// of modes, so Cfg 1–4 are FREE-ONLY: no dropdown, a sigil for the lap. The names stay because the four waypoint
/// defaults below are the centres of Dew, Cloud, Storm and Night, and the note there reads them against this list.
inline constexpr device::ZoneName kMacroConfigZoneNames[kZones] = {
	{ "Frost", "barely there — the macro hardly reshapes anything" },
	{ "Dew", "a light touch" },
	{ "Fog", "softened and blurred" },
	{ "Cloud", "gathering" },
	{ "Rain", "steady and obvious" },
	{ "Storm", "heavy" },
	{ "Dark", "near the top of what the macro can ask for" },
	{ "Night", "the whole of it" },
};

/// **GROOVE'S TEN FEELS** — a Choice since backlog IO, in `GrooveMode`'s order. The grid is the BAR's (a
/// quarter, an eighth, a sixteenth note), not the slice's. Swing: every second step late. Groove: every
/// step late by its own φ-derived amount, the Amount scaling it. Free: the same shape, the Amount walking
/// through a family of φ patterns instead of scaling one.
inline constexpr const char* kGrooveModeNames[] = {
	"Straight", "Swing 4",  "Swing 8", "Swing 16", "Groove 4",
	"Groove 8", "Groove 16", "Free 4", "Free 8",   "Free 16",
};
static_assert(std::size(kGrooveModeNames) == static_cast<size_t>(GrooveMode::kCount));

/// **WHICH GROOVE MODES READ `GrooveAmount` (a depth) AND WHICH READ `FreePhase` (a position)** — the two
/// controls' gates on `GrooveMode`, as bits of its value. Straight reads neither.
inline constexpr uint32_t kGrooveDepthModes =
    (1u << static_cast<uint32_t>(GrooveMode::Swing4)) | (1u << static_cast<uint32_t>(GrooveMode::Swing8))
    | (1u << static_cast<uint32_t>(GrooveMode::Swing16)) | (1u << static_cast<uint32_t>(GrooveMode::Groove4))
    | (1u << static_cast<uint32_t>(GrooveMode::Groove8)) | (1u << static_cast<uint32_t>(GrooveMode::Groove16));
inline constexpr uint32_t kGrooveFreeModes = (1u << static_cast<uint32_t>(GrooveMode::Free4))
                                             | (1u << static_cast<uint32_t>(GrooveMode::Free8))
                                             | (1u << static_cast<uint32_t>(GrooveMode::Free16));

/// WHAT ONE ZONE OF TRAVEL IS WORTH TO A MODULATION OFFSET, as `owltt_types.h` names it.
inline constexpr double kOnePhaseUnit = 1.0;

/// THE COORDINATE'S DECLARED RANGE. Not a bound — by design none of the zoned coordinates is
/// bounded. A zoned control is one infinite line and the range is only what a host needs in order
/// to draw a fader; `offsetsMayLeaveRange` is what says so.
inline constexpr double kCoordMin = -8388608.0;
inline constexpr double kCoordMax = 8388608.0;

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// THE ANCHORED PUSH'S FLOATS (backlog HT), stored in the row straight after the params.
inline constexpr size_t kAnchorFloats = 9; ///< the eight MacroPush terms, then 1 once anchored
inline constexpr size_t kAnchorFirst = static_cast<size_t>(Param::kCount);

/// The device's state, decoded. Mirrors the reference's `StutterConfig` plus the continuous
/// params it kept in the host's ParamManager rather than in the config.
struct Settings {
	// **FOUR OF THE REFERENCE'S SIX, IN ITS ORDER.** `macroConfig` and `macro` stood between `zoneB` and
	// `pWrite` and were deleted 2026-09-19 with `Param::MacroConfig` / `Param::Macro`: the perform
	// circle's centre is a zero push now, so there is no knob pair for the blend to fall back to. The
	// macro still exists on this device — four times over, one per waypoint (`cornerMacro` below) — and
	// the only thing that went is the fifth, central one.
	float zoneA = 0.0f;
	float zoneB = 0.0f;
	/// **0 FREEZES THE BUFFER**, which is the reference's default (`pWriteParam{0}`) and is a real
	/// choice rather than an inert one: a looper that overwrites as fast as it reads has nothing to
	/// scatter.
	float pWrite = 0.0f;
	/// 1 is full grain playback (`densityParam{50}`). 0 is all dry.
	float density = 1.0f;

	float rate = 0.5f;
	bool enabled = true; ///< bypass, on by default
	bool engage = false; ///< the gesture, off by default
	/// **SHUFFLE, MATCHING THE DECLARATION.** The two defaults have to agree or a device constructed
	/// before the host writes its fields plays a different mode for one block than the one the card
	/// draws — `rowOf(Settings{})` is exactly what `setFields` starts from for anything the host omits.
	Mode mode = Mode::Shuffle;

	bool quantized = true;

	float gamma = 0.0f;

	float pitchScale = 0.0f;   ///< index into kPitchScaleNames, as a normalised control
	float stereoWidth = 0.5f;  ///< the reference's `stereoWidthParam{25}` of 50

	/// **HELD AS THE DECODED QUANTITY, NOT AS THE CONTROL'S INDEX.** Every other field here is the
	/// control's own value because the control's value IS the quantity; these two are ladder rungs,
	/// and a rung number is meaningless to the engine. Decoding once in `stateOf` means `advanceSlice`
	/// and `resolveBufferFrames` cannot each invent their own reading of the ladder — which is the
	/// shape that left `macroConfigNorm` with three different scalings before the EH sweep.
	int32_t seqBars = kSeqLengthBars[static_cast<size_t>(kSeqLengthDefault)];
	double bufferBars = kBufferLengthBars[static_cast<size_t>(kBufferLengthDefault)];

	/// **WHETHER PLAYBACK BELONGS TO THE HOST'S TRANSPORT.** See `Param::TransportLocked` for the
	/// decision and for why the default is the OFF side rather than a taste.
	bool transportLocked = false;

	/// Seconds, decoded from the control's log. Zero means off — see `Param::MinEnv`.
	float minEnvSeconds = 0.0f;
	/// 0 is the input untouched, 1 is the device taking over. See `Param::Mix`.
	float mix = 1.0f;

	/// **[-1, +1]: +1 sends every slice to the delay, -1 sends none, 0 leaves phi alone.** See
	/// `Param::SendOverride` for the decision and `slice_plan.h`'s `overriddenProbability` for the
	/// arithmetic. **NOT CLAMPED HERE** — `overriddenProbability` owns that, at the one place that reads
	/// it, for the same reason `perfX`/`perfY` are left to `performWeightsOf`.
	float sendOverride = 0.0f;
	/// **INPUT GAIN IN dB — backlog KZ**, clamped to its declared −24..+24 by `stateOf` (a modulated value can arrive
	/// outside it). 0 is the identity: the capture is skipped over, not multiplied by one. See `Param::InputGain`.
	float inputGainDb = 0.0f;
	/// **FREEZE — backlog KX.** True: the ring takes no writes. See `Param::Freeze`.
	bool freeze = false;
	/// **RECORD GATE — backlog MJ**, the knob's 0..1 (clamped by `stateOf`). 0 is off. See `Param::RecordGate`.
	float recordGate = 0.0f;

	//----------------------------------------------------------------------------------------------//
	// THE TWO CONTINUOUS WRITERS — backlog GU/GV. `Param::DryWrite` carries the decisions.          //
	//----------------------------------------------------------------------------------------------//

	/// How much fresh INPUT is blended into the ring, per sample, at the linear write head. 0 is the
	/// identity: the buffer is written exactly as it was before this control existed.
	float dryWrite = 0.0f;
	/// How much of the DAMPED WET is blended into the ring, per sample, at the same head. Wet only —
	/// the dry does not go round.
	float feedback = 0.0f;
	/// **FALSE taps after the GRAIN lane, TRUE after the master lane as well.** The one thing not yet
	/// decided, so it is a switch rather than a constant.
	bool feedbackPostMaster = false;
	/// **AUDITION, GRAIN ONLY — `false` is `P Write First`, which is what ships.** See
	/// `Param::GrainOrder`; this field and its control are expected to be deleted once it is decided.
	bool grainPWriteLast = false;

	/// **THE TWO GROOVE CONTROLS' OWN VALUES (backlog IO)** — the mode as the stored float, the amount 0..1.
	/// Held as the controls rather than as the decoded feel so the row round-trips bit for bit; the one
	/// decode is `grooveModeOf` in `groove.h`. Straight, which is the identity, at 0.
	float grooveMode = 0.0f;
	float grooveAmount = 0.0f;
	/// FREE'S POSITION ALONG ITS WALK — unbounded, read only in the Free modes (`Param::FreePhase`).
	float freePhase = 0.0f;

	/// **THE FEEL THE ENGINE PLAYS: the controls decoded, or Straight outside `kGrooveModes`.** The mode
	/// gate is restated here rather than trusted to the card for `transportLockActive`'s reason — the
	/// honesty rule says verify by reading the engine, so the engine holds the condition the card draws.
	[[nodiscard]] GrooveFeel grooveFeel() const {
		if ((kGrooveModes & kModeBit(mode)) == 0u) {
			return {};
		}
		const GrooveMode feel = grooveModeOf(static_cast<double>(grooveMode));
		// FREE READS ITS OWN UNBOUNDED PHASE; Swing and Groove their 0..1 depth, clamped to what it means.
		if (isFree(feel)) {
			return { feel, std::isfinite(freePhase) ? static_cast<double>(freePhase) : 0.0 };
		}
		const double amount = std::isfinite(grooveAmount) ? std::clamp(static_cast<double>(grooveAmount), 0.0, 1.0)
		                                                  : 0.0;
		return { feel, amount };
	}

	/// **WHETHER THE TWO CONTINUOUS WRITERS ACT AT ALL, ASKED IN ONE PLACE.** `kWriterModes` carries
	/// the measurement and the reasons; this is the engine's road to the same fact, so the gate and
	/// the descriptors cannot drift apart.
	[[nodiscard]] constexpr bool hasFeedbackCircuit() const noexcept {
		return (kWriterModes & kModeBit(mode)) != 0u;
	}
	/// Whether either writer is actually asking for anything. **The deferral is only engaged when it
	/// is**, so a patch that never touches these two runs the write path it always ran.
	[[nodiscard]] constexpr bool writersActive() const noexcept {
		return hasFeedbackCircuit() && (dryWrite > 0.0f || feedback > 0.0f);
	}

	//----------------------------------------------------------------------------------------------//
	// THE PERFORM CIRCLE. Indexed by `perform.h`'s `Corner`: N, E, S, W.                            //
	//----------------------------------------------------------------------------------------------//

	/// **A WAYPOINT IS A CONFIGURATION ALONE. `cornerMacro` STOOD HERE AND IS DELETED (2026-09-19)** —
	/// the waypoints need no macro knobs; only the pad controls them. It held {0.9, 0.6, 0.2, 0.6},
	/// which is a spread of HOW FAR, and the puck's radius is how far now.
	///
	/// **WHAT REPLACES IT, AND IT IS A DECISION RATHER THAN A CONSEQUENCE:** each waypoint's
	/// push is built at **macro = 1.0** — the waypoint is its character at FULL strength — and the
	/// weights make the radius the amount (centre 0, half-way 0.5, rim 1.0), which is what they already
	/// did once the centre became a zero push. So the deleted four are not lost, they are the puck's
	/// distance from the middle, and a hand reaches them by dragging rather than by turning a knob.
	///
	/// **WHY THE SUBSTITUTION IS EXACT AND NOT MERELY PLAUSIBLE**, checked in `macro_push.h` rather than
	/// assumed: five of `macroPushOf`'s eight terms are `macro * f(cfg) * K` and the other three are
	/// that too above a `macro > 0.01f` gate, so scaling the push by `w` is scaling the macro by `w` —
	/// `w * (1.0 * f * K)` and `(w * f * K)` are the same number up to the order of two multiplies.
	///
	/// **THE ONE BEHAVIOURAL CHANGE, WHICH IS A SIMPLIFICATION AND IS NAMED RATHER THAN HIDDEN:** the
	/// 0.01 dead-zone gate can no longer bite. It was per waypoint (by design) and every
	/// waypoint is now at 1.0, so `dReverse`, `dPitch` and `dDelay` scale smoothly to zero as the puck
	/// comes in rather than STEPPING where a waypoint's own macro crossed 0.01. The design called that
	/// step "judged inaudible, not measured"; losing it is inaudible by the same token and in the
	/// direction of fewer discontinuities.
	///
	/// **WHAT THE CONFIGURATION IS, ROTATIONALLY** — the part that survives, and the part the circle is
	/// actually made of. It climbs one weather ladder round the rim: N Dew, E Cloud, S Storm, W Night,
	/// the zones 1, 3, 5, 7 named below, so it differs at all four waypoints and no two share a region.
	/// Three of the eight terms (dReverse, dPitch, dDelay) reach their `f` through `triangleFloat`,
	/// which is BIPOLAR (it negates its second half-wave), so for those three the configuration carries
	/// the SIGN — WHICH WAY — while the other five take `triangleSimpleUnipolar` and it sets magnitude
	/// alone. The radius is HOW FAR for all eight.
	///
	/// **NO WAYPOINT HAS DESTINATIONS OF ITS OWN, AND THAT IS WORTH SAYING OUTRIGHT**, because it looks
	/// as though the macros have different destinations. **They do not.** All four push
	/// THE SAME EIGHT TERMS — `MacroPush`'s eight fields, which are the only things a macro reaches in
	/// this device. What a configuration chooses is the DIRECTION in that eight-dimensional space: each
	/// term reads its own phi triangle at the same coordinate, and a triangle is zero over the half of
	/// its period past its duty, so at a given configuration some terms come out zero. A waypoint that
	/// seems to have "different destinations" is one whose coordinate silences a different subset —
	/// which is a property of where it sits on one shared ladder, not of it owning anything. Backlog FP
	/// has the measured table of which terms are live at each of the four defaults.
	///
	/// **AND THEY ARE PROVISIONAL.** The default coordinate is the tuning lever: these four are a spread
	/// chosen so the mechanism is audible, not a claim about which four sounds belong at the rim —
	/// **backlog FP**, which asks whether this arrangement plays well. This comment's job is the other
	/// half of that question: to say what the arrangement IS, so the ear has something to disagree with.
	///
	/// **AND THE STARTING POINT IS MEASURABLY LOPSIDED — FP CARRIES THE TABLE (2026-09-19).** At macro
	/// 1.0 these four produce 7, 2, 4 and 5 live terms of eight, and **E and S have BOTH zone phase
	/// pushes at exactly zero** — the term `slice_plan.h` measures at 71% of the difference between our
	/// plan and the reference's. It is a triangle NULL, not a defect: both pushes are half-rectified at a
	/// 0.5 duty and those two coordinates land past it. The zone centres were chosen to be NAMEABLE, and
	/// a name knows nothing about where the phi triangles are. **Read FP before changing a number here.**
	///
	/// The cfg waypoints are the CENTRES of four of the eight named regions, `(i + 0.5) / 8` for i = 1,
	/// 3, 5, 7 — and those indices are ZERO-BASED, so read against `kMacroConfigZoneNames` below they are
	/// **Dew, Cloud, Storm and Night**: a light touch, gathering, heavy, the whole of it. Naming them is
	/// what makes the numbers checkable by eye and the off-by-one reading impossible; the mapping is
	/// `ZoneSelect::zonesFromOrigin`, `floor(value / (kOnePhaseUnit / kZones))`, so 0.1875 / 0.125 = 1.5
	/// floors to 1. **Four rungs of one weather ladder rather than four unrelated places**, which is what
	/// makes X read as one direction of travel rather than as four settings.
	///
	/// A region's CENTRE rather than its edge, because an edge is the one coordinate where a hair of
	/// drift lands in the neighbouring region.
	std::array<float, 4> cornerCfg {0.1875f, 0.4375f, 0.6875f, 0.9375f};

	/// **THE PUCK, CENTRED AT THE ORIGIN.** -1..+1 on each axis; `performWeightsOf` owns the clamp and
	/// the de-NaN, so nothing else on the road repeats it.
	float perfX = 0.0f;
	float perfY = 0.0f;
	/// **HOW FAR THE CIRCLE PULLS THE MACRO AWAY FROM NOTHING — AND ONE IS THE RIGHT DEFAULT, NOT A
	/// BOLD ONE.** See the design's §2.2: the centre of the circle is neutral (`blendPush` at (0, 0)
	/// returns the centre's push — every term +0.0f but `subdivMix`, which is `kCentreSubdivMix` since
	/// 2026-09-23), so a puck left at the origin is a device with no macro but its resting ratchet at ANY
	/// depth; depth scales the way from that centre, not the centre. Depth 0 would therefore not be the cautious choice — it would ship seven controls
	/// that stay inert however far the puck is dragged, which is the honesty rule failing on a whole
	/// feature rather than on one knob.
	///
	/// **IT IS REDUNDANT WITH THE RADIUS FOR A HAND AND NOT FOR A ROUTE, which is why it survived the
	/// centre going to zero** (decided 2026-09-19). Halving the radius and halving this reach the same
	/// push — but the radius is TWO parameters and a direction, and this is ONE routable scalar, so an
	/// envelope or a macro can fade the whole circle in while the puck stays where a hand put it.
	///
	/// **THIS FIELD IS THE KNOB, NOT THE AMOUNT.** Since 2026-09-19 the travel is curved
	/// (`perform.h`'s `performAmountOf`, `kDepthCurve`), so what the device MULTIPLIES BY is `this^k`.
	/// Held as the control's own value like every other field here — the decode does no arithmetic, the
	/// row round-trips, and the one conversion happens where the push is built. The two are no longer
	/// redundant in the arithmetic sense either: halving the radius halves the push, halving this
	/// divides it by 64.
	float perfDepth = 1.0f;

	/// **THE ANCHORED PUSH — backlog HT** — an anchor button folds the current perform params into the
	/// base patch, keeping the sound but centring the puck. The push the plan took when the
	/// anchor was pressed — the eight `MacroPush` terms in `MacroPush`'s order — then 1 in the ninth float once anchored.
	/// Anchored, the plan takes this plus the puck's departure from the centre (`perform.h`'s `totalPushOf`), so a press
	/// that centres the puck leaves the plan exactly as it was, term for term.
	///
	/// **WHY A STORED PUSH AND NOT `base += push`:** six of the eight terms (the dry vote, the reverse, pitch and delay
	/// probabilities, the gate pull, the subdivision mix) have no base control at all — they exist only as the macro's
	/// push — so there is nothing to add them to. The push is CAPTURED state, so it is row state (`extraState`,
	/// key `perform_anchor`) and persists with the patch. All zeros, the default, is no anchor, and the device then
	/// computes exactly what it did before this existed.
	std::array<float, kAnchorFloats> anchor {};

	//----------------------------------------------------------------------------------------------//
	// PORTED AS-IS FROM THE REFERENCE: pure predicates on the config, no state.                     //
	//----------------------------------------------------------------------------------------------//

	/// Everything but Burst runs on the looper buffer.
	[[nodiscard]] constexpr bool isLooperMode() const noexcept { return mode != Mode::Burst; }

	/// WHETHER THE GESTURE SURVIVES ITS RELEASE: looper modes latch, Burst does not.
	/// **KEPT AS A PREDICATE, THOUGH NOTHING LATCHES ANY MORE.** `test_fx_sklitter_types.cpp` pins it;
	/// `EngageMachine` no longer CONSULTS it: see `engage.h`.
	[[nodiscard]] constexpr bool isLatched() const noexcept { return isLooperMode(); }
};

/// **WHAT DID NOT COME ACROSS, NAMED SO IT READS AS A DECISION RATHER THAN AN OVERSIGHT.**
///
/// - `useSongStutter` — there is no song to inherit from.
/// - `leakyWriteProb`, `pitchScale` (the uint8 one) — the reference marks both DEPRECATED in favour
///   of `pWriteParam` and `getPitchScale()`, and keeps them only so old hardware presets deserialise.
///   Feathers has no such presets (CLAUDE.md, 2026-08-22).
/// - `activeSource`, `pendingSource`, `isOrphaned`, `checkAndClearInheritConfig`, the cached-param
///   takeover — the reference has ONE Stutterer that tracks contend for; every Feathers device slot owns
///   its own instance, so none of it has anything to mean here.
/// - The `0-50` uint8 ranges — those were encoder detent counts, not meanings. The reference's own
///   getters divide by 50 to recover the number it wanted; the controls above ARE that number.

Settings stateOf(const Row& row);
Row rowOf(const Settings& settings);

inline constexpr const char* kTags[] = {"glitch", "fx"};

/// THE CARD'S FACE, IN GRID ORDER — see `kTraits.faceOrder`.
///
/// **`Macro` STOOD THIRD AND NOTHING TOOK ITS PLACE.** The control is deleted (2026-09-19) and, by
/// decision, nothing replaces it on the card or the window for now. So the face is FIVE cells rather than six, and the perform circle — the seven controls
/// that are what the macro became — is reachable from the EXPANDED PANEL and from nowhere else.
///
/// **THAT IS A HELD DECISION, NOT AN OVERSIGHT.** The planned surface (an expanding perform section with a
/// draggable puck, on the panel and on the card) is Phase B, waiting on the GPU shader spike; until it
/// lands the circle is reached the way every other unpinned control is, through `GenericExpanded`, which
/// draws the COMPLEMENT of this list and therefore cannot lose one. The registry-wide reachability case in
/// `test_gui_widgets_dahdsr.cpp` is what holds that to it.
inline constexpr uint8_t kFace[] = {
	static_cast<uint8_t>(Param::Engage), static_cast<uint8_t>(Param::Rate),
	static_cast<uint8_t>(Param::Density), static_cast<uint8_t>(Param::InputGain),
	static_cast<uint8_t>(Param::RecordGate), static_cast<uint8_t>(Param::Freeze),
	static_cast<uint8_t>(Param::PWrite), static_cast<uint8_t>(Param::Mix),
};

/// **THE PANEL'S SIDE COLUMN — backlog IG** (see `Traits::sideColumn`). Groove's mode and amount (two cells
/// since backlog IO), and the three loop settings
/// that say whose clock and how long: when a slice starts, whether the song owns playback, how many bars the
/// arrangement takes and how many the buffer holds. The column is a holding place until some of the
/// switches are consolidated, so this list is expected to change.
inline constexpr uint8_t kSideColumn[] = {
	// ONE CELL FOR GROOVE since the IO addendum: the mode's zone knob, whose value box is Amount's or Free
	// Phase's (`ParamDesc::modeCell`) — the two are seated inside it, not in this column.
	static_cast<uint8_t>(Param::GrooveMode),
	static_cast<uint8_t>(Param::TransportLocked),
	static_cast<uint8_t>(Param::SeqLength),
	static_cast<uint8_t>(Param::BufferLength),
};

/// Keys this device no longer stores.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "direction", "2026-11-09", "removed with the mode it applied to; no remaining mode reads a direction" },
};

inline constexpr device::Traits kTraits {
	/// THE TAG IS THE NAME. It was "SCAT" while `device::Traits::tag` was a hard four-byte field;
	/// It was widened to `device::kTagBytes` on 2026-09-14 and the abbreviation stopped being
	/// necessary. Still bounded — a tag longer than `kTagBytes` truncates on the wire — so the
	/// static_assert below holds the two together rather than leaving it to be noticed.
	///
	/// **RENAMED FROM "SCATTER" ON 2026-09-17** (do the rename — the tag, the name, the code
	/// and the comments). The reference's feature keeps ITS name: where a comment cites the reference —
	/// `ScatterMode`, `scatterDryMix`, `dsp/scatter.hpp` — it names their code, and those stay. A chain
	/// saved under the old tag no longer resolves and loads as "device missing"; nothing is saved yet.
	/// "SKLITTER" is exactly `kTagBytes` long, which the patch tag's NUL-terminated `kTagBytes + 1`
	/// storage allows (NoteExpr is the other eight).
	.tag = "SKLITTER",
	.name = "SKLITTER",
	.hasCard = true,
	/// THE REST OF THE CONTROLS LIVE BEHIND THE EXPAND BUTTON, drawn by `GenericExpanded` as the
	/// complement of the face below. Not a hand-written panel: a derived one cannot lose a control.
	.hasExpanded = true,
	.perVoiceControl = -1,
	/// THE ANCHORED PUSH (backlog HT), `Settings::anchor`: row state past the params, never a parameter.
	.extraState = kAnchorFloats,
	.extraStateKey = "perform_anchor",
	/// THE BYPASS, NOT THE GESTURE. See the note above.
	.enableControl = static_cast<int>(Param::Enabled),
	/// **SEVEN ON THE FACE, WHICH IS WHAT THE REFERENCE PUTS UNDER THE HANDS** — Zone A, Zone B, Macro
	/// Config, Macro, pWrite, Density and Rate. Everything after them is a menu setting or a
	/// push-and-twist secret over there, and drawing all nineteen flat presented a seven-knob
	/// instrument as a list (didnt the original have more like 8?).
	///
	/// IT WORKED BECAUSE THE ENUM KEPT THE REFERENCE'S ORDER. `faceControls` is a COUNT, so the playing
	/// surface had to be the first controls declared — and there it already was, because the first six
	/// were the order `processStutter` indexes its modulated values by and Rate followed them. That
	/// count is `0` now and the face is a LIST, so the history below is what the field records.
	/// **TEN, NOT SEVEN, AND THE THREE EXTRA ARE THE DIFFERENCE BETWEEN A DEVICE AND AN ORNAMENT.**
	/// Seven was the reference's KNOB count and it hid the two controls that make the thing work: with
	/// `Engage` behind the expand button there is no way to trigger the effect from the card, and with
	/// `Mode` behind it there is no way to choose what it does. Installed that way, the effect could not
	/// be made to audibly play the buffer from the card — it was reachable and not to hand, which
	/// from the player's side is the same thing.
	///
	/// The reference does not need Engage on a knob because it is the ENCODER HOLD — always under the
	/// thumb, never a menu item. A plugin has no encoder, so the gesture has to live on the face.
	/// `Enabled` is here too and costs no cell: the head draws it as the enable square.
	///
	/// **AND THEN THE ZONES LEFT THE CARD, SO THE COUNT COULD NOT SAY IT ANY MORE.**
	/// The zone controls left the card, Mix joined it, and the knobs became a grid instead of a list —
	/// the zones being Zone A, Zone B and Macro Cfg, all three zoned coordinates. They are the first three declared and cannot move, so the face is named as a list; see
	/// `Traits::faceOrder`. They stay one click away on the expanded panel, which draws every control.
	///
	/// **AND THEN `Macro` LEFT THE CARD BY BEING DELETED (2026-09-19), WITH NOTHING PUT IN ITS PLACE** —
	/// deliberately, unreachable from window and card for now. The face is five cells. `kFace`'s own note has the held decision behind that.
	.faceControls = 0,
	.faceOrder = kFace,
	/// **THREE ACROSS**: Engage (a knob-sized pad — make the enable button to be a full
	/// knob-sized piece, meaning Engage, asked), Rate, Density / Input Gain, P Write, Mix — Input Gain (backlog
	/// KZ) took the sixth cell, the one `Macro`'s deletion left, at the head of the row that reads in signal order:
	/// what is recorded, what is written back, what is heard. **`Freeze` (backlog KX) sits between Input Gain and
	/// P Write** — between what is recorded and what is written back, the two roads it stops — so the face is
	/// Engage, Rate, Density / Input Gain, Freeze, P Write / Mix. **`Record Gate` (backlog MJ) follows Input Gain**,
	/// the other half of "what is recorded", so the face is Engage, Rate, Density / Input Gain, Record Gate, Freeze /
	/// P Write, Mix. The
	/// looper-only three hide in Burst and the grid closes up behind them.
	/// **MODE LEFT THE CARD TOO**; it is
	/// on the panel, and the card still hides and shows by it.
	.faceColumns = 3,
	.tags = kTags,
	.sideColumn = kSideColumn,
	.retiredKeys = kRetiredKeys,
	/// **NO SITE: ITS LANES ARE ITS OWN** (D3, kept for this device when it was lifted for the Rack and the Chain, 2026-10-29).
	.siteLanes = device::Traits::SiteLanes::None,
};

/// A stored Mode label this device no longer offers, and the mode it loads as.
inline constexpr device::ChoiceAlias kModeAliases[] = { { "Classic", "Burst" } };

inline constexpr device::ParamDesc kParamDescs[] = {
    // The six modulatable ones, in the reference's order.
    // **UNBOUNDED COORDINATES, LIKE OWLTT'S CHARACTER AND VIBE.** Eight named regions on an infinite
    // line: it keeps turning past the eighth and each lap is the same eight somewhere else in the
    // field. `offsetsMayLeaveRange` and the `zones`/`modSpan` pair are what express that; the huge
    // declared range is a drawing convenience, not a limit.
    {"zone_a", "Zone A", kCoordMin, kCoordMax, 0.0, false, device::Unit::Number, {}, true, true,
     "Structural zone — which slice plays, its offset, its length, whether it skips. What each "
     "region does depends on the mode",
     kZones, kOnePhaseUnit, kZoneAZoneNames, static_cast<int16_t>(Param::Mode), kLooperModes},
    {"zone_b", "Zone B", kCoordMin, kCoordMax, 0.0, false, device::Unit::Number, {}, true, true,
     "Timbral zone — reverse probability, filter, delay send, envelope shape. What each region "
     "does depends on the mode",
     kZones, kOnePhaseUnit, kZoneBZoneNames, static_cast<int16_t>(Param::Mode), kLooperModes},
    // **`Macro Cfg` AND `Macro` STOOD HERE, BETWEEN Zone B AND P Write, AND ARE DELETED (2026-09-19).**
    // we will most likely get rid of the center macro and have it be zero. The perform circle's
    // centre is a constant zero push, so the knob pair it used to blend with has nothing to be; and a
    // configuration configures a macro, so `Macro Cfg` goes with `Macro` rather than surviving alone
    // (`zero × f(cfg) = zero`). **THE MACRO ITSELF IS NOT GONE FROM THE DEVICE** — it is the four
    // `Macro N/E/S/W` pairs below, which is what the circle made of it.
    //
    // **THE ZONED-COORDINATE ARGUMENT THAT LIVED IN THIS COMMENT MOVED TO `Cfg N` BELOW**, because it is
    // the same quantity and the four waypoints are the only things holding it now. It is not restated
    // here: one statement of a rule is the point of moving it.
    {"p_write", "P Write", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "How much fresh audio overwrites the buffer — 0 freezes what was recorded", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), (kLooperModes & ~kModeBit(Mode::Repeat))},
    {"density", "Density", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "How much of the output is grains rather than dry input", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes},

    {"rate", "Rate", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Slice length, relative to tempo — always follows the host clock"},
    {"enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Bypass switch — off silences the device"},
    // **MODULATABLE, BECAUSE THE TOOLTIP PROMISED IT AND THE DECLARATION REFUSED IT.** seems
    // like I cant automate the engage button. It could not, by any road. `DeviceControl::isDestination`
    // reads exactly this flag, so `false` meant no macro could route here; and no device control is a
    // CLAP parameter in either plugin (the rack publishes `kMacroCount` and nothing else), so there
    // was no automation lane either. A control whose own description reads *"Automate it"* and which
    // cannot be automated is the honesty rule failing in the plainest way available.
    //
    // A STEPPED TWO-VALUE DESTINATION IS WELL-DEFINED: a full-depth route moves it by the whole
    // declared range, which is 0 -> 1. It is the only Choice in this device that is a destination,
    // and that is the point — the others name a configuration, this one is a GESTURE, and a gesture
    // is the thing you automate.
    //
    // THE ROAD IS A MACRO: automate a macro in the host, route it here. Device controls being absent
    // from the host's parameter list is a larger question and is written up rather than fixed here.
    {"engage", "Engage", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Starts the stutter. Route a macro here and automate that — this is the encoder hold"},
    // **SHUFFLE IS THE DEFAULT**: the looper the rest of the device is built around. A patch that
    // stored a mode this device no longer has loads as the nearest one (`kModeAliases`).
    {"mode", "Mode", 0.0, 6.0, static_cast<double>(Mode::Shuffle), true, device::Unit::Choice, kModeNames,
     false, false, "What the buffer does to the audio", 0, 0.0, {}, -1, 0, false, nullptr, false, false, false, false, kModeAliases},

    // **IT SNAPS THE PRESS, NOT THE KNOB.** The description said it *"snaps Rate to a musical
    // division"*, and the code snapped on every read — so with it on, turning Rate during a gesture
    // stepped between five values. Now the press is snapped once and the knob then moves freely
    // relative to that snap, so Rate itself is never quantized. Burst only.
    {"quantized", "Quantized", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Burst: begins the capture on a musical division. Rate still moves freely from there", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kGatedModes},

    // **IT IS GAMMA. IT WAS CALLED "EVOLVE PHASE" TO GET PAST THE GUARD THAT SAYS SO.**
    //
    // The note that stood here argued this was a bounded 0..1 knob and therefore not a member of the
    // family, and recorded the registry-wide audit firing on the name as a reason to change the name.
    // Every clause of that was wrong. The reference's `gammaPhase` is floored at zero and OPEN
    // UPWARD (`gamma = max(0, gamma + offset * 0.1)`), `PhiTriContext` holds it in a `double`
    // *"for precision at large values"* because it is expected to run large, and `wrapPhase` reads it
    // as a LAP COUNT — which is exactly what the shaper's and automod's gammas are. so you
    // violated the codebase norm of gamma being a value box... and labeled gamma.
    //
    // SO IT IS DECLARED THE WAY THE FAMILY IS: a symmetric drag window, `Unit::Number`, default 0,
    // not a modulation destination. Symmetric rather than the reference's floor because, as
    // `shaper_types.h` puts it, *"declared 0..N... the very first backwards drag would hit a wall at
    // the place every patch starts"* — and the value is wrapped, so below zero is not a special
    // place. Its seating (`GammaBox`) owns the sensitivity at sixteen laps per drag, so the width
    // costs no resolution; the reference's 0.1-per-detent was that encoder's answer to the same
    // question.
    //
    // AND THE DESCRIPTION SAYS WHAT IT DOES. The old one said it *"scales how fast the phi evolution
    // moves"*; it scales nothing and has no rate. `ph = wrapPhase(gammaPhase * phiFreq)` is ADDED to
    // the moving term — one number that displaces every field by a different amount, because each
    // field carries its own phi multiplier. The pattern recombines rather than sliding.
    {"gamma", "Gamma", kCoordMin, kCoordMax, 0.0, false, device::Unit::Number, {}, false, false,
     "Re-seeds the whole pattern — one lap displaces every phi field by a different amount. "
     "On hardware this is the hidden push-and-twist control", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes},

    {"pitch_scale", "Pitch Scale", 0.0, 24.0, 0.0, true, device::Unit::Choice, kPitchScaleNames, false, false,
     "Which scale, or which fixed shift, Pitch mode snaps to", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kModeBit(Mode::Pitch)},
    {"stereo_width", "Stereo Width", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Grain mode only — 0 centres both voices, 1 sends them hard left and right", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kModeBit(Mode::Grain)},

    // **BOTH ARE `modulatable = false`** — the `true` in that slot on every row above is what makes a
    // control a modulation destination, and the buffer is deliberately not one. Sequence length is refused for the same reason and a second one: it changes what
    // the picture MEANS, and a band whose bar count is being swept by an LFO is not a picture.
    {"seq_length", "Seq Length", 0.0, 3.0, kSeqLengthDefault, true, device::Unit::Choice, kSeqLengthNames, false,
     false, "How many bars the arrangement takes to repeat", 0, 0.0, {},
     static_cast<int16_t>(Param::Mode), kLooperModes},
    {"buffer_length", "Buffer Length", 0.0, 4.0, kBufferLengthDefault, true, device::Unit::Choice, kBufferLengthNames,
     false, false,
     "How much audio the looper captures — changing it discards the buffer and records again", 0, 0.0,
     {}, static_cast<int16_t>(Param::Mode), kLooperModes},

    // **THE THIRD OF THE THREE LOOP SETTINGS, AND IT SITS WITH THEM ON PURPOSE.** Seq Length says how
    // many bars the arrangement takes, Buffer Length says how many it holds, and this says whose clock
    // all of that is measured against. A player reading down the panel meets the three together.
    //
    // **NOT MODULATABLE.** It is a setting in exactly the sense `Buffer Length` is (a setting rather
    // than a performance param, and not a mod target) — an LFO on it would start and stop the
    // device at the LFO's rate, which is a gate and not this control.
    {"transport_locked", "Transport Locked", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Playback stops with the host transport and the buffer freezes instead of recording over itself; "
     "off, the effect carries on over a parked transport. Either way the position comes off the song, "
     "so a restart is in phase",
     0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes},

    // **THE BOTTOM OF A `LogSeconds` RANGE MEANS OFF**, which is why the default is the minimum rather
    // than a small number: `device.h` states the convention, a host may print it as "off", and a
    // device may skip the stage entirely — all the same fact. 0.1 ms to 20 ms is the useful span; the
    // floor is shorter than anything that matters, which is exactly what the convention asks for.
    // **THE MODES THE ANTI-CLICK MACHINERY ACTUALLY SERVES, WHICH IS NARROWER THAN THE FIRST VERSION
    // CLAIMED.** The ramp is armed where the crossing gate is armed — in `advanceSlice` — and driven
    // from `playLooper`, so it acts in Repeat, Time, Shuffle, Pattern and Pitch.
    //
    // **GRAIN IS OUT BECAUSE ITS WINDOW ALREADY DOES BOTH ENDS**: `playGrain` multiplies every sample
    // by a triangle that starts and finishes at zero, so there is no edge for a floor to put under.
    // MEASURED before it was gated: 0.198 in Shuffle, EXACTLY ZERO in Grain.
    //
    // **AND CLASSIC AND BURST ARE OUT BECAUSE THE RAMP NEVER RE-ARMS THERE.** `envPos_` is reset in
    // `advanceSlice`, which neither calls — so after the first ramp it sits past the end for ever and
    // the control is INERT in both. The first version declared `kAllModes & ~Grain` and therefore
    // claimed two modes it does nothing in, which is the honesty rule failing quietly in the
    // direction of a promise.
    {"min_env", "Min Env", kMinEnvFloor, kMinEnvCeiling, kMinEnvFloor, false, device::Unit::LogSeconds,
     {}, true, false,
     "A floor under every grain's fade-in and fade-out — the crossing gate opens abruptly when it finds "
     "no crossing, and a length-gated grain stops dead",
     0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes & ~kModeBit(Mode::Grain)},
    {"mix", "Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry against the scattered signal — 100% is the device taking over, which is what a stutter is"},

    // **BIPOLAR, AND `Percent` ALREADY MEANS THAT** — its own declaration reads *"0..1 or -1..1, shown
    // x100"*, so the range is what says which, and the box prints -100%..+100% with nothing new added.
    //
    // **A DESTINATION, WHICH IS MOST OF WHY IT IS ONE CONTROL AND NOT TWO.** EW's third open question
    // was a knob-plus-switch against a single axis, and it was settled as one. A switch is a poor
    // modulation target — a route into it is a ramp through a cliff — where this axis takes an envelope
    // or an LFO and means something the whole way across: the send thickens towards +1 and thins to
    // nothing towards -1. The two-control shape is more honest about the override being a different
    // KIND of thing and gives a hand nothing to play.
    //
    // **IT STEERS TWO DESTINATIONS NOW, AND THAT IS WHAT KEEPS IT ONE CONTROL.** Since backlog EW's
    // grain lane there are two things drawn against this one probability — the internal echo
    // (`SlicePlan::shouldDelay`) and the lane (`shouldSendToLane`) — from different hash seeds, so they
    // fire independently at the same density. One knob, one character: how often phi sends a grain
    // somewhere else.
    //
    // **THE GATE WAS `kLooperModes & ~kModeBit(Mode::Grain)` AND GRAIN IS BACK IN IT.** The carve-out
    // was honest when it was written: `shouldDelay`'s only reader was `advanceSlice`, which `playGrain`
    // never calls, so in Grain the knob would have been a promise the device could not keep. The grain
    // lane gave Grain a reader — `grainPlanFor` builds a full plan at every voice wrap and it always
    // carried this override — so the carve-out became the OPPOSITE defect: a control hidden in a mode
    // where it changes the sound. **`test_fx_sklitter`'s honesty MATRIX caught it as a LIAR before
    // anyone looked**, which is the case working exactly as intended.
    //
    // **CLASSIC AND BURST STAY OUT, AND NOT ON PRINCIPLE.** They have their own renderers and no
    // plan-per-anything to hang a decision on, so there is nothing for this to steer there. They are
    // skipped by decision (2026-09-20), and may eventually leave the plugin. So this is where the engine happens to stand rather than a
    // boundary anybody designed, and `kLooperModes` already means exactly "not those two".
    {"send_override", "Send Override", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     // **THE +100% HALF IS TRUE OF THE LANE AND NOT OF THE ECHO, and saying "every grain" of both
     // undoes a correction this string already carried.** The grain lane's send is BINARY — a chosen
     // grain goes, whole — so "every grain" is exact for it. The ECHO additionally needs
     // `delaySendBits > 0`, which is Zone B's timbral bank and is a LEVEL rather than a gate: MEASURED
     // over 2,001 Zone B positions x 64 slices, at gamma 0 with a centred puck and no lap, **59.07% of
     // the Zone B range cannot send on ANY slice**, so "+100% to the echo" sends nothing there. Move
     // gamma, the lap or the puck off zero and every position opens. The -100% half is absolute for
     // both: a threshold of 0 makes the draw unsatisfiable.
     "Forces the send decision: +100% always, -100% never, 0 leaves the phi probability alone. Every "
     "chosen grain enters the lane; how much reaches the echo is still Zone B's",
     0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes},

    //----------------------------------------------------------------------------------------------//
    // THE TWO CONTINUOUS WRITERS AND THE TAP SWITCH — backlog GU/GV. See `Param::DryWrite`.          //
    //----------------------------------------------------------------------------------------------//
    //
    // **ALL THREE ARE `modulatable = false` AND THAT IS A DECISION RATHER THAN A GAP** — modulation
    // is deferred and may be added later. The slot after the default is what
    // `DeviceControl::isDestination` reads, so `false` here is the whole of the honesty: nothing can
    // route to them, and nothing claims it can.
    //
    // **AND THE DESCRIPTIONS NAME THE MODES, because the gate is narrower than the mask alone would
    // suggest to a reader.** `kWriterModes` is everywhere the device loops EXCEPT Repeat — Shuffle,
    // Time, Pattern, Pitch and, since backlog HU, GRAIN. Repeat is out on `P Write`'s own reason, and
    // Burst is not a looper at all. A player who turns a knob in one of those two sees
    // it hidden by the gate; a player reading the tooltip is told where it acts before they get there.
    //
    // **GRAIN WAS EXCLUDED FOR A DAY ON A CLAIM THAT MEASUREMENT REFUTED** — see `kWriterModes` for
    // the numbers, and for why the guard it was thought to need does not exist and is not missed.
    {"dry_write", "Dry Write", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,
     "Continuously blends the fresh input into the buffer — the clean overwrite, beside P Write's "
     "per-slice draw. Shuffle, Time, Pattern, Pitch and Grain", 0, 0.0, {},
     static_cast<int16_t>(Param::Mode), kWriterModes},
    {"feedback", "Feedback", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,
     "Blends the WET — post-effects and gently damped — back into the buffer, so the device eats "
     "what it just played. The dry does not go round. Shuffle, Time, Pattern, Pitch and Grain", 0, 0.0, {},
     static_cast<int16_t>(Param::Mode), kWriterModes},
    {"fb_tap", "FB Tap", 0.0, 1.0, 0.0, true, device::Unit::Choice, kFeedbackTapNames, false, false,
     "Where Feedback is taken from: after the GRAIN lane only, or after the master lane too — which "
     "puts a master device inside the loop, compounding every pass", 0, 0.0, {},
     static_cast<int16_t>(Param::Mode), kWriterModes},

    // **AN AUDITION CONTROL — IT IS SCAFFOLD AND IS MEANT TO BE DELETED.** `Param::GrainOrder` has the
    // whole argument; the short of it is that no one can choose between the two Grain blend orders
    // from a description, without hearing them, so both are built and the switch is how the choice is made. **When one is picked, the losing state and this row
    // go together** and the surviving order becomes unconditional. Do not grow features against it.
    // **2026-09-22: the default stays for now and the switch is KEPT** — the deletion waits on
    // backlog HZ (revisit by ear).
    //
    // **GRAIN ONLY, because the question only exists there.** Everywhere else the decided order is
    // already what runs — `P Write` is deferred with the other two and lands last — so a switch would
    // offer a choice between one real order and one that is not implemented. The honesty rule decides
    // the gate as much as the design does.
    {"grain_order", "Grain Order", 0.0, 1.0, 0.0, true, device::Unit::Choice, kGrainOrderNames, false, false,
     "AUDITION: which writer lands last in Grain. P Write First is what ships — Feedback blends over "
     "it. P Write Last defers P Write behind Dry and Feedback, matching every other mode", 0, 0.0, {},
     static_cast<int16_t>(Param::Mode), kModeBit(Mode::Grain)},

    // **GROOVE — A MODE AND AN AMOUNT (backlog IO, 2026-09-23; one zoned control from IG until then).**
    // a mode plus phase offset might be the right model. The mode is a list, so it is a
    // `ValueBox` with a menu — every feel on view before one is applied — and, as no Choice on this device
    // is, not a destination. The amount is the destination: a full-depth route moves it across its whole
    // 0..1, and it CANNOT carry the feel into the next one, because the feel is a different number.
    //
    // **The warp is latched per slice** — a slot's length is decided when it starts (`advanceSlice`) — so a
    // modulated amount never moves a boundary under a slice already playing; what a route changes is where
    // the NEXT slice ends, and the bar stays anchored because each slot ends on the current warp's absolute
    // boundary (`slotStartFrame_`).
    //
    // **THE AMOUNT IS A `digitBox`** — fractional percision drag: its readout prints a tenth of a
    // percent (or a phase to a thousandth for Free) and a drag grabbed on any digit moves that place.
    // **GATED ON THE MODE, WHICH IS GATED ON THE SLICE MODES** — in Straight there is nothing for it to be
    // an amount OF, and the honesty rule hides a control that changes no sound. The surfaces follow the
    // chain (`GenericCard::relevant`). DESIGNATED, because `digitBox` is the struct's last field.
    //
    // **AND IT IS DRAWN AS ONE ZONE KNOB (`modeCell`, backlog IO addendum)** — can we move groove
    // back to a normal zone knob look? The mode is the selector under the value, the value is Amount's box
    // or Free Phase's, whichever the mode gates on. Still three params; the look is the panel's.
    {.key = "groove_mode", .name = "Groove Mode",
     .min = 0.0,
     .max = static_cast<double>(kGrooveModeCount - 1),
     .def = 0.0,
     .stepped = true,
     .unit = device::Unit::Choice,
     .names = kGrooveModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "When each slice starts. Swing: every second step late. Groove: every step late by its own "
                "phi-derived amount. Free: a family of phi patterns, the Amount choosing which. On a quarter, "
                "eighth or sixteenth grid",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kGrooveModes,
     .shortName = "Groove",
     .modeCell = true},
    {.key = "groove_amount", .name = "Groove Amount",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = device::Unit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "How much of the Groove Mode: the swing % (50% straight, 66.7% the triplet, 75% the MPC's "
                "heaviest, at full), or the share of the groove template. Drag a digit to move that place",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::GrooveMode),
     .gateMask = kGrooveDepthModes,
     .digitBox = true},
    // **FREE PHASE — ZONE A'S SHAPE, BECAUSE IT IS ZONE A'S KIND OF QUANTITY** (backlog IO, the lead's pick
    // (B) of three): a position on a line with no end, `kCoordMin..kCoordMax` as a drag window,
    // `offsetsMayLeaveRange` so a route carries it past any value, and a `modSpan` so a full-depth route
    // moves a sensible distance rather than the whole window.
    //
    // **THE SPAN IS ONE UNIT (`kOnePhaseUnit`), Zone A's own.** Every step's rate is at least one, so one
    // unit of phase is at least one full rise of every weakest (off-beat) wave: a full-depth LFO walks
    // through a whole family of distinct patterns each cycle, and at the derived slope bound (φ/2 grid steps
    // per unit) never lurches. Half a unit would leave some off-beats never reaching their peak inside one
    // swing; several units would race through patterns faster than a bar can show them.
    //
    // **A ROUTE ON GROOVE AMOUNT DOES NOT CARRY OVER TO FREE PHASE, AND THAT IS RIGHT**: depth and phase are
    // different quantities, and a route that meant "swing harder" has no meaning as "walk further".
    //
    // A `digitBox`, seated before the drag-window road so a grabbed digit moves that place of the printed
    // phase (`12.050`), which is the value itself.
    {.key = "free_phase", .name = "Free Phase",
     .min = kCoordMin,
     .max = kCoordMax,
     .def = 0.0,
     .stepped = false,
     .unit = device::Unit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = true,
     .tooltip = "Where along Free's walk of phi patterns: 0 is straight, every other position a different "
                "pattern, the next position the next pattern, and no end — it never comes back round. Drag a "
                "digit to move that place",
     .zones = 0,
     .modSpan = kOnePhaseUnit,
     .zoneNames = {},
     .gateControl = static_cast<int16_t>(Param::GrooveMode),
     .gateMask = kGrooveFreeModes,
     .digitBox = true},

    //----------------------------------------------------------------------------------------------//
    // THE PERFORM CIRCLE — eleven appended controls, SEVEN since the six macros went on 2026-09-19.   //
    //----------------------------------------------------------------------------------------------//
    //
    // **THE EIGHT WAYPOINT TERMS ARE NOT MODULATABLE, AND THAT IS A DECISION RATHER THAN AN OVERSIGHT.**
    // No individual waypoint is a modulation destination. A waypoint is where
    // the circle's rim IS; moving one under modulation moves the instrument the player is aiming at,
    // so the aim and the target would both be in motion. The PUCK and its DEPTH are the performance,
    // and those two are destinations.
    //
    // **A ZONED COORDINATE THAT IS NOT A DESTINATION IS NEW HERE, AND IT DRAWS CORRECTLY.** Checked
    // rather than assumed: `zones` is read by `GenericCard`/`GenericExpanded` (which seat a
    // `ZoneCoordCell` — a widget that deliberately has no `setModulation` at all), by
    // `Knob::faceZoneSpan` (no zoned param ever gets a `Knob`; the widget sweep asserts it) and by
    // `device::routeSpanOf`. None of them asks whether the control is modulatable, and the roads that
    // DO — `DeviceControl::display()`, which returns early with `routable` false, and
    // `RackEngine::provision`, which stores a negative span so `resolveControl` returns the base — both
    // already answer for a non-destination. So the cell draws its number and its region name, and
    // neither lights nor takes a drop.
    //
    // **THE GATE IS THE DELETED `Macro`'S, COPIED RATHER THAN INVENTED**: the circle blends the macro's
    // own terms, so it means exactly what `Macro` and `Macro Cfg` meant — nothing outside the looper
    // modes.
    //
    // **`Macro N/E/S/W` STOOD HERE AND ARE DELETED (2026-09-19)**, with the four waypoints' pushes built
    // at macro = 1.0 and the puck's radius supplying the amount — see `Settings::cornerCfg` for the
    // decision and for why the substitution is exact. Seven controls remain of the eleven this device
    // appended two days earlier.
    //
    // **UNBOUNDED AND ZONED — AND SINCE THE `Macro Cfg` KNOB WAS DELETED (2026-09-19) THESE FOUR ARE THE
    // ONLY PLACE THE QUANTITY LIVES**, so the argument that used to sit on that row sits here.
    //
    // **A ZONED COORDINATE, AND THE REFERENCE SAYS SO BY ITS BASE CLASS.** `ScatterMacroConfig` is a
    // `ZoneBasedDualParam` — the same base `ScatterZoneA` and `ScatterZoneB` derive from — with eight
    // named regions and the same push-and-twist secret. The port first declared it a plain Percent knob
    // and gave its offset a control of its own, which is the arrangement the zone-phase rule ended.
    //
    // **UNBOUNDED, AND THE LAP IS WHERE THE DELETED PHASE WENT.** The knob was declared 0..1 and the
    // bound argued from the DSP's `std::clamp(macroConfigNorm + offset, 0.0f, 1.0f)`. The zone
    // should not be bounded: that argument answered the wrong question — the clamp is on the
    // SUM, and the sum has a second term. The reference's push-and-twist offset entered it as
    // `macroConfigPhaseOffset * 0.1f` and was open upward (`max(0.0f, phase + offset * 0.1f)`), so the
    // control was never bounded there either; the bound read off was the bound of ONE term.
    //
    // **SO THE WHOLE PART IS THAT OFFSET, WHICH IS WHAT FOLDING IT IN MEANT.** The fraction is the
    // coordinate inside the eight zones and each lap adds a tenth, exactly as the deleted param did per
    // unit. The rule stands unbent (2026-09-09): no zoned coordinate is bounded.
    //
    // WHAT IS STILL TRUE, SAID PLAINLY RATHER THAN QUIETLY DROPPED: the DSP clamps the sum, so around ten
    // laps the effect saturates — as it does in the reference, and as it did there for the same
    // arithmetic. An infinite LINE is not a promise of an infinite EFFECT; the zones make the same trade
    // wherever the hash they feed stops changing.
    //
    // `kOnePhaseUnit`, the same as Zone A and Zone B: the declared range is a drawing convenience rather
    // than a domain, so `(max - min) / zones` would make one zone of modulation a million units wide. One
    // lap is one unit and a lap is eight zones.
    //
    // **AND `offsetsMayLeaveRange` IS TRUE HERE FOR THE SAME REASON, THOUGH NOTHING CAN OFFSET THEM.**
    // These four shipped `false` on the argument that the field is about what an OFFSET may do and no
    // offset can arrive at a non-destination. That argument is right about CONSUMPTION and wrong about
    // what the field SAYS: it asks *"IS [min, max] A KNOB'S TRAVEL RATHER THAN THE DSP'S LIMIT?"*, and
    // `false` answers that ±8388608 is a real limit — about the very quantity the paragraphs above
    // call an infinite line, and that `test_fx_sklitter_types.cpp`'s zone case describes as *"one
    // infinite line with eight named regions on it, which is what offsetsMayLeaveRange plus zones/modSpan
    // express"*. Two declarations of one quantity disagreeing is a defect whether or not today's readers
    // notice, and that argument carries this on its own — it is inert either way today, and no road to
    // making a waypoint a modulation destination is open: the design forbids it.
    {"cfg_1", "Cfg 1", kCoordMin, kCoordMax, 0.1875, false, device::Unit::Number, {}, false, true,
     "The macro's configuration at waypoint 1, the top-left — eight named regions across one pass",
     kZones, kOnePhaseUnit, {}, static_cast<int16_t>(Param::Mode), kLooperModes},
    {"cfg_2", "Cfg 2", kCoordMin, kCoordMax, 0.4375, false, device::Unit::Number, {}, false, true,
     "The macro's configuration at waypoint 2, the top-right — eight named regions across one pass",
     kZones, kOnePhaseUnit, {}, static_cast<int16_t>(Param::Mode), kLooperModes},
    {"cfg_3", "Cfg 3", kCoordMin, kCoordMax, 0.6875, false, device::Unit::Number, {}, false, true,
     "The macro's configuration at waypoint 3, the bottom-right — eight named regions across one pass",
     kZones, kOnePhaseUnit, {}, static_cast<int16_t>(Param::Mode), kLooperModes},
    {"cfg_4", "Cfg 4", kCoordMin, kCoordMax, 0.9375, false, device::Unit::Number, {}, false, true,
     "The macro's configuration at waypoint 4, the bottom-left — eight named regions across one pass",
     kZones, kOnePhaseUnit, {}, static_cast<int16_t>(Param::Mode), kLooperModes},
    // **THE TWO PUCK AXES ARE CENTRED, WHICH IS THE LAST FIELD AND IS DRAWING ONLY.** See
    // `ParamDesc::centred`: the rest of these knobs is the MIDDLE of the range, so the value arc is
    // drawn from the centre out. -1..+1 with the neutral point at 0 is exactly the bipolar case that
    // field was added for; without it the arc would read "half on" for a puck that is doing nothing.
    {"perf_x", "Perf X", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "The perform puck, left to right", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes, true},
    {"perf_y", "Perf Y", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "The perform puck, top to bottom", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes, true},
    // NOT CENTRED: depth's rest is OFF, at the bottom of its travel, and its default of 1 is the top.
    //
    // **IT KEPT ITS NAME AND ITS PLACE WHEN THE CENTRE BECAME ZERO, AND THAT WAS DECIDED RATHER THAN
    // ASSUMED (2026-09-19).** Its meaning changed — "how far the circle pulls away from the KNOBS"
    // became "how far it pulls away from NOTHING" — and with the centre zero it is arithmetically
    // redundant with the puck's RADIUS: halving the radius and halving the depth reach the same push.
    // **IT SURVIVES BECAUSE THE TWO ARE NOT INTERCHANGEABLE TO A ROUTE.** The radius is two parameters
    // and a direction; this is ONE routable scalar, so an envelope or a macro can fade the whole circle
    // in while the puck stays exactly where a hand put it. Redundant for a HAND, not for a ROUTE — and a
    // control that a route needs is not an ornament.
    //
    // **AND ITS TRAVEL IS CURVED, WHILE ITS DECLARATION HERE IS NOT (2026-09-19).** Played, the useful
    // region of a linear strength knob was concentrated in its first 1-10%, so the knob carries a
    // curve. The amount is `depth^kDepthCurve`, applied
    // in `perform.h`'s `performAmountOf` — so the range below stays a plain 0..1 and a DAW's automation
    // lane means what it did. The curve is in how the value is USED, never in what the host sees.
    {"perf_depth", "Perf Depth", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "How far the perform circle pulls the macro away from nothing — route this to fade the whole "
     "circle in without moving the puck", 0, 0.0, {}, static_cast<int16_t>(Param::Mode), kLooperModes},
    // **INPUT GAIN (backlog KZ)** — see `Param::InputGain`. Every mode captures, so it is gated on nothing.
    {.key = "input_gain", .name = "Input Gain", .min = -24.0, .max = 24.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "Gain on what is recorded into the buffer — a quiet or hot source fills it at a usable level. "
                "The dry you hear is untouched"},
    // **FREEZE (backlog KX)** — see `Param::Freeze`. Every mode writes its ring, so it is gated on nothing. A
    // destination like Engage: a gesture, and a full-depth route moves it across 0.5.
    {.key = "freeze", .name = "Freeze", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
     .tooltip = "Holds the buffer as it stands: nothing is recorded or written back while it is on, so the "
                "slices keep cutting the same audio. Off resumes writing"},
    // **RECORD GATE (backlog MJ)** — see `Param::RecordGate`. Every mode records, so it is gated on nothing.
    {.key = "record_gate", .name = "Record Gate", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Only records while the input is above this level, so silence never overwrites the buffer; it "
                "stays open for half a second after the input falls. 0 is off",
     .shortName = "Rec Gate"},
};

/// **HOW LONG THE RECORD GATE STAYS OPEN AFTER THE INPUT FALLS BELOW ITS THRESHOLD — backlog MJ.** By design a
/// hardcoded 500 ms, no knob. Long enough that a decaying note is recorded to its tail rather than chopped where it
/// crosses the line, and the gate does not chatter on a signal hovering at it.
inline constexpr double kRecordGateHoldSeconds = 0.5;

/// **THE KNOB'S 0..1 AS A LINEAR AMPLITUDE: `value³`.** A gate threshold lives between about −60 and −10 dBFS, which
/// on a linear knob is the bottom 3% of its travel; cubed, 0.1 is −60 dBFS, 0.25 is −36, 0.5 is −18 and 1 is 0 dBFS.
/// The curve is in how the value is USED (Perf Depth's precedent), so a DAW's lane still means the stored number.
/// 0 is exactly 0, which `Sklitter::seatFreeze` reads as "off".
[[nodiscard]] constexpr float recordGateThreshold(float value) noexcept {
	return value * value * value;
}

static_assert(std::string_view(kTraits.tag).size() <= device::kTagBytes,
              "a tag longer than the field truncates on the wire, silently, and only shows up as a "
              "patch that will not load");

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount),
              "every control must declare itself, or the host reads past the table");
static_assert(std::size(kPitchScaleNames) == 25,
              "the pitch names index kScaleSemitones, which the reference sizes at 25");

/// **WHAT THE CARD IS SHOWN, AND WHY IT IS THIS AND NOT THE BUFFER.**
///
/// The buffer is rendered as a gradient whose chunks are re-ordered into their shuffled order, with
/// marks for what the other modes do to each.
///
/// **THE PICTURE IS THE ARRANGEMENT, NOT THE AUDIO.** A waveform would say what was recorded; what
/// this device DOES is read that recording out of order, backwards, doubled, shortened or not at all.
/// So each playback step publishes WHERE IN THE BAR it reads from — which a gradient renders directly,
/// because position in the bar is exactly what a gradient encodes — plus what is being done to it.
/// Two floats a step.
///
/// **BUILT INCREMENTALLY, one step at a time, as the bar plays.** Computing every step's plan up front
/// would mean running `planFor` and `scheduleFor` N times in a block; the device already computes
/// exactly this when each slice begins, so the picture costs two stores. It completes over one bar and
/// re-corrects itself whenever a control moves, which is also what the player wants to SEE happen.
enum class Reading : uint8_t {
	Status = 0,  ///< `engage::Status` as a number: 0 off, 1 recording, 2 standby, 3 playing
	/// **HOW MANY STEPS THE WHOLE SEQUENCE IS CUT INTO** — `slicesPerBar * seqBars`, clamped to
	/// `kPictureSteps`. It counted one BAR's steps until the band became the sequence.
	Steps,
	/// WHICH STEP IS PLAYING, **addressed in the sequence rather than in the bar**: the head is
	/// `seqBarIndex() * slicesPerBar + sliceIndex_`, so it sweeps the whole band instead of the first
	/// bar of it over and over.
	Step,
	StepPhase,   ///< 0..1 through the playing step
	Fill,        ///< 0..1 of the capture, while recording
	/// **HOW MANY BARS THE BAND SPANS**, so the surfaces can rule the bar lines. Derivable from
	/// `Steps` only with a slice count the card does not have — which would be a second opinion about
	/// the cut, and the device is the one that knows.
	SeqBars,
	//----------------------------------------------------------------------------------------------//
	// **THE TWO LEVELS THE PERFORM CIRCLE'S SPARKS ARE EMITTED FROM — backlog HR.** The wet output  //
	// and the grain fx output emit two different colours, the grain fx taken out of the master     //
	// level before the sparks.                                                                     //
	//----------------------------------------------------------------------------------------------//
	//
	// **THEY PARTITION THE SOUND, AND THE PARTITION IS EXACT RATHER THAN APPROXIMATE.** That is
	// possible only because **the grain lane is a DIVERSION and not a parallel send** — `renderChunk`
	// says so at length, and it is the fact this pair of readings rests on. A chosen grain LEAVES the
	// wet and the lane's output is added back in its place, so at the instant the two meet:
	//
	//     wetL_  == everything that did NOT take the lane
	//     sendL_ == the lane's output, which is everything that DID
	//
	// and their sum is the whole. **So "wet minus grain fx" is not a subtraction at all: it is the wet
	// READ ONE LINE EARLIER**, which is exact where subtracting afterwards would leak a float's worth
	// of error into a partition whose whole value is that it does not.
	//
	// **WHY IT HAD TO BE A PARTITION** (by design, and the honesty rule is why it matters): the
	// lane rejoins the wet, so a wet read after the rejoin ALREADY CONTAINS the lane. Two colours
	// claiming to show two things while one contains the other is a picture asserting something false —
	// and the whole point of the second colour is that you can SEE how much of the sound took the lane,
	// which is otherwise invisible.
	//
	// **BOTH ARE SCALED BY THE PER-SAMPLE MIX**, so a device at Mix 0 reads zero on both while a loud
	// track walks straight through it. That is the same honesty choice as the level itself: the sparks
	// claim *this instrument is doing that*, and a meter on the OUTPUT would spark for a sound the
	// device had no hand in. It is also what lets the pad go perfectly still at rest, which
	// `perform_pad.h` requires.
	//
	// **WHAT THEY DELIBERATELY DO NOT ACCOUNT FOR: THE MASTER LANE.** It runs on the SUM, after this
	// point, so separating its effect on the two halves would mean running it twice. The two numbers
	// are therefore the partition **where the two roads meet**, scaled by the mix that will be applied
	// — not a prediction of what each contributes to the final output. A master lane with a gate or a
	// gain in it will make the picture optimistic by however much it moves, and that is stated here
	// rather than left for a reader to discover.
	//
	// **HELD, NOT DECAYED, AND FOR BUS UTILITY'S REASON** (`busutil_meter.h`'s `RawPeak`): a value
	// published per block is overwritten several times between two repaints, so a surface reading it
	// at 60 fps would draw whichever block happened to land last and would never see a transient in any
	// other. Both rise INSTANTLY and may only fall at a window boundary `kOutLevelHoldSeconds` wide.
	// **The two share one window**, so their ratio is a ratio of two numbers measured over the same
	// interval — two independent windows would make "how much went each way" a comparison across time.

	/// Linear sample peak of what did NOT take the grain lane, times the mix. 0..1+.
	WetLevel,
	/// Linear sample peak of the grain lane's OUTPUT, times the mix — what it produced, not what was
	/// diverted into it (the grain fx output). 0..1+.
	GrainLevel,
	/// **RATE AS THE NOTE VALUE PLAYING: the N of "1/N"** — backlog GS, `rateDivisionFor`. A length in
	/// Burst, from the EFFECTIVE position while a gesture is live (a quantized press moves it
	/// off the knob, and only the device knows by how much); a slice count in a looper mode. The card's
	/// Rate readout prints it through `kTextOps`.
	RateDivision,
	/// **THE GROOVE THE SLICES ARE CUT ON — its MODE, as `GrooveMode`'s number, or 0 (Straight) where it
	/// does not act** (backlog IG; the amount beside it since backlog IO). The band draws each step's edge at
	/// `grooveWarp` of its straight phase, so the swung grid is visible; published rather than read off the
	/// field so a mode where Groove is gated off draws the straight grid it actually plays — the device
	/// knows, and says, as it does for `SeqBars`. **The amount is the RESOLVED one**, so a routed groove
	/// draws where it is.
	Groove,
	GrooveAmount,
	//----------------------------------------------------------------------------------------------//
	// **GRAIN ONSETS — backlog HR round 2.** consider emitting larger sparks when grains //
	// fire, color these based on grain sampled gates.                                                //
	//----------------------------------------------------------------------------------------------//
	//
	// **AN ONSET IS AN EVENT, AND EVERY OTHER READING HERE IS A LEVEL.** A surface reading "the current
	// value" at 60 Hz drops most events that happen between two reads. HR's draft proposed a ring of onset
	// records for that; **these are COUNTERS instead, and a counter cannot drop an event**: the surface
	// differences two reads and gets exactly how many grains fired in between, of each kind, however many
	// blocks' publishes it slept through. No new road across the boundary, no capacity, no overflow policy
	// — the readings channel already carries floats lock-free and a monotone count survives being read
	// late. What it gives up is WHEN inside the interval each one fired, which a 60 Hz picture cannot show
	// anyway.
	//
	// **ONE COUNTER PER GATE CLASS, `grainOnsetClass(dry, reversed)`** — HR's recommendation: the two
	// sampled gates a picture can carry legibly, `useDry` (with the macro's and the density draw's dry,
	// i.e. the gate as the grain actually plays it) and `shouldReverse`, and nothing else. Four classes,
	// so four counters; the joint cannot be rebuilt from one counter per flag.
	//
	// **Counted mod `kGrainOnsetWrap` (2^24)**, the largest range a float carries exactly; the surface
	// differences mod the same. Never reset — not even by `reset()` — because a counter that went
	// backwards would read as sixteen million grains; the surface treats a backwards count as a reset
	// and re-baselines, and caps a forward one (`PerformPad::kMaxOnsetsPerTick`).
	GrainOnsets,
	kGrainOnsetsLast = GrainOnsets + 3,
	/// **HOW MANY TIMES REPEAT HAS RESTARTED ITS SLICE, mod `kGrainOnsetWrap`** — a counter, for
	/// `GrainOnsets`' reason (repeat slices get the split spark, splitting roughly every repeat).
	/// Counted where Repeat's slot ends, which is where the repeated
	/// slice starts again. Zero movement in every other mode.
	RepeatCount,
	/// **THE REPEAT'S PERIOD, SECONDS** — the slot that last restarted, over the sample rate. What the
	/// perform circle paces a repeat spark's split by, so its child parts about when the next repeat
	/// fires. A level, not an event: the last period stands until the next restart replaces it.
	RepeatPeriod,
	/// **HOW LONG ONE REPEAT READS, AS A FRACTION OF THE BUFFER — Repeat's face**: the ribbon draws a
	/// bar split horizontally, the bottom being the repeating duration with the subdivisions on top. On the scale the steps' sources are on, so a
	/// step's span is `[source, source + RepeatLength)`. **Zero in every other mode**, which is how the
	/// surfaces choose the chunk face — the device knows its mode, and says, as it does for `Groove`. At
	/// rest it is what the settings imply; engaged, what the playing slot holds (backlog IB's rule).
	RepeatLength,
	/// **THE RECORD LIGHT — backlog MJ.** 1 when the buffer took a write during the last block (a sample landed at a
	/// write gain above 0: the capture, the pre-warm, Dry Write, Feedback or P Write), 0 when nothing did — counted
	/// by `Ring` where the write lands, so the light is the write gate's own answer and not a guess from the
	/// settings. Shut by the Record Gate, Freeze or a locked stop alike; and off in Burst at rest, where
	/// nothing records.
	Recording,
	kHeaderCount
};

/// HOW MANY GATE CLASSES A GRAIN ONSET IS COUNTED IN — `Reading::GrainOnsets` onward. See there.
inline constexpr size_t kGrainOnsetClasses = 4;
static_assert(static_cast<size_t>(Reading::kGrainOnsetsLast) - static_cast<size_t>(Reading::GrainOnsets) + 1
                  == kGrainOnsetClasses,
              "one reading per onset class");
/// THE COUNTERS WRAP HERE: 2^24, the last integer a `float` holds with every integer below it exact.
inline constexpr uint32_t kGrainOnsetWrap = 1u << 24u;
/// **WHICH COUNTER A GRAIN'S ONSET GOES IN.** Bit 0 is the DRY gate, bit 1 the REVERSE gate. The one
/// place the packing is spelled, for the device that writes it and the surface that reads it.
[[nodiscard]] constexpr size_t grainOnsetClass(bool dry, bool reversed) noexcept {
	return (dry ? 1u : 0u) | (reversed ? 2u : 0u);
}
[[nodiscard]] constexpr bool grainOnsetClassIsDry(size_t cls) noexcept { return (cls & 1u) != 0u; }
[[nodiscard]] constexpr bool grainOnsetClassIsReversed(size_t cls) noexcept { return (cls & 2u) != 0u; }

/// **HOW LONG `Reading::OutLevel` IS HELD BEFORE IT MAY FALL.** 50 ms — three frames at 60 fps, so a
/// surface cannot miss a transient between two draws, and short enough that the number still reads as
/// live. `busutil_meter.h`'s `kRawPeakHoldSeconds` is the same number for the same reason; they are
/// two devices' own constants rather than one shared, because either may be tuned without the other.
inline constexpr double kOutLevelHoldSeconds = 0.05;

/// HOW MANY STEPS THE PICTURE CAN HOLD. The slice ladder tops out well inside this; a bar cut finer
/// than the eye can read is not a picture worth drawing, so the extra steps are simply not published
/// and the card draws what it is given.
///
/// **SIXTY-FOUR, BECAUSE THE PICTURE IS THE WHOLE SEQUENCE NOW AND NOT ONE BAR.** On the
/// installed build the playhead overwrote the effected display bar as it passed, because the
/// playhead spanned only one bar while the arrangement can have a four-bar period. That was MEASURED before it was changed: up to TEN OF SIXTEEN
/// slots changed from one bar to the next, because a slot was indexed by position within a BAR and
/// rewritten as the head reached it, while the arrangement evolves over four. What the player was
/// looking at was never a pattern — it was a seam walking across the band.
///
/// Four bars at sixteen slices is sixty-four, which is the ladder's top rung crossed with the
/// sequence's. Finer cuts still overflow and are still simply not published, exactly as before.
///
/// **CRAM RATHER THAN PAGE, BY DECISION** — pages were the first idea, but with a varying sequence
/// length cramming expresses the length more naturally. A two-bar sequence is then visibly half the chunks of a four-bar
/// one, which a paged view would have hidden behind a page count.
inline constexpr size_t kPictureSteps = 64;

/// **PER STEP: where it reads from, what is done to it, and how long its ratchet's piece is** — three
/// floats (backlog IX). The third is a LENGTH, the one per-step quantity a bitfield cannot carry: the
/// piece a ratchet restarts is the slice's audible length over the count, floored at 24 ms of the host's
/// frames, so only the device can say what fraction of the cell it is.
inline constexpr size_t kStepStride = 3;
/// Where step `k`'s readings begin: source, then flags, then `StepRatchet`.
[[nodiscard]] constexpr size_t stepReading(size_t k) {
	return static_cast<size_t>(Reading::kHeaderCount) + kStepStride * k;
}
inline constexpr size_t kReadingCount = stepReading(kPictureSteps);

/// **THE MODIFIERS, PACKED INTO ONE FLOAT.** A step's flags are a small set of booleans and the sink
/// carries floats; a bitfield read back with `std::lround` is exact for every value these take, and
/// four more reading slots per step would be four more things to keep in step.
///
/// **AND TWO SMALL COUNTS (backlog IX), which are as exact as the booleans:** the ratchet's count and the
/// long grain's span, each four bits holding `count − 1`, so a step with neither reads 0 there and every
/// flag word published before them decodes unchanged.
enum class StepFlag : uint32_t {
	Reversed = 1u << 0u,
	PitchedUp = 1u << 1u,
	Dry = 1u << 2u,   ///< the step passes the input rather than the buffer
	Known = 1u << 3u, ///< this step has played at least once, so its entry means something
	/// **THE STEP DOES NOT PLAY: a long grain that began earlier is still sounding over it.** The head
	/// steps `sliceIndex_ += grainLength`, so the steps a long grain spans never start a slice of their own.
	Covered = 1u << 13u,
};
/// Bits 4–7: the ratchet's count minus one — `SlicePlan::subdivisions`, 1..12.
inline constexpr uint32_t kStepRatchetShift = 4u;
/// Bits 8–12: the long grain's span in steps minus one — `SliceSchedule::grainLength`, 1..32. FIVE bits
/// since Time's override (2026-09-23): its Zone A combines up to the bar's whole slice count, which is 32
/// at the finest rate, where the hashed long grain stops at 8.
inline constexpr uint32_t kStepSpanShift = 8u;
inline constexpr uint32_t kStepSpanMask = 0x1Fu;
inline constexpr uint32_t kStepCountMask = 0xFu;

/// **THE AUX OUTPUTS — FOR THE FX RACK (backlog EW, and it is now TWO lanes rather than one: a GRAIN lane
/// the per-slice decision feeds grain by grain, and a MASTER lane over the summed output), NOT read by
/// anything inside SKLITTER.** The macro's road to the plan is unchanged and internal; these
/// exist so the gesture that shapes the plan can also play the effects the grains land in.
///
/// **THE WEIGHTS ARE PUBLISHED AFTER NORMALISATION**, so what a patch reads is what the blend used rather
/// than a second computation that can fall out of step — the reason for publishing the
/// weights at all instead of publishing x and y and letting each reader do the arithmetic.
///
/// **SEVEN WAS `device::kMaxDeviceOutputs` EXACTLY** until 2026-11-07, when outputs became sized per device and the
/// constant a bound of 64; an eighth output now costs only SKLITTER's own
/// storage. `src/plugin/device_registry.h`'s `widestDeviceOutputs()` static_assert is what says so at
/// compile time (the path is spelled because this header is in `src/fx/sklitter/`, and a bare
/// `device_registry.h` reads like a sibling).
///
/// **THE FOUR WEIGHTS ARE NOT `Perf`-PREFIXED, AND THAT IS A DECISION RATHER THAN AN OVERSIGHT**: a name
/// here is read inside this device's own list of seven, where `Weight 1` is unambiguous, and the surfaces
/// that draw outputs draw them as short chips on a card — so four characters saved is four characters of
/// label that fits. `SourceDesc::name`'s own rule ("qualified by the device … 'Envelope', not 'Out'") is
/// about not assuming what ELSE is in the list; `Weight 1` satisfies it.
///
/// **THE BYPASS TAKES THE OUTPUTS WITH IT — BY DECISION, over a per-output survival switch.**
/// `kTraits.enableControl` names `Param::Enabled`, and `RackEngine::advanceModulators` publishes the REST
/// value for a device whose enable is down — uniformly, for every device that has ever had an output. A
/// dual device is the first place that rule has an audible second meaning ("the stutter is off, so is the
/// gesture"), and it stays: one switch, one meaning, and no carve-out in a rule that is the same
/// everywhere. A player who wants the puck while the stutter is off routes from something else. The
/// behaviour is pinned by a subcase in `test_rack_engine.cpp`; this is the decision that subcase defends.
inline constexpr device::SourceDesc kOutputs[] = {
	{.key = "perf_x", .name = "Perf X", .bipolar = true},
	{.key = "perf_y", .name = "Perf Y", .bipolar = true},
	{.key = "weight_1", .name = "Weight 1"},
	{.key = "weight_2", .name = "Weight 2"},
	{.key = "weight_3", .name = "Weight 3"},
	{.key = "weight_4", .name = "Weight 4"},
	{.key = "perf_speed", .name = "Perf Speed"},
};
inline constexpr size_t kPerformOutputs = std::size(kOutputs);

/// **WHAT EACH INDEX MEANS, SAID ONCE.** `kOutputs` is a table and `Sklitter::performOut_` is an array, and
/// until this enum existed the only thing tying index 3 to `Weight 2` was the order the two were written
/// in — every writer and every case spelled a bare literal.
///
/// **AND THE COUNT ASSERT DOES NOT GUARD THIS, WHICH IS WHY THE NAMES ARE PINNED BELOW.** A
/// `kPerformOutputs == 7` check survives any REORDER of the table: swap two rows and the count is still
/// seven, the assert still passes, and the device publishes the puck under `Weight 1`'s name and
/// `Weight 1`'s `bipolar = false` — a wrong label and a wrong declared range on a source a patch routes
/// from, with nothing red anywhere. So the mapping from ordinal to NAME is asserted directly.
enum class Out : size_t { X, Y, Weight1, Weight2, Weight3, Weight4, Speed };

/// The ordinal of an output, for the array writes and the `outputAt` reads that need one.
[[nodiscard]] constexpr size_t outIndex(Out output) { return static_cast<size_t>(output); }

static_assert(kPerformOutputs == outIndex(Out::Speed) + 1,
              "`Out` and `kOutputs` are one list; a row added to either belongs in both");
static_assert(std::string_view(kOutputs[outIndex(Out::X)].name) == "Perf X");
static_assert(std::string_view(kOutputs[outIndex(Out::Y)].name) == "Perf Y");
static_assert(std::string_view(kOutputs[outIndex(Out::Weight1)].name) == "Weight 1");
static_assert(std::string_view(kOutputs[outIndex(Out::Weight2)].name) == "Weight 2");
static_assert(std::string_view(kOutputs[outIndex(Out::Weight3)].name) == "Weight 3");
static_assert(std::string_view(kOutputs[outIndex(Out::Weight4)].name) == "Weight 4");
static_assert(std::string_view(kOutputs[outIndex(Out::Speed)].name) == "Perf Speed");
/// **THE POLARITY, ALL SEVEN, because the message below is a claim about all seven.** It checked X, Y, N
/// and Speed and asserted "the four weights and the speed do not" — a message promising more than its
/// assert, which is the shape this file keeps catching elsewhere. The name asserts above already pin the
/// order, so this is belt-and-braces; completing it costs one line and makes the sentence true.
static_assert(kOutputs[outIndex(Out::X)].bipolar && kOutputs[outIndex(Out::Y)].bipolar,
              "the puck reaches both ways; the four weights and the speed do not");
static_assert(!kOutputs[outIndex(Out::Weight1)].bipolar && !kOutputs[outIndex(Out::Weight2)].bipolar
                  && !kOutputs[outIndex(Out::Weight3)].bipolar && !kOutputs[outIndex(Out::Weight4)].bipolar
                  && !kOutputs[outIndex(Out::Speed)].bipolar,
              "a weight is a share of one and a speed is a distance; neither has a sign");

/// HOW MANY SAMPLES ONE `kModOps.tick` ADVANCES. It is the device's own declaration (`ModOps::blockSamples`
/// exists so the vocabulary does not have to learn a modulator-tier constant), and `Perf Speed`'s Δt is
/// this over the host's sample rate — so this number is part of the speed's arithmetic and not only its
/// rate.
///
/// **SIXTY-FOUR, WHICH IS THE RATE A NEW PUCK ACTUALLY ARRIVES AT — it was 32, and that was half a rate
/// with nothing to measure on the other half.** `RackEngine::processStereo` cuts the block on
/// `device::kTickWindow` (64) and calls `beginPiece(64, …)`, so `advanceModulators` resolves the row and
/// calls `setFields` ONCE per 64 samples; at 32 the accumulator crossed twice per delivery, and the second
/// tick always saw `puckX_ == lastPuckX_` — a `hypot` and an `exp` for a displacement that was zero by
/// construction. Worse than free: the FIRST of the two divided a whole delivery's displacement by half a
/// delivery's Δt, so its instantaneous reading was 2× the true speed.
///
/// **WHAT THAT MECHANISM ACTUALLY COST, MEASURED — and it is NOT "the output was twice as large".** The
/// EMA sees `2v, 0, 2v, 0, …` and averages them, so the alternating steady state is the solution of that
/// two-tick recurrence with `a = 1 − exp(−1/120)` (Δt/τ at 32 samples and 48 kHz): it oscillates between
/// **0.995833·v and 1.004167·v, a ±0.42% tremor at 750 Hz** — one full cycle per two ticks at 1500 ticks a
/// second — **about a mean of exactly v**. So the reading was RIGHT and jittering: a fixed vibrato on every
/// destination the speed reached, with no hand behind it. At 64 each tick sees `moved/dt = v` and the
/// recurrence converges with no ripple at all.
///
/// **THE UNCHANGED MEAN IS WHAT MAKES THOSE TWO STATEMENTS AGREE RATHER THAN CONTRADICT.** The cases read
/// the same 0.384614 at both tick rates, which is the Δt-based coefficient doing what it claims — the 2×
/// was in the per-tick INPUT, never in the published average. It is also what both existing modulators
/// declare (`mod::kBlockSamples`).
///
/// The time constant is unchanged by definition rather than by luck, and the cases assert that VALUE
/// (`saturate(1.25)`, restated independently) rather than comparing two runs — which is what makes the
/// paragraph above a measurement instead of a claim.
inline constexpr size_t kPerformTickSamples = 64;

/// **THE SHAPE IS DECIDED, THESE TWO NUMBERS ARE GUESSES TO BE SET BY EAR.** τ for the moving
/// average of |Δpuck|/Δt, and the speed at which the normalised reading is 0.5.
inline constexpr double kSpeedTauSeconds = 0.080;
inline constexpr double kSpeedHalfUnitsPerSecond = 2.0; // a diameter a second reads 0.5

extern const device::AudioOps kAudioOps;
extern const device::ModOps kModOps;
/// **THE POST-EFFECTS CHAIN THIS DEVICE HOLDS** — backlog EW, the inner lane on its summed output. A
/// table of one-line forwards into `rack::InnerChain`; every decision it embodies is in that unit,
/// and `sklitter.h` carries the rest of the reasoning.
///
/// **IT IS DEFINED IN `sklitter.cpp`, WHICH IS ABOVE THE RACK, AND THAT IS WHY THIS DEVICE'S TWO
/// TRANSLATION UNITS MOVED OUT OF `feathers-fx`.** A tenant cannot hold its own host: `feathers-rack`
/// links `feathers-fx`. See the `feathers-fx-sklitter` target's note in CMakeLists.txt.
extern const device::ContainerOps kContainerOps;
/// RATE PRINTED AS THE NOTE VALUE IT PLAYS — backlog GS; defined in `sklitter_types.cpp`.
extern const device::TextOps kTextOps;

/// **WHAT THE SEVEN OUTPUTS ARE COMPUTED FROM: THE PUCK, AND NOTHING ELSE** (backlog HS,
/// `device::SelfRouting`). The main controls other than the perform-impacting ones sit
/// downstream of perform, so perform can modulate them.
///
/// READ OFF THE MOD ROAD, NOT GUESSED: `kModOps.setFields` is `Sklitter::setPerformFields`, which reads
/// `Param::PerfX` and `Param::PerfY` and no other field (X, Y and the four weights are one
/// `performWeightsOf(x, y)`); `tickPerform` reads the two members that wrote and their previous values
/// (`Perf Speed`); `performOutput` reads the array those two fill. **`Perf Depth` and the four `Cfg`s ARE
/// NOT HERE, and that is a finding rather than an omission** — they shape the PUSH (`targetPush`, the audio
/// road), and no output reads the push. `Param::Enabled` is refused too, by `mayRouteToSelf` itself: it is
/// `kTraits.enableControl`, which the engine's publish reads.
///
/// **IF THE MOD ROAD EVER READS ANOTHER FIELD, IT GOES HERE IN THE SAME COMMIT** — the host cannot check
/// this list, and a control missing from it is a one-tick feedback loop that nothing refuses.
///
/// **`Perf Depth` IS HERE BY DECISION, NOT BY LOOP** — the outputs may not
/// modulate it. The paragraph above still holds — no output reads Depth, so there is no loop to
/// refuse — but Depth is how far the puck's picture PUSHES the sound, and the perform surface bending
/// its own push is not a road the player is offered. So the list is "what this device's outputs may not
/// reach", of which "what they are made of" is the part a loop forces and Depth the part a decision adds.
inline constexpr uint8_t kFeedsPerformOutputs[] = {
	static_cast<uint8_t>(Param::PerfX),
	static_cast<uint8_t>(Param::PerfY),
	static_cast<uint8_t>(Param::PerfDepth),
};
inline constexpr device::SelfRouting kSelfRouting { .feedsOutputs = kFeedsPerformOutputs };

/// The Descriptor: everything the host needs to know about SKLITTER.
///
/// **THE REGISTRY'S FIRST DEVICE THAT IS BOTH AN AUDIO DEVICE AND A MODULATION SOURCE**, which needed no
/// engine surgery: `rack_engine.cpp` already builds one storage through the FIRST op table in the fixed
/// order audio, mod, notes — so `kModOps`' construct/prepare are never called for this device and must
/// still describe the same class — and `RackEngine::advanceModulators` ticks any slot whose descriptor has
/// `mod` ops without excluding audio chains. Design §4.1.
inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.audio = &kAudioOps,
	.mod = &kModOps,
	// **AND THE REGISTRY'S FIRST DEVICE THAT IS ALSO A CONTAINER WHILE BEING ANYTHING ELSE.** The one
	// container before it, `rack::kDeviceDescriptor`, declares `.container` and `.audio` and nothing
	// more — no outputs, no mod road. This one declares all four, which needed no host surgery either:
	// every site that drives a container tests `container != nullptr && <op> != nullptr` and adds a
	// behaviour, and none of them excludes the mod road or the outputs. `Descriptor`'s own standing
	// rule — the four pointers are independent — is what makes that true rather than lucky.
	.container = &kContainerOps,
	.selfRouting = &kSelfRouting,
	.text = &kTextOps,
	.category = device::Category::RhythmGlitch,
};

} // namespace feathers::fx::sklitter

// **THE ONE-WAY INCLUDE RULE, MADE SELF-ENFORCING RATHER THAN MERELY WRITTEN DOWN.** Three comments in
// this subsystem say `sklitter_types.h` must stay free of `perform.h` — the maths depends on the types,
// never the reverse — and until now nothing checked it. The failure mode a back-edge produces is the
// nastiest kind to read: this header's `#pragma once` makes the re-entry a NO-OP, so `perform.h` would
// carry on with `Settings` not yet declared and the compiler would report a partial-definition error
// deep inside a function, naming neither file's include line.
//
// **THIS MARKER IS DEFINED AT THE END, WHICH IS THE WHOLE TRICK.** It is set only once this header has
// been read to completion, so `perform.h`'s `#error` beside its include fires exactly on the back-edge
// (where the re-entry returns early and the marker is still undefined) and never on the legal path
// (where this header completes before `perform.h` resumes).
#define FEATHERS_SKLITTER_TYPES_H_COMPLETE 1
