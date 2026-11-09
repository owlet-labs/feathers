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

#include "device/registry.h"
#include "mod/follower/follower_core.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// THE FOLLOWER (backlog MY, MY ADDENDUM, NW, OP) — an envelope follower, and the control → trigger decoder: its
// Threshold turns any signal into one-tick Trig pulses in one visible place.
//
// **ONE DEVICE, MODE = Control | Audio (backlog OP).** It was two devices because whether a device's outputs are computed from the audio passing
// through it was a STATIC declaration (`ModOps::readsAudio`). It is a function of the row now (`ModOps::readsAudioOf`):
//
//   - **CONTROL** follows its Input knob (route any source into it), reads no chain audio, and may reach every
//     destination — the voice, a DAHDSR's Trig, an fx before or after it. One instance: Poly is greyed.
//   - **AUDIO** hears the audio AT ITS SLOT — the fx lane's bus where it sits, or, under Poly, its own voice's output
//     where it sits in that voice's chain — and may reach only what is downstream of that audio. A route upstream
//     would be a hidden one-tick feedback loop. MEASURED IN THE AUDIO ROAD, PUBLISHED AT THE NEXT MODULATION TICK: at
//     most one tick (64 samples) behind. Under Poly each voice instance follows its voice and publishes for its note
//     through the rack's per-voice road (`RackEngine::publishesFromVoices`); a destination with no voice reads the
//     newest note's.
//
// FLIPPING THE MODE DELETES THE ROUTES THE NEW ANSWER FORBIDS (the reorder rule), the card names them in the tray,
// and one undo puts the mode and the routes back (`ChainRecord::takePrunedRoutes`, `FollowerCard`). The Follower Tap
// and its tag FOLTAP are retired, unmigrated (patch compatibility is not a constraint).
//
// **THE DETECTOR: SIGNED, PEAK OR RMS.** Signed is the control detector and Control's default: no rectification, a
// lag of a bipolar signal (Attack while rising, Release while falling), so Env is bipolar under it and Threshold
// acts on the signed value; Floor does nothing there and is greyed. Peak and RMS rectify, by design, for audio — and
// are Audio's choices: flipping to Audio from Signed takes Peak, and Signed under Audio (a hand-written row) runs as
// Peak (`settingsOf`).
//
// THE AUDIO ROAD WRITES NOTHING (`AudioOps::listensOnly`), and in Control mode it does not listen either.

