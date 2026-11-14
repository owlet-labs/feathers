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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "fx/automod/automod.h"

#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "fx/disperser/disperser_types.h"
#include "fx/comp/comp_types.h"
#include "fx/delay/delay_types.h"
#include "fx/pitch/pitch_types.h"
#include "fx/harm/harm.h"
#include "fx/harm/harm_types.h"
#include "fx/noise/noise_types.h"
#include "fx/phaser/phaser_types.h"
#include "fx/eroder/eroder_types.h"
#include "fx/eq/eq_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/clip/clip_types.h"
#include "fx/drawn/drawn_types.h"
#include "fx/spectral/spectral_types.h"
#include "fx/filter/filter_types.h"
#include "fx/waypoint/waypoint_types.h"
#include "fx/busutil/busutil_types.h"
#include "fx/filter/multimode_filter.h"
#include "fx/shaper/shaper.h"
#include "fx/sklitter/sklitter.h"
#include "plugin/device_registry.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "rack/site_chains.h"
#include "plugin/device_row.h"
#include "plugin/fx_chain.h"
#include "backplane/graph.h"
#include "composite/composite_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

// EVERY AUDIO DEVICE IN THE REGISTRY, ASKED THE SAME QUESTIONS — reset, partition invariance, finite
// output under extreme settings, the sample rate, note and mono roads, tails, and whether every
// declared control changes the sound. Each device is built alone, the way a host builds it (`Raw`),
// so what fails here is the device and not whatever runs it.
//
// THIS FILE WAS `FxRunner`'s SUITE, and the runner is gone: Feathers' fx lane runs on the rack engine
// (`plugin/lane_rack.h`). Every claim this file made about the LANE — adoption without allocation,
// parking, identity, the per-voice arrangement, reorder, the connector, the clock — moved to
// `tests/test_lane_rack.cpp`, asked of the lane that replaced it. What is left is about the devices.

namespace {

using namespace feathers;

/// EVERY DEVICE IN THE REGISTRY THAT CAN RENDER, in registry order. The sweeps below are built out of
/// this rather than out of named devices, so a device added to the registry is exercised here without
/// anybody remembering to come back.
std::vector<const device::Descriptor*> audioDevices() {
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& d : kDevices) {
		// A LISTENER RENDERS NOTHING (`AudioOps::listensOnly`): every claim below would be true of it as of a wire.
		if (d.audio != nullptr && !d.audio->listensOnly) {
			out.push_back(&d);
		}
	}
	return out;
}

} // namespace

//==================================================================================================
// THE SWEEPS — every device in the registry asked the same question, from the registry
//==================================================================================================

namespace {

/// ONE DEVICE, BUILT THE WAY THE HOST BUILDS IT, in storage this harness owns.
///
/// THROUGH `setFields` AND A REAL ROW, never by touching a DSP object directly. `test_fx_drawn.cpp`
/// records what the shortcut costs: a scratch probe that drove the DSP straight read a flat trace
/// where the row road read 102.9 dB, because `stateOf` sanitises on the way in. **A probe that
/// bypasses the ingest guard is measuring a device the host cannot ask for.**
struct Raw {
	const device::AudioOps* ops = nullptr;
	void* mem = nullptr;
	/// THE PLAYHEAD AND THE RATE IT MOVES AT — see `process`. Declared here because the constructor's
	/// initialiser list touches `rate_`, and a member initialised out of declaration order is a
	/// warning this build treats as an error.
	double rate_ = 48000.0;
	double beats_ = 0.0;
	/// THE FURNITURE'S BLEND THE ROW ASKS FOR (backlog NR) — see `fields`. Declared before the constructor touches it.
	const device::Descriptor* desc_ = nullptr;
	float wet_ = 1.0f;
	float dry_ = 0.0f;

	// A DEVICE THAT HOLDS DEVICES IS BUILT THE WAY THE HOST BUILDS IT, WHICH INCLUDES HANDING IT A
	// CATALOGUE — the rule every host obeys (`ContainerOps::adopt`). The rack is swept EMPTY, because that
	// is what a rack just added to a lane is; what an empty rack cannot show — a macro changing the sound
	// — is recorded in the honesty sweep's list below with the cases that measure it.

	// `std::aligned_alloc` RATHER THAN ALIGNED `operator new` — chosen while this file replaced the
	// global allocator, where g++-14 -O3 raised a false -Werror=mismatched-new-delete against the
	// other form. Kept: it is the storage a host gives a device, which is all this needs.
	Raw(const device::Descriptor& desc, double rate, const DeviceRow& row) : ops(desc.audio), rate_(rate), desc_(&desc) {
		REQUIRE(ops != nullptr);
		const size_t align = std::max<size_t>(ops->stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((ops->stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		ops->construct(mem);
		ops->prepare(mem, rate, 4096);
		if (desc.container != nullptr && desc.container->adopt != nullptr) {
			desc.container->adopt(mem, hostCatalogue());
		}
		fields(row);
	}
	~Raw() {
		ops->destruct(mem);
		feathers::test::heapFree(mem);
	}
	Raw(const Raw&) = delete;
	Raw& operator=(const Raw&) = delete;

	void fields(const DeviceRow& row) {
		ops->setFields(mem, row.field.data(), row.fieldsPresent);
		// **AND THE SLOT'S FURNITURE** (backlog NR): a device whose Mix is the slot's hands back its wet, and every host
		// blends it — so "alone, the way a host builds it" blends it too, at the row's Mix and wet Gain.
		const int mixAt = desc_->traits.mixControl;
		wet_ = 1.0f;
		dry_ = 0.0f;
		if (!desc_->traits.mixInDevice && mixAt >= 0) {
			const auto at = [&](int c, float fallback) {
				return c >= 0 && static_cast<size_t>(c) < row.fieldsPresent ? row.field[static_cast<size_t>(c)] : fallback;
			};
			const float mix = std::clamp(at(mixAt, static_cast<float>(desc_->params[static_cast<size_t>(mixAt)].def)), 0.0f, 1.0f);
			const float p = std::clamp(at(desc_->traits.wetGainControl, 0.5f), 0.0f, 1.0f);
			wet_ = mix * ((2.0f * p) * (2.0f * p));
			dry_ = 1.0f - mix;
		}
	}
	void reset() {
		ops->reset(mem);
		beats_ = 0.0;
	}

	/// **THE PLAYHEAD MOVES, AND UNTIL 2026-09-15 IT DID NOT.** This fixture never called
	/// `setTransportClock`, so every device in every sweep here ran with no tempo and a playhead
	/// frozen at beat zero. For a device that only filters or shapes, that is harmless. For a device
	/// that WAITS FOR THE GRID it is fatal and silent: SKLITTER's looper modes arm on a beat boundary,
	/// so a still playhead leaves them in Standby for ever — `test_fx_sklitter.cpp`'s own `drive` helper
	/// carries that note, and this file did not have the equivalent.
	///
	/// **IT SURFACED THE DAY THE DEFAULT MODE CHANGED**, because the old default needed no grid: it
	/// captured a division and played. Three sweeps went silent at once and reported the device as producing
	/// nothing — which is exactly what a looper does when nothing ever tells it a beat has passed.
	///
	/// 120 BPM, the same assumption `Sklitter::kAssumedSecondsPerBeat` makes and the same one the rest
	/// of this repo's fixtures use.
	/// THE CLOCK THIS CALL SEES, and the step it takes afterwards. Split out of `process` because the
	/// MONO road is driven directly by one case and must see the same playhead — two roads reading two
	/// different transports is not a difference between the roads, it is a difference between the
	/// fixtures, and the case comparing them cannot tell.
	void tickClock(int frames) {
		if (ops->setTransportClock != nullptr) {
			device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(0.5);
			clock.songPositionBeats = beats_;
			clock.playing = true;
			ops->setTransportClock(mem, clock);
		}
		beats_ += static_cast<double>(frames) / (0.5 * rate_);
	}

	/// **A LONG CALL IS BROKEN INTO BLOCKS, BECAUSE A HOST NEVER MAKES ONE.** Some cases here hand the
	/// device 105,600 frames at once so a looper has time to fill its bar. With the clock set once for
	/// the whole call the playhead never MOVES inside it, and a device that arms on a grid line — every
	/// looper mode SKLITTER has — waits for a beat that cannot arrive. `touched == 0`, reported as "the
	/// device is a wire at these settings", when what it actually was is a device still waiting.
	///
	/// **THE CHUNK IS NOT A SECOND PARTITION.** A caller's own partition is preserved: a 256-frame call
	/// is one chunk of 256, a 40-frame call one chunk of 40. This only subdivides calls LONGER than a
	/// chunk, which no host makes, so the partition sweep still sees whatever partition dependence the
	/// device really has. 4800 is a tenth of a beat at 120 BPM and 48 kHz — `test_fx_sklitter.cpp`'s
	/// `drive` uses the same number for the same reason.
	void process(float* l, float* r, int frames) {
		constexpr int kChunk = 4800;
		for (int done = 0; done < frames; done += kChunk) {
			const int n = std::min(kChunk, frames - done);
			tickClock(n);
			const std::vector<float> dryL = wet_ != 1.0f || dry_ != 0.0f ? std::vector<float>(l + done, l + done + n) : std::vector<float> {};
			const std::vector<float> dryR = dryL.empty() ? std::vector<float> {} : std::vector<float>(r + done, r + done + n);
			ops->processStereo(mem, l + done, r + done, n);
			for (size_t i = 0; i < dryL.size(); ++i) { // the slot's furniture (backlog NR), as `fields` aimed it
				l[static_cast<size_t>(done) + i] = l[static_cast<size_t>(done) + i] * wet_ + dryL[i] * dry_;
				r[static_cast<size_t>(done) + i] = r[static_cast<size_t>(done) + i] * wet_ + dryR[i] * dry_;
			}
		}
	}


};

/// THE DEVICE'S NAME AS A STRING, because a bare `const char*` reaches doctest's stringifier as a
/// pointer and the failure line reads `desc->traits.name := 1`. The whole value of a registry sweep
/// is that a failure says WHICH device, so the conversion is not cosmetic.
std::string deviceName(const device::Descriptor& desc) {
	return desc.traits.name == nullptr ? std::string("<unnamed>") : std::string(desc.traits.name);
}

/// A SIGNAL WITH SOMETHING AT EVERY FREQUENCY AND NO SILENCE ANYWHERE, so a device that only touches
/// part of the band still shows up. Deterministic, and written out here rather than drawn from a
/// generator so two runs of this file are the same two runs.
std::vector<float> probeSignal(size_t n, float amplitude, uint32_t seed) {
	std::vector<float> out(n, 0.0f);
	uint32_t s = seed;
	for (size_t i = 0; i < n; ++i) {
		s = s * 1664525u + 1013904223u;
		const float noise = static_cast<float>(static_cast<int32_t>(s >> 8) % 2001 - 1000) / 1000.0f;
		const auto t = static_cast<double>(i);
		out[i] = amplitude
		         * (0.6f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.011 * t))
		            + 0.4f * noise);
	}
	return out;
}

/// EVERY AUDIO DEVICE'S DEFAULT ROW WITH ITS ENABLE SWITCH ON.
///
/// A DEVICE THAT IS OFF IS A WIRE, and a sweep run against wires passes for every device and proves
/// nothing — the vacuity the "two refusals compare equal" entry in the playbook is about.
///
/// ASKED OF `enableControlIn`, NOT OF THE CONTROL'S NAME. The first draft of this searched
/// `params[i].name` for "Enable", which `device.h` names as the rejected alternative and says was
/// *"recorded here so it is not re-proposed"* — it breaks on a rename, breaks on a device that
/// spells it differently, and makes a display string load-bearing. The declaration is an index, and
/// `enableControlIn` is the refusal that goes with it: an index past the end is `kNoControl` rather
/// than trusted.
//--------------------------------------------------------------------------------------------------//
// TWO THINGS THESE SWEEPS HAVE TO KNOW ABOUT ONE DEVICE, AND WHY THEY LIVE HERE.                     //
//--------------------------------------------------------------------------------------------------//
//
// **SKLITTER IS A MOMENTARY EFFECT AND EVERY OTHER DEVICE IN THIS REGISTRY IS ALWAYS-ON.** It does
// nothing until its gesture is raised, and once raised it captures a whole slice before it plays a
// sample. So the sweeps below are blind to it twice over: their row leaves the gesture down, and
// their probe is shorter than the slice. Both would pass it VACUOUSLY -- "reset cleared it" and "it
// is not partition-dependent" are true of a wire.
//
// THIS KNOWLEDGE IS IN THE TEST LAYER ON PURPOSE, beside `kCodecs`, which is here for the same
// reason and says so: putting it in the device contract would make every third-party guest implement
// a hook that exists for a sweep. Neither changes what ships.
//
// **AND THE PROBE IS LENGTHENED ONLY FOR THE DEVICE THAT NEEDS IT.** MEASURED: this binary already
// takes 69 s, and giving every device SKLITTER's probe would be 12x to 47x of that -- past the
// container's own command bound. Only the device that needs the frames pays for them.

/// THE CONTROL THAT MUST BE RAISED FOR THIS DEVICE TO DO ANYTHING AT ALL, beyond its bypass.
/// `device::kNoControl` for every always-on device, which is all of them but two.
///
/// **CLIP'S IS ITS DRIVE.** At its defaults (Drive 0 dB, Ceiling 0 dBFS, a knee starting at 0.9) the
/// sweeps' probes never reach the curve, so every sweep measured a unity pass-through and the audibility
/// sweep reported Freq, Foldback, Grit, Depth and Bits dead: each needs Mode AND a signal over the knee,
/// and the arrangements move one control at a time. Raised to +24 dB the probe clips in every mode. The
/// shipped default stays 0 dB — a clipper added to a chain must not change the sound until asked.
size_t gestureControlIn(const device::Descriptor& desc) {
	if (device::sameTag(desc.traits.tag, fx::sklitter::kTraits.tag)) {
		return static_cast<size_t>(fx::sklitter::Param::Engage);
	}
	if (device::sameTag(desc.traits.tag, fx::clip::kTraits.tag)) {
		return static_cast<size_t>(fx::clip::Param::Drive);
	}
	return device::kNoControl;
}

/// **THE OPERATOR SWITCHES A DEVICE IS SILENT-IN-EFFECT WITHOUT**: Spectral at its defaults is every operator off, the
/// input delayed by its latency to the bit, so every sweep would measure a delay line. Its Hz operators are switched on
/// here, as Clip's Drive is raised above. Odd/Even is left off: it needs a note, and these sweeps are the mix. The shipped
/// default stays every operator off — the device changes nothing until asked.
std::vector<size_t> operatorSwitchesIn(const device::Descriptor& desc) {
	if (device::sameTag(desc.traits.tag, fx::spectral::kTraits.tag)) {
		using fx::spectral::Param;
		// F2: FILTER 2 TOO, so its controls (its Gain under its own Mode's Bell) are swept as filter 1's are.
		return { static_cast<size_t>(Param::FilterOn), static_cast<size_t>(Param::Filter2On), static_cast<size_t>(Param::CombOn),
		         static_cast<size_t>(Param::VowelOn), static_cast<size_t>(Param::TiltOn) };
	}
	return {};
}

/// HOW MANY FRAMES BEFORE THIS DEVICE CAN BE HEARD.
///
/// **SKLITTER'S ANSWER CHANGED WITH ITS DEFAULT MODE** (the default mode is now Shuffle). In
/// Burst it captures a DIVISION — 6000 frames at its default rate, 120 BPM and
/// 48 kHz — and 8192 clears it. Shuffle is a looper: it captures a whole BAR, four beats, 96,000
/// frames, and then waits for a beat boundary to trigger. Three sweeps in this file went silent the
/// moment the default moved, each reporting the device as producing nothing, which is what a looper
/// two seconds into its first capture genuinely does.
///
/// **A BAR AND A BEAT, not a round number that happened to work.** `96000 + 9600` is what
/// `test_fx_sklitter.cpp` drives for the same reason and for the same device; a margin chosen by
/// experiment would go quietly wrong the day the bar length does.
int framesFor(const device::Descriptor& desc, int base) {
	// **SPECTRAL IS ITS LATENCY LATE, AND ITS WET ENGAGES ONLY ONCE FOUR FRAMES OVERLAP** (spectral_stft.h): at 48 kHz on
	// its default stop that is L = N = 480 frames, then a hop of crossfade, then L again before the wet reaches the output.
	// Four latencies clear all three.
	if (device::sameTag(desc.traits.tag, fx::spectral::kTraits.tag)) {
		return std::max(base, 4 * static_cast<int>(fx::spectral::latencyFor(fx::spectral::Resolution::Balanced, 48000.0)));
	}
	if (device::sameTag(desc.traits.tag, fx::sklitter::kTraits.tag)) {
		return std::max(base, 96000 + 9600);
	}
	// **THE DELAY NEEDS TWO TAPS IN THE WINDOW, NOT ONE, AND THE SECOND IS WHAT FEEDBACK MEANS.**
	// The first tap is the same sample whatever `Delay Feedback` is set to — feedback only decides
	// what goes back INTO the line, so a probe one tap long reports the device's most important
	// control dead. Two taps and the control is the difference between them.
	//
	// **DERIVED FROM THE SLOWER OF THE TWO CLOCKS THESE SWEEPS RUN, which is the one with no clock at
	// all.** The default sync is `1/8`, half a beat. The audibility sweep hands the device 120 BPM,
	// where that is 0.25 s; the reset, partition and silence sweeps hand it NOTHING, and
	// `fx::delay::tapSeconds` then falls back to one second a beat — so the same control is 0.5 s
	// there, twice as long. Sizing from the 120 BPM figure would leave three sweeps half a tap short,
	// which is the shape of bug this whole function exists to prevent.
	//
	//     0.5 s x 48 kHz = 24,000 frames a tap; two taps is 48,000; plus a tap of margin.
	if (device::sameTag(desc.traits.tag, fx::delay::kTraits.tag)) {
		return std::max(base, 48000 + 4800);
	}
	// **THE PITCH SHIFTER READS FROM BEFORE A SHORT PROBE BEGAN.** Shifting UP, a head is seated
	// `(ratio - 1)` grains behind the write position so that it cannot overtake it, and Feedback adds
	// a grain of loop delay on top. `variedRowFor` lands on +8 semitones, a 68 ms grain and Feedback
	// up: the first wet sample is ~1.6 grains in, about 5,200 frames — so a 256-frame probe hears the
	// dry signal scaled by `Mix` and nothing else, and reports Size, Jitter, Mode and Feedback dead.
	// Five grains: the seat, a lap of the loop, and margin.
	if (device::sameTag(desc.traits.tag, fx::pitch::kTraits.tag)) {
		return std::max(base, 16384);
	}
	// **THE DISPERSER'S ECHOES ARRIVE AFTER A 256-FRAME PROBE ENDS.** Punch and Chirp write ahead into a line
	// folded to 10-200 ms — under the fixture's note 84 the echo is 7,096 frames out — so their pills (and 2f's) read
	// as dead. 8,192 frames holds that first echo.
	if (device::sameTag(desc.traits.tag, fx::disperser::kTraits.tag)) {
		return std::max(base, 8192);
	}
	// **THE SHAPER NEEDS ONE TABLE BUILD IN THE WINDOW, or every control that only shapes the TABLE is
	// reported dead** (backlog IE: Cheby and Stair, which change nothing but the curve). A fresh device
	// reads the identity until its first build lands — 1025 entries at two a frame — and then crossfades
	// over 512 samples: ~1025 frames before the new curve is wholly heard, four times the base window.
	// Colour and Knee were never caught by this because each also moves something per-sample.
	if (device::sameTag(desc.traits.tag, fx::shaper::kTraits.tag)) {
		return std::max(base, 8192);
	}
	// **FEATHERVERB'S PREDELAY LANDS AT 0.6 UNDER `variedRowFor`**: 30 ms, 1440 frames at 48 kHz and 2880 at
	// 96 kHz, before anything but the dry comes out. 512 frames of it is a wire at both rates. 4096 holds the
	// first reflections at both.
	if (device::sameTag(desc.traits.tag, fx::featherverb::kTraits.tag)) {
		return std::max(base, 8192);
	}
	// **RESONATE'S STRING MOVES A FEW HUNDRED TIMES A SECOND, AND ITS NOTES ARRIVE BETWEEN CALLS.** A fresh
	// string is still, and the tone is played by its motion: nothing the input or the bow does is heard
	// before the string's first tick (~139 frames at 48 kHz) has moved it, and a note head takes 5 ms to
	// open. The honesty sweep also plays it two notes a third and two thirds of the way in (see
	// `renderWith`), so each third needs a few ticks of its own. 2048 frames is fifteen ticks.
	if (device::sameTag(desc.traits.tag, fx::resonate::kTraits.tag)) {
		return std::max(base, 2048);
	}
	// **THE FLANGER IS 8 MS LATE, THROUGH ZERO ON OR OFF** (`phaser::latencySamplesFor`: a latency Through Zero
	// cannot move, so flipping it asks the host for no restart), and its read sits up to 20 ms further in. A
	// 256-frame probe hears only the latent dry, and reports Delay and Through Zero dead. 4096 holds both at 96 kHz.
	if (device::sameTag(desc.traits.tag, fx::phaser::kTraits.tag)) {
		return std::max(base, 4096);
	}
	// **NOISE'S CLICKS ARE SPARSE, and a 256-frame probe holds none at its default eight a second** — every Impulses
	// control would read dead. 4096 frames hold about twenty at the 256 a second the honesty sweep's click bases use.
	if (device::sameTag(desc.traits.tag, fx::noise::kTraits.tag)) {
		return std::max(base, 4096);
	}
	return base;
}

/// HOW MANY SILENT BLOCKS BEFORE THIS DEVICE'S TAIL IS OVER — `framesFor`'s companion at the other
/// end of the render, and it exists for the same reason: a window too short to contain a device's
/// tail reports it as ringing forever, which is the same word the case uses for a real defect.
///
/// **A DELAY'S TAIL IS LONG BECAUSE A DELAY IS LONG, AND THAT IS NOT THE DEFECT THIS CASE HUNTS.**
/// The bug it was written for is a decay that STICKS — `PuckMotion` parked at 1.38e-322, OWLTT and
/// Waypoint parked at 9.5e-43 and 1.4e-45 and reading identically across thirty seconds. A delay at
/// `variedRowFor`'s settings is not parked; it is decaying at 0.66 per half-second tap through a
/// feedback loop, and it reaches EXACT zero when it gets there. The 400-block window is 4.3 s, which
/// is a fraction of one such tail.
///
/// **THE NUMBER IS MEASURED, NOT DERIVED, AND THE ARITHMETIC WAS WRONG BY A FACTOR OF TWO.**
/// Predicted from the decay alone: 0.8 x 0.66^k < 1e-7 needs 39 taps, ~19 s. MEASURED: it settles at
/// block **3715**, 39.6 s — twice that, because the tail is not only the line. `Delay Tone` lands at
/// 0.2 under `variedRowFor`, a 50 Hz highpass whose own pole (a = 0.9935 at 48 kHz) is far slower
/// than the loop and keeps re-exciting it. CLAUDE.md's rule, met head-on: the model had the shape
/// and the instrument had the number.
///
/// **9000 RATHER THAN 8000, because the case asserts settling by HALFWAY as well as settling at
/// all**, and 3715 against a 4000 halfway mark is a 7% margin — thin enough that a tone tweak would
/// turn a working device red. 4500 leaves 21%.
int silentBlocksFor(const device::Descriptor& desc, int base) {
	if (device::sameTag(desc.traits.tag, fx::delay::kTraits.tag)) {
		return std::max(base, 9000);
	}
	// **THE PITCH SHIFTER'S FEEDBACK IS A LOOP TOO, and the same distinction holds**: decaying and
	// reaching exact zero is not parked. MEASURED at `variedRowFor`'s settings (Feedback 57%, a
	// 68 ms grain, +8 semitones): the last non-zero block is **235**, against a halfway mark of 200.
	// 800 puts halfway at 400, a 41% margin. (At the knob's maximum the tail is 17.9 s, which
	// `test_fx_pitch.cpp` measures and bounds for itself.)
	if (device::sameTag(desc.traits.tag, fx::pitch::kTraits.tag)) {
		return std::max(base, 800);
	}
	// **FEATHERVERB ENDS BY THE REFERENCE'S OWN GATE, AT 12 s** (`featherverb_types.h`, `kGateSeconds`): the
	// engine stops a reverb 12 s after anything was last sent to it, once its level is under e^9. Its Owl
	// and Vast floors would otherwise never reach exact zero; the gate is what ends them on the hardware,
	// and it is ported. MEASURED at `variedRowFor`'s settings (the default mode, Feather): the last
	// non-zero block is **1124**, 12.0 s. 2600 puts halfway at 1300, a 16% margin.
	if (device::sameTag(desc.traits.tag, fx::featherverb::kTraits.tag)) {
		return std::max(base, 2600);
	}
	// **RESONATE'S TAIL IS ITS SYMPATHETIC DRONE RINGING DOWN** (2026-09-26): with no note held, the drone is
	// what reads the string, and it falls on its follower's decay once the input stops, cutting to exact zero
	// at its floor (-100 dB). Not parked on a denormal: it reaches zero when it gets there. MEASURED at
	// `variedRowFor`'s settings, with Bow at 0.7 and with Bow at 0 alike (the drone decides both): the last
	// non-zero block was **510**, 5.4 s; with v3's drone makeup (x6, +15.6 dB against the same -100 dB floor) it is
	// **1261**, 13.4 s (MEASURED 2026-09-26). 3200 puts halfway at 1600, a 21% margin.
	if (device::sameTag(desc.traits.tag, fx::resonate::kTraits.tag)) {
		return std::max(base, 3200);
	}
	// **THE DISPERSER'S CHIRP IS A LOOP, AND SINCE ITS PILLS DEFAULT ON (round 3, 2026-09-27) THIS ROW TURNS IT UP**:
	// `variedRowFor` takes every pill's amount from 0.5 to 0.8, and with the pills powered that lifts Punch and Chirp
	// toward their tops, where the capped chirp loop rings for seconds. It reaches EXACT zero, not a denormal floor —
	// MEASURED at this row: the last non-zero block is **775**, 8.3 s. 2000 puts halfway at 1000, a 29% margin.
	if (device::sameTag(desc.traits.tag, fx::disperser::kTraits.tag)) {
		return std::max(base, 2000);
	}
	return base;
}

DeviceRow enabledRowFor(const device::Descriptor& desc) {
	DeviceRow row = defaultRowFor(desc, { 1 });
	const size_t gesture = gestureControlIn(desc);
	if (gesture != device::kNoControl && gesture < row.fieldsPresent) {
		row.field[gesture] = static_cast<float>(desc.params[gesture].max);
	}
	for (const size_t s : operatorSwitchesIn(desc)) {
		row.field[s] = 1.0f;
	}
	const size_t at = device::enableControlIn(desc.traits, desc.params);
	// A DEVICE WITH NO SWITCH IS A FAILURE ON ITS OWN NAME, not a silently-measured bypass. Every fx
	// device declares exactly one today (device.h says so); the day one does not, this says which.
	REQUIRE_MESSAGE(at != device::kNoControl, "no enable control declared by ",
	                std::string(deviceName(desc)));
	REQUIRE(at < row.fieldsPresent);
	row.field[at] = static_cast<float>(desc.params[at].max);
	return row;
}

/// THE SAME DEVICE SOMEWHERE ELSE — every continuous control pushed a long way off its default, and
/// every stepped one left alone.
///
/// A SECOND SETTING IS WHAT MAKES A RESET VISIBLE AT ALL, and the first draft of the sweep below did
/// not have one. `AudioOps::reset` has two meanings and the weaker one — forget the delay line,
/// remember that you were running — is INDISTINGUISHABLE from the stronger one when the device is
/// reset back onto the settings it already had: the smoother is already parked on the target, so
/// snapping to it and ramping to it land in the same place. The case passed on its first run against
/// a knife that could not cut. The history has to leave the device somewhere it must come BACK from.
///
/// STEPPED CONTROLS ARE HELD because moving a Type or a Choice changes which DSP runs, which is a
/// different claim from "the state was forgotten" and would let a device pass by having nothing in
/// common between the two runs.
DeviceRow variedRowFor(const device::Descriptor& desc) {
	DeviceRow row = enabledRowFor(desc);
	int moved = 0;
	for (size_t i = 0; i < desc.params.size() && i < row.fieldsPresent; ++i) {
		const device::ParamDesc& p = desc.params[i];
		if (p.stepped) {
			continue;
		}
		// **A DECLARED RANGE IS NOT ALWAYS A DOMAIN, AND 60 % OF A DRAG WINDOW IS NONSENSE.**
		//
		// This moved every continuous control to 60 % of its declared span. That is right for a
		// bounded coordinate and wrong for an UNBOUNDED counter, where the range is a drag window and
		// the value is read raw — `device.h` says so of automod's fines, and the shaper's Gamma and
		// Rotate joined them on 2026-09-04. MEASURED the day they did: 0.6 * (2^23) sets them to
		// **5,033,165**, where float32 spacing is 0.5 and the field's fractional coordinate is gone
		// entirely. The sweep was not asking "the same device somewhere else", it was asking for a
		// coordinate that cannot be represented.
		//
		// **CAPPED IN THE CONTROL'S OWN UNITS, not scaled**, because there is no factor that is right
		// for both kinds: `min(0.6 * span, kSweepExcursion)` reduces to the old behaviour for every
		// bounded param here (the widest is 64) and gives an unbounded one a few laps, which is what
		// "somewhere else" means for a counter. Same shape as `gamma_box.h`'s `min(laps, range)`, for
		// the same reason and stated in the same units.
		constexpr double kSweepExcursion = 8.0;
		const double reach = std::min(0.6 * (p.max - p.def), kSweepExcursion);
		const double away = p.def + reach;
		const double back = p.def - std::min(0.6 * (p.def - p.min), kSweepExcursion);
		const double to = (away == p.def) ? back : away;
		if (to != p.def) {
			row.field[i] = static_cast<float>(to);
			++moved;
		}
	}
	CHECK_MESSAGE(moved >= 1, "nothing continuous to move on ", std::string(deviceName(desc)));
	return row;
}

} // namespace

TEST_CASE("EVERY audio device: reset means AS IF JUST SWITCHED ON, not merely with the memory cleared") {
	// `AudioOps::reset`'s own words: *"FORGET THE STATE AND THE FACT THAT IT WAS RUNNING — 'as if just
	// switched on', not merely 'with the memory cleared'. The distinction is audible: a filter that
	// forgets its delay line but remembers it was active ramps its coefficients in from the last
	// note's."* That header also records how the weaker meaning got in: **the entry point had no
	// caller outside tests, and an uninterrogated capability drifts to whatever its implementor
	// guessed.**
	//
	// THE SUITE'S OWN HEADER LISTS THIS AS OBLIGATION 4 and the case above discharges it FOR THE
	// FILTER. This asks every device in the registry, driven from the registry — a hardcoded list
	// inside a sweep is the thing that stops sweeping the day a device ships.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	// A FLOOR, so "the registry returned nothing" is a failure rather than a silent pass.
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 256;

	for (const device::Descriptor* desc : devices) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		// PER DEVICE, because one of them cannot be heard inside 256 frames. See `framesFor`.
		const int frames = framesFor(*desc, kFrames);
		const std::vector<float> probe = probeSignal(frames, 0.25f, 0x51ed1234u);
		// LOUD AND DIFFERENT, so whatever the device remembers is worth remembering.
		const std::vector<float> history = probeSignal(frames, 0.9f, 0xbeef0007u);
		const DeviceRow row = enabledRowFor(*desc);

		const DeviceRow elsewhere = variedRowFor(*desc);

		// THE HISTORY IS A DIFFERENT SETTING, NOT JUST A LOUD SIGNAL. See `variedRowFor`: the weaker
		// reading of `reset` is invisible against a device reset onto the settings it already had.
		const auto run = [&](bool withHistory, bool withReset) {
			Raw dev(*desc, 48000.0, withHistory ? elsewhere : row);
			std::vector<float> l = probe;
			std::vector<float> r = probe;
			if (withHistory) {
				std::vector<float> hl = history;
				std::vector<float> hr = history;
				dev.process(hl.data(), hr.data(), frames);
			}
			if (withReset) {
				dev.reset();
			}
			if (withHistory) {
				dev.fields(row);
			}
			dev.process(l.data(), r.data(), frames);
			return l;
		};

		const std::vector<float> fresh = run(false, false);
		const std::vector<float> carried = run(true, false);
		const std::vector<float> afterReset = run(true, true);

		// NON-VACUITY FIRST, and it is the assertion that makes the next one mean anything: if a
		// device's history does not reach the probe at all, "reset cleared it" is true of a wire.
		int carriedDiffs = 0;
		for (int i = 0; i < frames; ++i) {
			if (carried[i] != fresh[i]) {
				++carriedDiffs;
			}
		}
		CHECK_MESSAGE(carriedDiffs > 0, "history does not reach the probe on ", who,
		              " — this sweep cannot see a reset on it");

		int resetDiffs = 0;
		float worst = 0.0f;
		for (int i = 0; i < frames; ++i) {
			if (afterReset[i] != fresh[i]) {
				++resetDiffs;
				worst = std::max(worst, std::fabs(afterReset[i] - fresh[i]));
			}
		}
		CAPTURE(worst);
		CHECK_MESSAGE(resetDiffs == 0, "reset did not restore the just-switched-on state on ", who);
	}
}

