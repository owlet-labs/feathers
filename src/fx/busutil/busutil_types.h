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

// BUS UTILITY — gain, pan, dual pan, width and polarity, on the summed bus.
//
// The design decisions are stated here only where a line of code would otherwise look arbitrary.
//
// THE JOB PHRASE, which is this repo's test for whether a device may exist at all (can you name the
// job in one phrase a musician would use?): **set the level and the stereo
// image.** A hand does it, statically. That tense is the whole boundary between this device and
// Unpan, which holds a value rather than setting one and is therefore a different device.
//
// **BUS ONLY, BY DESIGN.** `perVoiceControl` stays `-1`, and the
// NAME carries the decision so a browser answers "why can't I put one per voice" before it is asked.
//
// THE WHOLE DEVICE IS ONE 2x2 MATRIX AND THAT IS THE DESIGN.
//
//     out = G . P . D . W . I . in
//
// polarity, width, dual pan, pan, gain — every one of them a linear map from the input pair to the
// output pair. Composing them into a single matrix per block buys three things that five sequential
// stages would not:
//
//   1. THE ORDER BECOMES LITERAL MATRIX ORDER, written once in `matrixOf` where it can be read,
//      rather than implied by the sequence of statements in a process loop.
//   2. ONE RAMP FIXES EVERY ZIPPER. The four coefficients slew toward their new values over a fixed
//      TIME (`kRampSeconds`), so gain, pan, width and polarity all smooth by one mechanism. There is
//      no per-control smoothing to get individually wrong, and no control that was forgotten.
//   3. IT IS ASSERTABLE. `matrixForTest` hands a case the four composed numbers, so a test states a
//      claim about a VALUE. CLAUDE.md's rule — a claim about a value cannot be inferred from a
//      rendered buffer — has been paid for four times in this repo, and every instance was a PARTIAL
//      effect surviving a difference check. A wrong pan law is exactly a partial effect.

namespace feathers::fx::busutil {

/// THE PLUGIN'S ID BLOCK IS ASSIGNED IN THIS ORDER, so APPENDING IS THE ONLY SAFE EDIT — a device
/// row is index-aligned with this enum.
enum class Param : int {
	Enabled,
	/// **THE OUTPUT FADER — a POSITION, not decibels.** 0.5 is unity at twelve o'clock, 0 is silence,
	/// 1 is +12 dB. See `device::Unit::Fader`, and `faderGain` below for the curve.
	///
	/// IT WAS A dB TRIM OF +-24 AND IS NOW A FADER: a volume knob must reach silence, and the trim
	/// bottomed out at -24 dB. The fader is unity at 12:00 with boost above. The dB range could not reach
	/// silence at all without a discontinuity bolted onto its end, which was refused by name.
	Gain,
	/// BALANCE ACROSS THE PAIR, not a placement. See `panBalance`.
	Pan,
	/// WHERE THE LEFT INPUT CHANNEL IS PLACED, -1..1. Defaults to hard left, which with `PanR` at
	/// hard right makes dual pan the identity.
	PanL,
	/// WHERE THE RIGHT INPUT CHANNEL IS PLACED, -1..1.
	PanR,
	/// MID/SIDE SCALE. 0 is mono, 1 is unchanged, 2 is double the side.
	Width,
	/// **TWO SWITCHES RATHER THAN ONE, AND THAT IS THE POINT OF HAVING THEM.** Inverting BOTH
	/// channels of a stereo bus is inaudible in isolation; inverting ONE is the tool — it is how a
	/// correlated pair is heard as correlated, and it is what makes a mono-summed check meaningful.
	/// A single "Polarity" switch would be a control that does nothing you can hear.
	InvertL,
	InvertR,

	/// **THE DC / RUMBLE FILTER'S CORNER, WITH `Off` AT THE BOTTOM AND `Off` AS THE DEFAULT.**
	/// add the dc filter/hipass. cutoff knob. It should be pre metering of course.
	///
	/// **OFF IS NOT OPTIONAL AND IT IS NOT TIDINESS.** A filter running at ANY corner changes the
	/// samples, and this device's first promise is that adding one renders the bits that were there
	/// before it existed — asserted sample-exact, not approximately. A highpass parked at 5 Hz would
	/// be inaudible and would still break that claim, and "inaudible" is not a thing a test can
	/// assert. So the stage is genuinely skipped at `Off`, not run with a harmless corner.
	///
	/// A LADDER RATHER THAN A CONTINUOUS SWEEP, for the reason the same question got the same answer
	/// when it was mono-below's: `device.h` gives `LogSeconds` a convention that its minimum means
	/// "no time at all" and there is NO such convention for frequency — a filter at its lowest cutoff
	/// is emphatically not off — so a continuous knob would draw "5" while the stage was bypassed.
	/// `Off` as a NAMED step says it, and today has twice shown what an ambiguous label costs.
	Highpass,