namespace feathers::mod::follower {

/// ORDER IS THE ROW ORDER AND THE DESCRIPTOR ORDER.
enum class Param : size_t {
	/// WHAT IT FOLLOWS — its Input (Control) or the audio at its slot (Audio). Decides what its outputs may reach.
	Mode,
	/// THE CONTROL INPUT — rests at 0, takes routes: the signal the follower follows in Control.
	Input,
	Detector,
	Attack,
	Release,
	Gain,
	Floor,
	Threshold,
	/// THE ARCHITECTURE SWITCH, in Audio only (`Traits::perVoiceRequires`).
	Poly,
	/// THE BYPASS — read by the host: a disabled modulator publishes its rest (`RackEngine::advanceModulators`).
	Enabled,
	kCount,
};

/// ORDER IS THE OUTPUT ORDER.
enum class Output : size_t { Env = 0, Trig, kCount };

/// THE MODE'S ENTRIES, in stored order.
enum class Mode : uint8_t { Control = 0, Audio, kCount };

/// THE TIMES' RANGES, in log2(seconds) — `Unit::LogSeconds`, whose MINIMUM means "no time at all": at the bottom
/// the follower tracks its input exactly. Attack 0.1 ms .. 1 s, default 5 ms; Release 1 ms .. 5 s, default 150 ms.
inline constexpr double kAttackLog2Min = -13.287712379549449; // log2(0.0001)
inline constexpr double kAttackLog2Max = 0.0;                 // log2(1)
inline constexpr double kAttackLog2Default = -7.643856189774724; // log2(0.005)
inline constexpr double kReleaseLog2Min = -9.965784284662087;    // log2(0.001)
inline constexpr double kReleaseLog2Max = 2.321928094887362;     // log2(5)
inline constexpr double kReleaseLog2Default = -2.736965594166206; // log2(0.15)

/// THE FLOOR'S BOTTOM IS OFF — a floor of exactly 0, not −72 dB's 0.00025, so the bottom of the travel means
/// "no floor" by the same convention the times use.
inline constexpr double kFloorDbMin = -72.0;

/// A STORED TIME AS SECONDS — the bottom of the range is 0 (no smoothing).
[[nodiscard]] inline float secondsOf(float log2Seconds, double log2Min) noexcept {
	if (!std::isfinite(log2Seconds) || log2Seconds <= static_cast<float>(log2Min) + 1.0e-4f) {
		return 0.0f;
	}
	return std::exp2(log2Seconds);
}

/// DECIBELS AS A LINEAR GAIN.
[[nodiscard]] inline float gainOf(float db) noexcept {
	return std::isfinite(db) ? std::pow(10.0f, db / 20.0f) : 1.0f;
}

/// THE FLOOR AS A LINEAR LEVEL — 0 at the bottom of its travel.
[[nodiscard]] inline float floorOf(float db) noexcept {
	if (!std::isfinite(db) || db <= static_cast<float>(kFloorDbMin) + 1.0e-4f) {
		return 0.0f;
	}
	return std::pow(10.0f, db / 20.0f);
}

/// THE DETECTOR'S MENU, in `Detector`'s order. Audio offers the first two (`kAudioDetectorNames`).
inline constexpr const char* const kDetectorNames[] = { "Peak", "RMS", "Signed" };
inline constexpr const char* const kAudioDetectorNames[] = { "Peak", "RMS" };
inline constexpr const char* const kModeNames[] = { "Control", "Audio" };
inline constexpr const char* const kOffOnNames[] = { "Off", "On" };
static_assert(std::size(kDetectorNames) == static_cast<size_t>(Detector::kCount));
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "mode", .name = "Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kModeNames, .modulatable = false,
	  .tooltip = "Control follows its Input and may reach anything. Audio follows the audio here and may reach only "
	             "what is after it",
	  // A FLIP DELETES ROUTES, SO NO DAW LANE MAY MOVE IT MID-SONG: never on a host slot.
	  .hostSlot = false },
	{ .key = "input", .name = "Input", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "The control signal it follows in Control — rests at 0; route a source into it", .centred = true },
	{ .key = "detector", .name = "Detector", .min = 0.0, .max = 2.0, .def = static_cast<double>(Detector::Signed),
	  .stepped = true, .unit = device::Unit::Choice, .names = kDetectorNames, .modulatable = false,
	  .tooltip = "Signed lags the signal as it is, negative too. Peak follows the louder ear's magnitude; RMS the mean "
	             "power (a full-scale sine reads 71%)" },
	{ .key = "attack", .name = "Attack", .min = kAttackLog2Min, .max = kAttackLog2Max, .def = kAttackLog2Default,
	  .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	  .tooltip = "How fast the envelope rises — the time to cover 63% of a step. At the bottom it does not smooth" },
	{ .key = "release", .name = "Release", .min = kReleaseLog2Min, .max = kReleaseLog2Max, .def = kReleaseLog2Default,
	  .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
	  .tooltip = "How slowly the envelope falls — the time to cover 63% of a drop" },
	{ .key = "gain", .name = "Gain", .min = -24.0, .max = 24.0, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
	  .tooltip = "Scales the envelope before the floor — raise it for a quiet source" },
	{ .key = "floor", .name = "Floor", .min = kFloorDbMin, .max = 0.0, .def = kFloorDbMin, .stepped = false,
	  .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
	  .tooltip = "The level where the envelope leaves zero; below it the output rests. At the bottom: no floor. Not "
	             "under Signed" },
	{ .key = "threshold", .name = "Threshold", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Trig fires when the envelope rises to this line, and re-arms once it falls below half of it" },
	{ .key = "poly", .name = "Poly", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false,
	  .tooltip = "On: one per voice, each following its own voice where it sits. Off: one, following the bus here" },
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true,
	  .tooltip = "Bypass switch — off: both outputs rest at 0" },
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// **THE ROW, DECODED** — every control clamped to its declared range, in its STORED units (log2 seconds, dB), so a
/// value inside `[min, max]` comes back as itself (`test_device_ranges`). The core is handed the derived values.
struct Settings {
	Mode mode = Mode::Control;
	float input = 0.0f;
	/// AS STORED — what the card shows.
	Detector detector = Detector::Signed;
	float attackLog2 = static_cast<float>(kAttackLog2Default);
	float releaseLog2 = static_cast<float>(kReleaseLog2Default);
	float gainDb = 0.0f;
	float floorDb = static_cast<float>(kFloorDbMin);
	float threshold = 0.5f;
	bool poly = false;
	bool enabled = true;