TEST_CASE("EVERY audio device: the same samples in a different partition are the same samples out") {
	// **WHERE THE HOST CUTS ITS BLOCK IS NOT A SETTING.** A device is handed N samples; how many
	// calls they arrive in is the host's business and a CLAP host cuts at every event, so the same
	// passage arrives in a different partition every time it is played. A device whose output depends
	// on the partition sounds different at a different buffer size, and there is nothing the user can
	// turn to get the other sound back.
	//
	// IT IS NOT A HYPOTHETICAL HERE. `DrawnFilter::processStereo` restarted its control chunking at
	// every call until 9137b44, which ran its 5 ms smoother at 1.25 ms on eight-sample blocks —
	// measured 0.414467 against a contract value of 0.274006. HARM carries `gridPhase_` and
	// `MultimodeFilter` carries `untilTick_` for exactly this; drawn carried nothing, and no case saw
	// it because every smoothing case in `test_fx_drawn.cpp` drove at exactly `kControlBlock`, the one
	// size at which an absolute grid and a per-call one agree.
	//
	// THE SMOOTHER HAS TO BE TRAVELLING or this asks nothing. A device parked on its target gives the
	// same answer under any partition however it counts, so the fixture drives a warm-up at one
	// setting, changes to another, and measures the journey.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kWarm = 64; // a whole number of every control grid in the registry
	constexpr int kFrames = 256;
	const std::vector<float> warm = probeSignal(kWarm, 0.4f, 0x0badc0deu);
	const std::vector<float> probe = probeSignal(kFrames, 0.25f, 0x51ed1234u);
	std::vector<std::string> unseenPartition;

	// THE PARTITIONS. The uneven one is the realistic case rather than the exotic one: a host cuts at
	// every event, so 40 + 24 + 56 + 8 + 128 is what a bar of music looks like from inside a device.
	const std::vector<std::vector<int>> partitions = {
		{ kFrames },
		{ 32, 32, 32, 32, 32, 32, 32, 32 },
		{ 8,  8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
		  8,  8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 },
		{ 40, 24, 56, 8, 128 },
	};

	std::vector<std::string> dependent;
	for (const device::Descriptor* desc : devices) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		const DeviceRow row = enabledRowFor(*desc);
		const DeviceRow elsewhere = variedRowFor(*desc);

		// **THE WARM-UP IS PER DEVICE, not the measured window.** The partitions are built from
		// `kFrames` and must keep summing to it, so the lever is how long the device has been running
		// BEFORE the measurement starts -- warm it past its first slice and it is travelling by the
		// time the probe arrives, which is what this case needs of every device.
		const int warmFrames = framesFor(*desc, kWarm);
		const std::vector<float> warmed = probeSignal(warmFrames, 0.4f, 0x0badc0deu);

		const auto run = [&](const std::vector<int>& parts) {
			Raw dev(*desc, 48000.0, elsewhere);
			// WARM UP AT THE OTHER SETTING, IN ONE CALL FOR EVERY PARTITION, so the run being compared
			// starts from an identical state and an identical grid phase. 64 is a whole number of every
			// control block in the registry (32 and 16), so the phase is zero for all of them.
			std::vector<float> wl = warmed;
			std::vector<float> wr = warmed;
			dev.process(wl.data(), wr.data(), warmFrames);
			// AND NOW IT IS TRAVELLING.
			dev.fields(row);
			std::vector<float> l = probe;
			std::vector<float> r = probe;
			int done = 0;
			for (const int take : parts) {
				dev.process(l.data() + done, r.data() + done, take);
				done += take;
			}
			REQUIRE(done == kFrames);
			return l;
		};

		const std::vector<float> reference = run(partitions.front());
		// NON-VACUITY: the journey has to reach the output, or "every partition agrees" is a claim
		// about a wire. Compared against the same device parked on `row` from the start, which is
		// where the smoother is heading.
		{
			Raw parked(*desc, 48000.0, row);
			std::vector<float> wl = warmed;
			std::vector<float> wr = warmed;
			parked.process(wl.data(), wr.data(), warmFrames);
			std::vector<float> l = probe;
			std::vector<float> r = probe;
			parked.process(l.data(), r.data(), kFrames);
			int travelling = 0;
			for (int i = 0; i < kFrames; ++i) {
				if (l[i] != reference[i]) {
					++travelling;
				}
			}
			// **THE SET THIS SWEEP CANNOT SEE, SPELLED WHOLE** rather than a per-device CHECK with
			// nowhere to record that the blindness is known. A device joining it is a regression and
			// a device LEAVING it means this comment has stopped being true -- the same shape as the
			// block-size exemption below.
			//
			// **THE SET IS EMPTY, AND SKLITTER LEAVING IT IS THIS ASSERTION DOING ITS JOB.** It read:
			// *"SKLITTER HAS NO SMOOTHER. Its controls are read ONCE, at the press, and the slice then
			// plays from a captured buffer: there is nothing travelling between two settings for a
			// partition to disagree about."* That was an accurate description of a DEFECT: on the
			// installed build, turning Rate did not change a running scatter.
			// The rate is meant to be read every block, with only the captured LENGTH fixed at the
			// press; the port froze both. With the read speed live, two
			// settings do differ downstream of the press and this sweep can see the device.
			//
			// SO NOTHING IS EXEMPT NOW. The spelling stays whole for the reason it was whole before:
			// a device joining this list is a regression, and it is the sentence above that caught a
			// comment which had stopped being true.
			if (travelling == 0) {
				unseenPartition.push_back(who);
			}
		}

		const auto peakOf = [](const std::vector<float>& v) {
			float p = 0.0f;
			for (const float x : v) {
				p = std::max(p, std::fabs(x));
			}
			return p;
		};

		int worstPartition = 0;
		float worst = 0.0f;
		float worstPeak = 0.0f;
		for (size_t p = 1; p < partitions.size(); ++p) {
			const std::vector<float> other = run(partitions[p]);
			for (int i = 0; i < kFrames; ++i) {
				const float d = std::fabs(other[i] - reference[i]);
				if (d > worst) {
					worst = d;
					worstPartition = static_cast<int>(p);
					worstPeak = peakOf(other);
				}
			}
		}
		if (worst != 0.0f) {
			dependent.push_back(who);
			CAPTURE(worstPartition);
			CAPTURE(worst);
			// THE PEAKS, BECAUSE "IT DIFFERS" AND "IT IS LOUDER" ARE DIFFERENT FINDINGS. A device whose
			// partition dependence is a phase or a ramp shape has two runs at the same level; one whose
			// output grows when the blocks get short is doing something a user would hear as a
			// transient, and the number below is what tells them apart.
			MESSAGE("partition-dependent: " << who << " worst |diff| " << worst << " at partition "
			                                << worstPartition << "; peak one-call " << peakOf(reference)
			                                << " vs peak that partition " << worstPeak
			                                << "; input peak " << peakOf(probe));
		}
	}

	// THE EXEMPTION IS AN ASSERTION, NOT A COMMENT. `OWLTT` is partition-dependent, and since the
	// 2026-08-28 decision the REASON has changed: both of its time constants are now expressed in
	// seconds (see `Owltt::kGainSmoothingSeconds` and the `levelAlpha` note in `owltt.cpp`), and what
	// is left is `updateLevel` taking the PEAK over the whole block — a 256-sample call sees a higher
	// peak than eight 32-sample calls of the same audio. Measured through the plugin, replacing the
	// block max with per-sample detection collapses the difference from 0.0314169 to 0.00370073, so
	// the residue is the detection WINDOW rather than a rate. Redesigning a detector this device
	// inherited from the firmware as block-based is a sound decision and is not taken here.
	//
	// SPELLED AS THE WHOLE SET so it can go red in BOTH directions: a second device joining the list
	// is a regression, and OWLTT leaving it means this comment has stopped being true. A sentence
	// saying "OWLTT is exempt" would still read correctly on the day either happened.
	const std::vector<std::string> expected { "OWLTT" };
	CHECK(dependent == expected);
	CHECK(unseenPartition == std::vector<std::string> {});
}

TEST_CASE("EVERY audio device: a hot signal through extreme settings stays a finite signal") {
	// **A DEVICE THAT PRODUCES A NaN POISONS THE WHOLE BUS FOR THE REST OF THE SESSION.** Every
	// arithmetic op downstream of one is a NaN, the master fader cannot rescue it, and the only cure
	// is a state the user has no button for. The same is true one step down for an infinity. So this
	// is not a quality bar, it is a safety property — and unlike almost everything else in this repo
	// it can be asked of every device with no knowledge of what any of them does.
	//
	// EVERY STEPPED CONTROL AT EVERY VALUE, not the default one. A filter's Type selects which DSP
	// runs, so a suite that leaves it alone has tested one of nine — and the comb is the one with a
	// feedback loop and a saturator in front of it, which is where a divergence would
	// live. Continuous controls are pinned at both extremes around it.
	//
	// THE INPUT IS HOT AND HAS A DC STEP AND A SILENCE. 4.0 is +12 dBFS, which a chain of devices in
	// front of this one can absolutely produce; the DC is what finds an integrator with no leak, and
	// the silence at the end is where a diverging feedback loop shows itself with nothing to blame.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 512;
	std::vector<float> hot = probeSignal(kFrames, 4.0f, 0xfeed5eedu);
	for (int i = 128; i < 192; ++i) {
		hot[i] = 4.0f; // a DC step
	}
	for (int i = 384; i < kFrames; ++i) {
		hot[i] = 0.0f; // and then nothing to hide behind
	}

	// GENEROUS ON PURPOSE. The claim being made is "it did not diverge", not "it was quiet": a
	// resonant filter at maximum Q fed +12 dBFS is ALLOWED to ring loudly, and a tight bound here
	// would be asserting a taste rather than a safety property.
	constexpr float kDiverged = 1.0e6f;

	// ALLOCATED ONCE AND REFILLED. Copy-constructing the buffers inside the loop is the same test,
	// and g++-14 -O3 inlines the vector's allocation into this file's REPLACED `operator new` deeply
	// enough to raise a spurious -Werror=mismatched-new-delete against the aligned delete in `Raw`.
	std::vector<float> l(kFrames, 0.0f);
	std::vector<float> r(kFrames, 0.0f);

	int configurations = 0;
	for (const device::Descriptor* desc : devices) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		const size_t enable = device::enableControlIn(desc->traits, desc->params);
		REQUIRE(enable != device::kNoControl);
		int hereConfigurations = 0;
		float loudest = 0.0f;

		// THE THREE BASE ROWS: everything at its floor, everything at its ceiling, and the declared
		// defaults. `extreme` walks the CONTINUOUS controls only — the stepped ones are enumerated
		// below, so pinning them here would test one arrangement three times.
		const auto baseRow = [&](int which) {
			DeviceRow row = enabledRowFor(*desc);
			for (size_t i = 0; i < desc->params.size() && i < row.fieldsPresent; ++i) {
				if (desc->params[i].stepped || i == enable) {
					continue;
				}
				if (which == 0) {
					row.field[i] = static_cast<float>(desc->params[i].min);
				}
				else if (which == 1) {
					row.field[i] = static_cast<float>(desc->params[i].max);
				}
			}
			return row;
		};

		// ONE OVERRIDE PER RENDER: `{control, value}`, plus the empty one that renders the base row as
		// it stands.
		//
		// THE EMPTY ONE IS NOT TIDINESS. The first draft nested the render inside the stepped-control
		// loop, so **a device with no stepped control but its enable rendered ZERO times** — `Drawn`
		// is exactly that device, and the sweep reported 456 finite configurations without having run
		// it once. Caught by the per-device non-vacuity guard below, which is the entire argument for
		// having one.
		struct Override {
			size_t control;
			float value;
		};
		std::vector<Override> overrides { { device::kNoControl, 0.0f } };
		for (size_t p = 0; p < desc->params.size() && p < kMaxDeviceFields; ++p) {
			if (!desc->params[p].stepped || p == enable) {
				continue;
			}
			const auto lo = static_cast<int>(std::lround(desc->params[p].min));
			const auto hi = static_cast<int>(std::lround(desc->params[p].max));
			// A "stepped" control with max below min is a declaration somebody forgot to finish, and
			// it would make this loop silently contribute nothing.
			CHECK_MESSAGE(hi >= lo, "stepped control with max below min on ", who);
			// EVERY VALUE OF EVERY STEPPED CONTROL, one control at a time. The full cross product is
			// not the claim — a device with four choices of eight would be four thousand renders for a
			// property that is about each DSP path being sane, not about their combinations.
			for (int v = lo; v <= hi; ++v) {
				overrides.push_back({ p, static_cast<float>(v) });
			}
		}

		for (int which = 0; which < 3; ++which) {
			{
				for (const Override& over : overrides) {
					DeviceRow row = baseRow(which);
					const size_t p = over.control;
					if (p != device::kNoControl) {
						row.field[p] = over.value;
					}

					Raw dev(*desc, 48000.0, row);
					std::copy(hot.begin(), hot.end(), l.begin());
					std::copy(hot.begin(), hot.end(), r.begin());
					dev.process(l.data(), r.data(), kFrames);
					++configurations;
					++hereConfigurations;

					int nonFinite = 0;
					int diverged = 0;
					float peak = 0.0f;
					for (int i = 0; i < kFrames; ++i) {
						for (const float x : { l[i], r[i] }) {
							if (!std::isfinite(x)) {
								++nonFinite;
								continue;
							}
							peak = std::max(peak, std::fabs(x));
							if (std::fabs(x) > kDiverged) {
								++diverged;
							}
						}
					}
					if (nonFinite != 0 || diverged != 0) {
						CAPTURE(which);
						CAPTURE(p);
						CAPTURE(over.value);
						CAPTURE(peak);
						CHECK_MESSAGE(nonFinite == 0, "NaN or infinity out of ", who);
						CHECK_MESSAGE(diverged == 0, "diverged past 1e6 out of ", who);
					}
					loudest = std::max(loudest, peak);
				}
			}
		}
		// NON-VACUITY, PER DEVICE AND BY NAME. "Nothing was infinite" is true of silence, and a sweep
		// that renders four hundred bypasses is the shape of green this playbook keeps finding. The
		// input peaks at 4.0, so ONE configuration somewhere in this device's grid reaching 1.0 is a
		// low bar deliberately — the claim is that the hot signal got through, not that it got through
		// everywhere.
		CAPTURE(hereConfigurations);
		CAPTURE(loudest);
		CHECK_MESSAGE(loudest >= 1.0f, "no configuration passed a hot signal on ", who,
		              " — this sweep is measuring a bypass");
		CHECK_MESSAGE(hereConfigurations >= 3, "fewer than three configurations on ", who);
		MESSAGE(who << ": " << hereConfigurations << " configurations, loudest peak " << loudest);
	}
	// **THE KNOWN-BAD FOR THIS CASE IS AN INJECTION, AND THAT IS WORTH SAYING.** It found a real one
	// once — `drawn` reaching 2.70204e+19 at every control's ceiling, which is what `kMinDenominator`
	// exists for — but that configuration no longer diverges even with BOTH of `peakMagnitude`'s
	// floors removed, because drawn's normalisation has been reworked since. So the only way to show
	// this case can still see a non-finite sample is to make one: dividing a single sample by zero in
	// `Harm::processStereo`, gated on a setting the sweep reaches, reddens it with `nonFinite 1 == 0`
	// on the device's own name. **A sweep with no live mutation available is a sweep whose detector
	// has to be demonstrated some other way, not one that may be assumed to work.**
	//
	// A FLOOR UNDER THE SWEEP ITSELF. A registry that answered nothing, or a device whose stepped
	// controls all happened to be skipped, would otherwise pass by doing nothing at all.
	CHECK(configurations >= 40);
	MESSAGE("extreme-settings configurations rendered: " << configurations);
}


