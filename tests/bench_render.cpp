/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE RENDER BENCHMARK — one patch, rendered in blocks, timed per block. NOT A TEST: it asserts nothing
// and ctest never runs it. It exists so a claim about the engine's COST has an instrument behind it
// (CLAUDE.md: "a model is evidence about mechanism, not about magnitude").
//
// WHAT IT BUILDS, through the editor's own road (`ParamBridge`, then `publishEditorLanes`), so every
// patch is one a hand could make: `--strip N` modulators (DAHDSR, MSEG, tracker, DAHDSR), `--lane N`
// devices (filter, harm, owltt, shaper), `--voices V` held notes, and `--routes N` routes generated
// round-robin over every source class — macros, weights, each strip modulator's output — into voice
// parameters and every modulatable device control, so a small N is already varied and a large one
// fills the table. `--routes 0` is the same patch with no modulation edges at all, so the routing
// tier's cost is the difference between two rows. `--stack N` plays N oscillators per note (backlog KE),
// `--voice-count C` sets Voice Count, and `--move 1` walks Pos X every block so the character keeps rebuilding
// — the moving case the voice stack's CPU guard is sized for.
// `--vowel 1` stands in vowel country, where every note bakes a vowel (voice stacking step 4 shares that bake).
// `--stand P` routes the Stack Index into Pos X at P per mille, so each part hears its own stand (step 4).
// `--fresh 1` deals every note Random Fresh stands; `--width 1` turns on terrain stereo with two wide sounds (step 4c).
// `--engine v5|additive` sets Osc Type (v5, the default, leaves every older invocation unchanged); `--drawer 1` sets
// a representative drawer through its real params (Filter LP resonant, Comb, Tilt, Odd/Even on, priced with the
// synth's per-chunk assembly: the additive plan's Task 10); `--drawer 2` also moves its cutoff every block (host
// automation; ±1 octave); `--drawer 3` and `--drawer 4` are 1 and 2 with the filter a HIGHPASS at 40 Hz, which culls no
// partial (the lowpass flushes the low notes' top partials to 0 and the bank skips them, which flatters 1 and 2).
// `--bend 1` moves the pitch wheel every block, so every voice's f0 moves once a block (the drawer's work over the
// timed blocks goes to stderr, which says which factors each case recomputed). `--frames N` sets the host block (default
// 256): at 32 every block is one control block, so `--bend 1` moves f0 every control block. `--note K` puts the first held note at MIDI key K
// (default 48) and `--step S` spaces the rest S semitones apart (default 3), so a low cluster plays many partials and a
// high one few. `--retrigger N` (N >= 2) releases and re-presses every held note every N blocks, a chord note-on burst in
// one block; the burst blocks' mean and max (0 without it) are appended beside the whole run's max, then the mean of the block
// AFTER each burst — the same voices sounding without the note-ons, so the burst's own cost is the difference.
// `--sounds N` (1..8) switches N sounds on, on a circle of radius 0.5 about the ORIGIN, so a `--move 1` build pays for N (`--sounds 1`
// switches the default patch's B off; 0 leaves the patch's two). It is applied AFTER `--width 1`, so it overwrites
// width's zone on B; Width stays on A and B only: `--sounds 8 --width 1` is 8 sounds, terrain stereo on, 2 of them wide.
// At Path 0 a still puck sits ON sound A (the others weigh 0); `--posx P` holds Pos X at P per mille so a still puck
// stands where every sound weighs, and `--weights 1` prints the weights read back after warm-up to stderr.
// `--plot 1` times publishEngineReadings after every block with the spectrum drawer's plot flag off, `--plot 2` with it on, so
// the plot's publish (D2) is the difference between the two rows; the frames published go to stderr.
// bench_render does NOT install the `DenormalGuard` that the plugin's process() wraps every block in, unless `--ftz 1` says to:
// then each timed and warm-up render is wrapped in one, save and restore included, as process() is (rows before 2026-10-11 never were).
// `--release 1` releases every held note at the end of warm-up, so the timed blocks are the release tail and the descent
// through the denormals that follows it — the population FTZ/DAZ is about (backlog "FTZ/DAZ", 2026-10-11).
// `--formant 1` switches the vowel Formant on in every sound (Open, depth 0.8), alone or beside `--drawer`; `--formant 2` also
// moves its Vowel every block, so the vowel factors are recomputed (spectral-ops, 2026-10-12).
// `--partials N` switches the Partials operator on in every sound at count N (spectral-ops, 2026-10-12).
// `--damp 1` switches Damp on in every sound (T60 0.5 s at 1 kHz, tilt 1; spectral-ops, 2026-10-12); `--damp 2` at T60
// 20 s and tilt 0, so no partial dies inside a retriggered note and the row prices the operator's own work.
// `--stretch 1` switches Stretch on in every sound at Amount 0.3 (spectral-ops, 2026-10-12).
// `--shift 1` switches Shift on in every sound at +50 Hz, `--shift 2` at −300 Hz, `--shift 3` at −500 Hz (spectral-ops, 2026-10-12).
// `--verb 1` puts a Featherverb at the head of the lane, so the tail is a reverb's seconds of tiny-but-nonzero audio through the chain.
// `--rack N` puts a Rack of five EMPTY chains at the head of the lane (rack stage 2, §4b: "measure it with bench_render"):
// 1 Parallel, 2 Bands 5 LR4 Before, 3 Bands 5 LR4 Both, 4 Bands 5 LR4 After — so a Bands row less the Parallel row is the
// crossovers' own cost.
// `--earphase 1` routes a Free LFO at Stereo Phase π/2 into Phase at 0.25 (a stereo route: Additive's right ear reads its own
// rotators, backlog OZ); `--earphase 2` is the same route at Stereo Phase 0 (mono), so 1 less 2 is the right ear's cost.
//
// WHAT IT PRINTS: one CSV row — the patch's shape as built (what was actually added and placed, not
// asked for), mean/p50/p90/p99 microseconds per block, the block's share of one core at this rate,
// and a checksum of the rendered output, which is what says two builds did the SAME work. (Kill-lanes
// made modulator-to-modulator pass-through, so the checksum moves a little across that boundary and
// nowhere else.)
//
// **THE SAME FILE COMPILES AGAINST THE PRE-KILL-LANES TREE WITH `BENCH_OLD_API`**, because a benchmark
// against "the old one" is only a benchmark if both sides run the same patch. The old bridge took a
// route as two COORDINATES and the old synth was handed its chains by value; those are the only two
// things the macro switches. CLAUDE.md's benchmarking rule applies: alternate the two binaries, take
// medians, and never trust one run on this host.

#include "fx/busutil/busutil_types.h"
#include "plugin/device_registry.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#ifndef BENCH_OLD_API
#include "plugin/denormal_guard.h"
#endif
#include "fx/owltt/owltt_types.h"
#include "fx/filter/filter_types.h"
#include "fx/harm/harm_types.h"
#ifndef BENCH_OLD_API
#include "fx/featherverb/featherverb_types.h"
#include "rack/parallel_rack.h"
#include "fx/spectral/spectral_types.h"
#endif
#include "fx/shaper/shaper_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/tracker/tracker_types.h"
#include "mod/lfo/lfo_types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#if defined(__GLIBC__)
#include <malloc.h> // mallinfo2: the HEAP line, glibc only
#endif
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kDefaultBlock = 256; ///< frames per host block unless `--frames` says otherwise

