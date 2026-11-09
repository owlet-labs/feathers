/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// WHAT ANY DEVICE DECLARES ABOUT ITSELF — the one vocabulary shared by everything this plugin hosts
// rather than implements.
//
// THIS FILE IS fx_tenant.h, PROMOTED. Every word of that file's argument was about DEVICES and none
// of it was about effects: a device owns its DSP, its control declarations, its state payload and
// its card, and includes NOTHING from src/plugin — no ParamId, no ParamSet, no ParamDesc, no widget
// base class. What made it live under src/fx was only that the filter was the first one.
//
// The fx and modulators are intended to be standalone capable, ie not coupled to the params of
// feathers. The filter already half-proved that — its DSP and its declarations are
// portable — and the half it broke is the CARD, which hand-wires its knobs by ParamId and so cannot
// be hosted anywhere the plugin's parameter space does not exist. A modulator written the same way
// would break it identically, and there would then be two of them.
//
// A DEVICE DEFINES ITS OWN INTERFACE. The generic card is the FALLBACK, not the plan.
//
// Devices define their own interfaces rather than the host drawing everything from these descriptors.
// The host may fall back to the generic 'knobs list' if a device does not implement one, but dockable
// devices can define their own card look and expanded view where applicable.
//
// So there are two tiers and the descriptors serve both:
//
//   - A device the host has no specific card for gets a generic one built by walking this list — a
//     knob per row, labelled by name, addressed by index. That is what makes a browser able to add a
//     device nobody wrote a card for, and it is the floor rather than the ceiling.
//
//     THE SELECTION IS STRUCTURAL, NOT A FLAG. This bullet used to say "declines to draw itself
//     (Traits::hasCard false)", and nothing has ever branched on that flag — see Traits below. The
//     lane builds `GenericCard` when it cannot build a specific card, which is a question it can
//     always answer correctly and a flag cannot.
//   - A device that DRAWS ITSELF lays out whatever it likes — the filter's curve, the MSEG's editor
//     — and its card is the better one, because only the device knows that a filter has a response
//     to show and an envelope has a shape.
//
// A DEVICE'S UI IS ENTIRELY ITS OWN, INCLUDING WIDGETS NOBODY HAS WRITTEN YET. Refining the
// above: new dockables define their ui however they want — including new widgets — but there is a
// unified 'knob like' api to unify the modulation surface.
//
// So the shared part is NOT a widget library the device must draw with. It is ONE INTERFACE that any
// widget implements in order to join the modulation surface, and it is deliberately the smallest
// thing that can be: what control am I, what is my value, what is my range, where has modulation put
// me, may a source reach me, what is my route amount, and when does a gesture start and stop. A
// widget answering those draws its own pixels however it likes and still ARMS, GLOWS and ROUTES
// exactly like every other control in the window.
//
// The point of drawing the line there is that the interesting controls are not knobs. An MSEG's
// draggable breakpoint, an XY pad, a curve segment bent by the mouse — none of those is a dial, all
// of them are things a hand should be able to point a modulator at. A contract that said "use our
// knob" would have forced every one of them to be a knob, or forced each one to reinvent the ring,
// the arm state and the amount drag, which is the same divergence arriving by the other road.
//
// SO THE SHARED SURFACE HAS THREE TIERS, AND ONLY THE FIRST IS COMPULSORY. Standard things like knobs
// are also in the shared api for simplicity and opt-in conformity, and there can be opt-in css/color
// conformity with something like parent.arc_glow_color, parent.bg_color, etc.
//
//   1. THE INTERFACE — required, and only of a widget that wants to be a modulation destination. A
//      device with no modulatable controls implements nothing at all.
//   2. STOCK WIDGETS — optional. The host ships a knob (and whatever else earns its place) already
//      implementing tier 1, because most controls ARE dials and nobody should write one twice. A
//      device that takes them gets conformity for free; a device that writes its own loses nothing
//      but the free part.
//   3. THEME ROLES — optional. A device drawing its own widgets can ask the host what colour a
//      modulation arc is, what the card ground is, what a rule is, and match without hard-coding a
//      palette it cannot know. `parent.arc_glow_color`, `parent.bg_color` — CSS custom properties in
//      spirit, inherited from the host rather than declared by the guest.
//
// THE THIRD TIER IS ROLES, NOT COLOURS, and this project already made that distinction and wrote
// down why: src/plugin/ui/theme.h says in as many words that "this file is about ROLES" and maps a
// generated token table onto kSurface, kLine, kInk, kMuted, kAccent, kValue and kModulation. That
// last one IS the arc glow. And the tokens themselves are generated at build time from the `:root`
// block of mock/feathers-ui-mock.html — so the CSS analogy is not an analogy, it is where the
// numbers literally come from. Publishing the ROLE and not the hex is what lets a host restyle
// without every docked device going stale, which is the entire reason CSS custom properties work
// that way too.
//
// NONE OF THE THREE IS WRITTEN YET, and deliberately. The interface belongs in its own header beside
// this one, and it should be designed against its first two real implementors — the editor's
// existing Knob, and whatever the DAHDSR's card wants for bending a curve — rather than guessed at
// from zero. Writing it now, with no implementor, is how an abstraction acquires the methods its
// author imagined instead of the ones its users need.
//
// AND A CONTROL IS ADDRESSED BY ITS INDEX IN THIS LIST, never by a host's identifier. This is the
// one place the existing filter card falls short of the contract — filter_card.cpp names its knobs
// by ParamId, so the card cannot exist anywhere the plugin's parameter space does not, even though
// the filter's DSP and declarations both can. A custom card is not the problem; reaching into the
// host to address its own controls is.
//
// WHERE A CONTROL'S VALUE LIVES IS THE HOST'S BUSINESS, NOT THE DEVICE'S, and that is what lets one
// vocabulary cover both tiers: the plugin seats an fx tenant's controls at clap ids so a
// host can automate them, and carries a modulator's in a tagged state block because CLAP's parameter
// list is static and the rack's contents are not. The device declares the same thing either way and
// never finds out which happened.