	/// **THE CLIPPER'S KNEE — 0 IS OFF, AND ABOVE THAT THE VALUE IS THE FRACTION OF THE RANGE THAT
	/// SATURATES.** Three requirements converged here: a hard clip, placed after the meter so a lit clip
	/// LED proves the clipper is active; a soft option that is a basic tanh; and, ideally, a curve that is
	/// linear up to some percentage — a 3% knee saturates only the top 3% of the amplitude.
	///
	/// **THE THIRD ONE SUBSUMES THE OTHER TWO, WHICH IS WHY IT IS ONE KNOB AND NOT A MODE.** A knee
	/// of a few percent IS a hard clip for any purpose a hard clip serves; a knee of 100% IS the basic
	/// tanh. Everything between is a saturation that leaves the signal EXACTLY untouched until it
	/// nears the ceiling — which is the property that makes it usable on a bus rather than a colour
	/// you have to commit to.
	///
	/// **THE TRAVEL IS SPECIFIED AS A READOUT:** fully counter-clockwise reads "off", then moving up
	/// "soft 100%" through "soft 1%", then "hard". So turning up makes the knee NARROWER — most saturation just
	/// off the bottom, least at the top, and a clamp at the end of the travel.
	///
	/// **A NAMED LADDER RATHER THAN A PERCENTAGE, BECAUSE THE TWO ENDS ARE NOT PERCENTAGES.** "Off"
	/// and "Hard" are states, not knee widths, and a `Percent` readout would print "0%" for one and
	/// "100%" for the other — numbers that describe neither. Today has cost three readings that meant
	/// something other than they said; this one says `off`, `soft 50%`, `hard`.
	///
	/// **AND IT SITS AFTER THE METER, WHICH IS THE OTHER DECISION.** A clipper ahead of the meter would
	/// make the 0 dBFS LED permanently dark — nothing downstream of one exceeds full scale — so the
	/// single indicator that could tell you it was working would be silenced by the thing it reports
	/// on. Metered ahead, the LED lights exactly when the clipper has something to do.
	Clip,

	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR): the slot supplies them and blends around the
	/// device, so the device never reads either. Mix is a dry/wet crossfade, 100% the wet; the wet Gain is a fader
	/// position (unity at `kFaderUnity`) on the wet only. Both defaults leave the device as it was, to the bit.
	Mix,
	WetGain,

	/// **MID AND SIDE TRIMS, IN dB** (backlog DM, 2026-10-30). Beside Width rather than instead of it: Width is the
	/// symmetric case and its key is stored, and the trims are the general one. 0 dB is the identity, and the stage is
	/// skipped there, so a device at its defaults renders the bits it rendered before these existed.
	MidTrim,
	SideTrim,
	/// **WHAT YOU HEAR, FOR CHECKING THE MIX** (backlog DM): Stereo, Mono, Left, Right, Side — each a 2x2, so it folds
	/// into the matrix, LAST, after the fader. The meter therefore reads what is being monitored. Mid is not a separate
	/// rung: with mid = (L+R)/2 it is the same matrix as Mono, and two names for one sound is a choice that does nothing.
	Monitor,
	/// **SWAP L/R** (backlog DM), FIRST, at the input — so every control after it (Invert L, Pan L) names the channel
	/// that is on the left by then.
	Swap,

	kCount,
};

/// THE 2x2 THIS DEVICE APPLIES. `outL = ll*inL + lr*inR`, `outR = rl*inL + rr*inR`.
///
/// NAMED BY DESTINATION-THEN-SOURCE, which is the order the multiplication reads in: `lr` is what
/// the RIGHT input contributes to the LEFT output. The alternative naming (source-then-destination)
/// reads the same on the diagonal and silently transposes off it, which is a bug that sounds like a
/// swapped channel and looks like nothing.
struct Matrix {
	float ll = 1.0f;
	float lr = 0.0f;
	float rl = 0.0f;
	float rr = 1.0f;

