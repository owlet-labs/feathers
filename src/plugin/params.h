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

#include "dsp/constellation.h"
// THE FX VOCABULARY, AND NO LONGER A DEVICE THAT SPEAKS IT. This was fx/filter/filter_types.h, and
// the swap is the swallowing's clearest structural mark: the table still needs to know what a tenant's
// control DECLARATION looks like — fxRow seats one at an id — but it no longer names any device in the
// fx lane, because a docked device's controls are its own state carried in its chain row. The rack's
// device is still included below, because a modulator's OUTPUT is a source this table addresses, which
// is a different relationship and the one that survives.
#include "fx/fx_tenant.h"
// The rack's device, for its control declarations — the same reason the filter's are included above,
// and the same shape: the destination table is BUILT from what the devices declare rather than from
// a second hand-kept list living here.
#include "device/modulatable.h"
#include "backplane/axes.h"
#include "backplane/route_list.h"
#include "backplane/route_probe.h"
#include "device/tick_window.h"
#include "device/transfer.h"
#include "device/registry.h"
#include "plugin/sound_travel.h"
#include "plugin/sound_weights.h"

#include "clap/ext/params.h"
#include "clap/id.h"
#include "clap/stream.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

// The plugin's parameter contract, deliberately kept out of the CLAP plugin class.
//
// Two reasons. The ids below are permanent API — a session saved by any build of feathers must load
// in every later one — so the table that defines them is worth testing directly, and a test that has
// to stand up a plugin instance needs a host. And the plugin class is then a binding: it forwards
// clap_plugin_params and clap_plugin_state straight to this, and owns no parameter knowledge of its
// own.
//
// Everything a host can see about a param — its id, name, range, default, taper and the text it
// prints — lives in one table, so a range and the text describing it cannot drift apart.

namespace feathers {

/// Stable parameter ids. THESE VALUES ARE PERMANENT: they are what a saved session stores, so a
/// changed id silently loads someone's patch wrong. Add new params with new ids; never renumber.
/// tests/test_params.cpp pins the whole table against accidental edits.
enum class ParamId : uint32_t {
	OscType = 0x0001,
	/// THE WARP SHAPE, ENGINE-WIDE (2026-08-28). Beside OscType because it is the same kind of thing:
	/// a shape the whole engine reads, not a value one sound carries. **It is engine-wide because an
	/// enum cannot be blended** — the flagship renders ONE cycle from the blended recipe, so a
	/// per-sound mode would have to be resolved by dominant weight and would step mid-crossfade. The
	/// DEPTH is per sound (`AWarp`..`HWarp`, +B) and blends like every other amount here.
	WarpMode = 0x0002,
	/// WHAT THE RESONATE ENGINE'S STRING IS (2026-09-25): Ring, Bar, Tube, Membrane or the fifth model
	/// (dsp/weave_models.h). Engine-wide for WarpMode's reason — there is ONE string, so there is one thing
	/// vibrating, whatever the constellation — and live only on Resonate (the honesty rule; the editor
	/// greys it elsewhere).
	ResonateModel = 0x0003,
	/// RESONATE'S OWN SHAPING. Engine-wide, one string: on Resonate these nine take the
	/// Explore disk's surround slots, and on every other engine they are hidden and do nothing. What each does
	/// to the string is dsp/character_bank.h ResonateShaping.
	ResonateWidth = 0x0004,     ///< stereo: a second scan head at an offset round the ring (0 = mono)
	ResonateBow = 0x0005,       ///< x the laws' bow depth, 0..4
	ResonateBowMode = 0x0006,   ///< the laws' own bow mode, or one forced
	ResonateShimmer = 0x0007,   ///< log2 x on the scan smoother's cutoff
	ResonateStiffness = 0x0008, ///< log2 x on the springs home (k)
	ResonateCoupling = 0x0009,  ///< log2 x on the neighbour springs (c)
	ResonateDecay = 0x000A,     ///< log2 of how much LONGER the ring rings (damping x 2^-v)
	ResonatePluck = 0x000B,     ///< x the note-on pluck's amplitude, 0..4
	ResonateTravel = 0x000C,    ///< x the ring's travel rate, 0..4
	/// THE STRING'S TICK RATE: log2 x the firmware's per-128-sample tick, 0..3 (x1..x8), default 1
	/// (x2). WeaveString::setSpeed says why the hardware ran faster than the port's first fixed clock.
	ResonateSpeed = 0x000D,

	AZone = 0x0010,
	AOffset = 0x0011,
	AGamma = 0x0012,

	BZone = 0x0020,
	BOffset = 0x0021,
	BGamma = 0x0022,

	Path = 0x0030,
	PosX = 0x0031,
	PosY = 0x0032,

	// SOUNDS C..H. A and B keep the ids they have had since v1 — ids are permanent — so the block for
	// the other six starts fresh rather than continuing a stride that 0x0030 already occupies. Stride
	// 0x10 per sound and the same +0/+1/+2 field order as A and B, so soundParam() below is one rule
	// with one exception rather than six special cases.
	CZone = 0x0100,
	COffset = 0x0101,
	CGamma = 0x0102,
	DZone = 0x0110,
	DOffset = 0x0111,
	DGamma = 0x0112,
	EZone = 0x0120,
	EOffset = 0x0121,
	EGamma = 0x0122,
	FZone = 0x0130,
	FOffset = 0x0131,
	FGamma = 0x0132,
	GZone = 0x0140,
	GOffset = 0x0141,
	GGamma = 0x0142,
	HZone = 0x0150,
	HOffset = 0x0151,
	HGamma = 0x0152,

	// WHERE EACH SOUND SITS IN THE BLEND PLANE, and whether it is in the constellation at all. A
	// sound's identity (zone/offset/gamma) says what it IS; its placement says where you have to stand
	// to hear it. Every slot has one, including A and B, which had none while the blend was a segment.
	//
	// WIDTH TOOK EACH STRIDE'S FREE +3 SLOT (V13), which is why the four ids of one sound sit 0..3
	// in its block. Categorically it is a RIDE-ALONG since V18  — a
	// setting of the sound's character that travels with the sound, where Place X/Y stay with the
	// slot. The id neighbourhood records where an id was free, not the category.
	APlaceX = 0x0200,
	APlaceY = 0x0201,
	AEnable = 0x0202,
	AWidth = 0x0203,
	BPlaceX = 0x0210,
	BPlaceY = 0x0211,
	BEnable = 0x0212,
	BWidth = 0x0213,
	CPlaceX = 0x0220,
	CPlaceY = 0x0221,
	CEnable = 0x0222,
	CWidth = 0x0223,
	DPlaceX = 0x0230,
	DPlaceY = 0x0231,
	DEnable = 0x0232,
	DWidth = 0x0233,
	EPlaceX = 0x0240,
	EPlaceY = 0x0241,
	EEnable = 0x0242,
	EWidth = 0x0243,
	FPlaceX = 0x0250,
	FPlaceY = 0x0251,
	FEnable = 0x0252,
	FWidth = 0x0253,
	GPlaceX = 0x0260,
	GPlaceY = 0x0261,
	GEnable = 0x0262,
	GWidth = 0x0263,
	HPlaceX = 0x0270,
	HPlaceY = 0x0271,
	HEnable = 0x0272,
	HWidth = 0x0273,

	// THE PER-SOUND FLAGSHIP CONTROLS — FmDepth (+4) and Smoothness (+5) in each sound's 0x0200 block.
	// FmDepth scales that sound's FM contribution (0 = off, 1 = full field-driven FM). Smoothness
	// scales the flagship smoothness field (doc §11, fm-architecture-measurements.md): default 1.0 =
	// full field authority (current verified behavior); 0.0 = plain morph. The two travel together and
	// the stride stays contiguous.
	AFmDepth = 0x0204,
	BFmDepth = 0x0214,
	CFmDepth = 0x0224,
	DFmDepth = 0x0234,
	EFmDepth = 0x0244,
	FFmDepth = 0x0254,
	GFmDepth = 0x0264,
	HFmDepth = 0x0274,
	ASmoothness = 0x0205,
	BSmoothness = 0x0215,
	CSmoothness = 0x0225,
	DSmoothness = 0x0235,
	ESmoothness = 0x0245,
	FSmoothness = 0x0255,
	GSmoothness = 0x0265,
	HSmoothness = 0x0275,

	// THE FIVE FIELD MODIFIER KNOBS — Quantize (+6), Sync (+7), Feedback (+8), Points (+9) and Grit
	// (+A) in each sound's 0x0200 block. Decided 2026-08-25.
	//
	// EACH SCALES THE FIELD THAT AUTHORS ITS QUANTITY, through the one fieldWithOverride curve the
	// Smoothness knob already uses: 0 defeats the field, kFieldNeutral (0.5, the DEFAULT for all
	// five) returns it exactly as the terrain authored it, 1.0 lifts it to full. Neutral is
	// bit-identical to the engine before these existed.
	//
	// THE FIRST FOUR ARE FLAGSHIP FIELDS and reach sound only on V5. GRIT IS NOT: it scales the morph
	// table's ampNoise (slice_build.cpp), which the Morph engine renders too — so it is the one of the
	// five whose control must not be greyed off-V5. The honesty rule read in the direction that gets
	// forgotten.
	AQuantize = 0x0206,
	BQuantize = 0x0216,
	CQuantize = 0x0226,
	DQuantize = 0x0236,
	EQuantize = 0x0246,
	FQuantize = 0x0256,
	GQuantize = 0x0266,
	HQuantize = 0x0276,

	ASync = 0x0207,
	BSync = 0x0217,
	CSync = 0x0227,
	DSync = 0x0237,
	ESync = 0x0247,
	FSync = 0x0257,
	GSync = 0x0267,
	HSync = 0x0277,

	AFeedback = 0x0208,
	BFeedback = 0x0218,
	CFeedback = 0x0228,
	DFeedback = 0x0238,
	EFeedback = 0x0248,
	FFeedback = 0x0258,
	GFeedback = 0x0268,
	HFeedback = 0x0278,

	APoints = 0x0209,
	BPoints = 0x0219,
	CPoints = 0x0229,
	DPoints = 0x0239,
	EPoints = 0x0249,
	FPoints = 0x0259,
	GPoints = 0x0269,
	HPoints = 0x0279,

	AGrit = 0x020A,
	BGrit = 0x021A,
	CGrit = 0x022A,
	DGrit = 0x023A,
	EGrit = 0x024A,
	FGrit = 0x025A,
	GGrit = 0x026A,
	HGrit = 0x027A,

	// THE WARP DEPTH KNOB (+B), one per sound.
	//
	// AN AMOUNT, NOT A BIAS — so its default is 0 and not kFieldNeutral, unlike the five above it.
	// There is no field behind it to return "as authored", which is what 0.5 means for those. Same
	// shape as FmDepth (+4), and dsp/warp.h says why in full.
	AWarp = 0x020B,
	BWarp = 0x021B,
	CWarp = 0x022B,
	DWarp = 0x023B,
	EWarp = 0x024B,
	FWarp = 0x025B,
	GWarp = 0x026B,
	HWarp = 0x027B,

	// THE ADDITIVE SHAPE AMOUNT (+C), one per sound (2026-10-08: one knob plus a dropdown, Warp's pattern). AN AMOUNT, Warp's kind, so its default is 0: at 0 the sound
	// is its terrain spectrum to the bit, at 1 it IS the engine-wide `AdditiveShape`. Live only on Additive (the
	// honesty rule; the knob is not drawn elsewhere).
	AShapeAmount = 0x020C,
	BShapeAmount = 0x021C,
	CShapeAmount = 0x022C,
	DShapeAmount = 0x023C,
	EShapeAmount = 0x024C,
	FShapeAmount = 0x025C,
	GShapeAmount = 0x026C,
	HShapeAmount = 0x027C,

	// THE ADDITIVE BIAS KNOBS (2026-10-10): Rolloff (+0), Formant (+1), Odd/Even (+2), Comb (+3), Detail (+4) and Off-grid (+5), in A SECOND
	// PER-SOUND BLOCK at 0x0280 + slot·0x10, because the 0x0200 block has only +D..+F free and six do not fit there.
	// 0x0280..0x02FF was unused and nothing addresses per-sound ids by range (the stride helpers below are the only
	// readers); +6..+F stay free for the next per-sound control. BIASES, each fieldWithOverride over its terrain field
	// (dsp/slice_build.h PartialBias names them), so all six default to kFieldNeutral (0.5), the terrain as authored.
	// Live only on Additive (the honesty rule; the knobs are not drawn elsewhere).
	ARolloff = 0x0280,
	BRolloff = 0x0290,
	CRolloff = 0x02A0,
	DRolloff = 0x02B0,
	ERolloff = 0x02C0,
	FRolloff = 0x02D0,
	GRolloff = 0x02E0,
	HRolloff = 0x02F0,

	AFormant = 0x0281,
	BFormant = 0x0291,
	CFormant = 0x02A1,
	DFormant = 0x02B1,
	EFormant = 0x02C1,
	FFormant = 0x02D1,
	GFormant = 0x02E1,
	HFormant = 0x02F1,

	AOddEven = 0x0282,
	BOddEven = 0x0292,
	COddEven = 0x02A2,
	DOddEven = 0x02B2,
	EOddEven = 0x02C2,
	FOddEven = 0x02D2,
	GOddEven = 0x02E2,
	HOddEven = 0x02F2,

	AComb = 0x0283,
	BComb = 0x0293,
	CComb = 0x02A3,
	DComb = 0x02B3,
	EComb = 0x02C3,
	FComb = 0x02D3,
	GComb = 0x02E3,
	HComb = 0x02F3,

	ADetail = 0x0284,
	BDetail = 0x0294,
	CDetail = 0x02A4,
	DDetail = 0x02B4,
	EDetail = 0x02C4,
	FDetail = 0x02D4,
	GDetail = 0x02E4,
	HDetail = 0x02F4,

	AOffGrid = 0x0285,
	BOffGrid = 0x0295,
	COffGrid = 0x02A5,
	DOffGrid = 0x02B5,
	EOffGrid = 0x02C5,
	FOffGrid = 0x02D5,
	GOffGrid = 0x02E5,
	HOffGrid = 0x02F5,

	// THE STRING'S FIELD SCALERS (2026-10-31): Brightness (+6), Sustain (+7),
	// Pick (+8), Noise (+9) and Stretch (+A) in the second per-sound block, beside the Additive bias knobs. Each is
	// fieldWithOverride over its string field (dsp/slice_build.h StringBias), so all five default to kFieldNeutral. Live
	// only on the Karplus-Strong engine (the honesty rule).
	AKsBrightness = 0x0286,
	AKsSustain = 0x0287,
	AKsPick = 0x0288,
	AKsNoise = 0x0289,
	AKsStretch = 0x028A,

	Relief = 0x0040,
	GlobalGamma = 0x0041,
	Weather = 0x0042,
	Width = 0x0043,

	// THE BASIC SHAPES MIX (2026-10-03): four engine-wide amounts, the
	// only net-new controls the engine has.
	// Normalised by their sum, with a small sine bias so all four at zero is a sine (dsp/basic_shapes.h).
	ShapesSine = 0x0044,
	ShapesTriangle = 0x0045,
	ShapesSaw = 0x0046,
	ShapesSquare = 0x0047,
	/// THE ADDITIVE SHAPE (design §4.1): Sine / Triangle / Saw / Square, ENGINE-WIDE for WarpMode's reason — each sound's
	/// Shape Amount blends toward ONE exact band-limited target, and an enum cannot be blended. Default Saw. Live only on
	/// Additive, where it is the box in Warp's mode-box slot.
	AdditiveShape = 0x0048,
	/// **A-PH: THE ADDITIVE ENGINE'S PHASES** (design §8;): Natural / Headroom / Sub priority, engine-wide (one bank
	/// of phases a note), live only on Additive. Sub and Ceiling are live only under Sub priority (the honesty rule).
	AdditivePhases = 0x0049,
	AdditiveSub = 0x004A,     ///< dB: the fundamental over the sound's own balance, applied after the RSS normalisation
	AdditiveCeiling = 0x004B, ///< dBFS: the per-voice peak a Sub-priority voice is scaled down to, never up
	/// **OE: THE ENGINE VOCODER'S ANALYSIS** (additive design §9.4), engine-wide because the sidechain is analysed once for every
	/// voice: the Spectral effect's Resolution, Bands, Attack and Release, and Freeze with its Key Track. Live only on Additive with
	/// some sound's Vocode on (Key Track only while Freeze is): the honesty rule.
	AdditiveVocodeResolution = 0x004C,
	AdditiveVocodeBands = 0x004D,
	AdditiveVocodeAttack = 0x004E, ///< ms
	AdditiveVocodeRelease = 0x004F, ///< ms
	AdditiveVocodeFreeze = 0x0055, ///< after Level: 0x0050/0x0051 stay unanswered (test_synth pins the deleted Attack/Release)
	AdditiveVocodeKeyTrack = 0x0056,
	/// OE2 (additive design §9.8): the banded road's zones and the stereo, ENGINE-WIDE (choices: an enum cannot morph; Stereo also
	/// decides what the one analysis measures), and Sibilance (one noise layer for the synth).
	AdditiveVocodeSpacing = 0x0057,
	AdditiveVocodeWidths = 0x0058,
	AdditiveVocodeStereo = 0x0059,
	AdditiveVocodeSibilance = 0x005A, ///< 0..1: the synth's one noise layer, gated by the vocoding voices

	/// **THE KARPLUS-STRONG ENGINE'S CONTROLS** (2026-10-31, KS design §4.1), engine-wide: every voice's string reads them.
	/// In 0x00C0, the block after the macros' (0x0080..0x00BF), which was unused. Live only on the Karplus-Strong engine.
	KsDecay = 0x00C0,     ///< log2 seconds: the T60 of a C3's fundamental; at the top, ring
	KsDecayTrack = 0x00C1,///< 0..1: how much shorter high notes ring, (f0 / C3)^-track
	KsRelease = 0x00C2,   ///< log2 seconds: the T60 a released string falls to; at the top, ring on
	KsHardness = 0x00C3,  ///< 0..1: the pluck noise's brightness
	KsPickup = 0x00C4,    ///< 0.02..0.5: where the pickup sits along the string
	KsWidth = 0x00C5,     ///< 0..0.3: how far apart the two ears' pickups sit
	KsSeed = 0x00C6,      ///< 0..999: which plucks the noise draws
	KsVelocity = 0x00C7,  ///< 0..1: how far a note's velocity moves its pluck's Hardness

	/// **THE DRUM ENGINE'S ENGINE-WIDE CONTROLS** (2026-11-05), after
	/// Pluck's in the same unused block. Each pad's own controls are the per-sound block at kDrumBlockBase (drumParam).
	DrumMode = 0x00C8,        ///< Pitched | Kit
	DrumPitchedKind = 0x00C9, ///< the kind every voice plays in Pitched mode
	DrumWalkX = 0x00CA,       ///< -1..1: the Kit Walk, every pad's place moved together (D3)
	DrumWalkY = 0x00CB,
	DrumSeed = 0x00CC,        ///< 0..999: which hits the noise draws
	/// **THE KIT-WIDE LAYER** (2026-11-06): neutral by default, so a kit is bit-identical until it moves. Kit Gamma and
	/// Kit Offset add to every pad's phase offset, Kit Zone to every pad's zone; the seven trims apply the per-pad scaler's own
	/// law (`fieldWithOverride`) on top of each pad's scaler. Kit mode only, as the Walk is.
	DrumKitGamma = 0x00CD,
	DrumKitZone = 0x00CE,
	DrumKitOffset = 0x00CF,
	DrumKitPitch = 0x00D0,
	DrumKitLength = 0x00D1,
	DrumKitSweep = 0x00D2,
	DrumKitTone = 0x00D3,
	DrumKitNoise = 0x00D4,
	DrumKitSnap = 0x00D5,
	DrumKitMetal = 0x00D6,

	/// **VOICE LEVEL — EACH NOTE'S GAIN, AND WHAT KEEPS IT SOUNDING**. The id the old Level wore, because this IS what that Level did: per voice, per note id, the default
	/// envelope's destination, the stack's "osc volume" — and a released voice is held while a per-voice source
	/// still sounds it here. 0x0050/0x0051 were Attack and Release, deleted the same day: the envelope is the
	/// voice's shape, and the edge left behind is a fixed de-click (`kVoiceEdgeSeconds`).
	VoiceLevel = 0x0052,
	MasterGain = 0x0053,
	/// **LEVEL — A PLAIN FADER** on the voices' sum, before the fx lane: unity by default, no default route, one
	/// value for every voice.
	Level = 0x0054,

	VoiceMode = 0x0060,
	VoiceCount = 0x0061,
	NotePriority = 0x0062,
	GlideTime = 0x0063,
	/// WHAT A NOTE-ON DOES TO THE RUNNING PHASE — three-way since V24, same id. Digi restarts, Analog
	/// leaves it absolutely alone, Phase Catchup bends it home over ~24 cycles (§5.8, V21c/V21d).
	/// DEFAULT CATCHUP: de-clicked retriggers are the feature's point. See kOscModeNames for the
	/// decision and for why widening this id cost nothing.
	OscMode = 0x0064,
	/// VOICE STACKING (backlog KE). How many oscillators one note
	/// plays, and how far apart in pitch they are fanned. In the voice block because a stack is part of
	/// what a voice IS, beside the count and the mode that already say the rest.
	StackCount = 0x0065,
	StackDetune = 0x0066,
	/// How far apart in the stereo field the stacked parts sit (step 2).
	StackSpread = 0x0067,
	/// THE STACK INDEX'S ZONE KNOBS (step 3): the Shape that gives each part its value, that Shape's amount (or
	/// Free's phase), the Mapping that decides which part gets which value and its amount, and the Curve that
	/// warps them all. They reach the sound only through a route from the Stack Index source (voice-stacking doc
	/// §3.2-3.3, `stack_index.h`).
	StackShape = 0x0068,
	StackShapeAmount = 0x0069,
	StackFreePhase = 0x006A,
	StackMap = 0x006B,
	StackMapAmount = 0x006C,
	StackCurve = 0x006D,
	/// **PHASE — AN OFFSET ON THE OSCILLATOR'S READ PHASE, 1.0 = ONE CYCLE** (2026-10-03: a slow modulator on
	/// it is deterministic drift, a fast one is "hacky FM"). Added to the read, never to the accumulator, so Digi
	/// starts at it, Analog reads the song's phase plus it and Catchup bends home to it with no code of their own.
	/// Resolved per sample (the window road) and unclamped, because phase wraps.
	OscPhase = 0x006E,
	/// **PHASE WIDTH — PHASE'S MODULATION, MIRRORED INTO THE RIGHT EAR**: left reads base + m,
	/// right base + m·(1 − 2w), where m is what the routes add. 0 is mono, 1 is the drift going opposite ways in the
	/// two ears — the FM stereo flip's arithmetic (osc_voice.cpp). Nothing to mirror without a route into Phase.
	OscPhaseWidth = 0x006F,

