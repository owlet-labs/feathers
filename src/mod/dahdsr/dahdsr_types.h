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
#include "mod/modulator.h"
#include "mod/trig.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// WHAT A DAHDSR IS, with none of what it does — the settings, the tapers and the stage names, in a
// header that pulls in no DSP.
//
// The split is filter_types.h's, and for the same two reasons. The concrete one: the patch stream
// and the card both need to say what an envelope is SET to without either of them compiling an
// envelope. The general one: reading a modulator's controls and drawing its shape are things a host
// might want without ever advancing it a sample. The state machine lives next door in dahdsr.h,
// which is the only file that knows how the stages run (the maths is stated in dahdsr.cpp).
//
// THIS FILE INCLUDES NOTHING FROM src/plugin, and neither does anything else under src/mod. The rule
// is fx_tenant.h's, stated there at length: a modulator owns its DSP, its settings and its card, and
// the plugin decides where in ITS world those land. What the fx lane proved for a device that
// processes audio, this is the first instance of for a device that emits modulation.

namespace feathers::mod::dahdsr {

/// HOW MANY SAMPLES ONE TICK OF THIS ENVELOPE COVERS, and it is not a free choice.
///
/// The engine already has a control-rate grid: PuckMotion::kTickSamples, the fixed 64-sample step
/// the puck's velocity moves on, and Synth::render caps every chunk at the next one of those so a
/// swipe sounds the same at every buffer size. A modulator that advanced on CHUNK boundaries instead
/// would put that guarantee back exactly where the moving-puck invariance test found it broken.
///
/// So it is 64 here too, and the plugin static_asserts the two are the same number rather than
/// trusting a comment. It is also a power of two, which the rack's block arithmetic assumes and
/// test_mod_dahdsr restates — which 64 is.
/// PROMOTED — see mod/modulator.h. Aliased rather than moved outright so every existing
/// `dahdsr::kBlockSamples` keeps working; there is one definition and this is a second NAME for it,
/// which is the opposite of a second copy.
inline constexpr int kBlockSamples = mod::kBlockSamples;

/// WHERE A STAGE TIME KNOB RUNS, in log2 seconds, and why these two numbers.
///
/// Even in octaves rather than in seconds, which is this project's rule for every time and frequency
/// control it has (see the filter's LogHertz, and glideSeconds). A knob linear in seconds spends
/// four fifths of its travel between one second and ten, where nobody needs resolution, and gives
/// the millisecond end — where an attack actually lives — a pixel.
///
/// The bottom is 2^-11 ≈ 0.49 ms: shorter than the shortest transient the ear resolves, so the knob
/// reaches "instant" without needing a special case. The top is 10 s, which is Bitwig's own ceiling
/// for an AHDSR stage and the parity target.
///
/// A knob at EXACTLY ZERO is not 0.49 ms, it is a stage that does not happen: the state machine
/// (dahdsr.cpp) reads 0 s for it and steps straight past the stage. That is deliberate and it is
/// what makes "no hold" reachable rather than "a hold too short to hear".
inline constexpr float kStageLog2SecondsMin = mod::kTimeLog2SecondsMin;
inline constexpr float kStageLog2SecondsMax = mod::kTimeLog2SecondsMax;

/// WHERE THE SMOOTHING TIME RUNS, on the same kind of taper: 2^-11 ≈ 0.49 ms to 2^0 = 1 s.
///
/// Bitwig states this control in milliseconds/seconds and so do we; what it is underneath is the
/// time constant of a one-pole on the envelope's output. See `Settings::smoothing` for what it is FOR,
/// which is not "make the envelope smoother" — a DAHDSR is already smooth. (It said
/// `Settings::smoothingOn`; that field is deleted, and a cross-reference to a name that no longer
/// exists is worse than none.) The bottom of the travel is OFF — `mod::smoothingCoeff`.
inline constexpr float kSmoothingLog2SecondsMin = mod::kSmoothingLog2SecondsMin;
inline constexpr float kSmoothingLog2SecondsMax = mod::kSmoothingLog2SecondsMax;

/// How far a shape control bends its stage, each way. Bitwig's per-stage curve control is bipolar
/// and so is this.
///
/// POSITIVE MEANS FULLER ON EVERY STAGE, which needed choosing. One curve read the same way on every
/// stage bends toward its END on a rising stage and a falling one alike, and attack is the only rising stage of the
/// three — so that reading would have positive push attack toward 1 but decay and release toward their
/// floors, which is three knobs where the same gesture means opposite things. dahdsr.cpp reads the
/// curve BACKWARDS on the falling stages for exactly this: positive is "the envelope stays high longer" on
/// attack, decay and release alike, negative is "it gets out of the way sooner", zero is a straight
/// line. Three knobs side by side on one card have to answer to the same hand.
inline constexpr float kShapeMax = mod::kShapeMax;

/// WHAT STARTS THE SHAPE, and what a gate has to do with it.
///
/// The trigger mode was decided in on 2026-08-22, against the worry that a free-running mode would
/// make the device's one-sentence description into two: it still has a global retrigger, so the
/// semantics are not bad. The reason is worth writing down — the gate here
/// is ALREADY global rather than per-note, so "the whole rack restarts together" is a concept this
/// device has from its first day. Free is a different answer to "when", not a new kind of thing.
/// PROMOTED to mod::Mode — see mod/modulator.h, where the prose that was here now lives. A
/// modulator's trigger mode is a fact about the RACK's gate, which is global, rather than about a
/// DAHDSR; the MSEG needs exactly the same three answers and must not have its own copy of them.
using Mode = mod::Mode;

/// Which stage the envelope is in — the state machine's own vocabulary, and the set a card has to
/// be able to draw and a test has to be able to name.
enum class Stage : uint8_t {
	Idle, ///< finished, or never started
	Delay,
	Attack,
	Hold,
	Decay,
	Sustain,
	Release,
};

//---------------------------------------------------------------------------//
// What this device's controls ARE — the declarations a host and a card read.  //
//---------------------------------------------------------------------------//

// THE DEVICE DECLARES ITS OWN CONTROLS, in device/device.h's vocabulary and nobody else's.
//
// This is the half that makes the thing standalone-capable, and it is the half a first draft of this
// module did not have. The filter had already proved the shape —
// fx::filter::kParamDescs, from which the plugin BUILDS its parameter rows — and this is the same
// statement for a device whose controls are deliberately not parameters at all.
//
// WHAT THE LIST BUYS, in the two tiers device.h describes. A device that declines to draw itself is
// rendered generically from these rows — a knob apiece, addressed by index — which is what lets a
// browser add a device nobody wrote a card for. THIS DEVICE WILL DRAW ITSELF: an envelope has a
// shape to show, and a list of nine knobs cannot show it. The declarations are still what its own
// card addresses its controls THROUGH, by index rather than by any host identifier, which is the
// difference between a bespoke card and a bespoke card that only works here.
//
// WHERE THE VALUE LIVES IS THE HOST'S BUSINESS. The plugin carries these in a tagged state block
// because CLAP's parameter list is static and a rack's contents are not; the filter's identical
// declarations get seated at clap ids instead. Neither device finds out which happened.

/// Every control this device has, in the order the row below packs them. APPEND ONLY: the index IS
/// what the patch stream stores.
enum class Param : int {
	Delay,
	Attack,
	Hold,
	Decay,
	Sustain,
	Release,
	AttackShape,
	DecayShape,
	ReleaseShape,
	/// **AND THE SWITCH BESIDE IT IS GONE — the bottom of this control's travel IS off** (2026-09-19: a separate smooth enable
	/// invited turning smooth up while the toggle was off, so smooth=0 is the off option). See `mod::smoothingCoeff`.
	Smoothing,
	/// **THE MODE — one dropdown that replaces the old Loop, SingleTrigger and Trigger toggles.** The
	/// three overlapped ("Once" was `SingleTrigger=On + Loop=Off`, and "Free + retrigger" was not
	/// expressible at all), so they collapsed into one enum. NOT MODULATABLE — a mode under a modulator
	/// is a switch chattering at 750 Hz, the same reason the filter's Type declines.
	Mode,
	/// **LEGATO — NOT A UI CONTROL, an internal flag the synth sets.** A new note under a held gate does
	/// NOT restart the envelope; the existing envelope continues. The synth reaches it by field index —
	/// it has no kParamDescs entry and draws nothing, which is why it is hidden. Declared here so the
	/// field order is one table rather than two.
	SingleTrigger,
	Bipolar,
	/// **THE BYPASS SWITCH, AND THE HEAD'S SQUARE DRAWS IT.** an fx device declares one and the shared
	/// head draws it for free; a modulator declared none, so `FxCard::hasEnable` was false and the card
	/// correctly drew nothing. This is the missing declaration rather than a missing control.
	///
	/// APPENDED rather than inserted at the front. HARM put its `Enabled` first and said why that was
	/// cheap; here the row is indexed by this enum in saved state and appending moves nothing, which is
	/// the smaller change for the same result. Patch compatibility is not a constraint either way
	/// (CLAUDE.md), so the tiebreak is simply which edit is easier to be sure about.
	Enabled,
	/// **THE AMOUNT — one knob that scales every output before every route** (backlog DH). Bipolar, so
	/// pulled below zero it inverts the whole modulator; unity by default, so a fresh envelope does
	/// what it always did. THE HOST APPLIES IT (`Traits::amountControl`, `RackEngine`'s publish) — the
	/// device never reads it, which is why it is a plain field of `Settings` and nothing more here.
	/// Appended, so no stored column moves.
	Amount,
	/// **POLY — one envelope per note, or one for the whole device** (then with no dual-path logic). On, every note runs its own envelope and
	/// every destination reads the note's own (a shared destination reads the most recently struck note's). Off,
	/// ONE envelope, gated by the rack's "any note held" and restarted by notes as the mode says, and every
	/// destination reads it. See `mod/per_note.h`. Appended, so no stored column moves.
	Poly,
	/// **SMOOTH THROUGH RETRIGGER** (backlog MM) — see `mod::PolyShape::processNotes`. Appended.
	SmoothCarry,
	/// **TRIG — FIRES THE ENVELOPE** (backlog MV): an ordinary modulatable control, fired when its resolved value
	/// rises to 0.5 and re-armed below 0.25 (`mod/trig.h`). Appended; keyed.
	Trig,
	/// **NOTES FIRE TRIG** (backlog NU): the note-on normal into Trig, and the switch that breaks it — `mod::notesTrigParamDesc`.
	NotesTrig,