namespace feathers::device {

enum class Unit : uint8_t {
	Number,   ///< bare number
	Percent,  ///< 0..1 or -1..1, shown ×100
	LogHertz, ///< the value IS log2(Hz); a frequency knob is even in octaves, never in hertz
	/// The value IS log2(s); a time control is even in octaves for the same reason a frequency one
	/// is — linear-in-seconds spends most of its travel above a second, where nobody needs
	/// resolution, and gives the millisecond end a pixel.
	///
	/// THE MINIMUM OF A LogSeconds RANGE MEANS "NO TIME AT ALL", by convention, because a logarithmic
	/// domain has no zero to mean it with. A host may print it as "off" and a device may skip the
	/// stage entirely; both readings are the same fact. Declare a range whose bottom is shorter than
	/// anything that matters and the convention costs nothing.
	LogSeconds,
	/// **A MULTIPLE OF SOMETHING ELSE THE DEVICE ALREADY KNOWS — `exp2(value)`, printed with an X.**
	///
	/// For the MSEG's Rate. The control scales a window the Base names, so what a hand wants to read is
	/// the RATIO, and "2X" says that where a duration cannot.
	///
	/// **IT REPLACED `LogSeconds` ON A CONTROL THAT WAS PRINTING A LIE.** Rate is `log2` and the window
	/// is `division x exp2(Rate)`, so with a tempo the number is a MULTIPLIER — and the box was
	/// rendering `exp2(Rate)` as seconds. `1 bar` at Rate `+1` read "2.00s" and meant two BARS, four
	/// seconds at 120bpm; `1/4` at the same Rate read "2.00s" too and meant half a second. Two readings
	/// four times apart, printed identically. `mseg_types.h` had named the hazard — *"the same stored
	/// number means '1.5 seconds' with no tempo and '1.5x the division' with one"* — and shipped it.
	///
	/// **THE TRADE, STATED: THIS IS NOW RIGHT WITH A TEMPO AND WRONG WITHOUT ONE**, where the window
	/// genuinely is `Rate` seconds and "2X" is the misleading half. That is the better way round rather
	/// than a fix for both: every host with a timeline is the first case and standalone is the second.
	/// Printing both would need the tempo IN THE EDITOR, and `gui_bridge` publishes none — the honest
	/// fix is a tempo channel, and it is not this one.
	///
	/// **THE CURVE IS THE DEVICE'S, NOT THIS UNIT'S**, which is why this is `exp2` and not a general
	/// remap: a control whose multiplier is some other function of its value cannot use this and should
	/// not pretend to. Automod's Rate is exactly that case — `exp2(2(v-1))` over a 0..2 range — and it
	/// keeps `Percent` until somebody re-ranges it or gives `Knob` a formatter.
	LogMultiple,
	Choice,   ///< index into `names`
	/// **THE VALUE IS A SEMITONE NUMBER AND IT IS SHOWN AS A NOTE NAME.** A root, a key, a transpose
	/// target — anything a player picks by ear rather than by arithmetic.
	///
	/// A GENUINE UNIT, ON `LogHertz`'s TERMS. That one exists because a frequency knob is even in
	/// octaves and nobody reads hertz off a fader; this one exists because nobody picks a root note by
	/// its MIDI number. The alternative considered and rejected was `Choice` with 128 names, which is
	/// wrong twice over: `Choice` means a list of unordered alternatives and a note is a POSITION on a
	/// scale you knob through, and a 128-entry table in one device's header is a thing every future
	/// note-valued control would copy.
	///
	/// **THE CONVENTION IS THE REFERENCE'S, AND IT IS NOT THE ONLY ONE IN USE.** `noteCodeToString`
	/// (the firmware's `util/functions.cpp:1826`) computes `octave = noteCode / 12 - 2`, so
	/// **60 is C3** — the Yamaha reading, against the C4 that Roland and scientific pitch notation use.
	/// Manufacturers genuinely disagree and there is no neutral answer, so this follows the
	/// reference instrument rather than picking one. Sharps rather than flats, which is the reference's default too
	/// (it makes flats a setting; we have no setting and no reason to invent one yet).
	Note,
	/// **THE VALUE IS A GAIN IN DECIBELS, AND 0 IS UNITY.** A trim, an output level, a makeup — any
	/// control whose travel a hand thinks about in dB rather than in a ratio.
	///
	/// ADDED ON THE THIRD INSTANCE, AND THE FIRST TWO SAID IN WRITING THAT THEY WANTED IT.
	/// `fx::shaper`'s Trim carried `Unit::Number` under the comment *"DECIBELS, AS A BARE NUMBER,
	/// because there is no dB unit"*, and `fx::automod`'s follower trim carried the same workaround
	/// with the same apology. Bus Utility's Gain would have been the third copy of that comment.
	///
	/// **APPENDED RATHER THAN SLOTTED BESIDE `Percent` WHERE IT BELONGS BY MEANING**, which costs
	/// nothing to reason about: no `Unit` is serialised anywhere — a device row stores floats and a
	/// tag, never a unit — so the enumerator's numeric value is an implementation detail. Appending
	/// is habit here, and habit is cheaper than re-establishing that fact for every reader.
	///
	/// THE HOST PRINTS IT SIGNED. Unlike `Percent`, which takes its sign from whether the range dips
	/// below zero, a dB reading is a comparison to unity and "6dB" without a sign is ambiguous about
	/// the direction a knob has moved. See `device::formatValue` (`device/format_value.h`).
	Decibel,
	/// **THE VALUE IS A FADER POSITION, 0..1, AND THE GAIN IS ITS SQUARE.** `gain = (2v)^2`, so 0.5 is
	/// UNITY at twelve o'clock, 0 is silence, and 1 is +12 dB. Printed in decibels, because that is
	/// what a hand reads; the value is the position, because that is what a knob has.
	///
	/// **IT EXISTS SO THAT SILENCE IS NOT A SPECIAL CASE.** A dB-valued gain cannot represent -inf, so
	/// reaching silence means bolting one onto the bottom of the range — a discontinuity in the middle
	/// of a control's travel. The knob goes from -inf up through 0 dB at its 12:00 default, with no
	/// nonlinearity bolted on. On an amplitude scale zero is simply the bottom,
	/// and the taper that puts unity at the centre is the one every fader already has.
	///
	/// **THE SQUARE IS A FEEL DECISION AND IT IS DELIBERATELY THE COMMON ONE.** A cube gives more
	/// resolution near unity and drops away faster below; nothing about that can be decided by
	/// reading, so it is the standard choice until a hand disagrees with it.
	///
	/// NOT A SKEW ON THE WIDGET, which was the alternative: `Knob` maps position to value linearly and
	/// has no taper mechanism, and adding one would put a curve into the most shared widget in the
	/// editor for one control. Declaring what the VALUE IS costs an enumerator and leaves every
	/// existing road — drag, modulation, automation, the row codec — exactly as it was.
	Fader,
	/// **THE VALUE IS AN ANGLE IN RADIANS, AND IT IS SHOWN AND TYPED IN DEGREES.** A display unit only: the
	/// stored number stays radians, so declaring it moves nothing a patch holds (backlog "Rotate reads
	/// 3.14": nobody turning a knob thinks in radians).
	///
	/// FOR A REAL ANGLE ONLY — one whose whole turn is the identity (cos/sin of it, a compass). Explore's
	/// Rotate and the Shaper/Phaser/Automod Rotates are NOT angles: they feed `sliceBasisFor`'s phi-spaced
	/// turn rates, which have no period at all, and "360 deg" there would claim one.
	///
	/// Whole signed degrees, " deg" after — the reading Explore's corners already printed by hand.
	Degrees,
};

/// EVERY DIVISION IN THREE FLAVOURS — straight, dotted, triplet. A list of straight divisions
/// alone had no dotteds or triplets.
///
/// GROUPED BY BASE RATHER THAN SORTED BY DURATION, which is a deliberate choice and not the obvious
/// one. Sorted by length the list interleaves — 1/64T, 1/64, 1/64., 1/32T, 1/32 — and a hand looking
/// for "an eighth, but dotted" has to know that a dotted eighth is longer than a quarter triplet to
/// find it. Grouped, the three flavours of a note sit together and the list reads the way a musician
/// names them.
///
/// A DOT IS x1.5 AND A TRIPLET IS x2/3, which is what those words mean: a dotted note is itself plus
/// half again, and three triplets occupy the space of two straight notes. Written as the arithmetic
/// rather than as decimals so the table cannot drift from the definition — `0.09375` is not
/// self-evidently a dotted 1/64 and `0.0625 * 1.5` is.
inline constexpr double kDivisionBeats[] = {
	0.0625, 0.0625 * 1.5, 0.0625 * 2.0 / 3.0, // 1/64
	0.125,  0.125 * 1.5,  0.125 * 2.0 / 3.0,  // 1/32
	0.25,   0.25 * 1.5,   0.25 * 2.0 / 3.0,   // 1/16
	0.5,    0.5 * 1.5,    0.5 * 2.0 / 3.0,    // 1/8
	1.0,    1.0 * 1.5,    1.0 * 2.0 / 3.0,    // 1/4 — one beat
	2.0,    2.0 * 1.5,    2.0 * 2.0 / 3.0,    // 1/2
	4.0,    4.0 * 1.5,    4.0 * 2.0 / 3.0,    // 1 bar
	8.0,    8.0 * 1.5,    8.0 * 2.0 / 3.0,    // 2 bars
	16.0,   16.0 * 1.5,   16.0 * 2.0 / 3.0,   // 4 bars
};
/// THE STANDARD SPELLINGS — a trailing dot for dotted, a trailing T for triplet. Not "dotted 1/8":
/// this is a dropdown a hand scans, and the suffix is what every DAW and every score uses.
inline constexpr const char* kDivisionNames[] = {
	"1/64", "1/64.", "1/64T", "1/32", "1/32.", "1/32T", "1/16", "1/16.", "1/16T",
	"1/8",  "1/8.",  "1/8T",  "1/4",  "1/4.",  "1/4T",  "1/2",  "1/2.",  "1/2T",
	"1 bar", "1 bar.", "1 barT", "2 bar", "2 bar.", "2 barT", "4 bar", "4 bar.", "4 barT",
};
inline constexpr size_t kDivisionCount = std::size(kDivisionBeats);
static_assert(std::size(kDivisionNames) == kDivisionCount);

/// The default: one bar, which is the length a hand assumes when it draws a shape and says nothing.
///
/// IT MOVED FROM 6 TO 18 when each base gained a dotted and a triplet, and the static_assert below is
/// what makes that safe to do: it pins the DEFAULT'S MEANING (four beats) rather than its index, so
/// the table can be reordered again and this line will fail rather than silently defaulting the
/// device to a dotted 1/16.
inline constexpr size_t kDefaultDivision = 18;
static_assert(kDefaultDivision < kDivisionCount);
static_assert(kDivisionBeats[kDefaultDivision] == 4.0);


//------------------------------------------------------------------------------------------------//
// THE WORLD A DEVICE SITS IN, as opposed to what any one device is.                                //
//------------------------------------------------------------------------------------------------//

/// THE HOST'S CLOCK, AS A DEVICE SEES IT — a fact about the world, not a property of the rack.
///
/// NAMED FOR WHAT IT IS RATHER THAN FOR WHO PASSES IT. The first draft called this `RackClock`, which
/// bakes today's only caller into the name of a thing that is about the song. The fx lane will want
/// it the day anything there syncs, and a device in that lane should not have to read "rack" to find
/// out what time it is.
///
/// WHY IT IS HERE AND NOT A FOURTH PARAMETER ON ONE DEVICE. mseg.h says it, about its own tick:
/// "`gate`, `retrigger` and `rateMul` mean exactly what they mean on the DAHDSR... they are the same
/// three because the rack calls both the same way, and A SECOND DEVICE THAT NEEDED A FOURTH WOULD BE
/// TELLING US THE RACK'S CONTRACT WAS WRONG rather than that this device was special." The MSEG needs
/// the host's tempo to know how long its window is; that is not the MSEG being special, it is the
/// rack having had no notion of the world's clock. So the clock joins the contract and every device
/// is handed it.
///
/// THE DAHDSR READS NO TEMPO, AND THAT IS NOT A DEAD CONTROL. The envelope
/// gets no tempo sync, because it has no rate knob and its stages are
/// named absolute durations. A tempo it does not read is not a switch that does nothing; it is the
/// host's clock arriving in a room where one device cares and another does not. (Its
/// Free loop reads the song's SECONDS while the transport plays — song-locked, still no tempo; dahdsr.cpp.)
///
/// IT IS NOT `rateMul`, WHICH WAS THE OBVIOUS PLACE AND IS SPOKEN FOR. dahdsr.h: `rateMul` "exists
/// for drawing rather than for playing — the engine always passes 1", and EnvCard picks it per stage
/// so one run of the real kernel draws a 10 ms attack and a 10 s release in one picture. A window
/// riding in on it would need the card to know the window in order to cancel it out.
///
/// PLAIN AND COPYABLE. Passed by value across the audio thread's tick, so there is nothing to
/// allocate and nothing to own — the no-allocation-after-activate rule reaches this struct too.
struct TransportClock {
	/// How long one beat lasts, or <= 0 when the host has not said.
	///
	/// ABSENCE AS A VALUE RATHER THAN AS A SECOND BOOLEAN, which is the same choice mseg's
	/// `kNoSustain` makes and for the same reason: "sustains at segment 3" and "does not sustain" are
	/// answers to one question, and a bool beside a number is two places for them to disagree.
	double secondsPerBeat = 0.0;