TEST_CASE("EVERY audio device: the sample rate reaches the DSP, and prepare is what carries it") {
	// `AudioOps::prepare` HANDS OVER THE RATE AND NOTHING ELSE DOES. Every time constant and every
	// corner frequency in every device is a number of SECONDS or HERTZ that has to be converted into
	// samples somewhere, and the only place that number arrives is here.
	//
	// A DEVICE THAT DROPPED IT WOULD STILL WORK, which is what makes this worth asserting: it would
	// filter, it would compress, it would ring — at 48 kHz's coefficients, running at 96. Every case
	// in this repository prepares at one rate, so nothing else asks the question.
	//
	// THE CLAIM IS A DIFFERENCE AND THAT IS THE RIGHT FORM HERE, unusually for this repo. The claim is
	// not "the corner is at F" — it is "the rate is READ", and a device that ignores its argument
	// produces byte-identical output from byte-identical input. There is nothing weaker about the
	// assertion than the property.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 512;
	const std::vector<float> probe = probeSignal(kFrames, 0.3f, 0x5a1e5a1eu);

	std::vector<std::string> rateFree;
	for (const device::Descriptor* desc : devices) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		// `variedRowFor` AND NOT `enabledRowFor`, and the difference is a real property of one device
		// rather than a convenience. **Drawn's shipped default is EXACTLY a wire** — the default is
		// flat by design, and `test_fx_drawn.cpp` asserts it at exact
		// equality at every puck position — so a device switched on at its declared defaults returns
		// its input byte for byte, and no question about its coefficients can be asked of it. The
		// first draft of this case used the default row and reported Drawn as ignoring its sample
		// rate; the `touched > 0` guard below is what caught that, and it is why the guard is here.
		const DeviceRow row = variedRowFor(*desc);
		// PER DEVICE: one of them is a wire until it has captured a whole slice. See `framesFor`.
		const int frames = framesFor(*desc, kFrames);
		const std::vector<float> probe = probeSignal(frames, 0.35f, 0x5a5a1234u);

		const auto at = [&](double rate) {
			Raw dev(*desc, rate, row);
			std::vector<float> l(static_cast<size_t>(frames), 0.0f);
			std::vector<float> r(static_cast<size_t>(frames), 0.0f);
			std::copy(probe.begin(), probe.end(), l.begin());
			std::copy(probe.begin(), probe.end(), r.begin());
			dev.process(l.data(), r.data(), frames);
			return l;
		};

		// THE SAME SAMPLES IN, AT TWO RATES AN OCTAVE APART. Not 44.1 and 48: a ratio of two moves
		// every corner by a whole octave, so a device that reads the rate cannot come back with
		// something that merely rounds to the same floats.
		const std::vector<float> low = at(48000.0);
		const std::vector<float> high = at(96000.0);

		int differed = 0;
		float worst = 0.0f;
		for (int i = 0; i < frames; ++i) {
			if (low[i] != high[i]) {
				++differed;
				worst = std::max(worst, std::fabs(low[i] - high[i]));
			}
		}
		CAPTURE(worst);
		// THE DEVICE HAS TO BE DOING SOMETHING AT ALL, or neither answer below means anything.
		int touched = 0;
		for (int i = 0; i < frames; ++i) {
			if (low[i] != probe[i]) {
				++touched;
			}
		}
		REQUIRE_MESSAGE(touched > 0, "the device is a wire at these settings on ", who);

		if (differed == 0) {
			rateFree.push_back(who);
		}

		// AND THE SMOOTHER, WHICH IS A SEPARATE ROAD FOR THE RATE and the one every device has. A
		// device may legitimately hold its corners in NORMALISED frequency and so be free of the rate
		// in its coefficients; nothing is free of it while it is MOVING, because a time constant in
		// seconds is a different number of samples at each rate. Asked by starting the device
		// somewhere else and reading the same 512 samples of its journey.
		const auto travelling = [&](double rate) {
			Raw dev(*desc, rate, variedRowFor(*desc));
			std::vector<float> w(64, 0.0f);
			std::vector<float> w2(64, 0.0f);
			dev.process(w.data(), w2.data(), 64);
			dev.fields(enabledRowFor(*desc));
			std::vector<float> l(static_cast<size_t>(frames), 0.0f);
			std::vector<float> r(static_cast<size_t>(frames), 0.0f);
			std::copy(probe.begin(), probe.end(), l.begin());
			std::copy(probe.begin(), probe.end(), r.begin());
			dev.process(l.data(), r.data(), frames);
			return l;
		};
		const std::vector<float> movingLow = travelling(48000.0);
		const std::vector<float> movingHigh = travelling(96000.0);
		int movingDiffered = 0;
		for (int i = 0; i < frames; ++i) {
			if (movingLow[i] != movingHigh[i]) {
				++movingDiffered;
			}
		}
		CHECK_MESSAGE(movingDiffered > 0, "a MOVING device is identical at 48 and 96 kHz on ", who,
		              " — prepare's rate reaches nothing at all");
	}

	// THE EXEMPTION IS AN ASSERTION, NOT A COMMENT — the same shape as OWLTT's in the partition sweep.
	//
	// **DRAWN HOLDS ITS CORNERS IN NORMALISED FREQUENCY.** `Point::angleLog2` is log2 of normalised
	// frequency with 0 at Nyquist, so a drawn curve is a shape on the spectrum rather than a set of
	// hertz, and its coefficients come out bit-identical at any rate. That is a DESIGN CHOICE and not
	// a defect — but it makes drawn the only device here whose corners move when the host's rate does
	// (`MultimodeFilter` stores `cutoffLog2Hz`, which is absolute), so the same patch filters
	// different frequencies at 44.1 kHz and 96 kHz. Named here so the difference is recorded
	// somewhere that goes red if it changes in either direction.
	// **AND BUS UTILITY IS RATE-FREE FOR A DIFFERENT REASON, WHICH IS WHY IT IS NAMED SEPARATELY
	// RATHER THAN APPENDED TO DRAWN'S SENTENCE.** Drawn's freedom is a CONSEQUENCE of holding its
	// corners in normalised frequency, and it carries a caveat: the same patch filters different
	// hertz at 44.1 and 96 kHz. Bus Utility has no frequency-domain behaviour AT ALL — every
	// coefficient it applies is a gain, a cosine or a sign, and none of them is a number of seconds
	// or hertz — so there is no corner to move and no caveat to record. Its steady output being
	// identical at both rates is the device being what it says it is.
	//
	// THE RATE DOES REACH IT, AND THE `travelling` HALF ABOVE IS WHAT SAYS SO. Its smoother is a
	// fixed TIME (`busutil::kRampSeconds`), so it slews over twice as many samples at 96 kHz — which
	// is the whole reason that second probe exists, and the reason a device can be on this list
	// without being a device that ignores `prepare`. The first version of this device ramped across a
	// BLOCK instead, which made it rate-free in both halves AND partition-dependent; it failed the
	// partition case two cases up rather than this one.
	//
	// **AND AN EMPTY CHAIN IS RATE-FREE FOR BUS UTILITY'S REASON, EXACTLY.** Every coefficient a chain
	// applies to the signal ITSELF is a gain — the wet path at `mix`, the dry path at `1 - mix`, both
	// through the output fader — and none of them is a number of seconds or hertz. What it HOLDS may
	// have corners, and each of those devices is swept on its own above; this row is about the chain's
	// own arithmetic, which is what an empty chain exposes.
	//
	// THE RATE DOES REACH IT, AND THE `travelling` HALF IS WHAT SAYS SO — the same sentence as Bus
	// Utility's. Its output ramp is a fixed TIME, 5 ms, so it slews over twice as many samples at
	// 96 kHz. **That ramp did not exist when the chain was first registered**, and this very case is
	// what reported it: *"a MOVING device is identical at 48 and 96 kHz on Chain — prepare's rate
	// reaches nothing at all"*. Mix and gain were step changes, i.e. a zipper.
	//
	// **AND CLIP IS RATE-FREE FOR BUS UTILITY'S REASON, AT THE SWEEP'S SETTINGS.** `variedRowFor` leaves
	// stepped controls at their defaults, so Mode stays Soft: a MEMORYLESS curve, `dyn::softClip`, with
	// nothing in it that is a number of seconds or hertz. Its 4x halfbands are rate-relative by
	// construction — their coefficients are fractions of whatever the rate is — so the steady output at
	// 48 and 96 kHz is the same bits. **Texture is the one mode with an absolute corner** (its high-pass
	// is designed in hertz at the processing rate; `test_fx_clip.cpp`'s corner case pins it at both
	// oversampling settings), and this sweep never selects it. The Drive gesture (`gestureControlIn`)
	// does not change this: it drives the curve harder, it does not give it a memory.
	//
	// THE RATE DOES REACH IT, AND `travelling` SAYS SO: its drive, ceiling, output and enable ramps are
	// fixed TIMES (`dyn::kRampSeconds`), as is its rest (`kRestSilentSeconds`), so a MOVING Clipper differs
	// between the two rates, which is what that half asserts.
	// THE RACK (backlog NO) joins the Chain for the Chain's reason: it holds devices and, empty, has none to carry a rate to.
	// **SIDECHAIN INJECT (backlog NW) IS RATE-FREE FOR BUS UTILITY'S REASON, EXACTLY**: every coefficient it applies is a gain (the mix, the
	// sidechain gain, the enable), none a number of seconds or hertz; its ramps are a fixed TIME, which `travelling` covers.
	const std::vector<std::string> expected { "Drawn", "Bus Utility", "Chain", "Clipper", "Rack", "Sidechain Inject" };
	CHECK(rateFree == expected);
}

TEST_CASE("OWLTT renders a block longer than its own stack buffer, instead of writing past it") {
	// **A PUBLIC ENTRY POINT THAT SMASHES THE STACK ON A CALL IT ACCEPTS.** `Owltt::processStereo`'s
	// band splitting holds eight `std::array<float, kSliceFrames>` on the stack and `memcpy`s
	// `frames` samples into two of them. Nothing between it and the host's call bounds the count —
	// the lane (then `FxRunner::processStereo`, now the rack engine's bus pass) checks `frames <= 0`
	// and nothing else — so a caller handing over more than 512 frames wrote past the end of them.
	//
	// UNREACHABLE THROUGH THE PLUGIN AND REACHABLE THROUGH THE API. `Synth::render` cuts every chunk
	// at `min(n - done, kRenderChunk = 32, ...)`, so the shipping path has never passed more than 32
	// and this was never a shipped crash. It is fixed rather than documented because the failure mode
	// of the version that was there is not a wrong number, and because a bound that lives only in a
	// caller two files away is a bound the next caller does not have.
	//
	// **THE KNOWN-BAD IS NOT IN THE SUITE, DELIBERATELY.** Before the slicing loop this case did not
	// fail, it CRASHED — and a crash-red masks every case after it in the same binary, so what would
	// be committed is a case that can hide its neighbours. The measurement was taken once, by hand,
	// on the unsliced source and is reported as it printed rather than as it was expected to:
	//
	//     FATAL ERROR: test case CRASHED: SIGABRT - Abort (abnormal termination) signal
	//     test cases: 1 | 0 passed | 1 failed | 15 SKIPPED
	//
	// **and the fifteen skipped are the whole point of keeping it out.** Fifteen cases in this binary
	// did not run, and the verdict line reads like a finished suite. What is committed is the green
	// half.
	const device::Descriptor* desc = deviceForTag(fx::owltt::kTraits.tag);
	REQUIRE(desc != nullptr);
	REQUIRE(desc->audio != nullptr);

	// FOUR TIMES THE SLICE, so the loop has to run four passes and a caller cannot pass by handling
	// one overflow and not a second.
	constexpr int kFrames = 4 * fx::owltt::Owltt::kSliceFrames;
	const DeviceRow row = enabledRowFor(*desc);
	const std::vector<float> probe = probeSignal(kFrames, 0.4f, 0xd077d077u);

	Raw dev(*desc, 48000.0, row);
	std::vector<float> l(kFrames, 0.0f);
	std::vector<float> r(kFrames, 0.0f);
	std::copy(probe.begin(), probe.end(), l.begin());
	std::copy(probe.begin(), probe.end(), r.begin());
	dev.process(l.data(), r.data(), kFrames);

	// EVERY SAMPLE, INCLUDING THE ONES PAST THE FIRST SLICE — which is the half a clamp would have
	// left as silence and which a bound-check-and-return would have left as the input.
	int nonFinite = 0;
	int touchedLate = 0;
	float peakLate = 0.0f;
	for (int i = 0; i < kFrames; ++i) {
		if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
			++nonFinite;
			continue;
		}
		if (i >= fx::owltt::Owltt::kSliceFrames) {
			peakLate = std::max(peakLate, std::fabs(l[i]));
			if (l[i] != probe[i]) {
				++touchedLate;
			}
		}
	}
	CHECK(nonFinite == 0);
	CAPTURE(peakLate);
	// THE TAIL WAS RENDERED, not passed through and not silenced. Both of the wrong fixes are visible
	// here and neither is visible from "it did not crash".
	CHECK_MESSAGE(touchedLate > 0, "the frames past the first slice were passed through untouched");
	CHECK_MESSAGE(peakLate > 0.0f, "the frames past the first slice were silenced");
}

TEST_CASE("EVERY device with a NOTE road: fields and notes may arrive in either order") {
	// **THE CONTRACT IS WRITTEN DOWN AND ASKED OF NOBODY.** `registry.h` on `setNoteContext`:
	// *"ORDER-INDEPENDENT BY CONTRACT: a device that uses both must latch each and recompute, because
	// a host may deliver them in either order and will deliver fields far less often than notes."*
	// Many devices have a note road now — the filter, HARM, waypoint, the shaper, the eroder, automod,
	// sklitter, the Smooth Shaper, and the rack device that fans it out — and each latches in its own
	// way: the filter latches the whole `State`, HARM copies three live fields back out of its own
	// target, waypoint keeps a `Settings` and a `NoteContext` side by side, the Smooth Shaper folds the
	// note's stack trim into its drive target. This case compares the two orders for every one of them.
	//
	// IT IS THE FAILURE THAT ALREADY HAPPENED HERE, one layer along. `multimode_filter.cpp` records
	// it: *"Handing setTarget the decoded Settings directly is what zeroed keyOffsetSemitones on
	// every call"* — a device driven entirely through `setFields` filtering at a FIXED corner, which
	// `registry.h` itself calls out as the partial effect a render check cannot see.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 256;
	// **AS STACKED VOICES, 4 PARTS 1 CENT APART** — a note the per-voice road now delivers (`NoteContext::stackCount`).
	// A device that reads only the pitch sees the same two notes it always did; the Smooth Shaper's note reaches its
	// sound ONLY through its stack trim, which at one voice is exactly 1 — so an unstacked pair would leave this case
	// blind to an ordering bug on it. Four octaves apart at 1 cent, the two trims are −5.0 dB and −0.1 dB.
	const device::NoteContext low { 36.0f, true, 4, 1.0f };
	const device::NoteContext high { 84.0f, true, 4, 1.0f };

	int swept = 0;
	// **DEVICES WHOSE NOTE REACHES THE SOUND ONLY THROUGH WHAT THEY HOLD** — a rack, whose note road is
	// real (it hands its host's note to every device inside it) and which, swept EMPTY as a rack just
	// added is, has nothing inside for the note to reach. An ASSERTED LIST, so a second such device is
	// added here on purpose rather than slipping past the non-vacuity check below.
	//
	// MEASURED, NOT REASONED: `test_rack_nesting.cpp`'s "a rack device hands its host's note to what it
	// holds" puts a keytracking filter inside a rack and shows the note moving the rack's output.
	//
	// **SKLITTER JOINED IT THE DAY IT GAINED A POST CHAIN (backlog EW), and this assert is what said
	// so rather than a person remembering to.** That device has never read a note and still does not:
	// its `setNoteContext` exists so a keytracking filter inside its chain can have the pitch, which
	// is the same sentence as the chain's. Swept EMPTY, as every device here is, the note reaches
	// nothing — so it belongs on this list for the reason the list exists, and NOT because the note
	// road is decorative. The alternative, declaring no `setNoteContext` at all, would have made the
	// gate on the chain's inner filters silently pitchless; see `sklitter.cpp`'s `kAudioOps`.
	//
	// **SORTED BEFORE COMPARING, because this is a claim about a SET and not about `kDevices`' order.**
	// With two entries the raw comparison started failing on the order the sweep happens to visit them
	// in, which is a property of where each sits in the registry table — a fact this case has no
	// opinion about and should not go red over.
	// **RESONATE JOINED IT WITH ITS TWO FX LANES (2026-10-05)**, for SKLITTER's reason: it reads notes through its NOTE
	// road (`kNoteOps`), and `setNoteContext` exists only so a keytracking filter inside a lane has the pitch.
	// THE RACK (backlog NO) joins for the Chain's reason: its note reaches only what its chains hold.
	// THE DELAY (delay-love S1) joins for the same reason: its note is forwarded only to its FEEDBACK FX lane.
	std::vector<std::string> reachesOnlyThroughMembers { "Chain", "Compressor", "Delay", "Rack", "Resonate", "SKLITTER" };
	std::sort(reachesOnlyThroughMembers.begin(), reachesOnlyThroughMembers.end());
	std::vector<std::string> throughMembers;
	for (const device::Descriptor* desc : devices) {
		if (desc->audio->setNoteContext == nullptr) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		if (desc->container != nullptr) {
			throughMembers.push_back(who);
			continue;
		}
		++swept;

		// EVERY STEPPED CONTROL AT ITS CEILING, on top of the varied row. Keytrack and Poly are
		// switches, so `variedRowFor` — which moves only the continuous controls — leaves them where
		// the defaults put them, and a filter with keytrack off ignores the note entirely. Derived
		// from `stepped` rather than from the two controls' names, for the reason `device.h` gives
		// about the enable: a display string is not a contract.
		DeviceRow row = variedRowFor(*desc);
		for (size_t i = 0; i < desc->params.size() && i < row.fieldsPresent; ++i) {
			if (desc->params[i].stepped) {
				row.field[i] = static_cast<float>(desc->params[i].max);
			}
		}

		// **AND THE PROBE MUST REACH THE ROAD.** The shaper's note reaches one thing, the Lowpass
		// extra, which is a PRE-transform of the part of the wave the mix threshold hands to the
		// curve; a sample below the threshold is returned as it came and hears no extra and no note.
		// `variedRowFor` moves Mix from 1.0 to 0.4, which puts the threshold at 0.594 in the input
		// units the device compares in, and a 0.3 probe never crosses it — so this case would read
		// "the note reaches nothing on Shaper" of a road that is intact. (It crossed before the
		// ceiling work only because Drive, varied to 0.8, put a 10.5x gain in front of the compare;
		// Drive is a table sweep now and the compare is against the input itself.)
		//
		// THE LEVEL IS READ OFF THE DEVICE, not restated: a Shaper built on this row reports the
		// threshold it will compare against, and the probe sits halfway between it and full scale.
		// Identity by the ops table rather than by name — a display string is not a contract.
		//
		// AND THE TABLE MUST HAVE LANDED — the second thing this case was measuring the absence of.
		// A shaper's first 513 samples read an IDENTITY placeholder while its table builds, and
		// 512 more fade it in; `shaper.h` calls that window "the effect has not arrived yet". On the
		// identity the read's bound is 1, so with the Integrator on (it is, every switch is at its
		// ceiling) the wet is pulled far enough under the threshold that the read pins at the
		// table's end for BOTH notes and the corner cannot show — MEASURED: 45 samples crossing,
		// 0 moved, lowpass corners 158 Hz and 2527 Hz. A built table's bound is 5..40 and does not
		// pin there. So each run is eight blocks and the LAST is compared: 2048 samples, past the
		// build and the fade. The ordering comparison is bit-equality and every device here is
		// deterministic, so a later block asks the same question of a device that has arrived.
		float amplitude = 0.3f;
		if (desc->audio == &fx::shaper::kAudioOps) {
			// **AND MIX BACK AT ITS CEILING FOR THIS DEVICE, because below it the note road is
			// PINNED OUT and this case cannot see what it exists to see.**
			//
			// The shaper's note reaches one thing, the Lowpass extra's corner. The threshold compare
			// is against the DRY sample (deliberately — `shaper.cpp` calls that load-bearing), while
			// the excess handed to the table is the LOWPASSED one, which the filter has pulled well
			// below the threshold. So the excess goes strongly negative, and `lookup` clamps its
			// argument to +-1.
			//
			// MEASURED at the varied row's mix 0.4 and drive 0.8: the read's rate is
			// `sweep/headroom` = 7.46, so an excess of -0.40 reads the table at -2.99 and -0.55 at
			// -4.11 — both pinned to `table[0]`, giving BIT-IDENTICAL output for cutoffs of 174.6 Hz
			// and 2793.1 Hz, a 16x spread. Under the read this replaced the rate was
			// `sweep/(headroom*secant)` = 1.52, which put those same excesses at -0.61 and -0.84,
			// inside the table — which is why this case passed before and not after.
			//
			// At mix 1.0 the threshold is -0.015 and the rate is ~0.99, so the excess stays in the
			// table and the corner is audible again. **The behaviour itself is accepted for now and
			// logged for a future pass** (2026-09-04); see the backlog. This
			// line is the fixture reaching a state where the road exists, not a workaround for it.
			const size_t mixAt = static_cast<size_t>(fx::shaper::Param::Threshold);
			REQUIRE(mixAt < row.fieldsPresent);
			row.field[mixAt] = static_cast<float>(desc->params[mixAt].max);
			// **AND A COLOUR WHERE THE LOWPASS EXISTS AT ALL.** The Lowpass extra runs only while the
			// field's slew intensity is above zero (`lowpassActive_`), and that triangle's duty was
			// set by `Harmonics` — which this fixture's varied row pushed to 0.8, a 100 % duty that is
			// live everywhere. `Harmonics` became `Emphasis` on 2026-09-22 and the duty is fixed at
			// `kSlewHarmonics` (70 %), which leaves the varied row's colour in the triangle's gap:
			// measured, the note then reached nothing. Found rather than typed, at the row's own knee
			// and gamma, so the road this case exists to watch is one the device actually has.
			const size_t colourAt = static_cast<size_t>(fx::shaper::Param::Colour);
			const size_t kneeAt = static_cast<size_t>(fx::shaper::Param::Knee);
			const size_t gammaAt = static_cast<size_t>(fx::shaper::Param::Gamma);
			REQUIRE(colourAt < row.fieldsPresent);
			bool found = false;
			for (int i = 0; i < 1000 && !found; ++i) {
				const float colour = static_cast<float>(i) / 1000.0f;
				const fx::shaper::CurveParams p =
				    fx::shaper::deriveCurve(row.field[kneeAt], colour, row.field[gammaAt], fx::shaper::kSlewHarmonics);
				if (p.slewIntensity > 0.5f) {
					row.field[colourAt] = colour;
					found = true;
				}
			}
			REQUIRE(found);

			Raw sh(*desc, 48000.0, row);
			std::vector<float> zl(kFrames, 0.0f);
			std::vector<float> zr(kFrames, 0.0f);
			sh.process(zl.data(), zr.data(), kFrames); // one block: the threshold is derived per block
			const float threshold = static_cast<const fx::shaper::Shaper*>(sh.mem)->thresholdForTest();
			REQUIRE(threshold < 1.0f);
			amplitude = std::max(amplitude, threshold + 0.5f * (1.0f - threshold));
		}
		if (desc->audio == &fx::eq::kAudioOps) {
			// **AND THE EQ BACK IN MINIMUM PHASE, AT NO OFFSET.** Every switch at its ceiling puts Key Track on (the road this case
			// watches) and Phase on Linear, whose ~0.19 s latency outlasts the eight blocks compared here: the output is
			// the dry delayed past the window, so the note would reach nothing visible for a reason that is not the road.
			// AND OFFSET BACK AT 0: varied to +6 octaves it puts every band at the design limit for BOTH notes, where the
			// shift is clamped away (MEASURED: noteMoved 0 with Linear already off).
			const size_t offsetAt = fx::eq::paramIndex(fx::eq::Param::Offset);
			REQUIRE(offsetAt < row.fieldsPresent);
			row.field[offsetAt] = 0.0f;
			const size_t phaseAt = fx::eq::paramIndex(fx::eq::Param::Phase);
			REQUIRE(phaseAt < row.fieldsPresent);
			row.field[phaseAt] = 0.0f;
		}
		if (device::sameTag(desc->traits.tag, fx::noise::kTraits.tag)) {
			// **AND NOISE BACK ON ITS NOISE.** Every switch at its ceiling puts it on Impulses synced to a four-bar triplet —
			// a click every five seconds, none inside these eight blocks — so the note (Key Track, on the filter) would
			// shape nothing. The noise sounds every sample, so the corner the note moves is heard.
			row.field[static_cast<size_t>(fx::noise::Param::Source)] = static_cast<float>(fx::noise::Source::Noise);
		}
		CAPTURE(amplitude);
		const std::vector<float> probe = probeSignal(kFrames, amplitude, 0x0d0e0d0eu);

		const auto run = [&](const device::NoteContext& note, bool noteFirst) {
			Raw dev(*desc, 48000.0, row); // Raw's constructor already delivered the fields once.
			if (noteFirst) {
				desc->audio->setNoteContext(dev.mem, note);
				dev.fields(row);
			}
			else {
				dev.fields(row);
				desc->audio->setNoteContext(dev.mem, note);
			}
			std::vector<float> l(kFrames, 0.0f);
			std::vector<float> r(kFrames, 0.0f);
			constexpr int kBlocksToLand = 8;
			for (int b = 0; b < kBlocksToLand; ++b) {
				std::copy(probe.begin(), probe.end(), l.begin());
				std::copy(probe.begin(), probe.end(), r.begin());
				dev.process(l.data(), r.data(), kFrames);
			}
			return l;
		};

		const std::vector<float> fieldsFirst = run(high, false);
		const std::vector<float> noteFirst = run(high, true);
		const std::vector<float> otherNote = run(low, false);

		// **NON-VACUITY FIRST, AND IT IS THE WHOLE CASE.** If the note does not reach this device's
		// sound, then "the two orders agree" is true of a device that ignores one of them — which is
		// exactly the defect the filter already shipped once. Four octaves apart, because a
		// keytracking corner has to move audibly between them.
		int noteMoved = 0;
		for (int i = 0; i < kFrames; ++i) {
			if (otherNote[i] != fieldsFirst[i]) {
				++noteMoved;
			}
		}
		//
		// **AND IT IS NOT ONLY A GUARD — IT IS THE DETECTOR FOR THE FILTER'S OWN OLD BUG.** Putting
		// `dsp.setTarget(stateOf(row, take).settings)` back in place of the latch fails HERE, on
		// `noteMoved == 0` and the message "the note reaches nothing on Filter", rather than on the
		// ordering check below. That is the correct place for it: the defect was not that the two
		// orders disagreed, it was that one road stopped arriving at all.
		REQUIRE_MESSAGE(noteMoved > 0, "the note reaches nothing on ", who,
		                " — this case cannot see an ordering bug on it");

		int differed = 0;
		float worst = 0.0f;
		for (int i = 0; i < kFrames; ++i) {
			if (noteFirst[i] != fieldsFirst[i]) {
				++differed;
				worst = std::max(worst, std::fabs(noteFirst[i] - fieldsFirst[i]));
			}
		}
		CAPTURE(worst);
		CHECK_MESSAGE(differed == 0, "the two delivery orders give different sound on ", who);
	}
	// A FLOOR: three devices declare a note road today.
	CHECK(swept >= 3);
	std::sort(throughMembers.begin(), throughMembers.end());
	CHECK(throughMembers == reachesOnlyThroughMembers);
}