	kCount,
};

inline constexpr auto& kOffOnNames = mod::kOffOnNames;
inline constexpr auto& kModeNames = mod::kModeNames;

/// The device qualifies its own names — "Env Attack", not "Attack" — for the reason device.h states
/// at length: a device does not get to assume what else is in the list it lands in.
/// WHICH OF THESE A MODULATOR MAY BE POINTED AT — the `modulatable` flag, and the division is the
/// same one the filter's descriptors make: an AMOUNT may be modulated, a control that rebuilds the
/// device's behaviour may not.
///
/// The ten continuous controls say yes. The four switches and the trigger mode say no, and that is
/// not timidity — a stepped control under a modulator is a control being rounded at 750 Hz, which
/// is a switch chattering rather than a setting being played. The filter's Type and Slope decline
/// for the same reason.
///
/// SO A MODULATOR'S OWN CONTROLS ARE DESTINATIONS, which is the unified surface decided on
/// 2026-08-22: a macro can sweep this envelope's attack, and an envelope can bend its own. See
/// params.h for the addressing and for why the self-modulating case is safe rather than blocked.
/// A TIME'S VALUE **IS** ITS log2 SECONDS, not a 0..1 position that becomes seconds later.
///
/// This was wrong in the first draft and the mistake is worth recording, because it is the exact
/// failure the declarations exist to prevent. The rows below said `Unit::LogSeconds` while the value
/// stored was a 0..1 knob position that only meant a duration after passing through this module's
/// own taper — so a generic card reading the descriptor would have printed "0.304" for a ten
/// millisecond attack, and a host that had never seen a DAHDSR could not have printed anything at
/// all. **A unit that describes something other than the value is worse than no unit**, because it
/// looks like it works.
///
/// The filter had it right all along and is the model: its cutoff declares LogHertz and its minimum
/// is literally `log2(20)`. So a stage runs from log2(0.49 ms) to log2(10 s) here, and
/// `stageSeconds()` is now `exp2` rather than a taper anyone has to know about.
///
/// AT THE VERY BOTTOM A STAGE DOES NOT HAPPEN. There is no "zero" in a logarithmic domain, so the
/// minimum itself carries that meaning — drag a time to the bottom of its travel and the stage is
/// skipped rather than made imperceptibly short. Same behaviour the 0..1 domain had at 0, expressed
/// where it can be read off the range instead of needing a threshold constant beside it.
inline constexpr device::ParamDesc kParamDescs[] = {
	{ "env_delay", "Env Delay", kStageLog2SecondsMin, kStageLog2SecondsMax, kStageLog2SecondsMin, false,
	  device::Unit::LogSeconds, {}, true, false,
	  "Holds the envelope back before the attack begins — a silence before the note starts" },
	// log2(0.010) — ten milliseconds.
	{ "env_attack", "Env Attack", kStageLog2SecondsMin, kStageLog2SecondsMax, -6.643856189774724, false,
	  device::Unit::LogSeconds, {}, true, false,
	  "How long the envelope takes to reach its peak after the trigger" },
	{ "env_hold", "Env Hold", kStageLog2SecondsMin, kStageLog2SecondsMax, kStageLog2SecondsMin, false,
	  device::Unit::LogSeconds, {}, true, false,
	  "Keeps the peak for a moment before the decay begins" },
	// log2(0.500).
	{ "env_decay", "Env Decay", kStageLog2SecondsMin, kStageLog2SecondsMax, -1.0, false,
	  device::Unit::LogSeconds, {}, true, false,
	  "How long the envelope takes to fall from the peak to the sustain level" },
	// The one level among the six times, so the one that is a percentage.
	{ "env_sustain", "Env Sustain", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
	  "The level the envelope holds at while the note is pressed" },
	// log2(0.250).
	{ "env_release", "Env Release", kStageLog2SecondsMin, kStageLog2SecondsMax, -2.0, false,
	  device::Unit::LogSeconds, {}, true, false,
	  "How long the envelope takes to fall to silence after the note is released" },
	{ "env_attack_curve", "Env Attack Curve", -kShapeMax, kShapeMax, 0.0, false, device::Unit::Percent, {}, true, false,
	  "Bends the attack — negative is a slow start that snaps open, positive is a fast start that eases" },
	{ "env_decay_curve", "Env Decay Curve", -kShapeMax, kShapeMax, 0.0, false, device::Unit::Percent, {}, true, false,
	  "Bends the decay — negative holds the peak longer, positive drops it faster" },
	{ "env_release_curve", "Env Release Curve", -kShapeMax, kShapeMax, 0.0, false, device::Unit::Percent, {}, true, false,
	  "Bends the release — negative fades slowly at first, positive drops quickly then lingers" },
	// **DEFAULTS TO THE BOTTOM OF ITS TRAVEL, WHICH IS OFF.** It read log2(0.020) — twenty
	// milliseconds — while a separate `Smoothing On` defaulted to 0, so the pair meant "off" and the
	// number was a suggestion for whoever switched it on. With the switch deleted the number IS the
	// state, so it has to say what the device does: nothing, until asked. The RIGHT default rather
	// than the one that keeps an old row sounding the same (CLAUDE.md, 2026-08-22).
	{ "env_smoothing", "Env Smoothing", kSmoothingLog2SecondsMin, kSmoothingLog2SecondsMax,
	  static_cast<double>(kSmoothingLog2SecondsMin), false, device::Unit::LogSeconds, {}, true, false,
	  "How much the envelope's edges are softened — prevents clicks on fast stages; at the bottom of its travel it is off" },
	// **THE MODE — one dropdown that replaces the old Loop / SingleTrigger / Trigger toggles.** The three
	// overlapped ("Once" was `SingleTrigger=On + Loop=Off`, and "Free + retrigger" was not expressible at
	// all), so they collapsed into one enum. Five choices: Gate, Loop, Once, Free, LFO Retrig. NOT
	// MODULATABLE — a mode under a modulator is a switch chattering at 750 Hz, the same reason the
	// filter's Type declines.
	{ "env_mode", "Env Mode", 0.0, static_cast<double>(mod::Mode::kCount) - 1.0, 0.0, true,
	  device::Unit::Choice, kModeNames, false, false,
	  "How the envelope runs — Gate retriggers while held, Loop cycles, Once fires and holds, Free runs continuously, LFO Retrig runs and restarts on every note" },
	// **LEGATO — NOT A UI CONTROL.** A new note under a held gate does NOT restart the envelope.
	// The synth sets it by field index; it draws nothing on the card. Declared here so the field
	// order is one table rather than two.
	{ "env_legato", "Env Legato", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "On: a new note under a held gate continues the existing envelope instead of restarting it" },
	{ "env_bipolar", "Env Bipolar", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "Swings the envelope between -1 and +1 instead of 0 and 1 — a negative amount then reaches backward" },
	/// **ON BY DEFAULT**, because a modulator added to a rack should modulate. The identity guarantee
	/// runs the other way from FM Depth's: an envelope that arrives switched OFF would be a device that
	/// does nothing until a hand finds a switch, which is the same "appears broken" the missing button
	/// caused.
	{ "env_enabled", "Env Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
	  "Switches the whole envelope on or off" },
	/// See `Param::Amount`. MODULATABLE: a macro riding an envelope's depth is the classic use, and a
	/// depth no source could reach would be the one such control in the plugin.
	{ "env_amount", "Env Amount", -1.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
	  "How deeply the envelope modulates its destination — the output level of the modulation signal" },
	/// See `Param::Poly`. ON BY DEFAULT: an envelope is a note's, and the default patch's Voice Level envelope must
	/// shape each note on its own (backlog EF). NOT MODULATABLE — a switch.
	{ "env_poly", "Env Poly", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "On: every note gets its own envelope. Off: one envelope every note shares, restarted by notes as the mode says" },
	/// See `Param::SmoothCarry`. Gated on Poly: under mono the one envelope's smoother already runs through a
	/// retrigger. Under Legato a held note is taken up whole, smoother and all, so this decides only a fresh start.
	{ "env_smooth_carry", "Env Smooth Carry", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "Smooth through retrigger: a new note's envelope glides from where the last note's smoothed output stood, "
	  "instead of starting its smoothing from zero. Needs Poly and some Smoothing",
	  0, 0.0, {}, static_cast<int16_t>(Param::Poly), 1u << 1 },
	/// See `Param::Trig`.
	mod::trigParamDesc("env_trig", "Env Trig",
	                   mod::trigTooltip<"Fires the envelope: it restarts when this rises past half and fires again "
	                                    "only after it falls below a quarter. Route a trigger here, or click to fire "
	                                    "by hand.">()),
	/// See `Param::NotesTrig`.
	mod::notesTrigParamDesc("env_notes_trig", "Env Notes Trig"),
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "env", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "DHDS",
	.name = "DAHDSR",
	.hasCard = true,
	// Not yet. The six times, three curves and four switches fit one card inside the design doc's
	// 320px cap; an expanded view would be a second surface with nothing extra to put on it.
	.hasExpanded = false,
	/// THE HEAD'S SQUARE. See `Param::Enabled` — declaring this is the whole of what the card needed.
	.enableControl = static_cast<int>(Param::Enabled),
	/// THE AMOUNT, applied by the host at its publish — see `Param::Amount` (backlog DH).
	.amountControl = static_cast<int>(Param::Amount),
	.tags = kTags,
};

/// WHAT THIS DEVICE PUBLISHES TO THE MODULATION BUS — one envelope signal.
///
/// DECLARED RATHER THAN INFERRED, which is the point of it existing at all. A host that assumed "it
/// is in the rack, therefore it has one output" would be a host that has to be edited the day a
/// device has two, or none, or is a note effect that also modulates. See device::SourceDesc for the
/// decision this comes from.
///
/// BIPOLAR IS DECLARED TRUE THOUGH THE SWITCH DEFAULTS OFF, and that is deliberate: what a device
/// declares here is the WIDEST signal it can publish, not what it happens to be doing this block.
/// The Bipolar control (Param::Bipolar) moves at runtime, and a host that resized its thinking every
/// time a switch moved would be a host whose route amounts change meaning under the user's hand.
///
/// **AND NAMING THAT CONTROL IS WHAT LETS A SURFACE ASK THE OTHER QUESTION.** The paragraph above is
/// about ROUTING and stands unchanged; `bipolarControl` is about DRAWING, where the honest answer is
/// what this envelope is publishing right now. With the switch off it publishes 0..1, so a negative
/// amount on it reaches downward only — which is what a ring must draw and what
/// `SourceDesc::bipolarControl` exists to let it find out.
inline constexpr device::SourceDesc kOutputs[] = {
	/// PER VOICE IN VALUE (backlog EF): the collapsed envelope for every un-migrated destination, and one
	/// envelope per NOTE behind `ModOps::outputForNote` for the per-voice runs — see `dahdsr.cpp`.
	{ .key = "envelope", .name = "Envelope", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar), .perVoice = true },
};