	TransposeOct = 0x0070,
	TransposeSemi = 0x0071,
	FineTune = 0x0072,
	BendRange = 0x0073,
	/// **COARSE — A CONTINUOUS PITCH OFFSET IN SEMITONES**: Fine Tune's road at Transpose's
	/// reach, so a modulator can sweep the pitch wide and smoothly, which the stepped Transpose Oct/Semi cannot.
	/// Resolved per voice, beside Fine Tune, in `Synth::semitonesForKey`.
	Coarse = 0x0074,

	// THE SPECTRAL DRAWER IS PER SOUND: its ids are a block of their own at 0x0400 + slot·0x20,
	// addressed by `spectralParam(slot, control)` below, not named here one by one.

	// THE MODULATION SOURCES, one 0x10 block per CARD in the modulator strip — the same stride the sound
	// slots use, and for the same reason: a block per addressable unit, its fields inside it.
	//
	// EACH MACRO IS A CARD, SO EACH MACRO IS A BLOCK, and that is a correction made before anything
	// shipped. The four ids first sat together inside one block, as though "Macros" were one card with
	// four knobs, while the strip drew four cards with four arrows. The id shape and the surface
	// disagreed about what a source IS, and the surface was right: the card is the unit a hand points
	// with. A φ LFO card lands at 0x00C0 with power, rate and depth as +0/+1/+2 inside its own block,
	// and this rule does not have to bend to let it in.
	//
	// ONE FIELD EACH, AND UNIPOLAR. A macro is a hand-set position, not a signal: the DIRECTION it
	// pushes lives in the route's amount, which is signed and reaches ±200%. A bipolar source would be a
	// second way to say the same thing, and the two would disagree on the glass — the ring is drawn from
	// the base toward base+amount, which has one honest reading only while the source runs 0 → 1. It
	// also keeps "at rest" and "turned all the way down" the same state, which is what makes a macro
	// that is routed but untouched provably silent.
	//
	// What a source is ROUTED to spends no id at all, and never will — see ModRoutes below.
	Macro1 = 0x0080,
	Macro2 = 0x0090,
	Macro3 = 0x00A0,
	Macro4 = 0x00B0,

