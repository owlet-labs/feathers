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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// COMP — a compressor and a limiter in one device, Mode = Compress | Limit.
//
// Basic compressor, limiter and clipper devices are the goal; compress and limit share one
// multimode device.
//
// **FRESH, TEXTBOOK, BY DESIGN** — not a port of the reference's RMS compressor and not a wrap of
// OWLTT's `BandCompressor`. Feed-forward, stereo-linked, a gain computer in dB with a quadratic soft
// knee, and the branching attack/release smoother in the dB domain (Giannoulis, Massberg & Reiss
// 2012).
//
// **LIMIT LOOKS AHEAD, AND THE DEVICE IS LATE BY IT IN BOTH MODES** (backlog LE, 2026-10-10; it was
// "zero latency by design" until devices could report one, backlog JM). Limit runs the shared
// `dsp::LookaheadLimiter` on COMP's own detector (`processSidechain`): its gain ramps down across
// `kLimitLookaheadSeconds` to what the loudest sample coming needs, so no peak is squared off at the
// ceiling, where the old instant attack clipped the rising edge of every new one. Compress is padded by
// the SAME delay — its output is the zero-latency device's, sample for sample, `latencySamplesFor`
// later — so a Mode flip never moves the latency and never restarts the host (the Flanger's TZ lesson,
// `phaser_types.h` `latencySamplesFor`). The final clamp at the ceiling stays: the SC HPF still hides bass the
// audio carries.

namespace feathers::fx::comp {

/// THE PLUGIN'S ID BLOCK IS ASSIGNED IN THIS ORDER, so a device row is index-aligned with this enum.
enum class Param : int {
	Enabled,
	/// COMPRESS, LIMIT OR COMPANDER (backlog QG) — a segmented control on the card, every choice visible on the face.
	Mode,
	Threshold,
	/// 1..20, AND THE TOP OF TRAVEL IS ∞ (`kRatioInfinite`): a brickwall, slope 0 above the knee.
	///
	/// PRINTED "N:1", AND "∞:1" FROM `kRatioInfinite` UP — `kTextOps`, the device's own text hook, so
	/// every surface that prints through a `DeviceControl` agrees rather than only the COMP card. A bare
	/// "20" at the top would be a lie about a slope of zero.
	Ratio,
	/// THE KNEE'S WIDTH IN dB, centred on the threshold. 0 is the hard curve.
	Knee,
	/// log2(seconds). **THE BOTTOM OF THE RANGE IS INSTANT**, device.h's LogSeconds convention — the
	/// card prints "off" there, and `timeSeconds` makes the DSP agree.
	Attack,
	/// log2(seconds). Live in both modes: Limit's attack is its lookahead, its release is this.
	Release,
	/// PEAK OR RMS. RMS is a one-pole mean square with a 10 ms TIME CONSTANT (`kRmsSeconds`), not a
	/// 10 ms boxcar average. Each channel (or each of Mid and Side) has its own detector; `StereoLink`
	/// decides how far each listens to the louder one.
	Detect,
	/// A HIGH-PASS ON WHAT THE DETECTOR HEARS, never on the audio. Off is a true skip. Live in both
	/// modes: bass pumping a limiter is the same problem as bass pumping a compressor.
	ScHpf,
	Makeup,
	/// PARALLEL COMPRESSION. `y = (1-mix)*x + mix*(x*g)`: 0 is EXACTLY the dry signal and 1 is EXACTLY
	/// the wet one.
	Mix,
	/// THE DRIVE INTO THE GAIN COMPUTER, in dB, in EVERY mode (Limit since LE, Compress since MI, Compander since QG), -24..+24 dB since 2026-11-07: the detector and the
	/// audio are pushed alike; the dry is not.
	Drive,
	/// LIMIT ONLY: NO OUTPUT SAMPLE EXCEEDS THIS.
	Ceiling,
	/// **BACKLOG LQ : "compressors incl owltt and limiter should get dual mono with stereo link
	/// control, as well as m/s switches."** Both modes, so neither is gated.
	///
	/// HOW FAR EACH CHANNEL'S DETECTOR LEANS TOWARD THE SHARED ONE, 0..1. The two detector levels are
	/// blended IN dB: `(1 - link) * own + link * shared`, where `shared` is the louder channel's level — the
	/// one detector this device had before LQ. 1 is that linked device exactly (the default, bit for bit);
	/// 0 is two independent compressors. Continuous, so a knob, and modulatable.
	StereoLink,
	/// L/R OR M/S — WHAT THE TWO "CHANNELS" ARE. In M/S the input is encoded `M = (L + R) / 2`,
	/// `S = (L - R) / 2` before detection and gain, and decoded `L = M + S`, `R = M - S` after, so Link
	/// blends Mid and Side. A two-state `SwitchChip`; not modulatable (a topology, not a quantity).
	StereoMode,
	/// **WHAT THE DETECTOR LISTENS TO: THE DEVICE'S OWN INPUT (Self) OR THE PLUGIN'S SIDECHAIN (Side)** (backlog JO).
	/// The audio path is the input either way; only the detector's copy changes. A segmented control, both choices on
	/// the face. COMPRESS AND COMPANDER: Limit holds a ceiling on the audio it passes, so its detector must be that audio.
	Detector,
	/// **COMPANDER ONLY (backlog QG): THE COMPANDER'S RATIO, 1.5..4, the NE570's 2 by default.** Its own key, not Ratio's:
	/// Compress's 1..20/∞ travel is the wrong scale for a power law about Ref.
	CmpRatio,
	/// **COMPANDER ONLY: THE MOST IT CUTS OR BOOSTS, dB** (6..24, 12 = Buckets' 0.25..4 clamp). Bounds how far quiet
	/// passages and tails are lifted.
	CmpRange,
	/// **COMPANDER ONLY: WHICH HALVES, AND WHERE THE LANE SITS**. Compress (the compress half, then the
	/// lane), Expand (the expand half at 1:R, then the lane), Compander (compress → lane → expand: the classic
	/// encode/decode) and Excomper (expand → lane → compress: its own — the lane's devices hear EXAGGERATED dynamics,
	/// and the output is re-levelled after). A segmented control, all four on the face. Stored by label.
	Topology,
	/// **PAIRED TOPOLOGIES ONLY: ONE RATIO, ATTACK AND RELEASE FOR BOTH HALVES (On, the default)**, so Compander and
	/// Excomper stay transparent with an empty lane. Off gives the SECOND half its own three, below. A `SwitchChip`.
	CmpLink,
	/// **THE SECOND HALF'S OWN RATIO, ATTACK AND RELEASE, WHILE UNLINKED** — the expander in Compander, the compressor in
	/// Excomper. Hidden while linked (the honesty rule: nothing reads them then).
	CmpRatio2,
	CmpAttack2,
	CmpRelease2,

