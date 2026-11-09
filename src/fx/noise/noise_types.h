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
#include "dsp/random_events.h"
#include "mod/rtrig/rtrig_types.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// NOISE — a device that ADDS noise or pseudorandom impulses to whatever passes, through its own filter
// (backlog MZ 2026-10-17: its own device, in Harm's philosophy, with a basic filter built in and no
// associated cut).
//
// **THE PASSING SIGNAL IS UNTOUCHED, AND THAT IS EXACT**: the generated signal is added at Level, and at Level 0
// (or switched off) the buffer is not written at all, so the output is the input to the bit. No notch, no cut,
// no dry/wet: there is nothing to blend against, only something to add.
//
// **ONE DEVICE WITH A SOURCE SWITCH, NOT TWO** (by decision): Noise — one Colour control tilting violet -> white ->
// pink -> brown — or Impulses, Dirac clicks from `dsp::RandomEvents`, the core NA's Random Trigger modulator will
// share. Each source's own controls are gated on Source, so a card hides what the other source would have read.
//
// **WHERE IT SITS DECIDES WHAT IT FOLLOWS.** Poly on (the default) runs one per voice before the voice's level
// stage (`Synth::applyVoiceFx` runs ahead of Voice Level), so the noise follows the note's amp and ends with the
// voice, and Key Track follows that voice's pitch. Poly off runs one on the summed bus, which sounds whether or
// not a note is held — and so keeps the host awake while Level is up (`holdsTail`).