struct Options {
	int routes = 16;
	int strip = 4;
	int lane = 4;
	int voices = 8;
	int stack = 1;      ///< oscillators per note (backlog KE); 1 is the synth as it always was
	int voiceCount = 0; ///< Voice Count to set, 0 to leave the default (8)
	int move = 0;       ///< 1: move Pos X every block, so the character (and every part's table) keeps rebuilding
	int stand = 0;      ///< per mille: a Stack Index route into Pos X at this amount, so every part stands apart
	int fresh = 0;      ///< 1: Stack Shape Random Fresh at full depth, so every note stands its parts somewhere new
	int width = 0;      ///< 1: two wide sounds and Width 1 — terrain stereo, and with a stack, a stand per part
	int vowel = 0;      ///< 1: stand in vowel country (zone 640, offset 1.3563, the vox tests' stand), so notes bake vowels
	int additive = 0;   ///< 1: Osc Type Additive (`--engine additive`); 0: V5, the default patch's engine
	int drumKind = 1;   ///< with `--engine drum`: the kind Pitched mode plays (drum::Kind's order; `--drum-kind`)
	double drumBell = 0.0; ///< with `--engine drum`: any value above 0 puts pad A's Bells zone at Knock, both moving bells sounding (`--drum-bell`; since the §14 consolidation the levels are the zone's)
	int earphase = 0;   ///< 1: an LFO at Stereo Phase π/2 onto Phase (stereo-live); 2: the same at Stereo Phase 0 (mono)
	int drawer = 0;     ///< 1: every oscillator gets a representative drawer through the voice's test-only seam; 2: and its cutoff moves every block;
	                    ///< 3, 4: as 1, 2 with the filter a highpass at 40 Hz, which culls no partial
	int bend = 0;       ///< 1: the pitch wheel moves every block (a slow sine, ±half its reach)
	int plot = 0;       ///< 1: publishEngineReadings timed after every block, the drawer's plot flag OFF; 2: the flag ON (D2)
	int formant = 0;    ///< 1: the vowel Formant on every sound, held; 2: and its Vowel moves every block (the vowel group recomputes)
	int partials = 0;   ///< N > 0: the Partials operator on in every sound at count N (8..320); 0: off
	int damp = 0;       ///< 1: Damp on in every sound (T60 0.5 s at 1 kHz, tilt 1); 2: T60 20 s, tilt 0 (no partial dies)
	int stretch = 0;    ///< 1: Stretch on in every sound at Amount 0.3 (B 2.7e-4)
	int shift = 0;      ///< 1: Shift on in every sound at +50 Hz; 2: at −300 Hz (low partials pushed below 0 Hz); 3: at −500 Hz
	int note = 48;      ///< the first held note's key
	int step = 3;       ///< semitones between held notes
	int retrigger = 0;  ///< N > 0: release and re-press every held note every N blocks (a chord note-on burst)
	int sounds = 0;     ///< N > 0: N sounds on, on a circle of radius 0.5 about the origin (0 leaves the patch's)
	int posX = 0;       ///< per mille: Pos X held here when `--move 0` (0 leaves it), so a still puck can stand off A
	int weights = 0;    ///< 1: print the sound weights read back after warm-up to stderr
	int frames = kDefaultBlock; ///< frames per host block (`--frames`): 32 makes every block one control block
	int ftz = 0;        ///< 1: wrap every render in the plugin's `DenormalGuard`, as process() does
	int release = 0;    ///< 1: release every held note at the end of warm-up, so the timed blocks are the tail
	int verb = 0;       ///< 1: a Featherverb at the head of the lane (added after the routes are dealt)
	int rack = 0;       ///< `--rack N`: a five-chain Rack at the head of the lane, 1 Parallel, 2..4 Bands 5 LR4 Before/Both/After
	int spectral = 0;   ///< 1..3: a Spectral (Fast, Balanced, Fine) at the head of the lane, its four Hz operators on
	int spectralPoly = 0; ///< 1: that Spectral runs per voice (its Poly switch), each voice its own STFT and gain delay
	int vocode = 0;     ///< 1: OE's Vocode on in every sound, a white-noise sidechain every block; 2: the same, frozen (the read alone); 3: live, Sibilance 0
	int blocks = 4000;
	int warmup = 400;
	const char* variant = "unnamed";
};