	kCount,
};

/// COMPANDER (backlog QG): Buckets' NE570-style compress half, from the
/// shared `dsp/compander.h` — a linear follower and `(env / Ref)^(1/R - 1)`, so quiet is lifted as loud is lowered.
enum class Mode : int { Compress, Limit, Compander, kCount };
inline constexpr const char* kModeNames[] = { "Compress", "Limit", "Compander" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

/// WHICH MODES A GATED CONTROL BELONGS TO, as a mask over `Mode` — the delay's shape.
inline constexpr uint32_t kModeBit(Mode m) {
	return 1u << static_cast<uint32_t>(m);
}
/// **THE HONESTY RULE**: in Limit these change nothing (by design), so the card hides them.
inline constexpr uint32_t kCompressOnly = kModeBit(Mode::Compress);
inline constexpr uint32_t kLimitOnly = kModeBit(Mode::Limit);
inline constexpr uint32_t kCompanderOnly = kModeBit(Mode::Compander);
/// LIVE WHEREVER THERE IS A GAIN COMPUTER WITH A DETECTOR, A MAKEUP AND A MIX: Compress and Compander.
inline constexpr uint32_t kCompressOrCompander = kModeBit(Mode::Compress) | kModeBit(Mode::Compander);

enum class StereoMode : int { LeftRight, MidSide, kCount };
inline constexpr const char* kStereoModeNames[] = { "L/R", "M/S" };
static_assert(std::size(kStereoModeNames) == static_cast<size_t>(StereoMode::kCount));

enum class Detector : int { Self, Side, kCount };
inline constexpr const char* kDetectorNames[] = { "Self", "Side" };
static_assert(std::size(kDetectorNames) == static_cast<size_t>(Detector::kCount));

enum class Detect : int { Peak, Rms, kCount };
inline constexpr const char* kDetectNames[] = { "Peak", "RMS" };
static_assert(std::size(kDetectNames) == static_cast<size_t>(Detect::kCount));

/// THE COMPANDER'S TOPOLOGY (backlog QG). The first half runs on the input, the lane after it, the second half (when
/// there is one) on the lane's output.
enum class Topology : int { Compress, Expand, Compander, Excomper, kCount };
inline constexpr const char* kTopologyNames[] = { "Compress", "Expand", "Compander", "Excomper" };
static_assert(std::size(kTopologyNames) == static_cast<size_t>(Topology::kCount));
/// THE TOPOLOGIES WITH TWO HALVES — where Link, and the second half's own three, mean anything.
inline constexpr uint32_t kPairedTopologies = (1u << static_cast<uint32_t>(Topology::Compander))
                                              | (1u << static_cast<uint32_t>(Topology::Excomper));
[[nodiscard]] constexpr bool isPaired(int topology) {
	return topology == static_cast<int>(Topology::Compander) || topology == static_cast<int>(Topology::Excomper);
}
/// DOES THE FIRST HALF EXPAND? Expand and Excomper start with the expander.
[[nodiscard]] constexpr bool firstExpands(int topology) {
	return topology == static_cast<int>(Topology::Expand) || topology == static_cast<int>(Topology::Excomper);
}

/// THE SIDECHAIN HIGH-PASS LADDER. **INDEX 0 IS `Off` AND IS NOT A FREQUENCY**, for Bus Utility's
/// reason: a filter at its lowest corner is not off, and a continuous knob would draw a number while
/// the stage was skipped. The unit is in the name because a Choice prints nothing else.
inline constexpr const char* kScHpfNames[] = { "Off",   "20Hz",  "40Hz",  "60Hz", "80Hz",
	                                           "120Hz", "200Hz", "300Hz", "500Hz" };
inline constexpr float kScHpfHz[] = { 0.0f, 20.0f, 40.0f, 60.0f, 80.0f, 120.0f, 200.0f, 300.0f, 500.0f };
static_assert(std::size(kScHpfNames) == std::size(kScHpfHz));

/// THE CORNER FOR A SETTING, or 0 when the stage is off. **0 IS THE REFUSAL, NOT A FREQUENCY.**
[[nodiscard]] constexpr float scHpfHz(int index) {
	return (index <= 0 || static_cast<size_t>(index) >= std::size(kScHpfHz)) ? 0.0f
	                                                                          : kScHpfHz[static_cast<size_t>(index)];
}

/// THE RANGES, NAMED ONCE — the descriptor and `stateOf`'s clamps read the same numbers.
inline constexpr double kAttackMinSeconds = 0.0001;
inline constexpr double kAttackMaxSeconds = 0.1;
inline constexpr double kAttackDefSeconds = 0.01;
/// 1 ms, BELOW THE DESIGN'S 5 ms, ON PURPOSE: the bottom of a LogSeconds range is "off" (instant
/// release, which distorts) and the card prints it so. Putting the bottom at 1 ms keeps "off" honest
/// and leaves 5 ms reachable just above it (decided 2026-09-25).
inline constexpr double kReleaseMinSeconds = 0.001;
/// THE RATIO AT WHICH THE CURVE BECOMES A BRICKWALL — the top of the Ratio control's travel. The
/// design: Ratio runs 1…20, with ∞ at the top of travel.
inline constexpr float kRatioInfinite = 20.0f;

/// THE SLOPE ABOVE THE KNEE, 1/R — and exactly 0 at `kRatioInfinite` and above.
[[nodiscard]] inline float slopeFor(float ratio) {
	return ratio >= kRatioInfinite ? 0.0f : 1.0f / ratio;
}
inline constexpr double kReleaseMaxSeconds = 2.0;
inline constexpr double kReleaseDefSeconds = 0.1;
/// THE RMS DETECTOR'S TIME CONSTANT — the design's "~10 ms window", realised as a one-pole.
inline constexpr double kRmsSeconds = 0.010;

/// **LIMIT'S LOOKAHEAD, FIXED — NOT A CONTROL** (backlog LE): a control that moved it would move the
/// latency, and a moving latency restarts the host. 1.5 ms is Featherverb's wet limiter's
/// (`kWetLookaheadSeconds`, backlog LD), the length measured there to take the squaring off new peaks.
/// In M/S it is SPLIT in series, half on Mid and Side and the rest on the L/R stage after decoding
/// (`midSideLookaheadFor`), so the total, and so the latency, is the same in every mode.
/// **DECIDED 2026-10-10**: 1.5 ms, round(0.0015 · rate), constant in both modes; the M/S
/// split (option (a), 0.75 + 0.75 ms); the release kept in dB. The length rests on LD's measurement — Featherverb's
/// instant-attack wet limiter stepped its gain by up to 2.8 dB in one sample and clicked 30–280 times a second, and
/// this lookahead took that off. Measured here against the zero-latency Limit: backlog LE.
inline constexpr double kLimitLookaheadSeconds = 0.0015;

/// **THE DEVICE'S LATENCY, IN BOTH MODES AND ENABLED OR NOT**: Limit's lookahead at this rate, and
/// Compress padded to it. Never under 2, so M/S's two stages each have at least one sample.
[[nodiscard]] inline uint32_t latencySamplesFor(double sampleRate) noexcept {
	const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
	return static_cast<uint32_t>(std::max(2L, std::lround(kLimitLookaheadSeconds * rate)));
}
/// M/S LIMIT'S MID/SIDE STAGE'S SHARE OF THE LOOKAHEAD; the L/R stage after decoding has the rest.
[[nodiscard]] inline uint32_t midSideLookaheadFor(uint32_t latency) noexcept {
	return latency / 2;
}

struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — every effect defaults to on when it is
	/// added. Pinned to the descriptor below.
	bool enabled = true;
	int mode = static_cast<int>(Mode::Compress);
	float thresholdDb = -18.0f;
	float ratio = 4.0f;
	float kneeDb = 6.0f;
	/// log2(seconds), as the control stores it. `timeSeconds` turns it into a time.
	float attack = static_cast<float>(-6.643856189774724); // log2(0.01)
	float release = static_cast<float>(-3.321928094887362); // log2(0.1)
	int detect = static_cast<int>(Detect::Peak);
	/// INDEX INTO `kScHpfNames`. 0 is Off.
	int scHpf = 0;
	float makeupDb = 0.0f;
	float mix = 1.0f;
	float driveDb = 0.0f;
	float ceilingDb = -1.0f;
	/// 1 IS THE LINKED DEVICE THIS WAS BEFORE LQ — see `Param::StereoLink`.
	float stereoLink = 1.0f;
	int stereoMode = static_cast<int>(StereoMode::LeftRight);
	/// SELF IS THE DEVICE THIS WAS BEFORE JO, bit for bit.
	int detector = static_cast<int>(Detector::Self);
	/// COMPANDER (backlog QG): the ratio about Ref, and the range in dB.
	float cmpRatio = 2.0f;
	float cmpRangeDb = 12.0f;
	int topology = static_cast<int>(Topology::Compress);
	bool cmpLink = true;
	float cmpRatio2 = 2.0f;
	float cmpAttack2 = static_cast<float>(-6.643856189774724); // log2(0.01), Attack's default
	float cmpRelease2 = static_cast<float>(-3.321928094887362); // log2(0.1), Release's default

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// Row: the serialized form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// WHAT THE METER PUBLISHES, IN THIS ORDER — the contract between `processStereo` and the card.
enum class Reading : size_t {
	/// LINEAR sample peak of the input (both channels), held 50 ms — before Limit's drive.
	InPeak,
	/// LINEAR sample peak of the output, held 50 ms.
	OutPeak,
	/// GAIN REDUCTION as a POSITIVE number of dB, held 50 ms. 0 is none. The DEEPER of the two channels'
	/// (backlog LQ), which is the one gain there was before the channels came apart.
	GainReduction,
	/// EACH CHANNEL'S OWN GAIN REDUCTION (backlog LQ), positive dB, held 50 ms: L then R, or M then S in
	/// M/S. In M/S Limit each includes the L/R stage that holds the ceiling after decoding.
	GainReductionA,
	GainReductionB,
	/// 1 WHILE SOMETHING SOUNDS ON THE PLUGIN'S SIDECHAIN (`device::SidechainPresence`), else 0. The card prints
	/// "sidechain silent" from it while Detector is Side. Not a level: a flag.
	SidechainSounding,
	kCount,
};

/// Giannoulis/Massberg/Reiss 2012, eq. 4: the static curve with a quadratic knee, in dB.
///
/// **THE KNEE IS C1 BY CONSTRUCTION**: at `over = -W/2` the quadratic term is 0 with zero slope, and
/// at `over = +W/2` it is `(1/R - 1)*W/2`, which is where the above-knee line sits, with slope 1/R.
/// **1/R IS `slopeFor(R)`**, so the top of the Ratio travel is a true brickwall (slope 0).
[[nodiscard]] inline float staticCurveDb(float xDb, float thresholdDb, float ratio, float kneeDb) {
	const float over = xDb - thresholdDb;
	const float slope = slopeFor(ratio);
	if (2.0f * over < -kneeDb) return xDb;
	if (kneeDb > 0.0f && 2.0f * std::fabs(over) <= kneeDb) {
		const float a = over + 0.5f * kneeDb;
		return xDb + (slope - 1.0f) * a * a / (2.0f * kneeDb);
	}
	return thresholdDb + over * slope;
}

/// **THE COMPANDER'S STATIC CURVE, IN dB** (backlog QG): `dsp/compander.h`'s law at steady state — a line of slope 1/R
/// through Ref, its gain clamped to +-range. For the card's plot; the DSP runs the linear law itself.
[[nodiscard]] inline float companderCurveDb(float xDb, float refDb, float ratio, float rangeDb) {
	const float g = (1.0f / std::max(ratio, 1.0f) - 1.0f) * (xDb - refDb);
	return xDb + std::clamp(g, -rangeDb, rangeDb);
}

/// **THE EXPANDER'S STATIC CURVE, IN dB** (backlog QG): 1:R about Ref — a gain of `(R - 1)(x - Ref)`, clamped to +-range.
/// The exact inverse of `companderCurveDb` at the same ratio and range (the doc has the algebra).
[[nodiscard]] inline float expanderCurveDb(float xDb, float refDb, float ratio, float rangeDb) {
	const float g = (std::max(ratio, 1.0f) - 1.0f) * (xDb - refDb);
	return xDb + std::clamp(g, -rangeDb, rangeDb);
}

/// One-pole coefficient for a time constant; 0 seconds is instant.
[[nodiscard]] inline float poleFor(double seconds, double sampleRate) {
	return seconds <= 0.0 ? 0.0f : static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
}

/// A LogSeconds CONTROL'S VALUE AS A TIME, WITH ITS MINIMUM MEANING NONE. device.h: *"THE MINIMUM OF
/// A LogSeconds RANGE MEANS 'NO TIME AT ALL'"*, and `value_box.cpp` prints "off" there — so the DSP
/// must be instant there too, or the card promises a stage the device still runs.
[[nodiscard]] inline double timeSeconds(float log2Seconds, double minSeconds) {
	const double s = std::exp2(static_cast<double>(log2Seconds));
	return s <= minSeconds * (1.0 + 1e-6) ? 0.0 : s;
}

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "dynamics", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "comp",
	.name = "Compressor",
	.hasCard = true,
	/// **A WINDOW SINCE BACKLOG QG**: the Compander's lane needs a rail to edit it (the Delay's FEEDBACK FX rail, the same
	/// component), and the window carries every control the card does, the second half's three and Link included.
	.hasExpanded = true,
	/// ONE DETECTOR ON THE BUS. A per-voice road is not implemented, so none is claimed.
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

inline constexpr int16_t kModeControl = static_cast<int16_t>(Param::Mode);
inline constexpr int16_t kTopologyControl = static_cast<int16_t>(Param::Topology);
inline constexpr int16_t kLinkControl = static_cast<int16_t>(Param::CmpLink);

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    {.key = "comp_enabled", .name = "Comp Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the compressor is in the chain"},
    {.key = "comp_mode", .name = "Comp Mode", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kModeNames,
     .tooltip = "Compress: threshold, ratio and knee shape the level. Limit: nothing passes the ceiling, "
                "looking 1.5 ms ahead so the gain is down before a peak arrives. Compander: the bucket-brigade chip's "
                "compressor, turning loud down and quiet up about Ref"},
    {.key = "comp_threshold", .name = "Comp Threshold", .min = -60.0, .max = 0.0, .def = -18.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "The level above which the signal is turned down. In Compander it is Ref, the level left alone: "
                "louder is turned down, quieter is turned up",
     .gateControl = kModeControl, .gateMask = kCompressOrCompander},
    {.key = "comp_ratio", .name = "Comp Ratio", .min = 1.0, .max = 20.0, .def = 4.0, .stepped = false, .unit = device::Unit::Number,
     .names = {}, .modulatable = true,
     .tooltip = "How hard it turns down above the threshold — 4 means 4 dB in gives 1 dB out; "
                "the top of the travel is infinite, a brickwall",
     .gateControl = kModeControl, .gateMask = kCompressOnly},
    {.key = "comp_knee", .name = "Comp Knee", .min = 0.0, .max = 24.0, .def = 6.0, .stepped = false, .unit = device::Unit::Decibel,
     .names = {}, .modulatable = true,
     .tooltip = "How gradually compression begins around the threshold — 0 is a hard corner",
     .gateControl = kModeControl, .gateMask = kCompressOnly},
    {.key = "comp_attack", .name = "Comp Attack", .min = -13.287712379549449, .max = -3.321928094887362, .def = -6.643856189774724,
     .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "How fast gain reduction arrives — off at the bottom is instant",
     .gateControl = kModeControl, .gateMask = kCompressOrCompander},
    {.key = "comp_release", .name = "Comp Release", .min = -9.965784284662087, .max = 1.0, .def = -3.321928094887362, .stepped = false,
     .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "How fast gain reduction lets go (in Compander, how fast the level follower falls) — off at the bottom is instant"},
    {.key = "comp_detect", .name = "Comp Detect", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kDetectNames,
     .tooltip = "Peak follows every sample; RMS follows the power with a 10 ms time constant",
     .gateControl = kModeControl, .gateMask = kCompressOrCompander},
    {.key = "comp_sc_hpf", .name = "Comp SC HPF", .min = 0.0, .max = static_cast<double>(std::size(kScHpfNames) - 1), .def = 0.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kScHpfNames,
     .tooltip = "A high-pass on what the detector hears, not on the audio — keeps bass from pumping the "
                "gain. Off is a true bypass"},
    {.key = "comp_makeup", .name = "Comp Makeup", .min = -12.0, .max = 24.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "Gain after compression, to win back the level it took", .gateControl = kModeControl,
     .gateMask = kCompressOrCompander},
    {.key = "comp_mix", .name = "Comp Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "Parallel compression — 0% is exactly the dry signal", .gateControl = kModeControl,
     .gateMask = kCompressOrCompander},
    // BACKLOG MI: live in BOTH modes, so ungated. The key is kept: the number still means dB in front of the gain computer.
    // AND NEGATIVE SINCE 2026-11-07 (compressor drive: lets let it go negative as well): -24..+24, the key kept — the
    // number still means dB in front of the gain computer, so a stored value means what it meant. Centred: 0 is its rest.
    {.key = "comp_drive", .name = "Comp Drive", .min = -24.0, .max = 24.0, .def = 0.0, .stepped = false, .unit = device::Unit::Decibel,
     .names = {}, .modulatable = true,
     .tooltip = "Gain in front of the compressor, the limiter or the compander — how hard the signal is pushed into it; "
                "negative backs it off. The dry is never driven",
     .centred = true},
    {.key = "comp_ceiling", .name = "Comp Ceiling", .min = -24.0, .max = 0.0, .def = -1.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "No output sample passes this level", .gateControl = kModeControl, .gateMask = kLimitOnly},
    // BACKLOG LQ. Live in BOTH modes, so neither carries a gate.
    {.key = "comp_stereo_link", .name = "Comp Stereo Link", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How far each channel's detector follows the louder one — 100% turns both down together, "
                "0% compresses each on its own"},
    {.key = "comp_stereo_mode", .name = "Comp Stereo Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kStereoModeNames,
     .tooltip = "L/R compresses left and right; M/S compresses the middle and the sides, then decodes back"},
    // BACKLOG JO. Compress only: Limit's ceiling is a promise about its own audio, so its detector is never the sidechain.
    {.key = "comp_detector", .name = "Comp Detector", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kDetectorNames,
     .tooltip = "Self: the compressor listens to its own input. Side: it listens to the plugin's Sidechain input "
                "instead, and turns its own input down by what the sidechain would have needed",
     .gateControl = kModeControl, .gateMask = kCompressOrCompander},
    // BACKLOG QG — the Compander's own two. Their own keys: Compress's Ratio means something else at 20.
    {.key = "comp_cmp_ratio", .name = "Comp Compander Ratio", .min = 1.5, .max = 4.0, .def = 2.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "The compander's ratio about Ref — 2 is the bucket-brigade chip's: 4 dB of change in gives 2 out, "
                "louder or quieter",
     .gateControl = kModeControl, .gateMask = kCompanderOnly},
    {.key = "comp_cmp_range", .name = "Comp Compander Range", .min = 6.0, .max = 24.0, .def = 12.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "The most the compander cuts or boosts — less keeps quiet passages and tails from being lifted far",
     .gateControl = kModeControl, .gateMask = kCompanderOnly},
    {.key = "comp_cmp_topology", .name = "Comp Compander Topology", .min = 0.0, .max = 3.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kTopologyNames,
     .tooltip = "Compress: squeeze, then the lane. Expand: widen, then the lane. Compander: squeeze, the lane, then "
                "restore — the lane hears an even level. Excomper: widen, the lane, then re-level — the lane hears the "
                "playing exaggerated",
     .gateControl = kModeControl, .gateMask = kCompanderOnly},
    {.key = "comp_cmp_link", .name = "Comp Compander Link", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "On: one Ratio, Attack and Release for both halves, so an empty lane gives back what went in. Off: the "
                "second half gets its own, starting where the first half's are",
     .gateControl = kTopologyControl, .gateMask = kPairedTopologies},
    {.key = "comp_cmp_ratio2", .name = "Comp Compander Ratio 2", .min = 1.5, .max = 4.0, .def = 2.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "The second half's ratio, while unlinked",
     .gateControl = kLinkControl, .gateMask = 1u},
    {.key = "comp_cmp_attack2", .name = "Comp Compander Attack 2", .min = -13.287712379549449, .max = -3.321928094887362,
     .def = -6.643856189774724, .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "The second half's attack, while unlinked — off at the bottom is instant",
     .gateControl = kLinkControl, .gateMask = 1u},
    {.key = "comp_cmp_release2", .name = "Comp Compander Release 2", .min = -9.965784284662087, .max = 1.0,
     .def = -3.321928094887362, .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "The second half's release, while unlinked — off at the bottom is instant",
     .gateControl = kLinkControl, .gateMask = 1u},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// EVERY DEFAULT PINNED TO THE DESCRIPTOR, so a fresh state cannot depend on which road it arrived by
/// (`fx::owltt` has the full statement).
namespace detail {
[[nodiscard]] constexpr float defOf(Param p) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
}
} // namespace detail
static_assert(Settings {}.enabled == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));
static_assert(Settings {}.mode == static_cast<int>(detail::defOf(Param::Mode)));
static_assert(Settings {}.thresholdDb == detail::defOf(Param::Threshold));
static_assert(Settings {}.ratio == detail::defOf(Param::Ratio));
static_assert(Settings {}.kneeDb == detail::defOf(Param::Knee));
static_assert(Settings {}.attack == detail::defOf(Param::Attack));
static_assert(Settings {}.release == detail::defOf(Param::Release));
static_assert(Settings {}.detect == static_cast<int>(detail::defOf(Param::Detect)));
static_assert(Settings {}.scHpf == static_cast<int>(detail::defOf(Param::ScHpf)));
static_assert(Settings {}.makeupDb == detail::defOf(Param::Makeup));
static_assert(Settings {}.mix == detail::defOf(Param::Mix));
static_assert(Settings {}.driveDb == detail::defOf(Param::Drive));
static_assert(Settings {}.ceilingDb == detail::defOf(Param::Ceiling));
static_assert(Settings {}.stereoLink == detail::defOf(Param::StereoLink));
static_assert(Settings {}.stereoMode == static_cast<int>(detail::defOf(Param::StereoMode)));
static_assert(Settings {}.detector == static_cast<int>(detail::defOf(Param::Detector)));
static_assert(Settings {}.cmpRatio == detail::defOf(Param::CmpRatio));
static_assert(Settings {}.cmpRangeDb == detail::defOf(Param::CmpRange));
static_assert(Settings {}.topology == static_cast<int>(detail::defOf(Param::Topology)));
static_assert(Settings {}.cmpLink == (detail::defOf(Param::CmpLink) >= 0.5f));
static_assert(Settings {}.cmpRatio2 == detail::defOf(Param::CmpRatio2));
static_assert(Settings {}.cmpAttack2 == detail::defOf(Param::CmpAttack2));
static_assert(Settings {}.cmpRelease2 == detail::defOf(Param::CmpRelease2));
// THE SECOND HALF'S RANGES AND DEFAULTS ARE THE FIRST'S, so unlinking at the defaults changes nothing.
static_assert(kParamDescs[static_cast<size_t>(Param::CmpRatio2)].def == kParamDescs[static_cast<size_t>(Param::CmpRatio)].def);
static_assert(kParamDescs[static_cast<size_t>(Param::CmpAttack2)].min == kParamDescs[static_cast<size_t>(Param::Attack)].min);
static_assert(kParamDescs[static_cast<size_t>(Param::CmpRelease2)].max == kParamDescs[static_cast<size_t>(Param::Release)].max);