TEST_CASE("EVERY device with a MONO road: it is the stereo road's left channel, sample for sample") {
	// **TWO LOOPS FOR ONE SOUND IS TWO PLACES FOR IT TO DRIFT.** `registry.h` makes `processMono` a
	// declared capability rather than a convention — *"a host holding a mono buffer for a device with
	// no mono road knows to give it two channels instead of hoping"* — and `Harm::processMono` says
	// out loud why it delegates rather than duplicating: *"two loops is two places for the envelope's
	// shape to drift apart."*
	//
	// `MultimodeFilter` DOES duplicate: `processMono` is a second copy of the whole type switch, one
	// channel wide, eight branches deep. Nothing compares the two, and the per-voice road is where
	// the mono one runs — so a drift between them is a difference between what a note sounds like
	// with the character bank's stereo window open and shut, which is not a knob anybody turns.
	//
	// SAMPLE FOR SAMPLE, NOT NEARLY. Given identical input on both channels the two roads run the
	// same arithmetic on the same numbers, so anything but bit equality is a real divergence rather
	// than a tolerance question.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 512;
	const std::vector<float> probe = probeSignal(kFrames, 0.35f, 0x33aa55ffu);

	// ALLOCATED ONCE AND REFILLED. Constructing these inside the loops is the same test, and g++-14
	// -O3 inlines the vector's allocation into this file's REPLACED `operator new` deeply enough to
	// raise a spurious -Werror=mismatched-new-delete. Same reason as the extremes sweep above.
	std::vector<float> sl(kFrames, 0.0f);
	std::vector<float> sr(kFrames, 0.0f);
	std::vector<float> m(kFrames, 0.0f);

	int swept = 0;
	for (const device::Descriptor* desc : devices) {
		if (desc->audio->processMono == nullptr) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		++swept;

		// EVERY STEPPED CONTROL WALKED, because a filter's Type chooses which of eight loops runs and
		// the copy is per branch. A sweep that drove one type would be testing one eighth of the
		// duplication.
		for (size_t p = 0; p < desc->params.size() && p < kMaxDeviceFields; ++p) {
			const auto lo = static_cast<int>(std::lround(desc->params[p].min));
			const auto hi = static_cast<int>(std::lround(desc->params[p].max));
			const int steps = desc->params[p].stepped ? hi - lo : -1;
			for (int v = lo; v <= (steps >= 0 ? hi : lo); ++v) {
				DeviceRow row = variedRowFor(*desc);
				if (steps >= 0) {
					row.field[p] = static_cast<float>(v);
				}

				std::copy(probe.begin(), probe.end(), sl.begin());
				std::copy(probe.begin(), probe.end(), sr.begin());
				{
					Raw dev(*desc, 48000.0, row);
					dev.process(sl.data(), sr.data(), kFrames);
				}

				std::copy(probe.begin(), probe.end(), m.begin());
				{
					Raw dev(*desc, 48000.0, row);
					dev.tickClock(kFrames); // the same playhead the stereo road just saw
					desc->audio->processMono(dev.mem, m.data(), kFrames);
					// THE SLOT'S FURNITURE ON THE MONO ROAD TOO (backlog NR), as `Raw::process` applies it on the stereo one.
					if (dev.wet_ != 1.0f || dev.dry_ != 0.0f) {
						for (size_t i = 0; i < m.size(); ++i) {
							m[i] = m[i] * dev.wet_ + probe[i] * dev.dry_;
						}
					}
				}

				// NON-VACUITY: the device has to have DONE something, or "the two roads agree" is a
				// statement about two copies of the input.
				int touched = 0;
				int differed = 0;
				float worst = 0.0f;
				for (int i = 0; i < kFrames; ++i) {
					if (sl[i] != probe[i]) {
						++touched;
					}
					if (m[i] != sl[i]) {
						++differed;
						worst = std::max(worst, std::fabs(m[i] - sl[i]));
					}
				}
				if (touched == 0) {
					continue; // a bypassed arrangement says nothing either way
				}
				if (differed != 0) {
					CAPTURE(p);
					CAPTURE(v);
					CAPTURE(worst);
					CHECK_MESSAGE(differed == 0, "the mono road is not the stereo road's left on ",
					              who);
				}
			}
		}
	}
	CHECK(swept >= 4);
}


TEST_CASE("EVERY audio device: the tail reaches exact silence, or is named as one that does not") {
	// **A GEOMETRIC DECAY DOES NOT REACH ZERO**, and `puck_motion.h` already paid for that lesson in
	// this repository: *"Multiplying by 0.982 each tick walks the value down through the denormals and
	// then STICKS: at 1.38e-322 the product rounds back to itself, because the step is smaller than
	// half the gap between adjacent denormals, and the velocity sits there for the rest of the
	// session."* It fixed it with `kRestThreshold`. **The filters have the same arithmetic and no such
	// floor.**
	//
	// MEASURED BEFORE THE FIX, a 0.8 burst then 4.3 seconds of silence at 48 kHz, at `variedRowFor`'s
	// settings:
	//
	//     device     blocks with a non-zero sample (of 400)   peak at block 200 / 1000 / 2999
	//     Filter                    1                          0 / 0 / 0
	//     Harm                     65                          0 / 0 / 0
	//     Drawn                     3                          0 / 0 / 0
	//     OWLTT                    400                          9.5008e-43 three times over
	//     Waypoint                400                          1.4013e-45 three times over
	//
	// **The two right-hand columns were identical to every digit across 2,800 blocks — thirty seconds.
	// They were not decaying, they were parked**, exactly as `PuckMotion` was. Both are fixed:
	// `MultimodeFilter` and `Owltt` each grew `kRestLevel` and `kRestSilentSeconds`, so the set below
	// is now EMPTY and the assertion is emptiness rather than a named exemption.
	//
	// WHAT IT COSTS, measured rather than asserted, and it is smaller in one direction than the folk
	// wisdom and larger in the other. Two thousand silent blocks, with and without the host setting
	// flush-to-zero, twice over with the same answer:
	//
	//     Filter   81-86 ms -> 53-54 ms with FTZ    ~1.55x
	//     OWLTT     78-81 ms -> 54-55 ms             ~1.45x
	//     Drawn   118-120 ms -> 83-87 ms            ~1.40x
	//     Harm     96-98 ms -> 78-79 ms             ~1.24x
	//     Waypoint  25.5 ms -> 21.4 ms              ~1.15x
	//
	// So every device is 15-55% slower on a SILENT block when the host does not set FTZ, and Feathers
	// sets it nowhere. AND THE AUDIBLE HALF IS NOT THERE: the same run compared a second burst
	// rendered after the silence with and without FTZ and found **one sample in 512 differing, worst
	// 2.8e-35** — real, and 600 dB down. The host-dependence is a bit-exactness question, not a sound
	// one, and saying so is the difference between a finding and an alarm.
	//
	// **THE SET IS ASSERTED, NOT COMMENTED**, on the rule the partition sweep uses: a third device
	// joining is a regression and either of these two leaving means this comment has stopped being
	// true, and both go red here rather than reading correctly forever.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 512;
	constexpr int kSilentBlocks = 400; // ~4.3 s at 48 kHz
	std::vector<float> burst(kFrames, 0.0f);
	for (int i = 0; i < kFrames; ++i) {
		burst[i] = 0.8f
		           * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.02
		                                         * static_cast<double>(i)));
	}
	std::vector<float> l(kFrames, 0.0f);
	std::vector<float> r(kFrames, 0.0f);

	std::vector<std::string> neverSilent;
	for (const device::Descriptor* desc : devices) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		DeviceRow row = variedRowFor(*desc);
		// RESONATE'S CYCLE PLAYS ITS DRONE ON ITS OWN (2026-09-27): above Rate 0 it sustains by design, for ever — it is
		// checked for exactly that below — so the rows whose tail must END have it off.
		if (device::sameTag(desc->traits.tag, fx::resonate::kTraits.tag)) {
			row.field[static_cast<size_t>(fx::resonate::Param::DroneCycleSync)] = 0.0f;
		}
		// SKLITTER'S RECORD GATE (backlog MJ) EXISTS SO SILENCE IS NEVER WRITTEN OVER THE LOOP: armed, the silence below
		// lands nothing and the loop sustains by design, as under Freeze — `test_fx_sklitter_gate.cpp` holds that. The
		// road this case is about is the loop that IS overwritten, so the gate is off here.
		if (device::sameTag(desc->traits.tag, fx::sklitter::kTraits.tag)) {
			row.field[static_cast<size_t>(fx::sklitter::Param::RecordGate)] = 0.0f;
		}
		// THE DELAY'S FEEDBACK REACHES 200% SINCE delay-love S6b, so 60% of its span is 134% — a loop that sustains by
		// design, as under Freeze (`test_delay_lane` "S6b Feedback to 200% ..." holds that it stays awake, bounded). The
		// road this case is about is the tail that ENDS, so it gets the varied value it had before the range widened.
		if (device::sameTag(desc->traits.tag, fx::delay::kTraits.tag)) {
			row.field[static_cast<size_t>(fx::delay::Param::Feedback)] = 0.8f;
		}
		// THE RACK IN BANDS (stage 2, §4b: "add a Bands-mode rack to the silence sweep"): five bands, filtered Both, at
		// Mix 50% — every crossover filter it owns, the dry's allpass chain twice over included, must reach exact zero.
		if (device::sameTag(desc->traits.tag, rack::parallel::kTraits.tag)) {
			row.field[static_cast<size_t>(rack::parallel::Param::Mode)] = static_cast<float>(rack::parallel::Mode::Bands);
			row.field[static_cast<size_t>(rack::parallel::Param::BandCount)] = 2.0f;
			row.field[static_cast<size_t>(rack::parallel::Param::Filtering)] = 2.0f;
			row.field[static_cast<size_t>(rack::parallel::Param::Mix)] = 0.5f;
			row.field[static_cast<size_t>(rack::parallel::Param::Enabled)] = 1.0f;
		}

		const int silentBlocks = silentBlocksFor(*desc, kSilentBlocks);
		Raw dev(*desc, 48000.0, row);
		// **LONG ENOUGH FOR A LOOPER TO HAVE SOMETHING TO DECAY.** The burst was one block, which is
		// four seconds short of what SKLITTER captures before it plays a note — so the case asked
		// whether a device that had never made a sound was still making one. `framesFor` says how long
		// each device needs; the burst repeats to fill it.
		const int burstFrames = framesFor(*desc, kFrames);
		for (int done = 0; done < burstFrames; done += kFrames) {
			std::copy(burst.begin(), burst.end(), l.begin());
			std::copy(burst.begin(), burst.end(), r.begin());
			dev.process(l.data(), r.data(), kFrames);
		}

		// NON-VACUITY: the burst has to have reached the output, or "the tail is silent" is a claim
		// about a device that never made a sound.
		float burstPeak = 0.0f;
		for (int i = 0; i < kFrames; ++i) {
			burstPeak = std::max(burstPeak, std::fabs(l[i]));
		}
		CAPTURE(burstPeak);
		// 1e-6, NOT 1e-3, AND THE NUMBER IS MEASURED RATHER THAN PICKED. `Drawn` at these settings
		// peaks at 2.27e-05 — six resonant bands normalised by the cascade trim, which is quiet by
		// design and not a fault. What the guard has to establish is that the burst reached the output
		// at all and that the journey to zero is a real one, and 1e-6 is still twenty orders above the
		// denormal range the claim below is about. Measured peaks: Filter 0.309581, Harm 0.813477,
		// OWLTT 4.21937, Waypoint 0.334864, Drawn 2.27254e-05.
		REQUIRE_MESSAGE(burstPeak > 1.0e-6f, "the burst produced nothing on ", who);

		int lastNonZeroBlock = -1;
		for (int b = 0; b < silentBlocks; ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			dev.process(l.data(), r.data(), kFrames);
			for (int i = 0; i < kFrames; ++i) {
				if (l[i] != 0.0f || r[i] != 0.0f) {
					lastNonZeroBlock = b;
					break;
				}
			}
		}
		CAPTURE(lastNonZeroBlock);
		// **AND ONCE ITS TAIL HAS GONE, THE DEVICE SAYS IT HOLDS NOTHING** (backlog MC) — `AudioOps::holdsTail`, which a
		// host's SLEEP now waits on as well as on a silent block: a device that never cleared it would keep the plugin
		// awake for ever, the denormal-parking failure this sweep exists for, one level down.
		const bool holdsAtEnd = dev.ops->holdsTail != nullptr && dev.ops->holdsTail(dev.mem);
		CAPTURE(holdsAtEnd);
		if (device::sameTag(desc->traits.tag, fx::resonate::kTraits.tag)) {
			// **WITH NO NOTE HELD, RESONATE IS HEARD THROUGH ITS DRONE, AND THE INPUT PLAYS THE DRONE** (by
			// design: a sympathetic string). Bow still keeps the string moving — the device never parks while
			// it is up, no timeout, ever — but with no note reading the string, only the drone does, and it rings
			// down on its follower's decay once the input stops. So at `variedRowFor`'s row (Bow 0.7, Sustain 0.6)
			// the tail ENDS, at exact zero, as every other device's does. (Before the drone followed the input,
			// this row rang for ever; it was the old drone switch that sustained, not the Bow.)
			CHECK_MESSAGE(lastNonZeroBlock < silentBlocks / 2, "Resonate at its varied row still sounding halfway");
			CHECK_FALSE(holdsAtEnd);
			// THE SAME ROW WITH BOW AND SUSTAIN AT 0: a tail that ends too.
			DeviceRow calmRow = row;
			calmRow.field[static_cast<size_t>(fx::resonate::Param::Bow)] = 0.0f;
			// AND SUSTAIN AT 0: above 0, a sounding head plays the whole string steadily, which
			// is the other self-sustaining state; `variedRowFor` puts it at 0.6.
			calmRow.field[static_cast<size_t>(fx::resonate::Param::Sustain)] = 0.0f;
			Raw calm(*desc, 48000.0, calmRow);
			for (int done = 0; done < burstFrames; done += kFrames) {
				std::copy(burst.begin(), burst.end(), l.begin());
				std::copy(burst.begin(), burst.end(), r.begin());
				calm.process(l.data(), r.data(), kFrames);
			}
			int calmLast = -1;
			for (int b = 0; b < silentBlocks; ++b) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				calm.process(l.data(), r.data(), kFrames);
				for (int i = 0; i < kFrames; ++i) {
					if (l[i] != 0.0f || r[i] != 0.0f) {
						calmLast = b;
						break;
					}
				}
			}
			CAPTURE(calmLast);
			MESSAGE("Resonate at Bow 0: last non-zero block " << calmLast << " of " << silentBlocks);
			CHECK_MESSAGE(calmLast >= 0, "Resonate at Bow 0 made no tail at all — the claim below is vacuous");
			CHECK_MESSAGE(calmLast < silentBlocks / 2, "Resonate at Bow 0 still sounding halfway through the silence");
			CHECK_FALSE(calm.ops->holdsTail(calm.mem));
			// AND WITH THE CYCLE ON, THE SAME CALM ROW NEVER FALLS SILENT: it sustains by design (no timeout, ever).
			DeviceRow cycleRow = calmRow;
			cycleRow.field[static_cast<size_t>(fx::resonate::Param::DroneCycleSync)] = static_cast<float>(fx::resonate::kCycleOneBar);
			Raw cycling(*desc, 48000.0, cycleRow);
			int cycleLast = -1;
			for (int b = 0; b < silentBlocks; ++b) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				cycling.process(l.data(), r.data(), kFrames);
				for (int i = 0; i < kFrames; ++i) {
					if (l[i] != 0.0f || r[i] != 0.0f) {
						cycleLast = b;
						break;
					}
				}
			}
			CHECK_MESSAGE(cycleLast == silentBlocks - 1, "Resonate with its Cycle on fell silent");
			CHECK(cycling.ops->holdsTail(cycling.mem)); // the Cycle plucks on its own: it holds for as long as it runs
			continue;
		}
		if (device::sameTag(desc->traits.tag, fx::delay::kTraits.tag)) {
			// **AND THE DELAY AGAIN, IN BBD MODE** (2026-11-05): the chip's two filters, its held sample, its compander's
			// two followers and its noise must all reach exact zero too — the noise rides the signal, so it stops with it.
			// The mode is a Choice, which `variedRowFor` leaves at Clean, so the BBD road needs a row of its own.
			DeviceRow bbdRow = row;
			bbdRow.field[static_cast<size_t>(fx::delay::Param::Mode)] = static_cast<float>(static_cast<int>(fx::delay::Mode::Bbd));
			bbdRow.field[static_cast<size_t>(fx::delay::Param::BbdNoise)] = 1.0f;
			Raw bbd(*desc, 48000.0, bbdRow);
			for (int done = 0; done < burstFrames; done += kFrames) {
				std::copy(burst.begin(), burst.end(), l.begin());
				std::copy(burst.begin(), burst.end(), r.begin());
				bbd.process(l.data(), r.data(), kFrames);
			}
			int bbdLast = -1;
			for (int b = 0; b < silentBlocks; ++b) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				bbd.process(l.data(), r.data(), kFrames);
				for (int i = 0; i < kFrames; ++i) {
					if (l[i] != 0.0f || r[i] != 0.0f) {
						bbdLast = b;
						break;
					}
				}
			}
			MESSAGE("Delay in BBD: last non-zero block " << bbdLast << " of " << silentBlocks);
			CHECK_MESSAGE(bbdLast >= 0, "the Delay in BBD made no tail — the claim below is vacuous");
			CHECK_MESSAGE(bbdLast < silentBlocks / 2, "the Delay in BBD still sounding halfway through the silence");
			CHECK_FALSE(bbd.ops->holdsTail(bbd.mem));
		}
		if (device::sameTag(desc->traits.tag, fx::noise::kTraits.tag)) {
			// **NOISE GENERATES: WITH LEVEL UP IT SOUNDS FOR EVER, BY DESIGN, AND SAYS SO** (backlog MZ). The rule's third
			// clause: a device that sustains by design stays awake, and no timeout. So the varied row (Level up) sounds to
			// the last block and holds; at Level 0 nothing is added, the memory is cleared, and the silence is EXACT — the
			// road a parked filter would break.
			CHECK_MESSAGE(lastNonZeroBlock == silentBlocks - 1, "Noise at Level > 0 fell silent");
			CHECK(holdsAtEnd);
			DeviceRow quietRow = row;
			quietRow.field[static_cast<size_t>(fx::noise::Param::Level)] = 0.0f;
			dev.fields(quietRow);
			int quietLast = -1;
			for (int b = 0; b < 40; ++b) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				dev.process(l.data(), r.data(), kFrames);
				for (int i = 0; i < kFrames; ++i) {
					if (l[i] != 0.0f || r[i] != 0.0f) {
						quietLast = b;
						break;
					}
				}
			}
			CAPTURE(quietLast);
			CHECK(quietLast >= 0);  // the Level ramp is heard going down...
			CHECK(quietLast < 20);  // ...and then nothing, to the bit
			CHECK_FALSE(dev.ops->holdsTail(dev.mem));
			continue;
		}
		if (lastNonZeroBlock == silentBlocks - 1) {
			neverSilent.push_back(who);
		}
		else if (!device::sameTag(desc->traits.tag, fx::sklitter::kTraits.tag)) {
			// AND THE ONES THAT DO SETTLE, SETTLE PROMPTLY — a device still ringing at four seconds
			// but silent at 4.3 would pass the set test above while being the same defect.
			CHECK_MESSAGE(!holdsAtEnd, "still holding a tail after falling silent on ", who);
			CHECK_MESSAGE(lastNonZeroBlock < silentBlocks / 2, "still sounding halfway through the "
			                                                    "silence on ", who);
		}
		else {
			// **SKLITTER IS A LOOPER AND SUSTAINING IS WHAT IT IS FOR.** With the gesture held it
			// replays a captured bar for as long as you hold it -- that is the effect, not a tail
			// that failed to decay. "Settles promptly" is a claim about a processor with a memory,
			// and this device's memory is the point of it.
			//
			// IT IS NOT IN `neverSilent` EITHER, because with pWrite up the loop does fill with the
			// incoming silence and eventually goes quiet -- just not by halfway. Named here rather
			// than in that set so the distinction stays visible.
			//
			// **AND IT MAY BE SILENT FROM THE FIRST BLOCK, WHICH IS ALSO CORRECT.** This asserted
			// `lastNonZeroBlock >= 0` -- "it sounded at some point" -- and that stopped being true
			// when the default mode moved to Shuffle. MEASURED at the exact row `variedRowFor`
			// builds: the device IS playing (status 3), and it is silent anyway, because that
			// arrangement puts Density at 0.4 and pWrite at 0.6. Most slices are dry, a dry slice
			// passes the INPUT, and the input is silence -- while pWrite writes that same silence
			// into the loop. A looper fed nothing, mostly dry, outputs nothing. Asserting otherwise
			// was asserting that a correct device is broken.
			//
			// THE CLAIM THAT SURVIVES IS THE ONE ABOVE: it is not in `neverSilent`, so it does not
			// ring for ever. Whether it sounds AT ALL under an arrangement chosen by a generic sweep
			// is not something this case can know, and `test_fx_sklitter.cpp` measures the playing
			// device directly.
			//
			// **AND ITS CAPTURE MUST REACH EXACT ZERO TOO — the class this sweep missed (backlog MC).** Its output can be
			// exactly silent while its loop still holds sound, so the output alone cannot show a loop parked on a
			// decaying residue; the RING is asked directly. Engaged here, it still holds real sound (MEASURED: 32,090 of
			// 96,000 frames, peak 0.32) — a held loop sustains by design. Fed on with silence, pWrite overwrites it,
			// and every loop write is floored (`Sklitter::kLoopFloor`), so it must ARRIVE at zero: MEASURED, 1,544 more
			// blocks (16.5 s). Without the floor it decays geometrically for minutes. A minute is the bound.
			const auto* sklitter = static_cast<const fx::sklitter::Sklitter*>(dev.mem);
			CHECK(sklitter->ringForTest().holdsSound());
			CHECK(holdsAtEnd);
			int more = 0;
			for (; more < 60 * 48000 / kFrames && dev.ops->holdsTail(dev.mem); ++more) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				dev.process(l.data(), r.data(), kFrames);
			}
			CAPTURE(more);
			size_t notZero = 0;
			for (size_t frame = 0; frame < sklitter->ringForTest().length(); ++frame) {
				float a = 0.0f;
				float b = 0.0f;
				sklitter->ringForTest().readAt(frame, &a, &b);
				notZero += (a != 0.0f || b != 0.0f) ? 1 : 0;
			}
			CHECK(notZero == 0u); // every frame EXACTLY zero
			CHECK_FALSE(dev.ops->holdsTail(dev.mem));
			CHECK(sklitter->statusForTest() == fx::sklitter::Status::Playing); // still engaged: holding nothing, not released
		}
	}

	// NAMED, NOT COUNTED. `CHECK(neverSilent.empty())` prints `false` and nothing else, and the whole
	// value of asking this of five devices at once is that the answer says WHICH.
	for (const std::string& who : neverSilent) {
		MESSAGE("NEVER REACHES SILENCE: " << who);
	}
	CHECK(neverSilent.empty());
}


TEST_CASE("EVERY audio device: one NaN and one +Inf sample do not latch — the output after them is finite, and the tail still reaches exact zero") {
	// **A NON-FINITE SAMPLE IS A HOST FAULT, AND A DEVICE THAT KEEPS IT IS A DEVICE THAT NEEDS A RESET.** A recursive
	// section holds a NaN for ever, a delay line replays it, and a rest flush whose peak scan is `std::max` reads it as
	// silence and never clears it. The sweep is the registry's: every device at its DEFAULT row, switched on, fed a
	// burst with a NaN at one frame and an +Inf at another, then silence.
	//
	// WHAT IS ASSERTED, per device: (1) every output sample after the LAST bad input frame is finite — the bad frame's
	// own output is the host's to have, and a device that passes a dry sample through may pass it; (2) a device whose
	// CLEAN run reaches exact zero within `silentBlocksFor` reaches it in the bad run too, and by the same halfway mark.
	// A device that sustains by design (its clean run never goes quiet) is read by the clean run, so no device is
	// exempted by name here and none is exempted silently.
	const std::vector<const device::Descriptor*> devices = audioDevices();
	REQUIRE(devices.size() >= 5);

	constexpr int kFrames = 512;
	constexpr int kBadNan = 100;
	constexpr int kBadInf = 200;
	constexpr int kBurstBlocks = 4;
	std::vector<float> burst(kFrames, 0.0f);
	for (int i = 0; i < kFrames; ++i) {
		burst[i] = 0.8f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.02 * static_cast<double>(i)));
	}
	std::vector<float> l(kFrames, 0.0f);
	std::vector<float> r(kFrames, 0.0f);

	struct Outcome {
		bool finite = true;
		int firstBadBlock = -1;
		int lastNonZeroBlock = -1;
		bool holds = false;
	};
	const auto run = [&](const device::Descriptor& desc, bool inject) {
		Outcome out;
		const DeviceRow row = enabledRowFor(desc);
		Raw dev(desc, 48000.0, row);
		const int silentBlocks = silentBlocksFor(desc, 400);
		const int burstFrames = std::max(framesFor(desc, kFrames), kBurstBlocks * kFrames);
		int block = 0;
		for (int done = 0; done < burstFrames; done += kFrames, ++block) {
			std::copy(burst.begin(), burst.end(), l.begin());
			std::copy(burst.begin(), burst.end(), r.begin());
			const bool bad = inject && block == 1;
			if (bad) {
				l[kBadNan] = std::numeric_limits<float>::quiet_NaN();
				r[kBadInf] = std::numeric_limits<float>::infinity();
			}
			dev.process(l.data(), r.data(), kFrames);
			if (inject && block >= 1) {
				const int from = block == 1 ? kBadInf + 1 : 0;
				for (int i = from; i < kFrames; ++i) {
					if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
						out.finite = false;
						out.firstBadBlock = out.firstBadBlock < 0 ? block : out.firstBadBlock;
					}
				}
			}
		}
		for (int b = 0; b < silentBlocks; ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			dev.process(l.data(), r.data(), kFrames);
			for (int i = 0; i < kFrames; ++i) {
				if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
					out.finite = false;
					out.firstBadBlock = out.firstBadBlock < 0 ? burstFrames / kFrames + b : out.firstBadBlock;
				}
				if (l[i] != 0.0f || r[i] != 0.0f) {
					out.lastNonZeroBlock = b;
				}
			}
		}
		out.holds = dev.ops->holdsTail != nullptr && dev.ops->holdsTail(dev.mem);
		return out;
	};

	int settled = 0;
	std::vector<std::string> latched;
	for (const device::Descriptor* desc : devices) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		const int silentBlocks = silentBlocksFor(*desc, 400);
		const Outcome clean = run(*desc, false);
		const Outcome bad = run(*desc, true);
		CHECK_MESSAGE(clean.finite, "the CLEAN run is not finite on ", who);
		CHECK_MESSAGE(bad.finite, "a NaN/Inf input left a non-finite output on ", who, " (first in block ", bad.firstBadBlock, ")");
		if (!bad.finite) {
			latched.push_back(who);
		}
		if (clean.lastNonZeroBlock < silentBlocks - 1) {
			++settled;
			CHECK_MESSAGE(bad.lastNonZeroBlock < silentBlocks - 1, "the tail no longer reaches exact zero after a NaN/Inf input on ", who);
			const bool promptly = bad.lastNonZeroBlock <= clean.lastNonZeroBlock + 2 || bad.lastNonZeroBlock < silentBlocks / 2;
			CHECK_MESSAGE(promptly,
			              "the tail is far longer after a NaN/Inf input on ", who);
			CHECK_MESSAGE(bad.holds == clean.holds, "holdsTail differs after a NaN/Inf input on ", who);
		}
	}
	for (const std::string& who : latched) {
		MESSAGE("NON-FINITE OUTPUT AFTER NaN/Inf INPUT: " << who);
	}
	// NON-VACUITY: the tail half is read only where the clean run settles, so most devices have to.
	CHECK(settled * 2 >= static_cast<int>(devices.size()));
}