Options parse(int argc, char** argv) {
	Options o;
	for (int i = 1; i + 1 < argc; i += 2) {
		const int value = std::atoi(argv[i + 1]);
		if (std::strcmp(argv[i], "--routes") == 0) {
			o.routes = value;
		}
		else if (std::strcmp(argv[i], "--strip") == 0) {
			o.strip = value;
		}
		else if (std::strcmp(argv[i], "--lane") == 0) {
			o.lane = value;
		}
		else if (std::strcmp(argv[i], "--voices") == 0) {
			o.voices = value;
		}
		else if (std::strcmp(argv[i], "--blocks") == 0) {
			o.blocks = value;
		}
		else if (std::strcmp(argv[i], "--warmup") == 0) {
			o.warmup = value;
		}
		else if (std::strcmp(argv[i], "--variant") == 0) {
			o.variant = argv[i + 1];
		}
		else if (std::strcmp(argv[i], "--stack") == 0) {
			o.stack = value;
		}
		else if (std::strcmp(argv[i], "--voice-count") == 0) {
			o.voiceCount = value;
		}
		else if (std::strcmp(argv[i], "--move") == 0) {
			o.move = value;
		}
		else if (std::strcmp(argv[i], "--vowel") == 0) {
			o.vowel = value;
		}
		else if (std::strcmp(argv[i], "--stand") == 0) {
			o.stand = value;
		}
		else if (std::strcmp(argv[i], "--fresh") == 0) {
			o.fresh = value;
		}
		else if (std::strcmp(argv[i], "--width") == 0) {
			o.width = value;
		}
		else if (std::strcmp(argv[i], "--earphase") == 0) {
			o.earphase = value;
		}
		else if (std::strcmp(argv[i], "--drum-kind") == 0) {
			o.drumKind = static_cast<int>(value);
		}
		else if (std::strcmp(argv[i], "--drum-bell") == 0) {
			o.drumBell = value;
		}
		else if (std::strcmp(argv[i], "--engine") == 0) {
			if (std::strcmp(argv[i + 1], "additive") == 0) {
				o.additive = 1;
			}
			else if (std::strcmp(argv[i + 1], "pluck") == 0) {
				o.additive = 2; // the Karplus-Strong engine (KS design §5)
			}
			else if (std::strcmp(argv[i + 1], "drum") == 0) {
				o.additive = 3; // the drum engine (drum proposal §8), in Pitched mode at --drum-kind
			}
			else if (std::strcmp(argv[i + 1], "v5") != 0) {
				std::fprintf(stderr, "bench_render: --engine takes v5, additive, pluck or drum, not '%s'\n", argv[i + 1]);
				std::exit(2);
			}
		}
		else if (std::strcmp(argv[i], "--drawer") == 0) {
			o.drawer = value;
		}
		else if (std::strcmp(argv[i], "--plot") == 0) {
			o.plot = value;
		}
		else if (std::strcmp(argv[i], "--formant") == 0) {
			o.formant = value;
		}
		else if (std::strcmp(argv[i], "--partials") == 0) {
			o.partials = value;
		}
		else if (std::strcmp(argv[i], "--vocode") == 0) {
			o.vocode = value;
		}
		else if (std::strcmp(argv[i], "--damp") == 0) {
			o.damp = value;
		}
		else if (std::strcmp(argv[i], "--stretch") == 0) {
			o.stretch = value;
		}
		else if (std::strcmp(argv[i], "--shift") == 0) {
			o.shift = value;
		}
		else if (std::strcmp(argv[i], "--bend") == 0) {
			o.bend = value;
		}
		else if (std::strcmp(argv[i], "--note") == 0) {
			o.note = value;
		}
		else if (std::strcmp(argv[i], "--step") == 0) {
			o.step = value;
		}
		else if (std::strcmp(argv[i], "--retrigger") == 0) {
			o.retrigger = value;
		}
		else if (std::strcmp(argv[i], "--sounds") == 0) {
			o.sounds = value;
		}
		else if (std::strcmp(argv[i], "--posx") == 0) {
			o.posX = value;
		}
		else if (std::strcmp(argv[i], "--frames") == 0) {
			o.frames = value;
		}
		else if (std::strcmp(argv[i], "--ftz") == 0) {
			o.ftz = value;
		}
		else if (std::strcmp(argv[i], "--verb") == 0) {
			o.verb = value;
		}
		else if (std::strcmp(argv[i], "--rack") == 0) {
			o.rack = value;
		}
		else if (std::strcmp(argv[i], "--spectral") == 0) {
			o.spectral = value;
		}
		else if (std::strcmp(argv[i], "--spectral-poly") == 0) {
			o.spectralPoly = value;
		}
		else if (std::strcmp(argv[i], "--release") == 0) {
			o.release = value;
		}
		else if (std::strcmp(argv[i], "--weights") == 0) {
			o.weights = value;
		}
	}
#ifndef BENCH_OLD_API
	if (o.sounds < 0 || o.sounds > static_cast<int>(feathers::kMaxSounds)) {
		std::fprintf(stderr, "bench_render: --sounds takes 0 (leave the patch's) to %zu, not %d\n", feathers::kMaxSounds, o.sounds);
		std::exit(2);
	}
	if (o.posX < -1000 || o.posX > 1000) {
		std::fprintf(stderr, "bench_render: --posx takes -1000..1000 per mille, not %d\n", o.posX);
		std::exit(2);
	}
#else
	// The pre-kill-lanes build carries none of the patch options added for the additive plan (they are new-API only
	// below), so it refuses them rather than ignoring them.
	if (o.sounds != 0 || o.posX != 0 || o.weights != 0 || o.additive != 0 || o.drawer != 0 || o.bend != 0 || o.plot != 0 || o.ftz != 0
	    || o.verb != 0 || o.spectral != 0 || o.formant != 0 || o.partials != 0 || o.damp != 0 || o.stretch != 0 || o.shift != 0
	    || o.earphase != 0) {
		std::fprintf(stderr,
		             "bench_render: --sounds/--posx/--weights/--engine additive/--drawer/--bend/--plot/--ftz/--verb/--spectral/--formant/--partials/--damp/--stretch/--shift/--earphase need the current API\n");
		std::exit(2);
	}
#endif
	if (o.release != 0 && o.retrigger != 0) {
		std::fprintf(stderr, "bench_render: --release 1 measures the tail; --retrigger would press the notes again\n");
		std::exit(2);
	}
	if (o.retrigger == 1 || o.retrigger < 0) {
		// Every block a burst leaves no block after one to compare against (after_mean would read 0).
		std::fprintf(stderr, "bench_render: --retrigger takes 0 (off) or N >= 2, not %d\n", o.retrigger);
		std::exit(2);
	}
	if (o.frames < 1 || o.frames > 4096) {
		std::fprintf(stderr, "bench_render: --frames takes 1..4096, not %d\n", o.frames);
		std::exit(2);
	}
	return o;
}

/// A ROUTE'S END, IN THE ONE VOCABULARY BOTH TREES CAN BE HANDED: which strip slot or lane position,
/// which output or control, or which host end. Translated to the bridge's own spelling below.
struct End {
	enum Kind { Macro, Weight, Param, Strip, Lane } kind;
	size_t index;  ///< macro index | weight slot | strip slot | lane position; unused for Param
	size_t member; ///< output index (a source) or control index (a destination); unused for host ends
	feathers::ParamId param = static_cast<feathers::ParamId>(0);
};

struct Wanted {
	End from;
	End to;
	float amount;
};

const char* const kStripTags[] = { feathers::mod::dahdsr::kTraits.tag, feathers::mod::mseg::kTraits.tag,
	                                feathers::mod::tracker::kTraits.tag, feathers::mod::dahdsr::kTraits.tag };
const char* const kLaneTags[] = { feathers::fx::filter::kTraits.tag, feathers::fx::harm::kTraits.tag,
	                               feathers::fx::owltt::kTraits.tag, feathers::fx::shaper::kTraits.tag,
	                               feathers::fx::busutil::kTraits.tag }; // --lane 5 adds Bus Utility (modulation smoothing)
const feathers::ParamId kVoiceParams[] = { feathers::ParamId::Path,      feathers::ParamId::PosX,
	                                        feathers::ParamId::PosY,      feathers::ParamId::VoiceLevel,
	                                        feathers::ParamId::GlideTime, feathers::ParamId::FineTune,
	                                        feathers::ParamId::Weather,   feathers::ParamId::Width };