/// **IS THIS CONTROL LIVE, ITS GATE'S GATE INCLUDED?** (backlog QG) The descriptor carries one gate per control, and the
/// Compander's controls nest: the second half's three are gated on Link, Link on Topology, Topology on Mode. A control is
/// live when its gate says so AND its gate control is itself live, so Ratio 2 is never shown in Limit just because Link
/// happens to be off. `field(i)` reads control i's current value. The card and the window both ask this one function.
template <typename Field>
[[nodiscard]] bool liveIn(Param p, const Field& field) {
	for (int16_t at = static_cast<int16_t>(p); at >= 0;) {
		const device::ParamDesc& d = kParamDescs[static_cast<size_t>(at)];
		if (d.gateControl < 0) {
			return true;
		}
		if (!device::relevantFor(d, field(static_cast<size_t>(d.gateControl)))) {
			return false;
		}
		at = d.gateControl;
	}
	return true;
}

/// THE RENDER PATH.
extern const device::AudioOps kAudioOps;
/// **AND A CONTAINER (backlog QG): THE COMPANDER'S LANE** — one lazy `rack::InnerChain` between the halves, the Delay's
/// feedback lane's component (`rack::LazyLane`). The holder lives above the rack (`comp_ops.cpp`, the Delay's place).
extern const device::ContainerOps kContainerOps;