	// THE FX LANE'S TENANTS, one 0x10 block per device at stride 0x0300 + n·0x10 — a block per
	// addressable unit, the same rule the sound slots and the modulator cards follow.
	//
	// THE ROWS FOR THESE ARE BUILT, NOT WRITTEN. Their names, ranges, defaults and units come from
	// the device's own descriptors under src/fx (see fxRow() in params.cpp), because a tenant that
	// declared its controls in one place and had them retyped by hand in another would have two
	// places for a range to be wrong. What the PLUGIN decides is where in the id space a device
	// lands and how its unit prints; what the DEVICE decides is what its controls are.
	//
	// The order inside a block is the device's declaration order, so appending is the only safe edit
	// on that side too — reordering the module's Param enum would renumber every saved patch.
	// THE FILTER'S IDS ARE GONE — 0x0300..0x030A, SWALLOWED 2026-08-23.
	//
	// A docked device's
	// controls are its own state, carried in its chain row and reached by the route table. They are
	// not parameters of Feathers, so the host never sees them and this table does not name them.
	//
	// THE BLOCK IS NOT RESERVED AND MUST NOT BE. Ids are forever WHILE A HOST PROJECT EXISTS, and
	// leaving a hole here to honour ids nothing can reach would be exactly the wart the project's
	// patch-compatibility policy says to refuse: take the clean design and break the format.
	// A future tenant may have 0x0300.
	//
	// WHAT THIS COSTS, STATED PLAINLY: the filter's ten controls disappear from the host's parameter
	// list, so DAW automation lanes written against them stop resolving, and every modulation
	// destination that sat AFTER them in this table shifts index — which is what a saved routing
	// stores. Both are breaking, both are sanctioned by the same policy (no saved patches, no other
	// users), and both stop being free the day that policy expires.
	// 0x030A..0x030F are the filter's own remaining slots, and Mix takes 0x030A if it is next.
	//
	// THE BLOCK IS CONTIGUOUS BY CONSTRUCTION, which is why the note that used to sit here could not
	// mean what it said: it promised 0x0308 to a Mix control that did not exist. Nothing can hold a
	// slot open. The rows are built from the device's descriptor list BY POSITION (fxRow below, and
	// appendModDestinations computing base + index), and the static_assert under the table requires
	// the block and the list to be one contiguous length — so whatever the device appends next takes
	// the next id, whatever it is called. Slope and Spread appended, so they took 0x0308 and 0x0309;
	// an allpass type becomes honest the day a Mix control exists, at whichever id is free then.
};

// The stepped params' choice lists. Enum and names sit together, and the static_assert under each
// pair is what keeps them together: a value added to one and not the other stops the build rather
// than shipping a menu entry that selects nothing, or an engine nothing can select.
//
// The names are also the wire format for text conversion, but NOT for state — a patch stores the
// index, so these strings can be reworded freely and the entries themselves must never be reordered.

/// Which character engine both sounds are built from. Extensible: appended entries must not
/// disturb existing indices — a patch stores the index.
///
/// V5 is the flagship (working name until it is named at its first sound — assumption V1; the
/// string is display-only, so the real name lands at zero migration cost). It is THE DEFAULT
/// (decided 2026-08-18): fresh instances get it; existing patches carry their stored
/// index and are untouched. Its recipe is the morph recipe through the slice decode with the
/// operator fields applied at build time, and it
/// renders on the morph wavetable path, so it adds no OscEngine of its own. Morph and Stair are the
/// frozen, backport-safe species (§5.0), golden-tested and untouched by flagship work.
///
/// RESONATE (2026-09-25) is the fourth: the firmware's PhiWeave scanned string, ported — ONE string shared
/// by every voice and plucked by each note, each voice scanning it at its own pitch (dsp/weave_string.h).
/// Its user-facing name is a product decision; the physics underneath keeps the firmware's (phi_weave, WeaveString).
///
/// BASIC SHAPES (2026-10-03) is the fifth: Sine / Triangle / Saw / Square from band-limited mip ladders, mixed
/// by four engine-wide knobs, with V5's FM, Warp and Grit on top (OscEngine::Shapes; dsp/basic_shapes.h).
///
/// ADDITIVE (2026-10-10) is the sixth: the terrain authors a SPECTRUM, and each voice renders it as a bank of up to
/// 320 sine partials at host rate (OscEngine::Partials). No Phi
/// prefix, as Basic Shapes; the name lands at first sound.
///
/// PLUCK (2026-10-31) is the seventh: Karplus-Strong, each voice its own plucked string whose length is the pitch, the
/// pluck's tone the Additive spectrum at the same place and the string's material five fields beside it
/// (OscEngine::String; dsp/ks_string.h). A working name.
///
/// DRUM (2026-11-05) is the eighth: a drum synthesiser of six kinds (Kick, Membrane, Snare, Metal, Clap, String), played
/// chromatically in Pitched mode or as a KIT, each of sounds A-H a pad with its own note, kind and terrain place
/// (OscEngine::Drum; dsp/drum_voice.h). A working name.
enum class OscType : uint8_t { Morph, Stair, V5, Resonate, BasicShapes, Additive, Pluck, Drum, kCount };
/// A stepped param value as an index into a list of `count` entries.
///
/// Rounds and then clamps, in that order and both unconditionally. setBase already rounds and clamps
/// everything that comes from outside, so in a running plugin this is a formality — but resolved() is
/// a sum, valueToText is handed values the plugin never stored, and an index derived without the
/// clamp reads off the end of a name array on the one path that skipped a door.
///
/// Public because the EDITOR needs the same rule. A control that rounds a stepped value its own way
/// is a control that can disagree with the engine about which option is selected, and there were
/// three copies of this arithmetic before it was hoisted.
size_t steppedIndex(double value, size_t count);

inline constexpr std::array<const char* const, 8> kOscTypeNames{"Morph", "Stair", "V5", "Resonate", "Basic Shapes",
                                                                "Additive", "Pluck", "Drum"};

/// THE ADDITIVE SHAPE's choices, in `partials::ShapeKind`'s order: character_params.h static_asserts the count where the
/// index is cast to a ShapeKind, and test_params pins each label.
inline constexpr std::array<const char* const, 4> kAdditiveShapeNames{"Sine", "Triangle", "Saw", "Square"};
/// **THE CORE'S RETIRED KEYS** — a key whose meaning changed is never reused (the ledger's `retired core` lines).
inline constexpr device::RetiredKey kCoreRetiredKeys[] = {
	{ "additive.sub", "2026-10-18",
	  "Sub became a LEVEL against the rest's energy (A-PH3), not a raise over the sound's own balance: additive.sub_level" },
	{ "terrain.likes", "2026-10-27", "each like gained its engine and sound (backlog ON: likes leaked across engines): terrain.likes.v2" },
	{ "terrain.likes.v2", "2026-10-27", "a like stores its 48-bin sixth-octave spectrum, not 12 harmonic magnitudes (the Hellinger fingerprint): terrain.likes.v3" },
	{ "terrain.likes.v3", "2026-10-28", "a like stores its FULL POINT in its engine's triangle space, so its place follows a rotate, a gamma or a dial (backlog ON): terrain.likes.v4" },
	{ "terrain.likes.v4", "2026-10-28", "a like stores its phase-warp angles as heard, which the P-MOBIUS likes fit reads: terrain.likes.v5" },
	{ "terrain.likes.v5", "2026-10-28", "a like stores the angles of EVERY engine's sites (Stair, Resonate and the vowel joined the phase warp): terrain.likes.v6" },
	{ "terrain.likes.v6", "2026-10-31", "a like stores the Karplus-Strong string's five material sites too (Pluck joined the phase warp): terrain.likes.v7" },
	{ "terrain.likes.v7", "2026-11-05", "a like stores the drum engine's seven sites and, on Drum, its kind (the drum joined the phase warp): terrain.likes.v8" },
	{ "vocode.freeze", "2026-10-27", "the engine vocoder's held envelope carries the right channel too (OE2 Stereo), a new layout: vocode.held" },
	{ "a.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): a.drum.velocity_zone" },
	{ "a.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): a.drum.velocity_zone" },
	{ "a.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): a.drum.wires" },
	{ "a.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): a.drum.wires" },
	{ "a.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): a.drum.wires" },
	{ "a.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): a.drum.wires" },
	{ "a.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): a.drum.wires" },
	{ "a.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "a.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "a.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "a.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "a.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): a.drum.bells" },
	{ "a.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): a.drum.bells" },
	{ "a.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): a.drum.bells" },
	{ "b.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): b.drum.velocity_zone" },
	{ "b.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): b.drum.velocity_zone" },
	{ "b.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): b.drum.wires" },
	{ "b.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): b.drum.wires" },
	{ "b.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): b.drum.wires" },
	{ "b.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): b.drum.wires" },
	{ "b.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): b.drum.wires" },
	{ "b.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "b.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "b.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "b.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "b.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): b.drum.bells" },
	{ "b.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): b.drum.bells" },
	{ "b.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): b.drum.bells" },
	{ "c.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): c.drum.velocity_zone" },
	{ "c.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): c.drum.velocity_zone" },
	{ "c.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): c.drum.wires" },
	{ "c.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): c.drum.wires" },
	{ "c.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): c.drum.wires" },
	{ "c.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): c.drum.wires" },
	{ "c.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): c.drum.wires" },
	{ "c.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "c.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "c.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "c.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "c.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): c.drum.bells" },
	{ "c.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): c.drum.bells" },
	{ "c.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): c.drum.bells" },
	{ "d.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): d.drum.velocity_zone" },
	{ "d.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): d.drum.velocity_zone" },
	{ "d.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): d.drum.wires" },
	{ "d.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): d.drum.wires" },
	{ "d.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): d.drum.wires" },
	{ "d.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): d.drum.wires" },
	{ "d.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): d.drum.wires" },
	{ "d.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "d.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "d.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "d.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "d.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): d.drum.bells" },
	{ "d.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): d.drum.bells" },
	{ "d.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): d.drum.bells" },
	{ "e.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): e.drum.velocity_zone" },
	{ "e.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): e.drum.velocity_zone" },
	{ "e.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): e.drum.wires" },
	{ "e.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): e.drum.wires" },
	{ "e.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): e.drum.wires" },
	{ "e.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): e.drum.wires" },
	{ "e.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): e.drum.wires" },
	{ "e.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "e.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "e.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "e.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "e.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): e.drum.bells" },
	{ "e.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): e.drum.bells" },
	{ "e.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): e.drum.bells" },
	{ "f.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): f.drum.velocity_zone" },
	{ "f.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): f.drum.velocity_zone" },
	{ "f.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): f.drum.wires" },
	{ "f.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): f.drum.wires" },
	{ "f.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): f.drum.wires" },
	{ "f.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): f.drum.wires" },
	{ "f.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): f.drum.wires" },
	{ "f.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "f.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "f.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "f.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "f.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): f.drum.bells" },
	{ "f.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): f.drum.bells" },
	{ "f.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): f.drum.bells" },
	{ "g.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): g.drum.velocity_zone" },
	{ "g.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): g.drum.velocity_zone" },
	{ "g.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): g.drum.wires" },
	{ "g.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): g.drum.wires" },
	{ "g.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): g.drum.wires" },
	{ "g.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): g.drum.wires" },
	{ "g.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): g.drum.wires" },
	{ "g.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "g.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "g.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "g.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "g.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): g.drum.bells" },
	{ "g.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): g.drum.bells" },
	{ "g.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): g.drum.bells" },
	{ "h.drum.velocity_tone", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): h.drum.velocity_zone" },
	{ "h.drum.velocity_tension", "2026-11-07", "absorbed into the Velocity zone (drum proposal §14 lever 2): h.drum.velocity_zone" },
	{ "h.drum.body_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): h.drum.wires" },
	{ "h.drum.wire_shape", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): h.drum.wires" },
	{ "h.drum.wire_contour", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): h.drum.wires" },
	{ "h.drum.buzz", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): h.drum.wires" },
	{ "h.drum.buzz_threshold", "2026-11-07", "absorbed into the Wires zone (drum proposal §14 lever 2): h.drum.wires" },
	{ "h.drum.wire_sweep", "2026-11-07", "yielded to the terrain: the Snare's Sweep field (drum proposal §14 lever 1)" },
	{ "h.drum.bell_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "h.drum.bell2_q", "2026-11-07", "yielded to the terrain: the place's Snap, scaled by the Bells zone (drum proposal §14 lever 1)" },
	{ "h.drum.bell2_ratio", "2026-11-07", "yielded to the terrain: the place's Metal (drum proposal §14 lever 1)" },
	{ "h.drum.bell_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): h.drum.bells" },
	{ "h.drum.bell2_level", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): h.drum.bells" },
	{ "h.drum.bell2_link", "2026-11-07", "absorbed into the Bells zone (drum proposal §14 lever 2): h.drum.bells" },
};
/// A-PH's three ways with the phases, in `partials::PhaseMode`'s order (test_params pins it).
inline constexpr std::array<const char* const, 3> kAdditivePhaseNames{"Natural", "Headroom", "Sub priority"};
/// OE: the engine vocoder's analysis stops, in fx::spectral::Resolution's order (the Spectral effect's own labels).
inline constexpr std::array<const char* const, 3> kAdditiveVocodeResolutionNames{"Fast", "Balanced", "Fine"};
/// OE2: the Spacing and Widths zones, in the Spectral effect's order and with its labels (fx/spectral kZoneNames, kWidthZoneNames;
/// test_params pins them equal), and the stereo modes (its kVocodeStereoNames).
inline constexpr std::array<const char* const, 7> kAdditiveVocodeSpacingNames{"Even", "Speech", "Low", "High", "Harmonic", "Inharmonic", "Free"};
inline constexpr std::array<const char* const, 6> kAdditiveVocodeWidthsNames{"Even", "Narrow Low", "Narrow High", "Harmonic", "Inharmonic", "Free"};
inline constexpr std::array<const char* const, 3> kAdditiveVocodeStereoNames{"Mono", "Stereo", "Spread"};

/// THE SPECTRAL FILTER's modes, in `spectral::FilterMode`'s order (Synth::seatDrawers casts the index; test_params pins
/// each label). Drawn as a SegmentedControl, all five on the face. Bell is F2's (additive design §3.6).
inline constexpr std::array<const char* const, 5> kSpectralFilterModeNames{"Low-pass", "High-pass", "Band-pass", "Notch", "Bell"};
/// F2's routing of the two filters, Series (the gains multiply) or Parallel (their held sum): a two-state SwitchChip.
inline constexpr std::array<const char* const, 2> kSpectralRoutingNames{"Series", "Parallel"};
/// THE PARTIALS OPERATOR'S CULL SCHEMES (backlog OH/OI), in spectral::PartialsScheme's order and with the Spectral device's labels
/// (fx/spectral kPartialsModeNames): the dropdown of the Partials mode cell on both surfaces. Stored by label, so OI's schemes append.
inline constexpr std::array<const char* const, 3> kSpectralPartialsModeNames{"Peaks", "Harmonics", "Centers"};

/// **THE SPECTRAL DRAWER, PER SOUND** (2026-10-10: each sound has its own drawer, and the voice runs ONE drawer
/// whose settings are the sounds' blended by Explore's weights — Synth::drawerFor; additive design §3.3, §4.2). The
/// v1 operators' controls, in the order the drawer's operator row draws them: each operator's switch, then its
/// controls. Filter Mode and Link are stepped choices; every other non-switch control is continuous, a modulation
/// destination (in each slot's destination run) resolved per voice.
enum class SpectralControl : uint8_t {
	FilterOn, FilterMode, FilterCutoff, FilterResonance, FilterSlope, FilterKeyTrack,
	CombOn, CombSpacing, CombDepth, CombPhase,
	FormantShiftOn, FormantShiftLow, FormantShiftHigh, FormantShiftCrossover, FormantShiftLink,
	TiltOn, TiltAmount,
	OddEvenOn, OddEvenBalance,
	FormantOn, FormantVowel, FormantDepth,
	PartialsOn, PartialsCount,
	DampOn, DampTime, DampTilt,
	StretchOn, StretchAmount,
	ShiftOn, ShiftHz,
	// F2 (additive design §3.6): the Bell's Gain on filter 1, the second filter, and the routing — the SECOND id block.
	FilterGain,
	Filter2On, Filter2Mode, Filter2Cutoff, Filter2Resonance, Filter2Slope, Filter2KeyTrack, Filter2Gain,
	FilterRouting,
	// OH: the Partials operator's cull scheme, its mode cell's dropdown (the second block too).
	PartialsMode,
	// OI: Centers' set, the Partials cell's other box (read only in Centers).
	PartialsSet,
	// OF: how far the Partials cull turns the rest down (the Spectral device's Partials Depth): 1, the default, is the cull as it was.
	PartialsDepth,
	// OE (additive design §9.4): the engine vocoder's per-sound controls — the THIRD id block.
	VocodeOn, VocodeDepth, VocodeFlatten, VocodeGate, VocodeLow, VocodeHigh,
	// OE2 (additive design §9.8): the banded road per sound — Bandwidth, and the Spacing and Widths zones' amounts and Free coordinates
	// (the zones themselves are engine-wide choices: an enum cannot morph).
	VocodeBandwidth, VocodeSpacingAmount, VocodeSpacingFree, VocodeWidthsAmount, VocodeWidthsFree,
	kCount
};
inline constexpr size_t kSpectralControlCount = static_cast<size_t>(SpectralControl::kCount);
/// The controls in the first id block (every one before F2's).
inline constexpr size_t kSpectralFirstBlockCount = static_cast<size_t>(SpectralControl::FilterGain);
/// The first control of the THIRD id block (OE, 2026-10-26): 0x0580 + slot·0x10, in SpectralControl's order from VocodeOn.
inline constexpr size_t kSpectralThirdBlockFirst = static_cast<size_t>(SpectralControl::VocodeOn);

/// THE ID LAYOUT: a 0x20 block per sound at 0x0400 + slot·0x20, the controls at +0..+0x12 in SpectralControl's order and
/// +0x13..+0x15 the vowel Formant's, +0x16..+0x17 Partials', +0x18..+0x1A Damp's, +0x1B..+0x1C Stretch's, +0x1D..+0x1E
/// Shift's, +0x1F free.
/// 0x0400..0x04FF was free on every branch (checked 2026-10-10); host slots live at 0x1000 + n, far above it.
/// **F2'S CONTROLS ARE A SECOND BLOCK** (2026-10-19), 0x10 per sound at 0x0500 + slot·0x10, in SpectralControl's order from
/// FilterGain, so no id of the first block moved.
inline constexpr uint32_t kSpectralBlockBase = 0x0400;
inline constexpr uint32_t kSpectralSlotStride = 0x20;
inline constexpr uint32_t kSpectralBlock2Base = 0x0500;
inline constexpr uint32_t kSpectralSlot2Stride = 0x10;
inline constexpr uint32_t kSpectralBlock3Base = 0x0580;
inline constexpr uint32_t kSpectralSlot3Stride = 0x10;
static_assert(kSpectralFirstBlockCount <= kSpectralSlotStride);
static_assert(kSpectralThirdBlockFirst - kSpectralFirstBlockCount <= kSpectralSlot2Stride);
static_assert(kSpectralControlCount - kSpectralThirdBlockFirst <= kSpectralSlot3Stride);
constexpr ParamId spectralParam(size_t slot, SpectralControl control) {
	const auto c = static_cast<uint32_t>(control);
	if (c >= kSpectralThirdBlockFirst) {
		return static_cast<ParamId>(kSpectralBlock3Base + static_cast<uint32_t>(slot) * kSpectralSlot3Stride
		                            + (c - static_cast<uint32_t>(kSpectralThirdBlockFirst)));
	}
	if (c >= kSpectralFirstBlockCount) {
		return static_cast<ParamId>(kSpectralBlock2Base + static_cast<uint32_t>(slot) * kSpectralSlot2Stride
		                            + (c - static_cast<uint32_t>(kSpectralFirstBlockCount)));
	}
	return static_cast<ParamId>(kSpectralBlockBase + static_cast<uint32_t>(slot) * kSpectralSlotStride + c);
}
/// The (slot, control) a spectral id addresses, or nullopt-ish (`slot` = kMaxSounds) for any other id.
struct SpectralAddress {
	size_t slot = kMaxSounds;
	SpectralControl control = SpectralControl::kCount;
};
constexpr SpectralAddress spectralAddressOf(ParamId id) {
	const auto raw = static_cast<uint32_t>(id);
	if (raw >= kSpectralBlock3Base && raw < kSpectralBlock3Base + kMaxSounds * kSpectralSlot3Stride) {
		const uint32_t within = (raw - kSpectralBlock3Base) % kSpectralSlot3Stride;
		if (within >= kSpectralControlCount - kSpectralThirdBlockFirst) {
			return {};
		}
		return { (raw - kSpectralBlock3Base) / kSpectralSlot3Stride,
			     static_cast<SpectralControl>(within + static_cast<uint32_t>(kSpectralThirdBlockFirst)) };
	}
	if (raw >= kSpectralBlock2Base && raw < kSpectralBlock2Base + kMaxSounds * kSpectralSlot2Stride) {
		const uint32_t within = (raw - kSpectralBlock2Base) % kSpectralSlot2Stride;
		if (within >= kSpectralThirdBlockFirst - kSpectralFirstBlockCount) {
			return {};
		}
		return { (raw - kSpectralBlock2Base) / kSpectralSlot2Stride,
			     static_cast<SpectralControl>(within + static_cast<uint32_t>(kSpectralFirstBlockCount)) };
	}
	if (raw < kSpectralBlockBase || raw >= kSpectralBlockBase + kMaxSounds * kSpectralSlotStride) {
		return {};
	}
	const uint32_t within = (raw - kSpectralBlockBase) % kSpectralSlotStride;
	if (within >= kSpectralFirstBlockCount) {
		return {};
	}
	return { (raw - kSpectralBlockBase) / kSpectralSlotStride, static_cast<SpectralControl>(within) };
}

/// **THE DRUM ENGINE'S PER-PAD CONTROLS** (2026-11-05): one block of 0x20
/// per sound at 0x0600 + slot·0x20, in this order. In Kit mode sound N is pad N; in Pitched mode the per-pad structure
/// (Kind, Note, Choke, Voices) is not read and the rest of pad A's block is the engine's. Each key names one meaning across
/// the six kinds (Decay is always the body's T60), so a kind that does not read a control hides it (drum::kindReads).
enum class DrumControl : uint8_t {
	Kind, Note, Choke, Voices, Trigger, Level, Pan, Tune, Decay, Release, SweepTime, Drive, Count, Spread,
	// THE VELOCITY ZONE (drum proposal §14, 2026-11-07): Level → Tone → Tension, absorbing Velocity and Vel Tension.
	VelocityZone,
	FieldPitch, FieldLength, FieldSweep, FieldTone, FieldNoise, FieldSnap, FieldMetal,
	// THE SNARE'S ENVELOPES (2026-11-06, drum proposal §12) and its two zone controls (§14): Wires (Tight → Loose → Buzzy →
	// Gated: the Shapes, the Contour, the Buzz and its Threshold) and Bells (Off → Body → Shell → Ring → Knock: the bells'
	// levels, their Q's scale and the Link). The bells' glide stays on handles.
	WireDecay, ClickDecay, Wires, Bells, BellStart, BellEnd, BellTime,
	// THE KICK'S SHAPE (2026-11-06, drum proposal §12 items 1-3), read only on a Kick.
	KickHold, KickDip, KickDipTime, ClickSweep, ClickSweepTime, StartPhase,
	// THE DRIVE CURVE AND ITS TONE (every kind) AND THE CLAP'S FILTER (2026-11-06).
	DriveCurve, DriveTone, ClapQ, ClapHighPass,
	// THE SECOND BELL'S FREE HZ AND ITS RING (2026-11-06, doc §13.2), on its handle.
	Bell2Hz, Bell2Time,
	// VELOCITY TO NOISE (2026-11-06, doc §13 item 4;).
	VelocityNoise,
	// WIDTH (a Snare's wires, a Clap's band; doc §13 item 7).
	Width,
	// MUTE: the pad's own, so a kit's pads all sound by default whatever their sounds' switches.
	// In Kit mode the sound card's switch is this.
	Mute,
	// BEND: how far whatever moves the voice past its key (the wheel,
	// an expression, a pitch route) moves this pad, held at the strike or moved while it rings. 0 ignores it.
	Bend,
	// THE LAYER SLOT (drum proposal §12, built 2026-11-07 stage (c)): a second hit of its own Kind struck with the pad, at its
	// own place (the pad's plus an offset vector), Level, Delay (ms) and High Pass; Lock keeps the offset when the pad is dragged.
	LayerKind, LayerLevel, LayerDelay, LayerHighPass, LayerLock, LayerOffsetX, LayerOffsetY,
	kCount
};
inline constexpr size_t kDrumControlCount = static_cast<size_t>(DrumControl::kCount);
inline constexpr uint32_t kDrumBlockBase = 0x0600;
inline constexpr uint32_t kDrumSlotStride = 0x20;
/// THE SECOND PAD BLOCK (2026-11-06): the first's 32 slots filled with the snare's controls, so DrumControl from index 32 is
/// 0x0700 + slot·0x20. One enum, two blocks, the split here alone.
inline constexpr uint32_t kDrumBlock2Base = 0x0700;
inline constexpr uint32_t kDrumSlot2Stride = 0x20; // 32 a slot (2026-11-06, S11): room for the second bell and the T stages
static_assert(kDrumControlCount <= kDrumSlotStride + kDrumSlot2Stride);
constexpr ParamId drumParam(size_t slot, DrumControl control) {
	const auto c = static_cast<uint32_t>(control);
	if (c >= kDrumSlotStride) {
		return static_cast<ParamId>(kDrumBlock2Base + static_cast<uint32_t>(slot) * kDrumSlot2Stride + (c - kDrumSlotStride));
	}
	return static_cast<ParamId>(kDrumBlockBase + static_cast<uint32_t>(slot) * kDrumSlotStride + c);
}
/// The drum controls that are amounts (each slot's destination run carries them, in this order) and the ones that are
/// switches or counts (not destinations: a stepped control under a modulator chatters rather than plays).
inline constexpr std::array<DrumControl, 42> kDrumContinuousControls {
	DrumControl::Level,      DrumControl::Pan,        DrumControl::Tune,        DrumControl::Decay,
	DrumControl::Release,    DrumControl::SweepTime,  DrumControl::Drive,       DrumControl::Spread,
	DrumControl::VelocityZone, DrumControl::FieldPitch, DrumControl::FieldLength, DrumControl::FieldSweep,
	DrumControl::FieldTone,  DrumControl::FieldNoise, DrumControl::FieldSnap,   DrumControl::FieldMetal,
	// THE SNARE'S: its envelopes, its two zones and the first bell's glide.
	DrumControl::WireDecay, DrumControl::ClickDecay, DrumControl::Wires, DrumControl::Bells,
	DrumControl::BellStart, DrumControl::BellEnd, DrumControl::BellTime,
	// THE KICK'S.
	DrumControl::KickHold, DrumControl::KickDip, DrumControl::KickDipTime, DrumControl::ClickSweep,
	DrumControl::ClickSweepTime, DrumControl::StartPhase,
	// THE DRIVE'S TONE AND THE CLAP'S FILTER.
	DrumControl::DriveTone, DrumControl::ClapQ, DrumControl::ClapHighPass,
	// THE SECOND BELL'S FREE HZ AND RING.
	DrumControl::Bell2Hz, DrumControl::Bell2Time,
	// VELOCITY TO NOISE, AND WIDTH.
	DrumControl::VelocityNoise, DrumControl::Width,
	// THE BEND.
	DrumControl::Bend,
	// THE LAYER SLOT.
	DrumControl::LayerLevel, DrumControl::LayerDelay, DrumControl::LayerHighPass, DrumControl::LayerOffsetX,
	DrumControl::LayerOffsetY
};
inline constexpr std::array<DrumControl, 10> kDrumSteppedControls { DrumControl::Kind,    DrumControl::Note,
	                                                              DrumControl::Choke,   DrumControl::Voices,
	                                                              DrumControl::Trigger, DrumControl::Count,
	                                                              DrumControl::DriveCurve, DrumControl::Mute,
	                                                              DrumControl::LayerKind, DrumControl::LayerLock };
static_assert(kDrumContinuousControls.size() + kDrumSteppedControls.size() == kDrumControlCount);
/// The choices. Voices' labels are the counts they stand for (kDrumVoicesCounts).
inline constexpr std::array<const char* const, 2> kDrumModeNames { "Pitched", "Kit" };
inline constexpr std::array<const char* const, 6> kDrumKindNames { "Kick", "Membrane", "Snare", "Metal", "Clap", "String" };
inline constexpr std::array<const char* const, 5> kDrumChokeNames { "Off", "1", "2", "3", "4" };
inline constexpr std::array<const char* const, 3> kDrumVoicesNames { "1", "2", "4" };
inline constexpr std::array<int, 3> kDrumVoicesCounts { 1, 2, 4 };
inline constexpr std::array<const char* const, 2> kDrumTriggerNames { "One-shot", "Gate" };
/// THE LAYER'S KINDS: Off, then every kind but String (a String plucks the voice's one string, which its pad may be using).
inline constexpr std::array<const char* const, 6> kDrumLayerKindNames { "Off", "Kick", "Membrane", "Snare", "Metal", "Clap" };
/// The drive's curves (drum proposal §12 item 4): Tanh is the round one; Tube leans, so it adds even harmonics; Soft Clip is
/// a cubic, flat-topped past its knee.
inline constexpr std::array<const char* const, 3> kDrumDriveCurveNames { "Tanh", "Tube", "Soft Clip" };
/// The second bell: Free at its own Hz, or Linked at Ratio × the first bell, riding its glide (a SwitchChip: two states).
inline constexpr std::array<const char* const, 2> kDrumBell2LinkNames { "Free", "Linked" };
/// The wires' contour (all three on the face): an exponential fall, a straight one, or chopped by a falling sawtooth.
inline constexpr std::array<const char* const, 3> kDrumWireContourNames { "Exp", "Linear", "Mod" };
enum class DrumMode : uint8_t { Pitched, Kit };

/// **THE DEFAULT KIT** (proposal §5): Pattern Drum's eight lanes, note for note and in its order (test_drum_engine pins the
/// notes against terrain-arp's lanes), each pad a kind and a voicing that sounds like its name at the terrain's middle.
struct DrumPadDefaults {
	uint8_t kind;  ///< index into kDrumKindNames
	uint8_t note;
	uint8_t choke; ///< index into kDrumChokeNames
	uint8_t voices;///< index into kDrumVoicesNames
	double tune;   ///< semitones from the kind's resting pitch
	double logDecay;     ///< log2 seconds (the param's own unit: a constant table cannot call log2 on every compiler)
	double logSweepTime; ///< log2 seconds
	double drive;
	uint8_t count;
	double fieldSweep, fieldNoise;
};
inline constexpr std::array<DrumPadDefaults, 8> kDrumPadDefaults { {
		// kind note choke voices tune  log2(decay s)        log2(sweep s)    drive count sweep noise
	{ 0, 36, 0, 0, 0.0,  -1.1520030934450500, -4.6438561897747247, 0.2, 3, 0.8, 0.3 },  // A Kick: 0.45 s, 40 ms
	{ 2, 38, 0, 1, 0.0,  -2.1844245711374275, -5.0588936890535687, 0.0, 3, 0.5, 0.75 }, // B Snare: 0.22 s, 30 ms
	{ 3, 42, 1, 0, 0.0,  -3.8365012677171206, -4.0588936890535687, 0.0, 3, 0.5, 0.5 },  // C Closed hat: 0.07 s
	{ 3, 46, 1, 1, 0.0,  -0.7369655941662062, -4.0588936890535687, 0.0, 3, 0.5, 0.5 },  // D Open hat: 0.6 s
	{ 4, 39, 0, 1, 0.0,  -1.5145731728297582, -4.0588936890535687, 0.0, 4, 0.5, 0.5 },  // E Clap: 0.35 s
	{ 1, 45, 0, 1, -5.0, -0.7369655941662062, -4.0588936890535687, 0.0, 3, 0.6, 0.5 },  // F Low tom: 0.6 s
	{ 1, 50, 0, 1, 3.0,  -1.0,                -4.0588936890535687, 0.0, 3, 0.6, 0.5 },  // G High tom: 0.5 s
	{ 3, 51, 0, 2, -3.0, 1.0,                 -4.0588936890535687, 0.0, 3, 0.5, 0.3 },  // H Ride: 2 s
} };
/// The switch of the operator a control belongs to (itself for a switch).
constexpr SpectralControl spectralOperatorOf(SpectralControl c) {
	switch (c) {
	case SpectralControl::FilterOn: case SpectralControl::FilterMode: case SpectralControl::FilterCutoff:
	case SpectralControl::FilterResonance: case SpectralControl::FilterSlope: case SpectralControl::FilterKeyTrack:
	case SpectralControl::FilterGain:
		return SpectralControl::FilterOn;
	case SpectralControl::Filter2On: case SpectralControl::Filter2Mode: case SpectralControl::Filter2Cutoff:
	case SpectralControl::Filter2Resonance: case SpectralControl::Filter2Slope: case SpectralControl::Filter2KeyTrack:
	case SpectralControl::Filter2Gain:
		return SpectralControl::Filter2On;
	// THE ROUTING IS BOTH FILTERS'; filed under filter 1's switch, and live only while both act (spectralRoutingInert).
	case SpectralControl::FilterRouting: return SpectralControl::FilterOn;
	case SpectralControl::CombOn: case SpectralControl::CombSpacing: case SpectralControl::CombDepth:
	case SpectralControl::CombPhase:
		return SpectralControl::CombOn;
	case SpectralControl::FormantShiftOn: case SpectralControl::FormantShiftLow: case SpectralControl::FormantShiftHigh:
	case SpectralControl::FormantShiftCrossover: case SpectralControl::FormantShiftLink:
		return SpectralControl::FormantShiftOn;
	case SpectralControl::TiltOn: case SpectralControl::TiltAmount: return SpectralControl::TiltOn;
	case SpectralControl::FormantOn: case SpectralControl::FormantVowel: case SpectralControl::FormantDepth:
		return SpectralControl::FormantOn;
	case SpectralControl::PartialsOn: case SpectralControl::PartialsCount: case SpectralControl::PartialsMode: case SpectralControl::PartialsDepth:
	case SpectralControl::PartialsSet:
		return SpectralControl::PartialsOn;
	case SpectralControl::DampOn: case SpectralControl::DampTime: case SpectralControl::DampTilt:
		return SpectralControl::DampOn;
	case SpectralControl::StretchOn: case SpectralControl::StretchAmount: return SpectralControl::StretchOn;
	case SpectralControl::ShiftOn: case SpectralControl::ShiftHz: return SpectralControl::ShiftOn;
	case SpectralControl::VocodeOn: case SpectralControl::VocodeDepth: case SpectralControl::VocodeFlatten:
	case SpectralControl::VocodeGate: case SpectralControl::VocodeLow: case SpectralControl::VocodeHigh:
	case SpectralControl::VocodeBandwidth: case SpectralControl::VocodeSpacingAmount: case SpectralControl::VocodeSpacingFree:
	case SpectralControl::VocodeWidthsAmount: case SpectralControl::VocodeWidthsFree:
		return SpectralControl::VocodeOn;
	default: return SpectralControl::OddEvenOn;
	}
}
/// The operator switches.
inline constexpr std::array<SpectralControl, 12> kSpectralSwitches{
    SpectralControl::FilterOn, SpectralControl::Filter2On, SpectralControl::CombOn, SpectralControl::FormantShiftOn, SpectralControl::TiltOn,
    SpectralControl::OddEvenOn, SpectralControl::FormantOn,
    SpectralControl::PartialsOn, SpectralControl::DampOn, SpectralControl::StretchOn,
    SpectralControl::ShiftOn, SpectralControl::VocodeOn};
/// The stepped controls (the switches, Mode, Link, the routing): host params, never destinations.
inline constexpr std::array<SpectralControl, 17> kSpectralSteppedControls{
    SpectralControl::FilterOn, SpectralControl::FilterMode, SpectralControl::CombOn, SpectralControl::FormantShiftOn,
    SpectralControl::FormantShiftLink, SpectralControl::TiltOn, SpectralControl::OddEvenOn,
    SpectralControl::FormantOn, SpectralControl::PartialsOn, SpectralControl::DampOn, SpectralControl::StretchOn,
    SpectralControl::ShiftOn,
    // F2, APPENDED (the knob list is a stance's wire order: an append moves no earlier knob).
    SpectralControl::Filter2On, SpectralControl::Filter2Mode, SpectralControl::FilterRouting,
    // OH, APPENDED likewise.
    SpectralControl::PartialsMode,
    // OE, APPENDED likewise.
    SpectralControl::VocodeOn};
/// The continuous controls, in row order: each slot's destination run carries these (buildModDestinations).
inline constexpr std::array<SpectralControl, 37> kSpectralContinuousControls{
    SpectralControl::FilterCutoff, SpectralControl::FilterResonance, SpectralControl::FilterSlope,
    SpectralControl::FilterKeyTrack, SpectralControl::CombSpacing, SpectralControl::CombDepth,
    SpectralControl::CombPhase, SpectralControl::FormantShiftLow, SpectralControl::FormantShiftHigh,
    SpectralControl::FormantShiftCrossover, SpectralControl::TiltAmount, SpectralControl::OddEvenBalance,
    SpectralControl::FormantVowel, SpectralControl::FormantDepth, SpectralControl::PartialsCount,
    SpectralControl::DampTime, SpectralControl::DampTilt, SpectralControl::StretchAmount,
    SpectralControl::ShiftHz,
    // F2, APPENDED (each slot's destination run and the knob list keep every earlier entry where it was within the slot).
    SpectralControl::FilterGain, SpectralControl::Filter2Cutoff, SpectralControl::Filter2Resonance,
    SpectralControl::Filter2Slope, SpectralControl::Filter2KeyTrack, SpectralControl::Filter2Gain,
    // OI, APPENDED likewise.
    SpectralControl::PartialsSet,
    // OF, APPENDED likewise.
    SpectralControl::PartialsDepth,
    // OE, APPENDED likewise.
    SpectralControl::VocodeDepth, SpectralControl::VocodeFlatten, SpectralControl::VocodeGate, SpectralControl::VocodeLow,
    SpectralControl::VocodeHigh,
    // OE2, APPENDED likewise.
    SpectralControl::VocodeBandwidth, SpectralControl::VocodeSpacingAmount, SpectralControl::VocodeSpacingFree,
    SpectralControl::VocodeWidthsAmount, SpectralControl::VocodeWidthsFree};
static_assert(kSpectralSteppedControls.size() + kSpectralContinuousControls.size() == kSpectralControlCount);
/// **THE STEREO DESTINATIONS** — the controls marked S in design §3.2, each row `ParamDesc::stereo`: a pair routed here
/// reaches the two ears apart (the voice builds a drawer per ear: Synth::drawerFor, OscVoice::setDrawer). Key track
/// and the crossover are mono, as §3.2 marks them.
inline constexpr std::array<SpectralControl, 16> kSpectralStereoControls{
    SpectralControl::FilterCutoff, SpectralControl::FilterResonance, SpectralControl::FilterSlope,
    SpectralControl::FilterGain, SpectralControl::Filter2Cutoff, SpectralControl::Filter2Resonance,
    SpectralControl::Filter2Slope, SpectralControl::Filter2Gain, SpectralControl::CombSpacing, SpectralControl::CombDepth, SpectralControl::CombPhase,
    SpectralControl::FormantShiftLow, SpectralControl::FormantShiftHigh, SpectralControl::TiltAmount,
    SpectralControl::OddEvenBalance, SpectralControl::FormantVowel};
/// **A CONTROL ITS OWN SOUND'S SETTINGS MAKE INERT**: Resonance under Notch (spectral_shaper.h: Notch has no peak), the high
/// shift and the crossover while Linked (the low shift then moves the whole envelope). ONE PREDICATE for the panel's
/// greying (SpectrumPanel::sync) and the engine's blend (Synth::drawerFor), so what the UI greys and what the morph
/// ignores cannot drift apart: a greyed knob may never move the sound (the honesty rule).
/// AND THE VOWEL AT DEPTH 0 (spectral-ops, 2026-10-12): `vowelSilent` is the sound's Vowel Depth at 0, where its vowel is not
/// heard, so the morph leaves its position out of the formants' blend (Synth::drawerFor) and the row greys the Vowel knob.
/// AND THE FILTER AT ITS OPEN END (Z7d): `filterOpen` is the sound's filter sitting where `spectral::filterOpenness` is 1 (Low Pass
/// at its top, High Pass at its bottom), where the filter is off — so its Resonance, Slope and Key Track change nothing.
/// **F2: EACH FILTER'S CONTROLS BY ITS OWN SETTINGS** — `filterMode` and `filterOpen` are those of the filter `c` belongs to
/// (`spectralFilterOf`); Gain acts only in Bell (index 4), and a Bell at 0 dB is open. The routing is not judged here: it is
/// inert unless both filters act (`spectralRoutingInert`).
constexpr int spectralFilterOf(SpectralControl c) {
	switch (c) {
	case SpectralControl::FilterOn: case SpectralControl::FilterMode: case SpectralControl::FilterCutoff:
	case SpectralControl::FilterResonance: case SpectralControl::FilterSlope: case SpectralControl::FilterKeyTrack:
	case SpectralControl::FilterGain:
		return 0;
	case SpectralControl::Filter2On: case SpectralControl::Filter2Mode: case SpectralControl::Filter2Cutoff:
	case SpectralControl::Filter2Resonance: case SpectralControl::Filter2Slope: case SpectralControl::Filter2KeyTrack:
	case SpectralControl::Filter2Gain:
		return 1;
	default: return -1;
	}
}
/// Filter `filter`'s control `c` (0 for filter 1's, 1 for filter 2's): the vocabulary is filter 1's, so code over "a filter"
/// is written once.
constexpr SpectralControl spectralFilterControl(int filter, SpectralControl c) {
	if (filter == 0) {
		return c;
	}
	switch (c) {
	case SpectralControl::FilterOn: return SpectralControl::Filter2On;
	case SpectralControl::FilterMode: return SpectralControl::Filter2Mode;
	case SpectralControl::FilterCutoff: return SpectralControl::Filter2Cutoff;
	case SpectralControl::FilterResonance: return SpectralControl::Filter2Resonance;
	case SpectralControl::FilterSlope: return SpectralControl::Filter2Slope;
	case SpectralControl::FilterKeyTrack: return SpectralControl::Filter2KeyTrack;
	case SpectralControl::FilterGain: return SpectralControl::Filter2Gain;
	default: return c;
	}
}
/// THE ROUTING CHANGES NOTHING UNLESS BOTH FILTERS ACT (each on and not at its open end): one filter is the same in Series
/// and in Parallel.
constexpr bool spectralRoutingInert(bool filter1Acts, bool filter2Acts) {
	return !(filter1Acts && filter2Acts);
}
constexpr bool spectralControlInert(SpectralControl c, size_t filterMode, bool linked, bool vowelSilent = false,
                                    bool filterOpen = false) {
	const bool res = c == SpectralControl::FilterResonance || c == SpectralControl::Filter2Resonance;
	const bool slopeOrKey = c == SpectralControl::FilterSlope || c == SpectralControl::Filter2Slope
	                        || c == SpectralControl::FilterKeyTrack || c == SpectralControl::Filter2KeyTrack;
	const bool gain = c == SpectralControl::FilterGain || c == SpectralControl::Filter2Gain;
	return (res && filterMode == 3) // FilterMode::Notch (index 3, test_params pins it)
	       || (gain && filterMode != 4) // FilterMode::Bell (index 4): no other mode reads Gain
	       || (filterOpen && (res || slopeOrKey))
	       || ((c == SpectralControl::FormantShiftHigh || c == SpectralControl::FormantShiftCrossover) && linked)
	       || (c == SpectralControl::FormantVowel && vowelSilent);
}

constexpr bool spectralStereo(SpectralControl c) {
	for (const SpectralControl s : kSpectralStereoControls) {
		if (s == c) {
			return true;
		}
	}
	return false;
}

/// The Resonate engine's physics models, in WeaveModel's order (dsp/weave_models.h). Spelled here because
/// a Choice's names must be a constant table and weave::modelName is a function; test_params pins every
/// entry to weave::modelName, so the two cannot drift — rename the model there and this goes red.
inline constexpr std::array<const char* const, 5> kResonateModelNames{"Ring", "Bar", "Tube", "Membrane", "Stiffening"};

/// The Resonate bow-mode choice: the laws' own (the zone's), or one of WeaveBow's five forced (index - 1).
inline constexpr std::array<const char* const, 6> kResonateBowModeNames{"Laws", "Triangle", "StickSlip", "Noise",
                                                                        "Pulses", "Walk"};

/// The engine a raw OscType VALUE selects, by the table's own stepped rule. ONE derivation for
/// every consumer — the synth and the editor share it through characterFromParams' wrapper, and
/// the two width controls' V5-only enablement (V12c honesty) reads it here directly — because a
/// consumer that rounded its own way would quietly disagree about which patches are flagship the
/// day a fourth engine lands.
inline OscType oscTypeFromValue(double value) {
	return static_cast<OscType>(steppedIndex(value, kOscTypeNames.size()));
}

/// A sound is either in the constellation or it is not. Stepped rather than continuous, because half
/// a sound is not a thing — how much of it you hear is decided by where you stand, not by a switch.
inline constexpr std::array<const char* const, 2> kEnableNames{"Off", "On"};

/// WHAT A NOTE-ON DOES TO THE RUNNING PHASE.
///
/// This id was a two-state catchup toggle for exactly as long as it took to say that. The widening
/// was free and only briefly so: 0x0064 has never been in an installed build, so no patch anywhere
/// carries a value under the old reading — after the first install that stops being true forever,
/// which is why it went in before the vox arc could ship one.
///
/// THE THREE ARE ONE QUESTION ASKED OF EVERY RETRIGGER ROAD, not three unrelated switches:
///  - Digi restarts the oscillator on every note, phase snapped to 0. The hard edge is the point.
///  - Analog is phase-absolute: a note-on never touches the phase. Each voice's oscillator carries
///    its own phase forward and a new note simply begins wherever that voice stands, which is what a
///    pool of free-running VCOs does. It is the DYING voice's phase that stays out of it — that
///    belongs to another oscillator, and adopting it would be continuity, which is Catchup's job.
///  - Phase Catchup is ours: the firmware's HARM mechanism, bending home over ~24 cycles instead of
///    jumping. DEFAULT, because de-clicked retriggers are the point (V21d).
///
/// NONE OF THE THREE IS THE OLD OFF POSITION, and that is worth saying rather than discovering: the
/// pre-mode behaviour was a MIXTURE — a fresh voice snapped to 0 (Digi) while a same-voice retrigger
/// left the phase running (Analog) — so no single mode reproduces it. Nothing is owed bit-identity
/// here, because the toggle never shipped; what the modes do is make that accidental mixture into a
/// coherent choice a player makes.
///
/// Names carry their own sense, as the two-state pair did and for the same reason: the Engine rail row
/// and a host's flat dropdown both show these bare. SHORT, because three of them share one
/// full-width row on a narrow rail — "Phase Catchup" does not fit beside two others, and the param's
/// own name says which question "Catchup" is answering.
inline constexpr std::array<const char* const, 3> kOscModeNames{"Digi", "Analog", "Catchup"};

/// The three, named — so the dispatch reads as the question it asks rather than as an index
/// comparison, and so a fourth mode cannot land without the compiler listing every road that decides
/// on one.
enum class OscMode : uint8_t { Digi, Analog, Catchup, kCount };
static_assert(kOscModeNames.size() == static_cast<size_t>(OscMode::kCount));
static_assert(kOscTypeNames.size() == static_cast<size_t>(OscType::kCount));

/// The mode a raw value selects, by the table's own stepped rule — ONE derivation, for the reason
/// oscTypeFromValue is one: a consumer that rounded its own way would disagree about which patches
/// are analog the day a fourth mode lands.
inline OscMode oscModeFromValue(double value) {
	return static_cast<OscMode>(steppedIndex(value, kOscModeNames.size()));
}

/// Poly is unrestricted polyphony to the voice-count limit. Mono retriggers the amp edges on every
/// new note; Legato moves the playing voice without retriggering them.
enum class VoiceMode : uint8_t { Poly, Mono, Legato, kCount };
inline constexpr std::array<const char* const, 3> kVoiceModeNames{"Poly", "Mono", "Legato"};
static_assert(kVoiceModeNames.size() == static_cast<size_t>(VoiceMode::kCount));

/// Which note wins when there are more notes than the synth can play. Named for the note that
/// SURVIVES, not the one that is dropped: "Low" keeps the low notes and sheds the high ones.
enum class NotePriority : uint8_t { Last, Low, High, kCount };
inline constexpr std::array<const char* const, 3> kNotePriorityNames{"Last", "Low", "High"};
static_assert(kNotePriorityNames.size() == static_cast<size_t>(NotePriority::kCount));

/// How a raw param value becomes text, and how text becomes a raw value again.
///
/// The unit is the whole of a param's taper: `Attack` is not "seconds with a log slider", it *is*
/// log2 seconds, because a CLAP param has no separate display curve — the host's slider is linear in
/// the value the plugin declares. So the curve has to be the value.
enum class ParamUnit : uint8_t {
	Number,      ///< bare number, 2 decimals
	Integer,     ///< bare number, no decimals (stepped counts)
	Zone,        ///< bare number, 1 decimal (quantised to uint16 only when a table is built)
	Percent,     ///< value ×100, "%"
	Cents,       ///< "cents"
	Semitones,   ///< signed, "st"
	Octaves,     ///< signed, "oct"
	Decibels,    ///< "dB"; at the declared minimum, "-inf dB"
	Gain,        ///< linear 0..1 amplitude, printed as dB; at 0, "-inf dB"
	LogHertz,    ///< value is log2(hertz); printed Hz or kHz
	LogSeconds,  ///< value is log2(seconds); printed ms or s
	Factor,      ///< a multiplier, printed "x1.00"
	LogFactor,   ///< value is log2(multiplier); printed as the multiplier, "x1.00"
	GlideSeconds,///< 0..1 cubic taper onto kGlideMaxSeconds; at 0, "Off"
	BendChoice,  ///< index into kBendRangeSemitones
	Choice,      ///< index into `names`
	Hertz,       ///< linear Hz, signed (the spectral Shift): "+50.0 Hz"
	Vowel,       ///< a position along PhiVox's eight vowel anchors (0..7), printed with the nearest one's name: "Open 3.00"
};

struct ParamDesc {
	ParamId id;
	const char* name;
	const char* module; ///< "" for top level
	double min;
	double max;
	double def;
	uint32_t flags; ///< clap_param_info_flags
	ParamUnit unit;
	std::span<const char* const> names; ///< Choice only
	/// Is [min, max] a knob's travel rather than a limit on the value the engine may see? Defaulted
	/// to the safe answer, so a row that says nothing gets today's rule: resolved() clamps the sum of
	/// base and every offset to the declared range. The filter's cutoff is the only row that says
	/// otherwise, and it says it in `fx::ParamDesc` — see there for why this is the device's call.
	bool offsetsMayLeaveRange = false;

	/// TOOLTIP TEXT — what this control does, in a sentence a user who does not know the device can
	/// understand. Lives beside `name` in the ParamDesc, so a tooltip can never describe the wrong
	/// control. A device that declares no tooltip shows its name instead — the fallback is the field
	/// above, not a separate default.
	const char* tooltip = nullptr;

	/// **THE NAME A PATCH WILL STORE THIS VALUE UNDER** (backlog JF §3.3) — `device::ParamDesc::key`'s rules:
	/// `[a-z0-9_.]`, unique, and it names a MEANING. Core parameters are still stored by CLAP id until JF
	/// step 2 keys them all; a row added since then declares its literal key now, so step 2 finds it written
	/// rather than has to invent it, and `tests/state_schema.ledger` carries it from the day it lands. LAST
	/// and defaulted, unlike the device struct's, because the rows before it are positional and predate it.
	const char* key = nullptr;

	/// **A STEREO DESTINATION** (backlog KU, `device::ParamDesc::stereo`'s meaning for a core parameter, 2026-10-05): a route
	/// from a source that is a PAIR reaches the two ears apart. Set on Phase only, the one core parameter with a per-ear
	/// reader (`Synth::seatReadPhase`). Every other row is mono: both ears hear the left.
	bool stereo = false;
};

/// Pitch-bend ranges the user can pick, in semitones. `ParamId::BendRange` is an INDEX into this,
/// not a semitone count: the choices are not contiguous, so a stepped param over the values
/// themselves would offer 3..6 and 13..23 as well.
inline constexpr std::array<int, 6> kBendRangeSemitones{1, 2, 7, 12, 24, 48};

/// The largest value `ParamId::VoiceCount` offers, and so the number of voices the synth must be
/// able to sound at once. The voice pool is sized from this at construction — a count param that
/// could exceed the pool would be a promise the audio thread has to allocate to keep.
inline constexpr int kMaxVoices = 16;

/// **THE MOST OSCILLATORS ONE NOTE CAN STACK** (backlog KE: 8, as in the firmware, whose
/// `kMaxNumVoicesUnison` is 8). `ParamId::StackCount`'s maximum, and so the size of every voice's part array,
/// built at construction for `kMaxVoices`'s reason.
inline constexpr int kMaxStack = 8;

/// Stack Detune's top, in CENTS OF TOTAL FAN — lowest part to highest (50 cents, ±25, as the firmware's
/// `kMaxUnisonDetune`, whose unit the voice-stacking doc §1.2 derives from its lookup table).
inline constexpr double kStackDetuneMaxCents = 50.0;

/// Fine Tune's reach either side of the note, in cents — its row's range, named because a Stack Index route into
/// it computes each part's own Fine Tune against the same range (`Synth::partTargetFor`).
inline constexpr double kFineTuneMaxCents = 100.0;

/// Coarse's reach either side of the note, in semitones: four octaves, Transpose Oct's own span.
inline constexpr double kCoarseMaxSemitones = 48.0;

/// **THE STACK INDEX'S SHAPES** (backlog KE step 3; voice-stacking doc §3.2), IW's modes in the firmware's order with
/// its quirks fixed: Random split in two so the choice is visible before it is made (Random is one fixed draw,
/// Random Fresh draws anew at each note), and Free last, the unbounded phi field. Each mode reads Stack Shape
/// Amount as its own 0..1 meta, except Free, which reads Stack Free Phase.
enum class StackShape : uint8_t { Linear, Power, SCurve, Step, Triangle, Sine, Random, RandomFresh, Drift, Free, kCount };
inline constexpr std::array<const char* const, 10> kStackShapeNames{
    "Linear", "Power", "S-Curve", "Step", "Triangle", "Sine", "Random", "Random Fresh", "Drift", "Free"};
static_assert(kStackShapeNames.size() == static_cast<size_t>(StackShape::kCount));

/// **AND ITS MAPPINGS** — which part gets which value. "As shaped" is new and first: the firmware sorted the shape's
/// values before permuting them, which threw away Triangle's peak and Sine's cycles, and with the sort gone the
/// identity is the mapping that keeps them (doc §1.4, the first quirk). The rest are the firmware's eight.
enum class StackMap : uint8_t { AsShaped, Symmetric, AntiSym, PitchUp, PitchDown, CenterOut, Rotate, Pairs, Shuffle, kCount };
inline constexpr std::array<const char* const, 9> kStackMapNames{
    "As shaped", "Symmetric", "Anti-sym", "Pitch+", "Pitch-", "Center-out", "Rotate", "Pairs", "Shuffle"};
static_assert(kStackMapNames.size() == static_cast<size_t>(StackMap::kCount));

/// **THE CPU GUARD'S OSCILLATOR BUDGET, B** (backlog KE; by design, voices steal sooner). A stack of N
/// may sound at most ⌊B / N⌋ notes, however high Voice Count is set; a stack of 1 is never limited by it,
/// because B ≥ kMaxVoices.
///
/// MEASURED, NOT CHOSEN: `tests/bench_stack` prints B for a target share of one core from the unit costs of an
/// oscillator whose table is moving (the dearer case), a table build and a vowel bake, under the per-block
/// build and bake budgets. On 2026-09-25, five runs of 9 alternated reps printed, for 70% of a core with the
/// tables moving: 37 and 42 at load average ~40, then 106, 48 and 69 at load ~9 — the unit costs themselves
/// moved by 2× between neighbouring runs on the quieter host (an oscillator 21 µs in one, 47 in the next), which
/// reads as the CPU's clock rather than as the code. 48 is the MEDIAN of the five, and gives stacks of 2, 3, 4, 6
/// and 8 whole-note limits of 16, 16, 12, 8 and 6. The 70% target is a judgement, and so is taking the median of
/// a spread this wide; both are product decisions to move (voice-stacking doc §5, step 0).
inline constexpr int kOscillatorBudget = 48;
static_assert(kOscillatorBudget >= kMaxVoices, "a stack of one must never be limited by the guard");

/// **HOW MANY NOTES A STACK OF `stack` MAY SOUND** under Voice Count `voiceCount` — the guard, and the one
/// function both the engine (which hands it to the voice manager) and the Engine rail (which shows it on the
/// Voice Count stepper) ask, so the number drawn and the number enforced cannot disagree.
constexpr int stackVoiceLimit(int voiceCount, int stack) {
	const int perStack = kOscillatorBudget / (stack < 1 ? 1 : stack);
	const int cap = perStack < 1 ? 1 : perStack;
	return voiceCount < cap ? voiceCount : cap;
}

/// Glide param 0..1 maps to seconds as `kGlideMaxSeconds * x³`.
///
/// Cubic rather than the log2-seconds the amp edges use, because glide has to reach exactly zero —
/// "no portamento" is a real setting and no logarithm gets there. The amp edges have the opposite
/// requirement (a 0 ms edge is a click), so they can afford a logarithm.
inline constexpr double kGlideMaxSeconds = 2.0;

/// Number of declared params. Fixed rather than derived so ParamSet's storage is a plain array;
/// paramDescs() is checked against it.
///
/// `kMaxSounds` comes from dsp/constellation.h, where the geometry that has to agree with it lives.
///
/// What each sound carries. The first three are its IDENTITY — what it sounds like. PlaceX/PlaceY
/// and Enable are its PLACEMENT — where it sits and whether it is there at all. Width is its first
/// RIDE-ALONG (V13 for the id, V18 for the category): a setting of the sound's character that
/// follows the sound through clone and stance where placement stays with the slot. It is still not
/// identity — preview presents "the sound at its own identity" with width suspended (V12a) — the
/// three-way split the ride-along rule drew.
enum class SoundField : uint32_t {
	Zone = 0,
	Offset = 1,
	Gamma = 2,
	PlaceX = 3,
	PlaceY = 4,
	Enable = 5,
	Width = 6
};

/// The id of one field of one sound.
///
/// ONE RULE WITH ONE EXCEPTION, rather than fifty-six literals that a typo could quietly reroute:
/// identity for A and B lives where v1 put it, everything else follows a stride. A wrong id here is
/// not a compile error, it is somebody's patch loading onto the wrong knob, so tests/test_params.cpp
/// pins every one of them.
constexpr ParamId soundParam(size_t slot, SoundField field) {
	const auto index = static_cast<uint32_t>(field);
	if (index <= static_cast<uint32_t>(SoundField::Gamma)) {
		if (slot == 0) {
			return static_cast<ParamId>(0x0010u + index);
		}
		if (slot == 1) {
			return static_cast<ParamId>(0x0020u + index);
		}
		return static_cast<ParamId>(0x0100u + (static_cast<uint32_t>(slot) - 2u) * 0x10u + index);
	}
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u
	                            + (index - static_cast<uint32_t>(SoundField::PlaceX)));
}

/// THE PER-SOUND SMOOTHNESS OVERRIDE KNOBS (params.h ASmoothness..HSmoothness, +5 in each sound's
/// 0x0200 placement block). Out of the SoundField stride, so addressed directly rather than through
/// soundParam(). See the doc §11 and SoundCoord::smoothnessScale.
inline constexpr ParamId smoothnessParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 5u);
}
/// THE PER-SOUND FM DEPTH KNOBS (AFmDepth..HFmDepth, +4). Direct address for the same reason.
inline constexpr ParamId fmDepthParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 4u);
}