/// EVERYTHING THIS DEVICE SAYS ABOUT ITSELF, in the one shape a host can hold without knowing which
/// device it is. Assembled from the declarations above rather than repeating any of them.
/// HOW A HOST ADVANCES THIS DEVICE — defined in dahdsr.cpp. See device::ModOps.
extern const device::ModOps kModOps;
/// AND HOW IT LISTENS TO THE NOTE LANE — the per-note envelopes (backlog EF). Defined in dahdsr.cpp.
extern const device::NoteOps kNoteOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.category = device::Category::Modulators,
};
/// WHAT ONE ENVELOPE IS SET TO. Every time is a 0..1 knob position on the taper above, NOT seconds —
/// because the knob is what a hand moves and what the patch stores, and a second domain here would
/// be a second place for a range to be wrong (fx::ParamDesc says the same thing in as many words).
///
/// NOT PARAMETERS. This is the routings block's decision applied to the
/// same kind of object: a modulator's controls are patch state on a tagged block, not clap params.
/// The reason is the browser that is coming — CLAP's parameter list is STATIC, so ids spent on a
/// fixed set of modulator slots would fix the modulator count and type forever, at whatever a build
/// happened to allocate before anyone knew what the rack would hold. The macros stay params and stay
/// host-facing; they are the mod-IN, and that is the tier split rather than an inconsistency.
/// EVERY DEFAULT IN THIS STRUCT IS READ OUT OF THE DECLARATIONS ABOVE, and none of them is written
/// twice.
///
/// The first draft had the numbers in both places — `float attack = 0.30416f;` here and
/// `{"Env Attack", …, 0.30416, …}` there — with a case to check they agreed. That case would have
/// been the third copy of the same knowledge, and this codebase's whole position on duplicated
/// knowledge is that the check is not the fix. `kParamDescs` is constexpr, so the default IS
/// reachable at compile time and there is nothing for a test to hold together.
///
/// It is also why the declarations sit above this struct rather than below it, which is the reverse
/// of filter_types.h's order: there, the descriptors describe params the plugin owns, so nothing in
/// the module needs their values. Here they are the source.
constexpr float defaultOf(Param param) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(param)].def);
}