	friend bool operator==(const Matrix&, const Matrix&) = default;
};

/// **WHERE UNITY SITS ON THE FADER — the centre, which is twelve o'clock on the knob.** `Knob` maps
/// position to value linearly over the declared range, so a default at the middle of 0..1 IS the
/// pointer straight up; nothing draws this, it falls out of the numbers.
inline constexpr double kFaderUnity = 0.5;

/// THE GAIN A FADER POSITION MEANS: `(2v)^2`. Unity at 0.5, silence at 0, +12 dB at 1.
///
/// **SILENCE IS THE BOTTOM OF THE SCALE RATHER THAN A CASE**, which is the whole reason the value is
/// a position: zero amplitude is just zero, where a dB-valued control would need -inf spliced onto
/// the end of its travel.
[[nodiscard]] constexpr float faderGain(float position) {
	const float p = position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position);
	return (2.0f * p) * (2.0f * p);
}
static_assert(faderGain(static_cast<float>(kFaderUnity)) == 1.0f,
              "the centre of the fader must be EXACTLY unity, or a device at its defaults is not the "
              "identity and every sample-exact claim in test_fx_busutil.cpp is a tolerance");

struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — all effects default to on when
	/// they are added. The static_assert below pins the two declarations together so a fresh state
	/// cannot depend on which road it arrived by.
	bool enabled = true;
	/// EVERY DEFAULT BELOW IS THE IDENTITY MATRIX, and that is a correctness requirement rather than
	/// a courtesy: a Bus Utility freshly added to a chain must render the bits that were there before
	/// it existed. `test_fx_busutil.cpp` pins it sample-exact rather than approximately.
	/// THE FADER POSITION, not a gain and not decibels. See `faderGain`.
	float gain = static_cast<float>(kFaderUnity);
	float pan = 0.0f;
	float panL = -1.0f;
	float panR = 1.0f;
	float width = 1.0f;
	bool invertL = false;
	bool invertR = false;
	/// INDEX INTO `kHighpassNames`. 0 is Off, and Off is a true bypass — see `Param::Highpass`.
	int highpass = 0;
	/// INDEX INTO `kClipNames`. 0 is Off; the rest run from the widest knee to a hard clamp.
	int clip = 0;
	/// dB. See `Param::MidTrim`.
	float midTrim = 0.0f;
	float sideTrim = 0.0f;
	/// INDEX INTO `kMonitorNames`. 0 is Stereo, the identity.
	int monitor = 0;
	bool swap = false;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// Row: the serialized form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);


/// **HOW LONG A COEFFICIENT TAKES TO REACH A NEW VALUE — A TIME, NOT A BLOCK.**
///
/// THE FIRST VERSION OF THIS DEVICE RAMPED ACROSS ONE BLOCK AND `test_fx_runner` KILLED IT. Its case
/// *"EVERY audio device: the same samples in a different partition are the same samples out"*
/// measured a worst |diff| of **0.0844** on Bus Utility at partition 2 — the same audio through the
/// same settings, rendered in different block sizes, coming out different. A per-block ramp makes
/// the host's buffer size audible, which is a property no device may have: the same project
/// bounces differently than it monitors.
///
/// A FIXED TIME IS PARTITION-INDEPENDENT BY CONSTRUCTION, because the progress is counted in SAMPLES
/// and carried across the block boundary rather than restarted at it. Splitting a block in two is
/// then exactly the same arithmetic in two pieces.
///
/// 5 ms: long enough that a step in a gain coefficient is a fade rather than a click, short enough
/// that a hand moving a knob feels the value and not the smoothing.
inline constexpr double kRampSeconds = 0.005;

/// CONSTANT-POWER PLACEMENT OF ONE MONO SOURCE — what DUAL PAN does to each input channel.
///
/// `gL = cos(theta)`, `gR = sin(theta)` with `theta = (pos + 1) * pi/4`, so a source at -1 is
/// entirely left, at +1 entirely right, and at 0 is at 0.7071 in both — equal POWER, which is what
/// keeps a sound the same apparent loudness as it crosses the field.
///
/// A CONSEQUENCE TO STATE BEFORE SOMEBODY REPORTS IT AS A BUG: dual-panning both channels to the
/// centre BOOSTS correlated material by 3 dB, because each channel arrives at 0.7071 in both outputs
/// and the two add. That is inherent to constant-power placement rather than a defect here, and it
/// is what every dual pan does.
struct Placement {
	float left = 1.0f;
	float right = 0.0f;
};
[[nodiscard]] Placement placeMono(float position);