/// EVERY ROUTE THE PATCH COULD CARRY, IN AN ORDER THAT VARIES EARLY: sources round-robin (macro, weight,
/// strip modulator, ...), each taking the next destination from a list that alternates voice parameters
/// with device controls. A weight aimed at the puck is in the list and the bridge refuses it — the
/// refusal is the product's, and `routes_placed` reports what actually landed.
std::vector<Wanted> candidates(size_t strip, size_t lane) {
	std::vector<End> sources;
	for (size_t m = 0; m < feathers::kMacroCount; ++m) {
		sources.push_back({ End::Macro, m, 0 });
	}
	for (size_t w = 0; w < std::min<size_t>(feathers::kWeightSourceCount, 4); ++w) {
		sources.push_back({ End::Weight, w, 0 });
	}
	for (size_t s = 0; s < strip; ++s) {
		sources.push_back({ End::Strip, s, 0 });
	}
	std::vector<End> destinations;
	for (const feathers::ParamId id : kVoiceParams) {
		destinations.push_back({ End::Param, 0, 0, id });
	}
	std::vector<End> controls;
	for (size_t p = 0; p < lane; ++p) {
		const feathers::device::Descriptor* d = feathers::deviceForTag(kLaneTags[p]);
		for (size_t n = 0; d != nullptr && n < d->modulatableCount(); ++n) {
			controls.push_back({ End::Lane, p, d->controlOfNthModulatable(n) });
		}
	}
	// interleave: one voice parameter, then two device controls, so both classes appear from the start
	std::vector<End> mixed;
	size_t pi = 0;
	size_t ci = 0;
	while (pi < destinations.size() || ci < controls.size()) {
		if (pi < destinations.size()) {
			mixed.push_back(destinations[pi++]);
		}
		for (int k = 0; k < 2 && ci < controls.size(); ++k) {
			mixed.push_back(controls[ci++]);
		}
	}
	// EVERY (source, destination) PAIR, each destination taken once per round with the source rotated by
	// the round, so the first dozen routes already span every source class and every destination class,
	// and the full list is the whole table the patch could carry.
	std::vector<Wanted> out;
	const float amounts[] = { 0.5f, -0.4f, 0.7f, 0.25f, -0.6f, 0.35f, 0.9f, -0.2f };
	for (size_t round = 0; !sources.empty() && round < sources.size(); ++round) {
		for (size_t di = 0; di < mixed.size(); ++di) {
			const End& s = sources[(di + round) % sources.size()];
			out.push_back({ s, mixed[di], amounts[out.size() % 8] });
		}
	}
	return out;
}

#ifdef BENCH_OLD_API
// THE PRE-KILL-LANES BRIDGE SPEAKS COORDINATES: a source is an index into one flat table with the
// device half laid out at a stride per slot, and a destination likewise, decoded through `DeviceSlots`.
size_t sourceOf(const feathers::ParamBridge&, End e) {
	switch (e.kind) {
	case End::Macro:
		return e.index;
	case End::Weight:
		return feathers::weightSourceOf(e.index);
	case End::Strip:
		return feathers::deviceSourceOf(feathers::deviceSlotOfRack(e.index), e.member);
	default:
		return feathers::kModSourceCount;
	}
}
size_t destinationOf(const feathers::ParamBridge& bridge, End e) {
	switch (e.kind) {
	case End::Param: {
		const int index = feathers::modDestinationIndexOf(e.param);
		return index < 0 ? feathers::kModDestinationCount : static_cast<size_t>(index);
	}
	case End::Lane:
		return feathers::deviceDestinationOfControl(bridge.deviceSlots(), feathers::deviceSlotOfFx(e.index), e.member);
	default:
		return feathers::kModDestinationCount;
	}
}
// The old bridge has no per-pair read-back by endpoint, so `placed` counts the pairs it was handed in
// range; it refuses the same weight-to-puck pairs the new one does, so the two builds carry the same set.
size_t setRoutes(feathers::ParamBridge& bridge, const std::vector<Wanted>& wanted, size_t count) {
	size_t placed = 0;
	for (size_t i = 0; i < count && i < wanted.size(); ++i) {
		const size_t s = sourceOf(bridge, wanted[i].from);
		const size_t d = destinationOf(bridge, wanted[i].to);
		if (s >= feathers::kModSourceCount || d >= feathers::kModDestinationCount) {
			continue;
		}
		const bool weightToPuck = feathers::isWeightSource(s) && feathers::isPuckDestination(d);
		bridge.setRouteAmount(s, d, wanted[i].amount);
		placed += weightToPuck ? 0 : 1;
	}
	return placed;
}
void handOff(feathers::ParamBridge& bridge, feathers::ParamSet& params, feathers::Synth& synth) {
	(void)synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);
}
#else
// THE BRIDGE SPEAKS ENDPOINTS: a device by the id it wears, an output or control by the device's own index.
feathers::backplane::Source sourceOf(const feathers::ParamBridge& bridge, End e) {
	switch (e.kind) {
	case End::Macro:
		return feathers::backplane::Source::core(e.index);
	case End::Weight:
		return feathers::backplane::Source::core(feathers::weightSourceOf(e.index));
	case End::Strip:
		return feathers::backplane::Source::output(bridge.rackDeviceAt(e.index), e.member);
	default:
		return feathers::backplane::Source::nobody();
	}
}
feathers::backplane::Destination destinationOf(const feathers::ParamBridge& bridge, End e) {
	switch (e.kind) {
	case End::Param: {
		const int index = feathers::modDestinationIndexOf(e.param);
		return index < 0 ? feathers::backplane::Destination::nobody()
		                 : feathers::backplane::Destination::core(static_cast<size_t>(index));
	}
	case End::Lane:
		return feathers::backplane::Destination::control(bridge.fxDeviceAt(e.index), e.member);
	default:
		return feathers::backplane::Destination::nobody();
	}
}
size_t setRoutes(feathers::ParamBridge& bridge, const std::vector<Wanted>& wanted, size_t count) {
	size_t placed = 0;
	for (size_t i = 0; i < count && i < wanted.size(); ++i) {
		const feathers::backplane::Source s = sourceOf(bridge, wanted[i].from);
		const feathers::backplane::Destination d = destinationOf(bridge, wanted[i].to);
		bridge.setRouteAmount(s, d, wanted[i].amount);
		placed += bridge.routeAmount(s, d) != 0.0f ? 1 : 0;
	}
	return placed;
}
void handOff(feathers::ParamBridge& bridge, feathers::ParamSet& params, feathers::Synth& synth) {
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	feathers::publishEditorLanes(bridge, params, synth);
}
#endif

double percentile(const std::vector<double>& sorted, double p) {
	if (sorted.empty()) {
		return 0.0;
	}
	const size_t at = std::min(sorted.size() - 1, static_cast<size_t>(p * static_cast<double>(sorted.size() - 1)));
	return sorted[at];
}

} // namespace