	/// WHETHER THE HOST TOLD US — DERIVED, NOT A SECOND FIELD, and that is a deliberate departure from
	/// the shape suggested for it. A `bool haveTempo` sitting beside the number is two places for one
	/// answer to live, which is the objection this codebase already raises to `kNoSustain` being an
	/// index rather than an index plus a flag: "answers to one question, and a bool beside a number is
	/// two places for them to disagree." Deriving it keeps the intended guarantee — a device cannot
	/// read the tempo without the validity, because the validity is a method on the same object — and
	/// removes the state that could contradict it.
	///
	/// Note this is independent of whether the transport is PLAYING —
	/// CLAP_TRANSPORT_HAS_TEMPO and CLAP_TRANSPORT_IS_PLAYING are separate flags, and a stopped host
	/// still reports its tempo. A sync that only worked while the transport rolled would be wrong for
	/// the commonest case there is: a stopped session with a tempo set.
	constexpr bool haveTempo() const noexcept { return secondsPerBeat > 0.0; }

	//----------------------------------------------------------------------------------------------//
	// WHAT THIS DOES NOT CARRY — WRITTEN DOWN SO THE NEXT DEVICE AUTHOR FINDS THE QUESTION ANSWERED  //
	// RATHER THAN INVENTING A PRIVATE SOLUTION TO IT.                                               //
	//----------------------------------------------------------------------------------------------//
	//
	// Why the fx lane needs this at all: effects without sync would be a big miss — synced lfos and
	// timebased delays and whatnot are absolutely going to need it. So the
	// audience is the LANE and its future tenants, not the one device that happens to be first.
	//
	// **THIS TYPE CARRIES A BEAT LENGTH AND NOTHING ELSE.** That is enough for a large class of
	// things and short of a smaller one, and the line between them is worth stating:
	//
	//   ENOUGH FOR       a time-based delay (N beats is N * secondsPerBeat), a free-running synced
	//                    LFO whose rate is a subdivision, any "how long is a musical unit" question.
	//
	//   NOT ENOUGH FOR   anything that must line up with the SONG. A synced LFO that re-phases on a
	//                    bar line, a delay that flushes at a loop point, a device that should sound
	//                    the same wherever the playhead is dropped — all of those need a POSITION,
	//                    and there is not one here.
	//
	// **AND ONE THAT IS EASY TO MISREAD**: validity is independent of whether the transport is
	// PLAYING. `CLAP_TRANSPORT_HAS_TEMPO` and `CLAP_TRANSPORT_IS_PLAYING` are separate flags, and a
	// stopped host still reports its tempo — see `haveTempo()` above. A device that wants "only while
	// rolling" cannot get it from this and must not infer it from the tempo's absence.
	//
	// **IF A DEVICE NEEDS THE POSITION, EXTEND THIS TYPE RATHER THAN ADDING A SECOND ONE.** It is
	// shared by both lanes now, so one extension serves the rack and the fx chain together, where an
	// fx-only transport would have to be extended twice and could disagree with this one about what
	// time it is. `mod/modulator.h` records the same instinct that put this type here in the first
	// place: it is named for what it IS rather than for who passes it.
	//
	// NOT BUILT SPECULATIVELY. The position is a real gap and nothing needs it today; building it
	// unasked would be declaring a capability with no consumer, which this project has a rule about.

	//----------------------------------------------------------------------------------------------//
	// WHERE THE SONG IS — backlog DQ, and the extension this type's own header asked for.           //
	//----------------------------------------------------------------------------------------------//
	//
	// The note above says a position is *"a real gap and nothing needs it today"* and that a device
	// needing one should **extend this type rather than adding a second**. Two consumers arrived:
	// automod's Free-mode LFO and the rack's `mod::Trigger::Free`, which had never worked. One extension serves both lanes, which is the
	// reason the extension was planned in advance.

	/// NOT A POSITION. Named so a reader meets the convention rather than a bare NaN.
	static constexpr double kNoPosition = std::numeric_limits<double>::quiet_NaN();

	/// WHERE THE PLAYHEAD IS, IN SECONDS OF SONG, or NaN when the host offers no seconds timeline.
	///
	/// **NaN RATHER THAN A SENTINEL, BECAUSE ZERO AND NEGATIVE ARE BOTH REAL POSITIONS.** The tempo
	/// above can use `<= 0` as its absence because no tempo is zero; a position of 0 is the song's
	/// start and a negative one is a host's pre-roll, so neither is available to mean "did not say".
	/// The convention that absence is a VALUE rather than a second boolean is kept — this is the same
	/// choice, made with the only spare value the type has.
	double songPositionSeconds = kNoPosition;

	/// WHERE THE PLAYHEAD IS, IN BEATS, or NaN when the host offers no beats timeline.
	///
	/// **BOTH TIMELINES, NOT ONE CONVERTED INTO THE OTHER, AND THAT IS THE POINT.**
	/// A phase locked to the song must be derived from the timeline its own PERIOD is measured in: a
	/// synced division's period is N beats, so beats keeps it on the grid and survives tempo
	/// automation; a free Hz rate's period is a number of seconds and has nothing to do with tempo, so
	/// deriving it from beats would make a deliberately unmusical rate drift with the tempo anyway.
	///
	/// Converting one into the other at the edge — which `feathers_clap.cpp` did, and had to, when
	/// only seconds were carried — collapses that distinction before any device can use it.
	double songPositionBeats = kNoPosition;

	/// WHETHER THE TRANSPORT IS ROLLING. **A GENUINELY SEPARATE FACT, not a re-encoding of the
	/// position**: a stopped host still reports where it is stopped, which is why this is a bool and
	/// the two above are not. `CLAP_TRANSPORT_IS_PLAYING` is its own flag for the same reason.
	///
	/// A device locking to the song wants it: **a stopped transport is the same position for as long
	/// as the user leaves it there**, so locking to it would freeze an LFO and hand every audition an
	/// identical phase. `Synth::lockedPhaseFor` already settled exactly this for oscillator phase — it
	/// falls back to a free-running counter while stopped — and the answer transfers unchanged.
	bool playing = false;

	[[nodiscard]] bool havePositionSeconds() const noexcept {
		return std::isfinite(songPositionSeconds);
	}
	[[nodiscard]] bool havePositionBeats() const noexcept {
		return std::isfinite(songPositionBeats);
	}

	static constexpr TransportClock fromSecondsPerBeat(double spb) noexcept { return TransportClock { spb }; }
	/// Nothing known. What a standalone build hands over, and a host sending neither timeline.
	static constexpr TransportClock unknown() noexcept { return TransportClock {}; }