/// BALANCE ACROSS A STEREO PAIR — what the PAN knob does, and it is a DIFFERENT LAW from the one
/// above, deliberately.
///
/// **UNITY AT CENTRE, ATTENUATING ONE SIDE.** `gL = min(1, 1 - pan)`, `gR = min(1, 1 + pan)`.
///
/// THE CONSTANT-POWER LAW WOULD BE WRONG HERE AND IT IS WORTH SAYING WHY, because the two controls
/// sit next to each other on the same card and the temptation to share one helper is real. Constant
/// power puts 0.7071 in both channels at the centre, which for a MONO SOURCE BEING PLACED is exactly
/// right — the source is one thing and its power is conserved. Applied to a STEREO PAIR it drops a
/// bus by 3 dB for the crime of existing, at the default position, before a hand has touched the
/// knob. A pan control that attenuates when centred is a pan control nobody trusts.
struct Balance {
	float left = 1.0f;
	float right = 1.0f;
};
[[nodiscard]] Balance panBalance(float pan);

/// **THE CLIP LADDER, READING EXACTLY AS THE TRAVEL.** Off at full anticlockwise,
/// then the widest knee, narrowing to one percent, then a hard clamp at the top.
inline constexpr const char* kClipNames[] = { "Off",     "Soft 100%", "Soft 50%",
	                                         "Soft 25%", "Soft 12%",  "Soft 6%",
	                                         "Soft 3%",  "Soft 1%",   "Hard" };
/// THE KNEE EACH STEP MEANS. **Index 0 is a placeholder nothing reads** (`clipEnabled` refuses it),
/// and the LAST is 0 — a knee of nothing, which is a clamp.
inline constexpr float kClipKnee[] = { 0.0f,  1.0f,  0.5f,  0.25f, 0.12f,
	                                   0.06f, 0.03f, 0.01f, 0.0f };
static_assert(std::size(kClipNames) == std::size(kClipKnee));

[[nodiscard]] constexpr bool clipEnabled(int index) {
	return index > 0 && static_cast<size_t>(index) < std::size(kClipKnee);
}
[[nodiscard]] constexpr float clipKnee(int index) {
	return clipEnabled(index) ? kClipKnee[static_cast<size_t>(index)] : 0.0f;
}

/// **ONE SAMPLE THROUGH THE CLIPPER.** `knee` is `Settings::clip`; at 0 it is a HARD CLAMP to ±1 (the
/// "Hard" rung, the last entry of `kClipKnee`). Off is not a knee: `clipEnabled` refuses it before this
/// is ever called, which is what makes "off" a genuine bypass rather than a curve that happens to be
/// straight. The curve itself is `dyn::softClip` (`fx/dyn/clip_curve.h`), shared with the dynamics
/// devices.
///
/// LINEAR BELOW `1 - knee`, THEN A tanh THAT REACHES 1 ASYMPTOTICALLY:
///
///     |y| = t + (1 - t) * tanh((|x| - t) / (1 - t)),  t = 1 - knee
///
/// **THE SHAPE IS C1-CONTINUOUS AT THE KNEE BY CONSTRUCTION, not by fitting.** `tanh'(0) == 1`, so
/// the curve leaves the linear region at exactly unit slope — there is no corner where the two meet,
/// which is what a listener would hear as a click on material grazing the threshold. At a knee of a
/// few percent the curved region is so short it is a hard clip; at 1.0, `t` is 0 and this is plain
/// `tanh(x)`.
[[nodiscard]] float clipSample(float x, float knee);

/// THE COMPOSED MATRIX FOR A SETTINGS — the whole device in one function.
///
/// **THE ORDER IS `G . P . D . W . I` AND IT IS AUDIBLE.** With a Stereo/Dual mode switch the
/// question would never arise, because only one pan would be live at a time; the decision that both
/// are live (pan is a knob of its own) is what makes the order a fact a user can hear. Width before dual
/// pan so the M/S scaling acts on the source material and the placement that follows is honest;
/// gain last so it is an output trim rather than something the M/S maths scales.
///
/// A DISABLED DEVICE RETURNS THE IDENTITY rather than being skipped by the caller, so that the ramp
/// in `kAudioOps` carries the bypass in and out instead of cutting to it. The enable is still a
/// hard-bypass in the sense that matters — a disabled Bus Utility changes no sample — but it gets
/// there over a block rather than in one sample, which a click would otherwise announce.
[[nodiscard]] Matrix matrixOf(const Settings& settings);

