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
#include "mod/modulator.h"
#include "mod/trig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>

// RANDOM TRIGGER — pseudorandom events as MODULATION (backlog NA (a); option 3 chosen: both roads).
//
// **THE SAME GENERATOR AS NOISE'S IMPULSES, NOT A SECOND ONE.** `dsp::RandomEvents` runs here one sample at a time,
// 64 samples a tick, from the stream NOISE's left ear opens on (`kEventStreamSalt` is NOISE's `Stream::EventsLeft`):
// the same controls and the same Stable seed give the same hits, on the same samples, with the same sizes — asserted
// by value against a NOISE instance in `test_mod_rtrig`. What differs is what a hit BECOMES:
//
//   - **Trig** — 1 for the one tick a hit lands in, 0 otherwise (MV's trigger output, emitted unsmoothed). Two hits in
//     one tick are one pulse; two in consecutive ticks read 1, 1 and a Trig taker fires once (it needs 0.25 to re-arm).
//   - **Size** — the latest hit's value, signed under Random polarity, held to the next hit.
//   - **Decay** — the latest hit's value falling away over Decay (to -60 dB at Decay, exact zero soon after), a small
//     envelope per hit.
//
// **POLY OR GLOBAL** (`Param::Poly`): global is one stream for the rack; Poly gives each note its own, opened at its On.
// Stable opens every stream (each note's, or the global one at each note) on the Seed's pattern; Free never restarts.