/// THE FIVE FIELD MODIFIER KNOBS (+6..+A), one accessor apiece and all four lines of each identical
/// but for the offset. Spelled out rather than folded into one `fieldScaleParam(slot, which)`
/// because the offsets are the ONE thing a caller must not get to choose wrongly: a single accessor
/// taking an index is an accessor whose two readers can disagree about what index 2 means, which is
/// this repo's most-repeated bug shape. See SoundCoord's matching members.
inline constexpr ParamId quantizeParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 6u);
}
inline constexpr ParamId syncParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 7u);
}
inline constexpr ParamId feedbackParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 8u);
}
inline constexpr ParamId pointsParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 9u);
}
inline constexpr ParamId gritParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 0xAu);
}
/// THE WARP DEPTH KNOB (+B). Spelled out like its five neighbours and for their stated reason.
inline constexpr ParamId warpParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 0xBu);
}
/// THE ADDITIVE SHAPE AMOUNT KNOB (+C). Spelled out like its neighbours and for their stated reason.
inline constexpr ParamId shapeAmountParam(size_t slot) {
	return static_cast<ParamId>(0x0200u + static_cast<uint32_t>(slot) * 0x10u + 0xCu);
}
/// THE SIX ADDITIVE BIAS KNOBS, in the second per-sound block (0x0280 + slot·0x10, +0..+5). Spelled out like their
/// neighbours and for their stated reason: the offset is the one thing a caller must not get to choose.
inline constexpr ParamId additiveRolloffParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 0u);
}
inline constexpr ParamId additiveFormantParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 1u);
}
inline constexpr ParamId additiveOddEvenParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 2u);
}
inline constexpr ParamId additiveCombParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 3u);
}
inline constexpr ParamId additiveDetailParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 4u);
}
inline constexpr ParamId additiveOffGridParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 5u);
}
/// The string's five field scalers for sound `slot` (+6..+A of the second per-sound block).
inline constexpr ParamId ksBrightnessParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 6u);
}
inline constexpr ParamId ksSustainParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 7u);
}
inline constexpr ParamId ksPickParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 8u);
}
inline constexpr ParamId ksNoiseParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 9u);
}
inline constexpr ParamId ksStretchParam(size_t slot) {
	return static_cast<ParamId>(0x0280u + static_cast<uint32_t>(slot) * 0x10u + 10u);
}

// THE MODULATION BUS.
//
// A source is a parameter like any other — the four macros are knobs a host can automate, and in a
// host with no modulation lane of its own (every VST3 one) they are the ONLY modulation this plugin
// can be given. What the bus adds is the fan-out: one source reaching several destinations at an
// amount per destination, which is the reference's mod matrix and Bitwig's modulator in the same shape.

/// How many macro cards the strip carries. Four, which is the reference's own number for the same object.
inline constexpr size_t kMacroCount = 4;

/// The one field of macro card `index`: 0x0080, 0x0090, 0x00A0, 0x00B0 — a block apiece, on the stride
/// the sound slots established. Pinned literally in tests/test_params.cpp, because an id that moved
/// would land somebody's routing on a different knob.
constexpr ParamId macroParam(size_t index) {
	return static_cast<ParamId>(0x0080u + static_cast<uint32_t>(index) * 0x10u);
}