/// THE HIGHPASS LADDER. **INDEX 0 IS `Off` AND IS NOT A FREQUENCY** — the parallel table keeps a
/// placeholder there and nothing reads it; `highpassHz` refuses index 0 rather than returning it.
///
/// THE RANGE SPANS TWO JOBS ON PURPOSE. 10-30 Hz is a DC and subsonic trap that costs nothing
/// audible; 50-120 Hz is a rumble filter that thins a patch deliberately. It was asked for
/// "dc filter/hipass" and those are the two halves of that slash.
/// **THE UNIT IS IN THE NAME**, because a `Choice` prints its name and nothing else — there is no
/// unit for the box to append the way `LogHertz` or `Decibel` would. A bare "30" on a card that also
/// shows percentages and decibels is a number without a dimension, and this card has already cost
/// Three readings today that meant something other than they said.
inline constexpr const char* kHighpassNames[] = { "Off",  "10Hz", "20Hz", "30Hz",
	                                             "50Hz", "80Hz", "120Hz" };
inline constexpr float kHighpassHz[] = { 0.0f, 10.0f, 20.0f, 30.0f, 50.0f, 80.0f, 120.0f };
static_assert(std::size(kHighpassNames) == std::size(kHighpassHz));

/// THE CORNER FOR A SETTING, or 0 when the stage is off. **0 IS THE REFUSAL, NOT A FREQUENCY.**
[[nodiscard]] constexpr float highpassHz(int index) {
	return (index <= 0 || static_cast<size_t>(index) >= std::size(kHighpassHz))
	           ? 0.0f
	           : kHighpassHz[static_cast<size_t>(index)];
}

/// THE MONITOR CHOICES (backlog DM). Stereo first and the default; each later rung sends one signal to both outputs.
inline constexpr const char* kMonitorNames[] = { "Stereo", "Mono", "Left", "Right", "Side" };
enum class Monitor : int { Stereo, Mono, Left, Right, Side };

/// THE M/S TRIMS' RANGE, dB either side of 0.
inline constexpr double kTrimDb = 12.0;

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "utility", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "busu",
	.name = "Bus Utility",
	.hasCard = true,
	/// NO EXPANDED VIEW. Eight controls fit a card; `hasExpanded` without a `makeExpandedView` entry
	/// is the declaration that gave THERE IS NO EXPANDED VIEW AVAILABLE. — both halves or
	/// neither, and this device needs neither.
	.hasExpanded = false,
	/// **BUS ONLY.** Not an omission and not a default: this device
	/// declares no per-voice road because by design it has none, and device.h's standing rule
	/// is that a device which has not implemented a per-voice road must not claim one.
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// THE SLOT BLENDS THIS (backlog NR): a plain crossfade, so the engine applies Mix and the wet Gain
	// around the device and its site, and the device hands back the wet.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