int main(int argc, char** argv) {
	const Options o = parse(argc, argv);
	const int blockFrames = o.frames;
#if defined(__GLIBC__)
	// THE HEAP THIS RUN'S INSTANCE HOLDS (the device field cap's measure): bytes in
	// use before anything is built, read again at the end, printed as a `HEAP` line on stderr with the inline sizes.
	const size_t heapAtStart = mallinfo2().uordblks;
#endif

	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(kSampleRate, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	params.setBase(static_cast<clap_id>(feathers::ParamId::AOffset), 0.0);
	if (o.vowel != 0) {
		// VOWEL COUNTRY (`--vowel 1`): every note bakes a vowel, which is what step 4 of voice stacking shares.
		params.setBase(static_cast<clap_id>(feathers::ParamId::AZone), 640.0);
		params.setBase(static_cast<clap_id>(feathers::ParamId::AOffset), 1.3563);
	}
	params.setBase(static_cast<clap_id>(feathers::ParamId::Path), 0.0);
	// A FIXED LEVEL: the default patch's Level base is zero, carried by the strip's envelope through a
	// route only the bridge makes (backlog EF); a bare Synth measures silence without this.
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0);
	for (size_t m = 0; m < feathers::kMacroCount; ++m) {
		params.setBase(static_cast<clap_id>(feathers::macroParam(m)), 0.35 + 0.1 * static_cast<double>(m));
	}

	// THE PATCH, THROUGH THE EDITOR'S ROAD. The default bridge holds one envelope and one filter, so the
	// first of each list is already there.
	feathers::ParamBridge bridge;
	size_t stripHeld = 1;
	for (int s = 1; s < o.strip && s < 4; ++s) {
		stripHeld += bridge.addRackDevice(kStripTags[s]) < feathers::kMaxRackSlots ? 1 : 0;
	}
	size_t laneHeld = 1;
	for (int p = 1; p < o.lane && p < static_cast<int>(std::size(kLaneTags)); ++p) {
		laneHeld += bridge.addFxDevice(kLaneTags[p]) < feathers::kMaxFxSlots ? 1 : 0;
	}
	const std::vector<Wanted> wanted = candidates(stripHeld, laneHeld);
	const size_t placed = o.routes > 0 ? setRoutes(bridge, wanted, static_cast<size_t>(o.routes)) : 0;
#ifndef BENCH_OLD_API
	if (o.verb != 0) {
		// A REVERB AT THE HEAD OF THE LANE (`--verb 1`), added after the routes are dealt so they are the same patch's: with `--release 1`
		// its tail feeds every device after it tiny-but-nonzero audio for seconds, the case backlog "FTZ/DAZ" named to watch.
		const size_t at = bridge.addFxDevice(feathers::fx::featherverb::kTraits.tag);
		if (at >= feathers::kMaxFxSlots || !bridge.moveFxDevice(at, 0)) {
			std::fprintf(stderr, "bench_render: --verb 1 could not put a Featherverb at the head of the lane\n");
			return 2;
		}
	}
	if (o.spectral != 0) {
		// A SPECTRAL AT THE HEAD OF THE LANE (`--spectral 1..3`, the Resolution stop + 1), Filter, Comb, the vowel and Tilt on, so
		// every frame is computed: the bus device's cost per stop.
		namespace sp = feathers::fx::spectral;
		const size_t at = bridge.addFxDevice(sp::kTraits.tag);
		if (at >= feathers::kMaxFxSlots || !bridge.moveFxDevice(at, 0)) {
			std::fprintf(stderr, "bench_render: --spectral could not put a Spectral at the head of the lane\n");
			return 2;
		}
		const feathers::DeviceInstanceId id = bridge.fxDeviceAt(0);
		bridge.setDeviceControl(id, static_cast<size_t>(sp::Param::Resolution), static_cast<float>(std::clamp(o.spectral, 1, 3) - 1));
		for (const sp::Param p : { sp::Param::FilterOn, sp::Param::CombOn, sp::Param::VowelOn, sp::Param::TiltOn }) {
			bridge.setDeviceControl(id, static_cast<size_t>(p), 1.0f);
		}
		bridge.setDeviceControl(id, static_cast<size_t>(sp::Param::TiltAmount), 3.0f);
		bridge.setDeviceControl(id, static_cast<size_t>(sp::Param::Poly), o.spectralPoly != 0 ? 1.0f : 0.0f);
	}
#endif
#ifndef BENCH_OLD_API
	// STANDS (`--stand P`, voice stacking step 4): the Stack Index into Pos X at P per mille, so each part stands apart.
	if (o.stand != 0) {
		bridge.setRouteAmount(feathers::backplane::Source::core(feathers::kStackIndexSource),
		                      feathers::backplane::Destination::core(static_cast<size_t>(
		                          feathers::modDestinationIndexOf(feathers::ParamId::PosX))),
		                      static_cast<float>(o.stand) / 1000.0f);
	}
	if (o.earphase != 0) {
		// AN LFO ONTO PHASE (`--earphase 1|2`, backlog OZ): Free, routed into Phase at 0.25; at 1 with Stereo Phase π/2, so
		// the route is stereo-live and Additive's right ear reads its own rotators, at 2 with Stereo Phase 0 (the same route,
		// mono), so 1 less 2 is the right ear's own cost.
		namespace lfo = feathers::mod::lfo;
		const size_t at = bridge.addRackDevice(lfo::kTraits.tag);
		if (at >= feathers::kMaxRackSlots) {
			std::fprintf(stderr, "bench_render: --earphase could not add an LFO to the strip\n");
			return 2;
		}
		const feathers::DeviceInstanceId id = bridge.rackDeviceAt(at);
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::Mode), static_cast<float>(static_cast<int>(lfo::Mode::Free)));
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::StereoPhase), o.earphase == 1 ? 1.5707964f : 0.0f);
		bridge.setRouteAmount(feathers::backplane::Source::output(id, 0),
		                      feathers::backplane::Destination::core(static_cast<size_t>(
		                          feathers::modDestinationIndexOf(feathers::ParamId::OscPhase))),
		                      0.25f);
	}
	if (o.rack != 0) {
		namespace pr = feathers::rack::parallel;
		const size_t at = bridge.addFxDevice(pr::kTraits.tag);
		if (at >= feathers::kMaxFxSlots || !bridge.moveFxDevice(at, 0)) {
			std::fprintf(stderr, "bench_render: --rack could not put a Rack at the head of the lane\n");
			return 2;
		}
		const feathers::DeviceInstanceId id = bridge.fxDeviceAt(0);
		if (o.rack >= 2) {
			bridge.setDeviceControl(id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(pr::Mode::Bands));
			bridge.setDeviceControl(id, static_cast<size_t>(pr::Param::BandCount), 2.0f); // 5
			const float filtering = o.rack == 3 ? 2.0f : (o.rack == 4 ? 1.0f : 0.0f);
			bridge.setDeviceControl(id, static_cast<size_t>(pr::Param::Filtering), filtering);
		}
	}
#endif
	handOff(bridge, params, synth);
#ifndef BENCH_OLD_API
	if (o.rack != 0) {
		// FIVE CHAINS, added to the running instance the bridge filed (the window's "+ Chain" road).
		auto* rack = feathers::rack::parallel::rackOf(bridge.fxBodies().surfaceOf(bridge.fxDeviceAt(0)));
		if (rack == nullptr) {
			std::fprintf(stderr, "bench_render: --rack found no running Rack to add chains to\n");
			return 2;
		}
		while (rack->chainCount() < 5) {
			if (rack->addChain() == SIZE_MAX) {
				std::fprintf(stderr, "bench_render: --rack could not add a chain\n");
				return 2;
			}
		}
	}
#endif