struct Settings {
	/// WHICH CONTROL IS THE TRIG, for `mod::PolyShape`'s per-note resolve (backlog MV, B).
	static constexpr size_t kTrigControl = static_cast<size_t>(Param::Trig);

	//------------//
	// The stages //
	//------------//

	/// Bitwig's pre-delay. Zero in the default patch — an envelope that waits is a choice.
	float delay = defaultOf(Param::Delay);
	/// 10 ms. Pinned against stageSeconds() in tests/test_mod_dahdsr.cpp, which is what keeps the
	/// declared number and the taper from drifting apart.
	float attack = defaultOf(Param::Attack);
	float hold = defaultOf(Param::Hold);
	/// 500 ms.
	float decay = defaultOf(Param::Decay);
	/// A LEVEL, not a time, and the only one of the six that is. 0..1.
	float sustain = defaultOf(Param::Sustain);
	/// 250 ms.
	float release = defaultOf(Param::Release);

	//------------//
	// The curves //
	//------------//

	/// The three TIMED stages take a shape; hold and sustain do not, because a flat stage has no
	/// curve to bend. That is Bitwig's own division and it is also the only one that means anything.
	float attackShape = defaultOf(Param::AttackShape);
	float decayShape = defaultOf(Param::DecayShape);
	float releaseShape = defaultOf(Param::ReleaseShape);