// THE SOURCES THAT ARE NOT KNOBS — the per-sound activation weights, one per slot.
//
// A macro is a hand-set position, so it is a parameter and a host may automate it. A weight is a
// READING: how much of one sound the blend is currently letting through, computed by the engine from
// where the puck stands and set by nobody. The standing rule keeps it that way — these are
// INTERNAL sources, they spend no ids, they are not enumerated to the host and they are not routable
// from outside. What they share with a macro is only the last step, a value in 0..1 that an amount
// scales into a destination, which is why they join the table here and why everything downstream of
// `sourceValue` can stop caring which kind of source it is holding.
//
// AFTER THE MACROS, NEVER BEFORE. Every source index a saved patch has ever held means the macro it
// meant, and it goes on meaning it. The same shape the reference gives its own bus, where the global
// sources sit below `kFirstLocalSource` and the local ones extend past it.
//
// A DORMANT MECHANISM UNTIL IT IS DECIDED OTHERWISE. Nothing in the editor offers these: the modulator
// strip builds its cards over `kMacroCount` by name and the arm refuses a focus above it, so widening
// the table here shows nobody anything. That is deliberate — the display question (meters, or real
// routable sources) is a design question, and the answer should not be decided by what happened to be
// easy to build.
inline constexpr size_t kFirstWeightSource = kMacroCount;
inline constexpr size_t kWeightSourceCount = kMaxSounds;

// AND THE SOURCES THAT ARE DEVICES — the modulator rack's envelopes.
//
// A macro is a hand-set position and a weight is a reading; an envelope is neither. It is a device
// with state of its own, gated by the note stack, advanced by the engine on the puck's own control
// grid and published here the way a weight is (ParamSet::setEnvelopeSource). What it shares with the
// other two is again only the last step — a number an amount scales into a destination — which is
// why it joins the table here rather than growing a second mechanism beside it.
//
// AFTER THE WEIGHTS, NEVER BEFORE, for the reason stated above the weights: a source's INDEX is what
// a saved patch stores, and every index that has ever meant something goes on meaning it. The
// chains block names a source by KIND and index (`rack/rack_state.h`, through `feathersCoreCodec`),
// so this growth needs no format change and a patch written here loads in a build without it.
//
/// FOUR RATHER THAN ONE, and one card rather than four. The rack ships a single DAHDSR today because
/// there is no affordance yet for adding a second — the browser is the next arc — but a source
/// index is PERMANENT, so the pool has to be sized before the browser rather than after it. Four
/// costs four rows of zeros in every patch and forecloses nothing; discovering later that the pool
/// is one would cost every patch written in between. This is the same "dormant until it is decided
/// otherwise" posture the weight sources shipped in, and for the same reason.
// ONE DEVICE SLOT SPACE OVER BOTH CHAINS — the fx lane and the modulator rack, addressed together.
//
// WHY THEY SHARE A SPACE RATHER THAN HAVING ONE EACH. By design a docked device's controls are
// not Feathers' parameters at all: a docked effect's controls are not on Feathers' CLAP surface, they
// are swallowed and reached by internal modulation, and modulators work exactly the same way. Once
// the filter's controls are device controls rather than clap params,
// an effect and a modulator are the same KIND of thing to this table — a device with declared
// controls and declared outputs — and giving them separate address spaces would be the host deciding
// a device's capabilities from its lane, which is the assumption this whole arc exists to refuse.
//
// A note effect that both passes notes and publishes modulation is the case that settles it: it
// belongs to no lane cleanly, and a lane-split table would have to hold it twice or pick a side.
//
// THE FX LANE COMES FIRST so that the rack's slots keep a stable relationship to each other, and the
// device slot index is what the runtime matrix is laid out against. Note that a saved patch does NOT
// store this index — it stores an instance id (mod_rack.h) — so this ordering is a runtime layout
// decision and not a format one, and it can be changed without touching anybody's patch.

/// **HOW MANY DEVICES THE FX LANE STARTS WITH ROOM FOR — A STARTING SIZE, NOT A LIMIT** (unbounded devices,
///). Every store sized from it grows: the record and its
/// published state on the message thread, the engine, the synth's chains and the readings pools on the
/// message thread and adopted at a drain with moves only. The name is the old one so a reader of the history
/// can follow it; nothing refuses a device at it any more.
inline constexpr size_t kMaxFxSlots = 8;

/// **HOW MANY DEVICES THE MODULATOR STRIP STARTS WITH ROOM FOR** — a starting size, as `kMaxFxSlots` is.
inline constexpr size_t kMaxRackSlots = 4;

/// BOTH CHAINS' STARTING SIZES — the engine's pool when it is built, and nothing more: a device is named by
/// the id it wears, not by where in the pool it sits (kill-lanes §2.4b), and the pool grows.
inline constexpr size_t kMaxDeviceSlots = kMaxFxSlots + kMaxRackSlots;

/// **"NO POSITION" — A REFUSED ADD, A DEVICE NOT FOUND, NO FOCUS — SPELLED AS ITSELF** (unbounded devices
/// step 1). It was spelled as the chain's capacity (`kMaxFxSlots`, `kMaxRackSlots`, `kMaxRackEnvelopes`),
/// which is only "not a position" while the capacity cannot grow: once it can, the old number names a real
/// device. The same value as `backplane::kNoPosition`, so a `< capacity` guard still refuses it.
inline constexpr size_t kNoPosition = static_cast<size_t>(-1);

/// The source index of the sound in `slot`.
constexpr size_t weightSourceOf(size_t slot) { return kFirstWeightSource + slot; }

/// Whether `source` is one of the weight sources rather than one of the macros.
///
/// BOUNDED BY THE NEXT CLASS'S FIRST INDEX, NOT BY THE TABLE'S END, and that distinction is not
/// pedantry — it was a live out-of-bounds read for the length of one build. This used to say
/// `< kModSourceCount`, which was the same thing while the weights WERE the last class; the moment
/// the envelopes were appended it started answering true for them, and slotOfWeightSource would then
/// have indexed weightSource_ past its end on every read of an envelope's value.
///
/// The lesson generalises to whatever class is appended next: a class predicate written against the
/// end of the table is a predicate that silently annexes its successor. Bound each one by its own
/// extent.
constexpr bool isWeightSource(size_t source) {
	return source >= kFirstWeightSource && source < kFirstWeightSource + kWeightSourceCount;
}

/// Which slot a weight source is about. Meaningless for a macro — ask isWeightSource first.
constexpr size_t slotOfWeightSource(size_t source) { return source - kFirstWeightSource; }

// THE CLASSES PARTITION THE TABLE: every index is exactly one kind of source, and the classes sit
// end to end with no gap and no overlap. This is what isWeightSource's comment above was bought
// with — the bound it used to carry passed every existing case and was still wrong.
static_assert(kFirstWeightSource == kMacroCount, "the weights start where the macros end");

/// **THE STACK INDEX** (backlog KE step 3; voice-stacking doc §3.3): a host-end source, after the weights, whose
/// value is per PART of a stacked voice, −1..+1, set by the Shape, Mapping and Curve. Its cell reads 0, so the
/// generic resolve sees the stack's centre; the synth sums the amounts routed from it into each destination it may
/// reach and gives part k `amount × index[k] × span` on top (`ParamSet::stackIndexAmount`). Routes are linear, which
/// is what makes that exact. A route carries its source by KIND (`rack::SourceKind::Stack`), so appending this
/// class moves no saved route.
inline constexpr size_t kFirstStackSource = kFirstWeightSource + kWeightSourceCount;
inline constexpr size_t kStackSourceCount = 1;
inline constexpr size_t kStackIndexSource = kFirstStackSource;
/// Bounded by its own extent, for the reason `isWeightSource` states.
constexpr bool isStackSource(size_t source) {
	return source >= kFirstStackSource && source < kFirstStackSource + kStackSourceCount;
}
/// EVERY HOST-END SOURCE: the macros, the weights, the stack index — the length of the host node's output cells.
inline constexpr size_t kCoreSourceCount = kMacroCount + kWeightSourceCount + kStackSourceCount;
static_assert(kFirstStackSource + kStackSourceCount == kCoreSourceCount, "the stack index ends the host's sources");

/// WHAT MAY BE MODULATED, and this list is the mock's, verbatim:
///
///     const MOD_TARGETS=['path','posx','posy','level','glide','fine']; // engine-honest destinations
///
/// Six, and not one identity parameter among them. That is the mock answering the question this
/// feature would otherwise have had to ask: a modulator PLAYS the instrument, it does not rewrite what
/// the instrument is. Zone, Offset and Gamma stay where a hand and an automation lane put them, which
/// is also what keeps A32 true — Explore watches identity, and identity does not move under a
/// modulator, so the chart cannot be made to repaint at modulation rate.
///
/// The engine agrees with the mock independently. Level, Glide and Fine reach the sample loop
/// directly; Path and Pos X/Y are a character rebuild, coalesced by Character::operator== and floored
/// by the bank's fade guard — exactly the cost a host automating the puck already pays.
///
/// AND THEN WEATHER AND WIDTH, which the mock does not list because neither was a parameter when it
/// was drawn. They were brought in on 2026-08-19, as a general principle rather than the two
/// entries: EVERY CONTINUOUS PERFORMANCE PARAMETER IS MODULATABLE. Weather is how much of the terrain
/// the puck's position lets through and Width is the master stereo scale; both are amounts you play
/// with, neither is what a sound IS, so both belong here and A32 is untouched by them — Explore
/// deliberately watches neither.
///
/// THE TEST TO APPLY TO THE NEXT CANDIDATE: **does modulating it change something OTHER THAN
/// ITSELF?** If it does, it does not belong here, whatever else is true of it.
///
/// This is the sharpened form of the rule above, and it is sharper for a reason — "is it
/// continuous" needs a judgement call and gets Place X/Y wrong. Place looks like a perfect candidate:
/// continuous, performance-ish, obviously fun to sweep. But moving a sound's ANCHOR changes that
/// sound's own weight in the blend, and every per-sound scalar blended by that weight moves with it.
/// MEASURED, by the session that built the FM engine: per-sound Width at 1.0 takes sound A from
/// weight 1.000 to 0.667, and its FM Depth and Smoothness contributions fall in exact proportion.
/// So an LFO on Place X is not a
/// modulator on a position — it is a modulator quietly amplitude-modulating the whole blend, and
/// nothing on screen would say so.
///
/// Weather and Width pass the test: each changes what it names and nothing else. Place fails it.
/// Enable fails a different way — it is stepped, and a stepped destination is a switch chattering at
/// the control rate rather than a setting being played, which is why the filter's Type and Slope
/// decline too.
///
/// IF PLACE IS EVER WANTED HERE, the weight coupling is the thing to settle first, and it is a
/// design question rather than a bug to fix.
///
/// APPENDED, NEVER INSERTED. They sit after the mock's six because a destination's INDEX is what a
/// saved patch stores; a patch written before this rule still means Path at 0 and Fine Tune at 5.
///
/// std::to_array rather than a spelled-out size, so a destination added here is ONE line rather than
/// one line and a number that has to agree with it.
inline constexpr auto kCoreModDestinations = std::to_array<ParamId>(
    {ParamId::Path, ParamId::PosX, ParamId::PosY, ParamId::VoiceLevel, ParamId::GlideTime,
     ParamId::FineTune, ParamId::Weather, ParamId::Width,
     // THE STACK'S DETUNE (backlog KE): continuous, an amount you play, and resolved per voice — so an
     // envelope can open a note's fan. Stack itself is stepped and is not here, by the rule above.
     ParamId::StackDetune,
     // AND ITS SPREAD (step 2), for the same reason: continuous, played, resolved per voice.
     ParamId::StackSpread,
     // AND THE STACK INDEX'S AMOUNTS AND CURVE (step 3): continuous, played, resolved per voice (Shape is
     // per note).
     // The two mode choices are stepped and are not here, by the rule above.
     ParamId::StackShapeAmount, ParamId::StackFreePhase, ParamId::StackMapAmount, ParamId::StackCurve,
     // RESONATE'S EIGHT CONTINUOUS SHAPERS (2026-09-26), appended: amounts you play. Its Bow Mode is stepped
     // and is not here, by the rule above.
     ParamId::ResonateWidth, ParamId::ResonateBow, ParamId::ResonateShimmer, ParamId::ResonateStiffness,
     ParamId::ResonateCoupling, ParamId::ResonateDecay, ParamId::ResonatePluck, ParamId::ResonateTravel,
     // AND ITS SPEED (2026-10-03): continuous, played, read at the string's next tick.
     ParamId::ResonateSpeed,
     // THE OUTPUT FADER AND THE READ PHASE (2026-10-03), appended: Level is a fader like any other, and Phase is
     // the destination the whole of its design is for — slow sources drift it, fast ones are PM.
     ParamId::Level, ParamId::OscPhase,
     // THE BASIC SHAPES MIX (2026-10-03), appended: four amounts you play (an LFO sweeping sine into saw is the point).
     ParamId::ShapesSine, ParamId::ShapesTriangle, ParamId::ShapesSaw, ParamId::ShapesSquare,
     // COARSE (2026-10-03), appended: continuous pitch in semitones, the destination for wide pitch sweeps.
     ParamId::Coarse,
     // A-PH (2026-10-18), appended: Sub priority's Sub and Ceiling, continuous.
     ParamId::AdditiveSub, ParamId::AdditiveCeiling,
     // OE (2026-10-26), appended: the engine vocoder's Bands, Attack and Release, continuous (read engine-wide).
     ParamId::AdditiveVocodeBands, ParamId::AdditiveVocodeAttack, ParamId::AdditiveVocodeRelease,
     // OE2 (2026-10-27), appended: Sibilance, continuous (read engine-wide).
     ParamId::AdditiveVocodeSibilance,
     // THE KARPLUS-STRONG CONTROLS (2026-10-31), appended: continuous, read engine-wide every block. Seed is stepped.
     ParamId::KsDecay, ParamId::KsDecayTrack, ParamId::KsRelease, ParamId::KsHardness, ParamId::KsPickup,
     ParamId::KsWidth,
     // AND VELOCITY'S DEPTH ON HARDNESS (2026-10-31), continuous, appended.
     ParamId::KsVelocity,
     // THE DRUM ENGINE'S KIT WALK (2026-11-05), appended: the engine-wide amounts you play (a slow LFO breathes the kit).
     ParamId::DrumWalkX, ParamId::DrumWalkY,
     // AND THE KIT-WIDE LAYER (2026-11-06), appended: Gamma, Zone, Offset and the seven trims, every one modulatable.
     ParamId::DrumKitGamma, ParamId::DrumKitZone, ParamId::DrumKitOffset, ParamId::DrumKitPitch, ParamId::DrumKitLength,
     ParamId::DrumKitSweep, ParamId::DrumKitTone, ParamId::DrumKitNoise, ParamId::DrumKitSnap, ParamId::DrumKitMetal});

/// HOW MANY DESTINATIONS A TENANT'S DECLARATIONS CONTRIBUTE — its own `modulatable` flags, counted.
constexpr size_t modDestinationsIn(std::span<const fx::ParamDesc> descs) {
	size_t count = 0;
	for (const fx::ParamDesc& desc : descs) {
		if (desc.modulatable) {
			++count;
		}
	}
	return count;
}

/// Those same declarations SEATED AT THE IDS THE PLUGIN GAVE THE BLOCK, appended to `out` at `at`,
/// and the new end returned.
///
/// The id arithmetic is the block rule this file states at the tenant ids: a device's params occupy
/// consecutive ids from its block's base, in the device's own declaration order. params.cpp holds the
/// static_assert that fails the build if a device's descriptor list and its id block ever disagree
/// about their length, which is the same fact this depends on.
constexpr size_t appendModDestinations(std::span<ParamId> out, size_t at,
                                       std::span<const fx::ParamDesc> descs, ParamId base) {
	for (size_t i = 0; i < descs.size(); ++i) {
		if (descs[i].modulatable) {
			out[at] = static_cast<ParamId>(static_cast<uint32_t>(base) + static_cast<uint32_t>(i));
			++at;
		}
	}
	return at;
}

/// AND THE WHOLE LIST: the six above, then the lane's tenants, in one derivation.
///
/// THE SIX COME FIRST AND STAY FIRST, and that is not a style choice. A destination's INDEX is what a
/// saved patch stores — the routing block is a table of amounts, not a list of names — so every patch
/// ever written means the six by the positions they hold here. Tenants append; nothing inserts.
///
/// A TENANT IS ADDED IN TWO PLACES, both of them here: a term in the count and a call in the builder.
/// It is not a hand-kept list of ids, which is the thing this replaced: the filter says which of its
/// controls are modulation destinations in its own descriptors (fx::ParamDesc::modulatable), where it
/// already says everything else about them, and this reads that. Tenant 2 declares the same flag and
/// arrives with modulatable params rather than with a second copy of this knowledge in src/plugin.
/// AND THE PER-SOUND PERFORMANCE KNOBS — Width, FM Depth and Smoothness, one of each per slot.
///
/// THE RULE APPLIED RATHER THAN EXTENDED. The principle is already written above the core six:
/// *EVERY CONTINUOUS PERFORMANCE PARAMETER IS MODULATABLE*, decided on 2026-08-19 as a general rule
/// rather than as two entries. These twenty-four are continuous, they are amounts you play with, and
/// none of them is what a sound IS — so they belong here, and they were missing only because they
/// were added to the plugin after the list was last touched. It was noticed from the other end: the width, fm and smooth controls did not glow in mod mode.
///
/// A32 IS UNTOUCHED. Explore watches IDENTITY — Zone, Offset, Gamma — and not one of these three is
/// that, which is the same test the Weather and Width entries above had to pass.
///
/// THREE PER SLOT RATHER THAN A FOURTH: Place X/Y are where a sound SITS in the constellation, which
/// is identity-adjacent geometry rather than a performance amount, and Enable is a switch — a
/// stepped control under a modulator is a switch chattering at 750 Hz rather than a setting being
/// played.
/// EIGHT NOW, NOT THREE. The five field modifier knobs (2026-08-25) meet the same test the trio above
/// met: continuous, an amount you play with, and not one of them is what a sound IS. Explore watches
/// IDENTITY — Zone, Offset, Gamma — and a scale over how much a FIELD contributes at the identity the
/// sound already has is not that, so A32 is untouched here exactly as it was for Width and Weather.
///
/// The brief named modulation as the point rather than a consequence: field modifier knobs in the
/// explore view (as mod targets).
/// AND TEN SINCE 2026-10-10: the additive Shape Amount (+C) joins at the end of each slot's run, Warp's kind.
/// AND SIXTEEN, the same day: the six additive bias knobs (0x0280 block) follow it, biases you play like the five.
/// AND TWENTY-EIGHT SINCE THE SPECTRAL DRAWER WENT PER SOUND (2026-10-10): its twelve continuous controls follow the bias
/// knobs in each slot's run, in kSpectralContinuousControls' order.
/// AND TWENTY-ONE BEFORE IT (2026-10-31): the string's five field scalers follow the bias knobs.
/// AND THE DRUM PAD'S SIXTEEN AMOUNTS (2026-11-05), after the spectral drawer's, in kDrumContinuousControls' order.
inline constexpr size_t kPerSoundDestinationsPerSlot = 21 + kSpectralContinuousControls.size() + kDrumContinuousControls.size();
inline constexpr size_t kPerSoundDestinationCount = kMaxSounds * kPerSoundDestinationsPerSlot;

/// HOW MANY DESTINATIONS HAVE A PARAMETER BEHIND THEM — the core six (now eight) and the per-sound
/// knobs.
///
/// THE FX LANE'S TENANTS ARE NOT HERE ANY MORE. A docked device's controls are destinations, but they
/// are DEVICE destinations — named by the device's id and its own control index (`Destination::control`)
/// — because a swallowed device has no parameter for a destination to stand behind. This term
/// existed only while the filter's controls were both.
/// **AND EACH SOUND'S IDENTITY — ZONE, OFFSET, GAMMA — AS EDIT-ONLY DESTINATIONS (backlog PQ, 2026-11-05).** A32 still
/// holds: nothing MODULATES what a sound is, because Explore draws identity from the base. What reaches these is a
/// RELATIVE macro, whose route is not a modulation at all but an edit of the base on the message thread
/// (`ui::RelativeMacroGesture`) — the same kind of write a hand on the Explore corner makes. So: the bridge lets only a
/// Relative macro route here (`ParamBridge::mayRoute`), and the engine never compiles a route here
/// (`ParamSet::rebuildCoreInputs`), so one left behind by a macro switched back to Absolute is inert.
/// After every other parameter destination, so the per-sound runs above keep their shape.
inline constexpr std::array<SoundField, 3> kEditOnlyFields { SoundField::Zone, SoundField::Offset, SoundField::Gamma };
inline constexpr size_t kEditOnlyDestinationCount = kMaxSounds * kEditOnlyFields.size();
inline constexpr size_t kFirstEditOnlyDestination = kCoreModDestinations.size() + kPerSoundDestinationCount;

inline constexpr size_t kParamDestinationCount = kFirstEditOnlyDestination + kEditOnlyDestinationCount;

/// A core destination index that only a Relative macro may reach and the engine never resolves (see above).
constexpr bool isEditOnlyDestination(size_t coreIndex) noexcept {
	return coreIndex >= kFirstEditOnlyDestination && coreIndex < kParamDestinationCount;
}

