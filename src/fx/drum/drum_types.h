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

#include "device/registry.h"
#include "dsp/drum_voice.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

//--------------------------------------------------------------------------------------------------//
// DRUM: ONE DRUM, AS A DEVICE IN A CHAIN.                                                            //
//--------------------------------------------------------------------------------------------------//
//
// **A GENERATOR IN A CHAIN.** It takes notes, strikes one hit of one kind (`drum::Hit`, dsp/drum_voice.h) on the
// key it answers, and ADDS the hit to what comes in. Every note passes through it untouched, so two Drums on one
// key sound together: that is how a drum is layered.
//
// **ITS CONTROLS ARE THE DRUM ENGINE'S, ONE LAYER'S WORTH**: the kind's own controls, the terrain that gives the seven
// fields (Zone, Offset, Gamma, Rotate and the unbounded travel X and Y, then the seven scalers), and what a pad has
// around its sound (Note, Level, Pan, Tune, Trigger, Voices, Release, Bend, and the old second layer's Delay and High
// Pass). A control a kind does not read is gated on the Kind and so is not drawn.
//
// **THE ROW IS THE STATE.** `Settings` is the row, each field cleaned into its declared range, so a patch stores each
// value by its key and nothing else; the decode into laws is `drum.cpp`'s.

namespace feathers::fx::drumdevice {

/// THE SCALE OF A DRAG WINDOW: the unbounded coordinates declare this range as a drawing convenience, never a limit
/// (`offsetsMayLeaveRange`, `isDragWindow`).
inline constexpr double kWindow = 8388608.0;

/// Stored by label, so the order may grow.
inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kTriggerNames[] = { "One-shot", "Gate" };
inline constexpr const char* kVoicesNames[] = { "1", "2", "4" };
inline constexpr int kVoicesCounts[] = { 1, 2, 4 };
inline constexpr int kMaxVoices = 4;
inline constexpr const char* kDriveCurveNames[] = { "Tanh", "Tube", "Soft Clip" };
inline constexpr const char* kPitchModeNames[] = { "Free", "Semitones", "Off" };

/// WHICH KINDS READ A CONTROL, as a bit per `feathers::drum::Kind` (Kick 0 .. String 5). `kindReads` and `kindControls`
/// (dsp/drum_voice.h) are the engine's own statement of the same; test_drum_device holds these equal to them.
inline constexpr uint32_t kKickKinds = 1u << 0;
inline constexpr uint32_t kSnareKinds = 1u << 2;
inline constexpr uint32_t kClapKinds = 1u << 4;
inline constexpr uint32_t kSweepKinds = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 5); ///< the Sweep field
inline constexpr uint32_t kSweepTimeKinds = (1u << 0) | (1u << 5);                       ///< Sweep Time
inline constexpr uint32_t kNoiseKinds = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4); ///< Noise, Velocity Noise
inline constexpr uint32_t kMetalKinds = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5);   ///< the Metal field
inline constexpr uint32_t kWidthKinds = (1u << 2) | (1u << 4);                           ///< Width

/// THE ORDER IS THE FACE'S: the first ten are the card, the rest the window. A patch stores each by its key, so
/// reordering is free.
enum class Param : uint8_t {
	Enabled,
	Kind,
	Note,
	Level,
	Pan,
	Tune,
	Decay,
	FieldTone,
	FieldNoise,
	Drive,
	Trigger,
	Voices,
	Release,
	Bend,
	Delay,
	HighPass,
	Seed,
	Zone,
	ZoneOffset,
	Gamma,
	Rotate,
	TravelX,
	TravelY,
	PitchMode,
	FieldPitch,
	FieldLength,
	FieldSweep,
	FieldSnap,
	FieldMetal,
	SweepTime,
	DriveCurve,
	DriveTone,
	VelocityZone,
	VelocityNoise,
	Width,
	Claps,
	Spread,
	ClapQ,
	ClapHighPass,
	WireDecay,
	ClickDecay,
	Wires,
	Bells,
	BellStart,
	BellEnd,
	BellTime,
	Bell2Hz,
	Bell2Time,
	KickHold,
	KickDip,
	KickDipTime,
	ClickSweep,
	ClickSweepTime,
	StartPhase,
	kCount
};
static_assert(static_cast<size_t>(Param::kCount) == 54);