/// HOW RATIO PRINTS — "4.0:1", and "∞:1" at and above `kRatioInfinite`, the same comparison
/// `slopeFor` makes. Every other control falls through to its declared unit.
extern const device::TextOps kTextOps;

/// TEST ONLY — THE SMOOTHED GAIN (dB, <= 0) THE DSP IS APPLYING, and the detector's last level in dB.
/// Neither is recoverable from a rendered buffer that also carries makeup, mix and a clamp.
/// Since LQ the gain is the DEEPER of the two channels'; the detector level is the SHARED one (the louder
/// channel's), which is what the device's one detector read before.
[[nodiscard]] float gainDbForTest(const void* storage);
[[nodiscard]] float detectorDbForTest(const void* storage);
/// TEST ONLY (backlog LQ) — ONE CHANNEL'S smoothed gain in dB: 0 is L (or M), 1 is R (or S). The claim
/// "Link 0 leaves R untouched" is about this number, which a buffer that also carries the input cannot give.
[[nodiscard]] float gainDbForTest(const void* storage, size_t channel);
/// TEST ONLY — M/S Limit's L/R stage, the gain in dB (<= 0) that holds the ceiling after decoding.
[[nodiscard]] float ceilingStageDbForTest(const void* storage);
/// TEST ONLY (backlog LE) — the loudest Limit sample BEFORE the final clamp, over the ceiling it entered with, as a
/// running maximum since construct or `reset`. At most 1 when the lookahead stages hold the ceiling as a GAIN; the
/// clamp can only make the output look as if they had.
[[nodiscard]] float peakOverCeilingForTest(const void* storage);
/// TEST ONLY — the linear gain (makeup included) Compress applied to the last sample, so a case can
/// state that Mix 1 is EXACTLY `x * g`.
[[nodiscard]] float lastGainForTest(const void* storage);
/// TEST ONLY (backlog QG) — the Compander follower's envelope (LINEAR) for channel 0 (L or M) or 1 (R or S). The
/// attack and release claims are about this number; the gain it sets is `gainDbForTest(storage, ch)`, signed.
[[nodiscard]] float envelopeForTest(const void* storage, size_t channel);
/// TEST ONLY (backlog QG) — the SECOND half's follower (paired topologies), and each half's last gain in signed dB per
/// channel. The identity claim (Compander and Excomper with an empty lane) is that these two sum to ~0 at steady state.
[[nodiscard]] float secondEnvelopeForTest(const void* storage, size_t channel);
[[nodiscard]] float firstHalfDbForTest(const void* storage, size_t channel);
[[nodiscard]] float secondHalfDbForTest(const void* storage, size_t channel);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: an effect, not a modulator. Spelled because every field of this struct is a decision.
	.outputs = {},
	.audio = &kAudioOps,
	.container = &kContainerOps,
	.text = &kTextOps,
	.category = device::Category::Dynamics,
};

} // namespace feathers::fx::comp