	/// **NOT DEFAULTED, AND NaN IS WHY.** A defaulted `operator==` compares the position fields with
	/// `==`, and `NaN != NaN` — so an unknown clock would not equal itself, and `unknownBlock()` would
	/// not equal a freshly built one.
	///
	/// **THE COST IS BIGGER THAN THAT TEST.** This comparison is a CHANGE DETECTOR: `Automod::refresh`
	/// and the mseg window hold a clock and compare it against the incoming one to decide whether to
	/// rebuild. Under a defaulted operator every host that offers no timeline would report a change on
	/// every block, forever — a silent, permanent rebuild storm on the audio thread, and nothing would
	/// have said so out loud.
	///
	/// So absence compares equal to absence, which is what "the host has not said" means when two of
	/// them are asked whether they agree.
	friend bool operator==(const TransportClock& a, const TransportClock& b) noexcept {
		const auto samePosition = [](double x, double y) {
			return (std::isnan(x) && std::isnan(y)) || x == y;
		};
		return a.secondsPerBeat == b.secondsPerBeat && a.playing == b.playing
		       && samePosition(a.songPositionSeconds, b.songPositionSeconds)
		       && samePosition(a.songPositionBeats, b.songPositionBeats);
	}
	friend bool operator!=(const TransportClock& a, const TransportClock& b) noexcept {
		return !(a == b);
	}
};

/// A DEVICE QUALIFIES ITS OWN CONTROL NAMES — "Filter Cutoff", not "Cutoff".
///
/// A device does not get to assume what else is in the list it lands in. The first tenant here named
/// its macros "Macro 1" and "Macro 2"; the plugin's table already had four global rows by those
/// names, and it requires every name in it to be distinct — the collision was caught by the param
/// table's own distinctness case on the first run. A host with a flat parameter list has the same
/// problem and no group column to lean on.
///
/// The device is the only party that knows its own name, so the device is where the qualifying
/// happens. This is a permanent rule, not a workaround for one collision.
///
/// One control of a device, in the device's own terms. `min`/`max`/`def` are the values the device's
/// DSP takes — there is no separate "normalised" domain anywhere in this project, and a second one
/// here would be a second place for a range to be wrong.
/// A NAME FOR ONE REGION OF A ZONED COORDINATE.
///
/// **FOUR DEVICES EACH DECLARED THIS STRUCT, IDENTICALLY** — owltt, shaper, automod, sklitter — and
/// each card then converted its own into the widget's (`owltt_expanded.cpp`'s `zonesFrom`). One type
/// here lets the names travel on the DESCRIPTOR instead, which is what a card that was not written
/// for a particular device needs in order to draw them at all.
///
/// AN EMPTY NAME IS A REAL ANSWER, not a gap: `automod_types.h` states the discipline — *"an
/// evocative name for an unmeasured region is a promise the sound may not keep."* The region still
/// counts and still reads as `zone : phase`.
struct ZoneName {
	const char* name = "";
	const char* description = "";
};

/// **A RANGE THIS WIDE IS A DRAG WINDOW, NOT A DOMAIN — AND NOTHING THAT DECLARES ONE MAY SIT ON A
/// KNOB.** `shaper_types.h` states the idiom for its own three: *"The range is a drag window, not a
/// domain, the same idiom automod's fines and gamma use... A plain `Knob` would have [made it
/// coarser] — `Knob::dragTo` maps one 140px drag across the whole declared range, so a wide range on
/// a knob is only a coarser knob."* At 2^24 that is 120,000 units per pixel.
///
/// **IT IS A DECLARED FACT RATHER THAN A NAME, WHICH IS THE WHOLE REASON IT EXISTS.** The audit that
/// enforces this seating matched the substring "Gamma" in a parameter's name, and I walked straight
/// through it: SKLITTER's gamma was renamed "Evolve Phase" *because* the guard fired, and the rename
/// was written up in that file as though dodging the check were the finding. The codebase norm is that
/// gamma is a value box labeled gamma. A predicate over the
/// declaration cannot be escaped by renaming the thing it describes.
///
/// ZONED COORDINATES ARE WIDE TOO AND ARE NOT THIS: they have their own seating (`ZoneCoordCell`),
/// chosen first, so this answers for what is left — gamma, rotate, and whatever next declares a
/// window instead of a range.
[[nodiscard]] inline constexpr bool isDragWindow(const struct ParamDesc& p) noexcept;

/// **A CHOICE LABEL RENAMED FOR DISPLAY, AND WHAT IT WAS** (backlog JF §3.4). A saved Choice is its
/// LABEL, so a label renamed on the card would orphan every patch holding it; one line here says
/// *"a file saying `from` means `to`"*, and the load finds it. `to` must be a label the control has now.
struct ChoiceAlias {
	const char* from;
	const char* to;
};

/// **A KEY THAT NO LONGER NAMES ANYTHING, AND WHY** (backlog JF §3.3) — protobuf's `reserved`, kept in
/// code. A key whose meaning changed is retired rather than reused: the control takes a NEW key, old
/// patches' values under the old one fall to the new control's default, and the load report says why
/// with `reason`. **A RETIRED KEY IS NEVER REUSED** — `device_registry.h` refuses one that is.
struct RetiredKey {
	const char* key;
	const char* date;
	const char* reason;
	/// **ITS VALUE CARRIED INTO A SUCCESSOR** (e.g. Only w/ Transport becoming Run): the key of the control that now
	/// holds this one's meaning. A patch's value under the retired key is read as that control's — a label through its
	/// `ChoiceAlias`es, a number coerced — wherever the patch names no value for the successor itself. Null: dropped.
	const char* carriedInto = nullptr;
};

/// **IS THIS A KEY A PATCH MAY STORE A VALUE UNDER?** Non-empty, `[a-z0-9_.]` only. Asked at compile time
/// of every control, output and extra-state name in `kDevices` (`device_registry.h`).
constexpr bool validStateKey(const char* key) {
	if (key == nullptr || key[0] == '\0') {
		return false;
	}
	for (size_t i = 0; key[i] != '\0'; ++i) {
		const char c = key[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
			return false;
		}
	}
	return true;
}

/// Two keys, compared as strings — usable in a constant expression, which `strcmp` is not.
constexpr bool sameKey(const char* a, const char* b) {
	if (a == nullptr || b == nullptr) {
		return a == b;
	}
	size_t i = 0;
	for (; a[i] != '\0' && b[i] != '\0'; ++i) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return a[i] == b[i];
}

/// FNV-1a over a key, so `keysDistinct` compares numbers rather than walking strings.
constexpr std::uint64_t keyHash(const char* key) {
	std::uint64_t h = 14695981039346656037ull;
	for (size_t i = 0; key[i] != '\0'; ++i) {
		h = (h ^ static_cast<unsigned char>(key[i])) * 1099511628211ull;
	}
	return h;
}

/// No two of `keys` are the same string. Every key must already be non-null.
///
/// **SORTED BY HASH, NOT COMPARED PAIRWISE, BECAUSE THE PAIRWISE FORM RAN OUT OF CONSTEXPR BUDGET**
/// `everyDeviceKeyed` is ONE constant evaluation over every device, and GCC caps one
/// evaluation at `-fconstexpr-ops-limit` (2^25). With Pattern at 242 fields the O(n²) `sameKey` walk —
/// keys sharing long prefixes like `step.12.gate` — measured between 24M and 33.5M ops at -O2, and `-g`
/// adds enough per statement to cross 2^25: the ASan gate's build (the only `-g` config) failed with a
/// bare "non-constant condition" and no note. Sorting hashes is O(n log n) integer work; `sameKey` runs
/// only inside a run of equal hashes, so a collision cannot hide a duplicate or invent one.
constexpr bool keysDistinct(const std::vector<const char*>& keys) {
	std::vector<std::pair<std::uint64_t, const char*>> hashed;
	hashed.reserve(keys.size());
	for (const char* k : keys) {
		hashed.emplace_back(keyHash(k), k);
	}
	std::sort(hashed.begin(), hashed.end(),
	          [](const auto& a, const auto& b) { return a.first < b.first; });
	for (size_t run = 0; run < hashed.size();) {
		size_t end = run + 1;
		while (end < hashed.size() && hashed[end].first == hashed[run].first) {
			++end;
		}
		for (size_t i = run; i < end; ++i) {
			for (size_t j = i + 1; j < end; ++j) {
				if (sameKey(hashed[i].second, hashed[j].second)) {
					return false;
				}
			}
		}
		run = end;
	}
	return true;
}

struct ParamDesc {
	/// **THE NAME A PATCH STORES THIS CONTROL'S VALUE UNDER — REQUIRED, AND NOT THE DISPLAY NAME** (backlog
	/// JF). `[a-z0-9_.]`, unique within the device, checked at
	/// compile time for every device in `kDevices`.
	///
	/// **A KEY NAMES A MEANING, NOT A KNOB.** Rename the control, move it, regroup it: KEEP THE KEY — a patch
	/// finds its value wherever the control now stands. Change what its number means (units, a rescale, a
	/// switch that becomes a scaler): GIVE IT A NEW KEY and retire the old one (`Traits::retiredKeys`), and
	/// old patches fall to the new control's default with the load report saying why. Never reuse a key.
	///
	/// **FIRST IN THE STRUCT, ON PURPOSE.** Every declaration here is positional, so a key added as the
	/// last field could be forgotten silently; as the first, a positional declaration that forgets it puts
	/// the display name in the key and a `double` in the name, which does not compile. A designated one
	/// that forgets it is caught by `device_registry.h`'s check. Seeded from the original display names,
	/// lowercased with every run of other characters turned to `_`, and frozen from then on.
	const char* key = nullptr;
	const char* name;
	double min;
	double max;
	double def;
	bool stepped; ///< the host should quantise it: choices and switches
	Unit unit;
	std::span<const char* const> names; ///< Choice only

	/// May a modulation source be routed to this? LAST AND DEFAULTED, so a device with no opinion
	/// declares nothing — and so the struct's bools never sit side by side in a positional
	/// initialiser, where `true, false` says nothing about which of them is which.
	///
	/// The plugin still owns the other half: where a destination sits in the route table, and whether
	/// a given source may reach it. The device only says which of its controls are eligible at all —
	/// knowledge the DEVICE has and the host does not, since a device knows which of its controls are
	/// amounts and which ones rebuild its signal path.
	bool modulatable = false;

	/// IS [min, max] A KNOB'S TRAVEL RATHER THAN THE DSP'S LIMIT? Almost always no, and the plugin's
	/// default is the safe one: resolved() clamps base-plus-offsets to the declared range, which is
	/// what makes a modulation offset from a host safe to accept — a resonance above 1 is not a
	/// setting that exists.
	///
	/// A device says yes when its control is a WINDOW onto a wider quantity and the DSP is bounded by
	/// something else. The filter's cutoff is the case: twenty octaves would not fit a knob usefully,
	/// but the corner the filter runs at is bounded by Nyquist, which the filter applies itself. Only
	/// the device can know this, which is why it is declared here and not in the plugin's table.
	///
	/// SAYING YES MEANS resolved() RETURNS THE RAW SUM, so a device that says it must be able to take
	/// any value its own offsets and a full-depth route can produce.
	bool offsetsMayLeaveRange = false;

	/// TOOLTIP TEXT — what this control does, in a sentence a user who does not know the device can
	/// understand. Lives beside `name` in the ParamDesc, so a tooltip can never describe the wrong
	/// control. The device owns this text; the surface that shows it asks. A device that declares no
	/// tooltip shows its name instead — the fallback is the field above, not a separate default.
	///
	/// THIS IS THE SEAM THAT LETS A DEVICE SAY SOMETHING WITHOUT KNOWING WHO IS LISTENING. A tooltip
	/// that is a field of ParamDesc travels with the device into a standalone build for free. A
	/// tooltip that is a method on a widget couples that widget to the surface that shows it.
	const char* tooltip = nullptr;

	/// HOW MANY ZONES THIS CONTROL'S RANGE IS DIVIDED INTO, or 0 for an ordinary continuous control.
	///
	/// A ZONE CONTROL IS A LIST WEARING A RANGE. OWLTT's Character and Vibe sweep 0..1 and the DSP
	/// reads them as eight discrete characters with a position inside each. Declaring the count here
	/// is what lets the rest of the system stop treating the range as a continuum.
	///
	/// WHAT IT CHANGES, AND IT IS ONE RULE WITH TWO READERS — a relative fine
	/// modulation destination: **for a zoned destination a route's amount is stated in
	/// ZONE WIDTHS, not in full ranges.** Amount 1.0 moves the value by one zone. Without this a
	/// route displaces the control across all eight and modulation JUMPS ZONES, which makes a
	/// modulated zone control useless as a zone control.
	///
	/// ENFORCED IN EXACTLY TWO PLACES, AND BOTH ARE REQUIRED:
	///   1. `ParamSet::rackOffsetAt` — the one place a device control's amount becomes an offset,
	///      for both the fx chain and the rack.
	///   2. `Knob::modRing` — the arc drawn on the face.
	///
	/// **THE SECOND IS NOT DECORATION, IT IS THE HONESTY RULE.** `params.cpp` states the invariant
	/// this field bends: the destination's range is the unit an amount is stated in, *"which is what
	/// lets the ring on the face be drawn from the amount without asking what the parameter is."*
	/// Bend it for the offset alone and the ring draws a full-range arc over a one-zone modulation —
	/// a picture that disagrees with the sound. So the field is declared once and read by both, or
	/// not added at all.
	///
	/// A FIELD IS EARNED BY A READER (backlog 16). This one arrives with both of them.
	uint32_t zones = 0;