inline constexpr int16_t kKindControl = static_cast<int16_t>(Param::Kind);
inline constexpr int16_t kTriggerControl = static_cast<int16_t>(Param::Trigger);

inline constexpr const char* kTags[] = { "drum", "generator" };

inline constexpr device::FaceSection kFaceSections[] = {
	{ "DRUM", static_cast<uint8_t>(Param::Kind) },
	{ "OUTPUT", static_cast<uint8_t>(Param::Level) },
	{ "BODY", static_cast<uint8_t>(Param::Decay) },
};

inline constexpr device::Traits kTraits {
	.tag = "DRUM",
	.name = "Drum",
	.hasCard = true,
	.hasExpanded = true,
	.perVoiceControl = -1,
	// **ENABLED IS THE ENGINE'S** (`engineEnable`): switched off the drum is parked and the chain's output is the dry.
	.enableControl = static_cast<int>(Param::Enabled),
	.faceControls = 10,
	.faceColumns = 4,
	.tags = kTags,
	.faceSections = kFaceSections,
};

/// A DRUM'S KIND NAMES, in the engine's own order (`feathers::drum::kKindNames`).
inline constexpr const char* kKindLabels[] = { "Kick", "Membrane", "Snare", "Metal", "Clap", "String" };
static_assert(std::size(kKindLabels) == feathers::drum::kKindCount);