constexpr std::array<ParamId, kParamDestinationCount> buildModDestinations() {
	std::array<ParamId, kParamDestinationCount> out {};
	size_t at = 0;
	for (const ParamId id : kCoreModDestinations) {
		out[at] = id;
		++at;
	}
	// The fx lane's tenants used to be appended here — see kParamDestinationCount for why a swallowed
	// device's controls are device destinations instead.
	// AND THE PER-SOUND KNOBS, AFTER THE LANE AND NEVER BEFORE IT. Appending them to
	// kCoreModDestinations would have been the obvious edit and it would have been the wrong one: the
	// filter's destinations would each have shifted by twenty-four, and a destination's INDEX is what
	// a saved patch stores — every routing anyone has made to a filter control would have silently
	// become a routing to something else.
	//
	// Slot-major, so a sound's eight sit together and a slot added later appends rather than
	// interleaves.
	//
	// THE FIVE FIELD MODIFIER KNOBS JOIN AT THE END OF EACH SLOT'S RUN, 2026-08-25 — which moves
	// every per-sound destination index above slot 0, and that is FINE HERE for a reason that has to
	// be said out loud rather than assumed: a destination's INDEX is what a saved patch stores, which
	// is exactly why the block comment above refuses to touch the ordering ahead of the fx lane. What
	// makes this safe is not the position, it is CLAUDE.md's 2026-08-22 relaxation — there are no
	// saved patches or projects, so there is no stored routing to re-point. The day that stops being
	// true, this loop is append-only like the one above it.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		out[at++] = soundParam(slot, SoundField::Width);
		out[at++] = fmDepthParam(slot);
		out[at++] = smoothnessParam(slot);
		out[at++] = quantizeParam(slot);
		out[at++] = syncParam(slot);
		out[at++] = feedbackParam(slot);
		out[at++] = pointsParam(slot);
		out[at++] = gritParam(slot);
		// WARP DEPTH IS A DESTINATION and the MODE is not — the mode is stepped, and stepped controls
		// are excluded from this table by rule (see OscType's note). Modulating the amount of one
		// shape is a sweep; modulating which shape is a switch.
		out[at++] = warpParam(slot);
		// AND THE ADDITIVE SHAPE AMOUNT (2026-10-10), the same reasoning: the amount is a sweep, the Shape a switch.
		out[at++] = shapeAmountParam(slot);
		// AND THE SIX ADDITIVE BIAS KNOBS (2026-10-10), continuous biases like the five field modifiers.
		out[at++] = additiveRolloffParam(slot);
		out[at++] = additiveFormantParam(slot);
		out[at++] = additiveOddEvenParam(slot);
		out[at++] = additiveCombParam(slot);
		out[at++] = additiveDetailParam(slot);
		out[at++] = additiveOffGridParam(slot);
		// AND THE STRING'S FIVE FIELD SCALERS (2026-10-31), biases you play like the others.
		out[at++] = ksBrightnessParam(slot);
		out[at++] = ksSustainParam(slot);
		out[at++] = ksPickParam(slot);
		out[at++] = ksNoiseParam(slot);
		out[at++] = ksStretchParam(slot);
		// AND THE SPECTRAL DRAWER'S CONTINUOUS CONTROLS (2026-10-10): per sound now, so per slot here.
		for (const SpectralControl c : kSpectralContinuousControls) {
			out[at++] = spectralParam(slot, c);
		}
		// AND THE DRUM PAD'S AMOUNTS (2026-11-05): per sound, so per slot here.
		for (const DrumControl c : kDrumContinuousControls) {
			out[at++] = drumParam(slot, c);
		}
	}
	// AND THE IDENTITY, EDIT-ONLY (backlog PQ), slot-major in `kEditOnlyFields` order.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		for (const SoundField field : kEditOnlyFields) {
			out[at++] = soundParam(slot, field);
		}
	}
	// A COUNT THAT DID NOT FILL THE LIST HANDS BACK AN EMPTY ONE, which the static_assert under this
	// turns into a build failure. Silently leaving a zero in the tail would be a destination with no
	// parameter behind it, and every consumer would find that out at a different moment.
	return at == kParamDestinationCount ? out
	                                    : std::array<ParamId, kParamDestinationCount> {};
}

inline constexpr std::array<ParamId, kParamDestinationCount> kModDestinations =
    buildModDestinations();

/// **A SOUND'S KNOBS** — every per-sound control that is a SETTING of the sound rather than its identity, its place,
/// its switch or its width (those travel in `ui::SlotState` already): its destination run less Width, which leads it.
/// What a clone copies, a reorder carries and a delete resets with the sound. Derived from the run, so a per-sound knob added to it joins by construction.
/// AND THE SPECTRAL DRAWER'S STEPPED CONTROLS (its switches, Mode, Link) after the run: settings of the sound that are
/// not destinations, so the run alone does not hold them.
/// AND THE DRUM PAD'S STEPPED CONTROLS (Kind, Note, Choke, Voices, Trigger, Count) after those, for the same reason.
inline constexpr size_t kPerSoundKnobCount =
    kPerSoundDestinationsPerSlot - 1 + kSpectralSteppedControls.size() + kDrumSteppedControls.size();
constexpr ParamId perSoundKnobParam(size_t slot, size_t k) {
	constexpr size_t run = kPerSoundDestinationsPerSlot - 1;
	constexpr size_t spectral = run + kSpectralSteppedControls.size();
	return k < run        ? kModDestinations[kCoreModDestinations.size() + slot * kPerSoundDestinationsPerSlot + 1 + k]
	       : k < spectral ? spectralParam(slot, kSpectralSteppedControls[k - run])
	                      : drumParam(slot, kDrumSteppedControls[k - spectral]);
}
static_assert(kModDestinations[kCoreModDestinations.size()] == soundParam(0, SoundField::Width),
              "each slot's destination run leads with its Width, which perSoundKnobParam skips");

// **THE CORE'S INTERNAL DESTINATIONS — modulatable, and NOT parameters** (2026-09-21).
//
// The decision: use the internal param type, so as not to spend CLAP params; later a small number may be
// re-connected to external-facing CLAP params, but that is a different workstream.
//
// IT WAS NEVER 1:1. Every docked device's control is a destination with no CLAP id
// (`Destination::control`), the rack host runs `coreDestinations = 0`, and the backplane bounds a core
// destination BY INDEX AND NOTHING ELSE (`routing.h`: `d.index >= axes.coreDestinations`). "A core
// destination is a ParamId" was a fact about this one table, and this is the table growing a second
// region that is not.
//
// THE CORE INDEX SPACE IS NOW TWO REGIONS:
//     [0, kParamDestinationCount)                        parameters, by `kModDestinations`
//     [kParamDestinationCount, kCoreDestinationCount)    internal, by `internalDestinationDesc`
// A saved route spells the first by ParamId and the second by `internalDestinationId` — a STABLE id,
// not the table position, so inserting a parameter destination (which shifts every internal index)
// re-points nothing on disk.
//
// DESCRIBED BY `device::ParamDesc`, the type every device control already uses — the "internal
// param type" — so the span, `modSpan` and the units come from machinery that exists and is tested,
// and so that exposing one to a host later is ADDING a parameter beside it in the same units rather
// than converting between two vocabularies.

/// WHICH HALF OF A SOUND'S PLACE — the polar pair asked for (drift becomes polar in the
/// plane with a theta knob, drift renamed radius; radius and theta are also mod
/// destinations).
enum class TravelAxis : uint8_t { Radius = 0, Theta = 1 };

inline constexpr size_t kInternalDestinationsPerSlot = 2;
inline constexpr size_t kInternalDestinationCount = kMaxSounds * kInternalDestinationsPerSlot;
/// EVERY DESTINATION THE CORE OWNS, parameters and internal both — what `feathersAxes()` declares and
/// what `ParamSet` lays its input ranges out over.
inline constexpr size_t kCoreDestinationCount = kParamDestinationCount + kInternalDestinationCount;

/// A sound's travel axis as an INTERNAL index, `[0, kInternalDestinationCount)`. Slot-major, like the
/// per-sound parameter run, so a sound's pair sits together.
inline constexpr size_t travelInternalIndex(size_t slot, TravelAxis axis) noexcept {
	return slot * kInternalDestinationsPerSlot + static_cast<size_t>(axis);
}
/// ...and as a CORE destination index — what `backplane::Destination::core` takes.
inline constexpr size_t travelDestinationIndex(size_t slot, TravelAxis axis) noexcept {
	return kParamDestinationCount + travelInternalIndex(slot, axis);
}
inline constexpr bool isInternalDestination(size_t coreIndex) noexcept {
	return coreIndex >= kParamDestinationCount && coreIndex < kCoreDestinationCount;
}

/// **WHAT A SAVED ROUTE CALLS AN INTERNAL DESTINATION.** Explicit and append-only, for the reason
/// `ParamId` is: the table position moves whenever a parameter destination is added ahead of it, and
/// a route that stored the position would silently become a route to something else.
/// `0x0100 + slot * 0x10 + axis` — a sound's block, with room in it, shaped like the per-sound
/// parameter block so the two read alike.
inline constexpr uint32_t internalDestinationId(size_t internalIndex) noexcept {
	return 0x0100u + static_cast<uint32_t>(internalIndex / kInternalDestinationsPerSlot) * 0x10u
	       + static_cast<uint32_t>(internalIndex % kInternalDestinationsPerSlot);
}
/// The way back, or -1 for an id this build does not have.
inline constexpr int internalIndexOfId(uint32_t id) noexcept {
	if (id < 0x0100u) {
		return -1;
	}
	const uint32_t slot = (id - 0x0100u) / 0x10u;
	const uint32_t axis = (id - 0x0100u) % 0x10u;
	return slot < kMaxSounds && axis < kInternalDestinationsPerSlot
	           ? static_cast<int>(slot * kInternalDestinationsPerSlot + axis)
	           : -1;
}

/// **ONE BASE TRAVEL** — what the Explore chart spans at zoom x1: 64 zones of the 1023 a zone dial
/// runs. Engine-side because a declared swing is engine-side truth; `ExploreStage::kBaseTravel` is
/// the same number and a `static_assert` there keeps them the same.
inline constexpr double kTravelBase = 64.0 / 1023.0;

/// THE DESCRIPTOR OF AN INTERNAL DESTINATION. Two shapes today, one per axis; the slot does not
/// change what a radius is.
///
/// THE RANGE IS A WINDOW, NOT A LIMIT for Theta — the engine clamps neither the place (a sound's place has no
/// ceiling: the world is meant to be infinite) nor the angle (periodic) — and `modSpan` is what a full
/// swing is worth: ONE WHOLE TURN for Theta, so a ramp into Theta is exactly one orbit. RADIUS IS ROUTED ALONG ITS
/// FACE (radius-mod-jump): its `modSpan` is ONE FACE, so a route's pull is in face units and
/// `playedRadius` (sound_travel.h) turns it into a radius — a full swing from the knob's zero reaches the face's top,
/// the whole plane (`kTravelRadiusMax`, backlog PO). The ring and the readout use the same function.
inline const device::ParamDesc& internalDestinationDesc(size_t internalIndex) noexcept {
	static const device::ParamDesc kRadius { .name = "Radius",
		                                     .min = 0.0,
		                                     .max = kTravelRadiusMax,
		                                     .def = 0.0,
		                                     .stepped = false,
		                                     .unit = device::Unit::Number,
		                                     .names = {},
		                                     .modulatable = true,
		                                     .offsetsMayLeaveRange = true,
		                                     // ONE FACE PER FULL SWING, IN FACE UNITS — see `playedRadiusFace`.
		                                     .modSpan = 1.0 };
	static const device::ParamDesc kTheta { .name = "Theta",
		                                    .min = -3.14159265358979323846,
		                                    .max = 3.14159265358979323846,
		                                    .def = 0.0,
		                                    .stepped = false,
		                                    // RADIANS, READ IN DEGREES — a real angle (cos/sin of it).
		                                    .unit = device::Unit::Degrees,
		                                    .names = {},
		                                    .modulatable = true,
		                                    .offsetsMayLeaveRange = true,
		                                    .modSpan = 2.0 * 3.14159265358979323846 };
	return internalIndex % kInternalDestinationsPerSlot == static_cast<size_t>(TravelAxis::Radius)
	           ? kRadius
	           : kTheta;
}

// AND THE DESTINATIONS THAT ARE NOT PARAMETERS AT ALL — a device's own controls.
//
// THE UNIFIED MODULATION SURFACE, decided on 2026-08-22: point a macro at an envelope's
// attack, or an envelope at its own. Bitwig lets you modulate a modulator and so does this; a
// surface that stopped at the device boundary would not be unified, it would be two surfaces.
//
// A DEVICE'S CONTROL HAS NO PARAMETER BEHIND IT — that is the rule that keeps the rack open to a
// browser — so `kModDestinations` cannot name one, and nothing here tries. A route names it as
// `backplane::Destination::control(id, control)`: the device by the id it wears, the control by the
// device's own index, and the device's descriptor says whether that control is modulatable
// (`RackEngine::rebuildInputs` builds no input for one that is not).
//
// THE COORDINATE BLOCK THAT STOOD HERE — `kFirstDeviceDestination`, a stride of columns per slot,
// `deviceDestinationOfOrdinal` and its two decoders, and the argument for laying a matrix out at the
// stride rather than packed — is deleted with the matrix (kill-lanes §2.4b). What survives of it is
// nothing: a destination is named by its device and the control's own index, and the route stores grow (backlog OJ).

/// **WHERE THE ENGINE'S ROUTE STORES START — NOT A BOUND** (backlog OJ, 2026-10-19;).
///
/// `kMaxRouteEdges` stood here: one route per destination plus slack, and past it a route was dropped and
/// counted. Every store a route passes through now grows in place on the message thread (`RouteList`,
/// `ParamSet::reserveRoutes`, `RackEngine::reserveRoutes`) before the publish that needs it, so this is only the
/// room a host has before its first growth — about the old cap's size, so nothing allocates more than it did until a
/// patch routes past it. A host that never wires the growth (`wireRouteGrowth`) keeps this room and COUNTS a
/// publish too big for it (`ParamSet::routeRoomRefusals`); it never truncates one.
inline constexpr size_t kInitialRouteRoom = 512;

/// **FEATHERS' OWN AXES, AS A FREE FUNCTION** — the five constants above as a `backplane::Axes`.
///
/// It lived inside `BridgeDeviceHost::axes()`, then `bridge_device_host.h`, then `device_registry.h`
/// (when `maySourceRouteTo` became a call into the backplane's predicate), and moved DOWN here, beside
/// what it is made of. **IT IS A HOST'S SHAPE, NOT AN ADDRESS SPACE** (kill-lanes §2.4b): the counts
/// size the engine's pools, and nothing derives a coordinate from them any more.
inline const backplane::Axes& feathersAxes() noexcept {
	static constexpr backplane::Axes kAxes {
		.coreSources = kCoreSourceCount,
		.coreDestinations = kCoreDestinationCount,
		.deviceSlots = kMaxDeviceSlots,
		.outputStride = device::kMaxDeviceOutputs,
	};
	return kAxes;
}

// THE COUNT AND THE LIST CANNOT DISAGREE, checked in both directions because they are two walks of
// the same flags. One too many is already a compile error — a constexpr subscript past the end is —
// and one too few would otherwise pass silently as a destination with no id, so the tail is checked:
// zero is not a parameter id, and an unwritten slot holds exactly that.
static_assert(!kModDestinations.empty());
static_assert(kModDestinations.back() != static_cast<ParamId>(0),
              "a destination was counted and never seated");

/// THE PARAMETER BEHIND A DESTINATION, OR NOTHING — and every caller goes through this rather than
/// subscripting kModDestinations.
///
/// THE TABLE IS SHORTER THAN THE DESTINATION SPACE since the rack joined it: a rack control is a
/// destination with no parameter, so `kModDestinations[d]` reads past the end for every one of them.
/// Four call sites were doing exactly that when the rack destinations were appended — all four
/// already null-checked what came back, so they degrade to a generic label rather than misbehaving,
/// but the read itself was out of bounds and a test caught it as garbage rather than as a crash.
///
/// Returns ParamId 0, which findParam does not know, so the existing null-checks keep working
/// unchanged.
constexpr ParamId destinationParamId(size_t destination) {
	return destination < kParamDestinationCount ? kModDestinations[destination]
	                                            : static_cast<ParamId>(0);
}

/// EVERY PARAMETER ID IS BELOW THIS — the size of the id-to-index tables, so a lookup is one load rather than a scan
/// (backlog FI: `resolved()` scanned the parameter list on every read, dozens of times a control tick). A table built
/// from an id past it does not compile.
inline constexpr size_t kParamIdSpan = 0x0800; // 0x0400..0x05FF: the per-sound spectral drawer; 0x0600..0x07FF: the drum pads' two blocks

/// Where each id sits in the destination list, -1 where it is not one.
inline constexpr std::array<int16_t, kParamIdSpan> kModDestinationIndexOfId = [] {
	std::array<int16_t, kParamIdSpan> table {};
	table.fill(-1);
	for (size_t i = 0; i < kModDestinations.size(); ++i) {
		const auto at = static_cast<size_t>(kModDestinations[i]);
		if (table.at(at) == -1) { // the FIRST, as the scan it replaced answered
			table.at(at) = static_cast<int16_t>(i);
		}
	}
	return table;
}();

/// Where `id` sits in the destination list, or -1 for a parameter that is not one.
constexpr int modDestinationIndexOf(ParamId id) {
	const auto at = static_cast<size_t>(id);
	return at < kParamIdSpan ? kModDestinationIndexOfId[at] : -1;
}

/// The mock's `Math.max(-2,Math.min(2,…))`: an amount reaches twice the destination's whole range, so
/// full-range modulation is available from any base position rather than only from the bottom of the
/// track.
inline constexpr float kMaxRouteAmount = 2.0f;

/// The mock's `if(Math.abs(routeAmt(id))<.03)delete S.lfo.routes[id]` — a route dragged back to
/// nothing is not a route at 0 %, it is gone, so the ring stops being drawn and the list stops listing
/// it.
inline constexpr float kRouteUnroutedBelow = 0.03f;

// THE HOST'S TWO NUMBERS AND THE CONTRACT'S ARE ONE PAIR OF NUMBERS. device/modulatable.h publishes
// both, because a WIDGET needs them — a drag that sets an amount has to know what to clamp to, and a
// widget told by whoever constructed it is a widget that can be told wrong. These static_asserts are
// what stop the two copies becoming two values.
static_assert(kMaxRouteAmount == device::kMaxAmount);
static_assert(kRouteUnroutedBelow == device::kAmountUnroutedBelow);


/// Whether `destination` is one of the three the puck is made of.
///
/// Named by ParamId rather than by index, so reordering kModDestinations cannot quietly change which
/// ones this is about.
constexpr bool isPuckDestination(size_t destination) {
	// A RACK CONTROL IS NOT A PARAMETER, so it is not one of these — and the bound has to be the
	// PARAMETER count rather than the table's end, or this would index kModDestinations past its own
	// length for every rack destination. The same trap isWeightSource fell into when the envelope
	// sources were appended: a predicate written against the end of a table annexes whatever is
	// appended after it.
	if (destination >= kParamDestinationCount) {
		return false;
	}
	const ParamId id = kModDestinations[destination];
	return id == ParamId::Path || id == ParamId::PosX || id == ParamId::PosY;
}

/// **WHERE THE STACK INDEX MAY ROUTE** (backlog KE step 3): Fine Tune (each part's pitch, the firmware's "osc
/// pitch"), Level (each part's gain, its "osc volume") and, since step 4, Pos X and Pos Y: the part's STAND on the
/// terrain, displaced from the puck by amount × index × span, so it hears the character (and the vowel) standing there.
/// The rest of the doc's table (pan, start phase, FM depth) is a line here when it lands.
constexpr bool isStackIndexDestination(size_t destination) {
	if (destination >= kParamDestinationCount) {
		return false; // bounded by the parameters, for isPuckDestination's reason
	}
	const ParamId id = kModDestinations[destination];
	return id == ParamId::FineTune || id == ParamId::VoiceLevel || id == ParamId::PosX || id == ParamId::PosY
	       || id == ParamId::OscPhase;
}

/// WHETHER THIS SOURCE MAY REACH THIS DESTINATION AT ALL — one authority, and it has to stay one.
///
/// Every road that can create a route asks this, and so does every surface that offers one: the knob's
/// `mappable`, which is both the armed glow and the gate on the drag, and the loader, so a patch
/// edited by hand cannot carry in a route the editor would refuse to draw. Two answers to this
/// question would be a glow that lies — a knob lit for a drag that then does nothing — which is the
/// exact failure the honesty rule exists to refuse.
///
/// A WEIGHT SOURCE MAY NOT REACH THE PUCK — a design decision, and one of TASTE rather than a
/// stability necessity, which is the part worth writing down for whoever wonders whether it can be
/// lifted.
///
/// The technical position is that the loop is safe. A weight is a function of where the puck stands,
/// so a weight routed to Path feeds back — but the value a route reads is the reading PUBLISHED from
/// the character the engine last built (ParamSet::setWeightSources), so resolved() loads a stored
/// number rather than re-evaluating the blend, and the loop closes across a block boundary the way a
/// feedback delay closes through its buffer. It cannot recurse and it cannot run away; it would be a
/// patch that plays itself at block rate. The decision is not to ship an instrument
/// that can wander off while nobody is touching it. The other three destinations — level, glide, fine
/// — are what these sources are for and stay open.
///
/// CLASS-SPECIFIC, NOT A BLANKET OVER THE PUCK. A macro reaches Path exactly as it always has, which
/// is most of what the modulation panel exists to do.
///
/// AN ENVELOPE MAY REACH EVERYTHING, INCLUDING THE PUCK, and that is not an exception to the rule
/// above — it is the rule read correctly. What the weight sources are foreclosed from is a LOOP: a
/// weight is a function of where the puck stands, so a weight pointed at Path feeds back, and an instrument
/// that can wander off while nobody is touching it is excluded. An envelope
/// reads nothing at all except the gate and the clock. Pointing one at the puck is the single most
/// obvious thing to do with it — a note that sweeps the blend as it is held — and there is no loop
/// to close.
/// NOT THE AUTHORITY. **THE AUTHORITY IS `ChainRecord::mayRoute`** (kill-lanes step 4b), which asks the
/// graph whether the route would close a cycle; a surface reaches it through `ParamBridge::mayRoute`.
/// This line used to point at `maySourceRouteTo`, which is the RETIRED band rule and a test fixture now.
///
/// THIS ANSWERS ONLY THE HALF THAT NEEDS NO DEVICE: is the pair in the table, and is it the
/// weight-source-to-puck case that is excluded. The other half — whether a rack destination names a
/// control that actually EXISTS on the device in that slot — cannot be answered here, because the
/// stride deliberately leaves slack columns past each device's real width and only the device knows
/// where its width ends.
///
/// THE SPLIT IS NAMED RATHER THAN IMPLIED BECAUSE THE UNNAMED VERSION SHIPPED A BUG FOR ONE BUILD.
/// The first draft of the stride work left this function called `maySourceRouteTo` and device-blind,
/// which quietly made every slack column routable: the DAHDSR's "Smoothing On" switch — declared NOT
/// modulatable — resolved to destination 48 and would have accepted a route. All 24 suites passed.
/// It was caught by asserting the value rather than by trusting the green, which is this project's
/// own rule arriving exactly where it said it would.
///
/// So the two halves are two names. A caller that reaches for this one gets a compile-time-obvious
/// wrong answer rather than a silently permissive one.
///
/// IT LIVES IN `detail` SO THAT REACHING FOR IT IS VISIBLE, and that is a guard against a specific
/// accident rather than decoration. The compiler already makes the OTHER failure impossible: keeping
/// a second definition of the authority alongside this one is a hard redefinition error, because
/// device_registry.h includes this header and both would be one signature in one namespace — tested
/// by injecting the duplicate, not reasoned about.
///
/// What the compiler CANNOT catch is a merge conflict resolved by renaming a CALL SITE to this
/// function. That compiles perfectly and silently drops the device half, so every slack column
/// becomes routable again and the whole suite stays green — which is exactly the bug this split was
/// created to fix, arriving by the one road the split left open. feathers-cc named it.
///
/// A namespace does not forbid it; it makes it LOUD. `detail::` in a call site is a reader asking to
/// reach past the front door, which is a thing review notices and a mechanical conflict resolution
/// does not produce by accident. The invariant to hold, and it is checkable in one line:
///
///     detail::maySourceRouteToIgnoringDevice has exactly ONE caller, and it is the authority.
///     Anything else calling it is a gate that has quietly lost its device half.
namespace detail {

/// Renamed from `maySourceRouteToUnchecked`: "unchecked" said what it skipped without saying what it
/// skipped it OF, and a name that does not say what is missing is a name somebody will read as
/// "the fast one".
///
/// **BY ENDPOINT SINCE KILL-LANES STEP 2** (`backplane::CorePolicy`): it reads the two HOST ends — a
/// device end is not its business — and the table bounds it used to check are the authority's own.
/// Feathers' one rule: a weight may not reach the puck.
constexpr bool maySourceRouteToIgnoringDevice(backplane::Source source, backplane::Destination destination) {
	// **THE STACK INDEX REACHES ONLY WHAT A PART HAS OF ITS OWN** (voice-stacking doc §3.3): its value is per part,
	// so a destination the parts share — a filter corner, the puck, a device's control — has no part to give it to.
	if (!source.isDevice() && isStackSource(source.index)) {
		return !destination.isDevice() && isStackIndexDestination(destination.index);
	}
	return !(!source.isDevice() && isWeightSource(source.index) && !destination.isDevice()
	         && isPuckDestination(destination.index));
}

} // namespace detail