	/// **HOW FAR A FULL-DEPTH ROUTE MOVES THIS CONTROL, IN ITS OWN UNITS. 0 MEANS "AS BEFORE".**
	///
	/// A SEPARATE FIELD, because `zones` cannot express it. The reader
	/// `ParamSet::rackOffsetAt` takes a span of `(max - min) / zones`, which is right for a bounded
	/// coordinate and wrong for a lapping counter: automod's fine params are declared 0..2^24, so
	/// **"one coarse unit" would need a zone count of 134 million** — not a zone count, and the range
	/// is not a domain either. By design the fine is an unbounded counter and 2^24 is a
	/// drag window.
	///
	/// MEASURED, and this is the number the three automod fines declare: **one zone of `Type` is
	/// 0.125, and the coarse and fine are ALREADY THE SAME SCALE** — `deriveAutomod` with
	/// `type + 0.125` and with `typePhase = 0.125` are bit-identical, because the derive reads the
	/// fine RAW and sums it with the coarse. So one coarse unit is `1 / zones` in the fine's own units.
	///
	/// **ZERO IS THE FALLBACK AND KEEPS EVERY OTHER PARAM BYTE-IDENTICAL**, which is asserted rather
	/// than assumed: a param that declares no span is scaled by `range / zones` exactly as it was.
	double modSpan = 0.0;

	/// THE NAMED REGIONS, when this control is a zoned coordinate. Empty is allowed and common.
	///
	/// **IT LIVES ON THE DECLARATION BECAUSE A STANDALONE DEVICE KEEPS ITS NAMES.** `owltt_types.h`
	/// states the rule for its own table — the names *"never lived"* in the editor — and this is the
	/// road that lets a card which has never heard of the device honour it anyway. Without it a
	/// generic card can size the zones but not name them, which is a coordinate you can turn and
	/// cannot read.
	///
	/// **LAST ON PURPOSE.** Every declaration in this repo initialises `ParamDesc` POSITIONALLY, so a
	/// field inserted mid-struct silently re-points every value after it — which is exactly what the
	/// first draft of this did: `modSpan` landed in `zoneNames` and the compiler caught it only
	/// because the types disagreed. They might not have.
	std::span<const ZoneName> zoneNames {};

	/// **WHICH CONTROL DECIDES WHETHER THIS ONE MEANS ANYTHING, or -1 when it always does.**
	///
	/// Many value boxes are mode specific and should be hidden when they are not used. SKLITTER is the case that forces it — in Classic NINE of its fifteen controls do
	/// nothing at all — but the shape is general and the reference states it per menu item as
	/// `isRelevant()`. The honesty rule already says a control is enabled iff it changes sound; this
	/// is the same rule for a control whose answer depends on another control's value.
	///
	/// **A GATE AND A MASK RATHER THAN A PREDICATE, so it stays a declaration.** A function pointer
	/// would let a device say anything and could not be read, compared or tested without running it.
	/// Every case this repo has is "relevant when control G is one of these values", which a bitmask
	/// over G's stepped values says exactly — and which a card can evaluate by reading one field.
	///
	/// LIMIT, SAID OUT LOUD: 32 values, because the mask is a `uint32_t`. Every gate in this repo is a
	/// mode or a switch with a handful; a gate with more would need a different shape and should not
	/// quietly get a truncated mask, so `relevantFor` asserts it.
	int16_t gateControl = -1;

	/// Bit i set means: this control is relevant while the gate control's value rounds to i.
	uint32_t gateMask = 0;

	/// **IS THE KNOB'S REST THE MIDDLE OF THE RANGE RATHER THAN ITS FLOOR?** A bipolar control — a
	/// macro switched to swing -1..+1 (backlog EG), a curve, a pan — reads as "nothing" at its centre,
	/// so its value arc is drawn from the centre out toward either end rather than from the bottom of
	/// the sweep. Drawing only: the range, the default and the DSP are what `min`/`max`/`def` say.
	/// `Modulatable::restNormalized` is the one place that turns this into a position on the sweep.
	/// LAST, because every device initialises this struct positionally and a field inserted higher
	/// would slide a tooltip into a bool (it did, once, for the length of one build).
	bool centred = false;

	/// **A SHORTER CAPTION FOR WHEN THE CELL IS NARROW, or null to use `name`.**
	///
	/// `name` is required to be device-qualified for the host parameter table and a card already
	/// strips the device word back off it — but `Grain Size` is still nine characters, and in a
	/// four-across grid the cell is about sixty pixels. MEASURED at `faceColumns = 4`: the delay
	/// rendered `GRAIN SIZ` and `FEEDBACI`, clipped mid-word with no ellipsis.
	///
	/// **THE SHORT FORM LEANS ON THE SECTION HEADING ABOVE IT**, which is what makes it short without
	/// being cryptic: under `GRAIN`, `Size` is unambiguous, and under `FEEDBACK`, `Amount` is. That
	/// is the same argument as stripping the device name — do not repeat the context you are already
	/// inside — one level further in.
	///
	/// **APPENDED AT THE END OF THE STRUCT, deliberately.** Every `ParamDesc` in this repo is
	/// initialised POSITIONALLY at some site or other, and a field inserted mid-struct silently
	/// re-points every value after it — it has happened twice (`modSpan` landed in `zoneNames`; a
	/// tooltip slid into a bool). At the end, every existing declaration keeps its meaning and gets
	/// the null default.
	const char* shortName = nullptr;

	/// **DRAW ME IN THE CELL BEFORE ME, UNDER MY NEIGHBOUR, RATHER THAN IN A CELL OF MY OWN.**
	///
	/// A grid cell is sized for a KNOB, and a knob is much taller than a picker or a switch. So a
	/// four-across row holding two small controls wastes most of two cells on air, and a row holding
	/// only small controls is a band of empty card. The delay's Mode and Dub are the
	/// case: they can be double stacked into one knob height.
	///
	/// Set on the SECOND of the pair. It shares the x of the control before it in the FACE order and
	/// sits beneath it, and the two together are one cell for every purpose the layout has — the row
	/// break counts them once, and the row is as tall as the tallest cell including this pair.
	///
	/// **IT IS THE SECOND THAT DECLARES IT, not the first, and not a pair listed somewhere else.**
	/// A separate pairing table would be a second statement of the face order that could disagree
	/// with it; declaring it on the follower means the relationship cannot outlive the ordering that
	/// gives it meaning. A `stacked` control that is FIRST in the face order has nothing to stack
	/// onto and is drawn normally rather than dropped.
	///
	/// Only sensible for controls shorter than a knob cell; two stacked knobs would overflow the row
	/// they are measured into. Nothing enforces that, because nothing can say what a third-party
	/// widget's height will be.
	bool stacked = false;

	/// **SEAT ME AS A VALUE BOX WHOSE EVERY PRINTED DIGIT IS A DRAG GRANULARITY — backlog IO.**
	///
	/// SKLITTER's groove wants fractional-precision drag for finer control. The digit-place drag (backlog BJ, `ui::ValueBox::placeAt`) lives on value boxes, and a
	/// continuous control is seated as a KNOB — so a control that wants it has to say so. Two readers, both
	/// required: `GenericCard` and `GenericExpanded` seat a `ValueBox` instead of a `Knob`, and the box
	/// converts a grabbed digit through the readout's measured slope when the readout is a straight-line
	/// TRANSLATION of the value (`ValueBox::scaledReadoutScale`) — which every other box refuses, because
	/// grabbing the "4" of "42%" on a 0..1 control would otherwise move the value by forty.
	///
	/// Appended LAST for `shortName`'s reason.
	bool digitBox = false;
	/// **DRAW THIS CHOICE AS A ZONE KNOB WHOSE VALUE IS WHICHEVER CONTROL GATED ON IT IS LIVE (backlog IO
	/// addendum).** SKLITTER's Groove wants a normal zone knob
	/// look. The Choice becomes the selector under the value (`‹ Swing 16 ›`), and each control whose
	/// `gateControl` is this one becomes that value's box in the modes that gate it on — so Mode, Amount and
	/// Free Phase stay three params, one road each, and read as one knob. `GenericExpanded` seats it
	/// (`ModeValueCell`); the followers are not seated as cells of their own. Appended LAST for `shortName`'s
	/// reason.
	bool modeCell = false;
	/// **GATED ON A `modeCell` CHOICE, BUT A CELL OF MY OWN.** A control gated on a mode cell is normally that cell's
	/// value in the modes that gate it on (above). One that is not a value of the mode at all — a switch that simply
	/// does nothing in some modes, as Featherverb's Oversample does nothing in Aurora — sets this: the gate still HIDES
	/// it where it is irrelevant, and it is seated as any other control. Appended LAST for `shortName`'s reason.
	bool ownCell = false;
	/// **LABELS THIS CHOICE HAS BEEN RENAMED FROM** (backlog JF §3.4) — a patch stores a Choice by its
	/// label, so a label renamed for the card adds a line here or orphans every saved value. Empty is every
	/// control today. Appended LAST for `shortName`'s reason.
	std::span<const ChoiceAlias> choiceAliases {};
	/// **A CONTINUOUS `Number` READ AS A WHOLE NUMBER.** A stepped Number already prints no decimals (a count);
	/// this is the continuous one whose fraction is real but not worth reading — Resonate's Size is a kernel
	/// length in taps, modulated smoothly, and heard as "64", not "64.00". Display only: the value, its
	/// stepping and its modulation are untouched. One reader, `device::formatValue`. Appended LAST for
	/// `shortName`'s reason.
	bool wholeReadout = false;

