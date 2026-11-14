/*
 * Copyright © 2024-2025 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/owltt/band_compressor.h"
#include "fx/owltt/crossover.h"
#include "device/zone_param.h"

#include "device/device.h"
#include "device/registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

// OWLTT (the reference's OTT) — 3-band upward/downward compressor with tonal shaping via CHARACTER and VIBE zones.
//
// Ported from the firmware's multiband.h. Ten crossover types (selected by Mode), full 8-zone
// character and vibe computation, linked parameter shift, per-band M/S width, soft clipping, and
// gain smoothing.
//
// Mode selects the crossover type AND serves as the enable switch (mode 0 = off). This matches the
// firmware's combined enable + crossoverType_ semantics. Per-band threshold/ratio are absolute
// values (0..1); the linked globals shift all bands by delta, matching setAllThresholds/setAllRatios/
// setLinkedBandwidth.

namespace feathers::fx::owltt {

/// OWLTT's 25 controls, in hardware menu order.
enum class Param : uint8_t {
	// Global
	Mode = 0,            // Stepped 1-10: 1=AP 6dB, 2=Quirky, 3=Twisted, 4=Weird, 5=LR2 Fast,
	                     //   6=LR2 Full, 7=LR4 Fast, 8=LR4 Full, 9=Inverted, 10=Twist3
	Enabled = 1,         // Bypass switch
	Threshold = 2,       // Linked global threshold
	Ratio = 3,           // Linked global ratio
	Bandwidth = 4,       // Linked global bandwidth
	UpDownSkew = 5,      // Up/down balance (-1=upward only, +1=downward only)
	Attack = 6,          // Linked global attack
	Release = 7,         // Linked global release
	Character = 8,       // 8-zone tonal character
	Vibe = 9,            // 8-zone phase/vibe

	// Low band (absolute values, shifted by linked globals)
	LowThreshold = 10,
	LowRatio = 11,
	LowBandwidth = 12,
	LowLevel = 13,

	// Mid band (absolute values, shifted by linked globals)
	MidThreshold = 14,
	MidRatio = 15,
	MidBandwidth = 16,
	MidLevel = 17,

	// High band (absolute values, shifted by linked globals)
	HighThreshold = 18,
	HighRatio = 19,
	HighBandwidth = 20,
	HighLevel = 21,

	// Output
	LowCrossover = 22,   // Low-mid split frequency
	HighCrossover = 23,  // Mid-high split frequency
	OutputGain = 24,     // Makeup gain
	Blend = 25,          // Wet/dry mix

	/// **TWO PHASE PARAMS STOOD HERE, AT 26 AND 27, AND ARE GONE, AND WITH THEM THE DUAL-GANG
	/// VALUE BOXES.** They were added in 2026-08-28 to reach `feelPhaseOffset_` and
	/// `vibePhaseOffset_`, which `owltt.cpp` read in six places and assigned nowhere.
	///
	/// **THE OFFSETS ARE STILL REACHED; THE PARAMS ARE NOT WHAT REACHES THEM.** `owltt.cpp` derives
	/// each from the whole part of its own coordinate — `std::floor(target_.character)` — which it can
	/// do because the coordinates stopped being bounded to 0..1. So the code they were added to
	/// activate is still live, and what was deleted is a second road to a position the first road
	/// already reaches. See `kParamDescs` below for the arithmetic and for what the deletion cost.
	///
	/// **THE SLOTS ARE NOT RESERVED.** Every later id would shift if anything were re-inserted here,
	/// and that is a hazard this device has already paid for once — see the row-walk case in
	/// `tests/test_fx_owltt.cpp`, and the ordering guard beside it. Nothing held an id (patch
	/// compatibility was not yet a constraint), so the right move was to close the gap.
	/// THE SOFT CLIPPER'S SWITCH (backlog EL: do the owltt ui work). The firmware
	/// toggles it from the Mode encoder's click and serialises it; this device always clipped. APPENDED,
	/// so nothing above moves; the row grows by one and the state version says so.
	SoftClip = 26,

	/// **CHARACTER'S AND VIBE'S MODES (backlog IW).** The dropdown selects a mode, not a phase region: the modes,
	/// then Free, and since the mode implementations are unbounded, the in-mode phase stays unbounded too. Each
	/// coordinate's eight zones are eight real behaviours (every one has a case in the DSP's switch), so each is a MODE:
	/// eight, then Free. See `zoneModeCoordinate` for how a mode and the unbounded value make the coordinate the DSP reads.
	CharacterMode = 27,
	VibeMode = 28,
	/// **BACKLOG LQ : "compressors incl owltt and limiter should get dual mono with stereo link
	/// control, as well as m/s switches."** Each band now has a detector and a gain PER CHANNEL (L and R, or M and
	/// S); Link blends each channel's detector level toward the band's shared one, in the detector's own log
	/// units (nepers — so in dB). 100% is the linked device this was before, bit for bit; 0% compresses the two
	/// channels of every band independently. Appended, so nothing above moves.
	StereoLink = 29,
	/// L/R OR M/S. In M/S each band is encoded to Mid and Side AFTER its width (the Character zone's per-band
	/// M/S width), detected and gained there, and decoded back — so Side's detector hears the side the band is
	/// actually sending on. A two-state chip; not modulatable.
	StereoMode = 30,
	/// **BACKLOG LR.** The Character zone sets each band's M/S width,
	/// below 1 in the bass of every non-weird zone and in all three bands at the patch default (0.7, 0.85, 0.9 —
	/// MEASURED in "LR: measured — ..."), and no control said so. Width SCALES the zone's three values the way the
	/// linked globals shift the per-band thresholds; each band's own Width scales its band on top. 100% is "as the
	/// zone sets it", to the bit; 0 sends no side (mono, wherever the two channels are gained alike: Link 100%, or M/S);
	/// 200% opens a band up past the zone. Effective width per band =
	/// zone x Width x band Width, inside the device's existing +-2 clamp. Appended, so nothing above moves.
	Width = 31,
	LowWidth = 32,
	MidWidth = 33,
	HighWidth = 34,
	/// **BACKLOG NB: DRIVE.** A gain in dB in front of the dynamics — COMP's
	/// Drive (MI): the crossovers, the detectors and the audio they gain all hear the driven input, while the dry the
	/// Blend mixes back stays the undriven one. 0 dB is `x * 1.0f`, the device before NB to the bit. Appended.
	Drive = 35,
	/// **BACKLOG ND: OPTIONAL LOOKAHEAD,** to outrun the spikes an aggressive setting resting at high upward
	/// gain produces when a signal suddenly gets hot. ONE delay, shared by every
	/// band and the dry, on the AUDIO the gains are applied to — the detectors hear the input undelayed, so the gain is
	/// already falling when the onset reaches it. A Choice of times (Off, then 1..10 ms) rather than a knob: the time is the
	/// device's latency, and every value a drag passes through would restart the host. Off is the device before ND, to
	/// the bit. Not modulatable (a modulated latency is assumption L1's refusal). Appended. AND THE RESPONSE FITS THE WINDOW
	/// (option 3, 2026-10-18): the detector's rise, the attack and a falling gain's smoother are capped to settle
	/// inside it (`BandCompressor::setLookaheadWindow`), as a lookahead limiter's are. None of that moves the latency.
	Lookahead = 36,
	/// **KEY TRACK** (the Phaser's form): the played note places the crossovers; Low and High Crossover
	/// become semitone OFFSETS from it (`trackedCrossoverHz`). Off is the device before it, to the bit. Appended.
	KeyTrack = 37,
	/// **PER-BAND UP/DOWN RATIO TRIMS, ATTACK/RELEASE OFFSETS AND INPUT LEVELS (the per-band rebuild).** Appended, so
	/// nothing above moves. Every one has a neutral default at which the device is the device before it, to the bit:
	/// the trims and offsets are centred on 0.5 (`effectiveBandOf` states the laws), the input level on 0 dB.
	LowUpRatio = 38,
	LowDownRatio = 39,
	MidUpRatio = 40,
	MidDownRatio = 41,
	HighUpRatio = 42,
	HighDownRatio = 43,
	LowAttack = 44,
	LowRelease = 45,
	MidAttack = 46,
	MidRelease = 47,
	HighAttack = 48,
	HighRelease = 49,
	LowInput = 50,
	MidInput = 51,
	HighInput = 52,
	kCount
};

/// Per-band settings: absolute values (linked globals apply a delta shift).
struct BandParams {
	float threshold = 0.5f; // Absolute threshold (0..1)
	float ratio = 0.5f;     // Absolute ratio (0..1); 0.5 is neutral, like the three beside it
	float bandwidth = 0.5f; // Per-band bandwidth
	float level = 0.5f;     // Per-band output level (0..1, 0.5 = unity)
	/// BACKLOG LR: scales this band's zone width on top of the global Width. 1 is "as the zone sets it".
	float width = 1.0f;
	/// THIS BAND'S UPWARD AND DOWNWARD RATIO TRIMS (0..1, 0.5 neutral): added to the band's effective ratio, in the
	/// ratio's own units, for that direction only. See `effectiveBandOf`.
	float upRatio = 0.5f;
	float downRatio = 0.5f;
	/// THIS BAND'S ATTACK AND RELEASE OFFSETS (0..1, 0.5 neutral) from the global Attack and Release.
	float attack = 0.5f;
	float release = 0.5f;
	/// THIS BAND'S INPUT LEVEL, dB, applied to the band after the split and before its detector and gain.
	float inputDb = 0.0f;
};

/// The full OWLTT state: globals + per-band absolutes.
struct Settings {
	float mode = 6.0f;           // 1-10 = crossover type (6 = LR2 Full default)
	bool enabled = true;         // Bypass switch
	float threshold = 0.5f;      // Linked global threshold
	// THE STRUCT DEFAULT IS NOT THE PARAMETER DEFAULT, HERE AND ONLY HERE, and it is deliberate: a
	// default-constructed `Settings` is a NO-OP device (band 0.5 + shift -0.5 = 0), which is what the
	// engine cases lean on and what `test_fx_owltt.cpp:609` records having been caught by. The
	// declared default a patch gets is 0.5, in `kParamDescs`.
	float ratio = 0.0f;          // Linked global ratio
	float bandwidth = 0.5f;      // Linked global bandwidth
	float upDownSkew = 0.5f;     // 0..1, 0.5 = balanced; owltt.cpp maps it to -1..1 for the DSP
	float attack = 0.25f;        // Linked global attack
	float release = 0.25f;       // Linked global release
	float character = 0.0f;      // 0..1
	float vibe = 0.0f;           // 0..1

	BandParams low {};
	BandParams mid {};
	BandParams high {};

	float lowCrossover = 200.0f;  // Hz
	float highCrossover = 2000.0f; // Hz
	float outputGain = 0.5f;       // 0..1, 0.5 = unity
	float blend = 1.0f;            // 0=dry, 1=wet
	/// THE CLIP CHOICE, as two flags so the many callers that set `softClip` keep meaning what they meant. `softClip`
	/// off is Off. On with `clipBandsOnly` false is Master, which is how the device always ran (each band's sum
	/// clipped, then the output); `clipBandsOnly` true is Bands: each band clipped before the sum, the sum not.
	bool softClip = true;
	bool clipBandsOnly = false;
	/// CHARACTER'S AND VIBE'S MODES (backlog IW): a zone index 0..7, or `kZoneModeFree`. Free is the rolling coordinate.
	uint32_t characterMode = 8;
	uint32_t vibeMode = 8;
	/// BACKLOG LQ — see `Param::StereoLink`. 1 is the linked device this was before LQ.
	float stereoLink = 1.0f;
	bool midSide = false;
	/// BACKLOG LR — see `Param::Width`. Scales all three zone widths; 1 is "as the zone sets it".
	float width = 1.0f;
	/// BACKLOG NB — see `Param::Drive`. dB in front of the dynamics; 0 is the undriven device.
	float driveDb = 0.0f;
	/// BACKLOG ND — see `Param::Lookahead`. An index into `kLookaheadNames`; 0 is Off.
	uint32_t lookahead = 0;
	bool keyTrack = false;

	/// The band by index, 0 low, 1 mid, 2 high.
	[[nodiscard]] const BandParams& band(size_t b) const noexcept { return b == 0 ? low : (b == 1 ? mid : high); }
};

/// **THE LOOKAHEAD'S CHOICES (backlog ND)**: Off, then whole milliseconds up to the chosen 10. `kLookaheadMs` is what each
/// label means; `lookaheadSamplesFor` turns it into the delay, which is also the latency the device reports.
inline constexpr const char* kLookaheadNames[] = { "Off", "1 ms", "2 ms", "3 ms", "4 ms", "5 ms", "6 ms", "7 ms", "8 ms", "9 ms", "10 ms" };
inline constexpr double kLookaheadMs[] = { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0 };
static_assert(std::size(kLookaheadNames) == std::size(kLookaheadMs));
inline constexpr uint32_t kLookaheadChoices = static_cast<uint32_t>(std::size(kLookaheadMs));
/// The longest, which sizes the delay line at `prepare`.
inline constexpr double kMaxLookaheadMs = kLookaheadMs[std::size(kLookaheadMs) - 1];

/// THE DELAY IN SAMPLES FOR A CHOICE AT A RATE — the one statement both the render and `latencySamples` read, so the
/// delay applied and the latency reported cannot disagree. 0 for Off and for anything out of range.
[[nodiscard]] inline uint32_t lookaheadSamplesFor(uint32_t choice, double sampleRate) noexcept {
	if (choice >= kLookaheadChoices || !(sampleRate > 0.0)) {
		return 0;
	}
	return static_cast<uint32_t>(std::lround(kLookaheadMs[choice] * 0.001 * sampleRate));
}

/// **BLEND 0 IS DRY IN EVERY MODE (backlog OY).** The LR modes hand the Blend a dry in the
/// crossover's own phase (`alignDry`, backlog ER) so a partial mix does not cancel at the corners — but at Blend 0
/// that made the output the input through an allpass whose phase followed the cutoffs. So the dry fades from the
/// raw input at Blend 0 to the aligned one at this Blend, linearly, and is aligned above it. Kept short by design: the
/// corner cancellation ER removed returns only below it.
inline constexpr float kDryAlignFadeBlend = 0.1f;

/// THE ALIGNED DRY'S SHARE at a Blend: 0 at Blend 0 (the raw input, exactly), 1 from `kDryAlignFadeBlend` up.
[[nodiscard]] inline float alignedDryShare(float blend) noexcept {
	return std::clamp(blend / kDryAlignFadeBlend, 0.0f, 1.0f);
}

/// THE DRIVE'S RANGE (backlog NB). +24 is COMP's top, so the two dynamics devices push equally hard. Below 0 because
/// OWLTT is an UPWARD compressor too, which COMP is not: a cut drops a hot source under the downward thresholds and
/// into the upward road's, which is a sound of its own rather than a quieter one. -12 is a quarter of the 48 dB the
/// threshold sweep spans, enough to move a mix-level source off the downward knee without leaving the picture.
inline constexpr float kDriveMinDb = -12.0f;
inline constexpr float kDriveMaxDb = 24.0f;

/// **THE OUTPUT KNOB AS A GAIN** (0..1 to 0..6.31 linear): the lower half is a straight 0..unity, the upper half takes
/// it up to +16 dB. Moved here from `owltt.cpp`, unchanged, so the window's Output readout reads the number the DSP uses.
[[nodiscard]] inline float outputGainFromKnob(float knob) {
	if (knob <= 0.5f) {
		return knob * 2.0f;
	}
	return 1.0f + (knob - 0.5f) * 2.0f * 5.31f;
}

/// EACH BAND'S INPUT LEVEL RANGE, dB (the per-band rebuild): symmetric, 0 dB the device before it.
inline constexpr float kBandInputMinDb = -18.0f;
inline constexpr float kBandInputMaxDb = 18.0f;

/// dB TO LINEAR, as `exp` rather than `pow(10, .)` — COMP's own formula, so a case can restate it.
[[nodiscard]] inline float driveGainOf(float db) {
	constexpr float kLn10Over20 = 0.11512925464970229f;
	return std::exp(db * kLn10Over20);
}

/// Row: the serialized form of Settings (what a patch stores).
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// Link a Row to a Settings.
Settings stateOf(const Row& row);
Row rowOf(const Settings& settings);

/// Traits: what OWLTT says about itself.
/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "dynamics", "fx" };

inline constexpr device::Traits kTraits {
	// THE TAG AND THE NAME AGREE AGAIN. They did not for a day: `device::Traits::tag` was a hard
	// FOUR-byte field, so "OWLTT" could not be the tag and the record carried "OWLT" instead. It was
	// widened it to `device::kTagBytes` on 2026-09-14 — "a longer identifier is probably fine" — and
	// the truncation went with it. Nothing ever rendered the tag; the browser, the card head and
	// every zone list take `name`.
	.tag = "OWLTT",
	.name = "OWLTT",
	// NEITHER FLAG IS READ BY ANYTHING — backlog 16. They are declared because every device declares
	// them and `-Wmissing-field-initializers` keeps that honest, not because they select anything:
	// the fx lane picks a card STRUCTURALLY, by what it can build, so OWLTT gets the generic knob list
	// from having no card of its own rather than from a flag saying so.
	.hasCard = true,
	.hasExpanded = true,

	// WHICH CONTROL SWITCHES OWLTT IN AND OUT, so the shared head draws its enable square from the
	// declaration rather than from a lane branch — the shared head's rule.
	//
	// ADDED AT MERGE. OWLTT declared the `Enabled` PARAM and left this at its -1 default, so the
	// registry-wide invariant "every audio device names its enable" went red the moment the two
	// branches met. Neither side was wrong on its own: the invariant landed on one branch while the
	// device was being written on another, and the merge is the first place they could disagree.
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Blend),
	.mixInDevice = true,
};

inline constexpr const char* kModeNames[] = {"AP 6dB",   "Quirky",   "Twisted", "Weird",    "LR2 Fast",
                                             "LR2 Full", "LR4 Fast", "LR4 Full", "Inverted", "Twist3"};

inline constexpr const char* kOffOnNames[] = {"Off", "On"};
/// THE CLIP CHOICE (`soft_clip`). It was Off|On; On is Master, the same index, so a stored 1 means what it meant, and a
/// patch that stored the label "On" loads as Master through `kClipAliases`.
inline constexpr const char* kClipNames[] = {"Off", "Master", "Bands"};
inline constexpr device::ChoiceAlias kClipAliases[] = { { "On", "Master" } };
/// BACKLOG LQ: what a band's two channels are.
inline constexpr const char* kStereoModeNames[] = {"L/R", "M/S"};

/// Param descriptors: the controls OWLTT publishes.
/// THE VALUES THE DEVICE ACTUALLY USES — the global knobs folded into the per-band ones.
///
/// the global threshold doesnt impact the graph bars... moving the threshold knob should
/// change the shown threshold in the graph because the thresholds in the graph are supposed to match
/// the actual thresholds the device uses st you might be able to set them by eye.
///
/// THIS IS THE HONESTY RULE APPLIED TO A PICTURE. A bar drawn at the STORED per-band value
/// is drawn at a number the compressor does not use: the device shifts every band by the global's
/// distance from neutral, so turning the global moved the sound and not the plot. A graph you cannot
/// set by eye is a graph that is lying about the thing it is a graph OF.
///
/// SHARED WITH `owltt.cpp` RATHER THAN RESTATED IN THE UI, and this is the file it has to live in.
/// The engine and the drawing must agree about what a threshold IS, and this codebase has spent a
/// week on pairs of numbers that agreed on the day they were written. One function, two callers,
/// nothing to drift.
///
/// ALL THREE SHIFTS ARE CENTRED — 0.5 is neutral, so a global displaces its band in either
/// direction and displaces it by NOTHING where it comes up.
///
/// RATIO USED TO BE THE ODD ONE OUT, and it cost the per-band ratios half their travel. The shift
/// was the global ITSELF rather than its distance from neutral, on the argument that "how much
/// compression" has a floor and no meaningful negative — which is true of the effective RATIO and
/// is not true of the OFFSET. The floor is the clamp below; the offset is a displacement, and a
/// displacement that can only add is a displacement that eats one end of the range it is added to.
///
/// WHAT IT COST, MEASURED. The global's declared default is 0.5, so a device out of the box had
/// every per-band ratio pinned to `clamp(band + 0.5)` = 0.5..1.0. The graph draws a strip over the
/// whole of 0..1 with its handle ON the effective value, so the ring stopped following the pointer
/// at mid-height and a drag DOWNWARD wrote nothing at all — per band ratio bars in popout
/// graph not responding. The bottom half of the strip, which is the direction a hand goes to take
/// compression OFF, was dead on the card and on the panel alike.
///
/// AND THE MOVE IS FREE, WHICH IS WHY IT IS THIS FIX AND NOT A UI ONE. The per-band ratios default
/// to 0.5 now instead of 0, so `clamp(0.5 + (g - 0.5))` = `g` — exactly what `clamp(0 + g)` gave
/// before, for every g. The global macro's whole sweep means what it meant, the device still
/// compresses out of the box through it rather than through twelve knobs, and nothing about the
/// sound at any default changed. Pinned by "a global at its declared default displaces nothing".
struct EffectiveBand {
	float threshold;
	float ratio;
	float bandwidth;
	float level;
	/// The two directions' ratios (the band's own `ratio` is what both started from) and the band's attack and release
	/// knobs, as `setRatioDown`, `setRatioUp`, `setAttack` and `setRelease` take them.
	float ratioDown;
	float ratioUp;
	float attack;
	float release;
};

/// How far a global displaces its per-band partner. Zero when the global sits at its own neutral.
inline float thresholdShiftOf(const Settings& s) {
	return s.threshold - 0.5f;
}
inline float ratioShiftOf(const Settings& s) {
	return s.ratio - 0.5f;
}
inline float bandwidthShiftOf(const Settings& s) {
	return s.bandwidth - 0.5f;
}

inline EffectiveBand effectiveBandOf(const BandParams& band, const Settings& s) {
	EffectiveBand out;
	out.threshold = std::clamp(band.threshold + thresholdShiftOf(s), 0.0f, 1.0f);
	out.ratio = std::clamp(band.ratio + ratioShiftOf(s), 0.0f, 1.0f);
	out.bandwidth = std::clamp(band.bandwidth + bandwidthShiftOf(s), 0.0f, 1.0f);
	// LEVEL HAS NO GLOBAL PARTNER. Listed anyway so a reader is told rather than left to notice.
	out.level = band.level;
	// THE PER-BAND TRIMS AND OFFSETS, IN THE SAME CENTRED FORM AS THE LINKED SHIFTS ABOVE: a displacement from 0.5,
	// added in the control's own units and clamped to its range. At 0.5 the displacement is exactly 0.0f, so the
	// value is the one before them to the bit. Up and Down trim the ratio of their own direction (the Up/Down skew
	// still balances the two globally); Attack and Release offset the global's.
	out.ratioDown = std::clamp(out.ratio + (band.downRatio - 0.5f), 0.0f, 1.0f);
	out.ratioUp = std::clamp(out.ratio + (band.upRatio - 0.5f), 0.0f, 1.0f);
	out.attack = std::clamp(s.attack + (band.attack - 0.5f), 0.0f, 1.0f);
	out.release = std::clamp(s.release + (band.release - 0.5f), 0.0f, 1.0f);
	return out;
}

/// **THE WIDTH A BAND SENDS ON (backlog LR)**: the Character zone's width for the band, scaled by the global Width
/// and the band's own. A PRODUCT, so 100% on both is the zone's width to the bit and 0 on either is no side at all.
/// Clamped to +-2, the bound the render loop always held the zone's width to.
inline float effectiveBandWidthOf(float zoneWidth, float globalWidth, float bandWidth) {
	return std::clamp(zoneWidth * globalWidth * bandWidth, -2.0f, 2.0f);
}

/// WHAT TO STORE IN A BAND SO ITS EFFECTIVE VALUE LANDS WHERE A HAND PUT IT — the inverse, for a
/// drag on the picture.
///
/// INVERTED FROM THE RELATION ABOVE rather than derived forwards. A drag on a bar means "put the
/// EFFECTIVE threshold here"; what gets written is the band's own number, and the difference between
/// those two is exactly the global. This file has already paid twice for an inverse written as a
/// second formula that ought to agree -- the bandwidth gap and the frequency axis.
inline float bandValueForEffective(float wantedEffective, float shift) {
	return std::clamp(wantedEffective - shift, 0.0f, 1.0f);
}

/// WHAT EACH ZONE OF CHARACTER AND VIBE IS — beside the controls they belong to, not in the editor.
///
/// THEY ARRIVED IN `owltt_expanded.cpp`, WHICH DEFEATS THE POINT OF DECLARING THEM AT ALL. The whole
/// reason for device-owned text is goal is to not couple to the feathers plugin so
/// tooltips might still work when the effects are standalone. A name list in the editor is a name
/// list a standalone OWLTT does not have — and it is a list that can describe the wrong control,
/// which one sitting next to `{"Character", ...}` cannot.
///
/// EIGHT ENTRIES EACH, INCLUDING THE TWO CHARACTER ZONES THE DSP DOES NOT USE. `owltt.cpp`'s switch
/// has no case 1 or 2, so they are named as unused rather than given invented names — the honesty
/// rule for words: a wrong name is worse than a number, because a number does not claim to mean
/// anything.
///
/// THE DESCRIPTIONS ARE READ FROM THE DSP, not from taste. Punch sets `response_ = 0.9 + pos*0.1`
/// (fast); Air and Rich set `0.1 - pos*0.1` and `0.15 - pos*0.15` (slow); OWLTT runs a triangle
/// across the full range. Each line says what its zone does because the code was read to write it.
struct ZoneName {
	const char* name;
	const char* description;
};

inline constexpr ZoneName kCharacterZoneNames[] = {
    {"Width", "Sweep from slightly narrow to very enhanced stereo"},
    // ZONES 1 AND 2 WERE BLANK, captioned "no case in the DSP's switch" — and the switch has both:
    // `case 1: // Timing` and `case 2: // Skew` in `computeCharacterAndVibe`, as the firmware's menu
    // names them (OWLTT parity sweep, 2026-09-16). The widget rule that numbers an unnamed region
    // stays (`ZoneSelect::captionText`); OWLTT simply no longer needs it.
    {"Timing", "Per-band attack and release spread — low slower, high faster"},
    {"Skew", "Per-band up/down balance shifts the tone as the direction changes"},
    {"Punch", "Very fast detection for transients"},
    {"Air", "Slow detection for smooth glue (70-145ms)"},
    {"Rich", "Slow for warm sustain"},
    {"Slam", "Fast for classic aggressive response"},
    {"OWLTT", "Oscillates full range for dynamic breathing"},
};

inline constexpr ZoneName kVibeZoneNames[] = {
    {"Sync", "All oscillations in phase"},
    {"Spread", "Evenly spread phases (120 degrees apart)"},
    {"Pairs", "Band pairs in/out of phase"},
    {"Cascade", "Progressive phase shift across bands"},
    {"Invert", "Opposite phases between parameters"},
    {"Pulse", "Clustered phases creating pulses"},
    {"Drift", "Slowly varying phase relationships"},
    {"Chaos", "Rapidly oscillating phase offsets"},
};

/// **THE MODE LISTS (backlog IW)** — the eight zones in their order, then Free, which is always last (inflate
/// comes first because free is infinite, the same rule). The zone names above stay as the descriptions.
inline constexpr uint32_t kZoneModeFree = 8;
inline constexpr const char* kCharacterModeNames[] = {"Width", "Timing", "Skew", "Punch", "Air", "Rich", "Slam", "OWLTT", "Free"};
inline constexpr const char* kVibeModeNames[] = {"Sync", "Spread", "Pairs", "Cascade", "Invert", "Pulse", "Drift", "Chaos", "Free"};
static_assert(std::size(kCharacterModeNames) == kZoneModeFree + 1 && std::size(kVibeModeNames) == kZoneModeFree + 1);
/// EVERY MODE, as a gate mask — the value is live in all of them (in a mode it is the amount and the lap; in Free, the
/// coordinate itself), so its box never goes idle.
inline constexpr uint32_t kAllZoneModes = (1u << (kZoneModeFree + 1)) - 1u;

/// **WHERE A MODE AND ITS VALUE PUT THE COORDINATE THE DSP READS (backlog IW).** if the modes impls are
/// unbounded, lets keep in-mode phase unbounded. So in a mode the value stays unbounded: its FRACTION is the mode's
/// amount across the zone, and its WHOLE part is the lap — the field phase `owltt.cpp` already takes from `floor`. The
/// coordinate is then `lap + (mode + amount) / 8`: the same lap, always inside the chosen zone, so a drag rolls through
/// the same behaviour somewhere else in the field and never into another one. In Free it is the value itself — the
/// rolling coordinate this control always was.
[[nodiscard]] inline float zoneModeCoordinate(uint32_t mode, float value) {
	if (mode >= kZoneModeFree || !std::isfinite(value)) {
		return value;
	}
	const float lap = std::floor(value);
	return lap + (static_cast<float>(mode) + (value - lap)) / static_cast<float>(kZoneModeFree);
}

/// ONE LAP, IN A COORDINATE'S OWN UNITS — the span a full-depth route moves Character or Vibe.
///
/// Read off how `owltt.cpp` consumes them (`wrapPhase(phase * kPhiNNN)`, and `fmod(... + phase, 1.0)`):
/// ONE is a full turn of the field. Named rather than spelled `1.0` twice, because the two
/// coordinates must agree and a literal at two call sites is free to drift.
///
/// **IT USED TO NAME A SEPARATE PHASE PARAM'S UNIT, AND THE NUMBER DID NOT MOVE WHEN THAT PARAM WENT.**
/// That is the point rather than a coincidence: `std::floor(coordinate)` feeds the same consumption
/// the deleted counter fed, so one unit is still one turn and a full-depth route still moves the field
/// exactly once round. See the Character declaration below for what DOES change (every intermediate
/// route amount) and why that was accepted.
inline constexpr double kOnePhaseUnit = 1.0;

inline constexpr device::ParamDesc kParamDescs[] = {
    // Global
    {"mode", "Mode", 1.0, 10.0, 6.0, true, device::Unit::Choice, kModeNames, false, false,
     "Crossover filter type — how the spectrum is split into bands"},
    // ON BY DEFAULT — all effects should default to on when they are added.
    // See `fx::filter::kParamDescs` for the decision and what it replaced. This device is also the one
    // whose two declarations had already drifted: `Settings::enabled` said true while this said 0,
    // and the static_assert below is what stops that recurring.
    {"enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Bypass switch — off silences the device"},
    {"threshold", "Threshold", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Downward compression threshold — bands compress above this level"},
    {"ratio", "Ratio", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Compression ratio — how much the signal is reduced above threshold"},
    {"bandwidth", "Bandwidth", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Upward/downward gap — range where neither compression acts"},
    {"up_down_skew", "Up/Down Skew", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Balance — favors upward (quiet) or downward (loud) compression"},
    {"attack", "Attack", 0.0, 1.0, 0.25, false, device::Unit::Percent, {}, true, false,
     "Attack time — how fast compression engages"},
    {"release", "Release", 0.0, 1.0, 0.25, false, device::Unit::Percent, {}, true, false,
     "Release time — how fast compression lets go"},
    // THE ZONE COUNT IS DECLARED, not left for a reader to infer from the names table. It is what
    // makes a route's amount mean ONE ZONE here rather than the whole range — see ParamDesc::zones.
    // `kCharacterZoneNames` must stay the same length, and a case pins the two together rather than
    // trusting them to agree.
    // **ONE CONTROL EACH, AND THE PHASE IS PART OF IT — no dual-gang value boxes.**
    //
    // **WHAT MADE THE SECOND BOX REDUNDANT WAS THE UNBOUNDING, not a change of mind.** While these
    // were 0..1 the phase was a separate quantity a hand could only reach through its own counter.
    // Unbounded, the WHOLE PART of this control IS the lap, and `owltt.cpp` derives the field rotation
    // from it — so turning past the eighth zone already drives the field. Every (zone, lap) pair is
    // reachable from this control alone: `c = lap + zone/8 + position/8`.
    //
    // What the deleted counter still bought was a FRACTIONAL field offset, between laps. One unit is
    // the natural quantum — `kOnePhaseUnit` is read off the consumption — so the in-between positions
    // were extra rather than the design, and they cost a second box on every coordinate.
    //
    // **`modulatable` MOVES HERE, because there is one control to aim at.** The old pair put the flag
    // on the phase; a device with no pair has no half to choose between.
    //
    // **AND `modSpan` STAYS `kOnePhaseUnit` = 1.0 SO THE FULL-DEPTH ENDPOINT IS UNCHANGED.** That is
    // not caution, it is arithmetic: a full-depth route moves this control by exactly ONE LAP, which
    // returns the zone to where it was and advances the field by one turn — precisely what a
    // full-depth route on `Character Phase` did. **Intermediate amounts DO differ**: half a route now
    // sweeps four zones where it used to rotate the field by half a turn. That is inherent to the
    // collapse rather than a choice, and it is said out loud because this device's routes were once
    // wrong by 64x through exactly this kind of confusion.
    {"character", "Character", -8388608.0, 8388608.0, 0.0, false, device::Unit::Number, {}, true, true,
     "Tonal character — in a mode, how far into it and which lap of the field; in Free, the coordinate "
     "rolling through all eight",
     8, kOnePhaseUnit, {}, static_cast<int16_t>(Param::CharacterMode), kAllZoneModes},
    {"vibe", "Vibe", -8388608.0, 8388608.0, 0.0, false, device::Unit::Number, {}, true, true,
     "Phase/vibe — in a mode, how far into it and which lap of the field; in Free, the coordinate rolling "
     "through all eight",
     8, kOnePhaseUnit, {}, static_cast<int16_t>(Param::VibeMode), kAllZoneModes},

    // Low band (offsets from global)
    {"low_threshold", "Low Threshold", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Low band threshold — offset from global"},
    {"low_ratio", "Low Ratio", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Low band ratio — offset from global"},
    {"low_bandwidth", "Low Bandwidth", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Low band gap — offset from global"},
    {"low_level", "Low Level", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Low band output level"},

    // Mid band (offsets from global)
    {"mid_threshold", "Mid Threshold", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Mid band threshold — offset from global"},
    {"mid_ratio", "Mid Ratio", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Mid band ratio — offset from global"},
    {"mid_bandwidth", "Mid Bandwidth", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Mid band gap — offset from global"},
    {"mid_level", "Mid Level", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Mid band output level"},

    // High band (offsets from global)
    {"high_threshold", "High Threshold", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "High band threshold — offset from global"},
    {"high_ratio", "High Ratio", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "High band ratio — offset from global"},
    {"high_bandwidth", "High Bandwidth", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "High band gap — offset from global"},
    {"high_level", "High Level", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "High band output level"},

    // Output
    // THE UNIT IS PRINTED, whole hertz (`ParamDesc::suffix`, `wholeReadout`): a bare "200.00" said nothing about 200 of what.
    {.key = "low_crossover", .name = "Low Crossover", .min = 20.0, .max = 2000.0, .def = 200.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low-mid split frequency", .wholeReadout = true, .suffix = "Hz"},
    {.key = "high_crossover", .name = "High Crossover", .min = 200.0, .max = 20000.0, .def = 2000.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid-high split frequency", .wholeReadout = true, .suffix = "Hz"},
    {"output_gain", "Output Gain", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
     "Output level after the bands are summed, in dB — 0 dB is unity, the top of the range is +16 dB"},
    {"blend", "Blend", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Wet/dry mix — balance between processed and original"},
    // THE SOFT CLIPPER'S SWITCH (backlog EL). On is how the device always ran: each band's sum is
    // knee-clipped at +6 dB and the output at 0 dBFS. Off lets a hot band through unclipped — the
    // firmware's `setSoftClipEnabled`, which its Mode encoder toggled with a click. Not modulatable:
    // a clipper switching under modulation is a click, not an effect.
    {.key = "soft_clip", .name = "Soft Clip", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kClipNames, .modulatable = false, .offsetsMayLeaveRange = false,
     .tooltip = "Soft clipper — Off lets peaks through; Master clips each band (+6 dB) and then the output (0 dBFS); "
                "Bands clips each band and leaves the sum alone",
     .shortName = "Clip", .choiceAliases = kClipAliases},
    // **CHARACTER'S AND VIBE'S MODE CELLS (backlog IW).** Stepped Choices, eight zones then Free, default Free — so a
    // fresh device sounds as it did: Free is the rolling coordinate. Not destinations: a mode switching under a route is
    // a different behaviour arriving mid-note, and IW's modes are chosen, not swept.
    {.key = "character_mode", .name = "Character Mode", .min = 0.0, .max = 8.0, .def = 8.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kCharacterModeNames, .modulatable = false, .offsetsMayLeaveRange = false,
     .tooltip = "Which character — eight behaviours, then Free, which rolls through all of them",
     .shortName = "Character", .modeCell = true},
    {.key = "vibe_mode", .name = "Vibe Mode", .min = 0.0, .max = 8.0, .def = 8.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kVibeModeNames, .modulatable = false, .offsetsMayLeaveRange = false,
     .tooltip = "Which vibe — eight phase relationships, then Free, which rolls through all of them",
     .shortName = "Vibe", .modeCell = true},
    // BACKLOG LQ. Link is a continuous blend, so a knob and a destination; the L/R-M/S switch is a topology
    // and not swept, the soft clipper's reasoning.
    {.key = "stereo_link", .name = "Stereo Link", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "How far each channel's detector follows the band's shared one — 100% turns both down together, "
                "0% compresses each channel of every band on its own",
     .shortName = "Link"},
    {.key = "stereo_mode", .name = "Stereo Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kStereoModeNames, .modulatable = false, .offsetsMayLeaveRange = false,
     .tooltip = "L/R compresses left and right; M/S compresses each band's middle and sides, then decodes back"},
    // BACKLOG LR. Four scalers on the Character zone's per-band M/S width: the global on the card, the three bands in
    // the window. 0..200% like the other widths in the rack (`bus_width`, the stereo modulator's Width); 100% is the
    // zone's own width, so a fresh device renders as it did before these existed.
    {.key = "width", .name = "Width", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Stereo width of all three bands, against what the Character zone sets — 100% is the zone's own, "
                "0 is mono, 200% twice as wide"},
    {.key = "low_width", .name = "Low Width", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low band stereo width, on top of Width — 100% is the zone's own, 0 is mono"},
    {.key = "mid_width", .name = "Mid Width", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid band stereo width, on top of Width — 100% is the zone's own, 0 is mono"},
    {.key = "high_width", .name = "High Width", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "High band stereo width, on top of Width — 100% is the zone's own, 0 is mono"},
    // BACKLOG NB. A gain, so a destination, and read per sample while modulated (`kWindowedControls`), as Blend is.
    {.key = "drive", .name = "Drive", .min = kDriveMinDb, .max = kDriveMaxDb, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Gain in front of the compressor — how hard the signal is pushed into the bands; Blend's dry is not driven"},
    // BACKLOG ND. A latency, so not a destination (L1); a Choice so a drag cannot restart the host at every step.
    {.key = "lookahead", .name = "Lookahead", .min = 0.0, .max = static_cast<double>(kLookaheadChoices - 1), .def = 0.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kLookaheadNames, .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Delays the audio, not the detectors, so the gain is already down when a loud onset arrives — "
                "adds that much latency; Off adds none"},
    {.key = "owltt_key_track", .name = "OWLTT Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false, .offsetsMayLeaveRange = false,
     .tooltip = "The last note played places the crossovers, and each becomes its offset from the note: Low at its "
                "default sits on the note, High keeps its default distance above",
     .shortName = "Key"},
    // THE PER-BAND REBUILD: six ratio trims (up and down for each band), six attack/release offsets and three input
    // levels. The trims and offsets are centred on 0.5 like the three per-band controls beside them; the input is a
    // gain in dB, Drive's form, per band.
    {.key = "low_up_ratio", .name = "Low Up Ratio", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low band upward ratio — trims this band's ratio for the upward compressor only; the centre leaves it as the band ratio sets it",
     .shortName = "Up"},
    {.key = "low_down_ratio", .name = "Low Down Ratio", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low band downward ratio — trims this band's ratio for the downward compressor only; the centre leaves it as the band ratio sets it",
     .shortName = "Down"},
    {.key = "mid_up_ratio", .name = "Mid Up Ratio", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid band upward ratio — trims this band's ratio for the upward compressor only; the centre leaves it as the band ratio sets it",
     .shortName = "Up"},
    {.key = "mid_down_ratio", .name = "Mid Down Ratio", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid band downward ratio — trims this band's ratio for the downward compressor only; the centre leaves it as the band ratio sets it",
     .shortName = "Down"},
    {.key = "high_up_ratio", .name = "High Up Ratio", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "High band upward ratio — trims this band's ratio for the upward compressor only; the centre leaves it as the band ratio sets it",
     .shortName = "Up"},
    {.key = "high_down_ratio", .name = "High Down Ratio", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "High band downward ratio — trims this band's ratio for the downward compressor only; the centre leaves it as the band ratio sets it",
     .shortName = "Down"},
    {.key = "low_attack", .name = "Low Attack", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low band attack — an offset from the global Attack; the centre leaves this band on the global",
     .shortName = "Att"},
    {.key = "low_release", .name = "Low Release", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low band release — an offset from the global Release; the centre leaves this band on the global",
     .shortName = "Rel"},
    {.key = "mid_attack", .name = "Mid Attack", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid band attack — an offset from the global Attack; the centre leaves this band on the global",
     .shortName = "Att"},
    {.key = "mid_release", .name = "Mid Release", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid band release — an offset from the global Release; the centre leaves this band on the global",
     .shortName = "Rel"},
    {.key = "high_attack", .name = "High Attack", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "High band attack — an offset from the global Attack; the centre leaves this band on the global",
     .shortName = "Att"},
    {.key = "high_release", .name = "High Release", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "High band release — an offset from the global Release; the centre leaves this band on the global",
     .shortName = "Rel"},
    {.key = "low_input", .name = "Low Input", .min = kBandInputMinDb, .max = kBandInputMaxDb, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Low band input level — gain on this band after the split and before its detector and compressor",
     .shortName = "In"},
    {.key = "mid_input", .name = "Mid Input", .min = kBandInputMinDb, .max = kBandInputMaxDb, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "Mid band input level — gain on this band after the split and before its detector and compressor",
     .shortName = "In"},
    {.key = "high_input", .name = "High Input", .min = kBandInputMinDb, .max = kBandInputMaxDb, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "High band input level — gain on this band after the split and before its detector and compressor",
     .shortName = "In"},
};

/// ND'S DEFAULT, PINNED TO THE DESCRIPTOR: Off is the device before the lookahead.
static_assert(Settings {}.lookahead == static_cast<uint32_t>(kParamDescs[static_cast<size_t>(Param::Lookahead)].def));

/// NB'S DEFAULT, PINNED TO THE DESCRIPTOR: 0 dB is the device before the drive.
static_assert(Settings {}.driveDb == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Drive)].def));

/// LR'S DEFAULTS, PINNED TO THE DESCRIPTOR: 100% everywhere is the zone's own width, the device before LR.
static_assert(Settings {}.width == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Width)].def));
static_assert(BandParams {}.width == static_cast<float>(kParamDescs[static_cast<size_t>(Param::LowWidth)].def));
static_assert(BandParams {}.width == static_cast<float>(kParamDescs[static_cast<size_t>(Param::MidWidth)].def));
static_assert(BandParams {}.width == static_cast<float>(kParamDescs[static_cast<size_t>(Param::HighWidth)].def));

/// LQ'S DEFAULTS, PINNED TO THE DESCRIPTOR, so a fresh `Settings` and a fresh row agree: Link 100% and L/R
/// are the linked device this was before.
static_assert(Settings {}.stereoLink == static_cast<float>(kParamDescs[static_cast<size_t>(Param::StereoLink)].def));
static_assert(Settings {}.midSide == (kParamDescs[static_cast<size_t>(Param::StereoMode)].def >= 0.5));

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// **THE ENABLE'S TWO DECLARATIONS, PINNED TOGETHER.** `stateOf` fills an omitted field from
/// `ParamDesc::def` while `Settings {}` hands a caller the struct's own initialiser, so a device
/// whose two numbers disagree has a fresh state that depends on which road it arrived by. THIS
/// DEVICE WAS ONE OF THE TWO THAT DID DISAGREE — struct true, descriptor 0 — and it went unnoticed
/// because every production road happens to assemble `Settings` from a row. That is a property of
/// today's callers, not a guarantee. HARM carries the same block for the same reason.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in owltt.cpp, where the DSP class is.
extern const device::AudioOps kAudioOps;

/// The Descriptor: everything the host needs to know about OWLTT.
//--------------------------------------------------------------------------------------------------//
// KEY TRACK — the Phaser's contract (`fx::phaser`, `phaser.h`'s class comment): the note //
// LATCHED while a key is held, held after the release, forgotten at the host's reset; no note yet, the knobs rule. //
//--------------------------------------------------------------------------------------------------//

/// THE NOTE'S OWN FREQUENCY, A4 = 69, the pitch clamped to [−60, 200] semitones — `fx::phaser::keyHzOf`, restated.
[[nodiscard]] inline double keyHzOf(double pitchSemitones) noexcept {
	const double p = std::isfinite(pitchSemitones) ? std::clamp(pitchSemitones, -60.0, 200.0) : 69.0;
	return 440.0 * std::exp2((p - 69.0) / 12.0);
}

/// **THE CROSSOVERS' ONE REFERENCE**: Low Crossover's default, 200 Hz, as the row stores it. WHERE THE PHASER'S CONTRACT
/// DID NOT FIT: its one knob is measured from its own default; two corners each measured from their own default would
/// both land on the note. From ONE reference the default split transposes with the note — Low on it, High 39.9
/// semitones (its default's 2 kHz) above.
inline constexpr double kCrossoverKeyRefHz = 200.0;

/// A CROSSOVER KNOB (Hz) AS AN OFFSET FROM THE NOTE, semitones.
[[nodiscard]] inline double crossoverKeyOffsetSemitones(double knobHz) noexcept {
	return 12.0 * std::log2(std::max(knobHz, 1e-6) / kCrossoverKeyRefHz);
}

/// **THE CORNER THE CROSSOVER RUNS AT**, Hz: the knob, or — tracking (Key Track on and a note heard) — the note moved
/// by the knob's offset, clamped to the audible 20 Hz .. 20 kHz.
[[nodiscard]] inline double trackedCrossoverHz(double knobHz, bool tracking, double noteHz) noexcept {
	if (!tracking) {
		return knobHz;
	}
	return std::clamp(noteHz * (knobHz / kCrossoverKeyRefHz), 20.0, 20000.0);
}

/// WHILE A NOTE IS TRACKED, A CROSSOVER KNOB READS AS ITS OFFSET FROM IT — "+7.0 st", `phaserOffsetText`'s form — and
/// reads one back. Only while tracking (the `NoteHz` reading non-zero); otherwise the unit prints, as the Phaser's does.
extern const device::TextOps kTextOps;
/// The glide and its landing — the Phaser's (`kKeySmoothSeconds`, `kGlideSnap`).
inline constexpr double kKeySmoothSeconds = 0.005;
inline constexpr double kGlideSnap = 1.0e-7;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.text = &kTextOps,
	.category = device::Category::Dynamics,
};

} // namespace feathers::fx::owltt