	//------------//
	// The extras //
	//------------//

	/// SMOOTHING IS ABOUT THE DESTINATION, NOT ABOUT THE ENVELOPE. A DAHDSR is already continuous;
	/// what is not continuous is what a fast one does at the far end of a route. Point one at Path
	/// and the character is rebuilt every tick, and a 1 ms attack asks the bank for a new build 750
	/// times a second — the one-pole here is what turns that into a glide the fade guard can absorb.
	/// **AT THE BOTTOM OF ITS TRAVEL IT IS OFF**, which is the whole control now — the `smoothingOn`
	/// flag that used to gate it is deleted. Off by default, so the envelope's
	/// shape is the envelope's shape until someone asks otherwise; `mod::smoothingCoeff` is where the
	/// bottom becomes a pass-through, for all three modulators at once.
	float smoothing = defaultOf(Param::Smoothing);

	/// THE ENVELOPE IS ALSO THE LFO — by design, and the reason there is no second
	/// device for the job. The mode decides how: Gate retriggers while held, Loop cycles while held,
	/// Once fires one cycle and holds, Free runs continuously, LFO Retrig runs and restarts on every
	/// note. Replaces the old `loop`, `singleTrigger` and `trigger` toggles, whose combinations
	/// overlapped ("Once" was `SingleTrigger=On + Loop=Off`, and "Free + retrigger" was not
	/// expressible at all).
	Mode mode = static_cast<Mode>(static_cast<int>(defaultOf(Param::Mode)));