// 27 = the original 25, plus Width (0x0043, the terrain-stereo arc), plus Osc Mode (0x0064, the
// retrigger question — V21d as a toggle, V24 as the three-way); the macros count themselves.
// kMaxSounds * 11 = each slot's placement block filled out: Place X/Y, Enable, the per-sound Width
// (V13, +3), the per-sound FM depth knob (doc §10, +4), the per-sound smoothness override (doc §11,
// +5), and the five FIELD MODIFIER knobs — Quantize, Sync, Feedback, Points, Grit (+6..+A,
// 2026-08-25). The fx lane's tenants add their own, and they COUNT THEMSELVES: the filter's block is as long as
// the filter's descriptor list, so a control appended in the module widens the table here without
// anyone remembering to come and say so.
/// THE FILTER'S TERM IS GONE FROM THIS SUM, which is the whole of what the swallowing costs the
/// host's parameter list: eleven controls that were parameters of Feathers are now state of a device
/// docked inside it.
// AND WARP, 2026-08-28: one ENGINE-WIDE mode (hence 27 -> 28) and one per-sound depth knob
// (+B, hence 11 -> 12). The split is not a taste — an enum cannot be blended, and the flagship
// renders one cycle from the blended recipe. See dsp/warp.h.
// AND THE STACK, 2026-09-25: Stack, Stack Detune and Stack Spread (backlog KE), hence 28 -> 31.
// AND THE RESONATE MODEL, 2026-09-25: one more ENGINE-WIDE choice, the string's physics, hence 31 -> 32.
// AND THE STACK INDEX, 2026-09-26: Shape, its Amount, Free Phase, Mapping, its Amount and Curve (KE step 3), 32 -> 38.
// AND RESONATE'S SHAPING, 2026-09-26: nine engine-wide params (Width .. Travel), hence 38 -> 47.
// AND RESONATE SPEED, 2026-10-03: the string's tick rate, hence 47 -> 48.
// AND 2026-10-03 (voice-level): Attack and Release out, Level (the fader), Phase and Phase Width in, hence 48 -> 49.
// AND THE BASIC SHAPES MIX, 2026-10-03: four engine-wide amounts (Sine .. Square), hence 49 -> 53.
// AND COARSE, 2026-10-03: a continuous pitch offset in semitones, hence 53 -> 54.
// AND THE ADDITIVE SHAPE, 2026-10-10: one engine-wide Choice (hence 54 -> 55) and one per-sound amount (+C, hence
// 12 -> 13).
// AND THE SIX ADDITIVE BIAS KNOBS, 2026-10-10: per sound, in the second per-sound block (0x0280), hence 13 -> 19.
// AND THE SPECTRAL DRAWER, 2026-10-10: nineteen controls PER SOUND (the 0x0400 block), counted by their own list.
// AND THE VOWEL FORMANT, 2026-10-12: three more per sound in the same block (kSpectralControlCount 19 -> 22).
// AND PARTIALS, 2026-10-12: two more (22 -> 24).
// AND DAMP, 2026-10-12: three more (24 -> 27).
// AND STRETCH, 2026-10-12: two more (27 -> 29).
// AND SHIFT, 2026-10-12: two more (29 -> 31).
// AND A-PH, 2026-10-18: Additive Phases, Sub and Ceiling (+3).
// AND OE, 2026-10-26: the engine vocoder's analysis, Resolution .. Key Track (+6).
// AND OE2, 2026-10-27: Spacing, Widths, Stereo, Sibilance (+4).
// AND THE KARPLUS-STRONG ENGINE, 2026-10-31: eight engine-wide controls (+8) and five field scalers per sound (19 -> 24).
// AND THE DRUM ENGINE, 2026-11-05: five engine-wide controls (+5) and a pad block per sound (kDrumControlCount).
// AND THE KIT-WIDE LAYER, 2026-11-06: Kit Gamma, Zone, Offset and seven field trims (+10).
inline constexpr size_t kParamCount = 49 + 4 + 1 + 1 + 3 + 6 + 4 + 8 + 5 + 10 + 6 * 3 + kMaxSounds * 24 + kMacroCount
                                      + kMaxSounds * kSpectralControlCount + kMaxSounds * kDrumControlCount;

/// Every declared param, in table order. Host param indices are positions in this span.
std::span<const ParamDesc> paramDescs();

/// Table position of `id`, or -1. Also the storage index inside ParamSet.
int paramIndexOf(clap_id id);

const ParamDesc* findParam(clap_id id);

/// FNV-1a over the ordered (id, name) list — what a host's automation lane and a saved patch address
/// a param by. See tests/test_params.cpp for the recorded constant and how to change it on purpose.
uint64_t paramIdFingerprint();

/// FNV-1a over the ordered (id, min, max, default) list, hashing the doubles as bit patterns.
///
/// Kept separate from the id fingerprint because it catches a different kind of broken patch and
/// should say so when it fails: narrowing a range does not stop an old patch loading, it makes it
/// load quietly clamped — the stored value survives, the sound does not.
uint64_t paramRangeFingerprint();

// Taper readings, exported so the numbers the voice layer acts on and the numbers the host prints
// can only ever come from the same place. Each takes a raw param value, which is what valueToText
// has and the voice layer does not — the voice layer reads them through ParamSet's accessors below,
// off resolved(), and never converts a raw value itself.
double attackReleaseSeconds(double logSeconds);

/// THE TAPER AN "AMOUNT" KNOB WEARS.
///
/// A power law and not a log, because a log has no value at zero and these knobs must be exactly
/// zero at rest: FM Depth's whole identity guarantee is that a patch which has never touched it is
/// byte-identical, and Weather's is the same sentence. `x * x` is zero at zero, one at one, and
/// spends the first half of the travel in the bottom quarter of the range.
///
/// WHICH HALF OF THIS IS MEASURED, because they are not the same half. The SHAPE is: both controls
/// move the sound almost entirely in the bottom of their travel. Weather, with the puck held far from
/// a sound's anchor, covered 98% of its total distance by knob 0.125. FM, on the metric that can
/// actually see it, climbs steeply and then wanders.
///
/// THE EXPONENT IS NOT MEASURED, and saying so is the point. Both controls move the sound through
/// CHARACTER space rather than along one axis, and every distance metric available saturates once two
/// waveforms decorrelate — so a measurement can say "the action is at the bottom" and cannot say
/// whether the right answer is 1.5, 2 or 3. Two is the conventional amount taper and is where this
/// starts; it is one number in one place, and it is to be moved by ear.
inline double amountTaper(double knob) {
	const double x = knob < 0.0 ? 0.0 : (knob > 1.0 ? 1.0 : knob);
	return x * x;
}
double glideSeconds(double taper01);
double decibelsToGain(double db);
int bendRangeSemitonesAt(double index);

/// How a value prints, for a consumer that has a value and no ParamSet.
///
/// The editor is that consumer, and deliberately so: it must never hold a ParamSet, because reading
/// one's modulation from the UI thread is precisely the race the threading note below exists to
/// prevent. Formatting depends on nothing but the table, so it does not need an instance —
/// ParamSet::valueToText is this function, kept as a member because that is what the CLAP extension
/// calls.
bool valueToText(clap_id id, double value, char* out, uint32_t capacity);

/// Its inverse, for the same consumer and on the same terms — the editor's type-in (backlog LS) reads a
/// core knob's typed text through the table a host's own type-in uses. `ParamSet::textToValue` is this.
bool textToValue(clap_id id, const char* text, double* out);

/// Base values plus a modulation offset per param.
///
/// CLAP separates the two: `CLAP_EVENT_PARAM_VALUE` moves the base (that is what automation writes
/// and what `params.value`, presets and state must report), while `CLAP_EVENT_PARAM_MOD` supplies a
/// transient offset that is never part of the patch. What the DSP uses is `resolved()` —
/// clamp(base + offset) — recomputed per block. Keeping the two apart from day one is the point:
/// retrofitting modulation onto a synth that stored one number per param means touching every
/// consumer.
///
/// Per-note modulation (Level carries `CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID`) does not belong here
/// — it is per-voice state and lands with the voice layer. This class holds the global lane only.
///
/// THREADING. The two halves have different rules, which is what makes this safe without a lock.
///
/// `mod_` is plain, and what protects it is exclusivity rather than thread identity: it is never
/// touched from two threads at once. The entry points that reach it are the whole of the argument —
///
///   * process(), audio thread, plugin active.
///   * clap_plugin.reset(), audio thread.
///   * clap_plugin_params.flush(), which CLAP puts on the AUDIO thread while the plugin is active
///     and on the MAIN thread while it is inactive. That second case is why "audio thread only" is
///     the wrong way to state this rule — it is a main-thread write, and it is safe because an
///     inactive plugin has no audio thread to race with.
///
/// A host calling flush() concurrently with process() would be breaking that contract, and no
/// arrangement of atomics here would make the result musically sensible anyway.
///
/// `base_` is genuinely shared (the audio thread writes it from param events, the main thread reads
/// it for params.value and state save, and writes it on state load), which is why it is atomic:
/// every access is a whole 8-byte load or store, so no reader ever sees half of one value and half
/// of another.
///
/// What the atomics do NOT impose is an order between a param event and a concurrent state load, so
/// whichever lands last wins. That is benign and left alone deliberately: hosts do not automate a
/// param while restoring a preset onto it, and the fix — a proper handoff queue — would buy
/// determinism nobody can hear at the cost of machinery on the audio thread. If a real need appears
/// it is a voice-layer-scale change, not a patch here.
/// A MACRO'S READING (a double) THROUGH A ROUTE'S CURVE — untouched for Lin, so a Lin route keeps its
/// double-precision road to the bit.
[[nodiscard]] inline double throughTransfer(device::Transfer t, double x) noexcept {
	return t == device::Transfer::Lin ? x : static_cast<double>(device::shapeTransfer(t, static_cast<float>(x)));
}

/// **ONE CORE ROUTE'S CONTRIBUTION, AS `ParamSet` SUMS IT** (backlog PU: the resolve's term, named, so the editor's
/// as-modulated readout calls it rather than restating it). A macro's reading is a double through `throughTransfer`;
/// a device output's is its float cell through `shapeTransfer`. `span` is the parameter's `max − min`.
[[nodiscard]] inline double coreRouteTerm(float amount, device::Transfer t, double reading, bool macro,
                                          double span) noexcept {
	return macro ? static_cast<double>(amount) * throughTransfer(t, reading) * span
	             : static_cast<double>(amount) * static_cast<double>(device::shapeTransfer(t, static_cast<float>(reading)))
	                   * span;
}

/// **WHERE A CORE PARAMETER LANDS FROM `base + host modulation + routes`** — the range stops it unless it says its
/// offsets may leave (`ParamSet::resolved`'s last line, shared with the readout).
[[nodiscard]] inline double landCore(const ParamDesc& desc, double sum) noexcept {
	return desc.offsetsMayLeaveRange ? sum : (sum < desc.min ? desc.min : (sum > desc.max ? desc.max : sum));
}

class ParamSet {
public:
	ParamSet();

	/// Base values to their declared defaults, modulation offsets to zero. Construction-time only —
	/// it writes both halves, so calling it while audio is running would race `mod_`.
	void reset();

	/// The current base value. An unknown id reads 0.0; callers that need to tell "unknown" from a
	/// param whose value is genuinely zero should ask findParam() first.
	double base(ParamId id) const { return base(static_cast<clap_id>(id)); }
	double base(clap_id id) const;

	/// Set a base value, clamped to the declared range and rounded if the param is stepped. Ignores
	/// unknown ids, and ignores values that are not finite.
	void setBase(clap_id id, double value);

	/// Replace this param's modulation offset. CLAP mod events carry an absolute offset from the
	/// base, not an increment, so each event overwrites rather than accumulates.
	///
	/// Ignores unknown ids, non-finite offsets, and params not declared modulatable. The offset is
	/// NOT clamped to the param's range — it is a distance rather than a setting, so a -0.5 on a
	/// param whose minimum is 0 is meaningful; resolved() clamps the sum, which is where a value
	/// becomes a setting.
	void setModOffset(clap_id id, double offset);

	/// All modulation offsets to zero — what a host expects after a transport stop or a reset.
	/// Audio thread only, like the rest of the modulation lane.
	void clearModulation();

	/// **TAKE THE EDITOR'S ROUTES — WHOLE, OR NOT AT ALL — AND KEEP THEM BY IDENTITY.** The road
	/// `publishEditorLanes` runs: the bridge publishes its routes as a `RouteList` (kill-lanes step 1) and
	/// this snapshots them into `staged_`. Only a snapshot that saw no publish land while it read is
	/// taken; a false keeps the list it had.
	///
	/// **NOTHING IS COMPILED HERE ANY MORE (§2.4b).** At the synth's next drain the engine builds its
	/// device nodes' inputs from `stagedRoutes()` and this object builds its own node's
	/// (`rebuildCoreInputs`): a route is an input on the node it reaches, and the only address anywhere is
	/// a pointer to the cell its source owns. Returns whether a new snapshot was taken.
	bool adoptRoutes(const backplane::RouteList& published);

	/// THE SNAPSHOT, for the synth to hand the engine at the drain. Valid until the next `adoptRoutes`.
	[[nodiscard]] backplane::RouteRun stagedRoutes() const noexcept { return { staged_.view(), stagedCount_ }; }

	/// **ROOM FOR `routes` ROUTES — MESSAGE THREAD, ALLOCATES** (backlog OJ). The staged snapshot and this node's
	/// inputs grow in place (`ChunkedPool`: nothing moves, so the audio thread and the editor may be reading them).
	/// The bridge's list calls it, through `wireRouteGrowth`, before it publishes a count that needs it.
	void reserveRoutes(size_t routes);
	/// How many routes the stores reach now.
	[[nodiscard]] size_t routeRoom() const noexcept { return std::min(staged_.capacity(), coreInputs_.capacity()); }
	/// **HOW MANY PUBLISHES WERE REFUSED FOR WANT OF ROOM** — a list bigger than the stores, which only an unwired
	/// host can produce (`wireRouteGrowth`). Not a truncation: the routes already adopted stay.
	[[nodiscard]] size_t routeRoomRefusals() const noexcept { return routeRoomRefusals_; }

	/// **THE HOST NODE'S OUTPUT CELLS** — the macros, then the weights, in the order `Source::core(i)`
	/// names them. An input from a host source points INTO this array — the engine's inputs through
	/// `setHostRoutes`, this object's own through `rebuildCoreInputs` — so it is written in place
	/// (`publishCoreSources`, `setWeightSources`) and never moves.
	[[nodiscard]] std::span<const float> coreSourceCells() const noexcept { return coreCells_; }