/// OFF/ON, DECLARED HERE RATHER THAN SHARED. Five device headers carry their own copy of this pair
/// and that is the vocabulary working as intended: a device includes nothing from the plugin and
/// nothing from its neighbours, so two words is the price of the boundary.
inline constexpr const char* kOffOnNames[] = { "Off", "On" };

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    // See `fx::filter::kParamDescs` for the decision and the world it replaced.
    {"bus_enabled", "Bus Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Whether the bus utility is in the chain"},
    {"bus_gain", "Bus Gain", 0.0, 1.0, kFaderUnity, false, device::Unit::Fader, {}, true, false,
     "Output fader — unity at twelve o'clock, silence fully down, +12 dB fully up"},
    {"bus_pan", "Bus Pan", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Balance across the pair — unity at centre, attenuating one side"},
    {"bus_pan_l", "Bus Pan L", -1.0, 1.0, -1.0, false, device::Unit::Percent, {}, true, false,
     "Where the left input channel is placed — with Pan R, this is the dual pan"},
    {"bus_pan_r", "Bus Pan R", -1.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Where the right input channel is placed"},
    {"bus_width", "Bus Width", 0.0, 2.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Mid/side scale — 0 is mono, 100% is unchanged, 200% is double the side"},
    {"bus_invert_l", "Bus Invert L", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Flip the polarity of the left channel alone — inverting both is inaudible, inverting one is "
     "the tool"},
    {"bus_invert_r", "Bus Invert R", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Flip the polarity of the right channel alone"},
    // **OFF BY DEFAULT, because a filter that ran on a freshly added device would change the mix
    // before a hand had touched anything** — the same reason every other default here is the
    // identity.
    {"bus_highpass", "Bus Highpass", 0.0, static_cast<double>(std::size(kHighpassNames) - 1), 0.0, true,
     device::Unit::Choice, kHighpassNames, false, false,
     "DC and rumble filter at 12 dB/oct, ahead of the meter — Off is a true bypass, not a corner "
     "set low"},
    {"bus_clip", "Bus Clip", 0.0, static_cast<double>(std::size(kClipNames) - 1), 0.0, true,
     device::Unit::Choice, kClipNames, false, false,
     "Saturation — off, then a tanh knee from the whole range down to one percent, then a hard "
     "clamp. Placed AFTER the meter, so the 0 dBFS LED lights whenever it is doing something"},
    {"bus_mix", "Bus Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry against wet, around the whole utility. The meter and the clip LED read the utility's own output, ahead of this blend"},
    {"bus_wet_gain", "Bus Wet Gain", 0.0, 1.0, kFaderUnity, false, device::Unit::Fader, {}, true, false,
     "Level of the wet signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
    // THE DM CONTROLS (2026-10-30). Every default is the identity and its stage is skipped there.
    {"bus_mid_trim", "Bus Mid Trim", -kTrimDb, kTrimDb, 0.0, false, device::Unit::Decibel, {}, true, false,
     "Level of the mid (what the two channels share), after Width"},
    {"bus_side_trim", "Bus Side Trim", -kTrimDb, kTrimDb, 0.0, false, device::Unit::Decibel, {}, true, false,
     "Level of the side (where the two channels differ), after Width"},
    {"bus_monitor", "Bus Monitor", 0.0, static_cast<double>(std::size(kMonitorNames) - 1), 0.0, true,
     device::Unit::Choice, kMonitorNames, false, false,
     "What you hear, to check the mix: stereo, the mono sum, one channel, or the side alone. Last in the chain, so the "
     "meter reads it too"},
    {"bus_swap", "Bus Swap", 0.0, 1.0, 0.0, true, device::Unit::Choice, kOffOnNames, false, false,
     "Swap the left and right channels, ahead of every other control"},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE ENABLE'S TWO DECLARATIONS, PINNED TOGETHER — see `fx::owltt` for the full statement of why a
/// descriptor default and a struct initialiser that disagree give a device two different fresh
/// states depending on which road it arrived by.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

/// AND EVERY OTHER DEFAULT PINNED THE SAME WAY, because for this device the identity claim rests on
/// them. A `Settings{}` whose width disagreed with the descriptor's default would make a freshly
/// added Bus Utility change the sound on one road and not the other.
static_assert(Settings {}.gain == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Gain)].def));
static_assert(Settings {}.pan == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Pan)].def));
static_assert(Settings {}.panL == static_cast<float>(kParamDescs[static_cast<size_t>(Param::PanL)].def));
static_assert(Settings {}.panR == static_cast<float>(kParamDescs[static_cast<size_t>(Param::PanR)].def));
static_assert(Settings {}.width == static_cast<float>(kParamDescs[static_cast<size_t>(Param::Width)].def));
static_assert(Settings {}.highpass
              == static_cast<int>(kParamDescs[static_cast<size_t>(Param::Highpass)].def));
static_assert(Settings {}.clip == static_cast<int>(kParamDescs[static_cast<size_t>(Param::Clip)].def));
static_assert(Settings {}.midTrim == static_cast<float>(kParamDescs[static_cast<size_t>(Param::MidTrim)].def));
static_assert(Settings {}.sideTrim == static_cast<float>(kParamDescs[static_cast<size_t>(Param::SideTrim)].def));
static_assert(Settings {}.monitor == static_cast<int>(kParamDescs[static_cast<size_t>(Param::Monitor)].def));
static_assert(Settings {}.swap == (kParamDescs[static_cast<size_t>(Param::Swap)].def >= 0.5));