inline constexpr device::ParamDesc kParamDescs[] = {
	{.key = "drum_enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	 .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	 .tooltip = "Whether the drum is in the chain"},
	{.key = "drum_kind", .name = "Kind", .min = 0.0, .max = static_cast<double>(feathers::drum::kKindCount - 1), .def = 0.0, .stepped = true,
	 .unit = device::Unit::Choice, .names = kKindLabels, .modulatable = false,
	 .tooltip = "Which drum this is: a pitch-swept Kick, a ringing Membrane, a Snare, a Metal cymbal or hat, a Clap, or a struck String"},
	{.key = "drum_note", .name = "Note", .min = 0.0, .max = 127.0, .def = 36.0, .stepped = true,
	 .unit = device::Unit::Note, .names = {}, .modulatable = false,
	 .tooltip = "The key this drum answers. Every note passes through whatever it does, so several drums on one key sound together"},
	{.key = "drum_level", .name = "Level", .min = -60.0, .max = 6.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
	 .tooltip = "How loud the drum is, added to what comes in"},
	{.key = "drum_pan", .name = "Pan", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "Where the drum sits between the speakers",
	 .centred = true},
	{.key = "drum_tune", .name = "Tune", .min = -48.0, .max = 48.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "The drum's pitch, in semitones from its kind's resting pitch",
	 .centred = true},
	{.key = "drum_decay", .name = "Decay", .min = -6.643856189774724, .max = 3.321928094887362, .def = -1.152003093445050, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How long the body rings: the time it takes to fall 60 dB. The terrain's Length stretches or shortens it"},
	{.key = "drum_f_tone", .name = "Tone", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "Brightness: where it is struck and how high its band sits"},
	{.key = "drum_f_noise", .name = "Noise", .min = 0.0, .max = 1.0, .def = 0.3, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How much of the hit is noise: a snare's wires, a hat's hiss",
	 .gateControl = kKindControl,
	 .gateMask = kNoiseKinds},
	{.key = "drum_drive", .name = "Drive", .min = 0.0, .max = 1.0, .def = 0.2, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How hard the hit is pushed into its Drive Curve: rounder and louder"},
	{.key = "drum_trigger", .name = "Trigger", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	 .unit = device::Unit::Choice, .names = kTriggerNames, .modulatable = false,
	 .tooltip = "One-shot plays the whole hit whatever the key does. Gate damps it to Release when the key is let go"},
	{.key = "drum_voices", .name = "Voices", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
	 .unit = device::Unit::Choice, .names = kVoicesNames, .modulatable = false,
	 .tooltip = "How many hits of this drum may ring at once; one more fades the oldest out"},
	{.key = "drum_release", .name = "Release", .min = -6.643856189774724, .max = 1.0, .def = -3.321928094887362, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "With Trigger on Gate, how quickly the hit is damped once the key is let go",
	 .gateControl = kTriggerControl,
	 .gateMask = 0x2u},
	{.key = "drum_bend", .name = "Bend", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How far a tuning expression on the key moves this drum, held when it is struck or moved while it rings: at 100% the whole move, at 0 the drum ignores it"},
	{.key = "drum_delay", .name = "Delay", .min = 0.0, .max = 10.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "How long, in ms, after the note the drum strikes"},
	{.key = "drum_hp", .name = "High Pass", .min = 4.321928094887363, .max = 11.0, .def = 4.321928094887363, .stepped = false,
	 .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
	 .tooltip = "Takes the lows out of the drum below this: at its bottom, nothing is taken"},
	{.key = "drum_seed", .name = "Seed", .min = 0.0, .max = 65535.0, .def = 0.0, .stepped = true,
	 .unit = device::Unit::Number, .names = {}, .modulatable = false,
	 .tooltip = "Which noise the drum strikes with. Each hit's noise is this seed, the key and the song's place, so the same song plays the same noise"},
	{.key = "drum_zone", .name = "Zone", .min = 0.0, .max = 1023.0, .def = 0.0, .stepped = true,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "The coarse place on the terrain the drum's character is read from",
	 .shortName = "Zone"},
	{.key = "drum_zone_offset", .name = "Offset", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "The fine place inside the zone"},
	{.key = "drum_gamma", .name = "Gamma", .min = -kWindow, .max = kWindow, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "The coarsest move through the terrain: a new family of characters each step",
	 .digitBox = true},
	{.key = "drum_rotate", .name = "Rotate", .min = -kWindow, .max = kWindow, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = false,
	 .tooltip = "Turns the plane the drum travels in, leaving the character at its home where it is",
	 .centred = true,
	 .digitBox = true},
	{.key = "drum_travel_x", .name = "Travel X", .min = -kWindow, .max = kWindow, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .offsetsMayLeaveRange = true,
	 .tooltip = "Where the drum stands across on the terrain: a place added to its home. It has no end",
	 .modSpan = 1.0,
	 .centred = true,
	 .digitBox = true},
	{.key = "drum_travel_y", .name = "Travel Y", .min = -kWindow, .max = kWindow, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .offsetsMayLeaveRange = true,
	 .tooltip = "Where the drum stands up or down on the terrain: a place added to its home. It has no end",
	 .modSpan = 1.0,
	 .centred = true,
	 .digitBox = true},
	{.key = "drum_pitch_mode", .name = "Terrain Pitch", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
	 .unit = device::Unit::Choice, .names = kPitchModeNames, .modulatable = false,
	 .tooltip = "How the terrain's pitch reaches the drum: Free follows it, Semitones rounds it to whole semitones, Off ignores it"},
	{.key = "drum_f_pitch", .name = "Pitch", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "The terrain's fine pitch: the middle is the terrain as found"},
	{.key = "drum_f_length", .name = "Length", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "The terrain's Length, which stretches Decay: the middle is the terrain as found"},
	{.key = "drum_f_sweep", .name = "Sweep", .min = 0.0, .max = 1.0, .def = 0.8, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How far the pitch drops from the strike: the middle is the terrain as found, up asks for more",
	 .gateControl = kKindControl,
	 .gateMask = kSweepKinds},
	{.key = "drum_f_snap", .name = "Snap", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How hard the attack is: a soft mallet to a hard stick"},
	{.key = "drum_f_metal", .name = "Metal", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How inharmonic it rings: a skin toward a bar or a bell",
	 .gateControl = kKindControl,
	 .gateMask = kMetalKinds},
	{.key = "drum_sweep_time", .name = "Sweep Time", .min = -7.643856189774724, .max = -1.0, .def = -4.643856189774724, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How quickly the pitch falls from the top of the sweep to rest",
	 .gateControl = kKindControl,
	 .gateMask = kSweepTimeKinds},
	{.key = "drum_drive_curve", .name = "Drive Curve", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
	 .unit = device::Unit::Choice, .names = kDriveCurveNames, .modulatable = false,
	 .tooltip = "The shape Drive pushes the hit into: Tanh is round, Tube leans to one side and adds even harmonics, Soft Clip flattens its tops"},
	{.key = "drum_drive_tone", .name = "Drive Tone", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "A tilt before the drive: down feeds it the lows, so they saturate; up feeds it the highs. The middle is flat"},
	{.key = "drum_velocity_zone", .name = "Velocity", .min = -8.0, .max = 8.0, .def = 0.6666666666666666, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "What a hit's velocity reaches, through named zones: only Level; and Tone (brightness and snap); and Tension (the glide deepens with velocity squared). Between two zones it moves from one to the next",
	 .modSpan = 1.0},
	{.key = "drum_velocity_noise", .name = "Velocity Noise", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How far a hit's velocity moves its noise share: up, a harder hit is noisier; down, cleaner",
	 .gateControl = kKindControl,
	 .gateMask = kNoiseKinds,
	 .centred = true},
	{.key = "drum_width", .name = "Width", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "How far a Snare's wires or a Clap's noise differ between the ears: at 0 they are the same",
	 .gateControl = kKindControl,
	 .gateMask = kWidthKinds},
	{.key = "drum_claps", .name = "Claps", .min = 1.0, .max = 8.0, .def = 3.0, .stepped = true,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "How many hands a Clap is: the bursts before its tail",
	 .gateControl = kKindControl,
	 .gateMask = kClapKinds},
	{.key = "drum_spread", .name = "Spread", .min = -7.643856189774724, .max = -3.321928094887362, .def = -5.058893689053568, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How far apart a Clap's hands land: tight is one big clap, wide a ragged crowd",
	 .gateControl = kKindControl,
	 .gateMask = kClapKinds},
	{.key = "drum_clap_q", .name = "Clap Q", .min = 0.5, .max = 8.0, .def = 1.6, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "How narrow a Clap's band is: low is a wide, papery slap; high a ringing, hollow one",
	 .gateControl = kKindControl,
	 .gateMask = kClapKinds},
	{.key = "drum_clap_hp", .name = "Clap High Pass", .min = 4.321928094887363, .max = 11.0, .def = 4.321928094887363, .stepped = false,
	 .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
	 .tooltip = "Takes the lows out of a Clap below this: at its bottom, nothing is taken",
	 .gateControl = kKindControl,
	 .gateMask = kClapKinds},
	{.key = "drum_wire_decay", .name = "Wire Decay", .min = -6.643856189774724, .max = 1.0, .def = -2.473931188332412, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How long a snare's wires buzz, on their own: the time they take to fall 60 dB",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_click_decay", .name = "Click", .min = -9.965784284662087, .max = -4.321928094887363, .def = -7.380821783940931, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How long a snare's stick click lasts",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_wires", .name = "Wires", .min = -8.0, .max = 8.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "A Snare's wires through named zones: Tight, Loose, Buzzy (rattling, following the head) and Gated (cut short). Between two zones it moves from one to the next",
	 .modSpan = 1.0,
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_bells", .name = "Bells", .min = -8.0, .max = 8.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "A Snare's two moving bells through named zones: Off, Body (one peak), Shell (two, linked), Ring (narrow and ringing) and Knock (wide, the second free). How narrow follows Snap",
	 .modSpan = 1.0,
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_bell_start", .name = "Bell Start", .min = 6.643856189774724, .max = 13.550746785383243, .def = 11.287712379549449, .stepped = false,
	 .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
	 .tooltip = "Where a snare's moving bell starts: a resonant peak that glides to Bell End",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_bell_end", .name = "Bell End", .min = 6.643856189774724, .max = 13.550746785383243, .def = 9.451211111832329, .stepped = false,
	 .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
	 .tooltip = "Where the moving bell comes to rest",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_bell_time", .name = "Bell Time", .min = -7.643856189774724, .max = -1.0, .def = -4.643856189774724, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How long the bell takes to glide to its end",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_bell2_hz", .name = "Bell 2 Hz", .min = 7.965784284662087, .max = 11.287712379549449, .def = 8.965784284662087, .stepped = false,
	 .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
	 .tooltip = "Where the second bell sits when it is free",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_bell2_time", .name = "Bell 2 Time", .min = -8.380821783940931, .max = -3.643856189774724, .def = -5.058893689053568, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How long the second bell rings: its level falls 60 dB in this time",
	 .gateControl = kKindControl,
	 .gateMask = kSnareKinds},
	{.key = "drum_kick_hold", .name = "Hold", .min = 0.0, .max = 200.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "How long, in ms, a Kick's body stays at full before it decays",
	 .gateControl = kKindControl,
	 .gateMask = kKickKinds},
	{.key = "drum_kick_dip", .name = "Dip", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	 .tooltip = "The fishtail: how deep a Kick's level dips between its snap and its boom",
	 .gateControl = kKindControl,
	 .gateMask = kKickKinds},
	{.key = "drum_kick_dip_time", .name = "Dip Time", .min = 2.0, .max = 80.0, .def = 14.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "Where, in ms, the fishtail's dip is deepest",
	 .gateControl = kKindControl,
	 .gateMask = kKickKinds},
	{.key = "drum_click_sweep", .name = "Click Sweep", .min = 0.0, .max = 6.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "A second, very short pitch drop on a Kick, in octaves: the click of the beater on top of the boom",
	 .gateControl = kKindControl,
	 .gateMask = kKickKinds},
	{.key = "drum_click_sweep_time", .name = "Click Sweep Time", .min = -10.965784284662087, .max = -5.643856189774724, .def = -8.380821783940931, .stepped = false,
	 .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	 .tooltip = "How quickly the click's pitch drop lands",
	 .gateControl = kKindControl,
	 .gateMask = kKickKinds},
	{.key = "drum_start_phase", .name = "Start Phase", .min = 0.0, .max = 90.0, .def = 0.0, .stepped = false,
	 .unit = device::Unit::Number, .names = {}, .modulatable = true,
	 .tooltip = "Where, in degrees, a Kick's sine starts: 0 is smooth; toward 90 the first cycle hits harder",
	 .gateControl = kKindControl,
	 .gateMask = kKickKinds},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// Row: the serialized form of the settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// **THE SETTINGS ARE THE ROW, CLEANED**: each field is its declared default when the row is short or the value is not
/// finite, rounded when stepped, and clamped into its range (a coordinate that may leave its range is clamped to the
/// window and no further).
struct Settings {
	Row v {};

	[[nodiscard]] float at(Param p) const { return v[static_cast<size_t>(p)]; }
	[[nodiscard]] int index(Param p) const { return static_cast<int>(std::lround(at(p))); }
	friend bool operator==(const Settings&, const Settings&) = default;
};

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// THE RENDER PATH AND THE NOTE ROAD, over one shared storage (drum.cpp).
extern const device::AudioOps kAudioOps;
extern const device::NoteOps kNoteOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	// **THE NOTE ROAD SHARES `.audio`'S STORAGE**, as RESONATE's does: `RackEngine` takes the first non-null ops and allocates once.
	.notes = &kNoteOps,
	.category = device::Category::RhythmGlitch,
};

} // namespace feathers::fx::drumdevice