#ifndef BENCH_OLD_API
	// THE STACK (backlog KE): `--stack N` oscillators per note; the guard then decides how many of `--voices` sound.
	params.setBase(static_cast<clap_id>(feathers::ParamId::StackCount), static_cast<double>(o.stack));
	params.setBase(static_cast<clap_id>(feathers::ParamId::StackDetune), 20.0);
	// `--fresh 1` (step 4c): every note deals its parts their own stands. `--width 1`: two wide sounds, terrain stereo.
	if (o.fresh != 0) {
		params.setBase(static_cast<clap_id>(feathers::ParamId::StackShape), static_cast<double>(feathers::StackShape::RandomFresh));
		params.setBase(static_cast<clap_id>(feathers::ParamId::StackShapeAmount), 1.0);
	}
	if (o.width != 0) {
		params.setBase(static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Enable)), 1.0);
		params.setBase(static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Zone)), 600.0);
		params.setBase(static_cast<clap_id>(feathers::soundParam(0, feathers::SoundField::Width)), 1.0);
		params.setBase(static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Width)), 1.0);
		params.setBase(static_cast<clap_id>(feathers::ParamId::Width), 1.0);
	}
#endif
	if (o.voiceCount > 0) {
		params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceCount), static_cast<double>(o.voiceCount));
	}
#ifndef BENCH_OLD_API
	// A CONSTELLATION (`--sounds N`): N sounds on, placed on a circle of radius 0.5 centred on the ORIGIN, and a build
	// pays for all N. Where the puck stands decides who weighs: at Path 0 with Pos X still it sits ON sound A (weight 1,
	// every other 0, read through `Synth::soundWeights()`); moving, or held off A with `--posx`, every sound weighs. The values ASKED are zone 97·i + 13 and offset −6 + 2.1·i; the Offset params range [0, 2]
	// and `setBase` clamps, so what LANDS is zones 13, 110, 207, 304, 401, 498, 595, 692 (distinct) at offsets
	// 0, 0, 0, 0.3, 2, 2, 2, 2. Not `bench_partials_build`'s constellation, which sets its offsets on the Character
	// directly. Kept as is because the design's measured rows (additive design §5.1) were taken with it. `--sounds 1`
	// leaves A alone and switches the default patch's B off; slots past N keep their defaults (off beyond B).
	if (o.sounds == 1) {
		params.setBase(static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Enable)), 0.0);
	}
	for (int s = 0; s < o.sounds; ++s) {
		const size_t slot = static_cast<size_t>(s);
		const double angle = 6.283185307179586 * static_cast<double>(s) / static_cast<double>(o.sounds);
		params.setBase(static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::Enable)), 1.0);
		params.setBase(static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::Zone)), 97.0 * s + 13.0);
		params.setBase(static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::Offset)), -6.0 + 2.1 * s);
		params.setBase(static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::PlaceX)), 0.5 * std::cos(angle));
		params.setBase(static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::PlaceY)), 0.5 * std::sin(angle));
	}
	// THE ENGINE (`--engine`, additive plan Task 10): V5 is the default patch's, so `v5` sets what was already there.
	params.setBase(static_cast<clap_id>(feathers::ParamId::OscType),
	               static_cast<double>(o.additive == 3   ? feathers::OscType::Drum
	                                   : o.additive == 2 ? feathers::OscType::Pluck
	                                   : o.additive != 0 ? feathers::OscType::Additive
	                                                     : feathers::OscType::V5));
	// ON PLUCK THE STRINGS RING (Decay at its top): a held note's string otherwise decays to exact silence and goes idle within
	// seconds, and 4000 blocks of idle strings would price nothing (KS design §5). The ringing string is the cost to measure.
	if (o.additive == 2) {
		params.setBase(static_cast<clap_id>(feathers::ParamId::KsDecay), feathers::findParam(static_cast<clap_id>(feathers::ParamId::KsDecay))->max);
	}
	// ON DRUM THE HITS RING TOO (drum proposal §8): Pitched mode at --drum-kind, every key a hit of that kind, Decay at its top and
	// Length lifted to full (×4: 40 s), so a held hit is still sounding at the end of the run rather than exactly silent.
	if (o.additive == 3) {
		params.setBase(static_cast<clap_id>(feathers::ParamId::DrumMode), 0.0);
		params.setBase(static_cast<clap_id>(feathers::ParamId::DrumPitchedKind), static_cast<double>(o.drumKind));
		const auto decay = feathers::drumParam(0, feathers::DrumControl::Decay);
		params.setBase(static_cast<clap_id>(decay), feathers::findParam(static_cast<clap_id>(decay))->max);
		params.setBase(static_cast<clap_id>(feathers::drumParam(0, feathers::DrumControl::FieldLength)), 1.0);
		// THE BELLS ZONE (§14): Knock (coordinate 0.8, the fifth of five stops), where both bells sound, or Off.
		params.setBase(static_cast<clap_id>(feathers::drumParam(0, feathers::DrumControl::Bells)), o.drumBell > 0.0 ? 0.8 : 0.0);
	}
	// THE DRAWER IS PER SOUND (2026-10-10): the same settings on every sound, so the blend is the D1 drawer exactly (every
	// operator on in every sound: mix 1) and the price is the assembly's over the sounds plus the operators'.
	const auto setDrawer = [&params](feathers::SpectralControl c, double value) {
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			params.setBase(static_cast<clap_id>(feathers::spectralParam(slot, c)), value);
		}
	};
	if (o.drawer != 0) {
		// A REPRESENTATIVE DRAWER: four amplitude operators on (Formant Shift and the drawn curve off, their costs being
		// a re-read and a band lookup), each at a setting that acts, so no identity fast path is taken. THROUGH THE
		// PARAMS, as a patch sets it, so the synth's per-chunk assembly is in the price.
		using P = feathers::SpectralControl;
		// 3 and 4: the same four operators with the filter a HIGHPASS at 40 Hz, which culls nothing (backlog LY review): the
		// lowpass flushes the low notes' top partials to 0 and the bank skips them, which flatters 1 and 2.
		const bool culls = o.drawer <= 2;
		setDrawer(P::FilterOn, 1.0);
		setDrawer(P::FilterMode, static_cast<double>(culls ? feathers::spectral::FilterMode::LowPass
		                                                           : feathers::spectral::FilterMode::HighPass));
		setDrawer(P::FilterCutoff, std::log2(culls ? 2500.0 : 40.0));
		setDrawer(P::FilterResonance, 0.6);
		setDrawer(P::FilterSlope, 0.4);
		setDrawer(P::FilterKeyTrack, 0.5);
		setDrawer(P::CombOn, 1.0);
		setDrawer(P::CombSpacing, 3.0);
		setDrawer(P::CombDepth, 0.7);
		setDrawer(P::CombPhase, 0.2);
		setDrawer(P::TiltOn, 1.0);
		setDrawer(P::TiltAmount, -3.0);
		setDrawer(P::OddEvenOn, 1.0);
		setDrawer(P::OddEvenBalance, -0.4);
	}
	// THE PARTIAL COUNT (`--partials N`, spectral-ops 2026-10-12): on in every sound at N, the CPU knob's price.
	if (o.partials != 0) {
		setDrawer(feathers::SpectralControl::PartialsOn, 1.0);
		setDrawer(feathers::SpectralControl::PartialsCount, static_cast<double>(o.partials));
	}
	// DAMP (`--damp 1`, spectral-ops 2026-10-12): T60 0.5 s at 1 kHz, tilt 1, on every sound.
	if (o.damp != 0) {
		setDrawer(feathers::SpectralControl::DampOn, 1.0);
		// 2: T60 20 s, tilt 0 — nothing dies within a retriggered note, so the row prices the operator's own work.
		setDrawer(feathers::SpectralControl::DampTime, o.damp == 2 ? std::log2(20.0) : -1.0);
		setDrawer(feathers::SpectralControl::DampTilt, o.damp == 2 ? 0.0 : 1.0);
	}
	// OE (`--vocode 1`): Vocode on in every sound at its defaults (Flatten 1, Depth 1); the sidechain is fed in renderBlock.
	if (o.vocode != 0) {
		setDrawer(feathers::SpectralControl::VocodeOn, 1.0);
		// 2 FREEZES after the warm-up (below), on an envelope the noise has made: the read is priced, the analysis is not.
		// 3: live, Sibilance 0 (OE2): the noise layer's share of the price, by difference.
		if (o.vocode == 3) {
			params.setBase(static_cast<clap_id>(feathers::ParamId::AdditiveVocodeSibilance), 0.0);
		}
	}
	// STRETCH (`--stretch 1`, spectral-ops 2026-10-12): Amount 0.3 on every sound.
	if (o.stretch != 0) {
		setDrawer(feathers::SpectralControl::StretchOn, 1.0);
		setDrawer(feathers::SpectralControl::StretchAmount, 0.3);
	}
	// SHIFT (`--shift 1` +50 Hz, `--shift 2` −300 Hz; spectral-ops 2026-10-12) on every sound.
	if (o.shift != 0) {
		setDrawer(feathers::SpectralControl::ShiftOn, 1.0);
		setDrawer(feathers::SpectralControl::ShiftHz, o.shift == 3 ? -500.0 : o.shift == 2 ? -300.0 : 50.0);
	}
	// THE VOWEL (`--formant`, spectral-ops 2026-10-12): Open at depth 0.8 on every sound, alone or beside `--drawer`'s four.
	if (o.formant != 0) {
		setDrawer(feathers::SpectralControl::FormantOn, 1.0);
		setDrawer(feathers::SpectralControl::FormantVowel, 3.0);
		setDrawer(feathers::SpectralControl::FormantDepth, 0.8);
	}