// INLINE CONSTEXPR IN THE HEADER, NOT A DEFINITION IN THE .cpp. `device_registry.h`'s
// `widestDeviceControls()` walks every registered device AT COMPILE TIME to size the modulation
// matrix stride, so a table that is merely `extern const` is "not usable in a constant expression"
// and the static_asserts there cannot run. It also only breaks the WHOLE build, never a scoped one.

/// THE RENDER PATH.
extern const device::AudioOps kAudioOps;

/// **HOW THE METER'S NUMBERS LEAVE THE AUDIO THREAD.** Called once per block, from `processStereo`,
/// with `Reading::kCount` values in the order `Reading` declares.
///
/// A CALLBACK RATHER THAN A PULL, matching automod's road exactly: the host installs one closure per
/// slot when the device is CONSTRUCTED and the device calls it while it still has the numbers. A pull
/// would have the UI reading the audio thread's storage directly, which this repo does do for the
/// shaper's single drive peak, and which gets less defensible the more words are being read at once.
using ReadingsCallback = void (*)(void* context, const float* values, size_t count);

/// INSTALL IT. `storage` is the ops instance the host constructed; `context` is handed back verbatim.
///
/// **A FUNCTION POINTER AND A CONTEXT, NOT A `std::function`.** automod's equivalent takes one and
/// carries a `static_assert(sizeof <= 16 && alignof <= 8)` beside every install site, because
/// libstdc++ heap-allocates a closure that misses its small-object buffer — **on the audio thread,
/// every block**, in automod's case. Two words cannot miss a buffer they never use, so the hazard
/// does not exist here and no site has to remember to assert it away.
void setReadingsCallback(void* storage, ReadingsCallback callback, void* context) noexcept;

/// TEST ONLY — THE MATRIX THE DSP IS CURRENTLY APPLYING, read out of an ops instance.
///
/// IT EXISTS BECAUSE A PAN LAW IS NOT RECOVERABLE FROM A RENDERED BUFFER. A case asking "did the pan
/// move" by comparing two renders passes whenever ANYTHING differs — including a device applying the
/// wrong law, the right law in the wrong order, or the law to the wrong channel. Every instance of
/// that failure this repo has recorded was a PARTIAL effect surviving a difference check, and a pan
/// law is nothing but partial effects. So the claim is made about the four numbers.
///
/// A FUNCTION RATHER THAN A PUBLISHED STRUCT: the ops state lives in an anonymous namespace, and a
/// test reaching it through a reinterpret_cast would be asserting against a layout nobody promised.
[[nodiscard]] Matrix matrixForTest(const void* storage);
/// TEST ONLY: how many `processStereo` calls followed modulation windows.
[[nodiscard]] size_t windowedCallsForTest(const void* storage);

/// TEST ONLY — the meter this instance is feeding, for the same reason as above: what a meter has
/// measured is not recoverable from the audio it measured.
class Meter;
[[nodiscard]] const Meter& meterForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: this is an effect, not a modulator. Spelled rather than defaulted because
	// -Werror=missing-field-initializers makes every field of this struct a DECISION.
	//
	// UNPAN, THE SIBLING DEVICE, IS THE ONE THAT WILL WANT THIS — its detected image position is
	// exactly a modulation source.
	//
	// **THAT ROAD HAS NOW BEEN DRIVEN, AND THIS COMMENT USED TO SAY IT HAD NOT.** It read: *"that road
	// has never been driven from the fx lane: every `src/fx/*` descriptor says `.outputs = {}` and only
	// `src/mod/*` devices carry `.mod`."* **SKLITTER falsified both halves** — it is an `src/fx/*`
	// device that declares seven `.outputs` and a `.mod` table over the same object
	// (`fx/sklitter/sklitter_types.h`'s `kOutputs`, `sklitter.cpp`'s `kModOps`). So whoever builds UNPAN has a worked example to copy rather than a first crossing to
	// make: the device is constructed once through `kAudioOps` (`buildSlot` reads the audio table's
	// `stateBytes` and never the mod table's), both tables describe the same class, and the mod road
	// must not write what the audio road owns.
	//
	// **ONE THING THE EXAMPLE DOES NOT SETTLE, so read it before copying:** a device that is both makes
	// `Graph::forEachPrecedence` infer "fed by the voice" from "holds an audio device", which is wrong
	// for a dual device seated in a modulator strip — backlog **FQ**. Unreachable today; not yet fixed.
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::Utility,
};

} // namespace feathers::fx::busutil