	/// **THE DEVICE'S OWN WORDS FOLLOW THIS CONTROL** (`TextOps`): a hand change of it owes the host a text rescan of
	/// the device's slots (`SlotOwner::retextIfWordsFollow`), as a gate already does. The Pattern device's selector
	/// reads "2 · D Dorian": its pattern's Root, Scale and Mode, and every Enable and Solo, change what it says.
	/// Appended LAST for `shortName`'s reason.
	bool retextsWords = false;
	/// **THIS CONTROL TAKES A VALUE PER EAR** (stereo modulation, backlog KU). A stereo route into it resolves a
	/// right ear of its own, which reaches the device through `setFieldsR`; into a control that says false, a pair
	/// is read at its LEFT ear. Only a destination can be stereo — a case pins it. Changes no
	/// stored key, so the schema ledger does not see it. Appended LAST for `shortName`'s reason.
	bool stereo = false;
	/// **ONE END OF THIS RANGE IS OFF, AND READS "off"** (backlog FA): -1 the minimum, +1 the maximum, 0 neither. For
	/// a filter whose open end the device treats as an exact bypass — the Shaper's input HPF at 20 Hz and LPF at
	/// 20 kHz — so the readout says what the engine does rather than naming a corner nothing is filtering at. Within
	/// `kOffEndSlack` of the end counts as AT it, the same slack the device uses. Display and typing only ("off"
	/// parses to that end); `LogSeconds` has its own "off" at the minimum and needs none of this. Changes no
	/// stored key. Appended LAST for `shortName`'s reason.
	int8_t offAtEnd = 0;
	/// **MAY A HOST SLOT CARRY THIS CONTROL TO THE DAW?** (backlog OP follow-up). False for a control whose every
	/// flip is a structural edit — the Follower's Mode deletes routes — so a DAW lane must not move it mid-song. Every
	/// road into a slot refuses it (`rack::SlotOwner`: a pin, a touch, a nested level's, and a saved map, whose stale
	/// entry is dropped and reported). Changes no stored key. Appended LAST for `shortName`'s reason.
	bool hostSlot = true;
};

/// HOW CLOSE TO AN `offAtEnd` END A VALUE MAY SIT AND STILL BE OFF, in the control's own units — a ten-thousandth
/// of an octave on a `LogHertz` range, so a value that took a round trip through a host's normalised range lands on
/// "off" rather than on a filter at 20.001 Hz. The device's bypass test and the readout share it.
inline constexpr double kOffEndSlack = 1.0e-4;

/// WHETHER `value` SITS AT THE END `desc.offAtEnd` NAMES — the one test the readout and the parser share.
[[nodiscard]] inline bool atOffEnd(const ParamDesc& desc, double value) noexcept {
	return (desc.offAtEnd < 0 && value <= desc.min + kOffEndSlack)
	       || (desc.offAtEnd > 0 && value >= desc.max - kOffEndSlack);
}

/// **IS THIS CONTROL LIVE, GIVEN WHAT ITS GATE IS SET TO?** `gateValue` is the current value of
/// `p.gateControl`, which the caller reads because only it knows the instance. A control with no gate
/// is always relevant, which is every control in the repo except the ones that say otherwise.
[[nodiscard]] inline bool relevantFor(const ParamDesc& p, double gateValue) noexcept {
	if (p.gateControl < 0) {
		return true;
	}
	const long long v = std::llround(gateValue);
	if (v < 0 || v >= 32) {
		// OUT OF THE MASK'S REACH IS RELEVANT, NOT HIDDEN. A control that vanished because its gate
		// held an unexpected value would be a control the player cannot get back.
		return true;
	}
	return (p.gateMask & (1u << static_cast<unsigned>(v))) != 0;
}

inline constexpr bool isDragWindow(const ParamDesc& p) noexcept {
	// 2^20. The windows in this repo are 2^24 and 2^23 wide and the widest ordinary range is the
	// 20..20000 of a crossover frequency, so the line is three orders of magnitude clear of both.
	constexpr double kWindowSpan = 1048576.0;
	return p.unit == Unit::Number && p.zones == 0 && (p.max - p.min) >= kWindowSpan;
}

/// **HOW FAR A FULL-DEPTH ROUTE MOVES THIS CONTROL, IN ITS OWN UNITS** — the one rule every host's
/// resolve uses (Feathers' `ParamSet`, the rack engine), so an amount means the same distance on the
/// same control wherever the device is hosted.
///
/// A declared `modSpan` wins; a zoned control is moved in ZONE WIDTHS; everything else by its whole
/// declared range. The rack engine used to take only the last two, which scaled a route onto automod's
/// fine controls by their whole 2^24 drag window — the case `modSpan` was declared to prevent.
constexpr double routeSpanOf(const ParamDesc& p) {
	const double full = p.max - p.min;
	if (p.modSpan > 0.0) {
		return p.modSpan;
	}
	return p.zones > 0 ? full / static_cast<double>(p.zones) : full;
}

/// **A CONTROL'S BASE PLUS WHAT REACHES IT, AS THE DEVICE RECEIVES IT.** A stepped control lands on a
/// step; the declared range stops it unless the control says its offsets may leave it
/// (`offsetsMayLeaveRange` — the cutoff whose corner Nyquist bounds, not the knob). One rule for every
/// host; the rack engine did not round a stepped control before this was shared.
inline double withRouteOffset(const ParamDesc& p, double base, double offset) {
	double value = base + offset;
	if (p.stepped) {
		value = std::round(value);
	}
	if (!p.offsetsMayLeaveRange) {
		value = value < p.min ? p.min : (value > p.max ? p.max : value);
	}
	return value;
}

/// **A CONTROL'S BASE AS A HOST STORES IT** — coerced against its declaration, so a widget may hand over
/// whatever its drag produced. ONE RULE FOR EVERY HOST (backlog DV): the engine that runs a row, the
/// rack's record and Feathers' knob roads all call this, so the editor draws the number the engine uses
/// and a patch moved between hosts reads back the same.
///
/// **CLAMPED, ALWAYS.** The knob's range is the knob's range: a
/// hand stops at the end of the travel. `offsetsMayLeaveRange` is about what an OFFSET may do to the value
/// the device receives (`withRouteOffset`), not about where the base may sit. The rack's engine read it
/// the other way until this — a cutoff's base could be stored past its range in the rack and not in
/// Feathers.
///
/// **A STEPPED CONTROL IS NOT ROUNDED WHERE IT IS STORED.** Feathers'
/// knob road used to round it and this function kept that for a day. The value is stored as the hand left
/// it; every READER takes the nearest step on its way in — each device's own decode, `withRouteOffset`,
/// the UI's `steppedIndex` — so a switch stored at 0.4 still plays and draws as off, and nothing between
/// the hand and the reader throws the fraction away.
///
/// A float past the controls is row state and is not a control: the caller stores it as given.
inline float coerceControl(const ParamDesc& p, float value) {
	const double v = static_cast<double>(value);
	return static_cast<float>(v < p.min ? p.min : (v > p.max ? p.max : v));
}

/// **IS THIS CONTROL A SWITCH, OR A PICKER WITH TWO ENTRIES SO FAR?**
///
/// **THE COUNT IS NOT THE TEST, AND TREATING IT AS ONE PUT A LIE ON A CARD.** Every surface in this
/// project used to branch on `names.size() == 2` — a chip rather than a box, a caption rather than a
/// value, no dropdown below three entries — and each of those was the right call for the only
/// two-valued control that existed anywhere: `Off`/`On`. Lit MEANS on, so a glowing chip captioned
/// `Ping-Pong` has told you everything, and a menu to pick between two is two clicks for what a nudge
/// does.
///
/// **THE DELAY'S `Mode` IS `Clean`/`Granular` AND BREAKS ALL THREE.** A lit chip captioned
/// `Delay Mode` never says the word `Granular`; the player is left inferring which of two words a
/// glow means (it is not obvious that the mode enables grain). It was the first non-`Off`/`On` two-valued choice in the registry — measured across
/// `src/fx` and `src/mod` — which is why the count stood in for the question for so long.
///
/// **AND THE COUNT IS WRONG ABOUT THE FUTURE TOO, which is what settled the shape.** Mode is
/// better a dropdown, for future entries like analog and tape. A control that is a chip at two entries and a dropdown at three changes shape under the
/// player when a mode is added — and in between it is exactly the MULTIVALUE TOGGLE CLAUDE.md names
/// an antipattern, discovered by applying its options rather than seeing them.
///
/// So: **`Off`/`On` is a switch. Any other named choice is a picker, however few entries it has
/// today.** Asked of the names, so it cannot disagree with them.
[[nodiscard]] inline bool isOffOnChoice(const ParamDesc& p) noexcept {
	if (p.unit != Unit::Choice || p.names.size() != 2u || p.names[0] == nullptr
	    || p.names[1] == nullptr) {
		return false;
	}
	return std::string_view(p.names[0]) == "Off" && std::string_view(p.names[1]) == "On";
}

/// **A NAMED GROUP OF CONTROLS ON A CARD FACE.** See `Traits::faceSections`.
///
/// `firstControl` is an index into the device's `params`, the same addressing every other field here
/// uses (`enableControl`, `gateControl`, `faceOrder`) — not a position in the FACE order, because a
/// face can be reordered and a control's index cannot.
struct FaceSection {
	const char* label = "";
	uint8_t firstControl = 0;
};

/// What the host needs to know to place a device, declared by the device.
///
/// THESE TWO WORDS ARE NOT THE SAME KIND OF THING, AND THIS COMMENT USED TO CALL THEM BOTH A
/// SWITCH. One is read and one is not. Backlog §16; corrected by counting readers rather
/// than by reading the sentence above it.
///
/// **`hasExpanded` IS LIVE.** `ui::hasExpandedView` (`expanded_views.cpp:77`) answers
/// `traits.hasExpanded && entryFor(desc) != nullptr` — BOTH halves, declaration first, so a device
/// that has not declared one does not get one even if an arm exists for it. `HarmCard` consults it
/// before offering the way in. It exists so that DECLINING COSTS NOTHING: the filter's card is the
/// whole surface wanted — it needs no 'expanded' view — and a
/// device that says so here is complete rather than owing the host an empty second view.
///
/// **`hasCard` IS READ BY NOTHING.** Four devices declare it, all `true`, and no production branch
/// anywhere consults it — verified by count, not by reading. The generic fallback it describes is
/// REAL: `GenericCard` walks the descriptor and draws a knob per row, so a browser can add a device
/// nobody wrote a card for. **But it is not selected by this flag.** `FxLane::ensureCard`
/// (`fx_lane.cpp:176-188`) falls through to `GenericCard` when it cannot build a specific card for
/// the tag, and to `NoEditorCard` when this build has never heard of the device at all.
///
/// THAT STRUCTURAL SELECTION IS THE BETTER MECHANISM AND SHOULD NOT BE REPLACED BY THE FLAG. A flag
/// can disagree with reality — a device declaring `hasCard = true` that this build has no card for
/// would get no card and no fallback — while "what the lane can actually BUILD" cannot. It also
/// serves a case the flag cannot reach at all: the unknown-tag branch has no descriptor to read a
/// flag OFF, which `test_gui_widgets.cpp:22341` says outright.
///
/// SO WHY IS THE FIELD STILL HERE. Because deleting it is a separate decision from correcting this
/// comment, and this repo's rule is that apparent redundancy is checked for load-bearing before
/// removal. What it buys today is a DECLARATION — `test_fx_filter.cpp:237` asserts the filter's, and
/// the sibling case for `hasExpanded` guards against `-Wmissing-field-initializers` silence. **A
/// field that records a decision is not the same as a field that drives one, and the failure of the
/// old wording was to describe the second while only the first was true.**
struct Traits {
	const char* tag;  ///< up to `device::kTagBytes`; the device's record in the chain or rack block
	const char* name; ///< what the card's header says
	/// DECLARED BY EVERY DEVICE, READ BY NOTHING — see above. It records the device's intent to draw
	/// itself; the generic fallback is chosen structurally by whether the lane can build a card.
	bool hasCard;
	/// LIVE: `ui::hasExpandedView` reads this, and a device that says false gets no way in.
	bool hasExpanded;