namespace feathers::fx::noise {

/// THE ROW ORDER. Rows are KEYED (`ParamDesc::key`), so this order is free to change; it is the card's reading
/// order, with the two sources' blocks together.
enum class Param : int {
	Enabled,
	/// NOISE OR IMPULSES — a segmented control, both on the face.
	Source,
	/// HOW MUCH IS ADDED: a fader position, gain (2v)^2, so 0 is silence and 0.5 is unity (0 dB).
	Level,
	/// NOISE ONLY: the spectral tilt, -1 violet (+6 dB/oct), 0 white, +0.5 pink (-3), +1 brown (-6).
	Colour,
	/// THE FILTER: which response, where, how sharp, and how far its corner follows the note.
	FilterType,
	Cutoff,
	Resonance,
	KeyTrack,
	/// HOW DIFFERENT THE EARS ARE: 0 the same signal in both, 1 two independent ones.
	Stereo,
	/// STABLE (the pattern restarts from Seed at every note, or reset) OR FREE (it never restarts).
	SeedMode,
	Seed,
	/// IMPULSES ONLY, from here down.
	Density,
	Sync,
	Timing,
	Jitter,
	Cluster,
	HitSize,
	Polarity,
	/// HOW LONG THE IMPULSE DECAY OUTPUT TAKES TO FALL 60 dB, log2 seconds — Random Trigger's Decay, the same law.
	/// It shapes a modulation output only; no click changes.
	Decay,
	/// **IMPULSES: THE TRIG** (backlog MZ, MV's convention): one hit per rising edge, sized by Hit Size's law and
	/// Polarity from a stream of its own (`mod::rtrig::FireStream`), so a fire never moves the generator's hits.
	Trig,
	/// THE ARCHITECTURE SWITCH: one per voice, or one on the bus (`Traits::perVoiceControl`).
	Poly,
	kCount,
};

enum class Source : int { Noise, Impulses, kCount };
inline constexpr const char* kSourceNames[] = { "Noise", "Impulses" };

enum class FilterType : int { Lowpass, Bandpass, Highpass, kCount };
inline constexpr const char* kFilterTypeNames[] = { "LP", "BP", "HP" };

enum class SeedMode : int { Stable, Free, kCount };
inline constexpr const char* kSeedModeNames[] = { "Stable", "Free" };

inline constexpr const char* kTimingNames[] = { "Regular", "Poisson", "Clustered" };
static_assert(std::size(kTimingNames) == static_cast<size_t>(dsp::EventTiming::kCount));
inline constexpr const char* kHitSizeNames[] = { "Fixed", "Uniform", "Exponential" };
static_assert(std::size(kHitSizeNames) == static_cast<size_t>(dsp::HitSize::kCount));

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kPolarityNames[] = { "Positive", "Random" };

/// SYNC: `Free` (Density sets the rate in hits per second) or one hit per musical division on average.
inline constexpr std::array<const char*, device::kDivisionCount + 1> kSyncNames = [] {
	std::array<const char*, device::kDivisionCount + 1> names {};
	names[0] = "Free";
	for (size_t i = 0; i < device::kDivisionCount; ++i) {
		names[i + 1] = device::kDivisionNames[i];
	}
	return names;
}();

/// THE RANGES, NAMED ONCE — the descriptor, `stateOf` and the card read the same numbers.
inline constexpr double kCutoffMinLog2 = 4.321928094887363;  // log2(20)
inline constexpr double kCutoffMaxLog2 = 14.287712379549449; // log2(20000)
inline constexpr double kDensityMinLog2 = -3.321928094887362; // log2(0.1)
inline constexpr double kDensityMaxLog2 = 10.965784284662087; // log2(2000)
inline constexpr double kDensityDefLog2 = 3.0;                // 8 hits per second
/// THE NOTE KEY TRACK IS MEASURED FROM: C4 (60) leaves the corner where Cutoff puts it.
inline constexpr float kKeyTrackReferenceKey = 60.0f;
/// WITH NO TEMPO FROM THE HOST, A SYNCED DENSITY ASSUMES 120 BPM — the fallback the rest of the fx lane uses.
inline constexpr double kFallbackSecondsPerBeat = 0.5;

/// WHICH VALUES OF A GATE CONTROL A CONTROL IS LIVE IN, as a mask.
inline constexpr uint32_t kBit(int v) {
	return 1u << static_cast<uint32_t>(v);
}
inline constexpr int16_t kSourceControl = static_cast<int16_t>(Param::Source);
inline constexpr int16_t kSyncControl = static_cast<int16_t>(Param::Sync);
inline constexpr int16_t kTimingControl = static_cast<int16_t>(Param::Timing);
inline constexpr int16_t kSeedModeControl = static_cast<int16_t>(Param::SeedMode);
inline constexpr uint32_t kNoiseOnly = kBit(static_cast<int>(Source::Noise));
inline constexpr uint32_t kImpulsesOnly = kBit(static_cast<int>(Source::Impulses));
/// DENSITY IS READ ONLY WHILE SYNC IS FREE (index 0).
inline constexpr uint32_t kFreeOnly = kBit(0);
inline constexpr uint32_t kRegularOnly = kBit(static_cast<int>(dsp::EventTiming::Regular));
inline constexpr uint32_t kClusteredOnly = kBit(static_cast<int>(dsp::EventTiming::Clustered));
inline constexpr uint32_t kStableOnly = kBit(static_cast<int>(SeedMode::Stable));

struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — every effect defaults to on when it is added. Pinned to the descriptor below, as every default is.
	bool enabled = true;
	int source = static_cast<int>(Source::Noise);
	/// THE FADER POSITION, not the gain — `levelGain` turns it into one.
	float level = 0.25f;
	float colour = 0.5f;
	int filterType = static_cast<int>(FilterType::Lowpass);
	/// log2(Hz), as the control stores it.
	float cutoff = static_cast<float>(kCutoffMaxLog2);
	float resonance = 0.0f;
	float keyTrack = 0.0f;
	float stereo = 1.0f;
	int seedMode = static_cast<int>(SeedMode::Stable);
	int seed = 0;
	/// log2(hits per second), as the control stores it.
	float density = static_cast<float>(kDensityDefLog2);
	/// 0 is Free; i > 0 is `device::kDivisionBeats[i - 1]`.
	int sync = 0;
	int timing = static_cast<int>(dsp::EventTiming::Poisson);
	float jitter = 0.25f;
	float cluster = 0.5f;
	int hitSize = static_cast<int>(dsp::HitSize::Fixed);
	bool randomPolarity = false;
	/// log2(seconds to -60 dB), as the control stores it.
	float decay = static_cast<float>(mod::rtrig::kDecayDefLog2);
	/// THE TRIG AS RESOLVED, 0..1 — its edge is the device's (`mod::TrigEdge`).
	float trig = 0.0f;
	bool poly = true;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// THE ROW: the serialized form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// THE FADER'S GAIN — `device::Unit::Fader`'s law, (2v)^2. 0 is exactly 0.
[[nodiscard]] inline float levelGain(float level) {
	const float v = 2.0f * std::fmax(level, 0.0f);
	return v * v;
}

/// THE TILT'S SLOPE IN dB PER OCTAVE AT A COLOUR: -6.0206 x colour, so +6 violet, 0 white, -3 pink, -6 brown.
[[nodiscard]] inline double slopeDbPerOctave(float colour) {
	return -20.0 * std::log10(2.0) * static_cast<double>(colour);
}

/// THE RATE OF IMPULSES IN HITS PER SECOND — Density while Sync is Free; one per division otherwise, at the
/// host's tempo or 120 BPM without one.
[[nodiscard]] double hitsPerSecond(const Settings& s, double secondsPerBeat);

/// THE CORE'S SETTINGS FOR THIS DEVICE'S CONTROLS — what NA's modulator will build from its own.
[[nodiscard]] dsp::RandomEventSettings eventSettingsOf(const Settings& s, double secondsPerBeat);

/// THE FOUR STREAMS A SEED NAMES, kept apart so a change to one cannot shift another.
enum class Stream : uint64_t { NoiseLeft = 1, NoiseRight, EventsLeft, EventsRight };

/// ONE STREAM'S SEED FROM THE DEVICE'S — splitmix64 of the pair.
[[nodiscard]] constexpr uint64_t streamSeed(uint64_t seed, Stream s) {
	uint64_t z = seed + 0x9e3779b97f4a7c15ull * static_cast<uint64_t>(s);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

/// THE LEFT EAR'S IMPULSE STREAM UNDER A STABLE SEED — what a note opens on, and what the card's strip draws.
[[nodiscard]] constexpr uint64_t eventStreamSeed(uint64_t seed) {
	return streamSeed(seed, Stream::EventsLeft);
}

/// WHAT THIS DEVICE IS, for a browser to group it by.
inline constexpr const char* kTags[] = { "generator", "noise", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "noise",
	.name = "Noise",
	.hasCard = true,
	/// NO EXPANDED VIEW: the card is the whole device.
	.hasExpanded = false,
	.perVoiceControl = static_cast<int>(Param::Poly),
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// A GENERATOR OFFERS A POST CHAIN AND NO FURNITURE (backlog NR, D5): what it adds can be processed, not mixed.
	.siteLanes = device::Traits::SiteLanes::PostOnly,
};

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    {.key = "noise_enabled", .name = "Noise Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the noise is added"},
    {.key = "noise_source", .name = "Noise Source", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kSourceNames,
     .tooltip = "What is added: coloured noise, or random clicks (impulses) — the filter shapes either"},
    // A FADER, NOT A dB RANGE: silence is the bottom of the travel rather than a value bolted under it, and 0 is
    // the bit-exact pass-through. -12 dB by default: noise at unity sits on top of most sources.
    {.key = "noise_level", .name = "Noise Level", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,
     .unit = device::Unit::Fader, .names = {}, .modulatable = true,
     .tooltip = "How much is added on top of what passes. At the bottom nothing is added and the signal "
                "passes untouched"},
    {.key = "noise_colour", .name = "Noise Colour", .min = -1.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "The noise's tilt: -100% violet (+6 dB per octave), 0 white, 50% pink (-3 dB), 100% brown "
                "(-6 dB). Its loudness stays the same as it tilts",
     .gateControl = kSourceControl, .gateMask = kNoiseOnly, .centred = true},
    {.key = "noise_filter_type", .name = "Noise Filter Type", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kFilterTypeNames,
     .tooltip = "The filter on what is added: lowpass, bandpass or highpass. The passing signal is not filtered"},
    {.key = "noise_cutoff", .name = "Noise Cutoff", .min = kCutoffMinLog2, .max = kCutoffMaxLog2,
     .def = kCutoffMaxLog2, .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .offsetsMayLeaveRange = true, .tooltip = "Where the filter acts on what is added"},
    {.key = "noise_res", .name = "Noise Resonance", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How sharply the filter rings. High resonance turns impulses into pitched pings"},
    {.key = "noise_keytrack", .name = "Noise Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {},
     .tooltip = "How far the corner follows the note from C4: 100% moves it an octave per octave. Per voice "
                "with Poly, the host's held note on the bus"},
    {.key = "noise_stereo", .name = "Noise Stereo", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How different the ears are: 0% the same in both, 100% two independent streams"},
    {.key = "noise_seed_mode", .name = "Noise Seed Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kSeedModeNames,
     .tooltip = "Stable: the same pattern from the Seed at every note (and at reset). Free: it never restarts"},
    {.key = "noise_seed", .name = "Noise Seed", .min = 0.0, .max = 999.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Number, .names = {}, .tooltip = "Which pattern plays while the seed is Stable",
     .gateControl = kSeedModeControl, .gateMask = kStableOnly},
    {.key = "noise_density", .name = "Noise Density", .min = kDensityMinLog2, .max = kDensityMaxLog2,
     .def = kDensityDefLog2, .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "Impulses: how many clicks a second, on average", .gateControl = kSyncControl,
     .gateMask = kFreeOnly},
    {.key = "noise_sync", .name = "Noise Sync", .min = 0.0, .max = static_cast<double>(kSyncNames.size() - 1),
     .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kSyncNames,
     .tooltip = "Impulses: Free sets the rate with Density; a division gives one click per division on "
                "average, at the host's tempo",
     .gateControl = kSourceControl, .gateMask = kImpulsesOnly},
    {.key = "noise_timing", .name = "Noise Timing", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kTimingNames,
     .tooltip = "Impulses: when the clicks fall — on a grid (with Jitter), at random (Poisson), or in bursts",
     .gateControl = kSourceControl, .gateMask = kImpulsesOnly},
    {.key = "noise_jitter", .name = "Noise Jitter", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Regular: how far each click strays from its grid point, up to half the gap either way",
     .gateControl = kTimingControl, .gateMask = kRegularOnly},
    {.key = "noise_cluster", .name = "Noise Cluster", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Clustered: how bursty — 0% is plain random, 100% packs the clicks into tight bursts. The "
                "average rate stays Density",
     .gateControl = kTimingControl, .gateMask = kClusteredOnly},
    {.key = "noise_hit_size", .name = "Noise Hit Size", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kHitSizeNames,
     .tooltip = "Impulses: how loud each click is — all the same, evenly spread, or mostly small with an "
                "occasional big one",
     .gateControl = kSourceControl, .gateMask = kImpulsesOnly},
    {.key = "noise_polarity", .name = "Noise Polarity", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kPolarityNames,
     .tooltip = "Impulses: every click upward, or each one up or down at random",
     .gateControl = kSourceControl, .gateMask = kImpulsesOnly},
    // RANDOM TRIGGER'S DECAY, its range, default and law (`mod::rtrig::HitDecay`): the two publish one envelope.
    {.key = "noise_decay", .name = "Noise Impulse Decay", .min = mod::kTimeLog2SecondsMin,
     .max = mod::kTimeLog2SecondsMax, .def = mod::rtrig::kDecayDefLog2, .stepped = false,
     .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "Impulses: how long the Impulse Decay modulation output takes to fall away from each click (to "
                "-60 dB). The clicks themselves do not change",
     .gateControl = kSourceControl, .gateMask = kImpulsesOnly},
    // **TRIG** (backlog MZ, MV): Random Trigger's Trig, key and all but the prefix — one hit per fire, the pad on the
    // card. Read under Impulses only, so it is hidden under Noise (the honesty rule).
    [] {
	    device::ParamDesc d = mod::trigParamDesc(
	        "noise_trig_in", "Noise Trig",
	        mod::trigTooltip<"Impulses: fires one click now, sized like the others. Route a trigger here, or click the "
	                         "pad.">());
	    d.gateControl = kSourceControl;
	    d.gateMask = kImpulsesOnly;
	    return d;
    }(),
    // ON BY DEFAULT: in a synth the noise belongs to the note — it follows the amp and ends with the voice. Off,
    // one instance on the bus sounds whether or not a note is held.
    {.key = "noise_poly", .name = "Noise Poly", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "On: one per voice, following each note's level and pitch. Off: one on the mix, always "
                "sounding"},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// EVERY DEFAULT PINNED TO THE DESCRIPTOR, so a fresh state cannot depend on which road it arrived by.
namespace detail {
[[nodiscard]] constexpr float defOf(Param p) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
}
} // namespace detail
static_assert(Settings {}.enabled == (detail::defOf(Param::Enabled) >= 0.5f));
static_assert(Settings {}.source == static_cast<int>(detail::defOf(Param::Source)));
static_assert(Settings {}.level == detail::defOf(Param::Level));
static_assert(Settings {}.colour == detail::defOf(Param::Colour));
static_assert(Settings {}.filterType == static_cast<int>(detail::defOf(Param::FilterType)));
static_assert(Settings {}.cutoff == detail::defOf(Param::Cutoff));
static_assert(Settings {}.resonance == detail::defOf(Param::Resonance));
static_assert(Settings {}.keyTrack == detail::defOf(Param::KeyTrack));
static_assert(Settings {}.stereo == detail::defOf(Param::Stereo));
static_assert(Settings {}.seedMode == static_cast<int>(detail::defOf(Param::SeedMode)));
static_assert(Settings {}.seed == static_cast<int>(detail::defOf(Param::Seed)));
static_assert(Settings {}.density == detail::defOf(Param::Density));
static_assert(Settings {}.sync == static_cast<int>(detail::defOf(Param::Sync)));
static_assert(Settings {}.timing == static_cast<int>(detail::defOf(Param::Timing)));
static_assert(Settings {}.jitter == detail::defOf(Param::Jitter));
static_assert(Settings {}.cluster == detail::defOf(Param::Cluster));
static_assert(Settings {}.hitSize == static_cast<int>(detail::defOf(Param::HitSize)));
static_assert(Settings {}.randomPolarity == (detail::defOf(Param::Polarity) >= 0.5f));
static_assert(Settings {}.decay == detail::defOf(Param::Decay));
static_assert(Settings {}.trig == detail::defOf(Param::Trig));
static_assert(Settings {}.poly == (detail::defOf(Param::Poly) >= 0.5f));

/// IS CONTROL `p` LIVE UNDER THIS ROW — its own gate, AND its gate control's, all the way up. Jitter is gated on
/// Timing and Timing on Source, so Jitter under Noise is hidden even though Timing still reads Regular.
/// `fieldAt(i)` answers the row's value of control i.
template <typename FieldAt>
[[nodiscard]] bool liveIn(Param p, const FieldAt& fieldAt) {
	int at = static_cast<int>(p);
	for (int depth = 0; depth < static_cast<int>(Param::kCount); ++depth) {
		const device::ParamDesc& d = kParamDescs[static_cast<size_t>(at)];
		if (d.gateControl < 0) {
			return true;
		}
		if (!device::relevantFor(d, fieldAt(static_cast<size_t>(d.gateControl)))) {
			return false;
		}
		at = d.gateControl;
	}
	return true;
}

/// THE RENDER PATH.
extern const device::AudioOps kAudioOps;
/// THE MOD ROAD OVER THE SAME OBJECT (backlog NA (b)) — SKLITTER's shape: a dual device, its outputs published at the
/// tick. See `noise.cpp`'s schedule for why a hit and its event land on the same tick.
extern const device::ModOps kModOps;

/// THE IMPULSES AS MODULATION OUTPUTS (backlog NA (b)), by index into `kOutputs`.
enum class Out : size_t { Trig, Size, Decay, kCount };
/// **PER VOICE** under Poly: each voice instance publishes its own hits (`RackEngine::outputForNoteAt`).
inline constexpr device::SourceDesc kOutputs[] = {
	// 1 FOR THE TICK A CLICK SOUNDS IN — a one-tick pulse, unsmoothed (backlog MV's trigger output).
	{ .key = "noise_hit_trig", .name = "Impulse Trig", .perVoice = true },
	// THE NEWEST CLICK'S VALUE, signed under Random polarity, held to the next.
	{ .key = "noise_hit_size", .name = "Impulse Size", .bipolar = true, .bipolarControl = static_cast<int>(Param::Polarity),
	  .perVoice = true },
	// THE NEWEST CLICK'S VALUE FALLING 60 dB IN DECAY — Random Trigger's Decay output, by the same `HitDecay`.
	{ .key = "noise_hit_decay", .name = "Impulse Decay", .bipolar = true,
	  .bipolarControl = static_cast<int>(Param::Polarity), .perVoice = true },
};
static_assert(std::size(kOutputs) == static_cast<size_t>(Out::kCount));

/// **THE HELD IMPULSE, SAVED WITH THE PATCH** (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE PERSISTS WITH THE
/// PATCH"): Impulse Size, the newest click's value held to the next, and Impulse Decay falling from it — each a value a
/// destination reads, so a reload holds them rather than opening at 0 until the next hit. Two floats, by their bits
/// (`device::BlockWriter`). **WHAT IS NOT SAVED, AND WHY**: the streams' position (white noise and the impulse clock).
/// Where a white-noise stream stands is not an audible decision, and a Stable stream restarts from its Seed at every
/// note and reset anyway; Free's starts from one fixed seed, deterministically. A voice instance's hold ends with its note.
inline constexpr uint32_t kHeldWords = 4;
uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "noise_held", .floats = kHeldWords },
};
inline constexpr device::StateOps kStateOps { .blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock };

/// TEST ONLY — what the installed coefficients say, because none of it is recoverable from a buffer that carries
/// every frequency at once (CLAUDE.md: assert the value).
/// |H| of the colour tilt as installed, normalisation included, at `hz`.
[[nodiscard]] double tiltGainForTest(const void* storage, double hz);
/// The filter's corner as installed, in Hz — Cutoff with Key Track applied, after smoothing.
[[nodiscard]] double filterCornerForTest(const void* storage);
/// |H| of the filter as installed, at `hz`.
[[nodiscard]] double filterGainForTest(const void* storage, double hz);
/// The impulse rate the generator is running at, in hits per second.
[[nodiscard]] double hitRateForTest(const void* storage);
/// The voice slot the host named through `AudioOps::setVoice`, or -1 for an instance never told.
[[nodiscard]] int64_t voiceForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// **ITS IMPULSES AS MODULATION** (backlog NA (b)): the same hits, on the tick whose window they sound in.
	.outputs = kOutputs,
	.audio = &kAudioOps,
	.mod = &kModOps,
	.state = &kStateOps,
	.category = device::Category::Utility,
};

} // namespace feathers::fx::noise