	/// **WHAT THE CORE RUNS**: Signed is Control's, and under Audio it runs as Peak (the header).
	[[nodiscard]] Detector running() const noexcept {
		return mode == Mode::Audio && detector == Detector::Signed ? Detector::Peak : detector;
	}
};

/// DECODE A ROW: an absent or NaN field reads as its default, a choice is rounded into its menu.
[[nodiscard]] inline Settings settingsOf(const float* fields, size_t count) noexcept {
	const auto at = [&](Param p) {
		const auto i = static_cast<size_t>(p);
		const device::ParamDesc& desc = kParamDescs[i];
		const auto def = static_cast<float>(desc.def);
		const float v = i < count && fields != nullptr && !std::isnan(fields[i]) ? fields[i] : def;
		return std::clamp(v, static_cast<float>(desc.min), static_cast<float>(desc.max));
	};
	const auto choice = [&](Param p) { return static_cast<int>(std::lround(at(p))); };
	Settings s;
	s.mode = static_cast<Mode>(choice(Param::Mode));
	s.input = at(Param::Input);
	s.detector = static_cast<Detector>(choice(Param::Detector));
	s.attackLog2 = at(Param::Attack);
	s.releaseLog2 = at(Param::Release);
	s.gainDb = at(Param::Gain);
	s.floorDb = at(Param::Floor);
	s.threshold = at(Param::Threshold);
	s.poly = choice(Param::Poly) != 0;
	s.enabled = choice(Param::Enabled) != 0;
	return s;
}

/// **WHETHER THIS ROW READS ITS AUDIO** — Audio mode (`ModOps::readsAudioOf`).
[[nodiscard]] inline bool readsAudioOf(const float* fields, size_t count) noexcept {
	return settingsOf(fields, count).mode == Mode::Audio;
}

/// **ENV AND TRIG.** Trig is a one-tick pulse to 1 (the MV convention), emitted unsmoothed: route it into a Trig at
/// depth 1 and every crossing fires it once. Env is bipolar under Signed alone (`bipolarChoice`). PER VOICE under
/// Poly in Audio: a destination in a voice reads that voice's follower.
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "env", .name = "Env", .bipolar = true, .bipolarControl = static_cast<int>(Param::Detector),
	  .bipolarChoice = static_cast<int>(Detector::Signed), .perVoice = true },
	{ .key = "trig", .name = "Trig", .bipolar = false, .perVoice = true },
};
static_assert(std::size(kOutputs) == static_cast<size_t>(Output::kCount));

/// RETIRED — never reuse.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "source", "2026-10-26",
	  "the Sidechain source is gone: the sidechain enters the chain at a Sidechain Inject device and a Follower in "
	  "Audio mode after it follows it" },
};

inline constexpr const char* kTags[] = { "follower", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "FOLLOWER",
	.name = "Follower",
	.hasCard = true,
	.hasExpanded = false,
	.perVoiceControl = static_cast<int>(Param::Poly),
	.perVoiceRequires = static_cast<int>(Param::Mode),
	.perVoiceRequiresWhy = "Poly follows each voice's audio — switch Mode to Audio",
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
	// SCHEMA 2 (backlog OP, 2026-10-27): Env is declared bipolar, which the ledger counts as a meaning change of
	// `env`. THE MIGRATION IS THE IDENTITY: Env is bipolar only under the new Signed detector, so a schema-1 row (Peak
	// or RMS, stored by label) publishes the 0..1 it always did, and a route's amount on it means what it meant.
	.schema = 2,
	// A MODULATOR THAT LISTENS, NOT AN EFFECT: no chain access site (backlog NR).
	.siteLanes = device::Traits::SiteLanes::None,
};

extern const device::AudioOps kAudioOps;
extern const device::ModOps kModOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.audio = &kAudioOps,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::follower
