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

// CLIP — a clipper with five characters and a 4x oversampling switch.
//
// The clipper stands alone, with character modes and an oversampling toggle; the control once
// called Detail is named Texture.
//
// **EVERY CURVE WORKS IN THE NORMALISED DOMAIN** `u = x * drive / ceiling`, and returns a value in
// [-1, 1] that the device scales back by the ceiling. So "inside the ceiling" is "inside ±1" for every
// mode, and each curve can be tested on its own without a gain in sight.

namespace feathers::fx::clip {

/// THE PLUGIN'S ID BLOCK IS ASSIGNED IN THIS ORDER, so a device row is index-aligned with this enum.
enum class Param : int {
	Enabled,
	/// WHICH TOP — a value box with a menu on the card, so every mode is visible before one is applied.
	Mode,
	/// dB INTO THE CLIPPER. -24 to +24: below 0 the signal reaches the curve quieter, so it clips less.
	Drive,
	/// WHERE THE TOP IS, in dBFS. With Oversample off no output sample passes it; with 4x the ceiling holds
	/// at the oversampled rate and the downsampled output can exceed it by a few dB (by design: documented,
	/// no clamp — the measured numbers are at the `os.process` call in `clip.cpp`).
	Ceiling,
	/// A TRIM AFTER THE CLIPPER, in dB.
	Output,
	/// OFF OR 4x. 4x is the default: a clipper is an alias generator, and the switch is for CPU.
	Oversample,
	/// THE FRACTION OF THE RANGE BELOW THE CEILING THAT BENDS, 0..1. Below `1 - knee` every mode that
	/// reads it is EXACTLY the input. In Bits, 0 is a hard clamp (no region to quantise).
	Knee,
	/// TEXTURE ONLY: log2(Hz) of the high-pass on what clipping removed. 500 Hz .. 12 kHz.
	Freq,
	/// TEXTURE ONLY: how much of that high-passed detail is carved back into the top.
	Foldback,
	/// GRIT ONLY: how much noise is carved into the top, scaled by the overshoot.
	Grit,
	/// FOLD ONLY: 0 is a clamp, 1 is a full triangle fold of the overshoot.
	Depth,
	/// BITS ONLY: the region above the knee quantised to 1 or 2 bits.
	Bits,
	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR), supplied and applied by the slot: the device
	/// never reads either, and the card does not draw them (the site's head row does). Mix is a crossfade (100% the
	/// wet), the wet Gain a fader position (unity at 0.5) on the wet only. Both defaults are the Clipper as it was, to
	/// the bit. They wait on `kOversampleLatency`: until 4x was declared, the slot's dry combed against the wet.
	Mix,
	WetGain,

	kCount,
};