TEST_CASE("a device holding sound behind an exactly silent output says so: holdsTail is true in the gap and false once the tail is gone") {
	// **THE SLEEP RULE'S SECOND HALF (backlog MC).** A host may SLEEP the plugin on an exactly silent block, and a delay
	// line is the device whose output is exactly silent while it still holds sound: between a click and its first
	// repeat. `AudioOps::holdsTail` is the device saying so. Asserted on the VALUE, per device, against a gap the
	// device really has: the click goes in, and the case must find a block that is EXACTLY silent while the op says
	// "holding" AND a later block that sounds — so the op answered true where a silent-output test would have slept
	// and lost what came next. Then the op must clear, and once a block is silent with nothing held, nothing more may
	// come out (that is the promise a SLEEP makes). Blocks of 32, a small host's, so a gap of a few ms is a whole block.
	struct Gap {
		const char* tag;
		void (*set)(DeviceRow& row);
		int seconds; // long enough for the tail to go AND the op to clear: a line clears a whole length after
	};
	const Gap gaps[] = {
		// CLEAN, WET ONLY, A 1 s TAP: a second of silence before the first repeat. The line is 16 s long and clears a
		// whole length after the last sound written into it.
		{ fx::delay::kTraits.tag,
		  [](DeviceRow& row) {
			  row.field[static_cast<size_t>(fx::delay::Param::Mix)] = 1.0f;
			  row.field[static_cast<size_t>(fx::delay::Param::Sync)] = 2.0f; // 1 s
			  row.field[static_cast<size_t>(fx::delay::Param::Feedback)] = 0.3f;
		  },
		  60 },
		// THE FLANGER, WET ONLY, 15 ms BEHIND, FEEDBACK UP AND THE LFO OFF: a click goes round the loop with exact zeroes
		// between laps.
		{ fx::phaser::kTraits.tag,
		  [](DeviceRow& row) {
			  row.field[static_cast<size_t>(fx::phaser::Param::Mode)] = 1.0f; // Flanger
			  row.field[static_cast<size_t>(fx::phaser::Param::Mix)] = 1.0f;
			  row.field[static_cast<size_t>(fx::phaser::Param::Feedback)] = 0.8f;
			  row.field[static_cast<size_t>(fx::phaser::Param::DelayMs)] = static_cast<float>(std::log2(0.015));
			  row.field[static_cast<size_t>(fx::phaser::Param::LfoOn)] = 0.0f;
		  },
		  20 },
		// THE COMB AT ITS LONGEST, THE DAMPER AT 0: echoes up to 4080 samples apart.
		{ fx::filter::kTraits.tag,
		  [](DeviceRow& row) {
			  row.field[static_cast<size_t>(fx::filter::Param::Type)] = static_cast<float>(fx::filter::Type::Comb);
			  row.field[static_cast<size_t>(fx::filter::Param::Cutoff)] = static_cast<float>(fx::filter::kCutoffMinLog2);
			  row.field[static_cast<size_t>(fx::filter::Param::Resonance)] = 0.9f;
			  row.field[static_cast<size_t>(fx::filter::Param::Macro1)] = 0.0f;
		  },
		  30 },
		// THE PITCH SHIFTER, WET ONLY, ITS LONGEST GRAIN AND FEEDBACK: heads seated behind the newest sound.
		{ fx::pitch::kTraits.tag,
		  [](DeviceRow& row) {
			  row.field[static_cast<size_t>(fx::pitch::Param::Mix)] = 1.0f;
			  row.field[static_cast<size_t>(fx::pitch::Param::Size)] = 250.0f;
			  row.field[static_cast<size_t>(fx::pitch::Param::Feedback)] = 0.5f;
		  },
		  30 },
		// THE ERODER: the dry is the click itself, and the wet arrives up to 5.8 ms later.
		{ fx::eroder::kTraits.tag,
		  [](DeviceRow& row) { row.field[static_cast<size_t>(fx::eroder::Param::Mix)] = 1.0f; },
		  10 },
		// FEATHERVERB, WET ONLY, 50 ms OF PREDELAY: the reverb's own gate holds through it.
		{ fx::featherverb::kTraits.tag,
		  [](DeviceRow& row) {
			  row.field[static_cast<size_t>(fx::featherverb::Param::Mix)] = 1.0f;
			  row.field[static_cast<size_t>(fx::featherverb::Param::Predelay)] = 1.0f;
		  },
		  40 },
	};

	constexpr int kFrames = 32;
	std::vector<float> l(kFrames, 0.0f);
	std::vector<float> r(kFrames, 0.0f);
	for (const Gap& gap : gaps) {
		const device::Descriptor* desc = nullptr;
		for (const device::Descriptor* d : audioDevices()) {
			if (device::sameTag(d->traits.tag, gap.tag)) {
				desc = d;
			}
		}
		REQUIRE(desc != nullptr);
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		REQUIRE(desc->audio->holdsTail != nullptr);
		DeviceRow row = enabledRowFor(*desc);
		gap.set(row);
		Raw dev(*desc, 48000.0, row);
		const auto holds = [&] { return dev.ops->holdsTail(dev.mem); };
		CHECK_FALSE(holds()); // a device that has heard nothing holds nothing

		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		l[0] = 0.8f;
		r[0] = 0.8f;
		dev.process(l.data(), r.data(), kFrames);
		CHECK(holds()); // the click is inside

		int gapBlock = -1;     // the first exactly silent block the op called holding
		int soundAfterGap = -1; // a block that sounded after it
		int clearedAt = -1;     // the first block the op said nothing is held
		bool sleepable = false; // a silent block with nothing held: where a host may now SLEEP
		int soundAfterSleepable = 0;
		const int blocks = gap.seconds * 48000 / kFrames;
		for (int b = 0; b < blocks; ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			dev.process(l.data(), r.data(), kFrames);
			bool sounding = false;
			for (int i = 0; i < kFrames; ++i) {
				sounding = sounding || l[i] != 0.0f || r[i] != 0.0f;
			}
			if (clearedAt >= 0) {
				// AN IIR STAGE AFTER A LINE (the Eroder's filters) may ring on audibly after the line is empty: that is
				// sound, and a host stays awake for sound. The promise is about what follows a SILENT block.
				soundAfterSleepable += sleepable && sounding ? 1 : 0;
				sleepable = sleepable || !sounding;
				CHECK_FALSE(holds()); // and once clear, it stays clear with nothing going in
				continue;
			}
			if (!sounding && gapBlock < 0 && holds()) {
				gapBlock = b;
			}
			if (sounding && gapBlock >= 0 && soundAfterGap < 0) {
				soundAfterGap = b;
			}
			if (!holds()) {
				clearedAt = b;
				sleepable = !sounding;
			}
		}
		CAPTURE(gapBlock);
		CAPTURE(soundAfterGap);
		CAPTURE(clearedAt);
		CHECK(gapBlock >= 0);       // an exactly silent block, with the op saying "holding"...
		CHECK(soundAfterGap >= 0);  // ...that was right: sound came after it
		CHECK(clearedAt >= 0);      // the op clears, without a timeout, once the tail is gone
		CHECK(soundAfterSleepable == 0); // and once a block is silent with nothing held, nothing more comes out
	}
}