#endif
	std::vector<float> left(static_cast<size_t>(blockFrames)), right(static_cast<size_t>(blockFrames));
	synth.render(left.data(), right.data(), blockFrames); // the sync that adopts the devices
#ifndef BENCH_OLD_API
	// OE (`--vocode 1`): a second of white noise from a fixed xorshift, looped block by block into the sidechain.
	std::vector<float> side(o.vocode != 0 ? 48000u : 0u);
	uint32_t rng = 0x2545f491u;
	for (float& x : side) {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		x = 0.3f * (2.0f * static_cast<float>(rng) / 4294967296.0f - 1.0f);
	}
	size_t sideAt = 0;
#endif
	// ONE HOST BLOCK, inside the plugin's `DenormalGuard` when `--ftz 1` asks (its save and restore are part of the price).
	const auto renderBlock = [&] {
#ifndef BENCH_OLD_API
		if (!side.empty()) {
			if (sideAt + static_cast<size_t>(blockFrames) > side.size()) {
				sideAt = 0;
			}
			synth.setSidechain(side.data() + sideAt, side.data() + sideAt, true);
			sideAt += static_cast<size_t>(blockFrames);
		}
		if (o.ftz != 0) {
			const feathers::DenormalGuard denormals;
			synth.render(left.data(), right.data(), blockFrames);
			return;
		}
#endif
		synth.render(left.data(), right.data(), blockFrames);
	};
	const auto keyOf = [&o](int v) { return static_cast<int16_t>(std::clamp(o.note + v * o.step, 0, 127)); };
	int32_t nextId = 1;
	std::vector<int32_t> heldIds(static_cast<size_t>(std::max(o.voices, 0)));
	for (int v = 0; v < o.voices; ++v) {
		heldIds[static_cast<size_t>(v)] = nextId++;
		synth.noteOn(0, keyOf(v), heldIds[static_cast<size_t>(v)], 0.8);
	}

#ifndef BENCH_OLD_API
	if (o.posX != 0 && o.move == 0) {
		// A STILL PUCK OFF A (`--posx`): held for the whole run, so the weights are fixed and nothing rebuilds.
		params.setBase(static_cast<clap_id>(feathers::ParamId::PosX), static_cast<double>(o.posX) / 1000.0);
	}
#endif

	for (int b = 0; b < o.warmup; ++b) {
		renderBlock();
	}
#ifndef BENCH_OLD_API
	if (o.vocode == 2) {
		params.setBase(static_cast<clap_id>(feathers::ParamId::AdditiveVocodeFreeze), 1.0);
	}
#endif
	if (o.release != 0) {
		// THE TAIL (`--release 1`): every held note let go once, so the timed blocks are the release, the lane's decay and
		// the descent through the denormals. `--retrigger` would press them again, so the two are not meant together.
		for (int v = 0; v < o.voices; ++v) {
			synth.noteOff(0, keyOf(v), heldIds[static_cast<size_t>(v)], 0.0);
		}
	}
#ifndef BENCH_OLD_API
	if (o.weights != 0) {
		// WHO WEIGHS (`--weights 1`): the blend as the synth computed it after warm-up, slot by slot (−: not live).
		const feathers::SoundWeights w = synth.soundWeights();
		std::fprintf(stderr, "weights:");
		for (size_t s = 0; s < feathers::kMaxSounds; ++s) {
			if (w.isLive(s)) {
				std::fprintf(stderr, " %.3f", w.at(s));
			}
			else {
				std::fprintf(stderr, " -");
			}
		}
		std::fprintf(stderr, "\n");
	}
#endif
#ifndef BENCH_OLD_API
	// THE DRAWER'S WORK over the timed blocks (`--drawer`), every oscillator in the pool summed: how often it prepared and
	// recomputed each factor group, which is what says a run measured the case it names (a held note recomputes nothing,
	// `--bend 1` recomputes the frequency groups every control block). Printed to stderr after the row.
	const auto drawerWork = [&synth] {
		feathers::spectral::GainCache<feathers::partials::kMaxPartials>::Work sum;
		for (int slot = 0; slot < feathers::kVoiceSlots; ++slot) {
			for (int part = 0; part < feathers::kMaxStack; ++part) {
				const auto w = synth.voiceStackOscForTest(slot, part).drawerWorkForTest();
				sum.prepares += w.prepares;
				sum.harmonic += w.harmonic;
				sum.filter += w.filter;
				sum.tilt += w.tilt;
				sum.vowel += w.vowel;
			}
		}
		return sum;
	};
	const auto workBefore = drawerWork();
#endif
#ifndef BENCH_OLD_API
	bridge.setPlotWanted(o.plot == 2);
	const uint32_t plotSerialBefore = bridge.plotSerial();