namespace feathers::mod::rtrig {

/// THE ROW ORDER. Rows are KEYED, so this order is free to change; it is the card's reading order.
enum class Param : int {
	Enabled,
	/// HITS PER SECOND, on average, while Sync is Free — log2, as NOISE's Density.
	Density,
	/// FREE, or one hit per musical division on average.
	Sync,
	Timing,
	/// REGULAR ONLY.
	Jitter,
	/// CLUSTERED ONLY.
	Cluster,
	HitSize,
	Polarity,
	/// HOW LONG THE DECAY OUTPUT TAKES TO FALL 60 dB, log2 seconds; the bottom of the travel is one tick.
	Decay,
	SeedMode,
	Seed,
	Poly,
	/// FIRES ONE HIT (backlog MV).
	Trig,
	kCount,
};

/// THE THREE OUTPUTS, by index into `kOutputs`.
enum class Output : size_t { Trig, Size, Decay, kCount };

enum class SeedMode : int { Stable, Free, kCount };
inline constexpr const char* kSeedModeNames[] = { "Stable", "Free" };
inline constexpr const char* kTimingNames[] = { "Regular", "Poisson", "Clustered" };
static_assert(std::size(kTimingNames) == static_cast<size_t>(dsp::EventTiming::kCount));
inline constexpr const char* kHitSizeNames[] = { "Fixed", "Uniform", "Exponential" };
static_assert(std::size(kHitSizeNames) == static_cast<size_t>(dsp::HitSize::kCount));
inline constexpr const char* kPolarityNames[] = { "Positive", "Random" };

/// SYNC: `Free` (Density sets the rate) or one hit per division on average — NOISE's list, built the same way.
inline constexpr std::array<const char*, device::kDivisionCount + 1> kSyncNames = [] {
	std::array<const char*, device::kDivisionCount + 1> names {};
	names[0] = "Free";
	for (size_t i = 0; i < device::kDivisionCount; ++i) {
		names[i + 1] = device::kDivisionNames[i];
	}
	return names;
}();

/// THE RANGES. Density stops at 200 hits a second: the tick is 750 Hz at 48 kHz, so a denser stream is a Trig held high.
inline constexpr double kDensityMinLog2 = -3.321928094887362; // log2(0.1)
inline constexpr double kDensityMaxLog2 = 7.643856189774724;  // log2(200)
inline constexpr double kDensityDefLog2 = 2.0;                // 4 hits per second
inline constexpr double kDecayDefLog2 = -2.0;                 // 250 ms
/// WITH NO TEMPO FROM THE HOST, A SYNCED DENSITY ASSUMES 120 BPM — the fallback NOISE and the fx lane use.
inline constexpr double kFallbackSecondsPerBeat = 0.5;
/// THE DECAY OUTPUT IS 1/1000 OF ITS HIT AT `Decay` SECONDS.
inline constexpr double kDecayFloorDb = -60.0;

/// GATES, as masks over a gate control's values.
inline constexpr uint32_t kBit(int v) {
	return 1u << static_cast<uint32_t>(v);
}
inline constexpr int16_t kSyncControl = static_cast<int16_t>(Param::Sync);
inline constexpr int16_t kTimingControl = static_cast<int16_t>(Param::Timing);
inline constexpr int16_t kSeedModeControl = static_cast<int16_t>(Param::SeedMode);
inline constexpr uint32_t kFreeOnly = kBit(0);
inline constexpr uint32_t kRegularOnly = kBit(static_cast<int>(dsp::EventTiming::Regular));
inline constexpr uint32_t kClusteredOnly = kBit(static_cast<int>(dsp::EventTiming::Clustered));
inline constexpr uint32_t kStableOnly = kBit(static_cast<int>(SeedMode::Stable));

struct Settings {
	bool enabled = true;
	float density = static_cast<float>(kDensityDefLog2);
	int sync = 0;
	int timing = static_cast<int>(dsp::EventTiming::Poisson);
	float jitter = 0.25f;
	float cluster = 0.5f;
	int hitSize = static_cast<int>(dsp::HitSize::Fixed);
	bool randomPolarity = false;
	float decay = static_cast<float>(kDecayDefLog2);
	int seedMode = static_cast<int>(SeedMode::Stable);
	int seed = 0;
	bool poly = false;
	/// THE TRIG CONTROL, resolved: a hit fires on its rising edge (`mod/trig.h`).
	float trig = 0.0f;
	/// FOR `PerNoteTable`'s per-note edge, as `mod::PolyShape` names it.
	static constexpr size_t kTrigControl = static_cast<size_t>(Param::Trig);

	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// THE RATE IN HITS PER SECOND — Density while Sync is Free; one per division otherwise, at the host's tempo or 120.
[[nodiscard]] double hitsPerSecond(const Settings& s, double secondsPerBeat);
/// THE CORE'S SETTINGS FOR THIS DEVICE'S CONTROLS — the mapping NOISE's `eventSettingsOf` makes from the same controls.
[[nodiscard]] dsp::RandomEventSettings eventSettingsOf(const Settings& s, double secondsPerBeat);

/// THE SEED A STABLE STREAM OPENS ON — splitmix64 of the device's Seed and NOISE's left-ear impulse salt, so a NOISE
/// and a Random Trigger on one Seed play one pattern (`fx::noise::eventStreamSeed`, restated: the mod tier does not
/// include an fx header, and `test_mod_rtrig` pins the two equal).
inline constexpr uint64_t kEventStreamSalt = 3;
[[nodiscard]] constexpr uint64_t eventStreamSeed(uint64_t seed) {
	uint64_t z = seed + 0x9e3779b97f4a7c15ull * kEventStreamSalt;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

/// THE DECAY OUTPUT'S PER-TICK FACTOR at `decayLog2` seconds to -60 dB; 0 at the bottom of the travel (one tick long).
[[nodiscard]] double decayFactor(float decayLog2, double tickSeconds);

/// **THE DECAY OUTPUT, ONE TICK AT A TIME** — the newest hit's value falling by `decayFactor` per tick. One rule for both
/// roads that publish it: this device's Decay and NOISE's Impulse Decay (backlog NA), so the two cannot drift.
class HitDecay {
public:
	void reset() noexcept { value_ = 0.0f; }
	/// THE FALL, FIRST IN A TICK, so a hit in the tick stands at its full value for the tick it lands in. At rest it IS
	/// zero (`kSmoothingRest`, the house's one-pole rest), not a value that never quite arrives.
	void fall(double factor) noexcept {
		value_ = static_cast<float>(static_cast<double>(value_) * factor);
		if (std::fabs(value_) < static_cast<float>(kSmoothingRest)) {
			value_ = 0.0f;
		}
	}
	/// A HIT: the value starts again from it (signed, as the hit is).
	void hit(float v) noexcept { value_ = v; }
	[[nodiscard]] float value() const noexcept { return value_; }

private:
	float value_ = 0.0f;
};

/// THE FIRE STREAM'S SEED FOR A STREAM OPENING ON `streamSeed` — splitmix64 of it under the fires' own salt, odd.
[[nodiscard]] constexpr uint64_t fireSeed(uint64_t streamSeed) {
	uint64_t z = (streamSeed ^ 0xf12ef12ef12ef12eull) + 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return (z ^ (z >> 31)) | 1u;
}

/// **A TRIG'S HITS, FROM A STREAM OF THEIR OWN** (backlog MV): one hit per fire, sized by the generator's law
/// (`dsp::RandomEvents::drawHit`, restated — two uniforms, always both drawn) and its polarity, but drawn apart from the
/// generator so a fire never moves the generator's own hits. One rule for both roads that are fired: this device's Trig
/// and NOISE's (backlog MZ), so a NOISE and a Random Trigger fired alike on one Seed hit alike.
class FireStream {
public:
	/// THE STREAM FROM A SEED. Never zero (xorshift's one fixed point).
	void seed(uint64_t s) noexcept { state_ = s != 0 ? s : 0x6a09e667f3bcc909ull; }
	[[nodiscard]] uint64_t state() const noexcept { return state_; }
	void setState(uint64_t s) noexcept { seed(s); }

	/// ONE FIRE'S HIT.
	[[nodiscard]] float next(const dsp::RandomEventSettings& s) noexcept {
		const double uSize = uniform();
		const double uSign = uniform();
		double size = 1.0;
		switch (s.size) {
		case dsp::HitSize::Uniform: size = 1.0 - uSize; break;
		case dsp::HitSize::Exponential:
			size = std::min(1.0, -std::log(1.0 - uSize) * dsp::RandomEvents::kExponentialMean);
			break;
		case dsp::HitSize::Fixed:
		case dsp::HitSize::kCount: break;
		}
		return static_cast<float>(s.randomPolarity && uSign < 0.5 ? -size : size);
	}

private:
	[[nodiscard]] double uniform() noexcept {
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		return static_cast<double>((state_ * 0x2545f4914f6cdd1dull) >> 11) * (1.0 / 9007199254740992.0);
	}

	uint64_t state_ = 0x6a09e667f3bcc909ull;
};

inline constexpr device::SourceDesc kOutputs[] = {
	// A ONE-TICK PULSE TO 1 — unipolar, and the same in every polarity.
	{ .key = "rtrig_trig", .name = "Trig", .perVoice = true },
	{ .key = "rtrig_size", .name = "Size", .bipolar = true, .bipolarControl = static_cast<int>(Param::Polarity),
	  .perVoice = true },
	{ .key = "rtrig_decay", .name = "Decay", .bipolar = true, .bipolarControl = static_cast<int>(Param::Polarity),
	  .perVoice = true },
};
static_assert(std::size(kOutputs) == static_cast<size_t>(Output::kCount));

inline constexpr const char* kTags[] = { "random", "trigger", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "rtrig",
	.name = "Random Trigger",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

inline constexpr device::ParamDesc kParamDescs[] = {
    {.key = "rtrig_enabled", .name = "Random Trigger Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the random trigger runs"},
    {.key = "rtrig_density", .name = "Random Trigger Density", .min = kDensityMinLog2, .max = kDensityMaxLog2,
     .def = kDensityDefLog2, .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "How many hits a second, on average", .gateControl = kSyncControl, .gateMask = kFreeOnly},
    {.key = "rtrig_sync", .name = "Random Trigger Sync", .min = 0.0, .max = static_cast<double>(kSyncNames.size() - 1),
     .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kSyncNames,
     .tooltip = "Free sets the rate with Density; a division gives one hit per division on average, at the host's "
                "tempo"},
    {.key = "rtrig_timing", .name = "Random Trigger Timing", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kTimingNames,
     .tooltip = "When the hits fall: on a grid (with Jitter), at random (Poisson), or in bursts"},
    {.key = "rtrig_jitter", .name = "Random Trigger Jitter", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Regular: how far each hit strays from its grid point, up to half the gap either way",
     .gateControl = kTimingControl, .gateMask = kRegularOnly},
    {.key = "rtrig_cluster", .name = "Random Trigger Cluster", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Clustered: how bursty. 0% is plain random, 100% packs the hits into tight bursts; the average rate "
                "stays Density",
     .gateControl = kTimingControl, .gateMask = kClusteredOnly},
    {.key = "rtrig_hit_size", .name = "Random Trigger Hit Size", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kHitSizeNames,
     .tooltip = "How big each hit is (Size and Decay): all 1, evenly spread, or mostly small with an occasional big one"},
    {.key = "rtrig_polarity", .name = "Random Trigger Polarity", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kPolarityNames,
     .tooltip = "Size and Decay: every hit upward, or each one up or down at random. Trig is always a pulse up"},
    {.key = "rtrig_decay", .name = "Random Trigger Decay", .min = kTimeLog2SecondsMin, .max = kTimeLog2SecondsMax,
     .def = kDecayDefLog2, .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "How long the Decay output takes to fall away from each hit (to -60 dB)"},
    {.key = "rtrig_seed_mode", .name = "Random Trigger Seed Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kSeedModeNames,
     .tooltip = "Stable: the same pattern from the Seed at every note. Free: it never restarts"},
    {.key = "rtrig_seed", .name = "Random Trigger Seed", .min = 0.0, .max = 999.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Number, .names = {},
     .tooltip = "Which pattern plays while the seed is Stable. A NOISE on the same Seed plays the same hits",
     .gateControl = kSeedModeControl, .gateMask = kStableOnly},
    {.key = "rtrig_poly", .name = "Random Trigger Poly", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "On: each note runs its own stream from its start. Off: one stream for every voice"},
    // **TRIG** (backlog MV): fires ONE hit, through the same size law, on the tick it rises past 0.5 — the pad on the card.
    mod::trigParamDesc("rtrig_trig_in", "Random Trigger Trig",
                       mod::trigTooltip<"Fires one hit now, sized like the others. Route a trigger here, or click the pad.">()),
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

namespace detail {
[[nodiscard]] constexpr float defOf(Param p) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
}
} // namespace detail
static_assert(Settings {}.enabled == (detail::defOf(Param::Enabled) >= 0.5f));
static_assert(Settings {}.density == detail::defOf(Param::Density));
static_assert(Settings {}.sync == static_cast<int>(detail::defOf(Param::Sync)));
static_assert(Settings {}.timing == static_cast<int>(detail::defOf(Param::Timing)));
static_assert(Settings {}.jitter == detail::defOf(Param::Jitter));
static_assert(Settings {}.cluster == detail::defOf(Param::Cluster));
static_assert(Settings {}.hitSize == static_cast<int>(detail::defOf(Param::HitSize)));
static_assert(Settings {}.randomPolarity == (detail::defOf(Param::Polarity) >= 0.5f));
static_assert(Settings {}.decay == detail::defOf(Param::Decay));
static_assert(Settings {}.trig == detail::defOf(Param::Trig));
static_assert(Settings {}.seedMode == static_cast<int>(detail::defOf(Param::SeedMode)));
static_assert(Settings {}.seed == static_cast<int>(detail::defOf(Param::Seed)));
static_assert(Settings {}.poly == (detail::defOf(Param::Poly) >= 0.5f));

/// IS CONTROL `p` LIVE UNDER THIS ROW — its gate, and its gate's gate. `fieldAt(i)` answers control i.
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

/// **THE DISTRIBUTION'S SHAPE, AS THE CARD DRAWS IT — derived from `dsp::RandomEvents`' own draws, not drawn from
/// the idea of them**, and checked against a histogram of the generator's gaps in `test_mod_rtrig`.
///
/// THE GAP'S DENSITY at `x` mean intervals (so every timing's mean is 1). Regular: each gap is the interval plus the
/// difference of two displacements uniform on ±j/2, a triangle on [1 - j, 1 + j]; at j = 0 a spike (`kSpike`).
/// Poisson: e^-x. Clustered: the generator's two exponentials, short with probability 0.9 x cluster.
inline constexpr double kSpike = -1.0;
[[nodiscard]] inline double gapDensity(dsp::EventTiming timing, float jitter, float cluster, double x) {
	if (x < 0.0) {
		return 0.0;
	}
	switch (timing) {
	case dsp::EventTiming::Regular: {
		const double j = std::clamp(static_cast<double>(jitter), 0.0, 1.0);
		if (j <= 0.0) {
			return x == 1.0 ? kSpike : 0.0;
		}
		const double d = std::fabs(x - 1.0);
		return d >= j ? 0.0 : (j - d) / (j * j);
	}
	case dsp::EventTiming::Clustered: {
		const double p = dsp::RandomEvents::kClusterShortProbability * std::clamp(static_cast<double>(cluster), 0.0, 1.0);
		const double s = dsp::RandomEvents::kClusterShortFraction;
		const double l = (1.0 - p * s) / (1.0 - p);
		return p / s * std::exp(-x / s) + (1.0 - p) / l * std::exp(-x / l);
	}
	case dsp::EventTiming::Poisson:
	case dsp::EventTiming::kCount: break;
	}
	return std::exp(-x);
}

/// THE HIT'S SIZE: its density on (0, 1) — Fixed none, Uniform 1, Exponential 3e^-3x — and the probability mass AT 1
/// (Fixed all of it; Exponential the capped tail, e^-3).
[[nodiscard]] inline double sizeDensity(dsp::HitSize size, double x) {
	if (x <= 0.0 || x >= 1.0) {
		return 0.0;
	}
	switch (size) {
	case dsp::HitSize::Uniform: return 1.0;
	case dsp::HitSize::Exponential: {
		const double mean = dsp::RandomEvents::kExponentialMean;
		return std::exp(-x / mean) / mean;
	}
	case dsp::HitSize::Fixed:
	case dsp::HitSize::kCount: break;
	}
	return 0.0;
}
[[nodiscard]] inline double sizeMassAtOne(dsp::HitSize size) {
	switch (size) {
	case dsp::HitSize::Fixed: return 1.0;
	case dsp::HitSize::Exponential: return std::exp(-1.0 / dsp::RandomEvents::kExponentialMean);
	case dsp::HitSize::Uniform:
	case dsp::HitSize::kCount: break;
	}
	return 0.0;
}

extern const device::ModOps kModOps;
/// THE NOTE LANE — each note's stream under Poly, and the Stable restart at a note under global. Defined in rtrig.cpp.
extern const device::NoteOps kNoteOps;

/// **THE GLOBAL STREAM, SAVED WITH THE PATCH** (backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE PERSISTS WITH THE
/// PATCH"): the held Size and Decay, the generator's and the Trig's positions, the hit count, and what the streams were
/// seeded from — so a reload goes on holding the last hit's size, falls from where the decay was, and hits where the
/// saved stream would have. In 16-bit words (`device::BlockWriter`). **WHAT IS NOT SAVED, AND WHY**: a Poly note's
/// stream restarts at every note-on by design, and a reload starts no notes.
inline constexpr uint32_t kStreamWords = 33;
uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "rtrig_stream", .floats = kStreamWords },
};
inline constexpr device::StateOps kStateOps { .blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock };

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.state = &kStateOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::rtrig