namespace {

/// What the honesty sweep found over one set of devices: the silent controls, the counts its floors read, and who it swept.
struct HonestySweep {
	std::vector<std::string> dead;
	int examined = 0;
	int arrangementsBuilt = 0;
	std::vector<std::string> swept;
};

/// **THE CONTROLS THE SWEEP VARIES: all of a device's, except the Rack's chain slots** (backlog NO item 2). A slot param
/// is heard only where a holder mirrors the body into it, and the sweep's bare rack has none, so all 96 came out silent.
/// Each silent control costs a render pair over EVERY arrangement, and each also ADDED arrangements: the case went from
/// 76 s to 167 s at load ~9 (MEASURED 2026-10-29: the Rack alone 135 s of 206 s with them, 3.7 s without). They are swept
/// where they CAN be heard, by value, every one: `test_parallel_rack.cpp`, "NO item 2: every chain slot param of every slot
/// changes the sound, by value — level, mute and solo, each routed in turn".
size_t sweptControls(const device::Descriptor& d) {
	const size_t all = std::min(d.params.size(), kMaxDeviceFields);
	return device::sameTag(d.traits.tag, rack::parallel::kTraits.tag) ? std::min(all, rack::parallel::kSlotParamBase) : all;
}

/// THE HONESTY SWEEP over every audio device `include` accepts. ONE SWEEP SPLIT FOUR WAYS (2026-10-18) so the suite can be
/// sharded: the cases after `honestyExpected` each take a part, and the last takes everything the others do not.
HonestySweep honestySweep(const std::function<bool(const device::Descriptor&)>& include) {
	// **THE HONESTY RULE, ASKED OF THE DSP RATHER THAN OF THE CARD.** CLAUDE.md: *"a control is
	// enabled iff it changes sound through a real param — verify by reading the engine, not by
	// grepping ParamIds."* The sweep that found six bugs in an afternoon asked whether every card's
	// enable switch could be switched; this asks the harder half, of all 168 controls the five
	// devices declare: does moving it move the audio?
	//
	// **"IN SOME ARRANGEMENT" IS THE WHOLE PRECISION**, and the first draft of this case got it wrong
	// in a way worth recording, because the wrong version is the tempting one. Driven against each
	// device's enabled DEFAULT row it reported ELEVEN dead controls, and every one was the fixture's
	// fault:
	//
	//   - **Drawn's Morph X/Y, Cutoff, Res and Wake.** The shipped default is four IDENTICAL bypassed
	//     corners — `test_fx_drawn.cpp` asserts it at exact equality — so the puck moves between four
	//     copies of the same filter and the globals transform a bypassed frame into a bypassed frame.
	//     Correct behaviour, and invisible until the frame is not flat.
	//   - **Waypoint's Cutoff 4, Resonance 4, Macro 1 4, Macro 2 4.** The fourth waypoint is only read
	//     near the top of `Morph`, and `Morph` is CONTINUOUS — so an arrangement set that walked only
	//     the stepped controls never went there.
	//   - **Waypoint's Keytrack.** It shifts the corner by the note, and the fixture delivered no note.
	//
	// So the arrangements walk the continuous controls as well as the stepped ones, and a note is
	// delivered to every device that has a road for one. **A sweep whose fixture cannot reach a
	// control reports it dead, which is the same word it uses for a real defect.**
	REQUIRE(audioDevices().size() >= 5);
	std::vector<const device::Descriptor*> devices;
	for (const device::Descriptor* desc : audioDevices()) {
		if (include(*desc)) {
			devices.push_back(desc);
		}
	}
	REQUIRE_FALSE(devices.empty());

	constexpr int kFrames = 256;
	// A NOTE WELL AWAY FROM THE FILTER'S REFERENCE KEY, so a keytracking corner has somewhere to move.
	const device::NoteContext note { 84.0f, true };
	// BOTH CHANNELS, and the first version kept only the left. `Harm Spread` puts its offset on the
	// RIGHT channel alone — *"SPREAD IS A HALF TURN AT THE TOP"*, applied to `r` and never to `l` —
	// so a left-only probe reported a working stereo control as dead. A control that moves one
	// channel moves the sound.
	std::vector<std::string> dead;
	int examined = 0;
	int arrangementsBuilt = 0;
	std::vector<std::string> swept;
	for (const device::Descriptor* desc : devices) {
		swept.push_back(deviceName(*desc));
		// PER DEVICE, and this is the sweep where it matters most: this is the HONESTY guard, and a
		// probe too short to hear a device reports every one of its controls dead. See `framesFor`.
		const int frames = framesFor(*desc, kFrames);
		const std::vector<float> probe = probeSignal(frames, 0.35f, 0xc0ffeeu);
		// **AND A STEREO ONE.** The right channel is the same sine under its OWN noise, so the probe has a Mid (the shared
		// sine) and a Side (the two noises' difference). It was one buffer on both channels until 2026-10-11, which has no
		// Side, so every control that acts only on one — a width, an M/S switch — read as dead here and had to be listed.
		const std::vector<float> probeRight = probeSignal(frames, 0.35f, 0x5e1de5e1u);
		// BOTH CHANNELS, and the first version kept only the left. `Harm Spread` puts its offset on
		// the RIGHT channel alone, so a left-only probe reported a working stereo control as dead.
		std::vector<float> aL(static_cast<size_t>(frames), 0.0f);
		std::vector<float> aR(static_cast<size_t>(frames), 0.0f);
		std::vector<float> bL(static_cast<size_t>(frames), 0.0f);
		std::vector<float> bR(static_cast<size_t>(frames), 0.0f);
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		const size_t enable = device::enableControlIn(desc->traits, desc->params);
		REQUIRE(enable != device::kNoControl);
		// **POLY IS EXEMPT AND THE EXEMPTION IS DERIVED.** `Traits::perVoiceControl` names the control
		// that decides how many instances the HOST runs and where they sit — `registry.h`:
		// *"ARCHITECTURE IS NOT A SETTING"*. One instance rendering one buffer cannot see it, and it
		// would be wrong if it could. Asked of the declaration rather than of the name "Poly".
		const int poly = desc->traits.perVoiceControl;

		// THE ARRANGEMENTS: the enabled default row, plus that row with EACH other control pushed to
		// each end (stepped controls walked value by value). One control moved per arrangement, not
		// the cross product — enough to reach a waypoint at the far end of a morph or a band that is
		// no longer bypassed, which is what the eleven false positives needed.
		std::vector<DeviceRow> arrangements { enabledRowFor(*desc) };
		for (size_t p = 0; p < sweptControls(*desc); ++p) {
			if (p == enable) {
				continue;
			}
			const device::ParamDesc& d = desc->params[p];
			if (d.stepped) {
				const auto lo = static_cast<int>(std::lround(d.min));
				const auto hi = static_cast<int>(std::lround(d.max));
				for (int v = lo; v <= hi; ++v) {
					DeviceRow row = enabledRowFor(*desc);
					row.field[p] = static_cast<float>(v);
					arrangements.push_back(row);
				}
			}
			else {
				// FIVE POINTS ACROSS THE RANGE, NOT TWO. `Morph` is continuous and its INTERIOR is
				// where the middle waypoints live: with four waypoints, waypoint 1 is at morph 1/3 and
				// waypoint 2 at 2/3, so an arrangement set that visited only the ends reached the
				// first and last waypoints and reported the middle two dead.
				for (const double t : { 0.0, 1.0 / 3.0, 0.5, 2.0 / 3.0, 1.0 }) {
					DeviceRow row = enabledRowFor(*desc);
					row.field[p] = static_cast<float>(d.min + (d.max - d.min) * t);
					arrangements.push_back(row);
				}
			}
		}
		// **THE PHASER/FLANGER'S EXPLORE PILLS** stand in front of the travel's fields (the house `fieldWithOverride`,
		// 2026-10-04): a knob away from 0.5 moves the sound at zero travel too now (0 and 1 force the field), so the rows
		// above already reach most of them — but Colour is heard only while Damping runs and Bias only with some Drive,
		// and the POWERS (off = the group's travel at 0) are heard only where there is travel. So PHFL also gets three
		// bases WITH travel — the row's extra state, folded as the pad stores it — plain, with Damping running and with
		// some Drive. Each pill is then probed over them like any control over any base.
		if (std::string_view(desc->traits.tag) == fx::phaser::kTraits.tag) {
			DeviceRow travelled = enabledRowFor(*desc);
			const size_t first = desc->params.size();
			REQUIRE(travelled.fieldsPresent == first + desc->traits.extraState);
			for (size_t i = 0; i < desc->traits.extraState; ++i) {
				const double v = 0.137 * static_cast<double>(i + 1) + 0.29;
				travelled.field[first + i] = static_cast<float>(v - std::floor(v));
			}
			arrangements.push_back(travelled);
			DeviceRow damped = travelled;
			damped.field[static_cast<size_t>(fx::phaser::Param::Damping)] = static_cast<float>(std::log2(3000.0));
			arrangements.push_back(damped);
			DeviceRow driven = travelled;
			driven.field[static_cast<size_t>(fx::phaser::Param::Drive)] = 0.4f;
			arrangements.push_back(driven);
		}
		// **NOISE'S IMPULSES** are read only under Source = Impulses, Jitter only with Timing Regular and Cluster only with
		// Clustered — gated twice over, and one control moved per arrangement never pairs them. So NOISE gets three bases on
		// Impulses, one per timing, at 256 clicks a second so a short probe holds dozens of them.
		if (device::sameTag(desc->traits.tag, fx::noise::kTraits.tag)) {
			for (const float timing : { 0.0f, 1.0f, 2.0f }) {
				DeviceRow clicks = enabledRowFor(*desc);
				clicks.field[static_cast<size_t>(fx::noise::Param::Source)] = static_cast<float>(fx::noise::Source::Impulses);
				clicks.field[static_cast<size_t>(fx::noise::Param::Density)] = 8.0f;
				clicks.field[static_cast<size_t>(fx::noise::Param::Timing)] = timing;
				arrangements.push_back(clicks);
			}
		}
		// **COMP'S COMPANDER SECOND HALF (backlog QG)** is read only in Compander mode, in a paired topology, while Link is off —
		// gated three deep, and one control moved per arrangement never reaches it. And at the defaults Link itself is silent BY
		// DESIGN (the second half's three start at the first half's, so unlinking changes nothing). So COMP gets two bases,
		// one per paired topology, unlinked and with the first half's Ratio moved off the second's.
		if (device::sameTag(desc->traits.tag, fx::comp::kTraits.tag)) {
			for (const fx::comp::Topology t : { fx::comp::Topology::Compander, fx::comp::Topology::Excomper }) {
				DeviceRow pair = enabledRowFor(*desc);
				pair.field[static_cast<size_t>(fx::comp::Param::Mode)] = static_cast<float>(fx::comp::Mode::Compander);
				pair.field[static_cast<size_t>(fx::comp::Param::Topology)] = static_cast<float>(t);
				pair.field[static_cast<size_t>(fx::comp::Param::CmpLink)] = 0.0f;
				pair.field[static_cast<size_t>(fx::comp::Param::CmpRatio)] = 3.0f;
				arrangements.push_back(pair);
			}
		}
		arrangementsBuilt += static_cast<int>(arrangements.size());

		const auto renderWith = [&](const DeviceRow& row, std::vector<float>& left,
		                            std::vector<float>& right) {
			Raw dev(*desc, 48000.0, row);
			// THE NOTE, for every device that has a road for one — Keytrack shifts the corner BY the
			// note, so without one it is a control with nothing to track and reads as dead.
			if (desc->audio->setNoteContext != nullptr) {
				desc->audio->setNoteContext(dev.mem, note);
			}
			// AND THE TEMPO, FOR THE SAME REASON, added when the fx lane gained a clock. A synced
			// control falls back to its free rate when the host has said nothing — correct
			// behaviour, and it makes the control read as DEAD to a fixture that delivers no tempo.
			// **The fixture's job is to reach the arrangement where a control matters**, and for a
			// sync control that arrangement is "a host with a tempo".
			//
			// 120 BPM: half a second a beat, so a 1/4 division is 2 Hz and a 1/8 is 4 — far enough
			// apart that moving the control moves the sound.
			if (desc->audio->setTransportClock != nullptr) {
				desc->audio->setTransportClock(dev.mem, device::TransportClock::fromSecondsPerBeat(0.5));
			}
			std::copy(probe.begin(), probe.end(), left.begin());
			std::copy(probeRight.begin(), probeRight.end(), right.begin());
			if (desc->notes == nullptr) {
				dev.process(left.data(), right.data(), frames);
				return;
			}
			// **A DEVICE WITH A NOTE ROAD IS PLAYED NOTES, THE WAY A HOST PLAYS IT: between process calls.**
			// `setNoteContext` above is the per-voice road and a note-road device has none, so without this
			// the fixture never plays it a note and every control that acts on notes reads as dead — the
			// fixture's gap, not the device's. One third with no note (a drone, if the device has one, is
			// heard), then a note, then a second a fifth above while the first is held: two held notes is
			// what makes Poly differ from Mono, and a note CHANGING while held is what a glide acts on.
			const int third = frames / 3;
			const auto play = [&](int16_t key) {
				device::NoteEvent on;
				on.kind = device::NoteEvent::Kind::On;
				on.key = key;
				on.velocity = 0.8;
				device::NoteEvent out[4];
				size_t n = 0;
				desc->notes->processNotes(dev.mem, &on, 1, out, 4, &n);
			};
			dev.process(left.data(), right.data(), third);
			play(72);
			dev.process(left.data() + third, right.data() + third, third);
			play(79);
			dev.process(left.data() + 2 * third, right.data() + 2 * third, frames - 2 * third);
		};

		for (size_t p = 0; p < sweptControls(*desc); ++p) {
			if (p == enable || (poly >= 0 && static_cast<int>(p) == poly)) {
				continue;
			}
			++examined;
			const device::ParamDesc& d = desc->params[p];
			// **THE ENDS ARE NOT ENOUGH FOR A CONTROL WHOSE ENDS ARE NOT A SETTING.** `Automod :: Rotate`
			// used to span 0 to 2*pi, and while `settingsOf` wrapped a full turn onto zero its two ends
			// were literally the same setting: `min` against `max` compared a setting with itself and
			// the sweep reported a control that plainly moves the sound as dead. **Both halves of that
			// are gone since 2026-09-08** — the wrap is removed (a turn is NOT the identity; the basis
			// is phi triangles and phi is irrational, measured 1.607 against 1.140) and the range is now
			// the shaper's +/-8,388,608 drag window. The ends are still no good, for the new reason:
			// they are 16 million apart, which is a probe nothing renders meaningfully. The middle is
			// what this fixture uses, and it was the right answer under both regimes.
			//
			// So try the midpoint as well. It is general rather than a carve-out for one knob — any
			// wrapping or symmetric control has this shape, and the next one will not have to be
			// diagnosed from scratch. **Ordered so the ends go first and the midpoint is only reached
			// by a control that the ends failed to move**, which keeps the cost at zero for the 200-odd
			// controls that were already fine.
			//
			// **AND A FOURTH PROBE THAT IS NOT A POWER OF TWO, because the three above are.** A phase
			// control's range here is ±2^23 and its midpoint is 0, so every pair above feeds the device
			// a DYADIC value — and a phase is consumed as `wrapPhase(value * kPhiNNN)` where the φ
			// constant is a float. `2^23 × (a 24-bit mantissa)` is an exact integer, so it wraps to
			// **zero**: at the ends of its own declared range, `Shaper Gamma` renders identically to
			// gamma 0, and the sweep called a live control dead.
			//
			// **IT WAS HIDDEN BY A SECOND PARAM RATHER THAN BY LUCK.** `Shaper Colour Phase` used to be
			// summed into gamma before the derive read it, and the arrangements' nonzero value made the
			// sum non-dyadic. Deleting that param (BJ piece 3) removed the accidental dither and left
			// the degeneracy in plain sight — which is the good outcome: the fixture was passing for a
			// reason unrelated to what it was asserting.
			//
			// The offsets are deliberately ugly. A fixture whose probe values share a factor with the
			// quantity under test is a fixture that cannot express the failure — the same rule as the
			// non-dyadic coordinates in `test_fx_automod.cpp`'s phase-space case, for the same reason.
			const double span = d.max - d.min;
			const std::pair<double, double> probes[] = {
				{ d.min, d.max },
				{ d.min, d.min + span * 0.5 },
				{ d.min + span * 0.5, d.max },
				{ d.min + span * 0.3137, d.min + span * 0.7211 },
			};
			bool moved = false;
			for (const auto& [lowValue, highValue] : probes) {
				for (const DeviceRow& base : arrangements) {
					DeviceRow lowRow = base;
					DeviceRow highRow = base;
					lowRow.field[p] = static_cast<float>(lowValue);
					highRow.field[p] = static_cast<float>(highValue);
					if (lowRow.field[p] == highRow.field[p]) {
						continue;
					}
					renderWith(lowRow, aL, aR);
					renderWith(highRow, bL, bR);
					for (int i = 0; i < frames; ++i) {
						if (aL[i] != bL[i] || aR[i] != bR[i]) {
							moved = true;
							break;
						}
					}
					if (moved) {
						break;
					}
				}
				if (moved) {
					break;
				}
			}
			if (!moved) {
				dead.push_back(who + " :: " + (d.name == nullptr ? "<unnamed>" : d.name));
			}
		}
	}
	return HonestySweep { dead, examined, arrangementsBuilt, swept };
}

/// THE ACCOUNTED-FOR SET: every control, of every device, that this fixture is known not to hear, each with its reason.
std::vector<std::string> honestyExpected() {
	// **THE SET, NOT `dead.empty()`, AND EVERY NAME ON IT IS ACCOUNTED FOR.** Eight of the 165
	// controls are silent here, and each is a limit of the FIXTURE rather than a defect. Asserting the
	// whole set rather than emptiness is what makes a NEW silent control — the regression this case
	// exists for — a failure, while keeping the eight from being quietly re-discovered every time
	// somebody runs it.
	//
	//   Harm Phase          WAS ON THIS LIST AND IS NOT ANY MORE, and the reason it came off is worth
	//                       keeping. Its entry read: *"`min` and `max` ARE THE SAME POINT. Phase is in
	//                       turns and the oscillator is `sin(2*pi*(phase + offset))`, so 0 and 1 are
	//                       one place on the circle. Nothing to fix and nothing a fixture can do."*
	//                       The diagnosis was exactly right and the last sentence was false — the
	//                       fixture could try the MIDDLE, and half a turn is audible. It was written
	//                       while every probe was an endpoint, so "nothing a fixture can do" meant
	//                       "nothing THIS fixture does", and the two are not the same sentence.
	//                       **An accounted-for entry is a claim, and it ages against the instrument
	//                       it was written for.** `Automod :: Rotate` is what forced the re-reading:
	//                       a second control arriving with the identical symptom, which is how a
	//                       one-off explanation gets recognised as a class. (It was called CIRCULAR
	//                       here; it is not, and the claim that a turn was the identity was retired
	//                       with the engine wrap that made it look true — see above.)
	//
	//   Harm Release        TWO TRANSITIONS THIS FIXTURE DOES NOT MAKE. A release time acts when the
	//   Harm Porta          gate FALLS and a portamento when the note CHANGES; the probe holds one
	//                       note and one gate for 256 samples, so neither control has anything to act
	//                       on.
	//
	//                       **`OWLTT Release` WAS ON THIS LIST AND HAS LEFT IT, which is a real change
	//                       and not a fixture tweak.** Until `runEnvelope` was corrected, the descent
	//                       from the upward expander's idle gain was classified as a release, so the
	//                       release knob governed the ATTACK and the attack knob governed nothing —
	//                       and against a steady probe that made Release look inert for the wrong
	//                       reason. With attack and release doing their own jobs the release acts on
	//                       the probe's own level fluctuations, so it moves the sound and the set
	//                       shrank by one. The assertion caught the change rather than the change
	//                       being noticed.
	//
	//   Waypoint Macro 1 2  A TYPE AND A MORPH TOGETHER, and this is the one that was measured rather
	//   Waypoint Macro 1 3  than reasoned. `waypoint::targetForTest` shows the value reaching the
	//   Waypoint Macro 1 4  DSP's own `Settings` at every morph position and every type — so the road
	//                       is intact. Whether that setting is AUDIBLE is the filter type's business
	//                       (`MacroKind`), and reaching waypoint 2, 3 or 4 needs `Morph` moved as
	//                       well. This fixture moves ONE control per arrangement, so it never pairs
	//                       them.
	//
	//   Drawn Wake          IT COLOURS THE JOURNEY AND NEVER THE DESTINATION — the device's own words,
	//                       and its suite asserts exactly that. A freshly built device SNAPS to its
	//                       first settings, so there is no journey here to colour.
	//
	//   Shaper Harmonics    **OFF THIS LIST (2026-09-22): THE CONTROL BECAME `Shaper Emphasis`.** It
	//                       set only the slew stage's duty, heard only through Lowpass or Integrator,
	//                       and in practice it was never heard to affect the sound. Its slot is now
	//                       a pre/de-emphasis tilt round
	//                       the curve, audible wherever the curve bends — which this fixture reaches.
	//
	//   Shaper Sub          **OFF THIS LIST NOW, AND NOT BECAUSE THE DEVICE CHANGED — BECAUSE THE
	//                       FIXTURE CAN FINALLY REACH THE FIELD.** The entry stood here and read:
	//                       *"THE SWITCH IS LIVE AND THE FIELD IS SILENT AT THIS POSITION"* — the
	//                       sub-harmonic's depth is not a control, it is read off the colour field,
	//                       whose triangle runs at 30% duty, so most of the axis asks for no sub at
	//                       all. That was true, and its stated cause was *"pairing it with Colour is
	//                       what this fixture cannot do"*.
	//
	//                       The pairing was always available through GAMMA, which is the field's
	//                       second axis and which this sweep samples at five points of its declared
	//                       range like any other continuous control. It did not work because
	//                       `stateOf` clamped gamma to 0..1 while the declaration ran to +-2^24, so
	//                       all five of those points decoded to just 0 or 1 and the field never
	//                       moved. With the clamp gone the five are five real positions, one of them
	//                       has the sub alive, and the switch is audible in that arrangement.
	//
	//                       **SO THE ENTRY WAS A DESCRIPTION OF A BUG WEARING THE SHAPE OF A FIXTURE
	//                       LIMIT**, which is the failure mode a known-gap table has: it records that
	//                       something is unreachable without asking why. Attribution measured both
	//                       ways — with the clamp restored this case is green with the entry present,
	//                       and without it the entry has to go.
	//   Drawn <f>.<b> Type  A TYPE IS A CONSTRAINT ON A FUTURE GESTURE, NOT A VALUE IN THE SIGNAL
	//                       PATH. Setting it moves nothing by itself: it decides where the NEXT POLE
	//                       DRAG puts that band's zero, so it is heard only paired with the pole —
	//                       the same shape as Waypoint's three macros and Shaper's two, and this
	//                       fixture moves one control per arrangement.
	//
	//                       ALL TWENTY-FOUR, and the count is the design rather than an oversight:
	//                       six bands times four frames, because by design type is
	//                       independent across frames. **They are generated here rather than typed
	//                       out** — twenty-four literals would be a list nobody could check against
	//                       the device, and the loop below is the same `(frame, band)` walk the
	//                       descriptor table itself is built from.
	//
	//                       MEASURED RATHER THAN REASONED, which this list requires:
	//                       `test_fx_drawn.cpp`'s "a stored type binds the zero to the pole" pairs a
	//                       type with a pole drag and asserts the zero lands on
	//                       `zeroPlacementFor(type, pole)`; and "two frames whose bands differ only
	//                       by type sound identical" asserts the other half directly — with no pole
	//                       moved, the whole type axis is sample-for-sample silent, which is the row
	//                       this fixture sees.
	std::vector<std::string> expected {
		"Harm :: Harm Release",   "Harm :: Harm Porta",
		"Waypoint :: Macro 1 2",  "Waypoint :: Macro 1 3",  "Waypoint :: Macro 1 4",
		"Drawn :: Wake",
		// EQ BANDS 1 AND 8 ARE CUTS AND ARE OFF BY DEFAULT (a cut that is on changes the sound, and the
		// default EQ is identity). Their Gain is gated to Bell and the shelves, so it is heard only after
		// TWO moves — the band switched on AND its type set to one with a gain. This fixture moves one
		// control per arrangement. Bands 2-7 are on and have a gain type, and their Gain is reached.
		"EQ :: Band 1 Gain",      "EQ :: Band 8 Gain",
		// THE DISPERSER'S 2f POWER: the echo written at twice the delay's rate is written only while Chirp writes
		// (`chirpGain2F_ = writeAmount × blend`), and at the default Twist (Width) there is no Chirp, so switching
		// 2f off is heard only after TWO moves — Chirp up AND the power off. Its AMOUNT is reached since the pills
		// default on (round 3, 2026-09-27): the varied row lifts Chirp's pill too. `test_gui_widgets_devices`'
		// "DISPERSER's pills" asserts both move the blend the engine reads.
		"Disperser :: Disperser 2f On",
		// RESONATE'S NOTE RELEASE decides what a NOTE-OFF does to its note's head (Gate stops it, Ring lets it ring on), so it
		// is heard only with a note held and then let go while the string rings — and this fixture plays no notes.
		// `test_fx_resonate`'s "Note Release Ring makes key length not matter" asserts both states on the output.
		"Resonate :: Resonate Note Release",
		// RESONATE'S CYCLE TRANSPORT (v11) GATES THE CYCLE ON THE HOST'S TRANSPORT, and this sweep's rows leave the Cycle Off (its
		// default), so no arrangement here can hear it. `test_fx_resonate`'s "with Transport on the Cycle plucks only while the
		// host plays" asserts it by value, stopped and playing.
		"Resonate :: Resonate Cycle Transport",
		// RESONATE'S BRIDGE CURVE (v17) shapes only the MAIN string's bridge (the notes' and the drone's strings buzz on a point),
		// and the ring meets its bridge only under Jawari with the bow up and a note played — two moves and a note this fixture
		// does not make. `test_fx_resonate`'s "Bridge Curve still shapes the main string's bridge" asserts it on the output.
		"Resonate :: Resonate Bridge Curve",
		// RESONATE'S DRONE BOUNCE (v21) is how the drone strings come off their point, and they have one only above Drone Jawari 0
		// (its default) — a second move this sweep does not make. `test_fx_resonate`'s "Drone Bounce is the drone strings' bounce"
		// asserts it on the drone's voice, and the panel greys it at Drone Jawari 0.
		"Resonate :: Resonate Drone Bounce",
		// RESONATE'S DRONE THREAD (v21) moves the drone strings' point, and they have one only above Drone Jawari 0 — the same
		// second move. `test_fx_resonate`'s "Drone Thread moves the drone strings' point" asserts it by value.
		"Resonate :: Resonate Drone Thread",
		// RESONATE'S DRONE TO WET FX (2026-10-05) ROUTES BETWEEN ITS TWO FX LANES, and this sweep runs every device with its
		// lanes EMPTY — where the two routings are the same sound BY DESIGN, bit for bit. `test_resonate_lanes` asserts both
		// routings by value with a device in each lane, and that the empty-lane render is the DSP's own in both.
		"Resonate :: Resonate Drone to Wet FX",
	};
	for (size_t f = 0; f < feathers::fx::drawn::kFrames; ++f) {
		for (size_t b = 0; b < feathers::fx::drawn::kBands; ++b) {
			expected.push_back("Drawn :: " + std::to_string(f + 1) + "." + std::to_string(b + 1)
			                   + " Type");
		}
	}
	//   Automod Rotate      A RE-AIMING IS WHERE THE NEXT GESTURE GOES, NOT A CHANGE TO THE SOUND
	//                       STANDING THERE — the same decision twice: for Explore in `slice_basis.h`
	//                       (a rotation that changed the sound would be redundant with gamma, so it
	//                       keeps the sound until you move in xy/gamma) and for this device on
	//                       2026-09-01 (a build where rotate changed the sound with the other
	//                       controls stationary was wrong). The same
	//                       shape as Drawn's band types above: a constraint on a future gesture.
	//
	//                       IT WAS OFF THIS LIST BECAUSE IT WAS WRONG. An earlier version applied the
	//                       rotation to the stored phases inside `coordsOf` precisely so this sweep
	//                       would see it move, and that is the audible defect. The sweep asks
	//                       "does turning this alone change the sound", and for a re-aiming the
	//                       honest answer is no.
	//
	//                       MEASURED, NOT REASONED, as this list requires. `test_fx_automod.cpp`'s
	//                       "Rotate alone leaves the derived phases standing" asserts the standing
	//                       triple bit-identical across a Rotate change with the puck off the
	//                       centre (the row this fixture sees), and its next-drag subcase asserts the
	//                       other half: the pad's decode of the same position under a different
	//                       Rotate is a different triple AND a different derive, so the control is
	//                       live through the gesture it constrains. Known-badded by restoring the
	//                       `rotatePhases` call in `coordsOf`; that case goes red and this entry
	//                       would have to leave the list.

	// **`Shaper :: Rotate` IS THE SAME FACT ABOUT THE SAME MECHANIC.** The plane's turn mutates the
	// BASIS and leaves the puck, so the sound at rest cannot move — what changes is where the NEXT
	// gesture goes. Decided when the shaper gained a plane, and it is why the bit
	// rotation that used to hold this name was renamed `Mangle`: two unrelated `Rotate`s on one
	// device would have made this entry ambiguous about which one was dead.
	//
	// MEASURED, NOT REASONED, as this list requires: `test_fx_shaper.cpp`'s "Rotate alone leaves the
	// shaper's phases standing" asserts the eight derived phases bit-identical across a Rotate change,
	// and would go red if anything let the basis reach the read.
	expected.push_back("Shaper :: Shaper Rotate");

	// **`Spectral :: Spectral Curve` SWITCHES IN A CURVE THAT IS FLAT UNTIL IT IS DRAWN.** The bands are row state (stored in dB,
	// a zeroed row reads 0 dB everywhere: `spectral_types.h`), and no arrangement here draws one, so the switch multiplies
	// every bin by exactly 1. "spectral: the curve in Hz lies on a log axis..." in test_fx_spectral asserts what a drawn
	// curve does, bin by bin.
	expected.push_back("Spectral :: Spectral Curve");
	// **THE VOCODER'S KNOBS NEED A SOUNDING SIDECHAIN, and these sweeps connect none** (design §10.1: Vocode on with a silent
	// sidechain is silent, whatever its knobs say; the switch itself is heard, as that silence). test_fx_spectral's "vocoder: ..."
	// cases drive one and assert what each does.
	for (const char* name : { "Spectral :: Spectral Vocode Bands", "Spectral :: Spectral Vocode Attack",
	                          "Spectral :: Spectral Vocode Release", "Spectral :: Spectral Vocode Flatten",
	                          "Spectral :: Spectral Vocode Sibilance", "Spectral :: Spectral Vocode Carrier",
	                          "Spectral :: Spectral Vocode Gate", "Spectral :: Spectral Vocode Freeze", "Spectral :: Spectral Vocode Low",
	                          "Spectral :: Spectral Vocode High", "Spectral :: Spectral Vocode Stereo", "Spectral :: Spectral Vocode Bandwidth",
	                          "Spectral :: Spectral Vocode Spacing", "Spectral :: Spectral Vocode Spacing Amount", "Spectral :: Spectral Vocode Spacing Free",
	                          "Spectral :: Spectral Vocode Widths", "Spectral :: Spectral Vocode Widths Amount",
	                          "Spectral :: Spectral Vocode Widths Free" }) {
		expected.push_back(name);
	}
	// **FORMANT SHIFT'S HIGH AND CROSSOVER ACT ONLY UNLINKED, AND LINK ONLY WITH HIGH APART FROM LOW** (the drawer's rule,
	// spectralControlInert): the arrangements move one control at a time from Link on and both shifts at 0, so none of the three
	// is heard alone. "formant shift: ..." in test_fx_spectral asserts the shift itself.
	for (const char* name : { "Spectral :: Spectral Shift High", "Spectral :: Spectral Shift Crossover",
	                          "Spectral :: Spectral Shift Link" }) {
		expected.push_back(name);
	}
	// **CENTERS' SET IS READ ONLY WITH PARTIALS ON AND ITS MODE AT CENTERS** (backlog OI): two controls together, and this fixture
	// moves one per arrangement. "OI partials, Centers: …" in test_fx_spectral drives both and asserts each harmonic's level.
	expected.push_back("Spectral :: Spectral Partials Set");
	// **THE DELAY'S PATTERN AND LEVELS (backlog MH) CANNOT BE HEARD BY THIS PROBE'S VALUES, BY ARITHMETIC.** Each is a
	// ±2^23 zoned coordinate whose zone is `frac(c)·4`, and every probe pair above lands on a quarter: the ends and the
	// midpoint are whole numbers (zone 0, position 0), and the ugly pair is −3125595.25 and 3709442.5 as floats (zone 3
	// and zone 2, both at position 0). AT POSITION 0 EVERY FAMILY BUT CURVE IS EVEN SPACING AND LEVEL TAPS — the zones
	// meet seamlessly by design — so each pair renders the same pattern twice. Asserted by value instead, family by
	// family, in test_fx_delay's "MH: Pattern's families ..." and "MH: Levels' families ...".
	expected.push_back("Delay :: Delay Tap Levels");
	expected.push_back("Delay :: Delay Tap Pattern");
	// **THE DELAY'S CLEAR (delay-love S4) IS AN EDGE, AND THIS PROBE HOLDS A LEVEL.** Each arrangement sets the value from
	// the first block, and the first block after a reset takes Clear's level as it finds it — so a held high is no edge
	// and clears nothing, which is the rule (a patch saved with Clear high must not clear on load). Asserted by value in
	// test_delay_lane's "S4 Clear: ...", edge by edge.
	expected.push_back("Delay :: Delay Clear");

	// **`Shaper :: Shaper Deadzone` LEFT THIS LIST ON 2026-09-23 (backlog II).** It was a switch gating a
	// field that is zero at every colour this fixture visits, so it could not be heard here. As a field
	// SCALER its top lifts the width to the whole of it whatever the field offers (`fieldWithOverride`),
	// so the sweep now hears it — which is the scaler doing what it is for.
	expected.push_back("Automod :: Rotate");

	//   Chain Macro 1..8    A RACK'S MACRO IS SILENT UNTIL THE RACK ROUTES IT TO SOMETHING, and this
	//                       fixture sweeps the rack EMPTY because that is what a rack just added to a
	//                       lane is. Live through a gesture this fixture does not make — here, a route
	//                       inside the rack — the shape of Drawn's band types and Automod's Rotate.
	//
	//                       MEASURED, NOT REASONED, as this list requires, at both halves of the road:
	//                       `test_rack_engine.cpp`'s "a macro routed to a control moves that control's
	//                       RESOLVED VALUE" asserts the rack's own edges carry a macro to a control, and
	//                       `test_rack_nesting.cpp`'s "a rack device's macro control reaches a control
	//                       inside it" asserts this device's Macro controls are what set those macros.
	//                       Modulation inside a rack is the rack's edges; there is no second system.
	//   SKLITTER, thirteen    **THEY ACT ONLY IN THE LOOPER MODES, AND A LOOPER MODE CAPTURES A BAR
	//   of them             BEFORE IT PLAYS A SAMPLE.** A bar at a musical tempo is 96,000 frames at
	//                       48 kHz; this sweep's probe is 256, and `framesFor` already lifts SKLITTER's
	//                       to 8,192 -- which is what brought Engage, Mode, Rate and Quantized
	//                       OFF this list, because those act in a division-capturing mode. Lifting it to a
	//                       whole bar for every arrangement of every control is not affordable:
	//                       MEASURED, this binary already takes 69 s and a bar-length probe is
	//                       12x to 47x of that, past the container's own command bound.
	//
	//                       DRIVING IT AT A TEMPO WHOSE BAR FITS THE PROBE WAS TRIED AND MADE IT
	//                       WORSE -- fourteen dead rather than thirteen -- because shrinking the bar
	//                       shrinks the slices with it. Reverted rather than kept as a half-measure.
	//
	//                       **SO THE COVERAGE MOVES RATHER THAN DISAPPEARING, and that is the whole
	//                       basis for naming them here.** Every one of these thirteen is asserted BY
	//                       VALUE where the device can actually be heard:
	//                         Zone A / Zone B / Macro / Macro Cfg and their phases --
	//                           `test_fx_sklitter_modes.cpp`, "Zone A and Zone B drive DISJOINT halves
	//                           of the plan" and the domain sweep across both branches;
	//                         P Write -- `test_fx_sklitter.cpp`, "pWrite at zero FREEZES the capture,
	//                           and above zero lets it evolve", asserted on the RING;
	//                         Density, Stereo Width -- the looper render cases in the same file;
	//                         Pitch Scale -- `test_fx_sklitter_types.cpp`, "the pitch scale list
	//                           indexes the table it names".
	//
	//                       DELETE AN ENTRY FROM THIS BLOCK THE DAY ITS CONTROL MOVES THE SOUND IN AN
	//                       ARRANGEMENT THIS SWEEP CAN BUILD. The list going red in either direction
	//                       is the point.
	for (size_t n = 0; n < feathers::composite::kMacroCount; ++n) {
		expected.push_back("Chain :: Macro " + std::to_string(n + 1));
		// THE RACK'S MACROS, for the same reason (backlog NO): a macro reaches only a route inside, and the sweep builds none.
		expected.push_back("Rack :: Macro " + std::to_string(n + 1));
	}
	// THE RACK'S POSITION (stage 2): the sweep's rack holds ONE EMPTY CHAIN, which is the identity at every position
	// (§1's last column) — Select and Blend only move the sound across two or more chains, which this sweep cannot add.
	// Asserted BY VALUE where it is heard: `test_parallel_rack.cpp`, "Rack stage 2, Select: the chain nearest the
	// position is the one heard" and "Rack stage 2, Blend: the weights at 0, 1/4, 1/2 and 1". (The Mode left this list
	// with step 2: the sweep hears it on a one-chain rack — a switch fades the sum, and Mid/Side's M + S is the input only
	// to float precision. Which of the two it heard was not separated.)
	expected.push_back("Rack :: Rack Position");
	// AND RACK UNUSED (step 2): it decides only what a chain the mode gives no role does — run on silence or stop — and
	// such a chain is never heard. By value in `test_parallel_rack.cpp`, "rack_unused = Ring out" / "= Stop".
	expected.push_back("Rack :: Rack Unused");
	// AND CUTS 3 AND 4 (step 3): corners of the 5-band layout, and this sweep varies one control at a time from a base
	// with fewer bands — no arrangement it builds reaches them. Not dead: `test_parallel_rack.cpp`, "Rack stage 2, Bands
	// (Before): each chain hears its band" runs all four, by value; and the card hides them below 5 bands.
	expected.push_back("Rack :: Rack Cut 3");
	expected.push_back("Rack :: Rack Cut 4");
	// THE CHAIN SLOTS (backlog NO item 2) are not on this list because the sweep does not vary them: `sweptControls`
	// says why, and where every one of the 96 is heard by value.
	// **QUANTIZED JOINED THE LIST THE DAY THE DEFAULT MODE MOVED** (to Shuffle). It is Burst's
	// alone, and this sweep varies ONE control at a time from a base
	// arrangement, so with the base in Shuffle there is no arrangement it can build where Quantized
	// reaches the sound. The control is not dead; the sweep's shape cannot reach it, and
	// `test_fx_sklitter.cpp`'s "Quantize snaps the PRESS and then lets go of the knob" is where it is
	// measured instead. **This entry should come back out the day the sweep varies a gate control and
	// its dependants together**, which is the same shape as the relevance masks that now declare it.
	// **NINE OF SKLITTER'S TEN CAME OFF THIS LIST ON 2026-09-15, and that is the good news buried in a
	// red sweep.** Zone A, Zone B, Macro Cfg, Macro, P Write, Density, Gamma, Pitch Scale and Stereo
	// Width were all recorded here as controls this sweep could not reach. Every one of them is
	// mode-specific, the default mode then was one in which none of them does anything — so the
	// list was an accurate record of a sweep pointed at the one arrangement that shows least of the
	// device. Two changes the same day made them all reachable: the default moved to Shuffle
	// and `Raw` grew a moving playhead, without which a looper
	// waits for a grid line that never comes.
	//
	// **QUANTIZED IS THE ONE THAT STAYED**, and for the mirror-image reason: it is Burst's alone, so
	// with the base arrangement in Shuffle there is no row this sweep can build where it reaches the
	// sound. It is measured in `test_fx_sklitter.cpp`'s "Quantize snaps the PRESS and then lets go of
	// the knob". **This entry comes out the day the sweep varies a gate control together with its
	// dependants** -- which is exactly what `ParamDesc::gateControl` now declares, so the road is open.
	expected.push_back("SKLITTER :: Quantized");
	// **SEQ LENGTH JOINS FOR A THIRD REASON, AND IT IS THE CLEANEST OF THE THREE: THE CONTROL CANNOT
	// ACT INSIDE THIS PROBE'S WINDOW BY ARITHMETIC.**
	//
	// It folds the bar counter — `seqBarIndex()` is `barIndex_ % seqBars` — and `0 % n` is ZERO FOR
	// EVERY n. So in bar zero every rung of the ladder is the same rung, necessarily, and this sweep
	// drives `96000 + 9600` frames: one bar of CAPTURE (a looper plays nothing until its buffer is
	// full) plus a tenth. The device has barely begun playing bar zero when the probe ends.
	//
	// **THAT IS A PROOF THAT THE INSTRUMENT CANNOT SPEAK, NOT A GUESS THAT IT DID NOT.** The
	// distinction matters because "the test cannot see it" is exactly what a genuinely dead control
	// looks like too, and this list exists to catch the second kind. So it is asserted by VALUE
	// instead, where the device can be heard: `test_fx_sklitter.cpp`'s **"SEQ LENGTH changes the
	// arrangement, and it takes until the SECOND bar to say so"** drives to bar 2 by asking the bar
	// counter rather than by counting blocks, compares the published arrangement step for step at
	// seq=1 against seq=4, and asserts BOTH halves — that they differ at bar 2, and that they are
	// identical at bar 0, which is what would go red if the fold were applied in the wrong place.
	//
	// **BUFFER LENGTH, ADDED IN THE SAME COMMIT, IS NOT ON THIS LIST** — and that asymmetry is worth
	// keeping, because it is the sweep working. The buffer changes what is captured from the first
	// sample, so this probe hears it inside its own window and would go red if it stopped working.
	// Two new controls, one reachable and one not, and the list says which is which.
	expected.push_back("SKLITTER :: Seq Length");
	// **SEND OVERRIDE JOINS FOR SEQ LENGTH'S REASON, NOT QUANTIZED'S: THE EFFECT IT CONTROLS CANNOT
	// REACH THE OUTPUT INSIDE THIS PROBE'S WINDOW.**
	//
	// The knob forces the per-slice delay send to always or never (backlog EW). **The send is an
	// ECHO** — `advanceSlice` sets `delayTime_ = sliceFrames_ / 4`, a quarter of the captured BAR, and
	// the delayed copy is the only thing the send ever adds to the output. This sweep drives
	// `96000 + 9600` frames at 120 BPM: one bar of CAPTURE, because a looper plays nothing until its
	// buffer is full, plus a tenth of a bar. So it renders 9,600 frames of audio while the first echo
	// is still 24,000 frames from coming back — the control cannot be heard here however certain the
	// draw is, and a longer probe is what would be needed rather than a different arrangement.
	//
	// **MEASURED, NOT REASONED, as this list requires, and asserted by value where the device can be
	// heard.** `test_fx_sklitter.cpp`'s *"the Send Override is AUDIBLE, and the echo's quarter-bar
	// latency is why one device sweep cannot hear it"* renders this device at both ends of the knob in
	// THIS FIXTURE'S OWN WINDOW and finds the two identical (with the output non-silent, so that is a
	// statement about the send rather than about two silences), then renders 72,000 frames and finds
	// them different, with the first differing frame past a quarter bar. Both halves, so the entry is
	// attributable to the window rather than to a knob that does nothing.
	//
	// **AND THE PROBABILITY ITSELF IS ASSERTED AS A NUMBER**, which is the claim the knob actually
	// makes: `test_fx_sklitter.cpp`'s *"the Send Override knob reaches the plan the device is playing"*
	// reads `SlicePlan::delayProb` off the playing device at a HALF-STRENGTH setting — the endpoints
	// are blind to this feature by construction, since a forced-up probability of 1 is what no override
	// returns at p = 1 — and `test_fx_sklitter_modes.cpp` pins the arithmetic and the wiring.
	//
	// **THIS ENTRY COMES OUT THE DAY THIS SWEEP RENDERS PAST A QUARTER BAR OF PLAYBACK**, which is a
	// probe change and not a device one. It is not affordable today for the reason the SKLITTER block
	// above already states.
	expected.push_back("SKLITTER :: Send Override");
	// **`FB Tap` — backlog GU/GV, AND IT IS A LIMIT OF THIS FIXTURE RATHER THAN OF THE CONTROL**, on
	// two counts either of which is enough on its own.
	//
	// It chooses where the feedback loop is TAPPED: after the grain lane, or after the master lane as
	// well (an open design question: post grain fx or post master fx). So it can only be heard when
	// TWO other things are
	// true, and this sweep arranges neither: **`Feedback` must be off its default of zero** — with no
	// feedback there is nothing for a tap to choose between — and **the MASTER LANE must hold a
	// device**, because with both lanes empty the two taps are the same signal by construction, not by
	// accident. This sweep moves one control at a time against rows with empty lanes.
	//
	// **MEASURED WHERE IT CAN BE HEARD, AND AS A NUMBER RATHER THAN A DIFFERENCE**:
	// `test_fx_sklitter_feedback.cpp`'s *"the FB Tap switch really takes a different signal, and the
	// one it names"* seats a x3 device in the master lane, puts `Feedback` at full, and reads the tap
	// the device actually took off the engine — **0.5 on `Grain FX` and 1.5 on `Post FX`**, which is
	// the lane's gain exactly. A difference check could have told the two apart; only the ratio says
	// the switch took the signal it names.
	//
	// **THIS ENTRY COMES OUT THE DAY THIS SWEEP PUTS A DEVICE IN A CONTAINER'S INNER LANE**, which is
	// a probe change and not a device one — and it would retire several `Chain :: Macro N` entries
	// above with it, for the same reason.
	expected.push_back("SKLITTER :: FB Tap");
	// **`Transport Locked` — backlog GU, and it joined on a 2026-09-22 design decision, not on a regression.**
	// Until then this sweep heard the lock at the ENGAGE POINT: an unlocked press started the read
	// cursor at slice 0, a locked one at the song's slice. The decision: a press starts BOTH heads at
	// the song's phase in both settings, so the settings now differ only at a STOP — unlocked slips
	// past a stopped transport and rejoins on the restart; locked holds dry with the buffer frozen —
	// and this sweep drives a transport that never stops, for a window (`96000 + 9600` frames) too
	// short for the locked slot-end derivation to reach a slot end. Blind to the condition, not dead.
	//
	// **ASSERTED BY VALUE WHERE IT CAN BE HEARD:** `test_fx_sklitter.cpp`'s *"backlog GT: the device
	// stops with the transport, the buffer FREEZES, and the cursor is parked where it stood"* renders
	// the stop bit-for-bit dry with the ring unchanged, against a lock-off control that keeps sounding
	// and writing; *"backlog GT: a loop or a scrub the host makes WITHOUT stopping is followed, at the
	// slice boundaries"* covers the slot-end road.
	//
	// **THIS ENTRY COMES OUT THE DAY THIS SWEEP STOPS THE TRANSPORT**, a probe change and not a device one.
	expected.push_back("SKLITTER :: Transport Locked");
	// **`Grain Order` — backlog HU, AND IT IS GUARDED OFF HERE RATHER THAN MERELY QUIET.** The switch
	// chooses which of the ring's three writers lands LAST in Grain, and that question only exists
	// while a continuous writer is asking for something: with `Feedback` and `Dry Write` at their
	// defaults of zero, `Settings::writersActive()` is false and BOTH states take the same road, by
	// the same condition that makes them bit-identical at zero knobs. This sweep moves one control at
	// a time from rows that leave the others at their defaults, so it cannot reach the control by
	// construction — the same shape as `FB Tap` above, one step stronger.
	//
	// **AND IT IS AN AUDITION CONTROL, SO THIS ENTRY IS EXPECTED TO BE DELETED WITH IT.** A and B are
	// to be chosen by ear. Once one is chosen, the losing
	// order, the control and this line go together.
	//
	// **MEASURED WHERE IT CAN BE HEARD**, in `test_fx_sklitter_grainorder.cpp`: with the circuit
	// engaged the two orders differ across the whole ring — **RMS 0.118 at `P Write` 1.0 / Feedback
	// 1.0**, falling to 0.064 as Feedback approaches zero. A difference check could say the states
	// are not the same; the table says where they are most unlike, which is what a player needs.
	expected.push_back("SKLITTER :: Grain Order");
	// **`P Write` CAME BACK ON 2026-09-24 (backlog JC), AND WHAT IT HAD BEEN HEARD THROUGH WAS A DEFECT.**
	// The one road by which this short window heard `P Write` was Grain's voice A reading ONE FRAME
	// BEHIND THE WRITE HEAD for its whole first grain: armed at 0 with the head at 0, its offset was
	// `floor(phase · grainLength)`, which a double-accumulated phase floors to `k − 1` — so it read, a
	// sample later, what `P Write` had just written. The offset is now the firmware's integer counter; the
	// voice reads AT the head, before the write, as the reference does, and `P Write` reaches the output
	// a bar later instead — past the end of this window. MEASURED: the phase-derived offset restored alone
	// takes `P Write` back off this list. **Asserted by value** in `test_fx_sklitter.cpp`'s *"pWrite at
	// zero FREEZES the capture, and above zero lets it evolve"*, on the RING.
	expected.push_back("SKLITTER :: P Write");
	// **THE ELEVEN PERFORM CONTROLS WERE HERE AND ARE GONE, WHICH IS THIS LIST DOING ITS JOB.** They were
	// listed for the only honest reason on it — nothing read them: the circle's maths existed
	// (`src/fx/sklitter/perform.h`) and no plan site called it. The Perform wiring
	// joined the two, and the entry above said in as many
	// words that all eleven come out in that commit and that any left behind would mean the circle was not
	// wired. TEN of them moved the sound on the first sweep after the wiring; the eleventh is below, and
	// it is a limit of this fixture rather than of the circle.
	//
	// **AND THE DIFFERENCE BETWEEN THE TWO KINDS OF ENTRY IS WHAT MADE THAT PREDICTION CHECKABLE.**
	// Quantized and Seq Length are controls this SWEEP cannot reach — they need a better probe, and the
	// list says where each is asserted by value instead. The perform eleven were controls the DEVICE did
	// not read, which needs code. Only the second kind can be removed by writing some.
	//
	// **TEN OF THE ELEVEN LEFT. `Macro S` DID NOT, AND IT IS A NARROW QUANTIFIED MARGIN, NOT A STRUCTURAL
	// BAR.** A first draft of this entry said it "needs two controls moved at once", which reads as the
	// Waypoint-middle-corners shape above — something the fixture cannot express at all. The arithmetic
	// says something weaker and more useful, and all of it is MEASURED (2026-09-17, scratchpad probes over
	// `performPushOf`/`planFor` at the sweep's own row):
	//
	//   1. The arrangements are the default row with ONE control walked, so the row that puts the puck
	//      SOUTH (`Perf Y` at max) is also the row with **Zone B at 0 — inside `planFor`'s stride
	//      deadzone**. There the plan is SLICE-INVARIANT: of slices 1..511, **zero** differ from slice 0.
	//      **So the probe gets ONE draw however long it runs, and a longer probe cannot help.**
	//   2. At that row the S waypoint's two PHASE terms are exactly zero — its Cfg of 0.6875 puts both
	//      `triangleSimpleUnipolar` phases (0.874514 and 0.986766) past the 0.5 duty — `dryThreshold`
	//      reaches 0.449594 against the dry vote's 0.5, and `gatePull` (0.280996) and `subdivMix`
	//      (0.140855) are read inside branches that Zone B 0 and Zone A 0 hold shut. That leaves
	//      `dReverse` = 0.149342 as the only live term.
	//   3. **AND IT IS SHORT BY ABOUT 11%.** The smallest S waypoint macro that changes that one plan is
	//      **1.113**, against a declared maximum of 1. The other three are nowhere near their thresholds:
	//      N and W flip at **0.001**, E at **0.059** — which is the whole of why those three came off the
	//      list and this one did not. Nothing about S is structurally unreachable.
	//
	// **SO IT SHOULD AGE OFF THIS LIST, AND THAT WILL BE RIGHT RATHER THAN SURPRISING.** Three ordinary
	// changes would do it: a base row whose Zone B leaves the deadzone (at 0.55, macro 0 against 1 differs
	// at **64 of 64** slices), a sweep whose ARRANGEMENTS ever move two controls at once — it already
	// renders a base arrangement plus the probed control `p`, so what is missing is a base with two moved,
	// not a second variable — or the waypoint defaults
	// being retuned — which `sklitter_types.h` says outright is coming, by ear, under **backlog FP**. Any
	// of them reddens this case; the fix is then to delete this entry, not to restore it.
	//
	// **AND IT AGED OFF THIS LIST BY THE THIRD ROUTE, ON 2026-09-19: THE CONTROL WAS DELETED.** The
	// paragraph above predicted "the waypoint defaults being retuned" as one of three ordinary changes
	// that would redden this entry; what happened is blunter — `Macro N/E/S/W` are gone, because a
	// waypoint is its CONFIGURATION now and its push is built at macro 1.0, with the puck's radius
	// supplying the amount (no macro knobs: only the pad controls it). The
	// analysis above is kept because it is the record of WHY one of eleven behaved differently from the
	// other ten, and because its shape — a control whose margin is quantified rather than structural —
	// is the distinction this list exists to make. Nothing is pushed for it.
	//
	// **ASSERTED BY VALUE WHERE IT CAN BE, exactly as Seq Length is**: `test_fx_sklitter.cpp`'s *"the S
	// waypoint reaches the sound in an arrangement this fixture cannot build"* drives a playing device at
	// the puck's south rim with Zone B at 0.55, reads the push the device drew its slice from, and
	// asserts it IS the S waypoint's — then that moving `Cfg S` alone moves the plans.
	// **THE PHASER/FLANGER: ONE CONTROL THE ENGINE READS THAT THIS SWEEP CANNOT HEAR.**
	// Every control is wired; what is left is one FACT ABOUT ROTATE (it aims the explore travel
	// and never moves the sound, as the Shaper's does). Each is asserted by value in `test_fx_phaser.cpp`, named below.
	{
		const std::string phfl = std::string(feathers::fx::phaser::kDeviceName) + " :: ";
		// (TASK 3, THE FLANGER, took `Phaser Delay` and `Phaser Through Zero` off this list; TASK 4, THE
		// MODULATION, took nine of its ten source controls. The follower's Attack and Sens are gated on Env,
		// off by default, and are heard here because the arrangements include the row with Env on, the base
		// this sweep moves each of them over.)
		//
		// **ENV RELEASE IS THE TENTH, AND IT IS HARM RELEASE'S SHAPE: A TRANSITION THIS FIXTURE DOES NOT MAKE.**
		// A follower's release acts only when its input FALLS below its level. The probe is steady noise for
		// 256 frames, so with Env on (Attack at its 10 ms default) the level is still climbing toward the
		// noise's peaks — 16 ticks of a 480-sample time constant close only 1 − e^(−256/480) ≈ 41% of the gap
		// (arithmetic, not a measurement) — and every tick takes the Attack road. Release has nothing to act on. Not a gate the code could drop:
		// it is a fixture limit, the first kind of entry above. `test_fx_phaser.cpp`'s "the follower's Attack,
		// Release and Sens move the sweep with Env on" asserts all three by value against the one-pole's closed
		// form, Release on a tone that stops.
		// **AND IT LEFT THE LIST 2026-10-04**: the Flanger's latency stopped depending on Through Zero (the TZ-flip
		// restart that killed held notes), which lengthened this device's probe to 4096 frames (`framesFor`) — long
		// enough for the follower to reach the noise and take the Release road. The paragraph above is why it was here.
		// (TASK 5, THE BARBERPOLE, took `Phaser Sweep` off this list, and TASK 6, KEY TRACKING, `Phaser Key Track`:
		// this sweep delivers a held note (84) to every device with a note road, so Key Track On moves the centre. Key
		// Offset never joined the list and is retired since (2026-10-03: Center is the offset from a tracked note).)
		// RELEASE'S REACH GREW TO 20 s (2026-10-03) and changes nothing here: the probe never falls, at any Release.
		// **ROTATE STAYS, AFTER TASK 7 AS BEFORE IT — `Shaper :: Shaper Rotate`'s FACT ABOUT THE SAME MECHANIC.** The
		// explore layer (`phaser_phase_space.h`) is written, and Rotate turns the plane the pad's NEXT gesture moves in:
		// it aims the travel, it never reaches the derive, so the sound standing still cannot move — at this sweep's
		// zero-travel rows (where every offset is exactly 0 by construction) or anywhere else. MEASURED, NOT REASONED:
		// `test_fx_phaser.cpp`'s "explore: Rotate alone leaves the sound standing, with travel too" renders every
		// arrangement under two turns with the same live travel and asserts the output bit-identical, and "explore:
		// zero travel gives the plain parameter set bit for bit under any Rotate" does the same at home.
		expected.push_back(phfl + "Phaser Rotate");
		// (ROUND 2'S TEN EXPLORE PILLS AND THEIR POWERS ARE NOT ON THIS LIST: since the house law (2026-10-04) a knob is
		// heard at zero travel too, and the powers over the three arrangements WITH travel above — plain, with Damping
		// running, with some Drive — so every pill is heard over one.)
	}
	// **NOISE'S IMPULSE DECAY (backlog NA) SHAPES A MODULATION OUTPUT, NEVER THE AUDIO, BY DESIGN** — it is Random
	// Trigger's Decay on NOISE's Impulse Decay output, so it is heard only through a route, which this sweep makes none
	// of. Its claim is asserted as the number it is: test_mod_rtrig's "NOISE: Impulse Decay stands at each hit's value
	// and falls 60 dB in Decay seconds" (restated tick by tick) and "NOISE: Impulse Decay is Random Trigger's Decay, tick
	// for tick, on the same settings and Seed".
	expected.push_back("Noise :: Noise Impulse Decay");
	// **SIDECHAIN INJECT'S GAIN (backlog NW) SCALES THE SIDECHAIN, WHICH THIS SWEEP NEVER HANDS IT**: the fixture runs devices
	// on the bus road with no `setSidechain`, so the term Gain scales is silence at every arrangement (the Spectral vocoder's
	// controls are on this list for the same reason). Its claim is asserted as the number it is: test_fx_inject's "Inject at
	// Mix 1 outputs the sidechain times Gain, to the bit" (Gain +6 dB read as `gainLinForTest` and as the output's value) and
	// "Inject at Mix 50% is the two signals' average, and Gain scales only the sidechain's half".
	expected.push_back("Sidechain Inject :: Inject Gain");
	// **COMP'S STEREO MODE, OWLTT'S STEREO MODE (backlog LQ) AND OWLTT'S FOUR WIDTHS (backlog LR) WERE LISTED HERE,
	// FOR ONE REASON: THE PROBE WAS MONO**, so M/S encoded to Side = 0 and read exactly what L/R does, and a width had
	// no Side to scale. The probe is stereo now (see `probeRight`) and all six are heard — measured by deleting their
	// entries and running the sweep. COMP's would be heard even on a mono probe since backlog LE: M/S Limit splits its
	// lookahead between the Mid/Side stage and the L/R stage after decoding (`comp::midSideLookaheadFor`), so it ramps
	// over half the lookahead L/R does.
	return expected;
}

/// ONE PART OF THE SWEEP, HELD TO ITS SHARE OF THE SET: the entries of `honestyExpected` that name a device this part
/// swept. Every entry names SOME device — the catch-all part asserts it — so filtering cannot drop a stale entry silently.
void checkHonesty(const HonestySweep& sweep, int examinedFloor, int arrangementsFloor) {
	for (const std::string& s : sweep.dead) {
		MESSAGE("SILENT UNDER THIS FIXTURE: " << s);
	}
	MESSAGE("swept " << sweep.swept.size() << " devices: " << sweep.examined << " controls examined over "
	                 << sweep.arrangementsBuilt << " arrangements");
	CAPTURE(sweep.examined);
	CAPTURE(sweep.arrangementsBuilt);
	std::vector<std::string> expected;
	for (const std::string& e : honestyExpected()) {
		for (const std::string& who : sweep.swept) {
			if (e.rfind(who + " :: ", 0) == 0) {
				expected.push_back(e);
				break;
			}
		}
	}
	std::vector<std::string> dead = sweep.dead;
	std::sort(expected.begin(), expected.end());
	std::sort(dead.begin(), dead.end());
	CHECK(dead == expected);
	// FLOORS, so a sweep that examined nothing, or one whose arrangement set collapsed to the default
	// row, cannot pass by doing nothing. Each part's are what it measured on 2026-10-18, rounded down: Drawn 126 controls
	// over 703 arrangements, Resonate 53 over 382, SKLITTER 35 over 184 (34 over 179 once Direction left it, 2026-11-09), the other twenty devices 428 over 3227.
	CHECK(sweep.examined >= examinedFloor);
	CHECK(sweep.arrangementsBuilt >= arrangementsFloor);
}

/// THE THREE DEVICES THAT TAKE THREE QUARTERS OF THE SWEEP'S TIME, each swept in a case of its own so the suite can be
/// sharded (MEASURED 2026-10-18 at load ~28: Drawn 185 s, Resonate 182 s, SKLITTER 110 s, the other twenty ~140 s
/// together, in one case that was 485 s of the suite's 505). By NAME, and each named part REQUIREs that it found its
/// device, so a rename goes red instead of quietly moving the device into the catch-all.
constexpr std::string_view kHonestySweptAlone[] = { "Drawn", "Resonate", "SKLITTER" };

HonestySweep honestySweepOf(std::string_view name) {
	HonestySweep sweep = honestySweep([name](const device::Descriptor& d) { return deviceName(d) == name; });
	REQUIRE(sweep.swept.size() == 1);
	return sweep;
}

} // namespace