	/// WHICH CONTROL DECIDES HOW MANY OF THIS DEVICE THERE ARE — an index into `params`, or -1 for a
	/// device that is always one thing.
	///
	/// ARCHITECTURE IS NOT A SETTING, and this is the line between them. Every other control changes
	/// what one instance DOES; this one changes how many instances the host runs and where they sit —
	/// one per voice before the sum, or one on the summed bus. Only the device knows which of its
	/// controls means that, and the host cannot run the device correctly without being told.
	///
	/// AN INDEX RATHER THAN A NAME OR A FLAG-PER-CONTROL, for device.h's permanent reason: a control
	/// is addressed by its position in `params` and never by a host identifier. -1 is the honest
	/// default — most devices are one thing, and a device that has not implemented a per-voice road
	/// must not claim one, because the host's answer to "poly is on" is to stop running it on the bus.
	int perVoiceControl = -1;
	/// **THE CONTROL THE POLY SWITCH NEEDS ON** (backlog OP) — an index into `params`, or -1 for a Poly switch that
	/// always acts. While that control reads below 0.5 the device is one, on the bus, whatever `perVoiceControl`
	/// says, and the switch is greyed with `perVoiceRequiresWhy` (`ui::DeviceControl::unavailable`). The Follower's
	/// Poly follows each voice's AUDIO, so it needs Mode = Audio. Every reader of the Poly switch asks
	/// `device::polyAllowed` (registry.h) as well, so the engine, the converger and the card cannot disagree.
	int perVoiceRequires = -1;
	const char* perVoiceRequiresWhy = nullptr;

	/// **HOW MANY FLOATS OF STATE THIS DEVICE CARRIES BEYOND ITS DECLARED PARAMETERS.**
	///
	/// A device may carry state that is not a
	/// parameter. The row IS the device's state of record and the host carries it without parsing it,
	/// so a second home for per-device state would re-derive an association the row already has —
	/// and would break for a rack holding two of the same device unless it did.
	///
	/// **NOT PARAMETERS, AND THAT IS THE POINT.** Automod's
	/// folded travel is a state block, not params: these floats have no `ParamDesc`, never reach a host's parameter list, never appear in
	/// the mod-assign UI, and no surface seats them. They are row state — carried, saved and restored,
	/// and otherwise invisible.
	///
	/// **THE ROW RULE, IN ONE PLACE, BECAUSE TWO PLACES DISAGREED.** `fieldsPresent` is
	/// `params.size() + extraState`, never more and never less. Before this existed `toDeviceRow`
	/// pinned it to the declared count while `gui_bridge` grew it when a hand edited past the end —
	/// so a row could be either, depending on which road it last took, and anything stored past the
	/// params was dropped by one of them. That was a truncating writer sitting in the tree before
	/// anybody wrote one.
	///
	/// ZERO FOR EVERY DEVICE BUT AUTOMOD, and a case asserts that such a device round-trips exactly as
	/// it did before this field existed: widening the contract must cost nothing to anyone not using
	/// it.
	size_t extraState = 0;
	/// **THE NAME A PATCH STORES THAT BLOCK UNDER** (backlog JF §3.3) — one key for the whole run of
	/// `extraState` floats, `[a-z0-9_.]` and distinct from every control's key. Required when
	/// `extraState` is non-zero and absent when it is zero (`device_registry.h` checks both).
	const char* extraStateKey = nullptr;


	/// WHICH CONTROL SWITCHES THIS DEVICE IN AND OUT OF THE SIGNAL — an index into `params`, or -1 for
	/// a device that is always running.
	///
	/// IT EXISTS BECAUSE THE HEAD IS SHARED NOW. Both lanes wear one head, and
	/// a head slot must be justified by something true of EVERY device the lane can hold. An enable is
	/// not: every fx device declares exactly one and NO modulator declares any. So the square is drawn
	/// conditionally, and the condition has to come from the DEVICE rather than from a branch on which
	/// lane we happen to be in — a per-lane branch would be right today and wrong the first time a
	/// modulator wants switching off.
	///
	/// AN INDEX AND NOT A BOOL, for `perVoiceControl`'s reason and device.h's permanent rule: a control
	/// is addressed by its position in `params`. A `bool hasEnable` would be a claim ABOUT the params
	/// that could disagree with them; an index either names a control or does not.
	///
	/// AND NOT BY MATCHING THE NAME. "Find the param called something ending in Enabled" was the
	/// alternative and it is worse in every direction: it breaks on a rename, it breaks on a device
	/// that spells it differently, and it makes a display string load-bearing. Recorded here so it is
	/// not re-proposed.
	///
	/// THE HEAD READS THIS ON DAY ONE, which is the whole difference between it and the two inert
	/// flags beside it. `hasCard` and `hasExpanded` are read by nothing in `src/` (backlog 16); their
	/// defect is not that they exist but that nothing consumes them.
	int enableControl = -1;
	/// **THE CONTROL THAT SCALES EVERY OUTPUT** (backlog DH: an amount knob on modulators which
	/// simply scales output to all destinations) — one knob per modulator, bipolar (−1..1, unity by
	/// default), a destination like any other. THE HOST APPLIES IT at the one publish every reader
	/// takes (`RackEngine`), block value and per-voice runs alike, so a device declares a param and
	/// names it here and never reads it. −1: no such control; the outputs pass as they are.
	int amountControl = -1;

	/// **HOW MANY CONTROLS BELONG ON THE CARD'S FACE; the rest go to the expanded view. 0 means ALL,
	/// which is what every device declared before this existed and still declares today.**
	///
	/// WHY IT EXISTS. `GenericCard` draws a knob per declared control, which is right for a device
	/// whose controls are all of one importance and wrong for one with a small playing surface and a
	/// menu behind it. SKLITTER is the second kind: the reference puts SEVEN things under the hands --
	/// rate, two zones, two macros, write probability, density -- and everything else lives in a menu
	/// or behind a push-and-twist. Declaring all nineteen and drawing them flat presents a
	/// seven-knob instrument as a list.
	///
	/// **IT IS A COUNT RATHER THAN A LIST, and that is a constraint on the declaration rather than a
	/// shortcut.** The face controls must be the FIRST ones, so a reader of the param enum sees the
	/// playing surface at the top and the settings below it, and a card cannot quietly diverge from
	/// the order the enum states. SKLITTER's first seven already are its knobs, because the enum keeps
	/// the reference's own order.
	///
	/// **IT DOES NOT MAKE ANYTHING UNREACHABLE, WHICH IS THE WHOLE DIFFERENCE FROM A BESPOKE CARD.**
	/// OWLTT's hand-written card left fifteen of twenty-five controls with nowhere to be touched and
	/// had to be reverted; a split derived from the declaration cannot lose one, because the expanded
	/// view draws exactly the complement. `test_gui_widgets_dahdsr.cpp`'s reachability table is what
	/// holds that true.
	size_t faceControls = 0;

	/// **THE FACE AS A LIST, IN THE ORDER THE CARD LAYS IT OUT — and when it is not empty it replaces
	/// `faceControls`.**
	///
	/// WHY A LIST AFTER ALL. The count above argued that the face must be the FIRST controls declared,
	/// and for SKLITTER that stopped being possible: its card drops the zone
	/// controls and adds Mix — the first three declared are the zones, and they cannot
	/// move, because `test_fx_sklitter_types.cpp` pins the reference's positional order its reference
	/// indexes modulated values by. So the face names its members instead.
	///
	/// **THE COUNT'S REAL GUARANTEE SURVIVES, because it never came from the order.** Nothing is made
	/// unreachable by a face that omits a control: the expanded view draws EVERY control (the superset
	/// rule, `generic_expanded.cpp`), whatever the face is. Indices past the end are dropped.
	std::span<const uint8_t> faceOrder {};

	/// **HOW MANY CELLS ACROSS THE CARD'S FACE IS. 0 or 1 is the generic LIST** — one control per row,
	/// caption beside the face — which is what every device declared before this existed. More than
	/// one is a GRID of captioned cells, filled row by row in `faceOrder`'s order, with a two-way
	/// switch drawn as a knob-sized pad (knobs rearranged to a grid instead of a
	/// list, the enable button a full knob-sized piece). A property of the DEVICE
	/// rather than of a card class, because the lane sizes a card from the descriptor before there is
	/// an instance to ask — see `GenericCard::heightFor`.
	uint8_t faceColumns = 0;