	/// LEGATO — the card's "Leg" chip. A new note under a held gate does NOT restart the envelope;
	/// per note, the new note's envelope takes up the newest held one instead of starting from
	/// nothing. **The chip was deleted by accident in the Mode collapse and is back** — see
	/// `EnvCard::chips()`; this comment claimed for a day that it was internal and drew nothing,
	/// which is what a whole reachability guard was told and believed.
	bool singleTrigger = defaultOf(Param::SingleTrigger) >= 0.5f;

	/// BITWIG'S ±. Off, the envelope reads 0..1 and a route pushes its destination one way from
	/// wherever the base sits. On, it reads -1..1, so the sustain level becomes a centre a route
	/// swings either side of.
	bool bipolar = defaultOf(Param::Bipolar) >= 0.5f;

	/// **THE BYPASS SWITCH.** ON by default — see `Param::Enabled`. It sits in `Settings` rather than
	/// only in the row because `rowOf` packs `Settings` and a field missing here would be a row whose
	/// index 15 is silently zero while the descriptor declares 1: a default that disagrees with itself,
	/// which is exactly what `test_mod_dahdsr`'s index-correctness case caught.
	bool enabled = defaultOf(Param::Enabled) >= 0.5f;

	/// THE AMOUNT — carried here so the row round-trips whole; the host reads it, not the envelope.
	float amount = defaultOf(Param::Amount);