// NAMED DEVICE FIRST, AND THAT IS FOR THE SHARDS: ctest splits this suite by index in NAME order (tests/CMakeLists.txt),
// so the three slow parts are named to sort apart — "Drawn:" first, "Resonate:" and "SKLITTER:" near the end, the
// catch-all among the "EVERY ..." cases — and land in different shards. Named "(Drawn)" etc. they sorted together.
TEST_CASE("Drawn: every declared control changes the sound in SOME arrangement") {
	checkHonesty(honestySweepOf("Drawn"), 120, 700);
}

TEST_CASE("Resonate: every declared control changes the sound in SOME arrangement") {
	checkHonesty(honestySweepOf("Resonate"), 50, 380);
}

TEST_CASE("SKLITTER: every declared control changes the sound in SOME arrangement") {
	checkHonesty(honestySweepOf("SKLITTER"), 30, 175);
}

TEST_CASE("EVERY declared control of EVERY other device changes the sound in SOME arrangement") {
	// THE CATCH-ALL BY CONSTRUCTION: every audio device not swept alone above, so a new device lands here unasked.
	const HonestySweep sweep = honestySweep([](const device::Descriptor& d) {
		for (const std::string_view alone : kHonestySweptAlone) {
			if (deviceName(d) == alone) {
				return false;
			}
		}
		return true;
	});
	// AND THE PARTS TOGETHER ARE THE WHOLE: every audio device is in exactly one of the four.
	CHECK(sweep.swept.size() + std::size(kHonestySweptAlone) == audioDevices().size());
	// EVERY ACCOUNTED-FOR ENTRY STILL NAMES A DEVICE. Unsplit, a stale entry failed `dead == expected`; split, each part
	// keeps only its own devices' entries, so an entry naming no device at all would be dropped by every part.
	for (const std::string& e : honestyExpected()) {
		CAPTURE(e);
		bool named = false;
		for (const device::Descriptor* d : audioDevices()) {
			named = named || e.rfind(deviceName(*d) + " :: ", 0) == 0;
		}
		CHECK(named);
	}
	checkHonesty(sweep, 420, 3200);
}



TEST_CASE("OWLTT's gain smoothing is a time constant in seconds, not the host's block length") {
	// **THE BACKLOG'S DECISION OF 2026-08-28**, landed with the seam it needs. The claim is about a
	// NUMBER and cannot be asked of a render: while OWLTT's peak detector is still block-windowed, two
	// renders at different block sizes differ whatever the smoother does, so a difference check would
	// pass for the wrong reason forever.
	//
	// WHAT WAS WRONG: `smoothedGain_` was ramped linearly to its target across exactly `frames`
	// samples, so the smoothing time WAS the chunk length — and `Synth::render` cuts at
	// `min(n - done, 32, ...)` with a CLAP host cutting again at every event. Measured through the
	// plugin, one four-event script at host blocks 256/64/100/441: **0.0619171 worst against a 4.75021
	// peak**, where the other four devices are bit-identical.
	const device::Descriptor* desc = deviceForTag(fx::owltt::kTraits.tag);
	REQUIRE(desc != nullptr);

	SUBCASE("the coefficient is derived from the sample rate and from nothing else") {
		// AN INDEPENDENT RESTATEMENT of the one-pole, not a second call into `prepare`. A time constant
		// in seconds means the coefficient must HALVE its per-sample effect when the rate doubles, and
		// asserting it at two rates is what says "seconds" rather than "samples".
		for (const double rate : { 48000.0, 96000.0 }) {
			CAPTURE(rate);
			const DeviceRow row = enabledRowFor(*desc);
			Raw dev(*desc, rate, row);
			const auto* owltt = static_cast<const fx::owltt::Owltt*>(dev.mem);
			const double tau = fx::owltt::Owltt::kGainSmoothingSeconds * rate;
			const auto want = static_cast<float>(1.0 - std::exp(-1.0 / tau));
			CHECK(owltt->gainSmoothAlphaForTest() == doctest::Approx(want).epsilon(1e-6));
		}
	}

	SUBCASE("and a short call leaves the gain PART WAY, which is what the end-of-call snap destroyed") {
		// **THE SNAP IS THE HALF THAT ALMOST SURVIVED THE FIX.** `owltt.cpp` used to end every call with
		// "snap smoothed gains to target after the ramp completes" — correct for a ramp that ARRIVES,
		// and fatal to a one-pole: the gain would land exactly on its target at every call boundary
		// however the smoother was written, so the time constant would still have been the block. It
		// was found by measuring the fix and finding it had barely moved the number.
		//
		// SO THE ASSERTION IS THAT THE GAIN HAS **NOT** ARRIVED after one short call. With
		// `kGainSmoothingSeconds` at 256/3 samples, a 32-sample call closes
		// `1 - (1 - a)^32` of the distance — about 31% — so a gain sitting exactly on its target after
		// one such call is the snap, back.
		const DeviceRow row = variedRowFor(*desc); // compressing hard, so the target is far from 1
		Raw dev(*desc, 48000.0, row);
		const auto* owltt = static_cast<const fx::owltt::Owltt*>(dev.mem);

		constexpr int kShort = 32;
		std::vector<float> l(kShort, 0.0f);
		std::vector<float> r(kShort, 0.0f);
		const std::vector<float> probe = probeSignal(kShort, 0.9f, 0x5eed0001u);
		std::copy(probe.begin(), probe.end(), l.begin());
		std::copy(probe.begin(), probe.end(), r.begin());
		dev.process(l.data(), r.data(), kShort);

		// **THE VALUE, RESTATED INDEPENDENTLY.** The gain started at 1.0, the slice was heading for
		// `lastTargetBandGainForTest`, and a one-pole run for `kShort` samples closes exactly
		// `1 - (1 - alpha)^kShort` of that distance — about 31% at this constant. Written out here from
		// the algebra rather than compared against anything the device computed, which is what makes it
		// a value rather than an observation.
		//
		// **AND IT IS WHAT SEES THE END-OF-CALL SNAP.** The first draft asserted only that a second
		// call moved the gain again — which is true WITH the snap as well, because the detector's
		// target drifts between calls, so a snapped gain also differs from where it was. Two readings
		// that agree everywhere the case looked. The prediction below disagrees with the snap by the
		// whole remaining 69% of the distance.
		const double alpha = static_cast<double>(owltt->gainSmoothAlphaForTest());
		const double closed = 1.0 - std::pow(1.0 - alpha, kShort);
		CAPTURE(closed);
		CHECK(closed == doctest::Approx(0.312).epsilon(0.02));

		int moved = 0;
		for (size_t b = 0; b < 3; ++b) {
			const double target = static_cast<double>(owltt->lastTargetBandGainForTest(b));
			const double want = 1.0 + (target - 1.0) * closed;
			const double got = static_cast<double>(owltt->smoothedGainForTest(b));
			CAPTURE(b);
			CAPTURE(target);
			CAPTURE(want);
			CAPTURE(got);
			CHECK(got == doctest::Approx(want).epsilon(1e-4));
			// NON-VACUITY, PER BAND: the target has to be somewhere OTHER than where the gain started,
			// or "it closed 31% of the distance" is a statement about a distance of zero and both
			// readings agree by arithmetic.
			if (std::fabs(target - 1.0) > 0.01) {
				++moved;
			}
		}
		CHECK(moved == 3);
	}
}