#endif
	std::vector<double> us;
	us.reserve(static_cast<size_t>(o.blocks));
	std::vector<double> burstUs;
	burstUs.reserve(static_cast<size_t>(o.blocks));
	std::vector<double> afterUs; ///< the block after each burst: the same voices sounding, no note-ons — the comparator
	afterUs.reserve(static_cast<size_t>(o.blocks));
	double checksum = 0.0;
	for (int b = 0; b < o.blocks; ++b) {
		if (o.move != 0) {
			// A SLOW CIRCLE, so every block's character differs a little: the table (and every vowel) rebuilds as
			// often as the fade guard lets it — the moving case the stack's guard has to fit (voice-stacking §3.11).
			params.setBase(static_cast<clap_id>(feathers::ParamId::PosX), 0.3 * std::sin(0.05 * static_cast<double>(b)));
		}
#ifndef BENCH_OLD_API
		// THE MODULATED DRAWER (`--drawer 2`, `4`): the cutoff sweeps ±1 octave about 2.5 kHz (40 Hz), a new value every block, as
		// host automation delivers it, through its param.
		if (o.drawer == 2 || o.drawer == 4) {
			setDrawer(feathers::SpectralControl::FilterCutoff,
			          std::log2(o.drawer == 2 ? 2500.0 : 40.0) + std::sin(0.05 * static_cast<double>(b)));
		}
		// THE MOVING VOWEL (`--formant 2`): Vowel sweeps Round..Bright, a new value every block, so the vowel group recomputes.
		if (o.formant == 2) {
			setDrawer(feathers::SpectralControl::FormantVowel, 3.0 + std::sin(0.05 * static_cast<double>(b)));
		}
		// THE MOVING PITCH (`--bend 1`): the wheel, a new value every block, so each voice's f0 moves and every drawer
		// factor that reads a frequency (Filter, Tilt) is recomputed.
		if (o.bend != 0) {
			synth.pitchBend(0, 0.5 * std::sin(0.07 * static_cast<double>(b)));
		}
#endif
		// A CHORD NOTE-ON BURST (`--retrigger N`): every held note released and pressed again in this block, the events
		// timed with the render because a host delivers them inside the same process() call.
		const bool burst = o.retrigger > 0 && (b + 1) % o.retrigger == 0;
		const auto t0 = std::chrono::steady_clock::now();
		if (burst) {
			for (int v = 0; v < o.voices; ++v) {
				synth.noteOff(0, keyOf(v), heldIds[static_cast<size_t>(v)], 0.0);
				heldIds[static_cast<size_t>(v)] = nextId++;
				synth.noteOn(0, keyOf(v), heldIds[static_cast<size_t>(v)], 0.8);
			}
		}
		renderBlock();
#ifndef BENCH_OLD_API
		// THE ENGINE'S READINGS FOR THE EDITOR (`--plot 1|2`), timed with the block as the plugin's process() pays them: the
		// drawer's plot flag off (1) or on (2), so the plot's publish is the difference between the two rows (D2).
		if (o.plot != 0) {
			feathers::publishEngineReadings(bridge, synth);
		}
#endif
		const auto t1 = std::chrono::steady_clock::now();
		us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
		if (burst) {
			burstUs.push_back(us.back());
		}
		else if (o.retrigger > 0 && b > 0 && b % o.retrigger == 0) {
			afterUs.push_back(us.back());
		}
		if (b >= o.blocks - 100) {
			for (int i = 0; i < blockFrames; ++i) {
				checksum += std::fabs(static_cast<double>(left[static_cast<size_t>(i)]));
			}
		}
	}
	std::vector<double> sorted = us;
	std::sort(sorted.begin(), sorted.end());
	double mean = 0.0;
	for (const double x : us) {
		mean += x;
	}
	mean /= static_cast<double>(us.size());
	const double blockUs = 1e6 * static_cast<double>(blockFrames) / kSampleRate;

	double burstMean = 0.0;
	double burstMax = 0.0;
	for (const double x : burstUs) {
		burstMean += x;
		burstMax = std::max(burstMax, x);
	}
	burstMean = burstUs.empty() ? 0.0 : burstMean / static_cast<double>(burstUs.size());
	double afterMean = 0.0;
	for (const double x : afterUs) {
		afterMean += x;
	}
	afterMean = afterUs.empty() ? 0.0 : afterMean / static_cast<double>(afterUs.size());

#ifndef BENCH_OLD_API
	if (o.plot != 0) {
		std::fprintf(stderr, "plot frames published over %d timed blocks: %u (flag %s)\n", o.blocks,
		             bridge.plotSerial() - plotSerialBefore, o.plot == 2 ? "on" : "off");
	}
	if (o.drawer != 0 || o.formant != 0) {
		const auto w = drawerWork();
		std::fprintf(stderr, "drawer work over %d timed blocks (%d frames each), summed over the pool: prepares %u, "
		             "harmonic %u, filter %u, tilt %u, vowel %u\n",
		             o.blocks, blockFrames, w.prepares - workBefore.prepares,
		             w.harmonic - workBefore.harmonic, w.filter - workBefore.filter, w.tilt - workBefore.tilt, w.vowel - workBefore.vowel);
	}
#endif
	// WHAT WAS PLACED, NOT ASKED FOR: the sounds switched on as the params read back after the patch was built.
	int soundsPlaced = -1; // the pre-kill-lanes tree is not asked
#ifndef BENCH_OLD_API
	soundsPlaced = 0;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		soundsPlaced += params.base(static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::Enable))) >= 0.5 ? 1 : 0;
	}
#endif

	// The first sixteen columns are the row as it always was; everything after them is appended at the END.
	std::printf("variant,routes_asked,routes_candidates,routes_placed,strip,lane,voices,stack,move,blocks,mean_us,p50_us,p90_us,p99_us,pct_core,checksum,"
	            "engine,drawer,note,max_us,burst_mean_us,burst_max_us,after_mean_us,sounds_placed,step,retrigger,plot,ftz,release,verb,spectral,rack\n");
	std::printf("%s,%d,%zu,%zu,%zu,%zu,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f,%.3f,%.4f,%s,%d,%d,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", o.variant,
	            o.routes, wanted.size(), placed, stripHeld, laneHeld, o.voices, o.stack, o.move, o.blocks, mean, percentile(sorted, 0.50),
	            percentile(sorted, 0.90), percentile(sorted, 0.99), 100.0 * mean / blockUs, checksum, o.additive == 3 ? "drum" : o.additive == 2 ? "pluck" : o.additive != 0 ? "additive" : "v5",
	            o.drawer, o.note, sorted.empty() ? 0.0 : sorted.back(), burstMean, burstMax, afterMean, soundsPlaced, o.step, o.retrigger, o.plot,
	            o.ftz, o.release, o.verb, o.spectral, o.rack);
#if defined(__GLIBC__)
	std::fprintf(stderr, "HEAP,heap_bytes=%zu,sizeof_synth=%zu,sizeof_bridge=%zu,sizeof_params=%zu\n",
	             mallinfo2().uordblks - heapAtStart, sizeof(feathers::Synth), sizeof(feathers::ParamBridge),
	             sizeof(feathers::ParamSet));
#endif
	return 0;
}