	/// **WHERE A DEVICE'S CELLS ARE — the synth's answer, handed in for `rebuildCoreInputs`.** The
	/// collapsed cell and, for an output that varies per voice, the first of `voices` per-voice cells.
	/// Function pointers over `void*`, the vocabulary's idiom, so a case can stand in for an engine.
	struct DeviceCells {
		const void* host = nullptr;
		const float* (*cellOf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		const float* (*perVoiceCellOf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		size_t voices = 0;
		/// The per-voice 64-sample windows of an output, or nullptr.
		const float* (*perVoiceWindowOf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		/// A block-rate output's tick window, or nullptr.
		const float* (*windowOf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		/// **ITS RIGHT EAR** (backlog KU): the right cell, the right tick window, and the word whose bit `output` says the
		/// output is a pair NOW (`RackEngine::outputPairsOf`) — each nullptr where the left one is.
		const float* (*cellROf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		const float* (*windowROf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		const uint64_t* (*pairsOf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
		/// AND A PER-VOICE OUTPUT'S RIGHT-EAR WINDOWS (backlog PA), `perVoiceWindowOf`'s twin — current while it is a pair.
		const float* (*perVoiceWindowROf)(const void* host, DeviceInstanceId id, size_t output) = nullptr;
	};

	/// **BUILD THIS NODE'S INPUTS — one range per parameter, from the staged routes into a synth
	/// parameter.** At a drain, after the engine has adopted whatever arrived, so every device cell an
	/// input needs exists. A route whose source cannot be placed (a device not running, an output not
	/// declared) is not an input; nothing is guessed. Every range is zeroed first, the inputs written,
	/// then the ranges published — a reader on another thread sees fewer inputs mid-rebuild, never a
	/// wrong one.
	void rebuildCoreInputs(const DeviceCells& cells);

	/// **PUBLISH THE HOST NODE'S OUTPUTS** — each macro's resolved value into its cell, exactly as a
	/// device node publishes after its tick. Once per block and per control tick, before anything
	/// resolves; the weights write their cells as they arrive (`setWeightSources`).
	void publishCoreSources();

	/// **HOW EACH MACRO IS READ (backlog EG)** — bit `i` set means macro `i` publishes `2v - 1` of its
	/// 0..1 parameter rather than `v`. Written by the message thread (`publishEditorLanes`, from the
	/// bridge's modes), read by `publishCoreSources` at the block; one relaxed word, no allocation.
	void setMacroBipolarMask(uint32_t mask) noexcept;
	[[nodiscard]] uint32_t macroBipolarMask() const noexcept;

	/// **WHICH MACROS ARE RELATIVE (backlog PQ)** — bit `i` set means macro `i` is an encoder whose routes EDIT their
	/// destinations' bases on the message thread (`ui::RelativeMacroGesture`), so the engine reads it as resting at 0:
	/// its cell publishes 0 and both macro roads (`pullAt`, `buildWindowPlan`) read 0. Every transfer maps 0 to 0, so
	/// its routes add nothing per block. Clear (every Absolute macro), the reads are the expressions they always were.
	/// Written beside the bipolar mask (`publishEditorLanes`); one relaxed word, no allocation.
	void setMacroRelativeMask(uint32_t mask) noexcept;
	[[nodiscard]] uint32_t macroRelativeMask() const noexcept;
	/// **A MACRO AS EVERY ROUTE FROM IT READS IT** — `resolved(macroParam(i))`, `2v - 1` of it for a Bipolar macro (the
	/// value its cell publishes), or 0 for a Relative macro.
	[[nodiscard]] double macroReading(size_t i) const;


	/// **WHICH GENERATION OF THE EDITOR'S ROUTINGS THIS ENGINE IS RUNNING** — 0 until the first
	/// publish. `publishEditorLanes` compares it against `ParamBridge::routesGeneration()` and skips
	/// the whole road when they agree, which is every block on which no hand moved a ring.
	///
	/// **THE SKIP IS THE SAVING; PUBLISHING A SPARSE THING FASTER WAS NOT.** Measured A/B in one
	/// process: the edge publish is 16.55 µs against the dense loop's 9.85 µs, because a branchy scan
	/// of 27,840 floats does not vectorise where 27,840 stores do. What the edge store bought is the
	/// RESOLVE — ten edges instead of sixty sources per destination per block — and what buys the
	/// publish is not doing it. See `ParamBridge::routesGeneration`.
	uint32_t routesGeneration() const noexcept {
		return routesGeneration_.load(std::memory_order_relaxed);
	}

	/// **HOW MANY TIMES A PUBLISH HAS ACTUALLY RUN — `setRoutes`, or an `adoptRoutes` that applied.**
	///
	/// Found by a known-bad: a case asserting that an untouched bridge does not republish was VACUOUS,
	/// because `routesGeneration()` does not move whether the publish is skipped or repeated with the
	/// same generation. Making the skip observable takes a counter; there is no arrangement of the
	/// existing readings that recovers "was this function called".
	///
	/// **IT WAS `routePublishesForTest` AND IS NOT TEST-ONLY ANY MORE, WHICH IS A FINDING RATHER THAN A
	/// RENAME (2026-09-12).** `Synth::compileRoutes` (then the reach mask, now the drain that rebuilds every
	/// node's inputs) needs to know when the ROUTES changed, and its first form was
	/// written against `routesGeneration()` — which is the EDITOR's sequence and, by this very comment's
	/// own evidence, is not a change detector: it does not move for a `setRoutes`. The reach mask
	/// therefore went stale on any route published by that road, and a device silently stopped hearing a
	/// source it was routed. Caught by the case that pinned the mask against the graph. **The lesson is
	/// the one above, read the other way: a counter that a known-bad was needed to prove observable is
	/// exactly the counter a change detector wants — so if it earns a `ForTest` name, check whether
	/// something in `src/` is making do with the wrong reading instead.**
	///
	/// A PLAIN COUNTER, not an atomic: every reader is reached only from `process()` and
	/// `paramsFlush()`, which never run at once.
	size_t routePublishes() const noexcept { return routePublishes_; }

	/// WHAT THE WEIGHT SOURCES ARE READING — the engine's own activation weights, handed back in so a
	/// route from one can reach a destination. Snapshotted once a block from the character the synth
	/// last built, beside setRoutes and on the same argument: not an event, no timestamp, and cutting
	/// the block for it would invent a precision it does not have.
	///
	/// ONE BLOCK BEHIND, AND THAT IS WHAT MAKES IT TERMINATE. A weight is a function of the puck, and
	/// the puck is Path and Pos X/Y, which are destinations — so a weight routed to Path is a loop, and
	/// the macros' own argument for termination ("a macro is never itself a destination") does not
	/// cover it. What covers it is that this is a STORED READING of a character that has already been
	/// built: resolved() loads a number rather than re-evaluating the blend, so the loop closes through
	/// a block boundary the way a feedback delay closes through its buffer. A sound whose share drives
	/// the puck is a patch that plays itself, and it does it at block rate rather than by recursing.
	///
	/// An absent sound reads 0.0: a source that is not in the blend pushes its destination nowhere.
	void setWeightSources(const SoundWeights& weights);



	/// Whether anything at all is routed. Exposed so a caller that is about to walk a device's whole
	/// control list can skip it in the patch that routes nothing, which is most patches.
	bool anyRoute() const noexcept { return anyRoute_.load(std::memory_order_relaxed); }

	/// WHETHER ANY ENVELOPE REACHES ANYTHING, which is a question about cost rather than about sound.
	///
	/// Synth::render rebuilds only the CHARACTER on a control tick, and says why in as many words:
	/// the voice settings and the pitches "have not moved, and re-deciding them here would be work at
	/// eight hundred times a second for an answer that cannot change". A modulator makes that answer
	/// able to change — Fine Tune and the filter's corner are both destinations — so the engine has to
	/// re-decide them, and this is what lets it go on NOT doing so for every patch that has no
	/// envelope routed, which is every patch written before today.
	bool anyDeviceSourceRouted() const noexcept {
		return anyDeviceSourceRoute_.load(std::memory_order_relaxed);
	}

	/// clamp(base + host modulation + what the macros add) — the reading the DSP runs on. An unknown id
	/// reads 0.0.
	///
	/// THE THIRD TERM IS SEPARATE FROM THE SECOND ON PURPOSE. `mod_` belongs to the host: a
	/// CLAP_EVENT_PARAM_MOD overwrites it, and reset() clears it. A macro's contribution shares neither
	/// rule, so sharing the array would mean each lane silently deleting the other's work — the host's
	/// next event wiping a macro, or a reset wiping the patch's own modulation.
	///
	/// A destination reads its four amounts and the macros behind them; every other parameter takes one
	/// branch on `anyRoute_` and is exactly as fast as it was. A macro is never itself a destination,
	/// which is what makes this terminate rather than recurse — and is also the reference's rule, where
	/// maySourcePatchToParam refuses a source patched to itself.
	double resolved(ParamId id) const { return resolved(static_cast<clap_id>(id)); }
	double resolved(clap_id id) const;

	/// **THE SAME READING, FOR ONE VOICE — backlog DU's migration, and the absent argument is the
	/// point.**
	///
	/// The overload above IS that default handling — not a
	/// compatibility shim but literally the current code path — and a destination migrates by passing a
	/// voice. One that never migrates never notices this exists.
	///
	/// **A NEGATIVE VOICE IS THE COLLAPSED READING**, so the two overloads are one function with a
	/// sentinel rather than two implementations that could drift.
	double resolved(ParamId id, int voice) const {
		return resolved(static_cast<clap_id>(id), voice);
	}
	double resolved(clap_id id, int voice) const;
	/// **WHAT THIS RESOLVE READS AROUND ONE ROUTE INTO A CORE PARAMETER** (backlog PU) — `pullAt`'s walk with the
	/// route from `source` set aside: the source's reading, the other inputs before and after it, the host's
	/// modulation of the parameter and the span. False for a destination that is not a parameter (a device's control,
	/// a travel axis) or a device source no input holds yet. Reads only atomics, as `resolved` does.
	bool probeRoute(backplane::Destination destination, backplane::Source source, backplane::RouteProbe& out) const;
	/// WHETHER A PER-VOICE SOURCE REACHES THIS PARAMETER — so a caller that resolves per voice (Level in the
	/// render loop) can resolve ONCE when the answer would be the same for every voice, which is every patch
	/// without a per-voice source routed here. False for a parameter that is no destination.
	[[nodiscard]] bool variesPerVoice(ParamId id) const noexcept;

	/// **WHAT REACHES AN INTERNAL DESTINATION, in its own units** — the sum of `amount x source x span`
	/// over the routes into it, and nothing else: an internal destination has no base here and no host
	/// lane, so this IS its resolve. The owner of the quantity adds it to whatever the authored value
	/// is (`Synth::updateCharacter` for a sound's travel). `voice` as `resolved(id, voice)` takes it.
	///
	/// THE ONE SEAM THE CLAP-FACING WORKSTREAM NEEDS: exposing one of these to a host later is a
	/// parameter ADDED at the reader beside this call, in the same units, not a change to this.
	[[nodiscard]] double internalOffset(size_t internalIndex, int voice = -1) const;
	/// **WHAT THE PER-VOICE SOURCES ALONE ADD TO `id` FOR `voice`** (backlog EF) — the part of the resolve
	/// that belongs to THIS voice: a per-voice envelope's tail, not the base and not a macro. The voice's
	/// liveness reads it: a voice in release is not freed while this is still above the floor.
	[[nodiscard]] double perVoiceOffset(ParamId id, int voice) const;
	/// **WHAT THE ROUTES ADD TO `id` FOR `voice`** — every route, per-voice run, block-rate cell or macro, and NOT
	/// the base or the host's own modulation. The release hold reads it: a released voice lives while what is routed into its
	/// Voice Level stands above the floor, whatever device makes it. The base is excluded because a level is not a
	/// shape — Voice Level with nothing routed is the organ gate the voice-level design states.
	[[nodiscard]] double routedOffset(ParamId id, int voice) const {
		const int index = paramIndexOf(static_cast<clap_id>(id));
		return index < 0 ? 0.0 : sourceOffsetAt(index, voice);
	}
	/// WHETHER ANY ROUTE REACHES `id` — a source of any kind, live this block.
	[[nodiscard]] bool isRouted(ParamId id) const noexcept;
	/// `resolved(id, voice)` for `n` samples starting `phase` samples into the tick, with each
	/// per-voice window input read per sample. Returns false when no window input is MOVING for `voice` —
	/// the caller's scalar resolve is then the answer, at today's cost.
	bool resolveWindow(ParamId id, int voice, int phase, int n, float* out) const;

	/// **ONE PARAMETER'S INPUTS, GATHERED ONCE PER TICK** for the per-sample resolve, so the
	/// render loop reads no atomics and walks no route table. Built at a drain or a routed tick
	/// (`buildWindowPlan`), read per chunk per voice (`resolveWindowPlanned`).
	struct WindowPlan {
		static constexpr size_t kMaxInputs = 16;
		double scalar = 0.0; ///< base + host modulation + every block-rate and macro input
		double lo = 0.0;
		double hi = 1.0;
		bool clamp = true;
		size_t voices = 0;
		size_t windows = 0; ///< per-voice window inputs
		std::array<float, kMaxInputs> windowAmount {}; ///< amount × span
		std::array<const float*, kMaxInputs> window {}; ///< `voices * kTickWindowStride` floats
		std::array<device::Transfer, kMaxInputs> windowTransfer {};
		size_t blockWindows = 0; ///< block-rate window inputs: the same run for every voice
		std::array<float, kMaxInputs> blockAmount {};
		std::array<const float*, kMaxInputs> blockWindow {};
		std::array<device::Transfer, kMaxInputs> blockTransfer {};
		size_t cells = 0; ///< per-voice inputs with no window: one value per voice
		std::array<float, kMaxInputs> cellAmount {};
		std::array<const float*, kMaxInputs> cell {};
		std::array<device::Transfer, kMaxInputs> cellTransfer {};
		/// **THE EARS' DIFFERENCE** (backlog KU, 2026-10-05) — for a `stereo` parameter, the block-rate inputs whose source
		/// is a PAIR at this build: each is ALSO among `blockWindows` above (the left ear's road is unchanged), and here
		/// with its right window beside its left, so `resolveEarDifferencePlanned` can sum amount × (R − L) per sample.
		/// Zero for every mono parameter and every patch with no stereo route — which is what "stereo-live" means.
		size_t pairs = 0;
		std::array<float, kMaxInputs> pairAmount {};
		std::array<const float*, kMaxInputs> pairLeft {};
		std::array<const float*, kMaxInputs> pairRight {};
		std::array<device::Transfer, kMaxInputs> pairTransfer {};
		/// **AND THE PER-VOICE ONES** (backlog PA): per-voice window inputs whose source is a pair at this build (an LFO
		/// with Stereo Phase), each ALSO among `windows` above, with its right runs beside its left — read at the voice.
		size_t voicePairs = 0;
		std::array<float, kMaxInputs> voicePairAmount {};
		std::array<const float*, kMaxInputs> voicePairLeft {};
		std::array<const float*, kMaxInputs> voicePairRight {};
		std::array<device::Transfer, kMaxInputs> voicePairTransfer {};
		/// Whether any pair reaches this parameter — block-rate or per voice.
		[[nodiscard]] bool stereoLive() const noexcept { return pairs + voicePairs > 0; }
	};
	void buildWindowPlan(ParamId id, WindowPlan& plan) const;
	[[nodiscard]] static bool resolveWindowPlanned(const WindowPlan& plan, int voice, int phase, int n, double extra,
	                                               float* out) noexcept;
	/// **mR − mL PER SAMPLE** — what the stereo routes add to the right ear beyond what they add to the left, for `n`
	/// samples from `phase` in the tick. False (and nothing written) when the plan is not stereo-live. UNCLAMPED: the
	/// difference of two sums, which is the right ear exactly for a destination whose offsets may leave its range
	/// (Phase wraps); a clamping stereo destination would need its own right-ear clamp, and there is none yet.
	/// `voice` picks the per-voice pairs' runs (backlog PA); -1 asks for the block-rate pairs alone, and is refused when a
	/// per-voice pair exists.
	[[nodiscard]] static bool resolveEarDifferencePlanned(const WindowPlan& plan, int voice, int phase, int n,
	                                                      float* out) noexcept;
	/// **THE RIGHT EAR'S READING OF A `stereo` PARAMETER** — `resolved(id)` plus every pair input's (R − L), from the
	/// cells; `resolved(id)` itself when nothing stereo reaches it. For the editor's split ring (`ParamBridge::publish`).
	[[nodiscard]] double resolvedRight(ParamId id) const;
	/// Whether a route from a source that is a pair NOW reaches this `stereo` parameter.
	[[nodiscard]] bool stereoLive(ParamId id) const;

	/// Convenience readings of the tapered params, off `resolved()`.
	///
	/// These exist so no consumer ever writes the taper out again. A voice layer that spelled
	/// `exp2(resolved(Attack))` inline would be a second copy of the curve, free to drift from the
	/// one the host's display uses.
	double glideTimeSeconds() const { return glideSeconds(resolved(ParamId::GlideTime)); }
	double masterGain() const { return decibelsToGain(resolved(ParamId::MasterGain)); }
	int bendRangeSemitones() const { return bendRangeSemitonesAt(resolved(ParamId::BendRange)); }

	/// DOES THE FILTER'S CORNER FOLLOW THE NOTE. Tonal; on the bus, the one note the host picked —
	/// see below.
	// filterKeytrack() and filterPoly() lived here. Both are the DEVICE's state now: they travel in
	// its chain row and the engine reads them through fxFilterAt, which is what lets a second device
	// in the lane answer the same questions without this table learning what it is.

	/// ONE FILTER PER VOICE, OR ONE ON THE SUMMED BUS — architectural, and the other half of a
	/// control that used to do both jobs.
	///
	/// KEYTRACK NO LONGER IMPLIES POLY (2026-10-04). It did — "a corner cannot follow the note on a bus
	/// carrying six of them" — and the editor greyed keytrack while poly was off. But the HOST has one
	/// note to give the bus (its collapsed note, or an upstream Tracker's), so on the bus the corner
	/// follows that note, and keytrack is live whenever the device is: `OpsFilter::refresh`.


	/// Readings of the stepped params as the enums they mean.
	///
	/// Same rule as the tapers, and it matters more here: a consumer that wrote
	/// `static_cast<VoiceMode>(std::lround(resolved(VoiceMode)))` would be re-deriving the mapping
	/// from index to meaning, and a value outside the enum — which a malformed patch can produce —
	/// would land as an enum with no case in any switch. These clamp, so what comes out is always a
	/// declared entry.
	OscType oscType() const;
	VoiceMode voiceMode() const;
	NotePriority notePriority() const;

	/// The voice-count limit, 1..16. An int because it is a count of voices, not a curve.
	int voiceCount() const;

	/// How many oscillators each note stacks, 1..kMaxStack (backlog KE).
	int stackCount() const;
	/// THE DRUM ENGINE IN KIT MODE: every sound a pad, a note playing the pads mapped to it (drum proposal §5).
	bool drumKit() const;

	/// Voice Count as the guard allows it under the current stack — what the voice manager is handed.
	int voiceLimit() const { return stackVoiceLimit(voiceCount(), stackCount()); }

	/// The Stack Index's two mode choices, by the table's stepped rule (step 3).
	StackShape stackShape() const;
	StackMap stackMap() const;

	/// **THE SUMMED AMOUNT OF EVERY ROUTE FROM THE STACK INDEX INTO `id`** (backlog KE step 3; doc §3.3). Routes are
	/// linear, so part k's offset is this × its index × the destination's span, on top of a generic resolve that
	/// saw the source at its cell's 0. The routes are found among the destination's inputs by the cell they read,
	/// which is the stack's own and nobody else's. Zero when nothing is routed.
	[[nodiscard]] double stackIndexAmount(ParamId id) const;

	bool valueToText(clap_id id, double value, char* out, uint32_t capacity) const;
	bool textToValue(clap_id id, const char* text, double* out) const;

	/// Stream base values as id→double pairs behind a version byte. Modulation offsets are host
	/// state, not patch state, and are not written.
	bool save(const clap_ostream* stream) const;

	/// Read a patch, committing nothing until the stream ends cleanly.
	///
	/// Ids this build does not know are skipped, so a patch from a later version stays loadable; ids
	/// the stream omits keep their default, so a patch from an earlier version gains new params at
	/// theirs. Returns false on a stream it cannot trust — and on that path the live values are
	/// untouched, because a corrupt project file must not also cost the user the patch they had.
	///
	/// Writes `base_` and nothing else, which is what makes it safe to call from the main thread
	/// while audio runs. Live modulation is left in place: it belongs to the host, which re-sends it
	/// every block, so a preset load has no business clearing it.
	bool load(const clap_istream* stream);

	/// **`load` IN TWO HALVES**, for a caller whose patch has more in it than parameters (the plugin's
	/// state road): `read` parses into `Staged` and touches nothing; `commit` writes it. Between them the
	/// caller parses the rest of its stream, so a refusal anywhere in it leaves the parameters untouched too.
	struct Staged {
		std::array<double, kParamCount> base {};
	};
	/// Why a `read` refused — the event log names it.
	enum class ReadFailure : uint8_t { None, NoStream, Empty, Version, ShortRead };
	/// `version` (if given) receives the version byte the stream carried, or stays untouched if it had none.
	bool read(const clap_istream* stream, Staged* staged, ReadFailure* why = nullptr, uint8_t* version = nullptr) const;
	void commit(const Staged& staged);

private:
	/// **EVERY SOURCE THAT REACHES `destination`, AND BY HOW MUCH — ask this, not the array.**
	///
	/// The read both resolves want. `sourceOffsetAt` and `rackOffsetAt` are Feathers' resolve, and
	/// each of them asks one question — *what reaches this destination* — which the store answers
	/// today by being scanned for all sixty sources and `continue`-ing on the fifty-nine zeros.
	///
	/// **IT IS THE ARRAY TODAY AND THAT IS THE POINT.** Introducing it changes no behaviour — the
	/// walk order, the float-to-double widening and the zero skip are the ones the two loops already
	/// had — and it is what lets the store move under a green suite instead of under both resolves
	/// rewritten in the same commit. It is the same seam `backplane::Graph::amountAt` is on the other
	/// host (`ca27489`), aimed at Feathers' shape: the rack reads route amounts a POINT at a time and
	/// wanted an accessor; Feathers reads them a DESTINATION at a time and wants a walk.

	/// What the sources add to one destination, in the destination's own units. Zero for anything that
	/// is not a destination, and zero for every destination while nothing is routed.
	double sourceOffsetAt(int index, int voice) const;
	/// THE PULL ITSELF — shared by the parameter road and the internal one so they cannot drift.
	double pullAt(size_t destination, double span, int voice) const;


	std::array<std::atomic<double>, kParamCount> base_;
	std::array<double, kParamCount> mod_;
	/// **THIS NODE'S INPUTS, IN ONE ARRAY THE PER-PARAMETER RANGES INDEX INTO** (kill-lanes §2.4b) — the
	/// compiled plan for the synth's own parameters, which are the host node's controls. `value` is the
	/// source's collapsed cell; `perVoice` the first of its per-voice cells, or null ("voices within an
	/// edge"). ATOMIC PER FIELD: the audio thread rebuilds at the drain while the editor's thread may be
	/// resolving a value to draw, and a half-rewritten range must read as FEWER inputs, never as a
	/// pointer to nowhere — every cell pointed at is stable storage (`coreCells_`, a slot's `outputs`).
	struct CoreInput {
		std::atomic<const float*> value { nullptr };
		std::atomic<const float*> perVoice { nullptr };
		std::atomic<const float*> perVoiceWindow { nullptr };
		std::atomic<const float*> window { nullptr }; // a block-rate device source's tick window
		/// **THE SOURCE'S RIGHT EAR** (backlog KU): its right cell and right tick window, and the byte whose bit
		/// `pairBit` says it is a pair now — null for a host source (never a pair). A per-voice device source has them too
		/// since backlog PA (the LFO's Stereo Phase): its collapsed right cell, and `perVoiceWindowR` its per-voice runs.
		std::atomic<const float*> valueR { nullptr };
		std::atomic<const float*> windowR { nullptr };
		std::atomic<const float*> perVoiceWindowR { nullptr };
		std::atomic<const uint64_t*> pairs { nullptr };
		std::atomic<uint8_t> pairBit { 0 };
		std::atomic<float> amount { 0.0f };
		/// THE ROUTE'S CURVE (`device/transfer.h`), a `device::Transfer` — Lin keeps every road's old expression.
		/// The whole code — kind and coefficient step — so 16 bits.
		std::atomic<uint16_t> transfer { 0 };
		/// **A MACRO IS RESOLVED LIVE, NOT READ FROM ITS CELL** — `macroParam(macro)` when `macro >= 0`.
		/// The host node's macro is a control and an output at once: its reading is `resolved()`, which
		/// includes whatever the host is modulating IT by, and an editor's thread asking a parameter's
		/// value between two blocks must see the macro as it is now, not as the last block published it
		/// (`publishCoreSources` is the block's road; the cells it fills are for the ENGINE's inputs).
		std::atomic<int> macro { -1 };
		/// THE ROUTE'S SOURCE, packed (device << 32 | index) — what `probeRoute` finds the watched input by.
		std::atomic<uint64_t> source { ~uint64_t { 0 } };
	};
	struct CoreRange {
		std::atomic<uint32_t> first { 0 };
		std::atomic<uint32_t> count { 0 };
		/// WHETHER ANY INPUT INTO THIS DESTINATION VARIES PER VOICE — what lets a per-voice destination resolve
		/// ONCE when nothing per-voice reaches it (backlog DU's "default trivial handling", kept literal).
		std::atomic<bool> varies { false };
	};
	/// GROWN IN PLACE (backlog OJ) — the editor reads these while the drain rewrites them, so they never move.
	backplane::ChunkedPool<CoreInput, 16, 3, 2> coreInputs_ { kInitialRouteRoom };
	/// OVER BOTH REGIONS of the core index space — parameters, then the internal destinations.
	std::array<CoreRange, kCoreDestinationCount> coreRanges_ {};
	/// HOW MANY PER-VOICE CELLS A `perVoice` RUN HOLDS — the engine's voice count, as of the last rebuild.
	size_t perVoiceCells_ = 0;
	/// **THE HOST NODE'S OUTPUTS** — see `coreSourceCells()`. Written in place with `atomic_ref`, read
	/// through the pointers every input from a host source holds. Never moves.
	std::array<float, kCoreSourceCount> coreCells_ {};
	/// Which macros publish bipolar — see `setMacroBipolarMask`.
	std::atomic<uint32_t> macroBipolar_ { 0 };
	/// Which macros are relative — see `setMacroRelativeMask`.
	std::atomic<uint32_t> macroRelative_ { 0 };
	/// See `routeRoomRefusals`.
	size_t routeRoomRefusals_ = 0;
	/// WHERE `adoptRoutes` LANDS A SNAPSHOT BEFORE IT BELIEVES IT, and the list it compiles from until the next whole
	/// one. A `RouteStage`: grown on the message thread (`reserveRoutes`), never on the audio thread, never moved.
	backplane::RouteStage staged_ { kInitialRouteRoom };
	size_t stagedCount_ = 0;
	/// The editor generation this engine last applied. See routesGeneration().
	std::atomic<uint32_t> routesGeneration_ { 0 };
	/// See routePublishes().
	size_t routePublishes_ = 0;
	/// Whether any amount is non-zero, so the common patch — which routes nothing — costs one relaxed
	/// load and a branch per parameter read rather than a walk of the table.
	std::atomic<bool> anyRoute_ { false };
	/// The same question asked of the envelope rows alone — see anyEnvelopeRouted().
	std::atomic<bool> anyDeviceSourceRoute_ { false };
};

} // namespace feathers