enum class Mode : int { Soft, Texture, Grit, Fold, Bits, kCount };
inline constexpr const char* kModeNames[] = { "Soft", "Texture", "Grit", "Fold", "Bits" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

/// WHICH MODES A GATED CONTROL BELONGS TO, as a mask over `Mode` — the delay's shape.
inline constexpr uint32_t kModeBit(Mode m) {
	return 1u << static_cast<uint32_t>(m);
}
/// **THE HONESTY RULE**: each gated control is declared live exactly in the modes whose curve reads it.
inline constexpr uint32_t kKneeModes =
    kModeBit(Mode::Soft) | kModeBit(Mode::Texture) | kModeBit(Mode::Grit) | kModeBit(Mode::Bits);
inline constexpr uint32_t kTextureOnly = kModeBit(Mode::Texture);
inline constexpr uint32_t kGritOnly = kModeBit(Mode::Grit);
inline constexpr uint32_t kFoldOnly = kModeBit(Mode::Fold);
inline constexpr uint32_t kBitsOnly = kModeBit(Mode::Bits);

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kOversampleNames[] = { "Off", "4x" };
inline constexpr const char* kBitsNames[] = { "1 bit", "2 bit" };

/// THE TEXTURE HIGH-PASS RANGE, NAMED ONCE — the descriptor and `stateOf` read the same numbers.
inline constexpr double kFreqMinLog2 = 8.965784284662087;  // log2(500)
inline constexpr double kFreqMaxLog2 = 13.550746785383243; // log2(12000)
inline constexpr double kFreqDefLog2 = 11.550746785383243; // log2(3000)

/// **A FEEL CONSTANT, NOT A DERIVED ONE** — how deep full Grit carves at a full overshoot, as a
/// fraction of the ceiling. Chosen so 100 % is plainly audible without the top collapsing into noise;
/// listed for tuning by ear, and nothing else depends on its value.
inline constexpr float kGritScale = 0.25f;

struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — every effect defaults to on when it is added.
	/// Pinned to the descriptor below.
	bool enabled = true;
	int mode = static_cast<int>(Mode::Soft);
	float driveDb = 0.0f;
	float ceilingDb = 0.0f;
	float outputDb = 0.0f;
	bool oversample = true;
	float knee = 0.1f;
	/// log2(Hz), as the control stores it.
	float freq = static_cast<float>(kFreqDefLog2);
	float foldback = 0.5f;
	float grit = 0.5f;
	float depth = 1.0f;
	/// INDEX INTO `kBitsNames`: 0 is 1 bit, 1 is 2 bits.
	int bits = 0;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// Row: the serialized form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// **4x OVERSAMPLING DELAYS THE OUTPUT BY THIS MANY HOST SAMPLES, AND THE DEVICE SAYS SO** (backlog NR
/// 2026-10-28). `Oversampler4x` is two IIR halfband stages each way, so its delay is a property of the filters and
/// not of the rate. MEASURED from its impulse response (`test_fx_clip`, "NR: the declared latency is the oversampler's
/// measured delay"): group delay 5.636 samples at 10-100 Hz, 5.65 at 1 kHz, 5.91 at 5 kHz, 6.90 at 10 kHz, and the
/// impulse peaks at sample 6. Six is the nearest whole delay, and it is the one under which a 50% Mix against a
/// dry delayed by it stays within 0.15 dB to 12 kHz (no delay: -10.5 dB at 5 kHz). The residue above that is the
/// halfbands' own phase, not a misalignment: no integer delay matches an IIR decimator near its band edge.
/// **THE WHOLE DEVICE DELAYS BY IT WITH 4x ON, enable or not** (the latency contract): the enable crossfade's dry
/// and the disabled pass-through read the same 6-sample delay, so switching the device off moves nothing in time.
inline constexpr uint32_t kOversampleLatency = 6;

/// WHAT THE METER PUBLISHES, IN THIS ORDER — the contract between `processStereo` and the card.
enum class Reading : size_t {
	/// LINEAR sample peak of the INCOMING signal (both channels), held 50 ms — BEFORE Drive, as COMP's is
	/// before Limit's drive. The disabled pass-through pushes the same quantity, so the bar does not jump
	/// when the device is toggled.
	InPeak,
	/// LINEAR peak of the output, held 50 ms.
	OutPeak,
	/// 0 OR 1, held 50 ms: the driven input crossed where the current mode starts acting (`onset`).
	/// Metered AHEAD of the curve, so it lights exactly when the clipper has something to do (Bus
	/// Utility's rule) — and with 4x on the four oversampled frames the curve runs on, so an
	/// inter-sample peak past the onset lights it too.
	Over,
	kCount,
};

/// WHERE THE CURRENT MODE STARTS ACTING, in the normalised domain: `1 - knee` for the knee modes, 1 for
/// Fold (which is exact below ±1 whatever its depth).
[[nodiscard]] inline float onset(int mode, float knee) {
	return mode == static_cast<int>(Mode::Fold) ? 1.0f : 1.0f - std::clamp(knee, 0.0f, 1.0f);
}

/// FOLD: the overshoot folds back as a triangle instead of flattening. EXACT below ±1 — the early
/// return is what makes it exact rather than a triangle that happens to agree to rounding.
[[nodiscard]] inline float foldCurve(float u, float depth) {
	if (std::fabs(u) <= 1.0f) return u;
	float v = u + 1.0f;
	v -= 4.0f * std::floor(v * 0.25f); // [0, 4)
	const float folded = v < 2.0f ? v - 1.0f : 3.0f - v;
	return (1.0f - depth) * std::clamp(u, -1.0f, 1.0f) + depth * folded;
}

/// BITS: the region above the knee quantised to 2 (1 bit) or 4 (2 bits) levels — a stepped top.
/// **AT KNEE 0 IT IS A HARD CLAMP:** `t = 1`, so the quantised region is empty and everything past ±1
/// lands on ±1. That is why the card's Knee is live in Bits.
[[nodiscard]] inline float bitsCurve(float u, float knee, int bits /*1 or 2*/) {
	const float t = 1.0f - std::clamp(knee, 0.0f, 1.0f);
	const float a = std::fabs(u);
	if (a <= t) return u;
	const int levels = 1 << bits; // 2 or 4
	const float frac = (std::min(a, 1.0f) - t) / std::max(1.0f - t, 1e-9f);
	const int q = std::min(static_cast<int>(frac * static_cast<float>(levels)), levels - 1);
	const float stepped = t + (1.0f - t) * static_cast<float>(q) / static_cast<float>(levels - 1);
	return u < 0.0f ? -stepped : stepped;
}

/// TEXTURE, the Au5 method: carve |HP(u - c)| — the detail clipping removed, above Freq — into the top.
/// **THE CARVE STOPS AT ZERO, so `|y| <= |c|` HOLDS STRUCTURALLY** and no safety clamp is needed (a
/// clamp would make the ceiling test vacuous). `|detail|`, not `detail`: the form that honours it.
[[nodiscard]] inline float textureCurve(float c, float detail, float foldback) {
	const float carved = std::max(0.0f, std::fabs(c) - std::fabs(detail) * foldback);
	return c < 0.0f ? -carved : carved;
}

/// GRIT: noise carved into the top, scaled by the overshoot past the knee. `noise01` in [0, 1). Below
/// the knee `over` is 0 and the clipped value comes back EXACTLY.
[[nodiscard]] inline float gritCurve(float c, float u, float knee, float grit, float noise01) {
	const float over = std::min(std::max(0.0f, std::fabs(u) - (1.0f - knee)), 1.0f);
	const float carved = std::max(0.0f, std::fabs(c) - kGritScale * grit * over * noise01);
	return c < 0.0f ? -carved : carved;
}

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "dynamics", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "clip",
	.name = "Clipper",
	.hasCard = true,
	/// NO EXPANDED VIEW: the mode's two or three controls and four globals fit a card.
	.hasExpanded = false,
	/// ONE CLIPPER ON THE BUS. A per-voice road is not implemented, so none is claimed.
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// THE SLOT BLENDS THESE (backlog NR): a plain crossfade around the device, which hands back the wet.
	// With Oversample on the device declares `kOversampleLatency`, so the slot delays the dry by it.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

inline constexpr int16_t kModeControl = static_cast<int16_t>(Param::Mode);

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    {.key = "clip_enabled", .name = "Clip Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the clipper is in the chain"},
    {.key = "clip_mode", .name = "Clip Mode", .min = 0.0, .max = static_cast<double>(std::size(kModeNames) - 1), .def = 0.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kModeNames,
     .tooltip = "What the top of the wave becomes — a soft shoulder, carved-back detail, grit, a fold, or "
                "steps"},
    {.key = "clip_drive", .name = "Clip Drive", .min = -24.0, .max = 24.0, .def = 0.0, .stepped = false, .unit = device::Unit::Decibel,
     .names = {}, .modulatable = true, .tooltip = "Gain into the clipper — below 0 dB it reaches the curve quieter, so it clips less"},
    {.key = "clip_ceiling", .name = "Clip Ceiling", .min = -24.0, .max = 0.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "Where the top is. Exact with Oversample off; with 4x oversampling the output can exceed "
                "it by a few dB after downsampling"},
    {.key = "clip_output", .name = "Clip Output", .min = -24.0, .max = 12.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true, .tooltip = "Trim after the clipper"},
    {.key = "clip_oversample", .name = "Clip Oversample", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOversampleNames,
     .tooltip = "Run the clipper at four times the rate, so its harmonics do not fold back as aliases. "
                "With 4x the output can exceed the ceiling by a few dB after downsampling; the ceiling is "
                "exact with Oversample off. 4x also delays the output by 6 samples, which the host is told "
                "about, so a Mix under 100% stays aligned. Switching it may click"},
    {.key = "clip_knee", .name = "Clip Knee", .min = 0.0, .max = 1.0, .def = 0.1, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "How much of the range below the ceiling bends — below it the signal is untouched. At 0% the "
                "bend is a hard clamp, in every mode that has a Knee",
     .gateControl = kModeControl, .gateMask = kKneeModes},
    {.key = "clip_freq", .name = "Clip Freq", .min = kFreqMinLog2, .max = kFreqMaxLog2, .def = kFreqDefLog2, .stepped = false,
     .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "Texture: the detail clipping removed above this frequency is carved back into the top",
     .gateControl = kModeControl, .gateMask = kTextureOnly},
    {.key = "clip_foldback", .name = "Clip Foldback", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Texture: how much of that detail is carved back", .gateControl = kModeControl,
     .gateMask = kTextureOnly},
    {.key = "clip_grit", .name = "Clip Grit", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "Grit: noise carved into the top, deeper the harder it is driven — nothing below the knee "
                "is touched",
     .gateControl = kModeControl, .gateMask = kGritOnly},
    {.key = "clip_depth", .name = "Clip Depth", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .tooltip = "Fold: 0% flattens the overshoot, 100% folds it right back",
     .gateControl = kModeControl, .gateMask = kFoldOnly},
    {.key = "clip_bits", .name = "Clip Bits", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kBitsNames, .tooltip = "Bits: the top quantised to 1 or 2 bits", .gateControl = kModeControl,
     .gateMask = kBitsOnly},
    {.key = "clip_mix", .name = "Clip Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .tooltip = "Dry against clipped, around the whole device"},
    {.key = "clip_wet_gain", .name = "Clip Wet Gain", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Fader, .names = {}, .modulatable = true,
     .tooltip = "Level of the clipped signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// EVERY DEFAULT PINNED TO THE DESCRIPTOR, so a fresh state cannot depend on which road it arrived by.
namespace detail {
[[nodiscard]] constexpr float defOf(Param p) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
}
} // namespace detail
static_assert(Settings {}.enabled == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));
static_assert(Settings {}.mode == static_cast<int>(detail::defOf(Param::Mode)));
static_assert(Settings {}.driveDb == detail::defOf(Param::Drive));
static_assert(Settings {}.ceilingDb == detail::defOf(Param::Ceiling));
static_assert(Settings {}.outputDb == detail::defOf(Param::Output));
static_assert(Settings {}.oversample == (kParamDescs[static_cast<size_t>(Param::Oversample)].def >= 0.5));
static_assert(Settings {}.knee == detail::defOf(Param::Knee));
static_assert(Settings {}.freq == detail::defOf(Param::Freq));
static_assert(Settings {}.foldback == detail::defOf(Param::Foldback));
static_assert(Settings {}.grit == detail::defOf(Param::Grit));
static_assert(Settings {}.depth == detail::defOf(Param::Depth));
static_assert(Settings {}.bits == static_cast<int>(detail::defOf(Param::Bits)));

/// THE RENDER PATH.
extern const device::AudioOps kAudioOps;

/// TEST ONLY — |H| OF THE TEXTURE HIGH-PASS AS INSTALLED, at `hz`, evaluated at the rate the device is
/// RUNNING (4 fs with oversampling on). A corner is not recoverable from a clipped buffer that carries
/// every harmonic at once; the coefficients are.
[[nodiscard]] double textureHighpassGainForTest(const void* storage, double hz);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: an effect, not a modulator. Spelled because every field of this struct is a decision.
	.outputs = {},
	.audio = &kAudioOps,
	// Drive & Colour, beside Shaper and Eroder, not Dynamics beside COMP — decided 2026-09-25.
	.category = device::Category::DriveColour,
};

} // namespace feathers::fx::clip