	/// WHAT THIS DEVICE IS, FOR A BROWSER TO GROUP IT BY — declared, not inferred.
	///
	/// The device browser's tree is tags first, directory as fallback: a node is a pinned tag, and
	/// anything with no pinned tags falls back to where it
	/// sits in the source tree. This is the pinned half.
	///
	/// A DEVICE TAGS ITSELF because only the device knows what it is. A browser that grouped by
	/// directory alone would put the filter and the comb in "fx" and stop there, and one that grouped
	/// by a table in the host would be the host holding a fact about a device — the pin this whole arc
	/// removes everywhere else.
	///
	/// EMPTY IS FINE AND IS NOT A DEFECT. A device that declares nothing groups by its directory, which
	/// is what the fallback is for. That is also what a third-party guest gets before anyone has
	/// thought about where it belongs.
	///
	/// THE STRINGS ARE THE DEVICE'S OWN and are not checked against a list. A closed vocabulary would
	/// have to be extended before a guest could describe itself, which is the same closed-world
	/// assumption `lane` would have been.
	std::span<const char* const> tags {};

	/// **WHERE THE CARD DRAWS A HEADING, so a group of controls reads as a group.**
	///
	/// A generic card is a flat run of rows, and a flat run cannot say that four of its controls
	/// belong to a fifth. The delay is the case that forced this: switching its `Mode` to `Granular`
	/// makes four gated controls APPEAR, and nothing on the face connects them to the switch that
	/// summoned them. It is not obvious
	/// from the face that the mode enables grain.
	///
	/// **IT IS ON THE DESCRIPTOR RATHER THAN ON THE CARD, AND THAT IS FORCED.** `GenericCard::
	/// heightFor(desc, width)` is STATIC — the lane sizes a card from the descriptor before an
	/// instance exists, which is the same reason `faceColumns` lives here. A section list supplied by
	/// a card instance would be invisible to that call, and the lane would reserve a card too short
	/// by one heading per section: a layout bug that only appears once a device declares one.
	///
	/// **A HEADING IS DRAWN BEFORE `firstControl` AND ONLY IF SOMETHING UNDER IT IS LIVE.** A section
	/// whose every control is gated away by `relevantFor` draws nothing at all — a heading over
	/// emptiness is the same lie as a control that does nothing, and the delay in `Clean` mode is
	/// exactly that case: no GRAIN heading, because there is no grain.
	///
	/// Empty is fine and is what every device declares today; the card then draws the flat run it
	/// always has.
	std::span<const FaceSection> faceSections {};

	/// **CONTROLS THE EXPANDED PANEL SEATS IN A COLUMN OF THEIR OWN, beside its grid — backlog IG.**
	///
	/// SKLITTER's Groove needs a new column in controls until some of the switches are consolidated. The panel's grid fills row by row, so a
	/// FIFTH grid column would put a different control in it in every Mode (a gated cell closes its gap
	/// and the flow shifts). A column holds the SAME controls top to bottom whatever the Mode, which is
	/// what "a column for these" means to a hand looking for them.
	///
	/// **EMPTY IS EVERY DEVICE BUT ONE, AND AN EMPTY LIST IS THE PANEL IT ALWAYS WAS** — four columns,
	/// byte for byte. Indices into `Descriptor::params`, in top-to-bottom order; past the end is dropped.
	/// A control listed here is still gated by its `gateControl` exactly as a grid cell is.
	std::span<const uint8_t> sideColumn {};

	/// **KEYS THIS DEVICE HAS RETIRED, EACH WITH ITS REASON** (backlog JF §3.3) — see `RetiredKey`. A value
	/// a patch holds under one is dropped on load and the report gives the reason; a key listed here can
	/// never name a control again. Empty is every device on the day keys arrived.
	std::span<const RetiredKey> retiredKeys {};

	/// **THIS DEVICE'S OWN SCHEMA, WRITTEN BESIDE ITS VALUES** (backlog JF §3.5). **IT MOVES ONLY WHEN THE
	/// DEVICE WANTS A MIGRATION** — never for a control inserted, removed, reordered or appended, which keys
	/// absorb. A patch from a newer schema loads what this build knows, carries the rest, and says so.
	uint32_t schema = 1;
	/// **A LATENT DEVICE THAT MAY RUN PER VOICE** (the Spectral device's per-voice road). Assumption L2 runs a device
	/// that reports latency on the bus only (its Poly switch read as off); a device that sets this is exempt IN A HOST THAT
	/// COMPENSATES IT PER VOICE (`RackEngine::setAllowLatentPerVoice`: Feathers' lane, which delays each voice's gain run and its
	/// free by the per-voice latency). Inner chains keep L2: their per-voice wet/dry blend is not compensated (L7). Opt-in, so
	/// a future latent device with a Poly switch is not admitted by accident.
	bool latentPerVoice = false;

	/// **THE CHAIN FURNITURE'S TWO ROLES** (backlog NR):
	/// the control that is this device's dry/wet Mix (0 the input, 1 the wet), and the one that is its wet Gain
	/// (`Unit::Fader`, unity at `composite::kFaderUnity`). Indices into `params`, for `enableControl`'s reasons.
	/// −1: the device has no such control.
	///
	/// **THE SLOT APPLIES THEM, NOT THE DEVICE** — the engine blends the signal that entered the slot against what
	/// the slot's PRE chain, the device and its POST chain made of it, so a POST chain sits INSIDE the mix (a
	/// wet-only insert). A device declaring a role never reads the control; it hands back the wet.
	int mixControl = -1;
	int wetGainControl = -1;
	/// **A MIX THE DEVICE MAKES ITSELF** (D2): its dry/wet is part of the effect — a phaser's notches are
	/// the dry summed with the wet, a q31 firmware mix, a mix inside a limiter — so `mixControl` names it for the
	/// frame and the SLOT DOES NOT BLEND. A POST chain is then a series insert after the device's own mix.
	bool mixInDevice = false;
	/// WHICH OF THE SITE'S TWO CHAINS THIS DEVICE OFFERS (backlog NR, D5: a generator offers POST only). A device
	/// that holds chains of its own offers them too (D3, for the Rack and the Chain); one
	/// whose lanes ARE its sound (SKLITTER, Resonate) declares `None`.
	enum class SiteLanes : uint8_t { PreAndPost, PostOnly, None };
	SiteLanes siteLanes = SiteLanes::PreAndPost;

	/// **THE OUTPUT ARMS AS A FIXED GRID, THIS MANY COLUMNS — 0 IS THE HOUSE FLOW** (Twin Field: 2 rows of
	/// 4 mod outs on the card). Zero lays the chips by their names' widths and wraps
	/// (`layoutOutputChips`), as every device has. Non-zero lays them in equal cells, `outputGridColumns` to a row, in
	/// declared order, in every host — the grid is the device's, so no host can draw it another way. A chip narrower
	/// than its name prints the name's last word (`SourceArm::draw`).
	uint8_t outputGridColumns = 0;
};

/// WHICH CONTROL SWITCHES A DEVICE, or `kNoControl` — asked of the declaration, never assumed.
///
/// A DECLARATION PAST THE END IS REFUSED rather than trusted: an index is only an index while it
/// names something. That refusal is the whole reason this is a function and not a field read.
///
/// IT LIVES HERE SO THE DERIVATION HAS ONE HOME. `FxCard::enableControl` does the same thing for the
/// fx lane's cards and predates this; the head unification collapses the two, and whoever lands it
/// should point that one here rather than leave two copies of a refusal.
inline constexpr size_t kNoControl = static_cast<size_t>(-1);

/// **WHETHER AN ENABLE READS ON — THE STEPPED DESTINATION'S CONTRACT (backlog KW).** An enable is a modulation
/// destination and a STEPPED one: the resolve rounds base + routes to a step and clamps it to the range
/// (`withRouteOffset`), as the host quantises a stepped parameter, so a routed source crossing one half flips it — a
/// GATE. This is the one reader of the result; a value off the steps (a hand-written row) reads by the same half.
[[nodiscard]] constexpr bool enableOn(float resolved) noexcept {
	return resolved >= 0.5f;
}

constexpr size_t enableControlIn(const Traits& traits, std::span<const ParamDesc> params) {
	if (traits.enableControl < 0) {
		return kNoControl;
	}
	const auto at = static_cast<size_t>(traits.enableControl);
	return at < params.size() ? at : kNoControl;
}

/// HOW MANY OF A DEVICE'S CONTROLS ARE MODULATION DESTINATIONS — its own flags, counted.
constexpr size_t modulatableIn(std::span<const ParamDesc> descs) {
	size_t count = 0;
	for (const ParamDesc& desc : descs) {
		if (desc.modulatable) {
			++count;
		}
	}
	return count;
}

/// **ONE OF THE NEWEST EVENTS A MODULATOR MADE** (`ModOps::recentEvents`): a sample a Sample & Hold took, a hit a Random
/// Trigger made. A card reads the playhead once a frame, and a device that makes several events a frame (a fast clock,
/// a dense stream) showed only the newest; these are the ones in between. `ordinal` is the device's own count of them —
/// it only grows, and 0 is never an event, so an empty entry is all zero. `value` is what the card draws of it.
struct RecentEvent {
	uint32_t ordinal = 0;
	float value = 0.0f;

	friend bool operator==(const RecentEvent&, const RecentEvent&) = default;
};
/// HOW MANY A DEVICE OFFERS: a frame at 60 Hz against the 750 Hz tick is twelve ticks, and an event lands at most once
/// a tick on a S&H; sixteen covers a frame that ran late.
inline constexpr size_t kRecentEvents = 16;

/// THE WIRE FORM, ONE WORD: the ordinal high, the value's bits low — so a reader cannot pair one event's ordinal with
/// another's value, whichever ticks the words it reads were written on.
[[nodiscard]] inline uint64_t packRecentEvent(RecentEvent e) noexcept {
	return (static_cast<uint64_t>(e.ordinal) << 32) | std::bit_cast<uint32_t>(e.value);
}
[[nodiscard]] inline RecentEvent unpackRecentEvent(uint64_t w) noexcept {
	return { static_cast<uint32_t>(w >> 32), std::bit_cast<float>(static_cast<uint32_t>(w & 0xFFFFFFFFu)) };
}

} // namespace feathers::device