	/// POLY — see `Param::Poly`.
	bool poly = defaultOf(Param::Poly) >= 0.5f;

	/// SMOOTH THROUGH RETRIGGER — see `Param::SmoothCarry`.
	bool smoothCarry = defaultOf(Param::SmoothCarry) >= 0.5f;

	/// TRIG — the resolved reading `mod::PolyShape`'s edge detector watches; see `Param::Trig`.
	float trig = defaultOf(Param::Trig);

	/// NOTES FIRE TRIG — see `Param::NotesTrig`.
	bool notesTrig = defaultOf(Param::NotesTrig) >= 0.5f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// ONE SETTINGS BLOCK AS A FLAT ROW, in Param order — the device's own indexed view of itself.
///
/// THIS IS THE FIELD ORDER, AND IT LIVES HERE. A first draft put it in src/plugin/mod_rack.cpp,
/// which was the boundary violation this whole file exists to refuse: the order of a device's own
/// controls is the device's knowledge, and a host that wrote it down would be a host that has to be
/// edited when the device grows a knob.
///
/// THREE THINGS READ IT — the patch stream, the editor→engine channel, and a card walking
/// kParamDescs — and they read the same one, index-aligned with the declarations above. Three
/// hand-written copies of "delay, attack, hold, …" is the collision the state block ORDER already
/// had to be turned into data to stop, one level up.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Row rowOf(const Settings& settings);

/// The way back, from however many of the fields the caller actually has.
///
/// `fieldsPresent` is what makes a short row safe: a patch written by an earlier build carries fewer
/// numbers, and every field past the end keeps its DEFAULT rather than becoming a zero that looks
/// deliberate. A field that arrives as a non-number is treated the same way, and the result is
/// passed through sanitize() before it is returned.
Settings settingsOf(const Row& row, size_t fieldsPresent);

/// One control's value on this device's own 0..1-and-friends terms, by index — what a generic card
/// reads and writes without knowing what a DAHDSR is.
float valueOf(const Settings& settings, Param param);
void setValueOf(Settings* settings, Param param, float value);

/// What a stage's value means in seconds — `exp2`, and nothing else, because the value IS its log2
/// seconds. Kept as a function rather than left to every caller so the one special case has one home:
/// AT THE MINIMUM THE STAGE DOES NOT HAPPEN, and this reads back 0 rather than 0.49 ms to say so.
double stageSeconds(float log2Seconds);

/// The inverse, for a card placing a named time. Clamped to the declared range; zero or less asks
/// for "no stage" and lands on the minimum, so the pair round-trips.
float stageValueFor(double seconds);

/// The smoothing time in seconds — the one-pole's time constant. Same domain, its own range.
double smoothingSeconds(float log2Seconds);

/// EVERY VALUE THAT CAME FROM OUTSIDE, PUT BACK INSIDE ITS RANGE — one door, ParamSet::setBase's
/// door, for the same reason that one has it.
///
/// A patch is a file on somebody's disk and it outlives the build that wrote it. A shape of 40 read
/// out of one would reach the curve law as exp(8·40·40), and a NaN in any of these fields would walk
/// out of the envelope into whatever it is routed to and silence the instrument with nothing to
/// show for it. The loader calls this; so does anything else that hands an envelope settings it did
/// not compute itself.
void sanitize(Settings* settings);

} // namespace feathers::mod::dahdsr