//==================================================================================================
// CHAIN ACCESS SITES (backlog NR) — every effect's furniture, as decided, and Pitch's Mix moved to the slot
//==================================================================================================

TEST_CASE("NR: every device declares the furniture and the site the decisions give it (D1, D2, D3, D5)") {
	const auto findTag = [](const char* tag) -> const device::Descriptor* {
		for (const device::Descriptor& d : kDevices) {
			if (device::sameTag(d.traits.tag, tag)) {
				return &d;
			}
		}
		return nullptr;
	};
	// D1: THE SLOT BLENDS PITCH'S MIX — the one plain crossfade the DSP reading left on the list.
	const device::Descriptor* pitch = findTag(fx::pitch::kTraits.tag);
	REQUIRE(pitch != nullptr);
	CHECK(pitch->traits.mixControl == static_cast<int>(fx::pitch::Param::Mix));
	CHECK_FALSE(pitch->traits.mixInDevice);
	// D2: A MIX THE DEVICE MAKES ITSELF names its control and is never blended by the slot.
	for (const device::Descriptor& d : kDevices) {
		if (d.traits.mixControl < 0) {
			continue;
		}
		INFO(d.traits.name);
		CHECK(static_cast<size_t>(d.traits.mixControl) < d.params.size());
		CHECK(d.params[static_cast<size_t>(d.traits.mixControl)].unit == device::Unit::Percent);
		// THE SEVEN CUSTOM-CARD DEVICES THAT GOT THE FURNITURE (Clip and Drawn 2026-10-28) are
		// slot-blended too: a plain crossfade each.
		const bool slotBlended = &d == pitch || device::sameTag(d.traits.tag, fx::eq::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::disperser::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::filter::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::waypoint::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::busutil::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::clip::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::drawn::kTraits.tag)
		                         || device::sameTag(d.traits.tag, fx::harm::kTraits.tag);
		if (!slotBlended) {
			CHECK(d.traits.mixInDevice);
		}
		CHECK(d.traits.mixInDevice == !slotBlended);
	}
	// D3, LIFTED FOR THE RACK AND THE CHAIN: they offer PRE and POST, their furniture their own Mix
	// and Gain, mixed in the device; a container whose lanes ARE its sound (SKLITTER, Resonate) still offers none, and
	// neither does a listener. D5: A GENERATOR (Noise) OFFERS POST ONLY. Harm offers PRE too.
	for (const device::Descriptor& d : kDevices) {
		INFO(d.traits.name);
		const bool rackOrChain = device::sameTag(d.traits.tag, rack::parallel::kTraits.tag)
		                         || device::sameTag(d.traits.tag, composite::kTraits.tag);
		if (rackOrChain) {
			CHECK(rack::siteLanesOf(d) == device::Traits::SiteLanes::PreAndPost);
			CHECK(device::exportsSite(d));
			CHECK(device::exportsContainer(d));
			CHECK(d.traits.mixInDevice);
			CHECK(d.traits.wetGainControl >= 0);
			CHECK(d.params[static_cast<size_t>(d.traits.wetGainControl)].unit == device::Unit::Fader);
		}
		else if (device::sameTag(d.traits.tag, fx::delay::kTraits.tag)) {
			// THE DELAY HOLDS A LANE INSIDE ITS LOOP, which is not its sound (delay-love §1.2): PRE and POST stay what they
			// were — POST is how a device runs on every repeat INCLUDING the first, which the lane deliberately is not.
			CHECK(rack::siteLanesOf(d) == device::Traits::SiteLanes::PreAndPost);
		}
		else if (device::sameTag(d.traits.tag, fx::comp::kTraits.tag)) {
			// COMP'S COMPANDER LANE (backlog QG) sits BETWEEN its halves, which is not its sound either: PRE and POST stay.
			CHECK(rack::siteLanesOf(d) == device::Traits::SiteLanes::PreAndPost);
		}
		else if (d.container != nullptr || d.audio == nullptr) {
			CHECK(rack::siteLanesOf(d) == device::Traits::SiteLanes::None);
		}
	}
	for (const char* tag : { fx::harm::kTraits.tag, fx::noise::kTraits.tag }) {
		const device::Descriptor* d = findTag(tag);
		REQUIRE(d != nullptr);
		CHECK(rack::siteLanesOf(*d) == (device::sameTag(tag, fx::harm::kTraits.tag) ? device::Traits::SiteLanes::PreAndPost
		                                                                            : device::Traits::SiteLanes::PostOnly));
		CHECK((d->traits.mixControl >= 0) == device::sameTag(tag, fx::harm::kTraits.tag)); // Harm took the furniture (OW)
	}
}

namespace {

/// ONE PITCH THROUGH AN ENGINE SLOT AND ONE BUILT ALONE, fed the same input in the same 64-sample calls.
struct PitchPair {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	size_t lane = rack::RackEngine::kNoSlot;
	const device::Descriptor* desc = nullptr;
	void* alone = nullptr;

	PitchPair() {
		for (const device::Descriptor& d : kDevices) {
			if (device::sameTag(d.traits.tag, fx::pitch::kTraits.tag)) {
				desc = &d;
			}
		}
		REQUIRE(desc != nullptr);
		engine.prepare(48000.0, 4096);
		lane = engine.addDevice(desc->traits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		const size_t align = std::max<size_t>(desc->audio->stateAlign, alignof(std::max_align_t));
		alone = feathers::test::heapAlignedAlloc(align, ((desc->audio->stateBytes + align - 1) / align) * align);
		REQUIRE(alone != nullptr);
		desc->audio->construct(alone);
		desc->audio->prepare(alone, 48000.0, 4096);
	}
	~PitchPair() {
		desc->audio->destruct(alone);
		feathers::test::heapFree(alone);
	}
	PitchPair(const PitchPair&) = delete;
	PitchPair& operator=(const PitchPair&) = delete;

	/// Both control rows set to `shift` semitones (and everything else at its default), `mix` on the slot's.
	void set(float shift, float mix) {
		const size_t shiftAt = static_cast<size_t>(fx::pitch::Param::Shift);
		const size_t mixAt = static_cast<size_t>(fx::pitch::Param::Mix);
		engine.setField(lane, shiftAt, shift);
		engine.setField(lane, mixAt, mix);
		std::vector<float> row(desc->params.size());
		for (size_t c = 0; c < row.size(); ++c) {
			row[c] = static_cast<float>(desc->params[c].def);
		}
		row[shiftAt] = shift;
		row[mixAt] = mix;
		desc->audio->setFields(alone, row.data(), row.size());
	}
};

} // namespace

TEST_CASE("NR: Pitch at its Mix default is its wet bit for bit through the slot — the value its old in·(1−m) + wet·m "
          "gave — and at Mix 50% the slot's blend of that wet against the input") {
	PitchPair p;
	p.set(7.0f, 1.0f);
	std::vector<float> slotL(64);
	std::vector<float> slotR(64);
	std::vector<float> wetL(64);
	std::vector<float> wetR(64);
	for (int block = 0; block < 40; ++block) {
		for (size_t i = 0; i < slotL.size(); ++i) {
			const float x = 0.5f * std::sin(0.05f * static_cast<float>(block * 64 + static_cast<int>(i)));
			slotL[i] = wetL[i] = x;
			slotR[i] = wetR[i] = -x;
		}
		const std::vector<float> inL = slotL;
		p.engine.processStereo(slotL.data(), slotR.data(), 64, backplane::unknownBlock());
		p.desc->audio->processStereo(p.alone, wetL.data(), wetR.data(), 64);
		for (size_t i = 0; i < slotL.size(); ++i) {
			CHECK(slotL[i] == wetL[i]);
			CHECK(slotR[i] == wetR[i]);
			CHECK(slotL[i] == inL[i] * (1.0f - 1.0f) + wetL[i] * 1.0f);
		}
	}
	// AT 50% THE SLOT BLENDS, and the device still hands back the wet whatever its Mix says.
	p.set(7.0f, 0.5f);
	for (int block = 0; block < 8; ++block) { // past the 5 ms ramp
		for (size_t i = 0; i < slotL.size(); ++i) {
			const float x = 0.5f * std::sin(0.05f * static_cast<float>(4000 + block * 64 + static_cast<int>(i)));
			slotL[i] = wetL[i] = x;
			slotR[i] = wetR[i] = -x;
		}
		const std::vector<float> inL = slotL;
		p.engine.processStereo(slotL.data(), slotR.data(), 64, backplane::unknownBlock());
		p.desc->audio->processStereo(p.alone, wetL.data(), wetR.data(), 64);
		if (block >= 4) {
			for (size_t i = 0; i < slotL.size(); ++i) {
				CHECK(slotL[i] == wetL[i] * 0.5f + inL[i] * 0.5f);
			}
		}
	}
}

//==================================================================================================
// THE FURNITURE ON THE CUSTOM-CARD DEVICES (backlog NR): EQ, Disperser, Filter, Waypoint, Bus Utility
//==================================================================================================

namespace {

/// ONE DEVICE THROUGH AN ENGINE SLOT AND ONE BUILT ALONE, fed the same input in the same 64-sample calls, every control
/// but the ones set at its default. The slot owns the Mix and the wet Gain, so `alone` is the WET, whatever either says.
struct FurniturePair {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	size_t lane = rack::RackEngine::kNoSlot;
	const device::Descriptor* desc = nullptr;
	void* alone = nullptr;
	std::vector<float> row;
	uint32_t noise = 12345u;
	int blocks = 0;

	explicit FurniturePair(const char* tag) {
		for (const device::Descriptor& d : kDevices) {
			if (device::sameTag(d.traits.tag, tag)) {
				desc = &d;
			}
		}
		REQUIRE(desc != nullptr);
		engine.prepare(48000.0, 4096);
		lane = engine.addDevice(desc->traits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		const size_t align = std::max<size_t>(desc->audio->stateAlign, alignof(std::max_align_t));
		alone = feathers::test::heapAlignedAlloc(align, ((desc->audio->stateBytes + align - 1) / align) * align);
		REQUIRE(alone != nullptr);
		desc->audio->construct(alone);
		desc->audio->prepare(alone, 48000.0, 4096);
		row.resize(desc->params.size());
		for (size_t c = 0; c < row.size(); ++c) {
			row[c] = static_cast<float>(desc->params[c].def);
		}
		desc->audio->setFields(alone, row.data(), row.size());
	}
	~FurniturePair() {
		desc->audio->destruct(alone);
		feathers::test::heapFree(alone);
	}
	FurniturePair(const FurniturePair&) = delete;
	FurniturePair& operator=(const FurniturePair&) = delete;

	[[nodiscard]] size_t mixAt() const { return static_cast<size_t>(desc->traits.mixControl); }
	[[nodiscard]] size_t gainAt() const { return static_cast<size_t>(desc->traits.wetGainControl); }

	/// A CONTROL SET ON BOTH: the slot's row, and the lone device's (which ignores the two furniture controls).
	void set(size_t control, float value) {
		engine.setField(lane, control, value);
		row[control] = value;
		desc->audio->setFields(alone, row.data(), row.size());
	}

	/// ONE 64-FRAME BLOCK of broadband input through both. `in` is what entered, `slot` what the engine made of it,
	/// `wet` what the device made of it alone.
	struct Block {
		std::vector<float> inL, inR, slotL, slotR, wetL, wetR;
	};
	Block run() {
		Block b;
		b.inL.resize(64);
		b.inR.resize(64);
		for (size_t i = 0; i < 64; ++i) {
			noise = noise * 1664525u + 1013904223u;
			const float n = static_cast<float>(static_cast<int32_t>(noise)) / 2147483648.0f;
			const float tone = std::sin(0.07f * static_cast<float>(blocks * 64 + static_cast<int>(i)));
			b.inL[i] = 0.25f * n + 0.3f * tone;
			b.inR[i] = -0.2f * n + 0.3f * tone;
		}
		++blocks;
		b.slotL = b.wetL = b.inL;
		b.slotR = b.wetR = b.inR;
		engine.processStereo(b.slotL.data(), b.slotR.data(), 64, backplane::unknownBlock());
		desc->audio->processStereo(alone, b.wetL.data(), b.wetR.data(), 64);
		return b;
	}
	/// Blocks past the 5 ms furniture ramp (240 samples), so a change has landed.
	void settle() {
		for (int i = 0; i < 8; ++i) {
			(void) run();
		}
	}
};

} // namespace

TEST_CASE("NR: the furniture on EQ, Disperser, Filter, Waypoint, Bus Utility and Harm — Mix 100% and wet Gain at unity are the "
          "device bit for bit; Mix 0 is the input to the bit; the wet Gain scales the wet and not the dry") {
	struct Case {
		const char* tag;
		/// ONE CONTROL MOVED OFF ITS DEFAULT so the wet is not the input (the checks below read both).
		size_t control;
		float value;
	};
	const Case cases[] = {
		{ fx::eq::kTraits.tag, fx::eq::bandIndex(2, fx::eq::BandField::Gain), 12.0f },
		{ fx::disperser::kTraits.tag, static_cast<size_t>(fx::disperser::Param::Enabled), 1.0f },
		{ fx::filter::kTraits.tag, static_cast<size_t>(fx::filter::Param::Cutoff), 8.2f },
		{ fx::waypoint::kTraits.tag, static_cast<size_t>(fx::waypoint::Param::Morph), 0.0f },
		{ fx::busutil::kTraits.tag, static_cast<size_t>(fx::busutil::Param::Gain), 0.75f },
		// HIGH-PASS, so the wet is not the input whatever note is held (there is none): the brickwall removes the low end.
		{ fx::harm::kTraits.tag, static_cast<size_t>(fx::harm::Param::Mode), 1.0f },
	};
	for (const Case& c : cases) {
		FurniturePair p(c.tag);
		INFO(p.desc->traits.name);
		// THE DEFAULTS ARE THE FURNITURE'S IDENTITY: 100% and unity, declared so.
		CHECK(p.desc->params[p.mixAt()].def == doctest::Approx(1.0));
		CHECK(p.desc->params[p.gainAt()].def == doctest::Approx(0.5));
		CHECK(p.desc->params[p.gainAt()].unit == device::Unit::Fader);
		p.set(c.control, c.value);

		// (1) THE DEFAULTS: THE SLOT'S OUTPUT IS THE DEVICE'S OWN, BIT FOR BIT — today's sound — and the wet is not the input.
		bool wetDiffers = false;
		for (int block = 0; block < 40; ++block) {
			const FurniturePair::Block b = p.run();
			for (size_t i = 0; i < 64; ++i) {
				CHECK(b.slotL[i] == b.wetL[i]);
				CHECK(b.slotR[i] == b.wetR[i]);
				wetDiffers = wetDiffers || b.wetL[i] != b.inL[i];
			}
		}
		CHECK(wetDiffers);

		// (2) MIX 0: THE INPUT, TO THE BIT.
		p.set(p.mixAt(), 0.0f);
		p.settle();
		for (int block = 0; block < 8; ++block) {
			const FurniturePair::Block b = p.run();
			for (size_t i = 0; i < 64; ++i) {
				CHECK(b.slotL[i] == b.inL[i]);
				CHECK(b.slotR[i] == b.inR[i]);
			}
		}

		// (3) MIX 1 WITH THE WET GAIN UP: ONLY THE WET SCALES. Position 0.75 is (2 x 0.75)^2 = 2.25.
		const float up = rack::siteFaderGain(0.75f);
		CHECK(up == doctest::Approx(2.25f));
		p.set(p.mixAt(), 1.0f);
		p.set(p.gainAt(), 0.75f);
		p.settle();
		for (int block = 0; block < 8; ++block) {
			const FurniturePair::Block b = p.run();
			for (size_t i = 0; i < 64; ++i) {
				CHECK(b.slotL[i] == b.wetL[i] * up + b.inL[i] * 0.0f);
				CHECK(b.slotR[i] == b.wetR[i] * up + b.inR[i] * 0.0f);
			}
		}

		// (4) MIX 50% WITH THE WET GAIN DOWN (position 0.25 is 0.25): THE DRY IS UNTOUCHED, the wet is scaled and halved.
		const float down = rack::siteFaderGain(0.25f);
		CHECK(down == doctest::Approx(0.25f));
		p.set(p.mixAt(), 0.5f);
		p.set(p.gainAt(), 0.25f);
		p.settle();
		for (int block = 0; block < 8; ++block) {
			const FurniturePair::Block b = p.run();
			for (size_t i = 0; i < 64; ++i) {
				CHECK(b.slotL[i] == b.wetL[i] * (0.5f * down) + b.inL[i] * 0.5f);
				CHECK(b.slotR[i] == b.wetR[i] * (0.5f * down) + b.inR[i] * 0.5f);
			}
		}
	}
}

TEST_CASE("NR: CLIP's furniture with Oversample on — the slot's dry is the input late by the declared 6 samples, sample-exact; "
          "Mix 50% does not comb; Mix 100% and unity are the device bit for bit") {
	FurniturePair p(fx::clip::kTraits.tag);
	CHECK(p.desc->params[p.mixAt()].def == doctest::Approx(1.0));
	CHECK(p.desc->params[p.gainAt()].def == doctest::Approx(0.5));
	CHECK(p.desc->params[p.gainAt()].unit == device::Unit::Fader);
	CHECK(p.desc->params[static_cast<size_t>(fx::clip::Param::Oversample)].def == doctest::Approx(1.0)); // 4x, the default
	// THE SLOT REPORTS WHAT THE DEVICE DECLARES, and the declaration is the oversampler's (test_fx_clip measures it).
	CHECK(p.engine.latencySamples(48000.0) == fx::clip::kOversampleLatency);
	CHECK(fx::clip::kOversampleLatency == 6u);
	const size_t late = fx::clip::kOversampleLatency;

	// EVERY SAMPLE THAT WENT IN, so a dry `late` samples back can be read across block edges.
	std::vector<float> histL, histR;
	const auto runKept = [&] {
		FurniturePair::Block b = p.run();
		histL.insert(histL.end(), b.inL.begin(), b.inL.end());
		histR.insert(histR.end(), b.inR.begin(), b.inR.end());
		return b;
	};
	const auto dryAt = [&](const std::vector<float>& h, size_t i) {
		const size_t at = h.size() - 64 + i;
		return at >= late ? h[at - late] : 0.0f;
	};

	// (1) THE DEFAULTS, DRIVEN SO THE CLIPPER WORKS: the slot's output is the device's own, bit for bit.
	p.set(static_cast<size_t>(fx::clip::Param::Drive), 18.0f);
	bool wetDiffers = false;
	for (int block = 0; block < 40; ++block) {
		const FurniturePair::Block b = runKept();
		for (size_t i = 0; i < 64; ++i) {
			CHECK(b.slotL[i] == b.wetL[i]);
			CHECK(b.slotR[i] == b.wetR[i]);
			wetDiffers = wetDiffers || b.wetL[i] != dryAt(histL, i);
		}
	}
	CHECK(wetDiffers);

	// (2) MIX 0: THE INPUT `late` SAMPLES LATE, TO THE BIT — the dry is aligned by the declared count, not by a guess.
	p.set(p.mixAt(), 0.0f);
	for (int i = 0; i < 8; ++i) {
		(void) runKept();
	}
	for (int block = 0; block < 8; ++block) {
		const FurniturePair::Block b = runKept();
		for (size_t i = 0; i < 64; ++i) {
			CHECK(b.slotL[i] == dryAt(histL, i));
			CHECK(b.slotR[i] == dryAt(histR, i));
		}
	}

	// (3) MIX 50%, THE BLEND RESTATED: half the wet plus half the dry `late` samples back, sample-exact.
	p.set(p.mixAt(), 0.5f);
	for (int i = 0; i < 8; ++i) {
		(void) runKept();
	}
	for (int block = 0; block < 8; ++block) {
		const FurniturePair::Block b = runKept();
		for (size_t i = 0; i < 64; ++i) {
			CHECK(b.slotL[i] == b.wetL[i] * 0.5f + dryAt(histL, i) * 0.5f);
			CHECK(b.slotR[i] == b.wetR[i] * 0.5f + dryAt(histR, i) * 0.5f);
		}
	}
}

TEST_CASE("NR: CLIP at Mix 50% with Oversample on does not comb — a quiet tone below the knee comes out at its own level") {
	// BELOW THE KNEE THE CURVE IS EXACTLY THE INPUT, so the wet is the input through the halfbands alone and a comb is
	// the only thing that could move the level. MEASURED: with the dry undelayed (the device before NR) 1 kHz at 50% is
	// -0.61 dB and 5 kHz -10.5 dB; with it 6 samples late, 0.00 and -0.03 dB.
	for (const double hz : { 1000.0, 5000.0 }) {
		CAPTURE(hz);
		FurniturePair p(fx::clip::kTraits.tag);
		p.set(p.mixAt(), 0.5f);
		rack::RackEngine& e = p.engine;
		double in = 0.0, out = 0.0;
		std::vector<float> l(64), r(64);
		for (int block = 0; block < 400; ++block) {
			for (size_t i = 0; i < 64; ++i) {
				l[i] = r[i] = 0.1f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * (block * 64 + static_cast<int>(i)) / 48000.0));
			}
			const std::vector<float> src = l;
			e.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			if (block >= 100) {
				for (size_t i = 0; i < 64; ++i) {
					in += static_cast<double>(src[i]) * src[i];
					out += static_cast<double>(l[i]) * l[i];
				}
			}
		}
		const double db = 10.0 * std::log10(out / in);
		MESSAGE("CLIP Mix 50% at " << hz << " Hz: " << db << " dB");
		CHECK(std::fabs(db) < 0.1);
	}
}

TEST_CASE("NL: the brickwall's tail reaches exact silence in both its homes — HARM in High-pass and the Filter's Brickwall HP") {
	// THE SILENCE SWEEP ABOVE runs every device at `variedRowFor`, which leaves stepped controls at their defaults —
	// so HARM in Notch and the Filter in Lowpass, and the brickwall never runs there. Same burst, same silence, here,
	// with the switch thrown: a brickwall parked on a denormal would keep the plugin awake for ever.
	constexpr int kFrames = 512;
	constexpr int kSilentBlocks = 400; // ~4.3 s at 48 kHz
	std::vector<float> burst(kFrames, 0.0f);
	for (int i = 0; i < kFrames; ++i) {
		burst[static_cast<size_t>(i)] = 0.8f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 0.02 * i));
	}
	struct Home {
		const device::Descriptor* desc;
		size_t field;
		float value;
	};
	const Home homes[] = {
		{ &fx::harm::kDescriptor, static_cast<size_t>(fx::harm::Param::Mode), 1.0f },
		{ &fx::filter::kDescriptor, static_cast<size_t>(fx::filter::Param::Type),
		  static_cast<float>(fx::filter::Type::Brickwall) },
	};
	for (const Home& home : homes) {
		const std::string who = deviceName(*home.desc);
		CAPTURE(who);
		DeviceRow row = variedRowFor(*home.desc);
		row.field[home.field] = home.value;
		Raw dev(*home.desc, 48000.0, row);
		std::vector<float> l(burst), r(burst);
		dev.process(l.data(), r.data(), kFrames);
		float burstPeak = 0.0f;
		for (const float x : l) {
			burstPeak = std::max(burstPeak, std::fabs(x));
		}
		CAPTURE(burstPeak);
		REQUIRE(burstPeak > 1.0e-6f); // the burst (960 Hz) is in the passband and reached the output
		// AND IT WAS THE BRICKWALL THAT RAN — the switch reached the DSP, or this is a sweep of the notch and the lowpass.
		const double corner = home.desc == &fx::harm::kDescriptor ? fx::harm::opsHighpassCornerForTest(dev.mem)
		                                                          : fx::filter::opsBrickwallCornerForTest(dev.mem);
		CAPTURE(corner);
		REQUIRE(corner > 0.0);
		int lastNonZeroBlock = -1;
		for (int b = 0; b < kSilentBlocks; ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			dev.process(l.data(), r.data(), kFrames);
			for (int i = 0; i < kFrames; ++i) {
				if (l[static_cast<size_t>(i)] != 0.0f || r[static_cast<size_t>(i)] != 0.0f) {
					lastNonZeroBlock = b;
					break;
				}
			}
		}
		CAPTURE(lastNonZeroBlock);
		CHECK(lastNonZeroBlock < kSilentBlocks / 2);
		const bool holdsAtEnd = dev.ops->holdsTail != nullptr && dev.ops->holdsTail(dev.mem);
		CHECK_FALSE(holdsAtEnd);
	}
}
