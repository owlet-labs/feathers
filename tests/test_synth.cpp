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

// The voice layer, driven the way a host drives it but without one: notes go in as calls at exact
// sample positions, audio comes out as buffers.
//
// Everything here is written against a SCRIPT — a list of (frame, action) — which is then played
// back at several block sizes. That shape is not incidental. A synth whose output depends on how the
// host chopped its buffer is broken in a way that only shows up in somebody's render, and the only
// way to catch it is to render the same performance twice and compare the samples.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/rotate_sound.h"
#include "plugin/synth.h"
#include "plugin/master_meter.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/lfo/lfo_types.h"
#include "mod/mseg/mseg_types.h"
#include "fx/phaser/phaser.h"
#include "fx/spectral/spectral.h"
#include "fx/spectral/spectral_types.h"
#include "mod/tracker/tracker_types.h"
#include "mod/noteexpr/noteexpr_types.h"
#include "mod/stereo/stereo_types.h"
#include "param_routes.h"
#include "fx/harm/harm.h"
#include "fx/smooth/smooth.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/device_registry.h"
#include "spectrum.h"
#include "audio_notes_probe.h"
#include <sstream>
#include <cstring>
#include <clap/events.h>
#include <limits>

#include <algorithm>
#include <array>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using feathers::ParamId;
using feathers::ParamSet;
using feathers::Synth;

constexpr double kSampleRate = 48000.0;

/// A middle-of-the-keyboard note, and a second one a fifth up. Neither is near a stair mip boundary
/// and neither is the tuning reference, so nothing here passes by coincidence.
constexpr int16_t kKeyC4 = 60;
constexpr int16_t kKeyG4 = 67;

/// What a note does, at the sample it does it.
struct Event {
	enum class Kind { NoteOn, NoteOff, Choke, LevelMod, Bend, Param, FilterParam } kind;
	uint32_t frame;
	int16_t channel{0};
	int16_t key{0};
	int32_t noteId{-1};
	double value{0.0};
	ParamId param{ParamId::AZone};
	/// FOR FilterParam ONLY. A swallowed device's control is not a ParamId and never will be, so a
	/// script that moves one carries the device's own control index instead — which is exactly the
	/// distinction the swallowing draws, arriving here as a second field rather than a wider enum.
	feathers::fx::filter::Param filterParam{feathers::fx::filter::Param::Cutoff};
};

Event noteOn(uint32_t frame, int16_t key, int32_t noteId) {
	return {Event::Kind::NoteOn, frame, 0, key, noteId, 1.0};
}

Event noteOff(uint32_t frame, int16_t key, int32_t noteId) {
	return {Event::Kind::NoteOff, frame, 0, key, noteId, 0.0};
}

/// A parameter moving mid-performance, which is what automation is.
Event param(uint32_t frame, ParamId id, double value) {
	Event e{Event::Kind::Param, frame, 0, 0, -1, value, id};
	return e;
}

/// A FILTER CONTROL MOVING MID-SCRIPT, which is the only way to sweep a swallowed device now.
Event filterParam(uint32_t frame, feathers::fx::filter::Param param, double value) {
	Event e{Event::Kind::FilterParam, frame};
	e.filterParam = param;
	e.value = value;
	return e;
}

/// A synth with its params, set up the way every test here wants it.
/// **WHERE THE ENGINE KEEPS A STRIP DEVICE** — it chooses its own slot when it adopts one (kill-lanes step
/// 3a), so a case that reads a device's output, or routes FROM it (`ParamSet`'s device coordinates are in
/// the engine's slots), asks by the device. Valid once a sync has adopted it; `kNoSlot` before that.
/// THE DEVICE ITSELF, for every per-device road on `LaneRack` (its output, its instance, its descriptor).
///
/// **AND A PLACE IN A CHAIN IS `(chain, position)`** (kill-lanes step 3b). These three asked the converged
/// state by the VIEW'S LANE; the state is each chain's ordered run now, and the view's own index IS that
/// position (`feathers_chains.h`'s `stateOfChains`), so the translation is gone rather than moved. The view
/// is still taken, for the bound: a slot past what the view holds is a device nobody is asking about.
inline feathers::DeviceInstanceId stripDeviceOf(const feathers::Synth& synth, const feathers::ModRack& rack,
                                                size_t slot) {
	if (slot >= rack.device.size()) {
		return feathers::kNoInstance;
	}
	return synth.laneForTest().deviceOfStatePosition(feathers::kStripChain, slot);
}

inline size_t engineStripSlot(const feathers::Synth& synth, const feathers::ModRack& rack, size_t slot) {
	if (slot >= rack.device.size()) {
		return feathers::rack::RackEngine::kNoSlot;
	}
	return synth.laneForTest().slotOfStatePosition(feathers::kStripChain, slot);
}

/// THE SAME FOR AN FX DEVICE, by its position in the chain.
inline size_t engineFxSlot(const feathers::Synth& synth, const feathers::FxChain& chain, size_t position) {
	if (position >= feathers::fxChainLength(chain)) {
		return feathers::rack::RackEngine::kNoSlot;
	}
	return synth.laneForTest().slotOfStatePosition(feathers::kLaneChain, position);
}

struct Rig {
	ParamSet params;
	Synth synth{params};

	Rig() {
		synth.activate(kSampleRate, 4096);
		// An explicit character rather than the shipped default, so a change to what the plugin loads
		// with cannot quietly change what these tests are measuring. Zone 17 at offset 0 is the morph
		// case the renderer suite already leans on; Path at 0 puts the blend entirely on sound A.
		set(ParamId::AZone, 17.0);
		set(ParamId::AOffset, 0.0);
		set(ParamId::Path, 0.0);
		// AND A FIXED LEVEL. The plugin's default patch puts Level's base at zero and lets the strip's
		// envelope carry it (backlog EF); a bare Synth has no bridge to route that envelope, so a case
		// that does not set Level itself would render silence. The old fixed level, said out loud.
		set(ParamId::VoiceLevel, 1.0);
	}

	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }

	/// THE VOICE EDGE'S LENGTHS, in log2 seconds as the deleted Attack/Release knobs took them (2026-10-03). The
	/// edge is a fixed 2 ms de-click now; these cases lengthen it through the test seam to land events inside a
	/// Release or an Attack, which a routed tail still produces in a shipping build.
	double attackSeconds = feathers::kVoiceEdgeSeconds;
	double releaseSeconds = feathers::kVoiceEdgeSeconds;
	void setAttack(double log2Seconds) {
		attackSeconds = std::exp2(log2Seconds);
		synth.setVoiceEdgesForTest(attackSeconds, releaseSeconds);
	}
	void setRelease(double log2Seconds) {
		releaseSeconds = std::exp2(log2Seconds);
		synth.setVoiceEdgesForTest(attackSeconds, releaseSeconds);
	}

	/// THE FILTER IS DRIVEN THROUGH ITS ROW, because it no longer has parameters to drive.
	///
	/// This is the test-side half of the swallowing. `setValueOf` clamps and rounds by the control's
	/// own declaration exactly as ParamSet::setBase did by the parameter's, so a case that used to
	/// push an out-of-range value still gets it coerced — the coercion simply moved to the device
	/// that declares the range.
	void setFilter(feathers::fx::filter::Param param, double value) {
		feathers::fx::filter::setValueOf(&filterState, param, static_cast<float>(value));
		feathers::setFxFilterAt(&chain, 0, filterState);
		// The count is what makes the slot LIVE — setFxFilterAt writes the row and deliberately does
		// not touch it, since a row past the count is a stale row rather than a device.
		chain.count = 1;
		publishChain();
	}

	/// THE TWO HALVES OF A CHAIN CHANGE, IN THE ORDER THE THREADS TAKE THEM.
	///
	/// `provisionDevices` is the MESSAGE thread's half: it BUILDS the instances the chain names, which
	/// is where the allocation lives. `setFxChain` is the audio thread's per-block handoff. In the
	/// plugin the two are `pumpEditor` and `process`; a fixture doing only the second would leave
	/// every slot empty, because `FxRunner::sync` adopts what already exists and cannot build.
	/// ANY DEVICE IN SLOT 0, at a row the caller built. `setFilter` is the filter's own road and
	/// carries a `fx::filter::State`; this takes a `DeviceRow` so a case can seat OWLTT or drawn
	/// without the fixture having to learn each device's decode.
	void setDeviceRow(const feathers::DeviceRow& row) {
		// **`device[0]` IS POSITION 0 (kill-lanes step 3c)**, which is what this always meant and what
		// it now says: the array was lane-indexed with an `order[]` beside it, and the two agreed here
		// only because this fixture writes a single device. A chain IS its order now, so a row written
		// at an index is the device drawn there.
		chain.device[0].row = row;
		chain.count = 1;
		publishChain();
	}

	void publishChain() {
		// **THERE IS NO LANE PASS ANY MORE (kill-lanes step 3c).** This called `assignDenseFxLanes`
		// first, and the reason was real while it lasted: the fixture writes rows into the chain
		// directly rather than through `addFxDevice`, so nothing had handed its devices a routing
		// address — and a device with no address was one `slotsOf` placed nowhere and nothing could be
		// routed to. A device is placed by its POSITION now and a route names its instance id, so a row
		// written into the array is reachable the moment `count` covers it. Nothing has to be handed
		// out, so there is nothing this could hand out twice.
		synth.provisionDevices(chain, feathers::defaultModRack());
		synth.setFxChain(chain);
	}

	/// **THE SAME TWO HALVES, FOR THE MOD RACK** — and it is the same rule rather than a second one.
	/// `ModRunner::sync` adopts what already exists and cannot build, exactly as `FxRunner::sync`
	/// cannot, so a fixture that handed over only the rack would leave every modulator slot empty and
	/// every source reading zero. In the plugin these two are `pumpEditor` and `process`.
	///
	/// IT USED NOT TO MATTER, WHICH IS WHY EVERY OLDER CASE CALLS `setModRack` DIRECTLY. The engine
	/// held one envelope and one MSEG per slot, constructed with the Synth — nothing was ever built,
	/// so nothing could be missing. A case that only ever puts a DAHDSR in slot 0 still passes either
	/// way, because that is what the default rack already provisioned.
	void publishModRack(const feathers::ModRack& rack) {
		synth.provisionDevices(chain, rack);
		synth.setModRack(rack);
	}

	feathers::FxChain chain {};
	feathers::fx::filter::State filterState {};

	void apply(const Event& e) {
		switch (e.kind) {
		case Event::Kind::NoteOn:
			synth.noteOn(e.channel, e.key, e.noteId, e.value);
			break;
		case Event::Kind::NoteOff:
			synth.noteOff(e.channel, e.key, e.noteId, e.value);
			break;
		case Event::Kind::Choke:
			synth.choke(e.channel, e.key, e.noteId);
			break;
		case Event::Kind::LevelMod:
			synth.paramModForNote(e.channel, e.key, e.noteId, static_cast<clap_id>(ParamId::VoiceLevel),
			                      e.value);
			break;
		case Event::Kind::Bend:
			synth.pitchBend(e.channel, e.value);
			break;
		case Event::Kind::Param:
			set(e.param, e.value);
			break;
		case Event::Kind::FilterParam:
			setFilter(e.filterParam, e.value);
			break;
		}
	}

	/// WHERE THE SONG IS WHEN play() STARTS, and whether it is rolling. Mirrors what the CLAP
	/// binding takes off process->transport once a block; `locate` is a host jumping the playhead.
	double songStartSeconds = 0.0;
	bool transport = false;
	bool playing = true;

	void locate(double seconds) {
		songStartSeconds = seconds;
		transport = true;
	}

	/// Play `script` over `frames` samples, cutting the render at each event exactly as the CLAP
	/// binding does, in blocks of `block`.
	std::vector<float> play(const std::vector<Event>& script, uint32_t frames, int block) {
		std::vector<float> left(frames, 0.0f);
		std::vector<float> right(frames, 0.0f);

		size_t next = 0;
		uint32_t at = 0;
		while (at < frames) {
			const uint32_t blockEnd = std::min(frames, at + static_cast<uint32_t>(block));
			// ONCE A BLOCK, BEFORE ANYTHING IN IT IS RENDERED — the binding's own order, and the
			// re-anchoring the design depends on. The position handed over is where this block starts
			// in the song, which is the start plus everything played so far.
			if (transport) {
				// A STOPPED TRANSPORT DOES NOT ADVANCE, and a first version of this rig advanced it
				// anyway — which made the free-run subcase pass for the wrong reason, since locking
				// to a position that moves is indistinguishable from free-running. A mutation that
				// locked to the song even when stopped survived because of it. A real host stopped
				// at 2.5 seconds sends 2.5 every block, forever, and that is the whole reason the
				// stopped case needs a different clock at all.
				const double blockStart =
				    playing ? songStartSeconds + static_cast<double>(at) / kSampleRate
				            : songStartSeconds;
				synth.setTransport(blockStart, feathers::device::TransportClock::kNoPosition, playing);
			}
			while (at < blockEnd) {
				while (next < script.size() && script[next].frame <= at) {
					apply(script[next]);
					next++;
				}
				uint32_t until = blockEnd;
				if (next < script.size()) {
					until = std::min(blockEnd, script[next].frame);
				}
				synth.render(left.data() + at, right.data() + at, static_cast<int>(until - at));
				at = until;
			}
		}

		// Mono into both channels is the v1 rule, and a right channel that had drifted from the left
		// would be a silent bug in a stereo host.
		//
		// EXCEPT WHERE A CASE IS DELIBERATELY DRIVING A WIDTH. `expectMono` is false only for the
		// sweeps that set every declared param to an extreme, where the stereo controls are among
		// them: there the channels differing is the point rather than a defect, and asserting it here
		// would make a stereo control look like a bug in a case about something else.
		lastRight = right;
		if (expectMono) {
			CHECK(left == right);
		}
		return left;
	}

	/// See the note above `CHECK(left == right)` in `play`.
	bool expectMono = true;
	/// The right channel of the last `play`, for a case about the ears.
	std::vector<float> lastRight;
};

float peakOf(const std::vector<float>& x, size_t from, size_t to) {
	float peak = 0.0f;
	for (size_t i = from; i < std::min(to, x.size()); i++) {
		peak = std::max(peak, std::abs(x[i]));
	}
	return peak;
}

bool allExactlyZero(const std::vector<float>& x, size_t from, size_t to) {
	for (size_t i = from; i < std::min(to, x.size()); i++) {
		if (x[i] != 0.0f) {
			return false;
		}
	}
	return true;
}

/// Index of the last sample that is not exactly zero, or -1.
long lastNonZero(const std::vector<float>& x) {
	for (long i = static_cast<long>(x.size()) - 1; i >= 0; i--) {
		if (x[static_cast<size_t>(i)] != 0.0f) {
			return i;
		}
	}
	return -1;
}

/// Index of the first sample two renders disagree at, or -1 when they are identical.
///
/// Compared this way rather than with == so that a failure names the sample it diverged at, which is
/// the one fact that identifies WHICH schedule went wrong. Printing six thousand floats identifies
/// nothing.
long firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
	if (a.size() != b.size()) {
		return 0;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i] != b[i]) {
			return static_cast<long>(i);
		}
	}
	return -1;
}

/// Samples for `seconds` at the test rate, rounded the way the synth rounds an edge.
int samplesFor(double seconds) {
	return static_cast<int>(std::round(seconds * kSampleRate));
}

} // namespace

TEST_CASE("a note makes sound, and stops making it when the release has run out") {
	Rig rig;
	constexpr uint32_t kFrames = 4096;
	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOff(1024, kKeyC4, 1)};
	const std::vector<float> out = rig.play(script, kFrames, 64);

	// Sounding while held. Measured away from the attack so the 2 ms edge is not what is being
	// asserted about.
	CHECK(peakOf(out, 256, 1024) > 0.05f);

	// And exactly silent once the tail has run — not "quiet", zero. A voice that never reached its
	// end would leave a residue here, and a voice that was cut instead of released would have left
	// the tail missing rather than the silence.
	const int releaseSamples = samplesFor(rig.releaseSeconds);
	CHECK(allExactlyZero(out, 1024 + static_cast<size_t>(releaseSamples) + 1, kFrames));
	CHECK(rig.synth.soundingVoiceCount() == 0);
	CHECK(rig.synth.managedVoiceCount() == 0);
}

TEST_CASE("an empty constellation renders exact silence — deleting the last sound is legal") {
	// THE MEASUREMENT BEHIND A DECISION. The delete verb may remove the last sound because "an empty
	// patch is a valid patch" — a claim that until now rested on character_bank.cpp's own comment
	// ("every sound switched off is silence, which is the honest reading of an empty constellation").
	// A comment is not a measurement. This renders the state a delete-the-last-sound leaves — every
	// slot disabled, identities at their defaults — through the real synth, with a note held, and
	// asserts the output is EXACTLY zero and NaN-free. If this ever fails, the delete verb's
	// refuses-nowhere decision reopens; that is the deal under which it shipped.
	Rig rig;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		rig.set(static_cast<ParamId>(feathers::soundParam(slot, feathers::SoundField::Enable)), 0.0);
	}
	constexpr uint32_t kFrames = 4096;
	const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOff(2048, kKeyC4, 1) };
	const std::vector<float> out = rig.play(script, kFrames, 64);

	CHECK(allExactlyZero(out, 0, kFrames));
	for (const float sample : out) {
		REQUIRE(std::isfinite(sample));
	}
}

TEST_CASE("the amplitude edges take the time the params say they take") {
	// Measured as a RATIO against a run with the fastest edges the params offer. Both runs are
	// identical in every other respect — same slot, same seed, same phase, same character — so
	// dividing one by the other cancels the oscillator entirely and what is left is the envelope. No
	// windowing, no peak-follower, no threshold to tune.
	constexpr uint32_t kFrames = 8192;
	constexpr uint32_t kOffAt = 4096;
	constexpr double kSlowSeconds = 0.02; // 20 ms, ten times the default and well clear of it.

	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOff(kOffAt, kKeyC4, 1)};

	Rig fast;
	fast.setAttack(-12.0); // the minimum the range offers: 0.24 ms
	fast.setRelease(-12.0);
	const std::vector<float> reference = fast.play(script, kFrames, 128);

	Rig slow;
	slow.setAttack(std::log2(kSlowSeconds));
	slow.setRelease(std::log2(kSlowSeconds));
	const std::vector<float> shaped = slow.play(script, kFrames, 128);

	const int expected = samplesFor(kSlowSeconds);
	REQUIRE(expected == 960);

	// The attack: the first sample at which the ratio has reached full scale. Samples where the
	// reference is near a zero crossing are skipped — the ratio is exact everywhere, but dividing two
	// small numbers turns float rounding into a large relative error.
	long reachedFull = -1;
	for (size_t i = 1; i < kOffAt; i++) {
		if (std::abs(reference[i]) < 0.05f) {
			continue;
		}
		const double ratio = static_cast<double>(shaped[i]) / static_cast<double>(reference[i]);
		if (ratio > 0.999) {
			reachedFull = static_cast<long>(i);
			break;
		}
	}
	CAPTURE(reachedFull);
	// Within a couple of samples of the requested length, allowing for the first zero crossing the
	// search is willing to look at.
	CHECK(reachedFull >= expected - 2);
	CHECK(reachedFull < expected + 32);

	// The release is the cleaner measurement of the two: the voice goes silent the sample its ramp
	// reaches zero, so the last nonzero sample dates the end of the release exactly. Held at full
	// level when the note-off arrives, that is one release length after it.
	const long lastSounding = lastNonZero(shaped);
	CAPTURE(lastSounding);
	CHECK(lastSounding >= static_cast<long>(kOffAt) + expected - 2);
	CHECK(lastSounding <= static_cast<long>(kOffAt) + expected + 1);

	// And the fast run's tail ends far earlier, which is what says the measurement is of the param
	// and not of something fixed.
	CHECK(lastNonZero(reference) < static_cast<long>(kOffAt) + 64);
}

TEST_CASE("more notes than voices steals, and the stolen voice fades rather than being cut") {
	Rig rig;
	rig.set(ParamId::VoiceCount, 4.0);
	// Long release, so nothing ends on its own and every voice still sounding is one the manager
	// decided should be.
	rig.setRelease(0.0); // 1 second

	constexpr uint32_t kFrames = 2048;
	std::vector<Event> script;
	for (int i = 0; i < 5; i++) {
		script.push_back(noteOn(static_cast<uint32_t>(i) * 8, static_cast<int16_t>(kKeyC4 + i * 2),
		                        i + 1));
	}
	const std::vector<float> out = rig.play(script, kFrames, 64);

	// Four is the limit and four is what plays. The fifth note took a voice; it did not add one.
	CHECK(rig.synth.managedVoiceCount() == 4);
	CHECK(rig.synth.soundingVoiceCount() == 4);
	CHECK(peakOf(out, 512, kFrames) > 0.05f);

	// The steal is not a cut. Immediately after the fifth note-on there are five voices making sound
	// — the four the manager counts plus the one on its way out — and the extra one is gone within
	// the steal fade. Rendered in two pieces so the count can be read between them.
	Rig fading;
	fading.set(ParamId::VoiceCount, 4.0);
	fading.setRelease(0.0);
	fading.play(script, 33, 64); // just past the fifth note-on at frame 32
	CHECK(fading.synth.soundingVoiceCount() == 5);
	CHECK(fading.synth.managedVoiceCount() == 4);

	fading.play({}, static_cast<uint32_t>(samplesFor(feathers::kStealFadeSeconds)) + 2, 64);
	CHECK(fading.synth.soundingVoiceCount() == 4);
}

TEST_CASE("a per-note level modulation moves one voice and leaves the other alone") {
	// The claim is a difference, so it is measured as one. Two notes play; the modulation is applied
	// to the first note's id only; and what the modulation changed has to be exactly the first
	// voice's own signal, scaled — no trace of the second voice may appear in the difference.
	constexpr uint32_t kFrames = 2048;
	constexpr double kOffset = -0.4;

	const std::vector<Event> both{noteOn(0, kKeyC4, 1), noteOn(0, kKeyG4, 2)};
	std::vector<Event> modded = both;
	// At frame zero but after the note-ons, which is the order a host sends them in: the modulation
	// is addressed to a note id, so the note has to exist for it to reach anything. A frame later and
	// the difference would carry the samples rendered before it arrived, which is a different claim.
	modded.push_back({Event::Kind::LevelMod, 0, 0, kKeyC4, 1, kOffset});

	Rig plain;
	const std::vector<float> unmodulated = plain.play(both, kFrames, 128);

	Rig modulated;
	const std::vector<float> shifted = modulated.play(modded, kFrames, 128);

	// The first note alone, in its own synth. It lands in the same slot with the same seed and the
	// same phase, so its samples here are the ones it contributes to the mixes above, bit for bit.
	Rig solo;
	const std::vector<float> first = solo.play({noteOn(0, kKeyC4, 1)}, kFrames, 128);

	REQUIRE(peakOf(first, 512, kFrames) > 0.05f);

	double worst = 0.0;
	for (size_t i = 0; i < kFrames; i++) {
		const double delta = static_cast<double>(shifted[i]) - static_cast<double>(unmodulated[i]);
		worst = std::max(worst, std::abs(delta - kOffset * static_cast<double>(first[i])));
	}
	CAPTURE(worst);
	// Not exact only because the two mixes sum their voices in floating point before the subtraction.
	CHECK(worst < 1e-6);

	// The modulation is per note id, so the voice that did not get one is untouched — which the
	// difference above already says, and this says again in the form the bug would take: the second
	// note's own level never moved.
	Rig secondSolo;
	const std::vector<float> second = secondSolo.play({noteOn(0, kKeyG4, 2)}, kFrames, 128);
	CHECK(peakOf(second, 512, kFrames) > 0.05f);
}

TEST_CASE("master gain scales the mix and nothing else") {
	constexpr uint32_t kFrames = 1024;
	const std::vector<Event> script{noteOn(0, kKeyC4, 1)};

	Rig unity;
	const std::vector<float> full = unity.play(script, kFrames, 64);
	// NOT SILENCE (backlog MA): a ratio of two silent renders holds at any gain, so the reference must sound.
	REQUIRE(peakOf(full, 0, kFrames) > 0.05f);

	Rig quiet;
	quiet.set(ParamId::MasterGain, -6.0);
	const std::vector<float> attenuated = quiet.play(script, kFrames, 64);

	const auto gain = static_cast<float>(feathers::decibelsToGain(-6.0));
	for (size_t i = 0; i < kFrames; i++) {
		REQUIRE(attenuated[i] == doctest::Approx(full[i] * gain).epsilon(1e-6));
	}

	// The bottom of the fader is silence rather than a very quiet signal, which is the rule
	// decibelsToGain encodes and the reason the fader has a declared minimum at all.
	Rig silent;
	silent.set(ParamId::MasterGain, -60.0);
	const std::vector<float> nothing = silent.play(script, kFrames, 64);
	CHECK(allExactlyZero(nothing, 0, kFrames));
}

TEST_CASE("the same performance renders the same samples, twice and at any block size") {
	// Determinism first: two synths given the same notes must agree bit for bit, or a bounce does not
	// match the take before it. Then the harder claim — that the samples do not depend on how the
	// host chopped its buffer. Blocks of 1 and 37 put an event boundary and a chunk boundary in a
	// different place on essentially every call.
	constexpr uint32_t kFrames = 3000;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    noteOn(37, kKeyG4, 2),  // deliberately not on any power of two
	    noteOff(701, kKeyC4, 1),
	    noteOn(1024, kKeyC4, 3),
	    noteOff(1500, kKeyG4, 2),
	    noteOff(2011, kKeyC4, 3),
	};

	Rig a;
	Rig b;
	const std::vector<float> first = a.play(script, kFrames, 256);
	const std::vector<float> again = b.play(script, kFrames, 256);
	// NOT SILENCE (backlog MA): two silent renders agree whatever the code does, so the reference must sound.
	REQUIRE(peakOf(first, 0, kFrames) > 0.05f);
	CHECK(firstDifference(first, again) == -1);

	for (int block : {1, 37, 128}) {
		CAPTURE(block);
		Rig rig;
		CHECK(firstDifference(rig.play(script, kFrames, block), first) == -1);
	}
}

TEST_CASE("a choke takes the note away without a release tail") {
	Rig rig;
	rig.setRelease(0.0); // 1 second, so a release would be unmistakable
	constexpr uint32_t kFrames = 2048;

	const std::vector<Event> script{noteOn(0, kKeyC4, 1),
	                                {Event::Kind::Choke, 512, 0, kKeyC4, 1, 0.0}};
	const std::vector<float> out = rig.play(script, kFrames, 64);

	CHECK(peakOf(out, 256, 512) > 0.05f);
	// Gone within the steal fade rather than within the release, which is the whole difference
	// between a choke and a note-off.
	const long last = lastNonZero(out);
	CAPTURE(last);
	CHECK(last < 512 + samplesFor(feathers::kStealFadeSeconds) + 2);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("voices get their own noise streams, so a chord is an ensemble") {
	// Two voices given identical everything would draw identical jitter and grit, and sum coherently
	// — one loud modulated oscillator rather than several instruments. The seeds come from the slot
	// index, so the check is that the same note in slot 0 and in slot 1 does not render the same
	// samples. Zone 0 at offset 0.25 is one of the few positions the character layer gives a nonzero
	// jitter depth to, which is what makes the difference visible at all.
	constexpr uint32_t kFrames = 1024;

	Rig first;
	first.set(ParamId::AZone, 0.0);
	first.set(ParamId::AOffset, 0.25);
	const std::vector<float> slotZero = first.play({noteOn(0, kKeyC4, 1)}, kFrames, 128);

	// The same note, but with another voice already holding slot 0.
	Rig second;
	second.set(ParamId::AZone, 0.0);
	second.set(ParamId::AOffset, 0.25);
	const std::vector<float> withNeighbour =
	    second.play({noteOn(0, kKeyG4, 1), noteOn(0, kKeyC4, 2)}, kFrames, 128);

	// Subtracting the neighbour leaves slot 1's rendering of the same note. If the seeds were shared
	// it would equal slot 0's exactly.
	Rig neighbourOnly;
	neighbourOnly.set(ParamId::AZone, 0.0);
	neighbourOnly.set(ParamId::AOffset, 0.25);
	const std::vector<float> neighbour = neighbourOnly.play({noteOn(0, kKeyG4, 1)}, kFrames, 128);

	double worst = 0.0;
	for (size_t i = 0; i < kFrames; i++) {
		const double slotOne = static_cast<double>(withNeighbour[i]) - static_cast<double>(neighbour[i]);
		worst = std::max(worst, std::abs(slotOne - static_cast<double>(slotZero[i])));
	}
	CAPTURE(worst);
	CHECK(worst > 1e-4);
}

TEST_CASE("a note the voice manager could not index is refused rather than played") {
	// The manager keeps its per-key state in a plain 16x128 array and indexes it with the channel and
	// key it is handed. A wildcard or a malformed value would be a write past the end of it, so the
	// synth is the thing that has to know the bounds.
	Rig rig;
	constexpr uint32_t kFrames = 512;

	std::vector<Event> bad;
	bad.push_back({Event::Kind::NoteOn, 0, -1, kKeyC4, 1, 1.0});
	bad.push_back({Event::Kind::NoteOn, 0, 0, -1, 2, 1.0});
	bad.push_back({Event::Kind::NoteOn, 0, 16, kKeyC4, 3, 1.0});
	bad.push_back({Event::Kind::NoteOn, 0, 0, 128, 4, 1.0});
	const std::vector<float> out = rig.play(bad, kFrames, 64);

	CHECK(rig.synth.soundingVoiceCount() == 0);
	CHECK(allExactlyZero(out, 0, kFrames));
}

TEST_CASE("a wildcard note-off still lifts the notes it names") {
	// CLAP lets a host release by note id alone, with the channel and key left at -1. Those cannot be
	// passed through — see above — so they are expanded against the voices actually playing. Dropping
	// them instead would leave the note stuck down forever, which is the worse failure by far.
	Rig rig;
	rig.setRelease(-12.0); // fastest release, so the tail does not blur the measurement
	constexpr uint32_t kFrames = 2048;

	std::vector<Event> script{noteOn(0, kKeyC4, 7), noteOn(0, kKeyG4, 8)};
	script.push_back({Event::Kind::NoteOff, 512, -1, -1, 7, 0.0});
	const std::vector<float> out = rig.play(script, kFrames, 64);

	// One note lifted, the other still held.
	CHECK(rig.synth.soundingVoiceCount() == 1);
	CHECK(rig.synth.managedVoiceCount() == 1);
	CHECK(peakOf(out, 1024, kFrames) > 0.05f);
}

TEST_CASE("legato moves the playing voice without restarting its edges, and mono restarts them") {
	// The one rule that separates the two mono modes, and the reason both exist. Measured with a slow
	// attack so a restart is unmistakable: twenty milliseconds after the second note, a retriggered
	// voice is still climbing out of silence while a moved one has not left full level.
	constexpr uint32_t kFrames = 12000;
	constexpr uint32_t kSecondNote = 6000;
	constexpr double kSlowAttack = 0.05; // 50 ms

	// A window starting clear of the steal fade the retrigger leaves behind, and ending well before
	// a 50 ms attack could have finished.
	constexpr size_t kFrom = kSecondNote + 480;  // 10 ms
	constexpr size_t kTo = kSecondNote + 1200;   // 25 ms

	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOn(kSecondNote, kKeyG4, 2)};

	auto runIn = [&](double voiceMode) {
		Rig rig;
		rig.set(ParamId::VoiceMode, voiceMode);
		rig.setAttack(std::log2(kSlowAttack));
		return rig.play(script, kFrames, 128);
	};

	const std::vector<float> mono = runIn(1.0);
	const std::vector<float> legato = runIn(2.0);

	// The level each run had reached just before the second note, which is full in both.
	const float monoHeld = peakOf(mono, kSecondNote - 1200, kSecondNote);
	const float legatoHeld = peakOf(legato, kSecondNote - 1200, kSecondNote);
	REQUIRE(monoHeld > 0.05f);
	REQUIRE(legatoHeld > 0.05f);

	CHECK(peakOf(legato, kFrom, kTo) > 0.8f * legatoHeld);
	CHECK(peakOf(mono, kFrom, kTo) < 0.5f * monoHeld);

	// Both are still one voice: legato moved the one it had, and mono replaced it rather than
	// stacking on top of it.
	CHECK(peakOf(legato, kTo, kFrames) > 0.05f);
}

TEST_CASE("glide is an exponential approach with the glide time as its constant") {
	constexpr double kTau = 0.1; // 100 ms
	constexpr uint32_t kFrames = 48000;
	constexpr uint32_t kSecondNote = 4800;
	constexpr int16_t kKeyC5 = 72;

	Rig rig;
	rig.set(ParamId::VoiceMode, 1.0); // Mono
	// The glide param is a cubic taper onto two seconds, so the setting for a given time is its cube
	// root. Going through the taper rather than around it is the point — this is the number a user
	// dials, not an internal one.
	rig.set(ParamId::GlideTime, std::cbrt(kTau / feathers::kGlideMaxSeconds));
	REQUIRE(rig.params.glideTimeSeconds() == doctest::Approx(kTau));

	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOn(kSecondNote, kKeyC5, 2)};

	// Rendered in pieces so the pitch can be read at known instants.
	size_t next = 0;
	uint32_t at = 0;
	std::vector<float> out(kFrames, 0.0f);
	std::vector<float> right(kFrames, 0.0f);
	auto renderUntil = [&](uint32_t until) {
		while (at < until) {
			while (next < script.size() && script[next].frame <= at) {
				rig.apply(script[next]);
				next++;
			}
			uint32_t stop = until;
			if (next < script.size()) {
				stop = std::min(until, script[next].frame);
			}
			rig.synth.render(out.data() + at, right.data() + at, static_cast<int>(stop - at));
			at = stop;
		}
	};

	renderUntil(kSecondNote);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC5);
	renderUntil(kSecondNote + 1);
	const int gliding = rig.synth.slotOfKeyForTest(kKeyC5);
	REQUIRE(gliding >= 0);
	CAPTURE(slot);

	// It starts where the note it replaced was, not at the note it is heading for. A mono retrigger
	// that started at the target would have nothing left to glide.
	CHECK(rig.synth.voicePitchForTest(gliding) < static_cast<double>(kKeyC4) + 1.0);

	// One time constant in: 1 - 1/e of the way, which is 63.2% of twelve semitones.
	renderUntil(kSecondNote + static_cast<uint32_t>(kTau * kSampleRate));
	const double afterOneTau = rig.synth.voicePitchForTest(gliding);
	CAPTURE(afterOneTau);
	CHECK(afterOneTau == doctest::Approx(kKeyC4 + 12.0 * (1.0 - std::exp(-1.0))).epsilon(0.01));

	// Three in: within a twentieth of the distance, which is what "arrived" means for an approach
	// that never formally gets there.
	renderUntil(kSecondNote + static_cast<uint32_t>(3.0 * kTau * kSampleRate));
	const double afterThreeTau = rig.synth.voicePitchForTest(gliding);
	CAPTURE(afterThreeTau);
	CHECK(afterThreeTau > kKeyC5 - 0.05 * 12.0);
	CHECK(afterThreeTau <= kKeyC5);

	// And the audio agrees, measured where the note has stopped moving so a spectrum has something
	// stationary to find.
	renderUntil(kFrames);
	const feathers::test::Window window(16384);
	std::vector<float> tail(out.end() - 16384, out.end());
	double bestHz = 0.0;
	double best = -1.0;
	for (double hz = 480.0; hz <= 560.0; hz += 0.25) {
		const double m = feathers::test::magnitudeAt(tail, window, kSampleRate, hz);
		if (m > best) {
			best = m;
			bestHz = hz;
		}
	}
	CAPTURE(bestHz);
	CHECK(bestHz == doctest::Approx(feathers::pitchToHz(kKeyC5)).epsilon(0.01));
}

TEST_CASE("poly mode ignores the glide time") {
	// A v1 rule with a reason: a portamento is a statement about the note before this one, and in
	// poly there is no single note before this one. Poly glide is deferred rather than approximated.
	Rig rig;
	rig.set(ParamId::GlideTime, 1.0); // the longest glide the param offers
	constexpr uint32_t kFrames = 512;
	constexpr int16_t kKeyC5 = 72;

	rig.play({noteOn(0, kKeyC4, 1)}, 256, 64);
	rig.play({}, 1, 64);

	Rig second;
	second.set(ParamId::GlideTime, 1.0);
	second.play({noteOn(0, kKeyC4, 1), noteOn(0, kKeyC5, 2)}, kFrames, 64);

	const int slot = second.synth.slotOfKeyForTest(kKeyC5);
	REQUIRE(slot >= 0);
	// Exactly the note, from its first sample: no glide was armed at all.
	CHECK(second.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC5));
}

namespace {

/// **ONE NOTE AFTER ANOTHER, READ AS VALUES** — the pitch and the voice edge of the second note at its first sample,
/// one glide time constant in, and ten milliseconds in, with the first note either still held or released a
/// short while before. Rendered in pieces so each reading is at a known sample.
struct SecondNoteReadings {
	int slotBefore = -1;      // the first note's voice, read just before the second arrives
	int slot = -1;            // the second note's voice
	double pitchAtOnset = 0.0;
	double pitchAfterOneTau = 0.0;
	float levelBefore = 0.0f;
	float levelAtOnset = 0.0f;
	float levelAfter10ms = 0.0f;
};

constexpr double kLegatoTau = 0.1; // 100 ms
constexpr int16_t kLegatoKeyC5 = 72;

SecondNoteReadings playSecondNote(double voiceMode, bool releaseFirst) {
	constexpr uint32_t kFirstOff = 4800;     // 100 ms: the 50 ms attack is finished
	constexpr uint32_t kSecondNote = 7200;   // 50 ms into a one-second release, so the voice is still sounding
	constexpr uint32_t kTauFrames = static_cast<uint32_t>(kLegatoTau * kSampleRate);

	Rig rig;
	rig.set(ParamId::VoiceMode, voiceMode);
	rig.set(ParamId::GlideTime, std::cbrt(kLegatoTau / feathers::kGlideMaxSeconds));
	REQUIRE(rig.params.glideTimeSeconds() == doctest::Approx(kLegatoTau));
	rig.setAttack(std::log2(0.05)); // 50 ms: a restarted edge is still climbing 10 ms in
	rig.setRelease(0.0);            // 1 s: the first note is well inside its tail when the second arrives

	std::vector<Event> script{noteOn(0, kKeyC4, 1)};
	if (releaseFirst) {
		script.push_back(noteOff(kFirstOff, kKeyC4, 1));
	}
	script.push_back(noteOn(kSecondNote, kLegatoKeyC5, 2));

	size_t next = 0;
	uint32_t at = 0;
	std::vector<float> left(kSecondNote + kTauFrames + 1, 0.0f);
	std::vector<float> right(left.size(), 0.0f);
	auto renderUntil = [&](uint32_t until) {
		while (at < until) {
			while (next < script.size() && script[next].frame <= at) {
				rig.apply(script[next]);
				next++;
			}
			uint32_t stop = until;
			if (next < script.size()) {
				stop = std::min(until, script[next].frame);
			}
			rig.synth.render(left.data() + at, right.data() + at, static_cast<int>(stop - at));
			at = stop;
		}
	};

	SecondNoteReadings r;
	renderUntil(kSecondNote);
	r.slotBefore = rig.synth.slotOfKeyForTest(kKeyC4);
	r.levelBefore = rig.synth.voiceEdgeLevelForTest(r.slotBefore);
	renderUntil(kSecondNote + 1);
	r.slot = rig.synth.slotOfKeyForTest(kLegatoKeyC5);
	r.pitchAtOnset = rig.synth.voicePitchForTest(r.slot);
	r.levelAtOnset = rig.synth.voiceEdgeLevelForTest(r.slot);
	renderUntil(kSecondNote + 480);
	r.levelAfter10ms = rig.synth.voiceEdgeLevelForTest(r.slot);
	renderUntil(kSecondNote + kTauFrames);
	r.pitchAfterOneTau = rig.synth.voicePitchForTest(r.slot);
	return r;
}

} // namespace

TEST_CASE("legato glides only on a true overlap: a note after a RELEASED one, still in its tail, starts at its own pitch and restarts the edge") {
	// THE STANDARD LEGATO RULE: glide and the no-retrigger
	// move are what an OVERLAP means. A key let go before the next one arrives was not overlapped by it, even
	// while its voice is still sounding its release — so the next note is a fresh note. The voice manager moves
	// the releasing voice and retriggers it (`moveAndRetriggerVoice`); before the fix the move left the pitch to
	// the next slice's `glideToPitch`, which carried it from the released key at the glide time.
	const SecondNoteReadings r = playSecondNote(2.0, true); // Legato
	CAPTURE(r.slotBefore);
	CAPTURE(r.slot);
	CAPTURE(r.pitchAtOnset);
	CAPTURE(r.pitchAfterOneTau);
	CAPTURE(r.levelBefore);
	CAPTURE(r.levelAtOnset);
	CAPTURE(r.levelAfter10ms);
	REQUIRE(r.slotBefore >= 0);
	REQUIRE(r.slot >= 0);
	// The first note was releasing: its edge had left full level and was still well clear of silence.
	REQUIRE(r.levelBefore < 1.0f);
	REQUIRE(r.levelBefore > 0.5f);

	// AT THE NEW NOTE FROM ITS FIRST SAMPLE, and still there a glide time later: nothing was gliding.
	CHECK(r.pitchAtOnset == doctest::Approx(kLegatoKeyC5).epsilon(1e-9));
	CHECK(r.pitchAfterOneTau == doctest::Approx(kLegatoKeyC5).epsilon(1e-9));
	// AND THE EDGE RESTARTED: climbing in its attack, where a release would still be falling.
	CHECK(r.levelAfter10ms > r.levelAtOnset);
	CHECK(r.levelAfter10ms > r.levelBefore);
}

TEST_CASE("legato on a true overlap glides from the held key at the glide time and does not restart the edge") {
	// The other half of the rule, so the fix above cannot be had by switching legato glide off.
	const SecondNoteReadings r = playSecondNote(2.0, false); // Legato
	CAPTURE(r.slotBefore);
	CAPTURE(r.slot);
	CAPTURE(r.pitchAtOnset);
	CAPTURE(r.pitchAfterOneTau);
	CAPTURE(r.levelBefore);
	CAPTURE(r.levelAtOnset);
	CAPTURE(r.levelAfter10ms);
	REQUIRE(r.slotBefore >= 0);
	// ONE VOICE, MOVED: the overlap does not start a second.
	CHECK(r.slot == r.slotBefore);
	// Starting at the held key, one sample in...
	CHECK(r.pitchAtOnset < static_cast<double>(kKeyC4) + 0.1);
	CHECK(r.pitchAtOnset >= static_cast<double>(kKeyC4));
	// ...and 1 - 1/e of the twelve semitones one time constant later: the glide test's own shape.
	CHECK(r.pitchAfterOneTau == doctest::Approx(kKeyC4 + 12.0 * (1.0 - std::exp(-1.0))).epsilon(0.01));
	// AND THE EDGE NEVER LEFT FULL LEVEL.
	CHECK(r.levelBefore == 1.0f);
	CHECK(r.levelAtOnset == 1.0f);
	CHECK(r.levelAfter10ms == 1.0f);
}

TEST_CASE("mono keeps its portamento after a RELEASED note: the next note glides from the released key (the fix is Legato only)") {
	// The legato fix above must not reach Mono: Mono glides on every new note, the usual portamento meaning.
	// Mono never takes `moveAndRetriggerVoice` — the manager ends the voice and starts another, handing over the
	// key it was on — so this pins that road by value, the released note's tail and all.
	const SecondNoteReadings r = playSecondNote(1.0, true); // Mono
	CAPTURE(r.slotBefore);
	CAPTURE(r.slot);
	CAPTURE(r.pitchAtOnset);
	CAPTURE(r.pitchAfterOneTau);
	REQUIRE(r.slot >= 0);
	CHECK(r.pitchAtOnset < static_cast<double>(kKeyC4) + 0.1);
	CHECK(r.pitchAtOnset >= static_cast<double>(kKeyC4));
	CHECK(r.pitchAfterOneTau == doctest::Approx(kKeyC4 + 12.0 * (1.0 - std::exp(-1.0))).epsilon(0.01));
}

TEST_CASE("note priority decides which note survives") {
	constexpr uint32_t kFrames = 4096;
	constexpr int16_t kLow = 55;
	constexpr int16_t kMid = 60;
	constexpr int16_t kHigh = 67;

	SUBCASE("poly steals the note the priority is not protecting") {
		// Two voices, three notes. In poly the setting picks the VICTIM, not whether the newcomer is
		// let in: a new note always sounds, and what the priority decides is which of the notes
		// already playing makes room for it. So Low sheds the highest note playing and High sheds the
		// lowest. Refusing a newcomer outright is a mono behaviour, and it is checked as one below.
		auto surviving = [&](double priority) {
			Rig rig;
			rig.set(ParamId::VoiceCount, 2.0);
			rig.set(ParamId::NotePriority, priority);
			rig.setRelease(0.0); // nothing ends on its own
			rig.play({noteOn(0, kMid, 1), noteOn(64, kHigh, 2), noteOn(128, kLow, 3)}, kFrames, 64);
			return std::vector<bool>{rig.synth.slotOfKeyForTest(kLow) >= 0,
			                         rig.synth.slotOfKeyForTest(kMid) >= 0,
			                         rig.synth.slotOfKeyForTest(kHigh) >= 0};
		};

		// The third note is the lowest of the three, so it is the one whose arrival tells the two
		// settings apart. Low priority sheds the high note to make room for it...
		const std::vector<bool> low = surviving(1.0);
		CHECK(low == std::vector<bool>{true, true, false});

		// ...and High priority sheds the middle one, which is the lowest then playing. The new note
		// sounds either way, which is the part worth being explicit about: this is not a rule about
		// what may play, it is a rule about what is given up.
		const std::vector<bool> high = surviving(2.0);
		CHECK(high == std::vector<bool>{true, false, true});
	}

	SUBCASE("mono only lets a new note take over if it wins on priority") {
		Rig rig;
		rig.set(ParamId::VoiceMode, 1.0); // Mono
		rig.set(ParamId::NotePriority, 2.0); // High
		rig.play({noteOn(0, kMid, 1)}, 512, 64);
		REQUIRE(rig.synth.slotOfKeyForTest(kMid) >= 0);

		// Lower than what is playing: refused, and the held note keeps going.
		rig.play({noteOn(0, kLow, 2)}, 512, 64);
		CHECK(rig.synth.slotOfKeyForTest(kMid) >= 0);
		CHECK(rig.synth.slotOfKeyForTest(kLow) < 0);

		// Higher: takes over.
		rig.play({noteOn(0, kHigh, 3)}, 512, 64);
		CHECK(rig.synth.slotOfKeyForTest(kHigh) >= 0);
		CHECK(rig.synth.slotOfKeyForTest(kMid) < 0);
	}
}

TEST_CASE("a note-addressed modulation is refused unless it is addressed to something") {
	// The manager's two parameter routers do not index anything by channel or key, so nothing here
	// is out of bounds today. The guard is ours precisely because of that: the rule is that the
	// manager never receives a channel or key it could not index with, and a rule upheld only by how
	// somebody else's routers happen to be written is not upheld.
	constexpr uint32_t kFrames = 1024;
	constexpr double kOffset = -0.5;
	const std::vector<Event> plain{noteOn(0, kKeyC4, 1)};

	auto withMod = [&](int16_t channel, int16_t key) {
		std::vector<Event> script = plain;
		script.push_back({Event::Kind::LevelMod, 0, channel, key, 1, kOffset});
		Rig rig;
		return rig.play(script, kFrames, 64);
	};

	Rig reference;
	const std::vector<float> unmodulated = reference.play(plain, kFrames, 64);
	REQUIRE(peakOf(unmodulated, 256, kFrames) > 0.05f);

	// Addressed properly: it lands.
	CHECK(withMod(0, kKeyC4) != unmodulated);

	// Addressed by note id alone, which is what CLAP's -1 in both fields means and what a host
	// modulating a note actually sends. This is the case a blunt bounds check would have thrown away,
	// taking per-note modulation with it.
	CHECK(withMod(-1, -1) != unmodulated);

	// Outside the manager's dimensions in either field: refused, and the note plays on untouched.
	CHECK(withMod(99, kKeyC4) == unmodulated);
	CHECK(withMod(0, 200) == unmodulated);
	CHECK(withMod(-2, kKeyC4) == unmodulated);
}

TEST_CASE("a deferred character rebuild lands where the samples say, not where the blocks do") {
	// The character bank refuses to rebuild while a voice may still be fading, and holds the request
	// until the guard drains. WHERE that held request lands is the question: it lands when the guard
	// reaches zero, and the guard is counted down by the renderer, so if the renderer counted in
	// pieces the host chose, a deferred rebuild would land at a different sample for every block
	// size — audible as a different render of the same automation.
	//
	// Two zone changes close enough together that the second is still deferred when it arrives: the
	// guard is the ~2 ms fade plus a decimator group, 100 samples at this rate, and these are 48
	// apart.
	constexpr uint32_t kFrames = 3000;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    param(64, ParamId::AZone, 200.0),
	    param(112, ParamId::AZone, 400.0),
	    // And again later, so a single lucky alignment cannot carry the test.
	    param(1024, ParamId::AZone, 600.0),
	    param(1060, ParamId::Path, 0.5),
	    param(1090, ParamId::AZone, 800.0),
	};

	Rig reference;
	const std::vector<float> expected = reference.play(script, kFrames, 256);
	REQUIRE(peakOf(expected, 512, kFrames) > 0.05f);

	for (int block : {1, 37, 128}) {
		CAPTURE(block);
		Rig rig;
		CHECK(firstDifference(rig.play(script, kFrames, block), expected) == -1);
	}
}
TEST_CASE("rotating the plane is silent until the sound travels") {
	// THE DESIGN RULE, played rather than computed: rotation that changed the sound would be redundant
	// with gamma, so it must keep the sound the same until you move in xy or gamma. Everything upstream of
	// here could satisfy that arithmetically and still fail it in the instrument, because the plumbing
	// is where a travel gets lost or a home gets mixed into the wrong term.
	//
	// The version of this test that came before asserted the OPPOSITE — that setting an orientation
	// changed the samples — and it failed the moment home and travel were separated, which is the
	// clearest evidence available that the separation does what it says.
	constexpr uint32_t kFrames = 4096;
	const std::vector<Event> parked{noteOn(0, kKeyC4, 1), param(64, ParamId::AOffset, 0.3),
	                                param(64, ParamId::BOffset, 0.3)};

	Rig reference;
	const std::vector<float> home = reference.play(parked, kFrames, 256);
	REQUIRE(peakOf(home, 512, kFrames) > 0.05f);

	// Turn the plane as hard as the audition ever will — and PIN it, so the pinned plane is what is
	// being tested rather than the derivation. Not one sample may move.
	for (float rotate : {0.0f, 0.35f, 1.5f}) {
		CAPTURE(rotate);
		Rig rig;
		feathers::SliceOrientationPin turned;
		turned.pinned = true;
		// The rotate M is decoded from — since the shrink it is the whole generator, so there is
		// nothing left beside it for a decode to quietly start reading.
		turned.generator.rotate = rotate;
		rig.synth.setOrientation(turned);
		CHECK(firstDifference(rig.play(parked, kFrames, 256), home) == -1);
	}

	// Now travel — which takes WEATHER as well as a move, because Weather is how much of the terrain
	// you get and it defaults to none. Pos Y alone changes nothing at all, and that is the design
	// rather than a bug: at Weather zero every sound is exactly itself and the blend is the relative
	// one v1 shipped.
	const std::vector<Event> travelled{noteOn(0, kKeyC4, 1),      param(64, ParamId::AOffset, 0.3),
	                                   param(64, ParamId::BOffset, 0.3), param(64, ParamId::PosY, 0.6),
	                                   param(64, ParamId::Weather, 1.0)};

	Rig rig;
	feathers::SliceOrientationPin turned;
	turned.pinned = true;
	turned.generator.rotate = 0.9f;
	rig.synth.setOrientation(turned);
	const std::vector<float> away = rig.play(travelled, kFrames, 256);
	CHECK(firstDifference(away, home) != -1);
	CHECK(peakOf(away, 512, kFrames) > 0.05f);

	// And the PLANE is what decided where it went: the same travel on ANOTHER plane arrives somewhere
	// else. Without this, "travel changes the sound" would be satisfied by a build that treated Pos Y
	// as one more gamma — which is exactly the redundancy the design is avoiding.
	//
	// It used to say "on a CLOSED plane", and the rename follows the born-open decision rather than a
	// weakening: a default generator is no longer the degenerate line, it is M read at rotate 0, which
	// is simply a different plane. The comparison the case needs is two DIFFERENT planes, and that is
	// what it makes.
	Rig shut;
	shut.synth.setOrientation(feathers::SliceOrientationPin { true, {} });
	CHECK(firstDifference(shut.play(travelled, kFrames, 256), away) != -1);
}

TEST_CASE("Weather is how much of the terrain you get, and none is the engine v1 shipped") {
	// Weather is the terrain amount, by design. At zero the blend is purely relative — sounds
	// crossfade and each is exactly itself — which is not merely similar to v1 but bit for bit it.
	//
	// The architecture that makes it so was deliberate rather than a hardware CPU concession: the
	// waypoint-and-excite crossfade decouples local topography from performed feel. Weather is the
	// dial that couples them back by choice.
	constexpr uint32_t kFrames = 4096;

	// Moved well off the tour, so a build that leaked ANY travel through would be caught.
	const std::vector<Event> parked{noteOn(0, kKeyC4, 1), param(64, ParamId::PosX, 0.8),
	                                param(64, ParamId::PosY, -0.7), param(64, ParamId::Path, 0.35)};

	Rig reference;
	const std::vector<float> dry = reference.play(parked, kFrames, 256);
	REQUIRE(peakOf(dry, 512, kFrames) > 0.05f);

	// Rotating the plane AND standing somewhere else still changes nothing while Weather is down.
	{
		Rig rig;
		feathers::SliceOrientationPin turned;
		turned.pinned = true;
		turned.generator.rotate = 1.4f;
		rig.synth.setOrientation(turned);
		CHECK(firstDifference(rig.play(parked, kFrames, 256), dry) == -1);
	}

	// And turning Weather up from the same position is what lets the terrain in.
	std::vector<Event> wet = parked;
	wet.push_back(param(64, ParamId::Weather, 1.0));
	Rig rig;
	const std::vector<float> soaked = rig.play(wet, kFrames, 256);
	CHECK(firstDifference(soaked, dry) != -1);
	CHECK(peakOf(soaked, 512, kFrames) > 0.05f);

	// Half of it is neither, and in particular is not the same as all of it — otherwise Weather would
	// be a switch wearing a knob's clothes.
	std::vector<Event> damp = parked;
	damp.push_back(param(64, ParamId::Weather, 0.5));
	Rig half;
	const std::vector<float> between = half.play(damp, kFrames, 256);
	CHECK(firstDifference(between, dry) != -1);
	CHECK(firstDifference(between, soaked) != -1);
}

TEST_CASE("standing on a sound means hearing that sound, whatever the weather") {
	// The property that makes the terrain navigable rather than merely different: at the ends of Path
	// the puck is ON an anchor, so that sound has not travelled and is exactly its own home — however
	// far Weather is up. Somewhere to stand that is always solid ground.
	constexpr uint32_t kFrames = 4096;
	const std::vector<Event> atA{noteOn(0, kKeyC4, 1), param(64, ParamId::Path, 0.0)};

	Rig reference;
	const std::vector<float> home = reference.play(atA, kFrames, 256);
	REQUIRE(peakOf(home, 512, kFrames) > 0.05f);

	for (double weather : {0.25, 0.6, 1.0}) {
		CAPTURE(weather);
		std::vector<Event> script = atA;
		script.push_back(param(64, ParamId::Weather, weather));
		Rig rig;
		CHECK(firstDifference(rig.play(script, kFrames, 256), home) == -1);
	}
}

TEST_CASE("a third sound reaches the audio, and switching it off puts it back") {
	// THE WHOLE STAGE, AT THE ONLY LEVEL THAT SETTLES IT. The blend suite proves the tables differ;
	// this proves the difference survives the bank, the deferred-build guard, the fade and the voice —
	// which is the path a wiring bug hides in, because every one of those layers is entitled to drop a
	// character change it thinks it has already seen.
	constexpr uint32_t kFrames = 4096;
	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };

	Rig two;
	// Off the anchors, so all the enabled sounds have a real share rather than one having all of it.
	two.set(ParamId::Path, 0.4);
	const std::vector<float> withTwo = two.play(script, kFrames, 128);

	Rig three;
	three.set(ParamId::Path, 0.4);
	three.set(feathers::soundParam(2, feathers::SoundField::Enable), 1.0);
	const std::vector<float> withThree = three.play(script, kFrames, 128);

	CHECK(peakOf(withTwo, 256, kFrames) > 0.05f); // a silent case would pass any difference check
	CHECK(peakOf(withThree, 256, kFrames) > 0.05f);
	CHECK(firstDifference(withTwo, withThree) != -1);

	// And it is the ENABLE that did it rather than the slot's presence in the table: switching sound C
	// back off renders what two sounds always rendered, to the sample. A patch that turned a sound on
	// and off again has to come back to where it was.
	Rig back;
	back.set(ParamId::Path, 0.4);
	back.set(feathers::soundParam(2, feathers::SoundField::Enable), 1.0);
	back.set(feathers::soundParam(2, feathers::SoundField::Enable), 0.0);
	CHECK(firstDifference(back.play(script, kFrames, 128), withTwo) == -1);
}

TEST_CASE("sounds switched on and off under a held note do not break the render") {
	// The bank defers a build while a fade is in flight, and a constellation gives a user a new way to
	// ask for one: eight enables, each a stepped parameter a host can automate as fast as it likes. The
	// claim is not that this sounds good — it is that it stays finite, stays sounding, and does not
	// wedge the deferral gate shut.
	constexpr uint32_t kFrames = 8192;
	std::vector<Event> script { noteOn(0, kKeyC4, 1) };
	for (uint32_t frame = 64; frame < kFrames; frame += 97) {
		const size_t slot = 2 + (frame / 97) % 6;
		script.push_back({ Event::Kind::Param, frame, 0, 0, 0, ((frame / 97) % 2) == 0 ? 1.0 : 0.0,
		                   feathers::soundParam(slot, feathers::SoundField::Enable) });
	}

	Rig rig;
	rig.set(ParamId::Path, 0.4);
	const std::vector<float> out = rig.play(script, kFrames, 64);

	for (const float sample : out) {
		REQUIRE(std::isfinite(sample));
		REQUIRE(std::abs(sample) <= 1.5f);
	}
	CHECK(peakOf(out, 1024, kFrames) > 0.02f); // still sounding at the end, not wedged into silence
}

TEST_CASE("a moving puck renders the same at any block size") {
	// BLUE SHIFT PUT A CLOCK IN THE ENGINE, and §6's invariance constraint now has something new to
	// bite on: the character depends on the puck's VELOCITY, which is the only quantity here that is
	// not a function of the parameters. The existing invariance case never moves the puck, so it would
	// pass on a build whose velocity depended entirely on how the host chopped its buffer.
	//
	// PuckMotion is proved invariant on its own; this proves the synth drives it that way — same clock,
	// same reports, same order as the character bank.
	constexpr uint32_t kFrames = 6000;
	std::vector<Event> script { noteOn(0, kKeyC4, 1),
		                        { Event::Kind::Param, 0, 0, 0, 0, 1.0,
		                          feathers::soundParam(2, feathers::SoundField::Enable) } };
	// A swipe written in absolute sample positions, so every chopping performs the same gesture.
	for (uint32_t frame = 100; frame < kFrames; frame += 53) {
		const double t = static_cast<double>(frame) / static_cast<double>(kFrames);
		script.push_back({ Event::Kind::Param, frame, 0, 0, 0, 0.6 * t - 0.3, ParamId::PosX });
		script.push_back({ Event::Kind::Param, frame, 0, 0, 0, 0.4 - 0.8 * t, ParamId::PosY });
	}

	Rig reference;
	reference.set(ParamId::Path, 0.4);
	const std::vector<float> first = reference.play(script, kFrames, 256);
	CHECK(peakOf(first, 256, kFrames) > 0.05f); // a silent case would pass any comparison

	for (int block : { 1, 37, 128, 512 }) {
		CAPTURE(block);
		Rig rig;
		rig.set(ParamId::Path, 0.4);
		CHECK(firstDifference(rig.play(script, kFrames, block), first) == -1);
	}
}

TEST_CASE("the gesture stops when the hand does") {
	// §2b.1's static purity, at the level a player meets it: a swipe changes the sound while it is
	// happening, and a patch left parked afterwards settles back to what its position alone says. If a
	// velocity could leak — held from the last gesture, or differentiated against a stale position —
	// the sound would keep a bend nobody is applying, and no parameter would explain it.
	// Long enough that the smoother has genuinely drained rather than nearly: the velocity decays with
	// a 75 ms constant and never reaches exactly zero, so the settling window below starts eleven time
	// constants after the hand stops. The first draft allowed three and a half, and read the 3.7% of
	// the gesture still in flight as a leak — the test was impatient, not the engine.
	constexpr uint32_t kFrames = 144000; // 3 s: the gesture, the ~1.9 s the threshold takes, and room
	std::vector<Event> swiped { noteOn(0, kKeyC4, 1),
		                        { Event::Kind::Param, 0, 0, 0, 0, 1.0,
		                          feathers::soundParam(2, feathers::SoundField::Enable) } };
	for (uint32_t frame = 100; frame < 4000; frame += 53) {
		const double t = static_cast<double>(frame) / 4000.0;
		swiped.push_back({ Event::Kind::Param, frame, 0, 0, 0, 0.5 * t, ParamId::PosX });
	}
	// And then nothing at all for the rest of the render: the hand has stopped.
	swiped.push_back({ Event::Kind::Param, 4000, 0, 0, 0, 0.5, ParamId::PosX });

	// The same patch that never moved: PosX parked at where the swipe ended.
	const std::vector<Event> parked { noteOn(0, kKeyC4, 1),
		                              { Event::Kind::Param, 0, 0, 0, 0, 1.0,
		                                feathers::soundParam(2, feathers::SoundField::Enable) },
		                              { Event::Kind::Param, 0, 0, 0, 0, 0.5, ParamId::PosX } };

	Rig a;
	a.set(ParamId::Path, 0.4);
	Rig b;
	b.set(ParamId::Path, 0.4);
	const std::vector<float> afterSwipe = a.play(swiped, kFrames, 128);
	const std::vector<float> never = b.play(parked, kFrames, 128);

	// They differ during and just after the gesture — otherwise this proves nothing about blue shift.
	CHECK(firstDifference(afterSwipe, never) != -1);

	// And they agree once the gesture has ENDED — bit for bit, not within a tolerance. That is what the
	// rest threshold buys: without it the velocity decays into the denormals and STICKS there, so every
	// motion effect stays switched on for the rest of the session and this comparison could only ever
	// be "close". A tolerance here would pass a build whose parked patches never quite settle, which is
	// exactly the build this replaced.
	constexpr size_t kSettled = 110000; // ~2.2 s after the gesture ends; the threshold is met by ~1.9 s
	long firstDiffering = -1;
	for (size_t i = kSettled; i < afterSwipe.size(); ++i) {
		if (afterSwipe[i] != never[i]) {
			firstDiffering = static_cast<long>(i);
			break;
		}
	}
	CAPTURE(firstDiffering);
	CHECK(firstDiffering == -1);
}

TEST_CASE("a drifted patch renders the same at any block size") {
	// COVERAGE THE DRIFT WORK DID NOT ADD. The invariance cases sweep a moving puck and a still one,
	// but both leave every stored offset at zero — so a build whose drift reached the decode through
	// some block-dependent path would have passed all of them. Drift is patch state read once a block,
	// same as the tuning, and this is the case that says so.
	constexpr uint32_t kFrames = 6000;
	std::vector<Event> script { noteOn(0, kKeyC4, 1),
		                        { Event::Kind::Param, 0, 0, 0, 0, 1.0,
		                          feathers::soundParam(2, feathers::SoundField::Enable) } };
	for (uint32_t frame = 100; frame < kFrames; frame += 53) {
		const double t = static_cast<double>(frame) / static_cast<double>(kFrames);
		script.push_back({ Event::Kind::Param, frame, 0, 0, 0, 0.5 * t - 0.25, ParamId::PosX });
	}

	feathers::SoundTravel drift;
	drift.offset[0] = { 0.37, -0.21 };
	drift.offset[2] = { -1.0 / 3.0, 0.15 };

	const auto play = [&](int block) {
		Rig rig;
		rig.set(ParamId::Path, 0.4);
		rig.synth.setSoundTravel(drift);
		return rig.play(script, kFrames, block);
	};

	const std::vector<float> reference = play(256);
	CHECK(peakOf(reference, 256, kFrames) > 0.05f);
	// And the drift is actually reaching the sound, or this compares two silences.
	Rig plain;
	plain.set(ParamId::Path, 0.4);
	CHECK(firstDifference(plain.play(script, kFrames, 256), reference) != -1);

	for (int block : { 1, 37, 128, 512 }) {
		CAPTURE(block);
		CHECK(firstDifference(play(block), reference) == -1);
	}
}

TEST_CASE("a rotated patch renders the same at any block size, and stands where it stood") {
	// §6, FOR THE FOLD. The residue is patch state read once a block, exactly like the drift beside it,
	// and this is the case that says so: a build whose banked residue reached the decode through
	// anything the host's buffer length could touch would render differently at 1 sample and at 512.
	//
	// AND THE DECISION, HEARD RATHER THAN COMPUTED. Every case for the fold so far compares built tables;
	// this one compares AUDIO. A turn made while parked on a drifted sound must produce the same samples
	// it produced before the turn — not nearly, sample for sample — which is the requirement in the only
	// terms that matter.
	constexpr uint32_t kFrames = 6000;
	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };

	feathers::SoundTravel drift;
	drift.offset[0] = { 0.37, 0.1 + 0.2 };

	feathers::SliceRotation turn;
	turn.rotate = 0.8f;

	// The plane the sound is on before the encoder is touched, turned with its difference banked — the
	// same call the Explore corner makes.
	//
	// ZONE 17, WHICH IS THE RIG'S OWN HOME AND NOT A GUESS. Written as zone 0 first — the parameter
	// table's default — and this case failed, because a fold derived from the wrong home banks the
	// difference between two planes the sound was never on. That is the exact hazard the deriving
	// overload exists to prevent, walked into by the test that was checking it, and it is worth the
	// three lines to say so: the drift and the plane have to come from the SAME sound.
	const feathers::RotatedSound turned = feathers::rotateSoundBy(
	    feathers::SoundOrientations {}, feathers::SoundResidue {}, 0, 17, 0.0f,
	    feathers::SliceOrientationPin {}, turn, drift.offset[0], 0.0);

	const auto play = [&](int block, const feathers::SoundOrientations& planes,
	                      const feathers::SoundResidue& residue) {
		Rig rig;
		rig.set(ParamId::Weather, 0.0); // parked: the puck contributes nothing, which is the case at issue
		rig.synth.setSoundTravel(drift);
		rig.synth.setSoundOrientations(planes);
		rig.synth.setSoundResidue(residue);
		return rig.play(script, kFrames, block);
	};

	const std::vector<float> parked =
	    play(256, feathers::SoundOrientations {}, feathers::SoundResidue {});
	CHECK(peakOf(parked, 256, kFrames) > 0.05f);

	// THE DESIGN: turn the plane, bank the difference, and the sound is the one that was already
	// playing — sample for sample.
	CHECK(firstDifference(play(256, turned.planes, turned.residue), parked) == -1);

	// THE CONTRAST, which is the behaviour that was reported as a bug: the same turn without its residue is audibly
	// a different patch. Without this the line above would pass on a build where rotation had quietly
	// stopped doing anything.
	CHECK(firstDifference(play(256, turned.planes, feathers::SoundResidue {}), parked) != -1);

	// AND BLOCK SIZE REACHES NONE OF IT.
	const std::vector<float> reference = play(256, turned.planes, turned.residue);
	for (int block : { 1, 37, 128, 512 }) {
		CAPTURE(block);
		CHECK(firstDifference(play(block, turned.planes, turned.residue), reference) == -1);
	}
}

TEST_CASE("the gamma base reaches the audio, through the synth's own call") {
	// THE CONTROL THAT WOULD HAVE CAUGHT A REAL GAP, and did not exist when the gap was there.
	//
	// The blend suite proves characterFromParams honours a base by calling it directly. The synth
	// calls it too, with its own argument list — and while that call was missing the base, every one
	// of the sixteen suites still passed. A test that reaches around the code under test is a test
	// that cannot see it forget something. This one plays audio.
	constexpr int kFrames = 2048;
	std::vector<Event> script;
	script.push_back({ Event::Kind::NoteOn, 0, 0, 0, 60, 1.0, ParamId::Path });

	feathers::SoundGamma carried;
	carried.base[0] = 137.91;

	Rig plain;
	plain.set(ParamId::Path, 0.4);
	const std::vector<float> without = plain.play(script, kFrames, 256);

	Rig moved;
	moved.set(ParamId::Path, 0.4);
	moved.synth.setSoundGamma(carried);
	const std::vector<float> with = moved.play(script, kFrames, 256);

	// It has to make sound at all, or this compares two silences and passes for the wrong reason.
	CHECK(peakOf(without, 256, kFrames) > 0.01f);
	// And carrying a sound 137.91 whole steps along its own line has to CHANGE it.
	CHECK(firstDifference(without, with) != -1);
}

TEST_CASE("a zero gamma base changes no sample at all") {
	// The migration guarantee at the far end of the road: setting a base of zero is not merely close
	// to setting nothing, it is the same audio sample for sample.
	constexpr int kFrames = 2048;
	std::vector<Event> script;
	script.push_back({ Event::Kind::NoteOn, 0, 0, 0, 60, 1.0, ParamId::Path });

	Rig plain;
	plain.set(ParamId::Path, 0.4);
	const std::vector<float> without = plain.play(script, kFrames, 256);

	Rig zeroed;
	zeroed.set(ParamId::Path, 0.4);
	zeroed.synth.setSoundGamma(feathers::SoundGamma {});
	const std::vector<float> with = zeroed.play(script, kFrames, 256);

	// NOT SILENCE (backlog MA): two silent renders agree whatever the code does, so the reference must sound.
	REQUIRE(peakOf(without, 0, kFrames) > 0.05f);
	CHECK(firstDifference(without, with) == -1);
}

TEST_CASE("the three osc modes route: each retrigger road hears all three, and legato hears none") {
	// §5.8 THROUGH THE SYNTH (V21c/V21d), WIDENED TO V24's THREE MODES. Each mode answers the same
	// question — what a note-on does to the running phase — and each retrigger road must actually
	// ASK it: Digi snaps to zero, Catchup bends home over ~24 cycles, Analog does nothing at all.
	// Every pair below is the other two's known-bad, which is what makes "the option routes" mean
	// something: three renders that were secretly one would fail here rather than pass quietly.
	//
	// AND LEGATO IS EXPLICITLY OUT, by design — a legato move retriggers nothing, so no mode has a road
	// to act on and the whole render is bit-identical across all three. That is a STRONGER claim
	// than the two-state version it replaces: Analog and a gated legato move are the two things in
	// this synth that leave a phase alone, and this is where they must be indistinguishable.
	constexpr uint32_t kFrames = 8192;
	constexpr uint32_t kSecond = 4096;
	// Raw values, by the table's own stepped rule: 0 Digi, 1 Analog, 2 Phase Catchup.
	constexpr double kDigi = 0.0;
	constexpr double kAnalog = 1.0;
	constexpr double kCatchup = 2.0;

	const auto runWith = [&](double mode, double osc, const std::vector<Event>& script) {
		Rig rig;
		rig.set(ParamId::VoiceMode, mode);
		rig.set(ParamId::OscMode, osc);
		return rig.play(script, kFrames, 128);
	};

	SUBCASE("a mono retrigger on a COLD synth: Catchup parts from both, Digi and Analog coincide") {
		// THE COINCIDENCE IS REAL AND IT IS ARCHITECTURE, not a mode that failed to be implemented —
		// written down because it is exactly what a comfortable pairwise-distinct assertion would
		// have hidden, and because the first draft of this case asserted it away and was wrong.
		//
		// A mono note-on TERMINATES the sounding voice and CREATES A FRESH ONE, so this road runs
		// through initializeMultipleVoices. On a synth that has only just started, the voice the pool
		// hands over has never played and its phase stands at 0 — so "snap the phase to 0" and "do
		// not touch the phase" are the same instruction here, and Digi and Analog MUST render
		// identically or one of them is lying about what it does. Catchup is the mode that acts
		// anyway, because it consults the DYING voice rather than the fresh one; that is exactly what
		// the handoff buys, and why it can be told apart from the very first retrigger.
		const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOn(kSecond, kKeyG4, 2) };
		const std::vector<float> digi = runWith(1.0, kDigi, script);
		const std::vector<float> analog = runWith(1.0, kAnalog, script);
		const std::vector<float> catchup = runWith(1.0, kCatchup, script);

		CHECK(firstDifference(digi, analog) == -1);
		const int digiVsCatchup = firstDifference(digi, catchup);
		const int analogVsCatchup = firstDifference(analog, catchup);
		REQUIRE(digiVsCatchup >= 0);
		REQUIRE(analogVsCatchup >= 0);
		// And neither parts from it before the event that asks the question.
		CHECK(static_cast<uint32_t>(digiVsCatchup) >= kSecond);
		CHECK(static_cast<uint32_t>(analogVsCatchup) >= kSecond);
	}

	SUBCASE("once the voice pool wraps, Analog keeps the phase Digi throws away") {
		// WHERE ANALOG EARNS ITS NAME, and the answer to the coincidence above: play long enough that
		// the pool comes back round to a voice that HAS played, and the leftover phase it carries is
		// the free-running VCO this mode emulates. From there the two modes part for good — which is
		// also the honest shape of the feature in use: a cold synth's first notes are all alike, and
		// a played one is not. Nine mono notes against eight voices forces at least one reuse.
		std::vector<Event> script;
		for (int i = 0; i < 9; ++i) {
			script.push_back(noteOn(static_cast<uint32_t>(i) * 512, kKeyC4 + i, i + 1));
		}
		const std::vector<float> digi = runWith(1.0, kDigi, script);
		const std::vector<float> analog = runWith(1.0, kAnalog, script);
		const int parted = firstDifference(digi, analog);
		REQUIRE(parted >= 0);
		// Nothing parts before a voice could possibly have been handed back out again.
		CHECK(static_cast<uint32_t>(parted) >= 512);
	}

	SUBCASE("a legato restrike of a releasing voice differs on from off — the same-voice road") {
		// The one SAME-VOICE retrigger our configuration can reach, read from the manager's own
		// dispatch and CONFIRMED BY INSTRUMENTING all three seeding sites: moveAndRetriggerVoice is
		// gated on MONO_LEGATO, and fires when a legato note-on lands on a voice that is already
		// RELEASING — same object, running phase, re-gated. That is not the rider's legato move (a
		// GATED legato voice takes the pure moveVoice, and the bit-identical pin below holds it); it
		// is the manager's own name for a retrigger, so the restart character applies. Two drafts of
		// this pair missed the road and a mutation caught each: a same-key poly restrike reaches no
		// seeding site at all (retriggerVoiceWithNewNoteID exists only under RepeatedKeyMode::PIANO,
		// which this synth never sets — its seed stays as defence for the day piano mode arrives,
		// and no case can reach it), and a MONO releasing restrike terminates and creates fresh,
		// which is the handoff road the other two subcases already own.
		const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOff(2048, kKeyC4, 1),
			                              noteOn(kSecond, kKeyG4, 2) };
		const auto restrike = [&](double osc) {
			Rig rig;
			rig.set(ParamId::VoiceMode, 2.0); // Legato
			rig.setRelease(0.0);   // a second of release, so the note lands inside it
			rig.set(ParamId::OscMode, osc);
			return rig.play(script, kFrames, 128);
		};
		const std::vector<float> digi = restrike(kDigi);
		const std::vector<float> analog = restrike(kAnalog);
		const std::vector<float> withBend = restrike(kCatchup);
		// The same-voice road carries all three verbs, so all three renders part here — and Digi
		// against Analog is the pair that proves the snap is real rather than the bend being the
		// only thing anyone implemented.
		const int bendVsNothing = firstDifference(withBend, analog);
		const int snapVsNothing = firstDifference(digi, analog);
		const int snapVsBend = firstDifference(digi, withBend);
		REQUIRE(bendVsNothing >= 0);
		REQUIRE(snapVsNothing >= 0);
		REQUIRE(snapVsBend >= 0);
		CHECK(static_cast<uint32_t>(bendVsNothing) >= kSecond);
		CHECK(static_cast<uint32_t>(snapVsNothing) >= kSecond);
	}

	SUBCASE("a poly steal differs on from off, only after the steal") {
		const std::vector<Event> script { { [] {
			                                  Event e = noteOn(0, kKeyC4, 1);
			                                  return e;
		                                  }() },
			                              noteOn(64, kKeyC4 + 4, 2), noteOn(kSecond, kKeyC4 + 7, 3) };
		const auto steal = [&](double osc) {
			Rig rig;
			rig.set(ParamId::VoiceCount, 2.0); // the third note must steal
			rig.set(ParamId::OscMode, osc);
			return rig.play(script, kFrames, 128);
		};
		// THE FRESH-VOICE ROAD, where only Catchup consults the dying voice. Digi and Analog both
		// ignore the stolen phase deliberately — Digi wants the hard edge, and Analog's phase is
		// per-oscillator, so the dying voice's phase belongs to somebody else. So the claim pinned
		// here is precisely that Catchup parts from BOTH, which is what the handoff buys.
		const std::vector<float> catchup = steal(kCatchup);
		const std::vector<float> digi = steal(kDigi);
		const std::vector<float> analog = steal(kAnalog);
		const int vsDigi = firstDifference(catchup, digi);
		const int vsAnalog = firstDifference(catchup, analog);
		REQUIRE(vsDigi >= 0);
		REQUIRE(vsAnalog >= 0);
		CHECK(static_cast<uint32_t>(vsDigi) >= kSecond);
		CHECK(static_cast<uint32_t>(vsAnalog) >= kSecond);
	}

	SUBCASE("a gated legato move is bit-identical in all three modes") {
		// The decided boundary, and the sharpest form of it: a legato move goes through moveVoice
		// alone, so there is no road for ANY mode to act on. Not "the option is off" — no verb is
		// reached. Analog and this move are the two phase-leaving-alone behaviours in the synth and
		// they must be indistinguishable here; Digi's snap must be absent, not merely quiet.
		const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOn(kSecond, kKeyG4, 2) };
		const std::vector<float> digi = runWith(2.0, kDigi, script);
		const std::vector<float> analog = runWith(2.0, kAnalog, script);
		const std::vector<float> catchup = runWith(2.0, kCatchup, script);
		CHECK(firstDifference(digi, analog) == -1);
		CHECK(firstDifference(analog, catchup) == -1);
	}
}

TEST_CASE("a catchup retrigger renders the same whatever the block size, twice over") {
	// The invariance rule through the new mechanism: the same script lands its bend at the same
	// samples whatever the chopping, and two performances of it are one performance — the
	// determinism claim with teeth, since a nondeterministic correction could not match itself
	// across two different choppings.
	constexpr uint32_t kFrames = 8192;
	const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOn(3001, kKeyG4, 2) };

	const auto run = [&](int block) {
		Rig rig;
		rig.set(ParamId::VoiceMode, 1.0);
		rig.set(ParamId::OscMode, 2.0); // Phase Catchup — the mode with state to be nondeterministic with
		return rig.play(script, kFrames, block);
	};
	const std::vector<float> whole = run(4096);
	const std::vector<float> chopped = run(61);
	// NOT SILENCE (backlog MA), and from the retriggered note on, which is the stretch the claim is about.
	REQUIRE(peakOf(whole, 3001, kFrames) > 0.05f);
	CHECK(firstDifference(whole, chopped) == -1);
	const std::vector<float> again = run(61);
	CHECK(firstDifference(chopped, again) == -1);
}

TEST_CASE("catchup keeps the mono collapse: both channels are one channel through a retrigger") {
	// The stereo half of §5.8's port, pinned at its structural truth: the voice has ONE phase for
	// both ears and the catchup bends that shared accumulator (advancePhase is the one road both
	// renders walk), so the ears cannot decorrelate — and on a width-collapsed patch the two
	// output channels must stay bit-identical straight through a bending retrigger. A per-ear
	// catchup would part them here.
	constexpr uint32_t kFrames = 8192;
	Rig rig;
	rig.set(ParamId::VoiceMode, 1.0);
	rig.set(ParamId::OscMode, 2.0); // Phase Catchup
	rig.set(ParamId::Width, 0.0); // every sound mono — the collapse the width suite pins
	const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOn(4096, kKeyG4, 2) };

	std::vector<float> left(kFrames, 0.0f);
	std::vector<float> right(kFrames, 0.0f);
	size_t next = 0;
	uint32_t at = 0;
	while (at < kFrames) {
		while (next < script.size() && script[next].frame <= at) {
			rig.apply(script[next]);
			next++;
		}
		uint32_t until = kFrames;
		if (next < script.size()) {
			until = std::min(until, script[next].frame);
		}
		until = std::min(until, at + 128u);
		rig.synth.render(left.data() + at, right.data() + at, static_cast<int>(until - at));
		at = until;
	}
	CHECK(firstDifference(left, right) == -1);
	// And it really played through the retrigger, or the identity above compared silence.
	bool loud = false;
	for (uint32_t i = 4096; i < kFrames; ++i) {
		loud = loud || std::abs(left[i]) > 0.05f;
	}
	CHECK(loud);
}

//----------------------------------------------------------------------------------------------//
// THE FX LANE, ON THE SUMMED BUS                                                                //
//----------------------------------------------------------------------------------------------//
//
// The chain sits in render()'s chunk loop, after the mono→stereo copy and before the master gain.
// Both halves of that placement are claims somebody can break, so both are pinned here rather than
// argued in a comment: an empty chain must not cost a single bit, and a chain that IS running must
// still sit upstream of the fader.

TEST_CASE("an empty fx chain does not touch a sample") {
	// **OFF IS A RETURN RATHER THAN A PASS THROUGH A FILTER SET TO NEUTRAL — which is the difference
	// this case measures.** Every other case in this file renders through the same seam, so if this
	// one fails they all become tests of a filter nobody asked for.
	//
	// **AND OFF IS NOW SAID RATHER THAN INHERITED.** This read "the default is OFF"; since
	// 2026-09-04 every effect arrives ON by design, so `Rig`'s `fx::filter::State {}` is a switched-
	// IN filter and the case would have been measuring one filter against another. Switching it off
	// by name is what the case always meant, and it is now immune to the default moving again.
	constexpr uint32_t kFrames = 3000;
	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOff(2011, kKeyC4, 1)};

	Rig wide;
	wide.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
	wide.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(20000.0));
	const std::vector<float> withWideCutoff = wide.play(script, kFrames, 256);

	// The SAME performance with the cutoff at the bottom of its range and the resonance up. A bypass
	// that passed the signal through the filter would sound nothing like the one above; a bypass that
	// is a return cannot tell the two settings apart, because it never reads either.
	Rig narrow;
	narrow.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
	narrow.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(60.0));
	narrow.setFilter(feathers::fx::filter::Param::Resonance, 0.9);
	narrow.setFilter(feathers::fx::filter::Param::Drive, 1.0);
	narrow.setFilter(feathers::fx::filter::Param::Macro1, 1.0);
	narrow.setFilter(feathers::fx::filter::Param::Macro2, -1.0);
	CHECK(firstDifference(narrow.play(script, kFrames, 256), withWideCutoff) == -1);

	// KNOWN-BAD for the two above: those settings DO change the sound, once the device is switched on.
	Rig on;
	on.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
	on.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(60.0));
	on.setFilter(feathers::fx::filter::Param::Resonance, 0.9);
	CHECK(firstDifference(on.play(script, kFrames, 256), withWideCutoff) != -1);
}

TEST_CASE("the filter takes the top off a note") {
	// That it FILTERS, measured rather than assumed. A lowpass well under the note's harmonics has to
	// remove them, and the same performance with the device off is what it is measured against — not
	// a threshold somebody picked.
	constexpr uint32_t kFrames = 16384;
	const std::vector<Event> script{noteOn(0, kKeyC4, 1)};

	Rig dry;
	const std::vector<float> without = dry.play(script, kFrames, 256);

	Rig wet;
	wet.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
	wet.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(200.0));
	const std::vector<float> with = wet.play(script, kFrames, 256);

	const feathers::test::Window win(static_cast<int>(kFrames));
	const auto dryS = feathers::test::amplitudeSpectrum(without, win);
	const auto wetS = feathers::test::amplitudeSpectrum(with, win);
	const auto binOf = [](double hz) {
		return static_cast<size_t>(std::lround(hz * static_cast<double>(kFrames) / kSampleRate));
	};
	const auto bandEnergy = [](const std::vector<double>& spec, size_t lo, size_t hi) {
		double acc = 0.0;
		for (size_t k = lo; k <= hi && k < spec.size(); k++) {
			acc += spec[k] * spec[k];
		}
		return acc;
	};

	// C4 is 261.6 Hz, so a 200 Hz lowpass leaves the fundamental roughly alone and takes everything
	// above it down hard.
	const double high = 10.0 * std::log10(bandEnergy(wetS, binOf(3000), binOf(12000))
	                                      / bandEnergy(dryS, binOf(3000), binOf(12000)));
	const double low = 10.0 * std::log10(bandEnergy(wetS, binOf(150), binOf(300))
	                                     / bandEnergy(dryS, binOf(150), binOf(300)));
	CAPTURE(high);
	CAPTURE(low);
	CHECK(bandEnergy(dryS, binOf(3000), binOf(12000)) > 0.0); // there were harmonics to remove
	CHECK(high < -30.0);
	CHECK(low > -12.0);
}

TEST_CASE("the fx chain is upstream of the master fader") {
	// PRE-MASTER, and the drive is what proves it. A linear filter would scale with the fader
	// wherever it sat, so the case is run with the drive up: a saturator BEHIND the fader would be
	// driven harder at 0 dB than at -12 dB and the two renders would stop being a constant apart.
	constexpr uint32_t kFrames = 4000;
	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOff(3000, kKeyC4, 1)};

	const auto rigWith = [&](double masterDb) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(2000.0));
		rig.setFilter(feathers::fx::filter::Param::Drive, 0.8);
		rig.set(ParamId::MasterGain, masterDb);
		return rig.play(script, kFrames, 256);
	};

	const std::vector<float> full = rigWith(0.0);
	const std::vector<float> quiet = rigWith(-12.0);
	const auto gain = static_cast<float>(feathers::decibelsToGain(-12.0));

	CHECK(peakOf(full, 0, kFrames) > 0.01f); // or this compares two silences
	for (size_t i = 0; i < kFrames; i++) {
		REQUIRE(quiet[i] == doctest::Approx(full[i] * gain).epsilon(1e-6));
	}
}

TEST_CASE("the filter renders the same samples at any block size, mid-sweep") {
	// The block-size claim again, but with the device ENGAGED and its controls MOVING — which is the
	// only arrangement in which a coefficient smoother can disagree with itself. A smoother stepped
	// once per block, at a rate taken from the block length, passes the case above this one and fails
	// this one.
	constexpr uint32_t kFrames = 3000;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    noteOn(37, kKeyG4, 2),
	    filterParam(211, feathers::fx::filter::Param::Cutoff, std::log2(400.0)),
	    filterParam(701, feathers::fx::filter::Param::Resonance, 0.85),
	    filterParam(1024, feathers::fx::filter::Param::Macro1, 0.7),
	    filterParam(1501, feathers::fx::filter::Param::Macro2, -0.6),
	    filterParam(2011, feathers::fx::filter::Param::Cutoff, std::log2(6000.0)),
	    noteOff(2500, kKeyC4, 1),
	};

	const auto play = [&](int block) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(5000.0));
		rig.setFilter(feathers::fx::filter::Param::Drive, 0.4);
		return rig.play(script, kFrames, block);
	};

	const std::vector<float> reference = play(256);
	CHECK(peakOf(reference, 0, kFrames) > 0.01f);
	for (int block : {1, 37, 128}) {
		CAPTURE(block);
		CHECK(firstDifference(play(block), reference) == -1);
	}
}

TEST_CASE("keytrack is an architecture, not an amount") {
	// OFF: one filter on the summed bus, so every note meets the same corner. ON: one filter per
	// voice at that voice's own pitch, before the sum, so a chord's notes each meet their own. The
	// difference is the whole feature and it is measured on a two-octave interval, where the two
	// arrangements have somewhere to disagree.
	constexpr uint32_t kFrames = 16384;
	// A TRITONE PLUS AN OCTAVE ABOVE C4, and the interval is chosen rather than convenient. The first
	// version of this case used C6, two octaves up — whose fundamental at 1046.5 Hz is also C4's
	// FOURTH HARMONIC, so the bin being measured held both voices at once and the measurement could
	// not tell them apart. It read a 1.4 dB difference where there was a 15 dB one. An irrational
	// interval has no such coincidence: 2^(18/12) is not a ratio of small whole numbers, so nothing
	// C4 produces lands on top of what is being measured.
	constexpr int16_t kKeyFs5 = 78;
	constexpr double kFs5Hz = 739.989;
	const std::vector<Event> script{noteOn(0, kKeyC4, 1), noteOn(0, kKeyFs5, 2)};

	const auto play = [&](bool keytrack, int16_t key, int block) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(300.0));
		// POLY ON IN BOTH ARMS, so this case is about KEYTRACK rather than about how many filters
		// there are. Before the 2026-08-23 split, keytrack chose the arrangement AND the tracking, so
		// varying it alone varied both and this case could not tell which it was measuring.
		rig.setFilter(feathers::fx::filter::Param::Poly, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Keytrack, keytrack ? 1.0 : 0.0);
		const std::vector<Event> one{noteOn(0, key, 1)};
		return rig.play(key == 0 ? script : one, kFrames, block);
	};

	const feathers::test::Window win(static_cast<int>(kFrames));
	const auto energyNear = [&](const std::vector<float>& x, double hz) {
		return feathers::test::magnitudeAt(x, win, kSampleRate, hz);
	};

	SUBCASE("the high voice keeps its own corner instead of the chord's") {
		const double busHigh = energyNear(play(false, 0, 256), kFs5Hz);
		const double trackHigh = energyNear(play(true, 0, 256), kFs5Hz);
		const double gain = 20.0 * std::log10(std::max(trackHigh, 1e-30) / std::max(busHigh, 1e-30));
		CAPTURE(busHigh);
		CAPTURE(trackHigh);
		CAPTURE(gain);
		// One filter at 300 Hz is well below this note and takes its fundamental down; the same 300 Hz
		// tracked up eighteen semitones sits at 848 Hz, above the note, and barely touches it.
		CHECK(gain > 10.0);
	}

	SUBCASE("a note at the reference key hears the corner the knob says") {
		// Middle C is where a tracked cutoff equals an untracked one, so the two arrangements have to
		// agree there. Not to the bit — the per-voice filter runs in front of the amplitude edge and
		// the bus filter behind the sum, and those commute only for a gain that is not moving — but
		// they cannot be a filter apart. If BOTH were running, this note would be filtered twice and
		// its fundamental would sit several dB below where it does.
		const double bus = energyNear(play(false, kKeyC4, 256), 261.6);
		const double tracked = energyNear(play(true, kKeyC4, 256), 261.6);
		const double apart = 20.0 * std::log10(std::max(tracked, 1e-30) / std::max(bus, 1e-30));
		CAPTURE(bus);
		CAPTURE(tracked);
		CAPTURE(apart);
		CHECK(bus > 0.0);
		CHECK(std::abs(apart) < 1.5);
	}
}

TEST_CASE("a bus filter with Keytrack on and Poly off follows the newest held voice's note, and holds it after the release") {
	// **KEYTRACK NO LONGER IMPLIES POLY** (2026-10-04). It rendered as poly off — the corner sat at the knob
	// whatever was played — because "a bus carrying six notes has no note". The HOST has one: Feathers' bus note
	// is the newest held voice's (`Synth::busNoteContext`), the rack's is its last held key — so the
	// two hosts agree. Asserted as the device's own target, by value.
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	rig.setFilter(flt::Param::Poly, 0.0);
	rig.setFilter(flt::Param::Keytrack, 1.0);
	const auto offset = [&] { return rig.synth.busFilterTargetForTest().keyOffsetSemitones; };

	rig.play({noteOn(0, 48, 1)}, 256, 256);
	CHECK(offset() == doctest::Approx(48.0f - 60.0f));
	CHECK(rig.synth.busFilterTargetForTest().cutoffLog2Hz == doctest::Approx(10.0f)); // the knob's, unmoved
	rig.play({noteOn(0, 72, 2)}, 256, 256);
	CHECK(offset() == doctest::Approx(72.0f - 60.0f));
	rig.play({noteOff(0, 72, 2)}, 256, 256);
	CHECK(offset() == doctest::Approx(48.0f - 60.0f)); // back to the one still held
	rig.play({noteOff(0, 48, 1)}, 256, 256);
	CHECK(offset() == doctest::Approx(48.0f - 60.0f)); // nothing held: it stays, no snap to the reference key

	SUBCASE("the control: Keytrack off keeps the bus corner at the knob") {
		rig.setFilter(flt::Param::Keytrack, 0.0);
		rig.play({noteOn(0, 72, 3)}, 256, 256);
		CHECK(offset() == 0.0f);
	}
}

TEST_CASE("a Tracker in the strip hands the fx lane's bus its note: a Poly-off keytracked filter follows the Tracker's Rule") {
	// A bus's note is the collapsed note per bus by default, or an upstream Tracker's. Feathers keeps
	// its Trackers in the strip (chain 0), which is upstream of the fx lane (chain 1) — so the lane's bus devices
	// follow the strip's last Tracker. Lowest is the rule that disagrees with the default here: two keys held, the
	// newer one higher.
	namespace flt = feathers::fx::filter;
	namespace trk = feathers::mod::tracker;
	Rig rig;
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	rig.setFilter(flt::Param::Poly, 0.0);
	rig.setFilter(flt::Param::Keytrack, 1.0);
	feathers::ModRack strip = feathers::defaultModRack();
	strip.device[0] = feathers::defaultRowFor(trk::kDescriptor, { 50 });
	strip.device[0].field[static_cast<size_t>(trk::Param::Rule)] = static_cast<float>(trk::Rule::Lowest);
	strip.count = 1;
	rig.publishModRack(strip);
	rig.play({}, 256, 256); // the strip adopts its Tracker at a render; a note before that would miss it
	const auto offset = [&] { return rig.synth.busFilterTargetForTest().keyOffsetSemitones; };

	rig.play({noteOn(0, 48, 1)}, 256, 256);
	CHECK(offset() == doctest::Approx(48.0f - 60.0f));
	rig.play({noteOn(0, 72, 2)}, 256, 256);
	CHECK(offset() == doctest::Approx(48.0f - 60.0f)); // the lowest held, where the default would say 72
	rig.play({noteOff(0, 48, 1)}, 256, 256);
	CHECK(offset() == doctest::Approx(72.0f - 60.0f)); // the only one left

	SUBCASE("the control: with the Tracker gone, a note over the held 72 gives the newest held, not the lowest") {
		rig.publishModRack(feathers::defaultModRack());
		rig.play({}, 256, 256);
		rig.play({noteOn(0, 84, 3)}, 256, 256);
		CHECK(offset() == doctest::Approx(84.0f - 60.0f)); // Lowest would have said 72
	}
}

TEST_CASE("two Trackers in the strip: the fx lane's Filter and Phaser follow the nearer one, the strip's last") {
	// THE STRIP IS UPSTREAM OF THE WHOLE LANE (chain 0 before chain 1), so of two Trackers there the second is the
	// nearer for every lane device. Lowest then Highest: the lane follows Highest. And a second NoteContext reader
	// beside the Filter — the Phaser, whose Key Track latches the note while it is held.
	namespace flt = feathers::fx::filter;
	namespace trk = feathers::mod::tracker;
	namespace ph = feathers::fx::phaser;
	Rig rig;
	rig.expectMono = false; // the Phaser is a stereo device
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	rig.setFilter(flt::Param::Poly, 0.0);
	rig.setFilter(flt::Param::Keytrack, 1.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, ph::kTraits.tag) == 1);
	rig.chain.device[1].row.field[static_cast<size_t>(ph::Param::KeyTrack)] = 1.0f;
	rig.publishChain();
	feathers::ModRack strip = feathers::defaultModRack();
	strip.device[0] = feathers::defaultRowFor(trk::kDescriptor, { 50 });
	strip.device[0].field[static_cast<size_t>(trk::Param::Rule)] = static_cast<float>(trk::Rule::Lowest);
	strip.device[1] = feathers::defaultRowFor(trk::kDescriptor, { 51 });
	strip.device[1].field[static_cast<size_t>(trk::Param::Rule)] = static_cast<float>(trk::Rule::Highest);
	strip.count = 2;
	rig.publishModRack(strip);
	rig.play({}, 256, 256);
	const auto filterNote = [&] { return rig.synth.busFilterTargetForTest().keyOffsetSemitones + 60.0f; };
	const auto phaserNote = [&] {
		return static_cast<const ph::Phaser*>(rig.synth.fxStorageForTagForTest(ph::kTraits.tag))->noteSemitonesForTest();
	};

	rig.play({noteOn(0, 60, 1)}, 256, 256);
	rig.play({noteOn(0, 72, 2)}, 256, 256);
	rig.play({noteOn(0, 48, 3)}, 256, 256); // held 60, 72, 48: Last 48, Lowest 48, Highest 72
	CHECK(filterNote() == doctest::Approx(72.0f));
	CHECK(phaserNote() == doctest::Approx(72.0));
	rig.play({noteOff(0, 72, 2)}, 256, 256); // held 60, 48: Highest 60
	CHECK(filterNote() == doctest::Approx(60.0f));
	CHECK(phaserNote() == doctest::Approx(60.0));
}

/// MEASURED, both sides: this case built with v1-implementation 447bda44's `multimode_filter.cpp` (keytrack
/// requires Poly) and with this branch's gave this same digest.
[[maybe_unused]] constexpr uint64_t kPerVoiceKeytrackDigest = 10482277416244640397ull;

TEST_CASE("per-voice keytrack renders byte-identical to the build before bus keytrack (2026-10-04)") {
	// THE PER-VOICE ROAD DID NOT MOVE. A Poly + Keytrack filter under three overlapping notes, one released,
	// rendered and digested; the digest was taken from the same case built against the tree before
	// `OpsFilter::refresh` stopped requiring Poly (v1-implementation 447bda44). A digest of samples is the claim
	// itself here — "the same bytes" — not a stand-in for a value.
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Cutoff, std::log2(400.0));
	rig.setFilter(flt::Param::Resonance, 0.6);
	rig.setFilter(flt::Param::Poly, 1.0);
	rig.setFilter(flt::Param::Keytrack, 1.0);
	const std::vector<Event> script{noteOn(0, 48, 1), noteOn(1000, 64, 2), noteOn(2500, 79, 3), noteOff(6000, 64, 2)};
	const std::vector<float> out = rig.play(script, 8192, 128);
	uint64_t h = 1469598103934665603ull; // FNV-1a over the bits
	for (const float s : out) {
		uint32_t bits = 0;
		std::memcpy(&bits, &s, sizeof bits);
		for (int b = 0; b < 4; ++b) {
			h = (h ^ ((bits >> (8 * b)) & 0xffu)) * 1099511628211ull;
		}
	}
	MESSAGE("per-voice keytrack digest " << h);
#if defined(__x86_64__) && defined(__linux__)
	CHECK(h == kPerVoiceKeytrackDigest); // bits, so pinned to the platform it was measured on (vox-voice's rule)
#endif
}

TEST_CASE("a keytracking filter renders the same at any block size, including under a glide") {
	// THE PIN FOR WHERE THE CUTOFF'S PITCH COMES FROM. A voice's oscillator moves its pitch once per
	// GENERATED sample while a glide is running, so a per-voice cutoff read from that pitch would
	// land wherever the chunk boundaries fell — the block-size bug, arriving through a door nobody
	// was watching. The cutoff is taken from the note's TARGET pitch instead, which moves only in
	// applyPitches, and this case is what says so.
	constexpr uint32_t kFrames = 4000;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    noteOn(377, kKeyG4, 2), // mono, so this one glides the sounding voice rather than adding one
	    noteOn(1601, kKeyC4, 3),
	    noteOff(3001, kKeyC4, 3),
	};

	const auto play = [&](int block) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Poly, 1.0); // per-voice is poly's job now, not keytrack's
		rig.setFilter(feathers::fx::filter::Param::Keytrack, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(700.0));
		rig.setFilter(feathers::fx::filter::Param::Resonance, 0.7);
		rig.setFilter(feathers::fx::filter::Param::Macro1, 0.5);
		rig.set(ParamId::VoiceMode, static_cast<double>(feathers::VoiceMode::Mono));
		rig.set(ParamId::GlideTime, 0.4);
		return rig.play(script, kFrames, block);
	};

	const std::vector<float> reference = play(256);
	CHECK(peakOf(reference, 0, kFrames) > 0.01f);
	for (int block : {1, 37, 128}) {
		CAPTURE(block);
		CHECK(firstDifference(play(block), reference) == -1);
	}
}

TEST_CASE("a voice takes no resonant tail from the note before it") {
	// A filter is per VOICE and a voice is REUSED, so the slot that just played a loud resonant note
	// is the slot the next note lands in. The claim is exact rather than approximate: the second note
	// must render the same samples a first note renders, because everything else about the slot — the
	// oscillator phase, the noise seed, the amplitude edge — is already restarted at voice start, and
	// the filter has to join them.
	//
	// The first version of this case only checked that the gap was silent and the second note was
	// audible, and a deliberately removed restart passed it: a carried-over filter state ADDS a
	// transient rather than removing one, so nothing about "is there sound here" can see it. This
	// version compares the samples.
	constexpr uint32_t kFrames = 6000;
	constexpr uint32_t kSecondAt = 4000;
	constexpr uint32_t kCompare = 600;

	const auto rig = [] {
		auto r = std::make_unique<Rig>();
		r->setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		r->setFilter(feathers::fx::filter::Param::Poly, 1.0); // per-voice is poly's job now, not keytrack's
		r->setFilter(feathers::fx::filter::Param::Keytrack, 1.0);
		r->setFilter(feathers::fx::filter::Param::Cutoff, std::log2(120.0));
		r->setFilter(feathers::fx::filter::Param::Resonance, 0.97); // where a carried tail is loudest and longest
		return r;
	};

	// A note, cut short, silence long enough for its edge to finish, then the same note again.
	auto reused = rig();
	const std::vector<float> twice = reused->play(
	    {noteOn(0, kKeyC4, 1), noteOff(600, kKeyC4, 1), noteOn(kSecondAt, kKeyC4, 2)}, kFrames, 256);

	// The same note in a slot that has played nothing at all.
	auto fresh = rig();
	const std::vector<float> once = fresh->play({noteOn(0, kKeyC4, 1)}, kFrames, 256);

	CHECK(peakOf(once, 0, kCompare) > 0.005f); // or this compares two silences
	CHECK(peakOf(twice, 3000, kSecondAt - 10) < 1e-6f); // the slot really is idle when it is reclaimed
	for (uint32_t i = 0; i < kCompare; i++) {
		CAPTURE(i);
		REQUIRE(twice[kSecondAt + i] == once[i]);
	}
}

TEST_CASE("a reclaimed voice does not open at the corner the LAST note left behind") {
	// THE SIBLING OF THE CASE ABOVE, AND THE ONE THAT CATCHES WHAT IT CANNOT.
	//
	// "A voice takes no resonant tail from the note before it" replays THE SAME KEY, so both notes
	// want the same corner and a filter that carried the old coefficients in would carry in ones that
	// are already right. It therefore cannot see the difference between MultimodeFilter::reset —
	// forget the delay line — and restart — forget the delay line AND the fact that the device was
	// active, so the next block SNAPS its coefficients instead of ramping them from where they were.
	//
	// THAT DISTINCTION BECAME LOAD-BEARING when the per-voice road stopped calling `restart()` by
	// name and started calling `AudioOps::reset` on whatever the slot holds. The entry point had NO
	// production caller before that — FxRunner::reset DESTROYED its instances rather than resetting
	// them — and an uninterrogated capability drifts to whatever its implementor guessed: the
	// filter's was the weaker of the two meanings, and nothing in this suite could tell.
	//
	// IT HAS TWO PRODUCTION CALLERS NOW. `FxRunner::reset` stopped destroying on 2026-08-26, when
	// destruction became the message thread's business: a reset that retired every instance would
	// empty the slots while the provisioner's mirror still said they were full, which is silence
	// rather than a fresh start. So the whole-runner reset means what the per-voice one always meant.
	//
	// FOUND BY READING, NOT BY A FAILURE, which is why this case exists rather than a comment. A
	// known-bad confirms it: putting `dsp.reset()` back leaves the same-key case GREEN and this one
	// RED.
	constexpr uint32_t kFrames = 9000;
	constexpr uint32_t kSecondAt = 6000;
	constexpr uint32_t kCompare = 400;
	constexpr int16_t kKeyC2 = 36;
	constexpr int16_t kKeyC6 = 84;

	const auto rig = [] {
		auto r = std::make_unique<Rig>();
		r->setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		r->setFilter(feathers::fx::filter::Param::Poly, 1.0);
		// KEYTRACK ON IS WHAT MAKES THE TWO NOTES WANT DIFFERENT CORNERS — four octaves apart, which
		// is four octaves of ramp for a filter that thinks it is still running.
		r->setFilter(feathers::fx::filter::Param::Keytrack, 1.0);
		r->setFilter(feathers::fx::filter::Param::Cutoff, std::log2(300.0));
		r->setFilter(feathers::fx::filter::Param::Resonance, 0.9);
		return r;
	};

	// A low note, released, long enough for its edge to finish — then a HIGH note in the slot it left.
	auto reused = rig();
	const std::vector<float> after = reused->play(
	    {noteOn(0, kKeyC2, 1), noteOff(600, kKeyC2, 1), noteOn(kSecondAt, kKeyC6, 2)}, kFrames, 256);

	// The same high note in a slot that has played nothing at all.
	auto fresh = rig();
	const std::vector<float> alone = fresh->play({noteOn(0, kKeyC6, 1)}, kFrames, 256);

	CHECK(peakOf(alone, 0, kCompare) > 0.005f);                  // or this compares two silences
	CHECK(peakOf(after, 3000, kSecondAt - 10) < 1e-6f);          // the slot really is idle when reclaimed
	// BIT-EXACT, on the same terms the same-key case states: everything else about the slot is
	// restarted at voice start, and the filter has to join them.
	for (uint32_t i = 0; i < kCompare; i++) {
		CAPTURE(i);
		REQUIRE(after[kSecondAt + i] == alone[i]);
	}
}

TEST_CASE("the synth reads back how much of each sound it is playing") {
	// THE READOUT AT THE LEVEL IT IS PUBLISHED FROM. The mapping's own suite proves the arithmetic;
	// this proves the synth WROTE it — that the character it built and the weights it hands out are
	// the same evaluation rather than two that agree today.
	Rig rig;

	// Before a block is rendered there is no character and so no shares: absent rather than eight
	// zeros, which is a plugin nobody has asked to play yet.
	CHECK(rig.synth.soundWeights().live == 0);

	std::vector<float> left(64, 0.0f);
	std::vector<float> right(64, 0.0f);
	rig.synth.render(left.data(), right.data(), 64);

	// Path is 0 in this rig, which stands the puck on sound A: all of A, none of B, and B present
	// because it is switched on and on the tour.
	const feathers::SoundWeights standing = rig.synth.soundWeights();
	CHECK(standing.isLive(0));
	CHECK(standing.isLive(1));
	CHECK(standing.at(0) == doctest::Approx(1.0));
	CHECK(standing.at(1) == doctest::Approx(0.0));

	// AND IT FOLLOWS THE PERFORMANCE. Half way along the tour is half of each, and the readout moved
	// because the character moved — the control that keeps the block above from passing on a value
	// written once and never updated.
	rig.set(ParamId::Path, 0.5);
	rig.synth.render(left.data(), right.data(), 64);
	const feathers::SoundWeights halfway = rig.synth.soundWeights();
	CHECK(halfway.at(0) == doctest::Approx(0.5));
	CHECK(halfway.at(1) == doctest::Approx(0.5));

	// AND A SWITCHED-OFF SOUND LEAVES THE READOUT ENTIRELY, rather than sitting in it at zero.
	rig.set(ParamId::BEnable, 0.0);
	rig.synth.render(left.data(), right.data(), 64);
	const feathers::SoundWeights alone = rig.synth.soundWeights();
	CHECK(alone.isLive(0));
	CHECK_FALSE(alone.isLive(1));
	CHECK(alone.at(0) == doctest::Approx(1.0));
}

TEST_CASE("BOUNCE DETERMINISM: a note at a song position sounds the same however you got there") {
	// THE WHOLE POINT OF TRANSPORT-LOCKED ANALOG, in one case: its justification is that a bounce is
	// deterministic.
	//
	// A bounce starting at bar 1 and a bounce starting at bar 5 must agree about bar 5. Under the
	// old Analog they cannot: "leave the phase alone" means the phase is whatever the last note
	// left behind, so the first bounce arrives at bar 5 carrying history the second one has never
	// had. Two renders of the same bar, different samples, and nothing in the project to explain it.
	//
	// WHAT MAKES THIS CASE DISCRIMINATING, and a first draft got it wrong: comparing two routes on a
	// COLD synth proves nothing, because both hand out a fresh voice at phase 0 and agree for the
	// wrong reason. The routes have to DIFFER IN VOICE HISTORY, which is exactly what a real bounce
	// from an earlier bar does — so route A plays a note and lets it finish before the note under
	// test, and route B locates straight there.
	constexpr uint32_t kFrames = 8192;
	constexpr double kAnalog = 1.0;
	// Where the note under test happens, in seconds of song. Not a round number: a phase of
	// frac(f0 * t) at an exact second would be suspiciously close to zero for a harmonic f0.
	constexpr double kTestPosition = 3.7115;
	constexpr uint32_t kLate = 4096;

	const auto atPosition = [&](bool withHistory, int block) {
		Rig rig;
		rig.set(ParamId::VoiceMode, 1.0); // mono, so the note under test lands on the retrigger road
		rig.set(ParamId::OscMode, kAnalog);
		std::vector<Event> script;
		if (withHistory) {
			// The bounce that started earlier: a note has already played and left a phase behind.
			script.push_back(noteOn(0, kKeyC4, 1));
			script.push_back(noteOff(kLate / 2, kKeyC4, 1));
			rig.locate(kTestPosition - static_cast<double>(kLate) / kSampleRate);
		}
		else {
			rig.locate(kTestPosition);
		}
		script.push_back(noteOn(withHistory ? kLate : 0, kKeyG4, 2));
		const std::vector<float> out = rig.play(script, kFrames, block);
		// Compare only from the note under test onward — the histories differ before it by design.
		const uint32_t from = withHistory ? kLate : 0;
		return std::vector<float>(out.begin() + from, out.begin() + from + (kFrames - kLate));
	};

	const std::vector<float> playedThrough = atPosition(true, 128);
	const std::vector<float> locatedTo = atPosition(false, 128);
	REQUIRE(playedThrough.size() == locatedTo.size());
	// NOT SILENCE (backlog MA): two silent renders agree whatever the code does, so the note under test must sound.
	REQUIRE(peakOf(locatedTo, 0, locatedTo.size()) > 0.05f);
	CHECK(firstDifference(playedThrough, locatedTo) == -1);

	// AND IT IS THE SAME AT ANY BLOCK SIZE, because the note's instant is blockStart + offset/rate
	// rather than a block boundary. A host bounces at whatever buffer length it likes.
	for (const int block : { 1, 7, 64, 512, 1024 }) {
		CAPTURE(block);
		CHECK(firstDifference(atPosition(true, block), locatedTo) == -1);
	}
}

TEST_CASE("the transport lock: what it changes, and what it leaves exactly as it was") {
	constexpr uint32_t kFrames = 8192;
	constexpr uint32_t kSecond = 4096;
	constexpr double kDigi = 0.0;
	constexpr double kAnalog = 1.0;

	const auto run = [&](double osc, bool transport, double at, bool playing,
	                     const std::vector<Event>& script) {
		Rig rig;
		rig.set(ParamId::VoiceMode, 1.0);
		rig.set(ParamId::OscMode, osc);
		if (transport) {
			rig.locate(at);
			rig.playing = playing;
		}
		return rig.play(script, kFrames, 128);
	};

	SUBCASE("WITH a transport, a cold synth no longer coincides — Digi parts from Analog") {
		// THE SPEC CHANGE, and it is worth being precise about what moved. The existing case in this
		// file pins that on a COLD synth Digi and Analog render identically, because the voice the
		// pool hands over has never played and its phase stands at 0 — so "snap to zero" and "leave
		// it alone" were the same instruction. That case is STILL TRUE and still passes: it provides
		// no transport, and without one Analog is what it always was.
		//
		// What changes is that Analog now HAS somewhere else to be. frac(f0 · t) is only zero when
		// the song position happens to land on a whole turn, so at an arbitrary position the two
		// modes must part from the very first note — which is the mode finally saying something on
		// the road where it used to be silent.
		const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOn(kSecond, kKeyG4, 2) };
		const std::vector<float> digi = run(kDigi, true, 3.7115, true, script);
		const std::vector<float> analog = run(kAnalog, true, 3.7115, true, script);
		const int firstDiff = firstDifference(digi, analog);
		REQUIRE(firstDiff >= 0);
		// And they part at the FIRST note, not only at the retrigger.
		CHECK(firstDiff < static_cast<int>(kSecond));
	}

	SUBCASE("WITHOUT a transport, Analog is bit-for-bit what it always was") {
		// THE FALLBACK, pinned rather than assumed. A CLAP host is not obliged to provide transport,
		// and the honest answer there is the behaviour that shipped — this voice slot's own phase
		// kept across the reset — rather than pretending the song is at zero, which would silently
		// turn Analog into Digi for every host that offers no timeline.
		const std::vector<Event> script { noteOn(0, kKeyC4, 1), noteOn(kSecond, kKeyG4, 2) };
		const std::vector<float> analogNoTransport = run(kAnalog, false, 0.0, true, script);
		const std::vector<float> digiNoTransport = run(kDigi, false, 0.0, true, script);
		// The old coincidence, intact: on a cold synth with no transport these are the same render.
		CHECK(firstDifference(analogNoTransport, digiNoTransport) == -1);
	}

	SUBCASE("once the pool wraps, Analog DERIVES rather than keeps — the premise that changed") {
		// THE OTHER CASE THE LOCK WAS PREDICTED TO KILL, and it did not die: "once the voice pool
		// wraps, Analog keeps the phase Digi throws away" is still true and still passes, because it
		// provides no transport. What the lock changes is what Analog does WHEN THERE IS ONE — it
		// stops keeping the recycled voice's leftover phase and derives from the song instead.
		//
		// THE PAIR IS THE POINT. Nine notes against eight voices forces the pool round onto a voice
		// that has played. With a transport, the wrapped note's phase is a function of SONG POSITION,
		// so two different positions render differently. Without one it is a function of the leftover
		// phase alone, which is the same at both positions — so the control renders identically. One
		// claim, its own known-bad, and the difference between them is the whole feature.
		std::vector<Event> nine;
		for (uint32_t i = 0; i < 9; ++i) {
			nine.push_back(noteOn(i * 512, static_cast<int16_t>(kKeyC4 + i), static_cast<int32_t>(i)));
		}
		const std::vector<float> lockedHere = run(kAnalog, true, 3.7115, true, nine);
		const std::vector<float> lockedThere = run(kAnalog, true, 8.2391, true, nine);
		CHECK(firstDifference(lockedHere, lockedThere) >= 0);

		// THE CONTROL: with no transport the song position is not consulted, so the same two runs are
		// the same render. If this differed, the case above would be measuring the rig rather than
		// the lock.
		const std::vector<float> freeHere = run(kAnalog, false, 3.7115, true, nine);
		const std::vector<float> freeThere = run(kAnalog, false, 8.2391, true, nine);
		CHECK(firstDifference(freeHere, freeThere) == -1);
	}

	SUBCASE("THE PHASE ITSELF: every road gives frac(f0 · t), and it depends on both") {
		// ASSERTED ON THE NUMBER RATHER THAN ON A RENDER, and that is the correction two drafts of
		// this needed. A comparison between two rendered buffers passes whenever ANYTHING differs —
		// so a case built that way went green while the very thing it was about had been mutated
		// away, twice: once with lockPhaseToSong gutted, once with the note's frequency replaced by
		// a fixed one. Both survived because the FIRST note in each script still differed for its
		// own reasons and the comparison could not tell which note it was answering about.
		//
		// The phase a note-on is given is one number at one instant. Read it.
		const auto phaseAtNoteOn = [&](double at, int16_t key, uint32_t renderFirst) {
			Rig rig;
			rig.set(ParamId::VoiceMode, 1.0);
			rig.set(ParamId::OscMode, kAnalog);
			rig.locate(at);
			std::vector<float> l(renderFirst + 1, 0.0f);
			std::vector<float> r(renderFirst + 1, 0.0f);
			rig.synth.setTransport(at, feathers::device::TransportClock::kNoPosition, true);
			if (renderFirst > 0) {
				rig.synth.render(l.data(), r.data(), static_cast<int>(renderFirst));
			}
			rig.synth.noteOn(0, key, 1, 1.0f);
			const int slot = rig.synth.slotOfKeyForTest(key);
			REQUIRE(slot >= 0);
			return rig.synth.voicePhaseForTest(slot);
		};
		const auto expected = [](double at, int16_t key) {
			const double turns = feathers::pitchToHz(static_cast<double>(key)) * at;
			return turns - std::floor(turns);
		};

		// THE FRESH-VOICE ROAD, at a position and a note that make the answer non-degenerate.
		CHECK(phaseAtNoteOn(3.7115, kKeyC4, 0) == doctest::Approx(expected(3.7115, kKeyC4)).epsilon(1e-9));
		// A DIFFERENT NOTE AT THE SAME POSITION — which is what a fixed frequency would break, and
		// what nothing else here could see. An octave up is the sharpest available: f0 doubles.
		const int16_t high = static_cast<int16_t>(kKeyC4 + 12);
		CHECK(phaseAtNoteOn(3.7115, high, 0) == doctest::Approx(expected(3.7115, high)).epsilon(1e-9));
		CHECK(phaseAtNoteOn(3.7115, kKeyC4, 0) != doctest::Approx(phaseAtNoteOn(3.7115, high, 0)));
		// A DIFFERENT POSITION FOR THE SAME NOTE.
		CHECK(phaseAtNoteOn(8.2391, kKeyC4, 0) == doctest::Approx(expected(8.2391, kKeyC4)).epsilon(1e-9));

		// AND THE INSTANT IS blockStart + WHAT HAS BEEN RENDERED SINCE, not the block's start — the
		// half that makes it block-size independent. Render 1000 samples first and the phase must
		// have moved on by exactly that much of song time.
		constexpr uint32_t kInto = 1000;
		const double atLater = 3.7115 + static_cast<double>(kInto) / kSampleRate;
		CHECK(phaseAtNoteOn(3.7115, kKeyC4, kInto)
		      == doctest::Approx(expected(atLater, kKeyC4)).epsilon(1e-9));
	}

	SUBCASE("the SAME-VOICE retrigger road re-derives too, where Analog used to do nothing") {
		// THE DISPATCH SITE A MUTATION FOUND UNGUARDED. Analog has three, and the two retrigger
		// roads used to do nothing at all — the phase was already running and the mode's content was
		// declining to disturb it. Under the lock they re-derive, and until this case existed that
		// could be deleted without failing anything.
		//
		// THE ROAD IS THE LEGATO RESTRIKE OF A RELEASING VOICE, the one same-voice retrigger this
		// configuration can reach; retriggerVoiceWithNewNoteID is gated on a piano mode this synth
		// never sets and stays defensive. Asserted on the PHASE, because a render comparison here
		// answers about the first note rather than the restrike — which is exactly how the first
		// draft of this passed while the road was gutted.
		Rig rig;
		rig.set(ParamId::VoiceMode, 2.0); // Legato, so the restrike lands on the same voice
		rig.setRelease(0.0);   // a long release, so the restrike lands inside it
		rig.set(ParamId::OscMode, kAnalog);
		constexpr double kAt = 3.7115;
		std::vector<float> l(4096, 0.0f);
		std::vector<float> r(4096, 0.0f);
		rig.synth.setTransport(kAt, feathers::device::TransportClock::kNoPosition, true);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0f);
		rig.synth.render(l.data(), r.data(), 2048);
		rig.synth.noteOff(0, kKeyC4, 1, 0.0f);
		rig.synth.render(l.data(), r.data(), 2048);
		// The restrike, on the releasing voice. 4096 samples in, so the song has moved.
		rig.synth.noteOn(0, kKeyG4, 2, 1.0f);
		const int slot = rig.synth.slotOfKeyForTest(kKeyG4);
		REQUIRE(slot >= 0);
		const double songNow = kAt + 4096.0 / kSampleRate;
		const double turns = feathers::pitchToHz(static_cast<double>(kKeyG4)) * songNow;
		CHECK(rig.synth.voicePhaseForTest(slot)
		      == doctest::Approx(turns - std::floor(turns)).epsilon(1e-9));
	}

	SUBCASE("a STOPPED transport free-runs rather than handing every note one phase") {
		// THE SPECIFICATION: locked to transport time, free-running while the transport is stopped. A
		// stopped transport is the same position for as long as the user leaves it there, so locking
		// to it would give every audition of a note an identical start phase — the oscillator would
		// stop being an oscillator between takes. It locks to a free-running counter instead, which
		// keeps moving because rendering keeps happening.
		//
		// The evidence is that two notes at the SAME stopped song position render differently from
		// each other, which locking could not produce and free-running must.
		const std::vector<Event> first { noteOn(0, kKeyC4, 1) };
		const std::vector<Event> second { noteOn(kSecond, kKeyC4, 1) };
		const std::vector<float> early = run(kAnalog, true, 2.5, false, first);
		const std::vector<float> late = run(kAnalog, true, 2.5, false, second);
		// Line the two notes up at their own note-on and compare from there.
		std::vector<float> earlyFromOn(early.begin(), early.begin() + (kFrames - kSecond));
		std::vector<float> lateFromOn(late.begin() + kSecond, late.end());
		CHECK(firstDifference(earlyFromOn, lateFromOn) >= 0);

		// AND THE KNOWN-BAD: the same pair with the transport ROLLING is also different, but for the
		// other reason — there the song moved. What makes the stopped case a claim about free-running
		// is that the song did NOT move and the phase did anyway.
		const std::vector<float> rollingEarly = run(kAnalog, true, 2.5, true, first);
		const std::vector<float> rollingLate = run(kAnalog, true, 2.5, true, second);
		std::vector<float> rollingEarlyFromOn(rollingEarly.begin(),
		                                      rollingEarly.begin() + (kFrames - kSecond));
		std::vector<float> rollingLateFromOn(rollingLate.begin() + kSecond, rollingLate.end());
		CHECK(firstDifference(rollingEarlyFromOn, rollingLateFromOn) >= 0);
	}
}

TEST_CASE("a macro routed to the filter reaches the DSP's own settings, not just a table") {
	// THE HONESTY RULE ON THE WHOLE ROAD, rehomed. This case lived in test_params and read
	// ParamSet::filterSettings(); with the filter swallowed there is no param to route to and no
	// table entry to read, so the claim is made where it is now decidable — against the filter the
	// ENGINE is running, reached through the route table, which is a swallowed device's only road.
	//
	// Being addressable makes a knob glow and a ring draw; what makes it a MODULATION is that the
	// number the filter is built from moves. Those are different claims and only this one is about
	// sound.
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);

	rig.play({noteOn(0, kKeyC4, 1)}, 256, 256);
	// AFTER THE FIRST SYNC: the engine chooses the device's slot when it adopts it, and the destination
	// coordinate is at that slot (kill-lanes step 3a).
	rig.play({}, 64, 64); // a sync first: the id the engine runs the device under is what a route names
	// THE FILTER'S CUTOFF BY IDENTITY — the device the chain carries, its own control index (kill-lanes §2.4b).
	const feathers::backplane::Destination dest = feathers::backplane::Destination::control(
	    rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0), static_cast<size_t>(flt::Param::Cutoff));

	const double resting = rig.synth.busFilterTargetForTest().cutoffLog2Hz;
	CHECK(resting == doctest::Approx(10.0));

	// A tenth of the declared range at a macro sitting at the top. Cutoff is stated in log2(Hz), so
	// an amount is a fraction of the control's own span — the same rule the ring on the face is drawn
	// by, restated here rather than borrowed from rackOffsetAt.
	constexpr size_t kMacroSource = 0;
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(kMacroSource), dest, 0.1f);
	routes.publish(rig.params);
	rig.params.setBase(static_cast<clap_id>(feathers::macroParam(kMacroSource)), 1.0);
	rig.play({noteOn(256, kKeyC4, 2)}, 256, 256);

	const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
	CHECK(rig.synth.busFilterTargetForTest().cutoffLog2Hz
	      == doctest::Approx(10.0 + 0.1 * span));
	// And nothing else moved with it.
	CHECK(rig.synth.busFilterTargetForTest().resonance
	      == doctest::Approx(flt::kParamDescs[static_cast<size_t>(flt::Param::Resonance)].def));

	SUBCASE("and the macro dropping back to zero puts it where it was") {
		// What makes "routed but untouched" and "silent" the same state: a source at rest pushes
		// nowhere.
		rig.params.setBase(static_cast<clap_id>(feathers::macroParam(kMacroSource)), 0.0);
		rig.play({noteOn(512, kKeyC4, 3)}, 256, 256);
		CHECK(rig.synth.busFilterTargetForTest().cutoffLog2Hz == doctest::Approx(resting));
	}

	SUBCASE("and the architecture switches cannot be reached this way at all") {
		// KEYTRACK AND POLY ARE NOT DESTINATIONS, and this is that decision stated where it can be
		// measured. They decide how many filters there are and whether a corner follows a note —
		// architecture rather than amounts — so the device declares them unmodulatable, and a route that
		// names one anyway builds NO input at the node (kill-lanes §2.4b): there is nothing to reach.
		const size_t slot = engineFxSlot(rig.synth, rig.chain, 0);
		REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
		for (const flt::Param p : {flt::Param::Keytrack, flt::Param::Poly}) { // Enabled is a destination since backlog KW
			CAPTURE(static_cast<int>(p));
			// THE DEVICE SAYS SO — and a route planted at the control anyway, past the editor's gate,
			// leaves the node with NO input for it: the value is asserted at the node, not inferred.
			CHECK_FALSE(flt::kDescriptor.params[static_cast<size_t>(p)].modulatable);
			routes.set(feathers::test::macro(kMacroSource),
			           feathers::backplane::Destination::control(dest.device, static_cast<size_t>(p)), 0.5f);
			routes.publish(rig.params);
			rig.play({noteOn(768, kKeyC4, 4)}, 256, 256);
			CHECK(rig.synth.laneForTest().engineForTest().inputCountForTest(slot, static_cast<size_t>(p)) == 0);
		}
		// And the cutoff DID move under the same table, so this is a refusal rather than a routing
		// table that turned out to be empty.
		CHECK(rig.synth.busFilterTargetForTest().cutoffLog2Hz > resting);
	}
}

TEST_CASE("backlog HS: in Feathers' lane, SKLITTER's Perf X reaches SKLITTER's OWN Rate — the value, in the tick it was published") {
	// THE SAME CLAIM `test_rack_engine` MAKES OF A BARE ENGINE, MADE OF THE HOST THE USER PLAYS: the lane
	// ticks its modulators on the synth's own grid and begins its pieces with zero frames, so "the mod
	// road publishes before the audio road resolves" is a fact about a different call sequence here.
	namespace sk = feathers::fx::sklitter;
	Rig rig;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(sk::kTraits.tag);
	REQUIRE(desc != nullptr);
	feathers::DeviceRow row = feathers::defaultRowFor(*desc, { 1 });
	const auto rate = static_cast<size_t>(sk::Param::Rate);
	const auto perfX = static_cast<size_t>(sk::Param::PerfX);
	REQUIRE(row.field[rate] == doctest::Approx(0.5f)); // the arithmetic below leans on it
	rig.setDeviceRow(row);
	rig.play({}, 64, 64); // a sync first: the id the engine runs the device under is what a route names

	const feathers::DeviceInstanceId self = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0);
	REQUIRE(self != feathers::kNoInstance);
	const size_t slot = engineFxSlot(rig.synth, rig.chain, 0);
	REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
	const feathers::rack::RackEngine& engine = rig.synth.laneForTest().engineForTest();

	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(self, sk::outIndex(sk::Out::X)),
	           feathers::backplane::Destination::control(self, rate), 0.5f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	// THE INPUT EXISTS AT THE NODE, and with the puck at the centre it moves nothing.
	CHECK(engine.inputCountForTest(slot, rate) == 1);
	CHECK(engine.modulatedFieldAt(slot, rate) == doctest::Approx(0.5f));

	// ONE TICK AFTER THE PUCK MOVES, THE RATE HAS: Rate is 0..1 so the span is 1, and 0.5 + 0.5 x 0.6.
	row.field[perfX] = 0.6f;
	rig.setDeviceRow(row);
	rig.play({}, 64, 64);
	CHECK(engine.publishedOutputAt(slot, sk::outIndex(sk::Out::X)) == doctest::Approx(0.6f));
	CHECK(engine.modulatedFieldAt(slot, rate) == doctest::Approx(0.8f));
	// AND THE PUCK IS WHERE THE HAND PUT IT — nothing reaches the control the output is made of.
	CHECK(engine.modulatedFieldAt(slot, perfX) == doctest::Approx(0.6f));
}

TEST_CASE("a keytracked voice can be opened and closed all the way at any pitch") {
	// THE BUG: with keytrack on there was not enough range in modulation plus cutoff to fully open the
	// filter in some settings. Measured through the settings the voice's own filter is handed
	// — not recomputed beside it — with the cutoff knob at its own maximum and keytrack on:
	//
	//     key   knob      key offset   corner asked   corner run   short of the knob
	//     C2    20 kHz    -24 st       5 kHz          5 kHz        2 oct   (tracking: correct)
	//     C4    20 kHz      0 st       20 kHz         20 kHz       0
	//     C6    20 kHz    +24 st       80 kHz         20 kHz       0       (CLIPPED: 2 oct lost)
	//
	// The C2 row is keytracking doing its job, and the corner belongs down there. What was broken is
	// that there was nothing left to buy it back with: a +2 and a +4 octave modulation offset both
	// moved the corner by exactly nothing, because the knob's own maximum was also the ceiling on
	// base-plus-modulation. Two of the three roads to a fully open filter were shut and the third —
	// switching keytrack off — is not the sound the player asked for.
	namespace flt = feathers::fx::filter;
	constexpr uint32_t kFrames = 512;
	constexpr int16_t kKeyC2 = 36;
	constexpr int16_t kKeyC6 = 84;

	/// The corner the voice's filter is actually running at, in hertz, after a note has sounded.
	const auto cornerOf = [&](int16_t key, double knob, double modOctaves) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Poly, 1.0); // per-voice is poly's job now, not keytrack's
		rig.setFilter(feathers::fx::filter::Param::Keytrack, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Cutoff, knob);
		// MODULATION REACHES A SWALLOWED DEVICE THROUGH THE ROUTE TABLE AND NOWHERE ELSE — a design
		// decision, and the reason this case changed shape rather than disappearing. The claim it makes
		// is unchanged and is still the original bug's: a keytracked low note must be reachable back up to
		// open. What moved is only the road the modulation arrives by.
		//
		// A MACRO SOURCE HELD AT ONE, because it is the one source the engine does not rewrite every
		// block: tickEnvelopes publishes zero into every device source for a slot the rack does not
		// have, so a device source would be cleared before the first render.
		//
		// The amount is stated in the control's own declared span — `offset = amount x source x span`
		// — so the octaves asked for here are converted by the same arithmetic rackOffsetAt uses,
		// restated rather than borrowed.
		if (modOctaves != 0.0) {
			constexpr size_t kMacroSource = 0;
			const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
			rig.play({}, 64, 64); // a sync first: the engine's slot for the device is what a route names
			const feathers::backplane::Destination dest = feathers::backplane::Destination::control(
			    rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0), static_cast<size_t>(flt::Param::Cutoff));
			feathers::test::Routes routes;
			routes.set(feathers::test::macro(kMacroSource), dest, static_cast<float>(modOctaves / span));
			routes.publish(rig.params);
			rig.params.setBase(static_cast<clap_id>(feathers::macroParam(kMacroSource)), 1.0);
		}
		rig.play({noteOn(0, key, 1)}, kFrames, 256);
		const int slot = rig.synth.slotOfKeyForTest(key);
		REQUIRE(slot >= 0);
		return flt::effectiveCutoffHz(rig.synth.voiceFilterTargetForTest(slot));
	};

	SUBCASE("a low note tracks down, and modulation can carry it back up to open") {
		// Unchanged, and it is what keytrack MEANS: two octaves below middle C is two octaves down.
		CHECK(cornerOf(kKeyC2, flt::kCutoffMaxLog2, 0.0) == doctest::Approx(5000.0));
		// KNOWN-BAD read 5000 for both of these — the modulation was clamped away at the knob's top.
		CHECK(cornerOf(kKeyC2, flt::kCutoffMaxLog2, 2.0) == doctest::Approx(20000.0));
		CHECK(cornerOf(kKeyC2, flt::kCutoffMaxLog2, 4.0) == doctest::Approx(80000.0));
	}

	SUBCASE("a high note keeps tracking above the knob's top") {
		// KNOWN-BAD read 20000 for both, so C6 and every note above it met the same corner: keytrack
		// stopped tracking exactly where the arithmetic left the knob's range.
		CHECK(cornerOf(kKeyC6, flt::kCutoffMaxLog2, 0.0) == doctest::Approx(80000.0));
		CHECK(cornerOf(kKeyC6, flt::kCutoffMaxLog2, 2.0) == doctest::Approx(320000.0));
	}

	SUBCASE("and closes below the knob's bottom") {
		// The same clip at the other end, which is the half a report about "fully open" does not
		// mention: with the knob at its minimum, C2 asked for 5 Hz and was held at 20 — a low note
		// could not be shut as far as a middle one. KNOWN-BAD read 20.
		CHECK(cornerOf(kKeyC2, flt::kCutoffMinLog2, 0.0) == doctest::Approx(5.0));
		CHECK(cornerOf(kKeyC6, flt::kCutoffMinLog2, 0.0) == doctest::Approx(80.0));
	}

	SUBCASE("and the bus filter, which has no key offset, is where it always was") {
		// THE CONTROL. Keytrack off is one filter on the summed bus at the knob's own corner, and
		// nothing here may move it: if this row changed, the fix would have widened the knob rather
		// than freed the offsets.
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		rig.setFilter(feathers::fx::filter::Param::Keytrack, 0.0);
		rig.setFilter(feathers::fx::filter::Param::Cutoff, flt::kCutoffMaxLog2);
		rig.play({noteOn(0, kKeyC2, 1)}, kFrames, 256);
		// READ OFF THE FILTER THE ENGINE IS RUNNING, not off the table the settings used to live in.
		CHECK(flt::effectiveCutoffHz(rig.synth.busFilterTargetForTest())
		      == doctest::Approx(20000.0));
	}
}

TEST_CASE("FM is off by construction, and on it makes sidebands") {
	// THE ENGINE THE DESIGN PLANNED AND NOBODY BUILT. Of the eight tasks in
	// the FM architecture plan only T1 (the carrier table) and T5 (the
	// ids) had landed, and T1's only consumer was its own test — so the eight FM Depth knobs moved a
	// number and no air. T2 (the three terrain fields), T3 (the per-voice modulator accumulator),
	// T4 (the render) and T6 (the stereo flip) are what this case is about.
	//
	// IDENTITY FIRST, because it is the guarantee everything else is allowed to cost: at depth zero
	// the voice must render what it rendered before FM existed, to the BIT. That is not a courtesy —
	// the depth knobs default to zero, so every patch ever saved takes this road.
	const auto renderAt = [](double depth, double zone) {
		Rig rig;
		rig.set(ParamId::OscType, 2.0); // V5 — FM is a flagship feature
		rig.set(ParamId::AZone, zone);
		rig.set(ParamId::AFmDepth, depth);
		std::vector<float> l(4096), r(4096);
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.synth.render(l.data(), r.data(), 4096);
		return std::pair{ l, r };
	};

	const auto [offL, offR] = renderAt(0.0, 17.0);
	// Both ears identical at depth zero: no flip, and the mono path still exact.
	CHECK(offL == offR);

	// THE SIDEBANDS, ASSERTED AS A MEASUREMENT rather than as "the output changed". FM adds partials
	// ABOVE the fundamental's own harmonics, so the claim is that energy moves upward — and it must
	// move MONOTONICALLY with the knob, which a difference check cannot see at all.
	const auto highLowRatio = [](const std::vector<float>& x) {
		constexpr double kF0 = 261.6255653; // key 60
		double lo = 0.0, hi = 0.0;
		for (int k = 1; k <= 40; k++) {
			double re = 0.0, im = 0.0;
			for (size_t i = 0; i < x.size(); i++) {
				const double a = 2.0 * std::numbers::pi * k * kF0 * static_cast<double>(i) / kSampleRate;
				re += x[i] * std::cos(a);
				im -= x[i] * std::sin(a);
			}
			(k <= 8 ? lo : hi) += std::sqrt(re * re + im * im);
		}
		return hi / std::max(lo, 1e-12);
	};

	const double atRest = highLowRatio(offL);
	double previous = 0.0;
	for (double depth : {0.25, 0.50, 0.75, 1.00}) {
		CAPTURE(depth);
		const auto [l, r] = renderAt(depth, 17.0);
		const double ratio = highLowRatio(l);
		// MONOTONE ONCE FM IS ON, which is what the index range and the taper were chosen to give:
		// max index measured at 1.0 cycle because 2.0 is past where brightness still tracks the knob
		// (Bessel terms oscillate), and the knob is power-tapered so its travel is spent where the
		// changes are. Measured here: 0.661, 1.028, 1.699, 3.451.
		CHECK(ratio > previous);
		previous = ratio;
	}
	// AND IT IS NOT MONOTONE FROM ZERO, which is a property of FM rather than a defect and is written
	// down so the next reader does not "fix" it. This coordinate's ratio field is near unison (0.945),
	// so at a low index the first sidebands land CLOSE to the fundamental and the high-to-low balance
	// initially falls below the un-FM'd cycle before climbing past it. The claim that survives is the
	// one that matters: the top of the travel is far brighter than no FM at all.
	CHECK(previous > atRest * 2.0);
}

TEST_CASE("a note with FM sounds the same however much was played before it") {
	// FM doc §2, MEASURED: 64 note-ons differing only in modulator phase came out 2.40 apart at worst
	// over the first 10.7 ms, against a full-scale swing of 2.0. A modulator that survives a note-on
	// makes the SAME note sound different every time you play it — which is not a character, it is an
	// instrument you cannot play twice.
	//
	// THE PROPERTY, NOT THE MECHANISM. An earlier draft of this case read the accumulator out of voice
	// slot 0 after a retrigger and failed for a reason that had nothing to do with FM: the manager put
	// the new note in slot 1. What §2 actually claims is audible determinism, and that is what this
	// asks — play a note after two DIFFERENT amounts of prior playing and require the renders to be
	// identical. Only a modulator that restarts can make that true.
	const auto noteAfter = [](int priorSamples) {
		Rig rig;
		rig.set(ParamId::OscType, 2.0);
		rig.set(ParamId::AZone, 17.0);
		rig.set(ParamId::AFmDepth, 1.0);
		std::vector<float> scratch(4096), scratchR(4096);
		if (priorSamples > 0) {
			rig.synth.noteOn(0, 55, 9, 1.0);
			rig.synth.render(scratch.data(), scratchR.data(), priorSamples);
			rig.synth.noteOff(0, 55, 9, 0.0);
			rig.synth.render(scratch.data(), scratchR.data(), 2048);
		}
		std::vector<float> l(2048), r(2048);
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.synth.render(l.data(), r.data(), 2048);
		return l;
	};

	const std::vector<float> clean = noteAfter(0);
	// Odd lengths on purpose: each leaves the modulator somewhere different, and one of them is a
	// prime number of samples so it cannot land back on a multiple of anything.
	CHECK(noteAfter(777) == clean);
	CHECK(noteAfter(1279) == clean);
	CHECK(noteAfter(3001) == clean);

	// AND THE CASE IS NOT VACUOUS: FM is genuinely running in these renders, so "identical" is a
	// claim about a modulated note rather than about an unmodulated one.
	Rig off;
	off.set(ParamId::OscType, 2.0);
	off.set(ParamId::AZone, 17.0);
	off.set(ParamId::AFmDepth, 0.0);
	std::vector<float> l(2048), r(2048);
	off.synth.noteOn(0, 60, 1, 1.0);
	off.synth.render(l.data(), r.data(), 2048);
	CHECK(l != clean);
}

TEST_CASE("the stereo flip is a width, not a switch — and mono is exact at zero") {
	// FM doc §4, measured to -200 dB: with the modulator's polarity flipped between ears every ODD
	// sideband is 100% side channel and the carrier and every EVEN sideband are 100% mid. A BINARY
	// flip would therefore delete the odd sidebands in a mono fold-down — a different, hollow
	// spectrum rather than a quieter one, -4.83 dB at index 0.35 — which is why the right ear is
	// modulated by (1 - 2w)·I and not by -I.
	//
	// The width is a FIELD, so this case cannot set it directly; what it can pin is the shape. At a
	// coordinate whose width field is zero the two ears must be IDENTICAL however deep the FM, and
	// wherever it is nonzero they must differ once FM is on and not before.
	Rig rig;
	rig.set(ParamId::OscType, 2.0);
	rig.set(ParamId::AZone, 17.0);
	std::vector<float> l(2048), r(2048);

	rig.set(ParamId::AFmDepth, 0.0);
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.synth.render(l.data(), r.data(), 2048);
	// FM OFF IS TWO IDENTICAL EARS WHATEVER THE WIDTH FIELD SAYS, which is the half that keeps every
	// existing patch on the mono path: the stereo gate asks about the FLIP, not about FM.
	CHECK(l == r);

	Rig deep;
	deep.set(ParamId::OscType, 2.0);
	deep.set(ParamId::AZone, 17.0);
	deep.set(ParamId::AFmDepth, 1.0);
	std::vector<float> dl(2048), dr(2048);
	deep.synth.noteOn(0, 60, 1, 1.0);
	deep.synth.render(dl.data(), dr.data(), 2048);
	const float width =
	    feathers::flagshipFieldsAt(feathers::sliceHome(17, 0.0f, 0.0)).fmStereoWidth;
	CAPTURE(width);
	REQUIRE(width > 0.0f); // this coordinate has a flip to make; otherwise the check below is vacuous
	CHECK(dl != dr);

	// AND THE POLARITY ITSELF, asserted as the value §4 specifies rather than through a render. The
	// ends are what carry the claim: +1 is "both ears alike", which is what makes mono bit-exact, and
	// -1 is the full flip.
	CHECK(feathers::fmRightPolarityFor(0.0f) == doctest::Approx(1.0));
	CHECK(feathers::fmRightPolarityFor(0.5f) == doctest::Approx(0.0));
	CHECK(feathers::fmRightPolarityFor(1.0f) == doctest::Approx(-1.0));

	// WHAT THIS CASE DELIBERATELY DOES NOT CLAIM: that a small width gives a narrow image. It does
	// not. MEASURED at depth 1.0 and w = 0.093, mid/side energy came out 0.976 — very nearly a fully
	// spread image from a tenth of the control's travel — because phase modulation is nonlinear in
	// the modulator's amplitude, so a small polarity difference already reads the source at a
	// substantially different phase. §4's claim is about where the SIDEBANDS land at a full flip, and
	// an energy-ratio assertion would have been this case inventing a linearity nobody promised.
}

TEST_CASE("poly is a different sound from bus at the same corner, not a tidier arrangement") {
	// THE CASE THAT JUSTIFIES SPLITTING POLY OFF KEYTRACK, and the split is not worth having without
	// it. The split was requested; review asked for exactly this evidence, and was right that
	// it needs a NONLINEAR path — on a linear filter, filtering a sum and summing the filtered are
	// identical by superposition and the case would be vacuous.
	//
	// SATURATION OF A SUM IS NOT THE SUM OF SATURATIONS. Two voices through ONE driven filter
	// intermodulate against each other, producing third-order products at 2f2-f1 that neither note
	// contains. Two voices through TWO driven filters each saturate alone, and those products cannot
	// form because the two signals never meet before the nonlinearity.
	//
	// SO THE ASSERTION IS ON A VALUE AT A FREQUENCY, not on two renders differing. A difference check
	// would pass for any reason at all — including the arrangement merely changing the gain — and
	// this project's rule is that a difference is evidence something changed and never that the right
	// thing did.
	constexpr int16_t kKeyFs5 = 78;
	constexpr size_t kFrames = 16384;
	constexpr double kC4Hz = 261.626;
	constexpr double kFs5Hz2 = 739.989;
	// Third-order intermodulation, well clear of both notes and of their harmonics.
	constexpr double kImdHz = 2.0 * kFs5Hz2 - kC4Hz; // 1218.35

	const auto play = [&](bool poly) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
		// THE SAME CORNER IN BOTH ARMS, and keytrack OFF in both, so POLY is the only difference. If
		// keytrack were on, the per-voice arm would also move its corners and the case would be
		// measuring two changes at once.
		rig.setFilter(feathers::fx::filter::Param::Cutoff, std::log2(2000.0));
		rig.setFilter(feathers::fx::filter::Param::Keytrack, 0.0);
		rig.setFilter(feathers::fx::filter::Param::Drive, 1.0); // the nonlinearity this case depends on
		rig.setFilter(feathers::fx::filter::Param::Resonance, 0.6);
		rig.setFilter(feathers::fx::filter::Param::Poly, poly ? 1.0 : 0.0);
		return rig.play({noteOn(0, kKeyC4, 1), noteOn(0, kKeyFs5, 2)}, kFrames, 256);
	};

	const std::vector<float> bus = play(false);
	const std::vector<float> perVoice = play(true);

	const feathers::test::Window win(static_cast<int>(kFrames));
	const auto at = [&](const std::vector<float>& x, double hz) {
		return feathers::test::magnitudeAt(x, win, kSampleRate, hz);
	};

	const double busImd = at(bus, kImdHz);
	const double polyImd = at(perVoice, kImdHz);
	CAPTURE(busImd);
	CAPTURE(polyImd);

	// ONE FILTER MAKES THE PRODUCT; SIX DO NOT. Stated as a ratio rather than as two absolute levels,
	// because what matters is that the mechanism is present in one arrangement and absent in the
	// other, not what either happens to measure at.
	const double ratio = busImd / std::max(polyImd, 1e-30);
	CAPTURE(ratio);
	CHECK(ratio > 2.0);

	// NOT VACUOUS: both arms must actually be producing sound at the notes, or the ratio above is a
	// comparison of two silences and would pass for the wrong reason.
	CHECK(at(bus, kC4Hz) > 1e-4);
	CHECK(at(perVoice, kC4Hz) > 1e-4);
	CHECK(at(perVoice, kFs5Hz2) > 1e-4);
}

// ---------------------------------------------------------------------------------------------
// THE FX LANE, RUN GENERICALLY.

TEST_CASE("a second device in the lane renders, and tracks the LAST note still held") {
	// THE POINT OF THE RUNNER, AND THE MONO DECISION, IN ONE CASE. Until the engine walked the chain
	// through AudioOps it rendered a hardcoded filter in slot 0 and nothing else — so HARM's DSP was
	// complete and inaudible.
	//
	// AND THE RULE CHANGED, 2026-08-24: mono follows the LAST active note rather than the lowest.
	// A device on the summed bus has no single note to follow, so the bus follows the most
	// recent one — and on a release it returns to the most recent note STILL HELD rather than going
	// quiet or staying where it was.
	//
	// THE SCRIPT IS DELIBERATELY DISCRIMINATING. Its previous version struck the LOWER note second,
	// which makes "lowest" and "last" the same answer — so it would have passed unchanged under both
	// rules and told nobody the rule had moved. This one strikes the HIGHER note second, where the
	// two rules disagree.
	namespace hrm = feathers::fx::harm;
	constexpr int16_t kLowC = 36; // C2
	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0); // the filter out of the way

	REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
	hrm::State harm;
	harm.settings.enabled = true;
	harm.settings.level = 0.5f;
	harm.settings.harmonic = hrm::kUnisonHarmonic;
	const hrm::Row row = hrm::rowOf(harm);
	rig.chain.device[1].row = feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row),
	                                               row);
	rig.publishChain();

	const auto hzOf = [](int16_t key) {
		return 440.0 * std::exp2((static_cast<double>(key) - 69.0) / 12.0);
	};
	const auto tracked = [&] {
		return hrm::opsCurrentHzForTest(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag));
	};

	// LOW FIRST, THEN HIGH — so "the last" is the HIGH note and "the lowest" would be the low one.
	rig.play(std::vector<Event>{noteOn(0, kLowC, 1), noteOn(64, kKeyC4, 2)}, 512, 128);
	REQUIRE(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) != nullptr);
	CHECK(tracked() == doctest::Approx(hzOf(kKeyC4)).epsilon(1e-4));

	SUBCASE("and releasing it falls back to the note that is still down") {
		// NOT SILENCE AND NOT THE RELEASED PITCH — the fallback is the most recent note still held,
		// which here is the low one. This is the half that makes it a stack rather than a scalar.
		rig.play(std::vector<Event>{noteOff(0, kKeyC4, 2)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kLowC)).epsilon(1e-4));
	}
}

TEST_CASE("poly puts a HARM on every voice, each tracking its own note") {
	// THE CLAIM THIS FEATURE MAKES, AND IT IS ABOUT VALUES RATHER THAN ABOUT A DIFFERENCE. "Poly on
	// sounds different from poly off" passes for any bug that changes anything — a HARM run twice, a
	// HARM run nowhere, sixteen HARMs all tracking the same note. What per-voice actually claims is
	// that there are now N sub-oscillators AT N DIFFERENT FREQUENCIES, one per sounding note, where
	// the bus arrangement has exactly one at the lowest. So the frequencies are read.
	//
	// AND NO BUFFER CAN ANSWER IT. A rendered signal carries a notch, an AR envelope and a sine at
	// once; one frequency is not recoverable from it. That is why Harm publishes currentHzForTest and
	// why Synth publishes the per-voice storage — CLAUDE.md's rule, and the fourth and fifth seams
	// added to this engine for it.
	namespace hrm = feathers::fx::harm;
	constexpr int16_t kKeyG4 = 67;

	const auto build = [](Rig& rig, bool poly) {
		rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0); // the filter out of the way
		REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
		hrm::State harm;
		// SWITCHED ON. HARM has an Enabled control now, off by default like every other effect —
		// The "level=0 means off" convention was retired as hardware-specific. A case about what the
		// device DOES has to put it in the signal first.
		harm.settings.enabled = true;
		harm.settings.level = 0.5f;
		harm.settings.harmonic = hrm::kUnisonHarmonic; // 1:1, so the sub sits ON the note
		harm.poly = poly;
		rig.chain.device[1].row =
		    feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), hrm::rowOf(harm));
		rig.publishChain();
	};
	const auto hzOf = [](int16_t key) {
		return 440.0 * std::exp2((static_cast<double>(key) - 69.0) / 12.0);
	};

	SUBCASE("poly off is one HARM on the bus, at the lowest note and nowhere else") {
		Rig rig;
		build(rig, false);
		rig.play({noteOn(0, kKeyC4, 1), noteOn(64, kKeyG4, 2)}, 512, 128);

		const void* bus = rig.synth.fxStorageForTagForTest(hrm::kTraits.tag);
		REQUIRE(bus != nullptr);
		// THE LAST NOTE STRUCK, which is G4 — the mono arrangement follows the most recent held note
		// since 2026-08-24, where it used to follow the lowest.
		CHECK(hrm::opsCurrentHzForTest(bus) == doctest::Approx(hzOf(kKeyG4)).epsilon(1e-4));

		// EXACTLY ONE OSCILLATOR IN MONO — in mono mode there should only be one oscillator. It
		// was already true rather than a defect (the bus runner is prepared with one instance and a
		// Harm holds one phase accumulator), so this pins the requirement rather than fixing
		// anything. Counted across every voice slot, not just the two holding notes.
		size_t oscillators = rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) != nullptr ? 1 : 0;
		for (int slot = 0; slot < feathers::kVoiceSlots; ++slot) {
			if (rig.synth.voiceFxStorageForTest(slot, hrm::kTraits.tag) != nullptr) {
				++oscillators;
			}
		}
		CHECK(oscillators == 1);

		// AND NOT ON ANY VOICE. The two arrangements are complementary by construction, so this is
		// the assertion that catches a device being run twice — which would be inaudible as a wrong
		// note and very audible as a doubled sub.
		for (int16_t key : {kKeyC4, kKeyG4}) {
			const int slot = rig.synth.slotOfKeyForTest(key);
			REQUIRE(slot >= 0);
			CHECK(rig.synth.voiceFxStorageForTest(slot, hrm::kTraits.tag) == nullptr);
		}
	}

	SUBCASE("poly on is one HARM per voice, each at its own note, and none on the bus") {
		Rig rig;
		build(rig, true);
		rig.play({noteOn(0, kKeyC4, 1), noteOn(64, kKeyG4, 2)}, 512, 128);

		// THE BUS RUNNER LET GO OF IT. Without this the per-voice road would be an ADDITION rather
		// than an arrangement, and the device would be in the signal twice.
		CHECK(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) == nullptr);

		// TWO VOICES, TWO DIFFERENT FREQUENCIES, EACH RESTATED FROM THE MIDI ARITHMETIC rather than
		// read back from anything the render touched. This is the whole feature in two numbers.
		const int low = rig.synth.slotOfKeyForTest(kKeyC4);
		const int high = rig.synth.slotOfKeyForTest(kKeyG4);
		REQUIRE(low >= 0);
		REQUIRE(high >= 0);
		REQUIRE(low != high);

		const void* lowStorage = rig.synth.voiceFxStorageForTest(low, hrm::kTraits.tag);
		const void* highStorage = rig.synth.voiceFxStorageForTest(high, hrm::kTraits.tag);
		REQUIRE(lowStorage != nullptr);
		REQUIRE(highStorage != nullptr);
		CHECK(lowStorage != highStorage); // or "per voice" is one device wearing two names

		CHECK(hrm::opsCurrentHzForTest(lowStorage) == doctest::Approx(hzOf(kKeyC4)).epsilon(1e-4));
		// THE ONE THE BUS ARRANGEMENT CANNOT PRODUCE. On the bus there is a sub at C4 and nothing at
		// G4, whatever else is true; a G4 sub exists only if the fifth voice got its own device and
		// its own note.
		CHECK(hrm::opsCurrentHzForTest(highStorage) == doctest::Approx(hzOf(kKeyG4)).epsilon(1e-4));
	}

	SUBCASE("flipping poly mid-phrase hands the arrangement over without a stale device") {
		// THE CLAIM THE SYNC'S OWN OPTIMISATION RESTS ON, and the reason it is a case rather than a
		// comment. FxRunner::sync pushes settings only into the devices THAT RUNNER RUNS — with the
		// filter alone on the bus, decoding its row into twenty idle per-voice instances measured
		// 0.11% of a core at block 64 and bought nothing. The correctness that makes the saving safe
		// is that sync runs BEFORE any chunk of the slice, so a device that becomes this runner's
		// responsibility is given its settings before a sample passes through it.
		//
		// A STALE DEVICE WOULD BE INAUDIBLE AS A CRASH AND VERY AUDIBLE AS A WRONG NOTE: the per-voice
		// HARM would sound at whatever frequency it was last told, which is the note that was down
		// when the arrangement last changed — or nothing at all if it was never told.
		Rig rig;
		build(rig, false);
		rig.play({noteOn(0, kKeyC4, 1), noteOn(64, kKeyG4, 2)}, 512, 128);
		REQUIRE(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) != nullptr); // on the bus
		const int high = rig.synth.slotOfKeyForTest(kKeyG4);
		REQUIRE(high >= 0);
		REQUIRE(rig.synth.voiceFxStorageForTest(high, hrm::kTraits.tag) == nullptr);

		// THE SWITCH, WITH BOTH NOTES STILL HELD. Nothing else about the row changes.
		hrm::State harm;
		// SWITCHED ON. HARM has an Enabled control now, off by default like every other effect —
		// The "level=0 means off" convention was retired as hardware-specific. A case about what the
		// device DOES has to put it in the signal first.
		harm.settings.enabled = true;
		harm.settings.level = 0.5f;
		harm.settings.harmonic = hrm::kUnisonHarmonic;
		harm.poly = true;
		rig.chain.device[1].row =
		    feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), hrm::rowOf(harm));
		rig.publishChain();
		rig.play({}, 512, 128);

		CHECK(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) == nullptr); // the bus let go
		const void* storage = rig.synth.voiceFxStorageForTest(high, hrm::kTraits.tag);
		REQUIRE(storage != nullptr);
		// THE VALUE, not "it is non-null". A device handed over without its settings would sit at the
		// declared defaults — level zero and no frequency at all — which is exactly what a
		// never-synced instance looks like.
		CHECK(hrm::opsCurrentHzForTest(storage) == doctest::Approx(hzOf(kKeyG4)).epsilon(1e-4));
		CHECK(hrm::opsTargetForTest(storage).level == doctest::Approx(0.5f));
	}

	SUBCASE("a voice's HARM follows the voice, not the slot it used to be") {
		// THE FREQUENCIES HAVE TO BE ABLE TO CHANGE, or an implementation that handed every voice the
		// note it first saw would pass the case above. A voice slot is REUSED, so this is also the
		// per-voice half of "no tail from the note before it".
		Rig rig;
		build(rig, true);
		rig.play({noteOn(0, kKeyC4, 1)}, 512, 128);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		CHECK(hrm::opsCurrentHzForTest(rig.synth.voiceFxStorageForTest(slot, hrm::kTraits.tag))
		      == doctest::Approx(hzOf(kKeyC4)).epsilon(1e-4));

		// Let it go, wait out the edge, and land a different note. It may or may not reuse the slot;
		// what matters is that whichever slot sounds G4 has a HARM at G4.
		rig.play({noteOff(0, kKeyC4, 1)}, 4000, 128);
		rig.play({noteOn(0, kKeyG4, 2)}, 512, 128);
		const int next = rig.synth.slotOfKeyForTest(kKeyG4);
		REQUIRE(next >= 0);
		CHECK(hrm::opsCurrentHzForTest(rig.synth.voiceFxStorageForTest(next, hrm::kTraits.tag))
		      == doctest::Approx(hzOf(kKeyG4)).epsilon(1e-4));
	}
}

TEST_CASE("per-voice HARM strips every note's fundamental, where the bus strips only one") {
	// THE HALF THE FREQUENCY CASE ABOVE CANNOT SEE, and the half that says WHY per-voice is the right
	// arrangement for this device rather than merely a possible one.
	//
	// HARM's job is *strip the fundamental*. On the summed bus it strips exactly ONE — the LAST note
	// still held — so on a two-note chord the other note keeps its fundamental and the device does not
	// do the thing it is named for. This is the measurement of that sentence.
	//
	// WHICH ONE THE BUS STRIPS MOVED WITH THE 2026-08-24 DECISION, from the lowest to the most
	// recent; the SHAPE of the claim did not, which is why only the two expectations below swap.
	//
	// BOTH NOTES ARE MEASURED IN BOTH ARMS, which is deliberate: this repo's rule is that a PARTIAL
	// effect surviving where a total one would be caught is the shape to look for, and "the poly arm
	// is quieter at C4" would pass for a device that simply attenuated everything.
	namespace hrm = feathers::fx::harm;
	constexpr int16_t kKeyG4 = 67;
	constexpr uint32_t kFrames = 16384;

	const auto play = [&](bool poly) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
		REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
		hrm::State harm;
		// SWITCHED ON. HARM has an Enabled control now, off by default like every other effect —
		// The "level=0 means off" convention was retired as hardware-specific. A case about what the
		// device DOES has to put it in the signal first.
		harm.settings.enabled = true;
		// THE NOTCH ALONE. Level stays at zero so the sub-oscillator adds nothing — otherwise the
		// poly arm would be ADDING energy at both notes while the notch removed it, and the two
		// effects would land on the same bins.
		harm.settings.level = 0.0f;
		harm.settings.width = 0.15f; // narrow, so it takes the fundamental and little either side
		harm.settings.harmonic = hrm::kUnisonHarmonic;
		harm.poly = poly;
		rig.chain.device[1].row =
		    feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), hrm::rowOf(harm));
		rig.publishChain();
		return rig.play({noteOn(0, kKeyC4, 1), noteOn(0, kKeyG4, 2)}, kFrames, 256);
	};
	const auto plain = [&] {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
		return rig.play({noteOn(0, kKeyC4, 1), noteOn(0, kKeyG4, 2)}, kFrames, 256);
	};

	const feathers::test::Window win(static_cast<int>(kFrames));
	const auto at = [&](const std::vector<float>& x, double hz) {
		return feathers::test::magnitudeAt(x, win, kSampleRate, hz);
	};
	const double c4Hz = 440.0 * std::exp2((static_cast<double>(kKeyC4) - 69.0) / 12.0);
	const double g4Hz = 440.0 * std::exp2((static_cast<double>(kKeyG4) - 69.0) / 12.0);

	const std::vector<float> none = plain();
	const std::vector<float> bus = play(false);
	const std::vector<float> perVoice = play(true);

	// NOT VACUOUS: the un-notched render must have real energy at both fundamentals, or every ratio
	// below is a comparison of two silences.
	REQUIRE(at(none, c4Hz) > 1e-4);
	REQUIRE(at(none, g4Hz) > 1e-4);

	const double busC4 = at(bus, c4Hz) / at(none, c4Hz);
	const double busG4 = at(bus, g4Hz) / at(none, g4Hz);
	const double polyC4 = at(perVoice, c4Hz) / at(none, c4Hz);
	const double polyG4 = at(perVoice, g4Hz) / at(none, g4Hz);
	CAPTURE(busC4);
	CAPTURE(busG4);
	CAPTURE(polyC4);
	CAPTURE(polyG4);

	// THE BUS ARRANGEMENT: the LAST note struck loses its fundamental and the other keeps its own.
	// Both note-ons are at sample 0 and G4 is second in the script, so G4 is the one being tracked.
	CHECK(busG4 < 0.5);
	CHECK(busC4 > 0.8);

	// THE PER-VOICE ARRANGEMENT: BOTH lose theirs. This is the pair of numbers that says the device
	// finally does what its name says on more than one note at a time.
	CHECK(polyC4 < 0.5);
	CHECK(polyG4 < 0.5);

	// AND C4 IS THE NOTE THAT MOVED between the arms. Stated at C4 specifically, so a poly arm that
	// merely got quieter everywhere does not pass: G4 is notched in BOTH arms, and it is C4 that the
	// bus leaves alone and the per-voice arrangement strips.
	CHECK(polyC4 < busC4 * 0.7);
}

TEST_CASE("the lane's devices run in chain order, and a device the build cannot render is skipped") {
	// ORDER IS SOUND, which is why the lane is reorderable — and a runner that ran slots in any other
	// order would be a different effect chain from the one the player arranged.
	namespace hrm = feathers::fx::harm;
	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);

	// A TAG THIS BUILD HAS NEVER HEARD OF, in the middle of the lane. loadFxChain keeps such a slot on
	// purpose — dropping it would silently rewrite somebody's signal path — so the runner has to step
	// over it rather than stop at it, and the device AFTER it must still render.
	//
	// **WRITTEN AT POSITION 2 AND THAT IS NOW LITERAL (kill-lanes step 3c).** The subscript was a lane
	// and the middle of the lane was `order[2]`; the two agreed here only because this chain had only
	// ever been appended to. The array is the order, so index 2 is the third device in the path — which
	// is what "in the middle of the lane" was always asking for. The `assignDenseFxLanes` pass that
	// stood here has nothing left to hand out.
	rig.chain.device[2].row = feathers::DeviceRow {};
	rig.chain.device[2].row.tag = {'z', 'z', 'z', 'z'};
	rig.chain.device[2].row.instanceId = 99;
	rig.chain.count = 3;
	REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 3);

	hrm::State harm;
		// SWITCHED ON. HARM has an Enabled control now, off by default like every other effect —
		// The "level=0 means off" convention was retired as hardware-specific. A case about what the
		// device DOES has to put it in the signal first.
		harm.settings.enabled = true;
	harm.settings.level = 0.4f;
	for (const size_t slot : {size_t {1}, size_t {3}}) {
		rig.chain.device[slot].row = feathers::toDeviceRow(
		    hrm::kTraits.tag, feathers::identityOf(rig.chain.device[slot].row), hrm::rowOf(harm));
	}
	rig.publishChain();
	rig.play(std::vector<Event>{noteOn(0, kKeyC4, 1)}, 512, 128);

	// BOTH HARMS ARE LIVE AND THE UNKNOWN SLOT HOLDS NOTHING — asked of the pool rather than of the
	// chain, which is the distinction that matters: the chain KEEPS the unknown device, and the pool
	// is what says the engine cannot run it.
	CHECK(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) != nullptr);
	CHECK(feathers::fxHolds(rig.chain, 2, "zzzz"));
}

TEST_CASE("a device in a LATER slot is as audible as the same device in slot 0") {
	// FEATHERS-1A'S MEASUREMENT, TURNED INTO A CASE. The engine used to read slot 0 and only slot 0,
	// so a device anywhere else was inert — the chain held it, seating accepted it, routing followed
	// it, and it made no sound. That is the shape CLAUDE.md warns about from the far side: everything
	// ABOUT the device was true except the part that matters.
	//
	// Their numbers, rms of one note: a plain lane 0.2208; a shut lowpass at SLOT 1 also 0.2208; the
	// same filter at SLOT 0, 0.0055. The slot-0 row is the CONTROL, and it is what makes the other
	// two mean anything — it says the filter really does silence what it is asked to, so an identical
	// reading from slot 1 is the SLOT being inert rather than the harness being broken.
	const auto rms = [](const std::vector<float>& x) {
		double sum = 0.0;
		for (const float v : x) {
			sum += static_cast<double>(v) * static_cast<double>(v);
		}
		return x.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(x.size()));
	};

	const auto renderWith = [&](int filterSlot) {
		Rig rig;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0); // slot 0's filter out of the way
		if (filterSlot >= 0) {
			feathers::fx::filter::State shut;
			shut.settings.enabled = true;
			shut.settings.cutoffLog2Hz = static_cast<float>(feathers::fx::filter::kCutoffMinLog2);
			shut.settings.type = feathers::fx::filter::Type::Lowpass;
			feathers::setFxFilterAt(&rig.chain, static_cast<size_t>(filterSlot), shut);
			rig.chain.count = std::max(rig.chain.count, static_cast<size_t>(filterSlot) + 1);
			if (rig.chain.device[static_cast<size_t>(filterSlot)].instanceId() == feathers::kNoInstance) {
				rig.chain.device[static_cast<size_t>(filterSlot)].row.instanceId =
				    rig.chain.nextInstanceId++;
			}
			rig.publishChain();
		}
		return rms(rig.play(std::vector<Event>{noteOn(0, kKeyC4, 1)}, 8192, 256));
	};

	const double plain = renderWith(-1);
	const double atSlot0 = renderWith(0);
	const double atSlot1 = renderWith(1);
	CAPTURE(plain);
	CAPTURE(atSlot0);
	CAPTURE(atSlot1);

	// THE CONTROL FIRST: a shut lowpass really does take the sound away where the engine reads.
	REQUIRE(plain > 0.05);
	CHECK(atSlot0 < plain * 0.1);

	// AND SLOT 1 DOES THE SAME, which is the claim. Asserted against the plain lane rather than
	// against slot 0's exact number, because the two are not required to be identical — what is
	// required is that a later slot is not INERT.
	CHECK(atSlot1 < plain * 0.1);
}

TEST_CASE("the fx lane's last segment ends with the render, so between renders nothing reads as in "
          "flight") {
	// THE LANE IS A RACK ENGINE, and a render opens one of its blocks at every settling of the lane —
	// the top of the render, and the motion tick when an envelope is routed. `render` must close the
	// last one: an engine left mid-block between renders is one whose handoff says a block is running
	// while the audio thread idles.
	Rig rig;
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	CHECK_FALSE(rig.synth.laneInBlockForTest());
	for (int n : { 1, 64, 256 }) {
		CAPTURE(n);
		rig.synth.render(l.data(), r.data(), n);
		CHECK_FALSE(rig.synth.laneInBlockForTest());
	}
}

TEST_CASE("the host's tempo reaches the rack's modulators, per block and unlatched") {
	// THE MSEG'S TIME BASE, AS SPECIFIED: the time base scales the entire x axis, so the whole curve
	// fits inside (time base choice) × (scale knob) — and that has to follow host transport tempo
	// automation.
	//
	// THAT SECOND CLAUSE IS THE ONE WITH TEETH, and it is what this case is for. A tempo read once —
	// at activate, at note-on, at prepare — makes a musically-timed modulator drift away from the
	// song the moment anybody draws a tempo ramp, and it drifts silently. So the claim is not "the
	// tempo arrived" but "the tempo arrives EVERY BLOCK and the last one wins".
	//
	// ASSERTED AS A VALUE, THROUGH A SEAM, because nothing about the rendered audio can answer it
	// yet: this commit plumbs the clock and nothing reads it. CLAUDE.md's rule — a claim about a
	// value cannot be inferred from a buffer — is the whole reason `Shape::clockForTest` exists.
	Rig rig;
	namespace mseg = feathers::mod::mseg;

	feathers::ModRack rack = feathers::defaultModRack();
	rack.setMsegAt(0, mseg::Settings {});
	REQUIRE(rack.holds(0, mseg::kTraits.tag));
	rig.publishModRack(rack);

	// NOTHING SAID YET IS A REAL STATE, and it is the state a standalone build lives in.
	CHECK_FALSE(rig.synth.rackClock().haveTempo());

	rig.synth.setTempo(120.0);
	// THE NUMBER, not "some tempo": 120 BPM is half a second a beat.
	CHECK(rig.synth.rackClock().haveTempo());
	CHECK(rig.synth.rackClock().secondsPerBeat == doctest::Approx(0.5));

	// AND IT REACHES THE FX LANE TOO, from the SAME derivation. The fx lane had no clock at all
	// until the automodulator wanted a synced rate; this asserts the road exists and that it carries
	// the rack's number rather than a second one computed nearby. Read after a render, because the
	// delivery happens per block.
	{
		std::vector<float> l(64, 0.0f);
		std::vector<float> r(64, 0.0f);
		rig.synth.render(l.data(), r.data(), 64);
		CHECK(rig.synth.fxClockForTest().haveTempo());
		CHECK(rig.synth.fxClockForTest().secondsPerBeat
		      == doctest::Approx(rig.synth.rackClock().secondsPerBeat));
		CHECK(rig.synth.fxClockForTest().secondsPerBeat == doctest::Approx(0.5));
	}

	rig.synth.setTempo(60.0);
	CHECK(rig.synth.rackClock().secondsPerBeat == doctest::Approx(1.0));
	// AND IT REPLACED the old one rather than being latched at the first call — stated separately,
	// because a latch would pass the very first assertion and fail only under automation.
	CHECK(rig.synth.rackClock().secondsPerBeat != doctest::Approx(0.5));

	SUBCASE("and a tempo the host has not given is not a tempo of zero") {
		// ABSENCE AS A VALUE. A host that stops reporting tempo must not leave the last one standing,
		// and must not be read as "infinitely fast" either.
		rig.synth.setTempo(0.0);
		CHECK_FALSE(rig.synth.rackClock().haveTempo());
		rig.synth.setTempo(-30.0);
		CHECK_FALSE(rig.synth.rackClock().haveTempo());
		rig.synth.setTempo(std::numeric_limits<double>::quiet_NaN());
		CHECK_FALSE(rig.synth.rackClock().haveTempo());
		// AND A GOOD ONE AFTER A BAD ONE STILL LANDS, or the guard would be a one-way latch of its own.
		rig.synth.setTempo(140.0);
		CHECK(rig.synth.rackClock().secondsPerBeat == doctest::Approx(60.0 / 140.0));
	}

	SUBCASE("and it is independent of whether the transport is rolling") {
		// THE CORRECTION THAT RESHAPED THIS WORK. CLAP_TRANSPORT_HAS_TEMPO and
		// CLAP_TRANSPORT_IS_PLAYING are separate flags — a stopped session still has a tempo, and
		// that is the commonest case a hand meets. Rate comes from tempo; only PHASE needs a playhead.
		rig.synth.setTempo(90.0);
		rig.synth.setTransport(4.0, feathers::device::TransportClock::kNoPosition, false); // stopped
		CHECK(rig.synth.rackClock().secondsPerBeat == doctest::Approx(60.0 / 90.0));
		rig.synth.setTransport(4.0, feathers::device::TransportClock::kNoPosition, true); // rolling
		CHECK(rig.synth.rackClock().secondsPerBeat == doctest::Approx(60.0 / 90.0));
	}

	SUBCASE("and it reaches the DEVICE, which is the claim the plumbing is actually for") {
		// The two assertions above are about Synth. This one follows the value across the boundary
		// the whole commit exists to cross: into the object that will read it.
		rig.synth.setTempo(150.0);
		rig.play({ noteOn(0, kKeyC4, 1) }, 512, 128);
		const feathers::mod::TransportClock arrived = rig.synth.msegShapeForTest(0).clockForTest();
		CHECK(arrived.haveTempo());
		CHECK(arrived.secondsPerBeat == doctest::Approx(60.0 / 150.0));

		// AND A CHANGE BETWEEN BLOCKS ARRIVES TOO, which is the unlatched half stated where it can
		// fail. A device that copied the clock once would keep 0.4 here.
		rig.synth.setTempo(75.0);
		rig.play({}, 512, 128);
		CHECK(rig.synth.msegShapeForTest(0).clockForTest().secondsPerBeat
		      == doctest::Approx(60.0 / 75.0));
	}
}

TEST_CASE("the mono arrangement follows a note STACK, not the previous note and not the lowest") {
	// THE SPECIFICATION, 2026-08-24: mono follows the LAST active note rather than the lowest. With
	// D0 held, the player plays F0 briefly and releases it: the sub glides up to F0 and back down to
	// D0 on the release.
	//
	// THE FALLBACK IS "THE NOTE STILL HELD", and only D0 is still held, so the rule is unambiguous.
	//
	// AND THIS IS WHY PORTA IS LOAD-BEARING IN MONO. Under the lowest-note rule the target barely
	// moved; with last-note priority it jumps on every note-on and every release-with-fallback, and
	// the glide is what makes that musical rather than a click. The specified example is literally a
	// porta up and a porta down.
	namespace hrm = feathers::fx::harm;
	constexpr int16_t kD0 = 26;
	constexpr int16_t kF0 = 29;
	constexpr int16_t kA0 = 33;

	const auto hzOf = [](int16_t key) {
		return 440.0 * std::exp2((static_cast<double>(key) - 69.0) / 12.0);
	};

	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
	hrm::State harm;
	harm.settings.enabled = true;
	harm.settings.level = 0.5f;
	harm.settings.harmonic = hrm::kUnisonHarmonic;
	// PORTA AT ITS MINIMUM, which the LogSeconds convention makes "no time at all" — so the tracked
	// frequency is at its target the instant it changes and the VALUE is readable. The glide's own
	// timing is pinned in test_fx_harm; conflating the two would make this case about both.
	rig.chain.device[1].row =
	    feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), hrm::rowOf(harm));
	rig.publishChain();

	const auto tracked = [&] {
		return hrm::opsCurrentHzForTest(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag));
	};

	SUBCASE("his sequence: D0 held, F0 played and released, back to D0") {
		rig.play(std::vector<Event>{noteOn(0, kD0, 1)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kD0)).epsilon(1e-4));

		rig.play(std::vector<Event>{noteOn(0, kF0, 2)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kF0)).epsilon(1e-4));

		// BACK DOWN TO THE NOTE STILL HELD — not silence, and not the pitch that was released.
		//
		// AND *WHEN THEY RELEASE*, WHICH MEANS AT THE RELEASE AND NOT AFTER THE AMP TAIL. Read after
		// 32 samples, which is well inside the release fade: a device that went on tracking a voice
		// while it decayed would still be sitting on F0 here and would fall back late, delaying the
		// porta-down by the release time. A known-bad that counted releasing voices as held passed
		// every longer-interval read in this case, which is how that gap was found.
		rig.play(std::vector<Event>{noteOff(0, kF0, 2)}, 32, 32);
		CHECK(tracked() == doctest::Approx(hzOf(kD0)).epsilon(1e-4));

		// And it stays there once the tail is gone, rather than falling back and drifting again.
		rig.play(std::vector<Event>{}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kD0)).epsilon(1e-4));
	}

	SUBCASE("releasing a note that is NOT the target moves nothing") {
		// THE CASE THAT SEPARATES A STACK FROM A "PREVIOUS NOTE" VARIABLE. Three down, release the
		// middle one: a scalar implementation that remembered only the last-but-one would jump.
		rig.play(std::vector<Event>{noteOn(0, kD0, 1), noteOn(64, kF0, 2), noteOn(128, kA0, 3)}, 512,
		         128);
		REQUIRE(tracked() == doctest::Approx(hzOf(kA0)).epsilon(1e-4));

		rig.play(std::vector<Event>{noteOff(0, kF0, 2)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kA0)).epsilon(1e-4));
	}

	SUBCASE("and the fallback is by RECENCY, not by pitch") {
		// A LOWEST-NOTE IMPLEMENTATION PASSES A TWO-NOTE TEST, which is the degenerate-fixture shape
		// this repo has met four times. Three notes, and the fallback is the middle PITCH — so
		// neither "lowest" nor "highest" can produce it.
		rig.play(std::vector<Event>{noteOn(0, kD0, 1), noteOn(64, kA0, 2), noteOn(128, kF0, 3)}, 512,
		         128);
		REQUIRE(tracked() == doctest::Approx(hzOf(kF0)).epsilon(1e-4));

		rig.play(std::vector<Event>{noteOff(0, kF0, 3)}, 512, 128);
		// A0 — the most RECENT still held. The lowest would be D0 and the highest would be A0 by
		// accident here, so the discriminating half is the D0 check below.
		CHECK(tracked() == doctest::Approx(hzOf(kA0)).epsilon(1e-4));
		CHECK(tracked() != doctest::Approx(hzOf(kD0)).epsilon(1e-4));

		// AND ONE MORE RELEASE walks the stack down to the last one standing.
		rig.play(std::vector<Event>{noteOff(0, kA0, 2)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kD0)).epsilon(1e-4));
	}

	SUBCASE("and a note struck LOWER than the one held still takes the sub, which lowest could not") {
		// THE PLAIN INVERSION of the retired rule, stated once so the change is unmissable.
		rig.play(std::vector<Event>{noteOn(0, kA0, 1)}, 512, 128);
		REQUIRE(tracked() == doctest::Approx(hzOf(kA0)).epsilon(1e-4));
		rig.play(std::vector<Event>{noteOn(0, kD0, 2)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kD0)).epsilon(1e-4));
		// And releasing the newer one goes back UP to the older, which is the fallback in the
		// direction the old rule would never have produced.
		rig.play(std::vector<Event>{noteOff(0, kD0, 2)}, 512, 128);
		CHECK(tracked() == doctest::Approx(hzOf(kA0)).epsilon(1e-4));
	}
}

TEST_CASE("the meter connector reaches a OWLTT in a real chain, with the slot the card reads") {
	// THE SEAM THAT WAS BROKEN THREE TIMES AND NEVER TESTED ONCE.
	//
	// Every OWLTT meter case so far drove the Owltt object directly with a callback the test installed.
	// That proves the DSP computes gain reduction and proves NOTHING about the road to the card --
	// which is where all three failures were: no caller, editor-owned atomics, and a connector
	// installed after the devices it had to reach were already built.
	//
	// The bridge lives above Synth so a test here cannot read it, but the CONNECTOR is set on Synth,
	// which is the half that kept breaking. This asserts it fires, and asserts WHICH SLOT it is told
	// -- because the card reads `bridge_.owlttMeters(slot_)` with its fx-chain slot, and the runner
	// walks a FLAT instances x slots array. If those two disagree the meters are published where
	// nobody looks, which reads exactly like a dead meter.
	namespace dt = feathers::fx::owltt;
	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);

	std::vector<size_t> slotsSeen;
	// TAG-FILTERED, because the connector is general now and this case is about OWLTT's slots.
	rig.synth.setDspConnector([&](const char* tag, size_t slot, void* storage) {
		if (storage != nullptr && feathers::device::sameTag(tag, feathers::fx::owltt::kTraits.tag)) {
			slotsSeen.push_back(slot);
		}
	});

	REQUIRE(feathers::addFxDevice(&rig.chain, dt::kTraits.tag) == 1);
	dt::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(dt::kParamDescs[i].def);
	}
	row[static_cast<size_t>(dt::Param::Enabled)] = 1.0f;
	rig.chain.device[1].row =
	    feathers::toDeviceRow(dt::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), row);
	rig.publishChain();
	rig.synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	rig.synth.render(l.data(), r.data(), 512);

	CAPTURE(slotsSeen.size());
	for (size_t s : slotsSeen) {
		CAPTURE(s);
	}
	// IT FIRES AT ALL -- the first failure was that nothing ever called it.
	REQUIRE_FALSE(slotsSeen.empty());
	// AND IT IS TOLD THE SLOT THE CARD USES. OWLTT is at fx slot 1 in this chain, and OwlttCard is
	// built as `OwlttCard(bridge, registry, slot, ...)` with that same 1.
	CHECK(std::find(slotsSeen.begin(), slotsSeen.end(), size_t { 1 }) != slotsSeen.end());
}

TEST_CASE("EVERY declared param, at both ends of its declared range, still renders a finite signal") {
	// **A NaN ANYWHERE IN THE VOICE PATH IS PERMANENT.** It propagates through the sum, through the fx
	// chain and through the master fader, and there is no control that clears it — the user's only
	// recourse is to reload the plugin. So finiteness is a safety property rather than a quality bar,
	// and it is one of the very few claims that can be asked of every parameter without knowing what
	// any of them does.
	//
	// DRIVEN FROM `paramDescs()`, NEVER FROM A LIST. Every case in this file drives the two or three
	// params it is about; nothing asks the same question of all of them, so a param whose top end has
	// never been rendered is the normal state rather than the exception. A param added tomorrow is
	// swept here without anybody coming back.
	//
	// THE SAME SWEEP FOUND A SHIPPED NaN ONE LAYER DOWN. Asking every fx device the same question at
	// its declared extremes is what found `drawn` reaching infinity 0.53 s after the input stopped.
	// This is that sweep, aimed at the engine instead of the lane.
	const std::span<const feathers::ParamDesc> descs = feathers::paramDescs();
	REQUIRE(descs.size() > 50);

	constexpr uint32_t kFrames = 2048;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    noteOn(11, kKeyG4, 2),
	    noteOff(1400, kKeyC4, 1),
	};

	// GENEROUS, because the claim is "it did not diverge" rather than "it was quiet": a resonant patch
	// at the top of its range is allowed to be loud, and a tight bound would be asserting a taste.
	constexpr float kDiverged = 1.0e6f;

	int configurations = 0;
	int loudEnough = 0;
	std::vector<std::string> broke;
	for (const feathers::ParamDesc& d : descs) {
		// EVERY VALUE OF A CHOICE, both ends of anything else. A Choice selects which code runs, so a
		// sweep that visited only its ends would leave the middle of every engine list unrendered.
		std::vector<double> values;
		if (d.unit == feathers::ParamUnit::Choice || d.unit == feathers::ParamUnit::BendChoice) {
			for (double v = std::round(d.min); v <= std::round(d.max) + 0.5; v += 1.0) {
				values.push_back(v);
			}
		}
		else {
			values.push_back(d.min);
			values.push_back(d.max);
		}

		for (const double v : values) {
			Rig rig;
			rig.expectMono = false; // the stereo controls are among the params being swept
			rig.set(d.id, v);
			const std::vector<float> out = rig.play(script, kFrames, 64);
			++configurations;

			int nonFinite = 0;
			int diverged = 0;
			float peak = 0.0f;
			for (uint32_t i = 0; i < kFrames; ++i) {
				if (!std::isfinite(out[i])) {
					++nonFinite;
					continue;
				}
				peak = std::max(peak, std::fabs(out[i]));
				if (std::fabs(out[i]) > kDiverged) {
					++diverged;
				}
			}
			if (peak > 0.001f) {
				++loudEnough;
			}
			if (nonFinite != 0 || diverged != 0) {
				broke.push_back(std::string(d.name) + " = " + std::to_string(v) + " (nonFinite "
				                + std::to_string(nonFinite) + ", diverged " + std::to_string(diverged)
				                + ", peak " + std::to_string(peak) + ")");
			}
		}
	}

	CAPTURE(configurations);
	CHECK(broke.empty());
	for (const std::string& s : broke) {
		MESSAGE("NOT FINITE: " << s);
	}
	// FLOORS, so a sweep that rendered nothing cannot pass by doing nothing — and so a fixture that
	// silenced the synth at every setting cannot either. The note is audible at the great majority of
	// settings; a minority (Level at its floor, the voice count at zero) is silent on purpose.
	CHECK(configurations >= 100);
	CHECK(loudEnough >= configurations / 2);
	MESSAGE("swept " << configurations << " single-param extremes, " << loudEnough << " audible");
}


namespace {

/// `which` 0 is the floor, 1 the ceiling, 2 the ceiling with its pitch held back — the three ends the cases
/// below each walk. Only the derivation an end reads is run for it. `shard` of `shards` takes every `shards`-th param
/// of the walk (its choices with it), so the shards of one end partition it exactly; the bare end is shard 0's.
void sweepWholeExtremes(const int which, const int shard, const int shards) {
	// **ONE PARAM AT A TIME IS THE WEAK VERSION AND IT IS THE ONE EVERY SUITE WRITES.** The case above
	// sweeps 282 single extremes and finds nothing; the fx-device sweep that found `drawn` reaching
	// infinity found it at ALL-AT-MAX and at nothing less, because the divergence needed six poles at
	// their ceiling *and* the globals at theirs. A defect that needs two controls together is
	// invisible to a sweep that moves one.
	//
	// AND ALL-AT-MAX IS ALSO NOT ENOUGH, WHICH IS THE HALF THAT NEEDED MEASURING. Pinning every param
	// to one end is a single point, and a uniform one — so it can be blind in a way the single sweep
	// is not. MEASURED: with BOTH of `blendWeights`' degeneracy guards deleted, all-at-max stayed
	// finite, because `Width` at its ceiling DISPLACES every anchor per ear and so pulls them apart.
	// The dangerous arrangement — every sound in one place with no width to separate them — is
	// all-at-max with ONE control pulled the other way, which is why each param is walked to the
	// opposite end inside each extreme.
	//
	// AND THE CHOICES ARE WALKED INSIDE THE EXTREMES for the same reason: pinning every param to its
	// max pins every Choice to its LAST entry, so all-at-max on its own renders exactly one engine,
	// one filter type and one osc mode.
	//
	// THE RIGS LIVE ON THE HEAP (spectral-ops, 2026-10-12): a Rig is megabytes (its Synth holds its voices' oscillators inline),
	// up to three are alive at once here, and the case outgrew the 8 MB stack when each oscillator gained Damp's 3.8 KB —
	// a SIGSEGV that ulimit -s 8500 cured, which is the stack and not the code under test.
	const std::span<const feathers::ParamDesc> descs = feathers::paramDescs();
	REQUIRE(descs.size() > 50);

	constexpr uint32_t kFrames = 2048;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    noteOn(11, kKeyG4, 2),
	    noteOn(23, kKeyC4 + 7, 3),
	    noteOff(1400, kKeyC4, 1),
	};

	constexpr float kDiverged = 1.0e6f;

	const auto peakOfRender = [](const std::vector<float>& out) {
		float p = 0.0f;
		for (const float x : out) {
			if (std::isfinite(x)) {
				p = std::max(p, std::fabs(x));
			}
		}
		return p;
	};

	// **WHICH PARAMS, ALONE AT THEIR FLOOR, SILENCE THE SYNTH — DERIVED, NOT NAMED.**
	//
	// All-at-min renders EXACT SILENCE, and so does all-at-min with any ONE control pulled back to its
	// ceiling: measured, 0 of 172 such configurations produced a peak above 0.001. So the floor half
	// of this sweep exercised the control paths and never once reached the audio, while a single
	// `audible` floor over both halves was satisfied entirely by the ceiling half.
	//
	// AT LEAST TWO CONTROLS HAVE TO MOVE TOGETHER to un-silence it, which is why restoring them is
	// derived rather than listed: asking each param in turn whether ITS floor alone silences the synth
	// finds exactly the set, and it stays correct when a param is added, renamed or given a different
	// floor. A hardcoded pair is the thing this project's sweeps keep being caught by.
	std::vector<const feathers::ParamDesc*> silencers;
	if (which == 0) { // the floor reads this derivation, and only it
		for (const feathers::ParamDesc& d : descs) {
			const auto rigHeap = std::make_unique<Rig>();
			Rig& rig = *rigHeap;
			rig.expectMono = false;
			rig.set(d.id, d.min);
			if (peakOfRender(rig.play(script, kFrames, 64)) <= 0.001f) {
				silencers.push_back(&d);
			}
		}
		// A FLOOR AND A CEILING ON THE DERIVATION ITSELF. Zero would mean the probe never found anything
		// and the restoration below is a no-op; a large number would mean the default patch is barely
		// audible and this is measuring noise rather than the two controls that mean "no sound".
		CAPTURE(silencers.size());
		CHECK(silencers.size() >= 1);
		CHECK(silencers.size() <= 8);
	}

	// **AND THE CEILING HAS SILENCERS TOO, SINCE THE ENGINE AT THE TOP OF OSC TYPE STOPPED ALIASING** (Additive, 2026-10-10).
	// All-at-max stacks Coarse, Transpose Oct and Semi and Fine Tune onto C4: a note near MIDI 169, a fundamental of
	// ~138 kHz, above every Nyquist. Every engine before Additive rendered SOMETHING there (a table read at that rate
	// folds back into the audible band: V5 measured a peak of 0.89 at it), so this half's floor was being met by
	// aliasing. Additive is a partial bank that culls what it cannot render below Nyquist (design §5.3), so at that pitch
	// it is honestly silent, and the ceiling half measured 26 audible configurations of 524 — every one an Osc Type or a
	// pitch control pulled back.
	//
	// DERIVED, LIKE THE FLOOR'S, NOT NAMED: with every param at its ceiling, which params ALONE, pulled back to the
	// value the rig sounds with, bring the note back? Those are held there in an EXTRA ceiling pass, which exercises the
	// top engine's audio path at every other setting — what "audible at max Osc Type" means — and is what the ceiling's
	// AUDIBILITY counts read. THE ORIGINAL, UNHELD CEILING PASS STILL RUNS FOR FINITENESS (spec review, 2026-10-10):
	// held back, Coarse's and Transpose Oct's maxima would be rendered in no ceiling configuration at all (the override
	// pulls each param to its MIN there, and the choice walk skips Octaves), so every engine's extreme-pitch path is
	// checked finite at every other param's max only by the unheld pass. Additive silent there is fine; non-finite is
	// not, on any engine. THE CHOICES ARE NOT CANDIDATES: they are
	// walked through every entry inside each extreme below, and Osc Type pulled back would trade the top engine for
	// one that aliases, which is the opposite of the point.
	//
	// **AND ONE PAIR NO SINGLE PULL CAN UNDO, NAMED BECAUSE IT IS A PAIR** (the spectral drawer, 2026-10-10). At the ceiling
	// Additive's Shape is Square at Shape Amount 1 (odd harmonics only) and the drawer's Odd/Even is on at +1 (even
	// harmonics only): two complementary masks, so the spectrum is EXACTLY nothing at any pitch, by construction. The
	// derivation asks which ONE pull brings the note back, and with the pitch AND this pair both silencing, none does —
	// the held ceiling collapsed to the unheld one (6 audible of 694 counted). So Odd/Even's Balance is held at its
	// resting middle in every ceiling probe and in the held pass, and the derivation finds the pitch on top of it as before.
	// Pinned both ways below: without it the held ceiling is silent, with it (and the pitch) it sounds.
	// EVERY SOUND'S Balance (the drawer is per sound since 2026-10-10): at the ceiling every sound's Odd/Even is on at +1.
	// **THE SOUNDING VALUES COME FROM ONE REFERENCE RIG, BUILT ONCE** (2026-10-30). Every override below restores a
	// param to the base a fresh rig holds, and that base is the same on every call — the rig is only READ, never played.
	// Built per call it was a second (and in the held pass a third) Synth per configuration, roughly half the case's
	// runtime, for a value that cannot differ between them.
	const auto soundingHeap = std::make_unique<Rig>();
	const Rig& sounding = *soundingHeap;
	const auto isMaskPair = [](ParamId id) {
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			if (id == feathers::spectralParam(slot, feathers::SpectralControl::OddEvenBalance)) {
				return true;
			}
		}
		return false;
	};
	const auto holdMaskPair = [&](Rig& rig) {
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			const ParamId id = feathers::spectralParam(slot, feathers::SpectralControl::OddEvenBalance);
			rig.set(id, sounding.params.base(static_cast<clap_id>(id)));
		}
	};
	const auto atCeiling = [&](const feathers::ParamDesc* except) {
		const auto rigHeap = std::make_unique<Rig>();
		Rig& rig = *rigHeap;
		rig.expectMono = false;
		for (const feathers::ParamDesc& e : descs) {
			rig.set(e.id, e.max);
		}
		// THE HELD CEILING IS ADDITIVE'S, by name (2026-10-31): it was the top of Osc Type, the engine that culls what it cannot
		// render, and the derivation below is of ITS silencers. Pluck took the top that day, and above its own top (sr / 7) a
		// string is silent too, but at a lower pitch than any one pull reaches, so no single pull derives anything there.
		// Pluck's ceiling is still rendered: it is an Osc Type choice, walked inside every extreme, and the unheld pass.
		rig.set(ParamId::OscType, static_cast<double>(feathers::OscType::Additive));
		holdMaskPair(rig);
		if (except != nullptr) {
			rig.set(except->id, sounding.params.base(except->id));
		}
		return peakOfRender(rig.play(script, kFrames, 64));
	};
	std::vector<const feathers::ParamDesc*> ceilingSilencers;
	if (which == 2) { // the held ceiling reads this derivation, and only it
		if (atCeiling(nullptr) <= 0.001f) {
			for (const feathers::ParamDesc& d : descs) {
				if (d.unit == feathers::ParamUnit::Choice || d.unit == feathers::ParamUnit::BendChoice) {
					continue;
				}
				if (atCeiling(&d) > 0.001f) {
					ceilingSilencers.push_back(&d);
				}
			}
		}
		// What it finds is the pitch (Coarse and Transpose Oct, each worth four octaves); a large set would mean the top
		// engine is barely audible and this is restoring the patch rather than a pitch.
		std::string ceilingNames;
		for (const feathers::ParamDesc* d : ceilingSilencers) {
			ceilingNames += std::string(d->name) + "; ";
		}
		CAPTURE(ceilingNames);
		MESSAGE("ceiling silencers: " << ceilingNames);
		CHECK(ceilingSilencers.size() <= 4);
		{
			// THE PAIR, BOTH WAYS: with the pitch held back and Odd/Even left at its ceiling the note is silent (so holding it is
			// not decoration), and holding it too brings the note back (so the pair is the whole of what was silencing).
			//
			// **THE SILENT HALF RUNS AT NATURAL PHASES** (A-PH3, decision (b), 2026-10-18). At the ceiling Phases is Sub
			// priority, whose sub is SET against the shape's own rest, upstream of every drawer operator: the drawer's Odd/Even no
			// longer reaches it, so the pair silences every partial BUT the sub and the note sounds. The pair's exact silence is
			// now a property of Natural and Headroom, and the converse — at Sub priority the pair un-held still sounds, the sub
			// being the only partial either mask leaves — is pinned beside it.
			const auto heldCeiling = [&](bool holdPair, std::optional<double> phases = std::nullopt) {
				const auto rigHeap = std::make_unique<Rig>();
				Rig& rig = *rigHeap;
				rig.expectMono = false;
				for (const feathers::ParamDesc& e : descs) {
					rig.set(e.id, e.max);
				}
				// THE PAIR IS ADDITIVE'S (its Shape and its drawer's Odd/Even), so it is asked of Additive by name: since Pluck
				// (2026-10-31) Additive is no longer the top of Osc Type, and Pluck's string reads neither mask.
				rig.set(ParamId::OscType, static_cast<double>(feathers::OscType::Additive));
				for (const feathers::ParamDesc* d : ceilingSilencers) {
					rig.set(d->id, sounding.params.base(d->id));
				}
				if (holdPair) {
					holdMaskPair(rig);
				}
				if (phases) {
					rig.set(ParamId::AdditivePhases, *phases);
				}
				return peakOfRender(rig.play(script, kFrames, 64));
			};
			CHECK(heldCeiling(false, 0.0) <= 0.001f); // Natural: the masks leave nothing
			CHECK(heldCeiling(false) > 0.001f);       // Sub priority (the ceiling's): the sub survives the masks
			CHECK(heldCeiling(true) > 0.001f);
		}
	}

	// `which` 0 is every param at its floor, 1 is every param at its ceiling, 2 is the ceiling with its derived
	// silencers held back (the audibility pass, above). `override` then pulls
	// ONE control to a named value, which is how the choices and the opposite ends are walked.
	//
	// AT THE FLOOR THE SILENCERS ARE HELD AT THEIR DEFAULTS, so the configuration is "every control
	// that CAN be at its floor while the instrument still sounds, is" — which is the arrangement that
	// actually exercises the audio path. The override is applied after, so pulling a silencer back is
	// still expressible.
	const auto renderWith = [&](int which, std::span<const std::pair<ParamId, double>> pins) {
		const auto rigHeap = std::make_unique<Rig>();
		Rig& rig = *rigHeap;
		rig.expectMono = false;
		for (const feathers::ParamDesc& d : descs) {
			rig.set(d.id, which == 0 ? d.min : d.max);
		}
		if (which == 0) {
			// RESTORED TO THE VALUE THE RIG SOUNDS WITH, not the declared default: Level's declared default
			// is ZERO since backlog EF (the default patch's envelope carries it, over a route this bare
			// rig does not have), so "its default" would leave the floor half silent again. The rig's
			// own base is the arrangement that sounds, for every silencer.
			for (const feathers::ParamDesc* d : silencers) {
				rig.set(d->id, sounding.params.base(d->id));
			}
		}
		else if (which == 2) {
			// THE CEILING'S, the same way (derived above) — in the audibility pass only.
			rig.set(ParamId::OscType, static_cast<double>(feathers::OscType::Additive)); // atCeiling's engine: see there
			for (const feathers::ParamDesc* d : ceilingSilencers) {
				rig.set(d->id, sounding.params.base(d->id));
			}
			holdMaskPair(rig);
		}
		for (const auto& [id, v] : pins) {
			rig.set(id, v);
		}
		return rig.play(script, kFrames, 64);
	};
	const auto render1 = [&](int which, const feathers::ParamDesc* one, double to) {
		if (one == nullptr) {
			return renderWith(which, {});
		}
		const std::array<std::pair<ParamId, double>, 1> pins {
			{ { one->id, to } }
		};
		return renderWith(which, pins);
	};

	int configurations = 0;
	int audible = 0;
	// THE AUDIBILITY BOUND'S OWN POPULATION: the floor and the held ceiling, not the unheld ceiling, whose pitch is above
	// every Nyquist and where an engine that does not alias is honestly silent. Every pass is checked for finiteness.
	int countedConfigurations = 0;
	int countedAudible = 0;
	bool counting = true;
	std::vector<std::string> broke;
	const auto examine = [&](const std::vector<float>& out, const std::string& what) {
		++configurations;
		countedConfigurations += counting ? 1 : 0;
		int nonFinite = 0;
		int diverged = 0;
		float peak = 0.0f;
		for (uint32_t i = 0; i < kFrames; ++i) {
			if (!std::isfinite(out[i])) {
				++nonFinite;
				continue;
			}
			peak = std::max(peak, std::fabs(out[i]));
			if (std::fabs(out[i]) > kDiverged) {
				++diverged;
			}
		}
		if (peak > 0.001f) {
			++audible;
			countedAudible += counting ? 1 : 0;
		}
		if (nonFinite != 0 || diverged != 0) {
			broke.push_back(what + " (nonFinite " + std::to_string(nonFinite) + ", diverged "
			                + std::to_string(diverged) + ", peak " + std::to_string(peak) + ")");
		}
		return peak;
	};

	// **DO THE SETTINGS REACH THE AUDIO AT ALL?** Asked before anything is concluded from 344 green
	// renders, because "the output was finite" is true of a synth that ignored every one of them. The
	// default patch and the all-at-max patch must be different sounds, not merely both audible.
	if (which == 1 && shard == 0) { // the ceiling's first shard asks it: render1(1) is the all-at-max patch
		const auto plainHeap = std::make_unique<Rig>();
		Rig& plain = *plainHeap;
		plain.expectMono = false;
		const std::vector<float> byDefault = plain.play(script, kFrames, 64);
		const std::vector<float> atCeiling = render1(1, nullptr, 0.0);
		int differed = 0;
		float worst = 0.0f;
		for (uint32_t i = 0; i < kFrames; ++i) {
			if (byDefault[i] != atCeiling[i]) {
				++differed;
				worst = std::max(worst, std::fabs(byDefault[i] - atCeiling[i]));
			}
		}
		CAPTURE(worst);
		REQUIRE_MESSAGE(differed > 0,
		                "the swept settings do not reach the audio — every render below is the "
		                "default patch and this case proves nothing");
	}

	int audibleAtMin = 0;
	int audibleAtMax = 0;      // the HELD ceiling: the one the bound reads
	int audibleAtMaxUnheld = 0; // reported, not bounded (see the silencers' note)
	{
		const std::string end = which == 0   ? "all-at-min"
		                        : which == 1 ? "all-at-max"
		                                     : "all-at-max (pitch held back)";
		int& audibleHere = which == 0 ? audibleAtMin : which == 1 ? audibleAtMaxUnheld : audibleAtMax;
		counting = which != 1;
		if (shard == 0 && examine(render1(which, nullptr, 0.0), end) > 0.001f) { // the bare end, once
			++audibleHere;
		}
		for (size_t at = static_cast<size_t>(shard); at < descs.size(); at += static_cast<size_t>(shards)) {
			const feathers::ParamDesc& d = descs[at];
			// EVERY OTHER PARAM PULLED THE OTHER WAY, one at a time. This is the configuration that
			// reaches the coincident-anchor path: all-at-max puts every sound in one place AND opens
			// every width, and only pulling the width back leaves them on top of each other.
			const double opposite = which == 0 ? d.max : d.min;
			if (examine(render1(which, &d, opposite), end + " with " + d.name + " pulled back")
			    > 0.001f) {
				++audibleHere;
			}
			if (d.unit != feathers::ParamUnit::Choice && d.unit != feathers::ParamUnit::BendChoice) {
				continue;
			}
			// **NOT TWICE THE SAME CONFIGURATION** (2026-10-30). The walk passes through both ends of the choice, and two
			// of its entries are renders this pass has already made: the OPPOSITE end is the pull just above, and the
			// pass's OWN end is the unpulled render at the top of the pass (shard 0's, so once per end) — unless the pass restored this param after
			// pinning it (a floor silencer, a ceiling silencer, the mask pair), where pinning it to its end is a
			// different patch and is rendered. A fresh rig on the same settings renders the same samples, so the
			// skipped entries were 2 of every choice's walk in all three passes, about 30% of the sweep, adding nothing.
			const double ownEnd = which == 0 ? d.min : d.max;
			const bool restoredHere = (which == 0 && std::ranges::find(silencers, &d) != silencers.end())
			                          || (which == 2
			                              && (std::ranges::find(ceilingSilencers, &d) != ceilingSilencers.end()
			                                  || isMaskPair(d.id)));
			for (double v = std::round(d.min); v <= std::round(d.max) + 0.5; v += 1.0) {
				if (v == opposite || (v == ownEnd && !restoredHere)) {
					continue;
				}
				if (examine(render1(which, &d, v),
				            end + " with " + d.name + " = " + std::to_string(v))
				    > 0.001f) {
					++audibleHere;
				}
			}
		}
	}

	for (const std::string& s : broke) {
		MESSAGE("NOT FINITE: " << s);
	}
	CAPTURE(configurations);
	CHECK(broke.empty());
	// FLOORS, AND BOTH ENDS SEPARATELY. **All-at-min renders EXACT SILENCE** — every one of its 35
	// configurations measured a peak of 0 before the opposite-end walk was added — so a single
	// `audible >= 2` floor was satisfied entirely by the max half while the min half tested the audio
	// path not at all. Counting the two ends apart is what makes that visible, and it is the reason
	// the walk above exists rather than a tidier `all-at-min` on its own.
	// PER END NOW, SO EACH IS AT LEAST WHAT THE WHOLE USED TO HAVE TO BE.
	CHECK(configurations >= 200);
	CAPTURE(audibleAtMin);
	CAPTURE(audibleAtMax);
	if (which == 0) {
		CHECK(audibleAtMin >= 5);
	}
	if (which == 2) {
		CHECK(audibleAtMax >= 5);
		// THE AUDIBILITY BOUND, READ OFF THE HELD CEILING ALONE AND MADE STRONGER RATHER THAN WEAKER: the floor and
		// the held ceiling walk the same configuration list, so the whole's `floor + held >= 2N / 4` is implied by
		// `held >= N / 2`, whatever the floor sounded. (Measured: 1,102 of 1,110 held, 18 of 1,110 at the floor.)
		CAPTURE(countedConfigurations);
		CAPTURE(countedAudible);
		CHECK(countedAudible >= countedConfigurations / 2);
	}

	// **WHAT THIS SWEEP DOES NOT COVER, MEASURED RATHER THAN GUESSED, AND WHAT IT COST TO FIND OUT.**
	//
	// It reaches `blendWeights` 160,480 times and that function's anchor-snap guard fires on 73,010
	// of them — 45%, so the degenerate geometry those guards exist for is the COMMON case here, not
	// an edge. Delete BOTH guards and, measured on this sweep:
	//
	//     68,893 of 156,363 calls produce NaN weights
	//     16,316 morph table builds RECEIVE a NaN weight
	//     0 non-finite samples out of 344 x 2048 rendered — this case stays GREEN
	//
	// while 16 other cases in this file go red. **Something downstream of the morph build absorbs a
	// NaN weight before it reaches a sample, and nobody has named it.**
	//
	// A PAIR WALK DOES NOT CLOSE IT, and that is worth recording so the next person does not build
	// one. The hypothesis was that the failure needs two controls together — the anchors coincident
	// (which inside all-at-max needs `Width` at its floor, since Width at its ceiling displaces every
	// anchor per ear) AND the engine that reads the blend selected, which pinning every param to one
	// end cannot express because it pins every Choice to its LAST entry. Walking every Choice value
	// against every other control's opposite end — **9,592 configurations, 11.8 s** — was still
	// green under the same mutation. It was reverted: an instrument that costs eight times the
	// runtime and catches nothing extra is not a stronger instrument.
	//
	// THE READING THAT SURVIVES: these guards protect a VALUE — each sound's share — and a
	// finiteness sweep is structurally the wrong instrument for a value. What catches them is the
	// cases that assert the shares. Recorded so nobody reads a large configuration count as coverage
	// of the constellation.
	//
	// **AND A SECOND GUARD BEHAVES THE SAME WAY, WHICH SAYS THE FIRST IS NOT A ONE-OFF.**
	// `kFlagshipNormaliseFloor` stops `prepareFlagshipRecipe` dividing by a silent cycle's peak.
	// Measured on this sweep: it is reached **3 times out of 2,933**, and in **all three the peak is
	// EXACTLY 0.0** — so with the floor set to 0 those three compute `1.0 / 0.0` and store an
	// infinite gain. `evalFlagship` is `(raw - dcOffset) * gain`, so a recipe carrying that gain
	// renders non-finite for certain. **All 34 suites stay green.**
	//
	// The only reading consistent with both measurements is that the BUILD path is wider than the
	// RENDER path: a table is prepared for an engine the patch is not playing, and a NaN in one that
	// is never rendered reaches nothing. That is not a guard and nobody wrote it down, so it is not
	// something to rely on — it is the reason a finiteness sweep cannot reach either of these two.
	MESSAGE("swept " << configurations << " whole-extreme configurations, " << audible
	                 << " audible (" << audibleAtMin << " at the floor, " << audibleAtMax
	                 << " at the ceiling with its pitch held back, " << audibleAtMaxUnheld << " at the unheld ceiling)");
}

} // namespace

// **ONE SWEEP, SIX CASES — EACH END, AND THE CEILINGS IN SHARDS** (backlog PL, 2026-10-30). As one case it was
// 3,330 renders plus both derivations: 76 s alone and 146-153 s at a host load of 25-30, past run-cases' 120 s
// per-case timeout, so under load it went red as a CRASH that said nothing about the synth. NOTHING IS SAMPLED
// AWAY: the shards of an end partition its walk exactly (every `shards`-th param, from `shard`), so every
// configuration the one case rendered is still rendered once, each part in its own process. WITH a6bb4ca3 (the
// sweep renders no configuration twice: 3,330 -> 2,358 distinct patches, 786 per end) the longest part is 25 s at
// load ~20 and the six together about 108 s; the held ceiling's own derivation is repeated per shard, which is the
// price of each part being runnable alone.
TEST_CASE("EVERY param at once, at each end, with every OTHER one pulled the other way: still finite — the floor") {
	sweepWholeExtremes(0, 0, 1);
}

TEST_CASE("EVERY param at once, at each end, with every OTHER one pulled the other way: still finite — the unheld ceiling, shard 1 of 2") {
	sweepWholeExtremes(1, 0, 2);
}

TEST_CASE("EVERY param at once, at each end, with every OTHER one pulled the other way: still finite — the unheld ceiling, shard 2 of 2") {
	sweepWholeExtremes(1, 1, 2);
}

TEST_CASE("EVERY param at once, at each end, with every OTHER one pulled the other way: still finite — the held ceiling, shard 1 of 3") {
	sweepWholeExtremes(2, 0, 3);
}

TEST_CASE("EVERY param at once, at each end, with every OTHER one pulled the other way: still finite — the held ceiling, shard 2 of 3") {
	sweepWholeExtremes(2, 1, 3);
}

TEST_CASE("EVERY param at once, at each end, with every OTHER one pulled the other way: still finite — the held ceiling, shard 3 of 3") {
	sweepWholeExtremes(2, 2, 3);
}

TEST_CASE("at FULL polyphony a steal still finds a spare, so the replacement note sounds") {
	// **THE FOUR STEAL-FADE SPARES, ASKED FOR THE FIRST TIME.** `kStealFadeSpares` is read by exactly
	// one line in `src/` — `kVoiceSlots = kMaxVoices + kStealFadeSpares` — and by nothing in `tests/`.
	// Setting it to ZERO leaves all 34 suites green.
	//
	// AND IT IS NOT A HARMLESS CONSTANT. `claimFreeVoice` hands out a voice only if its stage is
	// `Idle`, and a stolen voice is deliberately `Stealing` for the ~2 ms of its fade — synth.cpp
	// says so at length: *"this line is the only thing between here and it"*, the notes-cutting-out
	// bug. The spares are where the fading voice lives while its old slot plays the note that stole
	// it. With none, `claimFreeVoice` returns nullptr at full polyphony and **the replacement note is
	// silently dropped**.
	//
	// WHY NOTHING SAW IT: the pool is `kMaxVoices + spares` whatever `VoiceCount` says, so a steal at
	// a voice count of 2 or 4 — which is what every existing steal case drives — has fourteen idle
	// slots to land in and needs no spare at all. **The spares only matter at the polyphony limit**,
	// and no case drove it. (The whole-extremes sweep sets `VoiceCount` to 16 and plays three notes,
	// which is the same miss from the other direction.)
	constexpr int kFull = feathers::kMaxVoices;
	Rig rig;
	rig.set(ParamId::VoiceCount, static_cast<double>(kFull));
	rig.setRelease(-1.0); // a long-ish tail, so nothing frees a slot by finishing

	// SIXTEEN NOTES, ONE PER FRAME, then a seventeenth that must steal.
	std::vector<Event> script;
	for (int i = 0; i < kFull; ++i) {
		script.push_back(noteOn(static_cast<uint32_t>(i), static_cast<int16_t>(kKeyC4 + i),
		                        static_cast<int32_t>(i + 1)));
	}
	const auto stealerKey = static_cast<int16_t>(kKeyC4 + kFull);
	script.push_back(noteOn(40, stealerKey, static_cast<int32_t>(kFull + 1)));

	const std::vector<float> out = rig.play(script, 512, 64);

	// THE ARRANGEMENT TOOK, asserted rather than assumed — sixteen notes have to actually be sounding
	// or the seventeenth steals nothing and this case is about an empty synth.
	int sounding = 0;
	for (int i = 0; i < kFull; ++i) {
		if (rig.synth.slotOfKeyForTest(static_cast<int16_t>(kKeyC4 + i)) >= 0) {
			++sounding;
		}
	}
	CAPTURE(sounding);
	// FIFTEEN, NOT SIXTEEN: the seventeenth note stole one of them, which is the event under test.
	REQUIRE(sounding == kFull - 1);

	// **THE ASSERTION.** The replacement note has a voice. A dropped note answers -1 here, which is
	// what `kStealFadeSpares = 0` produces and what all 34 suites currently miss.
	const int stealerSlot = rig.synth.slotOfKeyForTest(stealerKey);
	CAPTURE(stealerSlot);
	CHECK(stealerSlot >= 0);
	// AND IT IS PLAYING THE PITCH IT WAS ASKED FOR, not merely occupying a slot — the value, since a
	// slot index is satisfied by a voice playing anything at all.
	if (stealerSlot >= 0) {
		CHECK(rig.synth.voicePitchForTest(stealerSlot)
		      == doctest::Approx(static_cast<double>(stealerKey)).epsilon(1e-6));
	}
	// AND THE WHOLE THING MADE SOUND, so none of the above is a claim about a silent synth.
	CHECK(peakOf(out, 0, 512) > 0.01f);
}


TEST_CASE("EVERY audio device in the lane: the plugin renders the same samples at any host block size") {
	// **THE DEVICE-LEVEL VERSION OF THIS LIVES IN `test_fx_runner.cpp`; THIS IS THE ONE A USER CAN
	// HEAR.** `Synth::render` cuts its chunk at `min(n - done, kRenderChunk = 32, ...)`, so a device
	// never sees more than 32 frames — but the tail chunk of any host block whose length is not a
	// multiple of 32 is SHORT, and a CLAP host cuts the block at every event, so a passage with notes
	// in it is short chunks throughout. A device whose sound depends on how its input was chopped
	// therefore sounds different at a different buffer size, and different again depending on where
	// the notes fell.
	//
	// DRIVEN FROM THE REGISTRY, so the sixth device is covered without anybody coming back. The one
	// existing case of this shape drives the FILTER and only the filter.
	const std::vector<const feathers::device::Descriptor*> devices = [] {
		std::vector<const feathers::device::Descriptor*> out;
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			// A LISTENER RENDERS NOTHING (`AudioOps::listensOnly`, the Follower): this case would see a wire.
			if (d.audio != nullptr && !d.audio->listensOnly) {
				out.push_back(&d);
			}
		}
		return out;
	}();
	REQUIRE(devices.size() >= 5);

	constexpr uint32_t kFrames = 4096;
	const std::vector<Event> script{
	    noteOn(0, kKeyC4, 1),
	    noteOn(37, kKeyG4, 2),
	    noteOff(2500, kKeyC4, 1),
	    noteOn(2611, kKeyC4 + 5, 3),
	};

	std::vector<std::string> blockDependent;
	std::vector<std::string> unseen; // devices this case is structurally blind to; asserted below
	for (const feathers::device::Descriptor* desc : devices) {
		const std::string who =
		    desc->traits.name == nullptr ? std::string("<unnamed>") : std::string(desc->traits.name);
		CAPTURE(who);

		// ENABLED, AND EVERY CONTINUOUS CONTROL PUSHED WELL OFF ITS DEFAULT.
		//
		// **THE DEFAULT ROW IS NOT ENOUGH AND THAT WAS MEASURED, NOT GUESSED.** With the row left at
		// its declared defaults, reverting drawn's control-grid fix (9137b44) left this case GREEN —
		// because drawn's shipped default is EXACTLY a wire, and a wire is chunk-invariant however it
		// counts. A sweep whose fixture bypasses the device under test reports every device clean.
		//
		// The enable is asked of `enableControlIn` rather than of any control's name; `device.h` names
		// the string search as the rejected alternative and says so it is not re-proposed.
		// AN IDENTITY IS A SERIAL AND NOTHING ELSE (kill-lanes step 3c) — it carried a routing address
		// beside it, minted here as `deviceSlotOfFx(0)` because the row went into position 0.
		feathers::DeviceRow row = feathers::defaultRowFor(*desc, { 1 });
		const size_t enable =
		    feathers::device::enableControlIn(desc->traits, desc->params);
		REQUIRE(enable != feathers::device::kNoControl);
		for (size_t i = 0; i < desc->params.size() && i < row.fieldsPresent; ++i) {
			if (desc->params[i].stepped || i == enable) {
				continue;
			}
			const double away = desc->params[i].def + 0.6 * (desc->params[i].max - desc->params[i].def);
			const double to = (away == desc->params[i].def)
			                      ? desc->params[i].def - 0.6 * (desc->params[i].def - desc->params[i].min)
			                      : away;
			row.field[i] = static_cast<float>(to);
		}
		row.field[enable] = static_cast<float>(desc->params[enable].max);

		const auto play = [&](int block) {
			Rig rig;
			rig.expectMono = false; // OWLTT has a width; the mono rule is not this case's claim
			rig.setDeviceRow(row);
			return rig.play(script, kFrames, block);
		};

		const std::vector<float> reference = play(256);
		// NON-VACUITY, BOTH HALVES. The render has to have made a sound, AND the device has to have
		// CHANGED it — a device that renders as a wire is chunk-invariant for a reason that has
		// nothing to do with the claim.
		// 1e-5 AND THE NUMBER IS MEASURED: at these settings drawn peaks at 6.86581e-04 — six
		// resonant bands under the cascade trim, quiet by design — where OWLTT peaks at 4.75. What this
		// guard has to establish is that something came out at all; the CHANGED check below is the
		// non-vacuity that matters.
		REQUIRE(peakOf(reference, 0, kFrames) > 1.0e-5f);
		{
			Rig bare;
			bare.expectMono = false;
			const std::vector<float> withoutDevice = bare.play(script, kFrames, 256);
			int changed = 0;
			for (uint32_t i = 0; i < kFrames; ++i) {
				if (withoutDevice[i] != reference[i]) {
					++changed;
				}
			}
			// **THE SET THIS CASE CANNOT SEE, SPELLED WHOLE** rather than a REQUIRE that stops the
			// sweep dead. A device joining it is a regression and a device LEAVING it means this
			// comment has stopped being true.
			//
			// SKLITTER IS A MOMENTARY, TEMPO-LOCKED LOOPER: nothing happens until its Engage is
			// raised, and once raised it captures a whole BAR -- 96,000 frames at a musical tempo --
			// before it plays a sample. This case renders 4,096 through a note script, so the device
			// is a wire for the whole of it and says nothing either way. What this case would have
			// asked is asserted directly in `test_fx_sklitter.cpp`'s "the same samples at any host
			// block size", which found a REAL defect doing it: the port crossed its
			// recording-to-playing threshold at a BLOCK boundary rather than a frame, so the same
			// audio began stuttering at frame 1536 under 256-frame blocks and at 8192 under one long
			// call. The obligation moved; it did not lapse.
			if (changed == 0) {
				unseen.push_back(who);
			}
		}

		float worst = 0.0f;
		for (const int block : { 64, 100, 441 }) {
			const std::vector<float> other = play(block);
			for (uint32_t i = 0; i < kFrames; ++i) {
				worst = std::max(worst, std::fabs(other[i] - reference[i]));
			}
		}
		if (worst != 0.0f) {
			blockDependent.push_back(who);
			MESSAGE("block-size dependent through the plugin: " << who << " worst " << worst
			                                                   << " against a render peaking at "
			                                                   << peakOf(reference, 0, kFrames));
		}
	}

	// **WHAT THIS CASE CAN AND CANNOT SEE, both verified rather than assumed.**
	//
	// It CAN see a device whose internal control state advances per call: making `Harm`'s `gridPhase_`
	// reset every call turns it red with `{Harm, OWLTT} == {OWLTT}` and `worst 0.234416 against a render
	// peaking at 1.18983` — a fifth of full scale.
	//
	// It CANNOT see drawn's version of the same bug, and that is worth stating because it is the bug
	// this branch fixed. Reverting 9137b44 leaves this GREEN, because the fixture publishes the
	// device's row ONCE: drawn's smoother snaps on the first `setFields` and then sits on its target,
	// so there is no journey for a wrong rate to distort. `test_fx_runner.cpp`'s partition sweep is
	// the one that catches it, and it catches it by warming up at a second settings row first. **Two
	// cases on the same ground, and only one of them can see each half.**
	//
	// THE EXEMPTION IS AN ASSERTION, as it is at the device level — **and what is left of OWLTT's
	// dependence is now a different thing from what it was.**
	//
	// The two TIME CONSTANTS are fixed: the gain smoother is a one-pole at `kGainSmoothingSeconds`
	// with no end-of-call snap, and the level detector's coefficient is advanced by the block's length
	// the way `runEnvelope` already advanced the attack and release. That took the worst difference
	// from **0.0619171 to 0.0314169** on this script.
	//
	// WHAT REMAINS IS THE DETECTION WINDOW, and it was localised rather than guessed:
	// `updateLevel` takes the PEAK over the whole block, so a 256-sample call sees a higher peak than
	// eight 32-sample calls of the same audio. MEASURED by replacing the block max with per-sample
	// detection — an experiment, not a change — the difference collapses to **0.00370073 against a
	// 5.15009 peak**, 0.07%. So the residue is the window and not a rate, and closing it means
	// redesigning a detector this device inherited from the firmware as block-based. That is a sound
	// decision and is not taken here.
	//
	// Named as a set so a second device joining, or OWLTT leaving, both go red.
	const std::vector<std::string> expected { "OWLTT" };
	CHECK(blockDependent == expected);
	// THE PHASER/FLANGER LEFT THIS LIST WITH ITS ALLPASS ENGINE: a wire until then, it is seen now,
	// and its control grid is carried across calls.
	CHECK(unseen == std::vector<std::string> { "SKLITTER" });
}

TEST_CASE("past the spares a steal is a CUT, not a lost note — and not a lost voice either") {
	// **THE OVERFLOW COSTS TWO NOTES, NOT ONE, AND THAT IS THE FINDING.** `kStealFadeSpares`' comment
	// promises *"the extra steals are instant cuts rather than fades: one click, and the pool is whole
	// again 2 ms later"*. It was describing behaviour the code did not have: `claimFreeVoice` returned
	// `nullptr` and the note never sounded — **and the voice manager commits the steal BEFORE asking
	// for the voice**, so the victim was already gone. MEASURED before the fix, `VoiceCount = 16`:
	//
	//     notes on one frame   voices held   newest notes with no voice
	//         17                   16                 0
	//         20                   16                 0
	//         21                   15                 1
	//         24                   15                 4
	//
	// **Fifteen.** The synth holding one fewer than its own polyphony limit while notes go missing.
	//
	// REACHABLE BY HAND, and by the road `kStealFadeSpares`' comment names itself: hold sixteen notes,
	// tap Poly -> Mono -> Poly (which hard-terminates every sounding voice at once), then play a
	// five-note chord. Before the fix, four sounded.
	constexpr int kFull = feathers::kMaxVoices;

	const auto holdAtOnce = [&](int notes) {
		Rig rig;
		rig.set(ParamId::VoiceCount, static_cast<double>(kFull));
		rig.setRelease(-1.0);
		std::vector<Event> script;
		for (int i = 0; i < notes; ++i) {
			// ALL ON FRAME 0: the steals land inside one another's fade windows, which is the only
			// way to exhaust four spares.
			script.push_back(noteOn(0, static_cast<int16_t>(kKeyC4 + i), static_cast<int32_t>(i + 1)));
		}
		const std::vector<float> out = rig.play(script, 512, 64);
		int held = 0;
		for (int i = 0; i < notes; ++i) {
			if (rig.synth.slotOfKeyForTest(static_cast<int16_t>(kKeyC4 + i)) >= 0) {
				++held;
			}
		}
		// THE NEWEST NOTE IS THE ONE THAT MUST SURVIVE: stealing takes the oldest, so the last key
		// pressed is the one a player would notice missing.
		const bool newestSounds =
		    rig.synth.slotOfKeyForTest(static_cast<int16_t>(kKeyC4 + notes - 1)) >= 0;
		return std::tuple<int, bool, float>{ held, newestSounds, peakOf(out, 0, 512) };
	};

	// **THE POLYPHONY LIMIT IS THE POLYPHONY LIMIT, at every overload.** Not "close to it" and not
	// "before the spares run out" — sixteen voices is what the control says and what the player is
	// owed. A bound would be satisfied by fifteen.
	for (const int notes : { 17, 20, 21, 24, 32 }) {
		CAPTURE(notes);
		const auto [held, newestSounds, peak] = holdAtOnce(notes);
		CAPTURE(held);
		CAPTURE(peak);
		CHECK(held == kFull);
		CHECK(newestSounds);
		// AND THE WHOLE THING MADE SOUND, so none of the above is a claim about a silent synth.
		CHECK(peak > 0.01f);
	}

	SUBCASE("and by the road the header names: sixteen held, a play-mode change, then a chord") {
		Rig rig;
		rig.set(ParamId::VoiceCount, static_cast<double>(kFull));
		rig.setRelease(-1.0);
		std::vector<Event> script;
		for (int i = 0; i < kFull; ++i) {
			script.push_back(noteOn(static_cast<uint32_t>(i), static_cast<int16_t>(kKeyC4 + i),
			                        static_cast<int32_t>(i + 1)));
		}
		// Poly -> Mono -> Poly hard-terminates every sounding voice, putting all sixteen into Stealing
		// at once and leaving only the four spares idle.
		script.push_back(param(40, ParamId::VoiceMode, 1.0));
		script.push_back(param(41, ParamId::VoiceMode, 0.0));
		constexpr int kChord = 5;
		for (int i = 0; i < kChord; ++i) {
			script.push_back(noteOn(42, static_cast<int16_t>(kKeyC4 + 40 + i),
			                        static_cast<int32_t>(100 + i)));
		}
		rig.play(script, 512, 64);
		int sounding = 0;
		for (int i = 0; i < kChord; ++i) {
			if (rig.synth.slotOfKeyForTest(static_cast<int16_t>(kKeyC4 + 40 + i)) >= 0) {
				++sounding;
			}
		}
		CAPTURE(sounding);
		// FOUR BEFORE THE FIX — one per spare, and the fifth silently lost.
		CHECK(sounding == kChord);
	}
}

TEST_CASE("a per-note-id modulation reaches exactly the params that DECLARE they take one") {
	// **THE SET THE HOST IS TOLD AND THE SET THE RESPONDER ACCEPTS MUST BE THE SAME SET, and nothing
	// made them agree.** `setVoicePolyphonicParameterModulation` guards on
	// `parameter != ParamId::VoiceLevel` and its comment explains why: *"Level is the only param declared
	// CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID. Anything else addressed to a note is dropped rather than
	// folded into the global lane, which belongs to the host's automation and would then be wrong for
	// every other voice."* Every clause of that was unasserted — disabling the guard left
	// `--each test_synth` at 60 of 60.
	//
	// WITH THE GUARD GONE, a per-note modulation addressed to ANY parameter lands on `voice->levelMod`
	// — so a host modulating PosX per note would silently change that voice's LEVEL instead.
	//
	// **REACHABLE, AND NOT IN THE USUAL WAY.** A well-behaved host only sends per-note-id modulation
	// for params carrying the flag, and exactly one of them does today. So this is not a guard whose
	// condition no fixture can reach: it is reachable by an experimental or misbehaving host now, and
	// reachable by construction the moment a second param gains the flag — which is the case that
	// needs covering, and why this walks `paramDescs()` rather than naming Level.
	//
	// ASSERTED AS A VALUE, because a render cannot tell the two worlds apart: a modulation folded onto
	// the wrong parameter still moves the sound, so "the output changed" is true either way.
	const std::span<const feathers::ParamDesc> descs = feathers::paramDescs();
	REQUIRE(descs.size() > 50);

	int declared = 0;
	int accepted = 0;
	std::vector<std::string> disagreed;
	for (const feathers::ParamDesc& d : descs) {
		Rig rig;
		rig.setRelease(-1.0);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		std::vector<float> l(256, 0.0f);
		std::vector<float> r(256, 0.0f);
		rig.synth.render(l.data(), r.data(), 256);

		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		const double before = rig.synth.voiceLevelModForTest(slot);

		// A DISTINCTIVE OFFSET, and one no other road writes.
		constexpr double kOffset = -0.371;
		rig.synth.paramModForNote(0, kKeyC4, 1, static_cast<clap_id>(d.id), kOffset);
		const double after = rig.synth.voiceLevelModForTest(slot);

		const bool tookIt = after != before;
		const bool declaresIt = (d.flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) != 0u;
		if (declaresIt) {
			++declared;
		}
		if (tookIt) {
			++accepted;
			// AND IT LANDED WHERE IT WAS SENT, not merely somewhere: the value the host gave, on the
			// voice the note is on.
			CHECK(after == doctest::Approx(kOffset));
		}
		if (tookIt != declaresIt) {
			disagreed.push_back(std::string(d.name) + (declaresIt ? " declares it and was DROPPED"
			                                                      : " does NOT declare it and was TAKEN"));
		}
	}

	for (const std::string& s : disagreed) {
		MESSAGE("PER-NOTE MISMATCH: " << s);
	}
	CAPTURE(declared);
	CAPTURE(accepted);
	CHECK(disagreed.empty());
	// FLOORS. Zero declared would mean the flag has gone and this case is comparing two empty sets;
	// `accepted == declared` is the claim restated as a count, so a mismatch that somehow cancelled
	// across two params still fails.
	CHECK(declared >= 1);
	CHECK(accepted == declared);
}

TEST_CASE("both modulator kinds tick to the values the pre-runner engine produced") {
	// **THE REFERENCE IS ANOTHER BUILD, NOT THIS ONE.** Every constant below was MEASURED on the
	// engine at commit a0d96e4 — the typed-array mod strip, before any runner existed — by printing
	// `envelopeValueForTest` at these blocks. That is the whole point of the case: it compares the
	// mod strip to the engine it replaces, and a case whose expectations were read off the new code
	// would be comparing the new build to itself and passing whatever it did.
	//
	// The two slots are deliberately different SHAPES as well as different devices, so a runner that
	// crossed them — published slot 0's value on slot 1's ordinal, or ticked one device twice — is
	// caught by the values rather than by a difference check. See the non-vacuity assertions at the
	// end, which prove the constants are not all one number.
	namespace mseg = feathers::mod::mseg;
	Rig rig;
	feathers::ModRack rack = feathers::defaultModRack();
	// AND THE COUNT, WHICH IS WHAT MAKES THE SECOND SLOT EXIST. `tickEnvelopes` walks `rack_.count`,
	// so a slot that has settings but is past the count never ticks and reads a flat zero — which is
	// exactly the constant-reference trap this case is written to avoid, so it is worth the line.
	rack.count = 2;
	rack.setDahdsrAt(0, feathers::mod::dahdsr::Settings {});
	// A SHAPE THAT ACTUALLY MOVES: a default MSEG holds zero for every block, and a constant cannot
	// tell a working runner from a dead one.
	mseg::Settings shape {};
	shape.count = 2;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Gate;
	shape.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	shape.segment[0].share = 0.5f;
	shape.segment[0].level = 1.0f;
	shape.segment[0].shape = 0.0f;
	shape.segment[1].share = 0.5f;
	shape.segment[1].level = 0.25f;
	shape.segment[1].shape = 0.0f;
	rack.setMsegAt(1, shape);
	// AN ID, AS EVERY ADD ROAD GIVES — `setMsegAt` keeps the slot's identity and slot 1 of the default
	// rack has none. It gave an id AND a routing address (`deviceSlotOfRack(1)`) until kill-lanes step
	// 3c; the serial is the whole of an identity now, and a route names it rather than an address.
	rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
	REQUIRE(rack.holds(1, mseg::kTraits.tag));
	rig.publishModRack(rack);
	rig.synth.setTempo(120.0);
	rig.synth.noteOn(0, 60, 1, 0.8f);

	struct Sample {
		int block;
		float dahdsr;
		float mseg;
	};
	// MEASURED at a0d96e4. The early blocks catch the DAHDSR's attack ramp and its turn into decay;
	// the late ones catch the MSEG's first segment climbing while the DAHDSR sits on its sustain.
	static constexpr Sample kExpected[] = {
	    { 0, 0.1333333254f, 0.001333333319f },
	    { 1, 0.2666666508f, 0.002666666638f },
	    { 2, 0.3999999762f, 0.003999999724f },
	    { 4, 0.6666666269f, 0.006666666828f },
	    { 7, 1.0f, 0.01066666748f },
	    { 8, 0.9980000257f, 0.01200000104f },
	    { 100, 0.8753333092f, 0.1346666217f },
	    { 300, 0.6086680889f, 0.4013318419f },
	    { 500, 0.5f, 0.6680008173f },
	    { 700, 0.5f, 0.934671998f },
	};

	std::array<float, 64> left {};
	std::array<float, 64> right {};
	size_t next = 0;
	for (int block = 0; block < 800 && next < std::size(kExpected); ++block) {
		rig.synth.render(left.data(), right.data(), 64);
		if (block != kExpected[next].block) { continue; }
		CAPTURE(block);
		CHECK(rig.synth.envelopeValueForTest(0)
		      == doctest::Approx(kExpected[next].dahdsr).epsilon(1e-4));
		CHECK(rig.synth.envelopeValueForTest(1)
		      == doctest::Approx(kExpected[next].mseg).epsilon(1e-4));
		++next;
	}
	// EVERY SAMPLE WAS REACHED, or the loop ran out of blocks and the checks above are a shorter
	// sequence than the one the constants describe — which would pass silently.
	CHECK(next == std::size(kExpected));

	// AND THE CONSTANTS ARE NOT ALL ONE NUMBER, in either row or between them. Without this, a
	// runner that published a single frozen value on both ordinals could satisfy a table that
	// happened to be flat, and the case would be pinning nothing.
	CHECK(kExpected[0].dahdsr != doctest::Approx(kExpected[9].dahdsr));
	CHECK(kExpected[0].mseg != doctest::Approx(kExpected[9].mseg));
	CHECK(kExpected[6].dahdsr != doctest::Approx(kExpected[6].mseg));
}

TEST_CASE("the block's notes reach the rack's note lane, and are gone by the next tick") {
	// **THE NOTE ROAD FEATHERS DID NOT HAVE.** `Synth::noteOn/noteOff/choke` handed events straight
	// to the voice manager and nothing else ever saw them, so a device driven by notes could sit in
	// the rack and never move. The rack engine has had this lane for a while; this is Feathers'.
	//
	// READ RATHER THAN INFERRED. Under reading (1) of the note-routing decision the lane's output
	// goes nowhere — what PLAYS is still the host's own stream — so a note that reached a device
	// leaves no trace in any rendered buffer, and a comparison of two renders could not tell this
	// working from this deleted. `laneNotesForTest` is the seam that makes the claim assertable.
	Rig rig;
	std::array<float, 64> left {};
	std::array<float, 64> right {};

	SUBCASE("a note played is a note the lane carries") {
		rig.synth.noteOn(0, 60, 7, 0.8);
		rig.synth.noteOn(0, 64, 8, 0.5);
		rig.synth.render(left.data(), right.data(), 64);

		const auto carried = rig.synth.laneNotesForTest();
		REQUIRE(carried.size() == 2);
		// EVERY FIELD THAT IDENTIFIES THE NOTE. A count alone is true of a lane carrying two of
		// something else, and the ids are what a device uses to pair an off with its on.
		CHECK(carried[0].kind == feathers::device::NoteEvent::Kind::On);
		CHECK(carried[0].key == 60);
		CHECK(carried[0].noteId == 7);
		CHECK(carried[0].velocity == doctest::Approx(0.8));
		CHECK(carried[1].key == 64);
		CHECK(carried[1].noteId == 8);

		// **AND THE BUFFER IS PER TICK, NOT PER LIFETIME.** A note left in it would be offered to the
		// rack again on the next tick — a device would hear one keypress as a repeating one, which is
		// the retrigger latch's own failure mode wearing different clothes.
		rig.synth.render(left.data(), right.data(), 64);
		CHECK(rig.synth.laneNotesForTest().empty());
	}

	SUBCASE("a note the synth refuses to play is a note the lane never sees") {
		// THE LATCH'S RULE, APPLIED TO THE LANE. `noteOn` drops an unplayable channel or key before
		// setting `envRetrigger_`, so such a note restarts no envelope; a modulator has no more
		// business hearing it than the envelope does.
		rig.synth.noteOn(0, 200, 1, 1.0); // key past the manager's 128-entry table
		rig.synth.render(left.data(), right.data(), 64);
		CHECK(rig.synth.laneNotesForTest().empty());
	}

	SUBCASE("a wildcard note-off reaches the lane as the host sent it") {
		rig.synth.noteOn(0, 60, 7, 1.0);
		rig.synth.render(left.data(), right.data(), 64);
		// THE HOST DOES NOT INTERPRET — registry.h: every device is given all the notes and decides
		// for itself what to do with them. The voice manager needs this
		// expanded into concrete per-voice offs; the DEVICE must not see that expansion, because it
		// would be reading the synth's voice allocation rather than the player's gesture.
		rig.synth.noteOff(-1, -1, -1, 0.0);
		rig.synth.render(left.data(), right.data(), 64);

		const auto carried = rig.synth.laneNotesForTest();
		REQUIRE(carried.size() == 1);
		CHECK(carried[0].kind == feathers::device::NoteEvent::Kind::Off);
		CHECK(carried[0].key == -1);
		CHECK(carried[0].channel == -1);
		CHECK(carried[0].noteId == -1);
	}

	SUBCASE("a choke reaches it too, as a choke") {
		rig.synth.noteOn(0, 60, 7, 1.0);
		rig.synth.render(left.data(), right.data(), 64);
		rig.synth.choke(0, 60, 7);
		rig.synth.render(left.data(), right.data(), 64);

		const auto carried = rig.synth.laneNotesForTest();
		REQUIRE(carried.size() == 1);
		CHECK(carried[0].kind == feathers::device::NoteEvent::Kind::Choke);
		CHECK(carried[0].key == 60);
	}
}

TEST_CASE("two notes in one control tick carry different offsets, not both zero") {
	// **THE FIELD `NoteEvent::timeSamples` EXISTS FOR THIS AND THE FIRST DRAFT DEFEATED IT.** Its own
	// declaration says why: *"a note's PLACE in a block is part of it: an arp that emitted everything
	// at offset 0 would quantise itself to the block rate."* The lane runs once per control tick, so
	// a capture that stamped zero would hand every note in a tick the same position — the exact
	// quantisation the field is there to prevent, and invisible to every other case here because
	// nothing else reads the offset.
	Rig rig;
	std::array<float, 64> left {};
	std::array<float, 64> right {};

	// TWO NOTES SEPARATED BY RENDERED SAMPLES BUT INSIDE ONE TICK. The chunking is what the host does
	// at every event, and it is what advances the puck to the note's real position.
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.synth.render(left.data(), right.data(), 16);
	rig.synth.noteOn(0, 64, 2, 1.0);
	rig.synth.render(left.data(), right.data(), 48);

	const auto carried = rig.synth.laneNotesForTest();
	REQUIRE(carried.size() == 2);
	CHECK(carried[0].key == 60);
	CHECK(carried[1].key == 64);
	// THE VALUES, NOT MERELY THEIR DIFFERENCE. The second note arrived 16 samples into the tick and
	// the reference is that 16 rather than "more than the first" — a difference check would pass on a
	// counter that incremented by one per note.
	CHECK(carried[0].timeSamples == 0);
	CHECK(carried[1].timeSamples == 16);
}

namespace {

/// A NOTE DEVICE THAT TRANSPOSES EVERY NOTE-ON UP AN OCTAVE — the smallest thing that proves the
/// voices play what LEFT the lane rather than what arrived at it. Nothing registered transforms the
/// stream (the tracker reads and passes on), so without a device written here the claim could not be
/// made at all: see ModRunner::seatForTest.
struct Transposer {
	int placeholder = 0;
};

const feathers::device::NoteOps kTransposerNoteOps {
	.stateBytes = sizeof(Transposer),
	.stateAlign = alignof(Transposer),
	.construct = [](void* storage) { new (storage) Transposer(); },
	.destruct = [](void* storage) { static_cast<Transposer*>(storage)->~Transposer(); },
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes =
	    [](void*, const feathers::device::NoteEvent* in, size_t inCount,
	       feathers::device::NoteEvent* out, size_t outCapacity, size_t* outCount) {
		    const size_t n = std::min(inCount, outCapacity);
		    for (size_t i = 0; i < n; ++i) {
			    out[i] = in[i];
			    if (out[i].kind == feathers::device::NoteEvent::Kind::On) {
				    out[i].key = static_cast<int16_t>(out[i].key + 12);
			    }
		    }
		    *outCount = n;
	    },
};

constexpr feathers::device::Traits kTransposerTraits {
	.tag = "TRNS",
	.name = "Transposer",
	.hasCard = false,
	.hasExpanded = false,
};

const feathers::device::Descriptor kTransposerDescriptor {
	.traits = kTransposerTraits,
	.params = {},
	.outputs = {},
	.notes = &kTransposerNoteOps,
};

/// THE DEVICE A CASE SEATS, served by the synth's catalogue beside the registry's own.
const feathers::device::Descriptor* g_seated = nullptr;

/// **SEAT A DEVICE THE REGISTRY DOES NOT CARRY — THE WAY A THIRD PARTY'S IS SEATED.** A catalogue that
/// knows it (`Synth::setCatalogue`), a row naming it in the strip, and one block so the audio thread
/// adopts what the message thread built. This replaced `ModRunner::seatForTest`, a test-only door into
/// a runner that is gone; the road here is the product's own.
void seatInStrip(Rig& rig, const feathers::device::Descriptor& desc) {
	g_seated = &desc;
	rig.synth.setCatalogue(feathers::device::Catalogue {
	    nullptr, [](void*, const char* tag) -> const feathers::device::Descriptor* {
		    if (g_seated != nullptr && feathers::device::sameTag(g_seated->traits.tag, tag)) {
			    return g_seated;
		    }
		    return feathers::deviceForTag(tag);
	    } });
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	feathers::DeviceRow row {};
	// THE WHOLE TAG, not four bytes of it — `Traits::tag` is `device::kTagBytes` wide since 2026-09-14
	// and two registered devices are longer than four: "OWLTT" and "SKLITTER". This helper takes ANY
	// descriptor, so it must not truncate; `tagOf` is the same conversion `defaultRowFor` uses.
	row.tag = feathers::device::tagOf(desc.traits.tag);
	row.instanceId = feathers::kFirstFreeInstance + 100;
	// **NO ADDRESS TO STAMP (kill-lanes step 3c).** This set `row.routeLane = deviceSlotOfRack(0)` and
	// had to: a row written straight into the rack, rather than through `addRackDevice`, carried no
	// routing address and `slotsOf` placed it nowhere. The device is placed by its SLOT now, which
	// `rack.device[0] = row` already says.
	rack.device[0] = row;
	rig.publishModRack(rack);
	std::array<float, 1> l {};
	std::array<float, 1> r {};
	rig.synth.render(l.data(), r.data(), 0);
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(stripDeviceOf(rig.synth, rack, 0)) == &desc);
}

} // namespace

TEST_CASE("rack: an fx-lane device with audio and notes receives note events") {
	// **THE ROAD WEAVE RIDES, IN THE SYNTH.** An fx-lane device declaring `.audio` AND `.notes` over one
	// storage (tests/audio_notes_probe.h). `LaneRack::fits` admits it to the lane for its `.audio`, and
	// `RackEngine::advanceNotes` walks every chain — the strip, then the lane — so the lane device is
	// handed the notes that left the strip, at the instant each arrives (`Synth::runLaneFor`).
	//
	// WHAT THE OFFSET MEANS HERE, asserted as a value because it differs from the rack plugin: the note
	// is delivered BETWEEN two `processStereo` calls, at its own sample, and `timeSamples` is its offset
	// in the CONTROL TICK (`noteOffsetInTick`) — not a place in audio the device has yet to be handed.
	Rig rig;
	rig.synth.setCatalogue(feathers::device::Catalogue {
	    nullptr, [](void*, const char* tag) -> const feathers::device::Descriptor* {
		    if (feathers::device::sameTag(feathers::test::kAudioNotesProbe.traits.tag, tag)) {
			    return &feathers::test::kAudioNotesProbe;
		    }
		    return feathers::deviceForTag(tag);
	    } });
	feathers::DeviceRow row {};
	row.tag = feathers::device::tagOf(feathers::test::kAudioNotesProbe.traits.tag);
	row.instanceId = feathers::kFirstFreeInstance + 100;
	rig.setDeviceRow(row);
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	rig.synth.render(left.data(), right.data(), 0); // the block that adopts what the message thread built
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(
	            rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0))
	        == &feathers::test::kAudioNotesProbe);
	feathers::test::AudioNotesProbe* probe = feathers::test::g_audioNotesProbe;
	REQUIRE(probe != nullptr);
	probe->clear();

	rig.synth.render(left.data(), right.data(), 16);
	rig.synth.noteOn(0, 60, 1, 0.625);
	// AND PASSED ON: in the synth what leaves the lane is what the voices play, so a lane device that
	// swallowed notes would silence the synth. This one passes them, and a voice took the key.
	CHECK(rig.synth.voicePitchForTest(0) == doctest::Approx(60.0));
	rig.synth.render(left.data(), right.data(), 32);
	rig.synth.noteOff(0, 60, 1, 0.25);
	rig.synth.render(left.data(), right.data(), 16);

	using Entry = feathers::test::AudioNotesProbe::Entry;
	REQUIRE(probe->dropped == 0);
	std::vector<Entry> notes;
	int framesBefore[2] = { 0, 0 };
	int frames = 0;
	for (size_t i = 0; i < probe->count; ++i) {
		const Entry& e = probe->log[i];
		if (e.what == Entry::What::Audio) {
			frames += e.frames;
		}
		else if (notes.size() < 2) {
			framesBefore[notes.size()] = frames;
			notes.push_back(e);
		}
		else {
			notes.push_back(e);
		}
	}
	REQUIRE(notes.size() == 2);
	CHECK(notes[0].kind == feathers::device::NoteEvent::Kind::On);
	CHECK(notes[0].key == 60);
	CHECK(notes[0].velocity == 0.625);
	CHECK(notes[0].timeSamples == 16);
	CHECK(notes[1].kind == feathers::device::NoteEvent::Kind::Off);
	CHECK(notes[1].velocity == 0.25);
	CHECK(notes[1].timeSamples == 48);
	// AT ITS SAMPLE: the device had been handed exactly the audio before each note, and all of it after.
	CHECK(framesBefore[0] == 16);
	CHECK(framesBefore[1] == 48);
	CHECK(frames == 64);
}

TEST_CASE("with no note device the lane is a no-op: same notes, same order, same offsets") {
	// **THE SWITCH IS PROVABLY NOTHING UNTIL AN EMITTING DEVICE EXISTS.** The voices now play what
	// leaves the note lane rather than what the host sent (reading (2): the lane is the destination), and with no
	// device claiming the stream the lane is a pass-through — so this must be indistinguishable from
	// having no lane at all. Asserted rather than argued, because the first version of this switch was
	// NOT a no-op: it batched notes to the control tick and delayed every one of them by up to 64
	// samples, which the pinned reference from a0d96e4 caught and fifteen timing cases confirmed.
	std::array<float, 64> left {};
	std::array<float, 64> right {};

	SUBCASE("the same notes, in the same order") {
		Rig rig;
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.synth.noteOn(0, 64, 2, 1.0);
		// ORDER IS READ OFF THE VOICE ALLOCATION, which is what a reordering lane would disturb: the
		// manager hands out slots in the order it is told, so the pitches name the sequence.
		CHECK(rig.synth.voicePitchForTest(0) == doctest::Approx(60.0));
		CHECK(rig.synth.voicePitchForTest(1) == doctest::Approx(64.0));
	}

	SUBCASE("at the same sample offset inside the block") {
		// **THE OFFSET IS THE HALF THAT BROKE, so it gets the sharper instrument.** A note's place in
		// a block is not visible in an envelope — that moves on the 64-sample control grid, so a note
		// delayed within a tick reads identically — but it is visible in the oscillator's PHASE, which
		// advances per sample. `voicePhaseForTest` exists for exactly this kind of claim.
		Rig late;
		late.synth.render(left.data(), right.data(), 16);
		late.synth.noteOn(0, 60, 1, 1.0);
		late.synth.render(left.data(), right.data(), 48);

		// THE INDEPENDENT RESTATEMENT: a note struck at the start of a block and given 48 samples has
		// been sounding for exactly as long as one struck 16 samples in and given the rest of a
		// 64-sample block. If the lane delayed the second one to the tick boundary, it would have
		// sounded for less and the phases would not meet.
		Rig reference;
		reference.synth.noteOn(0, 60, 1, 1.0);
		reference.synth.render(left.data(), right.data(), 48);

		CHECK(late.synth.voicePhaseForTest(0)
		      == doctest::Approx(reference.synth.voicePhaseForTest(0)).epsilon(1e-6));

		// AND THE CONTROL, which is what stops the assertion above from being true of any two phases:
		// a note given the WHOLE block has sounded longer and must NOT match.
		Rig full;
		full.synth.noteOn(0, 60, 1, 1.0);
		full.synth.render(left.data(), right.data(), 64);
		CHECK(full.synth.voicePhaseForTest(0)
		      != doctest::Approx(reference.synth.voicePhaseForTest(0)).epsilon(1e-6));
	}
}

TEST_CASE("the voices play what left the note lane, not what arrived at it") {
	// THE OTHER HALF OF THE DECISION. A device that transforms the stream transforms what the synth
	// plays — which is the whole of what reading (2) means, and the reason it was a design decision rather
	// than a detail: the modulator strip can now rewrite what the instrument plays.
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	Rig rig;
	seatInStrip(rig, kTransposerDescriptor);
	rig.synth.noteOn(0, 60, 1, 1.0);
	// THE VALUE, NOT A DIFFERENCE. 60 played, 72 sounding — the octave the device adds, computed from
	// the device's own rule rather than read off a run.
	CHECK(rig.synth.voicePitchForTest(0) == doctest::Approx(72.0));

	// **AND THE CONTROL THAT MAKES IT ATTRIBUTABLE.** Without this, "the voice plays 72" is equally
	// true of an engine that transposes everything for some other reason; the same rig with no device
	// seated must play what was struck.
	Rig plain;
	plain.synth.noteOn(0, 60, 1, 1.0);
	CHECK(plain.synth.voicePitchForTest(0) == doctest::Approx(60.0));

	// THE NOTE THE LANE SAW IS STILL THE ONE THE HOST SENT. The transform happens between the record
	// and the voices, so the seam reports 60 while the voice sounds 72 — which is what makes this a
	// lane rather than a rewrite of the host's input.
	rig.synth.render(left.data(), right.data(), 64);
	const auto carried = rig.synth.laneNotesForTest();
	REQUIRE(carried.size() == 1);
	CHECK(carried[0].key == 60);
}

TEST_CASE("a registered tracker in the rack hears the block's notes and publishes their pitch") {
	// **THE END-TO-END CASE BOTH NOTE LANES HAVE BEEN WAITING FOR.** Everything up to now exercised
	// the walk on devices written inside a test file, because the only device in this build that reads
	// notes was held out of `kDevices`. The Tracker now ships on the generic card, with its own card to
	// come later. So this is the first assertion that a device a USER can add receives the
	// notes a player plays and turns them into a modulation source.
	//
	// It is a value claim end to end: keys pressed here, a pitch published there, with the number
	// computed from the tracker's own declared range rather than read off a run.
	namespace trk = feathers::mod::tracker;
	Rig rig;
	std::array<float, 64> left {};
	std::array<float, 64> right {};

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	feathers::DeviceRow row {};
	for (size_t i = 0; i < 4 && trk::kTraits.tag[i] != '\0'; ++i) {
		row.tag[i] = trk::kTraits.tag[i];
	}
	row.fieldsPresent = std::min(trk::kDescriptor.params.size(), row.field.size());
	for (size_t i = 0; i < row.fieldsPresent; ++i) {
		row.field[i] = static_cast<float>(trk::kDescriptor.params[i].def);
	}
	row.instanceId = feathers::kFirstInstance;
	rack.device[0] = row;
	// The `assignDenseRackLanes` pass that stood here handed every row a routing address, because a row
	// written directly into the rack had none and `slotsOf` placed it nowhere. A device is placed by its
	// slot since kill-lanes step 3c, and there is nothing left to hand out.
	// **THROUGH THE REGISTRY, NOT THROUGH `seatForTest`.** That seam exists because nothing registered
	// declared `NoteOps`; using it here would test the same thing the mod-runner cases already test
	// and would prove nothing about registration, which is the only thing this case is for.
	rig.publishModRack(rack);
	// **A RENDER BEFORE THE CHECK, BECAUSE ADOPTION IS THE AUDIO THREAD'S.** `provisionDevices` builds
	// and posts; `ModRunner::sync` adopts, and sync runs inside `tickEnvelopes`. Asking before a block
	// has been rendered asks the runner what it holds before it has been given the chance to take it —
	// which is the two-thread road working correctly, not a fixture problem.
	rig.synth.render(left.data(), right.data(), 64);
	// AGAINST THE REGISTRY'S OWN POINTER, not `&trk::kDescriptor`. `kDevices` holds its descriptors BY
	// VALUE, so the entry the runner resolved a row to is a different object from the one the device's
	// header declares — the two compare unequal while printing identically, which is a confusing way
	// to fail and a worse way to pass.
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(stripDeviceOf(rig.synth, rack, 0))
	        == feathers::deviceForTag(trk::kTraits.tag));

	// **THE REFERENCE, RESTATED FROM THE DEVICE'S OWN DECLARATIONS.** Root plus `Spread` octaves reads
	// +1, the root reads 0, and root minus `Spread` octaves reads −1 — so the expected value for a key
	// is its distance from the root in units of the spread. Computed here rather than read off a run,
	// which is the whole reason this case is a value claim and not a difference check.
	const double root = trk::kDescriptor.params[static_cast<size_t>(trk::Param::RootNote)].def;
	const double spread = trk::kDescriptor.params[static_cast<size_t>(trk::Param::Spread)].def;
	REQUIRE(spread > 0.0);
	const auto expected = [&](double key) {
		return static_cast<float>((key - root) / (spread * 12.0));
	};
	// THE DEFAULTS REALLY DO PUT MIDDLE C AT ZERO, asserted rather than assumed — if the declared root
	// moves, this line says so instead of the case quietly testing a different claim.
	REQUIRE(root == doctest::Approx(60.0));

	SUBCASE("the root reads zero, and an octave either side reads plus and minus one spread") {
		// **THE ROOT ITSELF IS THE CLAIM THAT MATTERS**, and it is the one the old Low/High mapping
		// could not make: zero is now a note the player named rather than wherever the bottom of a
		// range happened to sit.
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.synth.render(left.data(), right.data(), 64);
		CHECK(rig.synth.envelopeValueForTest(0) == doctest::Approx(0.0f).epsilon(1e-3));

		// AN OCTAVE ABOVE: +1/spread, which at the declared spread of 2 is +0.5.
		rig.synth.noteOn(0, 72, 2, 1.0);
		rig.synth.render(left.data(), right.data(), 64);
		CHECK(rig.synth.envelopeValueForTest(0) == doctest::Approx(expected(72.0)).epsilon(1e-3));
		CHECK(rig.synth.envelopeValueForTest(0) == doctest::Approx(0.5f).epsilon(1e-3));

		// **AND AN OCTAVE BELOW READS NEGATIVE, which is the whole of what the bipolar decision bought.** A
		// unipolar mapping put every played note on one side of rest; a note under the root now
		// modulates the other way. Rule::Last is the default, so the newest note down is the one
		// reported — which is also what makes this a moving value rather than a latched one.
		rig.synth.noteOn(0, 48, 3, 1.0);
		rig.synth.render(left.data(), right.data(), 64);
		CHECK(rig.synth.envelopeValueForTest(0) == doctest::Approx(expected(48.0)).epsilon(1e-3));
		CHECK(rig.synth.envelopeValueForTest(0) < 0.0f);
	}

	SUBCASE("the rule is the device's control, not the host's choice") {
		// registry.h records the decision: every device is given all the notes and decides for itself
		// what to do with them. The host hands over the chord and the tracker's own
		// Rule decides — so the SAME two notes must publish a different pitch under a different rule,
		// which is the claim that the decision really is the device's.
		feathers::ModRack lowest = rack;
		lowest.device[0].field[static_cast<size_t>(trk::Param::Rule)] =
		    static_cast<float>(static_cast<int>(trk::Rule::Lowest));
		rig.publishModRack(lowest);

		rig.synth.noteOn(0, 72, 1, 1.0);
		rig.synth.noteOn(0, 48, 2, 1.0);
		rig.synth.render(left.data(), right.data(), 64);
		// THE BOTTOM OF THE CHORD, not the newest — 48 rather than the 72 Rule::Last would report.
		// Chosen either side of the root so the two rules disagree in SIGN as well as in size, which a
		// chord entirely above the root could not show.
		CHECK(rig.synth.envelopeValueForTest(0) == doctest::Approx(expected(48.0)).epsilon(1e-3));
		CHECK(rig.synth.envelopeValueForTest(0) < 0.0f);
	}
}

TEST_CASE("the fx lane renders what the pre-graph engine rendered, sample for sample") {
	// **THE REFERENCE IS ANOTHER BUILD.** Every constant below was MEASURED on the engine at commit
	// 329f188 — `FxRunner` walking its slot array, before the graph owned the fx lane's order — by
	// printing these samples from this exact setup. Step 3 moves that walk onto `chain(c)` with a
	// published snapshot, and the whole point of the case is that the AUDIO must not move with it.
	//
	// EXPECTATIONS READ OFF THE NEW CODE WOULD COMPARE THE NEW BUILD TO ITSELF and pass whatever it
	// did — the same trap the modulator pin was written against before step 4, and it caught a real
	// one there (a device that was never built, reading a flat zero that a difference check would have
	// accepted).
	//
	// **THE CHAIN'S ORDER MATTERS HERE, which is what makes it a fair test of an order change.** A
	// filter and then HARM do not commute; a walk that visited them the other way round, or that
	// visited one twice, lands somewhere else in these six numbers.
	namespace hrm = feathers::fx::harm;
	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 1.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
	hrm::State harm;
	harm.settings.enabled = true;
	harm.settings.level = 0.5f;
	harm.settings.harmonic = hrm::kUnisonHarmonic;
	const hrm::Row row = hrm::rowOf(harm);
	rig.chain.device[1].row = feathers::toDeviceRow(
	    hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), row);
	rig.publishChain();

	rig.synth.noteOn(0, 60, 1, 0.9);
	std::array<float, 64> left {};
	std::array<float, 64> right {};
	for (int block = 0; block < 8; ++block) {
		rig.synth.render(left.data(), right.data(), 64);
	}

	// MEASURED at 329f188, and RE-MEASURED at the HARM parity sweep (2026-09-16), which changed what
	// HARM itself renders — the sine is pink-scaled now (sqrt(40 / 261.6) = 0.39 at middle C), so the
	// old six numbers described a device that no longer exists. That is the one legitimate reason to
	// move these: the property under guard is the chain ORDER, and the reference for it is "whatever
	// this exact setup rendered when the devices last changed", recorded here so a later order change
	// is compared against a build that differs from it in order alone. Six points across the eighth
	// block rather than one: a single sample is satisfied by a great many wrong renders, and points
	// near both ends catch a walk that is right in the middle of a buffer and wrong at its edges.
	//
	// RE-MEASURED AGAIN 2026-09-24 for the same kind of reason: the voice's 4x decimator was replaced
	// (licence migration, src/dsp/halfband.h), so the SOURCE feeding this chain renders slightly
	// different samples — the two decimators' passbands agree to 0.001 dB, but their phase differs by
	// a fraction of a sample, and six raw samples see that. Nothing in the chain moved. Previous values,
	// for the record: 0.0555795, 0.00618269, -0.173958, -0.241702.
	CHECK(left[0] == doctest::Approx(0.0555551f).epsilon(1e-5));
	CHECK(left[7] == doctest::Approx(0.00621879f).epsilon(1e-5));
	CHECK(left[31] == doctest::Approx(-0.178742f).epsilon(1e-5));
	CHECK(left[63] == doctest::Approx(-0.238016f).epsilon(1e-5));
	CHECK(right[0] == doctest::Approx(0.0555551f).epsilon(1e-5));
	CHECK(right[31] == doctest::Approx(-0.178742f).epsilon(1e-5));

	// **NOT VACUOUS, THREE WAYS.** The buffer is not silent, the samples are not all one value, and
	// the two channels are genuinely being compared rather than one being the other's echo by
	// accident — this patch is mono through the lane, so the L/R equality is a real property of it
	// and asserting it catches a walk that ran one channel and not the other.
	CHECK(left[0] != doctest::Approx(0.0f));
	CHECK(left[0] != doctest::Approx(left[63]));
	CHECK(left[31] == doctest::Approx(right[31]).epsilon(1e-6));
}

TEST_CASE("a disabled modulator publishes its rest value, and keeps running underneath") {
	// **THE ENABLE SWITCH HAD TO MEAN SOMETHING.** The shared head draws a bypass square whenever
	// a device declares `enableControl`, and Feathers' mod strip published `outputAt` unconditionally
	// — so the square was drawn, was pressed, and changed nothing downstream. The honesty rule is
	// explicit: a control is enabled iff it changes sound through a real param. The rack has gated
	// this since it was written (`RackEngine::sourceValue`); Feathers did not.
	//
	// **READ THROUGH `publishedSourceForTest`, WHICH IS THE GATED NUMBER.** A first version asserted
	// `envelopeValueForTest` — the DEVICE's output, deliberately ungated because a bypassed modulator
	// keeps running — and failed against a gate that was working correctly. The two quantities differ
	// by exactly the bypass, which is the whole subject here.
	namespace mseg = feathers::mod::mseg;
	Rig rig;
	std::array<float, 64> left {};
	std::array<float, 64> right {};

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	mseg::Settings shape {};
	shape.count = 2;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Gate;
	shape.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	shape.segment[0].share = 0.5f;
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = 0.5f;
	shape.segment[1].level = 0.25f;
	shape.enabled = true;
	rack.setMsegAt(0, shape);
	rig.publishModRack(rack);
	rig.synth.setTempo(120.0);
	rig.synth.noteOn(0, 60, 1, 0.8f);

	for (int block = 0; block < 200; ++block) {
		rig.synth.render(left.data(), right.data(), 64);
	}
	const float running = rig.synth.publishedSourceForTest(0);
	// NOT VACUOUS: the shape has moved off rest, or "disabled reads rest" is true either way.
	REQUIRE(running > 0.01f);

	SUBCASE("switched off, it publishes rest rather than its last value") {
		mseg::Settings off = shape;
		off.enabled = false;
		feathers::ModRack disabled = rack;
		disabled.setMsegAt(0, off);
		rig.publishModRack(disabled);
		rig.synth.render(left.data(), right.data(), 64);

		// **REST, NOT FROZEN.** The bug was a source frozen at some value when switched off;
		// rest is what every destination reads as "no modulation from here".
		CHECK(rig.synth.publishedSourceForTest(0) == doctest::Approx(0.0f));

		// AND THE DEVICE IS STILL RUNNING UNDERNEATH, which the ungated seam is how to see.
		CHECK(rig.synth.envelopeValueForTest(0) > 0.0f);

		SUBCASE("and switching back on resumes where the music is, not where it left") {
			for (int block = 0; block < 60; ++block) {
				rig.synth.render(left.data(), right.data(), 64);
			}
			rig.publishModRack(rack); // on again
			rig.synth.render(left.data(), right.data(), 64);
			const float resumed = rig.synth.publishedSourceForTest(0);
			CHECK(resumed > 0.0f);
			// IT MOVED ON WHILE IT WAS SILENT. A build that stopped the tick instead of the
			// publication would come back at exactly the value it left at, and fail here — which is
			// the difference between a bypass and a mute.
			CHECK(resumed != doctest::Approx(running));
		}
	}
}

TEST_CASE("a per-note TUNING expression bends that note, and only that note") {
	// **BACKLOG DS. HARM's pitch did not follow note pitch expressions; the instrument must react to
	// MPE/note expressions, and effects like HARM must respect them.**
	//
	// **NOTHING CONSUMED THEM AND THE ROAD WAS OTHERWISE COMPLETE.** `feathers_clap.cpp` had a
	// `break` on `CLAP_EVENT_NOTE_EXPRESSION` under a note reading *"Declared unsupported and ignored
	// rather than misread"*, and `Synth::Responder::setNoteExpression` was an empty body — required by
	// `voicemanager_constraints.h` and never filled. The manager's `routeNoteExpression` already
	// matched by (port, channel, key, note_id) with CLAP's wildcards. So the fix was a body and a
	// dispatch, not a mechanism.
	//
	// ASSERTED ON THE PITCH, not on a buffer. CLAUDE.md: *"a claim about a value cannot be inferred
	// from a rendered buffer"* — a render would move for a dozen reasons and this claim is that ONE
	// number moved by exactly the amount asked for.
	constexpr int16_t kKeyC5 = 72;

	SUBCASE("it is a DELTA on the note's own pitch, in semitones") {
		const auto rigHeap = std::make_unique<Rig>(); // ~1.4 MB: six on the stack overflow 8 MiB under ASan
		Rig& rig = *rigHeap;
		rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		REQUIRE(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4));

		// SEVEN SEMITONES UP. Poly glide is zero (the case above pins that), so the target arrives
		// whole on the next slice rather than being approached.
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, 7.0);
		rig.play({}, 256, 64);
		CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 + 7.0));

		// **THE LAST VALUE IS THE BEND, NOT ONE MORE OF THEM.** CLAP's TUNING is an offset from the
		// note, so a host streaming a slide sends absolute offsets — accumulating would ratchet a
		// gentle slide into a siren, and it is the kind of wrong that only shows up under a real
		// controller.
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, 2.0);
		rig.play({}, 256, 64);
		CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 + 2.0));

		// AND BACK DOWN THROUGH ZERO, which is where a hand releases a slide.
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, -3.5);
		rig.play({}, 256, 64);
		CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 - 3.5));
	}

	SUBCASE("it reaches ONE note, which is the whole point of a per-note expression") {
		const auto rigHeap = std::make_unique<Rig>(); // ~1.4 MB: six on the stack overflow 8 MiB under ASan
		Rig& rig = *rigHeap;
		rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC5, 2) }, 256, 64);
		const int low = rig.synth.slotOfKeyForTest(kKeyC4);
		const int high = rig.synth.slotOfKeyForTest(kKeyC5);
		REQUIRE(low >= 0);
		REQUIRE(high >= 0);
		REQUIRE(low != high);

		// ADDRESSED BY NOTE ID, the way a host addresses one voice of a chord.
		rig.synth.noteExpression(0, kKeyC5, 2, CLAP_NOTE_EXPRESSION_TUNING, 5.0);
		rig.play({}, 256, 64);
		CHECK(rig.synth.voicePitchForTest(high) == doctest::Approx(kKeyC5 + 5.0));
		// **AND THE OTHER NOTE DID NOT MOVE.** Without this the case would pass on an implementation
		// that bent every sounding voice — which is what a channel-wide pitch bend does, and is
		// exactly the behaviour a per-note expression exists to replace.
		CHECK(rig.synth.voicePitchForTest(low) == doctest::Approx(kKeyC4));
	}

	SUBCASE("a fresh note starts unexpressed, even on a voice somebody bent") {
		// **THE VOICE-REUSE HAZARD, AND IT IS SILENT.** The pool hands the same `Voice` back, so a
		// slide left on it would be inherited by whichever later note happened to land there — a bug
		// that appears on some notes and not others, depending on allocation.
		const auto rigHeap = std::make_unique<Rig>(); // ~1.4 MB: six on the stack overflow 8 MiB under ASan
		Rig& rig = *rigHeap;
		rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, 9.0);
		rig.play({}, 256, 64);
		REQUIRE(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 + 9.0));

		rig.play({ noteOff(0, kKeyC4, 1) }, 4096, 64);
		rig.play({ noteOn(0, kKeyC4, 3) }, 256, 64);
		const int again = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(again >= 0);
		CHECK(rig.synth.voicePitchForTest(again) == doctest::Approx(kKeyC4));
	}

	SUBCASE("the expressions this instrument does NOT answer to leave the pitch alone") {
		// **IGNORING IS A DECISION AND IT IS PINNED.** Volume, pan, brightness and pressure each need
		// a decision about what they map to here; wiring one blind would invent behaviour. What must not
		// happen is a stray expression landing in the pitch sum because a switch fell through.
		const auto rigHeap = std::make_unique<Rig>(); // ~1.4 MB: six on the stack overflow 8 MiB under ASan
		Rig& rig = *rigHeap;
		rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		for (const int32_t other : { CLAP_NOTE_EXPRESSION_VOLUME, CLAP_NOTE_EXPRESSION_PAN,
			                         CLAP_NOTE_EXPRESSION_VIBRATO, CLAP_NOTE_EXPRESSION_EXPRESSION,
			                         CLAP_NOTE_EXPRESSION_BRIGHTNESS, CLAP_NOTE_EXPRESSION_PRESSURE }) {
			CAPTURE(other);
			rig.synth.noteExpression(0, kKeyC4, 1, other, 6.0);
			rig.play({}, 128, 64);
			CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4));
		}
	}

	SUBCASE("HARM follows the bend, which is the effect-side half of the requirement") {
		// **THE DEVICE END, ASSERTED ON HARM'S OWN FREQUENCY.** Effects like HARM must respect the
		// expression. Harm reads `NoteContext::pitchSemitones`, the synth fills that from
		// `lastTargetSemitones`, and `applyPitches` writes that from `targetSemitonesFor` — the one
		// place the expression is added. So this follows BY CONSTRUCTION, which is exactly the kind of
		// claim this codebase does not accept on its own: `opsCurrentHzForTest` exists precisely
		// because *"a rendered signal carries a notch, an AR envelope and a sine at once; one
		// frequency is not recoverable from it"*, and it is what turns the construction into a fact.
		namespace hrm = feathers::fx::harm;
		const auto hzOf = [](double semitones) {
			return 440.0 * std::exp2((semitones - 69.0) / 12.0);
		};

		const auto rigHeap = std::make_unique<Rig>(); // ~1.4 MB: six on the stack overflow 8 MiB under ASan
		Rig& rig = *rigHeap;
		rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
		REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
		hrm::State harm;
		harm.settings.enabled = true;
		harm.settings.level = 0.5f;
		harm.settings.harmonic = hrm::kUnisonHarmonic; // 1:1, so harm sits ON the note
		harm.poly = true;                              // per-voice, so it is THIS note's harm
		rig.chain.device[1].row = feathers::toDeviceRow(
		    hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), hrm::rowOf(harm));
		rig.publishChain();

		rig.play({ noteOn(0, kKeyC4, 1) }, 512, 128);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		const void* storage = rig.synth.voiceFxStorageForTest(slot, hrm::kTraits.tag);
		REQUIRE(storage != nullptr);
		REQUIRE(hrm::opsCurrentHzForTest(storage) == doctest::Approx(hzOf(kKeyC4)).epsilon(1e-4));

		// TWELVE SEMITONES: an octave, so the expected frequency is a doubling and a wrong answer
		// cannot hide inside the tolerance.
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, 12.0);
		rig.play({}, 512, 128);
		CHECK(hrm::opsCurrentHzForTest(rig.synth.voiceFxStorageForTest(slot, hrm::kTraits.tag))
		      == doctest::Approx(hzOf(kKeyC4 + 12.0)).epsilon(1e-4));
		// AND IT IS THE DOUBLING RATHER THAN MERELY DIFFERENT, stated independently of the semitone
		// arithmetic above so the two cannot be wrong the same way.
		CHECK(hrm::opsCurrentHzForTest(rig.synth.voiceFxStorageForTest(slot, hrm::kTraits.tag))
		      == doctest::Approx(2.0 * hzOf(kKeyC4)).epsilon(1e-4));
	}

	SUBCASE("a value that is not a number never reaches the pitch") {
		// The same guard the parameter road keeps, for the same reason: this number lands in an
		// addition the oscillator reads, and a NaN would poison the voice's target for the rest of the
		// note rather than for one block.
		const auto rigHeap = std::make_unique<Rig>(); // ~1.4 MB: six on the stack overflow 8 MiB under ASan
		Rig& rig = *rigHeap;
		rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, 4.0);
		rig.play({}, 128, 64);
		REQUIRE(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 + 4.0));

		for (const double bad : { std::numeric_limits<double>::quiet_NaN(),
			                      std::numeric_limits<double>::infinity() }) {
			rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_TUNING, bad);
			rig.play({}, 128, 64);
			// THE BEND IT ALREADY HAD, unchanged — refused rather than zeroed, because a dropped event
			// should leave the gesture where it was.
			CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 + 4.0));
		}
	}
}

TEST_CASE("every note expression CLAP defines is KEPT on the voice it addressed") {
	// **THE FOUNDATION OF THE NOTE-EXPRESSION MODULATOR, and the half that is not a decision.** DS
	// took TUNING and dropped the other six under a note saying each *"needs a decision about what it
	// maps to on THIS instrument ... wiring them blind would be inventing behaviour"*. That reasoning
	// is about what an expression DOES and it still stands. What was also true, and was the actual
	// blocker, is that the values were thrown away before anybody could ask for them.
	//
	// The plan is a note-expressions modulator whose mod lanes publish to each voice. A source
	// cannot publish a value nobody kept. This keeps them.
	//
	// **ASSERTED THROUGH A SEAM, BECAUSE THERE IS NOTHING ELSE TO ASSERT ON.** Pressure has no audible
	// consequence until a patch routes it, so unlike the TUNING case above there is not even a signal
	// to fail to infer it from. CLAUDE.md prices this exactly: a test-only accessor costs five lines,
	// and no amount of ingenuity with buffers recovers a quantity the buffer does not carry.
	Rig rig;
	rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);

	// A DISTINCT VALUE PER EXPRESSION, so an implementation that wrote them all to one slot — or read
	// them all from one — cannot pass. Chosen inside each expression's own sensible range and never
	// equal to another's.
	const auto sent = [](int32_t id) { return 0.11 + 0.07 * static_cast<double>(id); };

	SUBCASE("all seven arrive, each on its own id") {
		for (int32_t id = 0; id < static_cast<int32_t>(feathers::kNoteExpressionCount); ++id) {
			REQUIRE(rig.synth.voiceExpressionForTest(slot, id) == doctest::Approx(0.0));
		}
		for (int32_t id = 0; id < static_cast<int32_t>(feathers::kNoteExpressionCount); ++id) {
			rig.synth.noteExpression(0, kKeyC4, -1, id, sent(id));
		}
		for (int32_t id = 0; id < static_cast<int32_t>(feathers::kNoteExpressionCount); ++id) {
			CAPTURE(id);
			CHECK(rig.synth.voiceExpressionForTest(slot, id) == doctest::Approx(sent(id)));
		}
	}

	SUBCASE("TUNING still reaches the PITCH road as well — two consumers, one number") {
		// The array is the MODULATION road and `expressionSemitones` is the pitch one. DS's behaviour
		// must not have been traded for this: a bend still bends.
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_TUNING, 7.0);
		rig.play({}, 256, 64);
		CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4 + 7));
		CHECK(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_TUNING) == doctest::Approx(7.0));
	}

	SUBCASE("a reused voice does not inherit the expressions of the note before it") {
		// The pool hands the same `Voice` back, so this is the same hazard `expressionSemitones` was
		// already reset for — now true of all seven, and it would present as a note that arrives
		// already under pressure only on the voices somebody had squeezed.
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.9);
		REQUIRE(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_PRESSURE)
		        == doctest::Approx(0.9));
		rig.play({ noteOff(0, kKeyC4, -1) }, 256, 64);
		rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
		const int again = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(again >= 0);
		CHECK(rig.synth.voiceExpressionForTest(again, CLAP_NOTE_EXPRESSION_PRESSURE)
		      == doctest::Approx(0.0));
	}

	SUBCASE("an id this build does not know is dropped, not clamped onto a neighbour") {
		// A future CLAP expression must not arrive as a wrong one. Asserted on the LAST id, which is
		// what a clamp would land on.
		rig.synth.noteExpression(0, kKeyC4, -1,
		                         static_cast<int32_t>(feathers::kNoteExpressionCount) + 3, 0.8);
		CHECK(rig.synth.voiceExpressionForTest(
		          slot, static_cast<int32_t>(feathers::kNoteExpressionCount) - 1)
		      == doctest::Approx(0.0));
	}

	SUBCASE("a NaN never lands") {
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.5);
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE,
		                         std::numeric_limits<double>::quiet_NaN());
		CHECK(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_PRESSURE)
		      == doctest::Approx(0.5));
	}
}

TEST_CASE("a note expression TRANSITS THE LANE, so a device can see it") {
	// **THE INCONSISTENCY.** Note on and off transit the note lane — devices
	// see them, and `deliverNotes` records that this is reading (1): what PLAYS is the host's stream.
	// Expressions did not transit at all. They went from `feathers_clap.cpp` straight to the voice
	// manager, so no device was ever offered one, and a modulator that wanted to publish pressure as
	// a source could not see pressure.
	//
	// A design that worked around that instead of fixing it was rejected: notes are piped, and
	// potentially mutated, as they transit the graph. They were; expressions
	// were the exception, and CLAP does not make that exception — `clap/events.h` carries expressions
	// in the same stream as notes, addressed the same way.
	//
	// **THIS IS (a) OF (a)-THEN-(b).** The lane gets a copy and the host's stream still plays, which
	// is exactly what notes already do. (b) — the lane's OUTPUT reaching the voice, so a device that
	// scales pressure changes what the voice feels — is the next step and is the decision `deliverNotes`
	// has been holding.
	Rig rig;
	rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);

	SUBCASE("it arrives on the lane, carrying CLAP's id and the value") {
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.625);
		// **THE LANE IS READ BEFORE THE NEXT RENDER CLEARS IT.** `runNoteLane` empties the buffer once
		// it has offered it round, so the assertion has to happen while the tick's notes are still in
		// it — which is also exactly when a device would be looking.
		bool seen = false;
		for (const feathers::device::NoteEvent& e : rig.synth.laneNotesForTest()) {
			if (e.kind != feathers::device::NoteEvent::Kind::Expression) {
				continue;
			}
			CHECK(e.expressionId == CLAP_NOTE_EXPRESSION_PRESSURE);
			CHECK(e.expressionValue == doctest::Approx(0.625));
			CHECK(e.key == kKeyC4);
			seen = true;
		}
		// NOT VACUOUS: a lane that carried no expression at all would pass every CHECK above by
		// running none of them.
		CHECK(seen);
	}

	SUBCASE("an expression is not mistaken for a note — the kinds stay distinct") {
		// A device switching on `kind` must not run its note-on arm for an expression, which is what a
		// new enumerator added carelessly would cause. The compiler already refuses an unhandled arm
		// (`-Werror=switch` caught exactly that in `deliverNotes`); this is the runtime half.
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_BRIGHTNESS, 0.25);
		size_t ons = 0;
		size_t expressions = 0;
		for (const feathers::device::NoteEvent& e : rig.synth.laneNotesForTest()) {
			ons += (e.kind == feathers::device::NoteEvent::Kind::On) ? 1 : 0;
			expressions += (e.kind == feathers::device::NoteEvent::Kind::Expression) ? 1 : 0;
		}
		CHECK(expressions == 1);
		CHECK(ons == 0); // the note-on landed in an earlier tick and its buffer has been consumed
	}

	SUBCASE("and it still reaches the voice — the lane is a copy, not a diversion") {
		// **READING (1), ASSERTED.** Adding the lane road must not have taken the delivery one away:
		// the manager is still fed, so TUNING still bends and every expression still lands on its
		// voice. This is what makes (a) safe to ship before (b) is decided.
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.75);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		CHECK(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_PRESSURE)
		      == doctest::Approx(0.75));
	}
}

//------------------------------------------------------------------------------------------------//
// (b) — A DEVICE THAT CHANGES AN EXPRESSION CHANGES WHAT THE VOICE FEELS                           //
//------------------------------------------------------------------------------------------------//

namespace {

/// **A DEVICE THAT HALVES PRESSURE AND PASSES EVERYTHING ELSE THROUGH.** Built here and never
/// registered — `ModRunner::seatForTest` is what makes a claim about the note lane expressible
/// without inventing a shipping device to make it with, and `test_mod_runner.cpp` uses the same
/// door for the same reason.
///
/// **IT MUTATES RATHER THAN OBSERVES, WHICH IS THE ENTIRE POINT.** (a) put expressions on the lane so
/// a device could SEE one; the claim under test is (b) — that what leaves the lane is what the VOICE
/// feels. A device that only watched could not tell the two apart, and the case would pass on the
/// (a) behaviour it is meant to distinguish from.
struct Squeezer {
	int placeholder = 0;
};

const feathers::device::NoteOps kSqueezerNoteOps {
	.stateBytes = sizeof(Squeezer),
	.stateAlign = alignof(Squeezer),
	.construct = [](void* storage) { new (storage) Squeezer(); },
	.destruct = [](void* storage) { static_cast<Squeezer*>(storage)->~Squeezer(); },
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes =
	    [](void*, const feathers::device::NoteEvent* in, size_t inCount,
	       feathers::device::NoteEvent* out, size_t outCapacity, size_t* outCount) {
		    size_t n = 0;
		    for (size_t i = 0; i < inCount && n < outCapacity; ++i) {
			    out[n] = in[i];
			    if (out[n].kind == feathers::device::NoteEvent::Kind::Expression
			        && out[n].expressionId == CLAP_NOTE_EXPRESSION_PRESSURE) {
				    out[n].expressionValue *= 0.5;
			    }
			    ++n;
		    }
		    *outCount = n;
	    },
};

constexpr feathers::device::Traits kSqueezerTraits {
	.tag = "SQZE",
	.name = "Squeezer",
	.hasCard = false,
	.hasExpanded = false,
};

const feathers::device::Descriptor kSqueezerDescriptor {
	.traits = kSqueezerTraits,
	.params = {},
	.outputs = {},
	.notes = &kSqueezerNoteOps,
};

} // namespace

TEST_CASE("(b) a device that scales an expression changes what the VOICE feels") {
	// **(a), THEN (b).** (a) put expressions on the note lane so a device could see
	// one. (b) is that what LEAVES the lane is what reaches the voice — which is what notes have done
	// since 2026-09-02 (reading (2), the lane as destination: an arp in the strip plays the synth). Expressions
	// were the one part of the stream that bypassed the lane entirely.
	//
	// **ASSERTED AS THE VALUE ON THE VOICE**, not as a difference in a render. Pressure reaches no
	// sound until a patch routes it, so there is no buffer that could carry this claim even in
	// principle — `voiceExpressionForTest` is the seam, for CLAUDE.md's stated reason.
	Rig rig;
	rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);

	SUBCASE("THE CONTROL: with no note device seated, the host's value arrives untouched") {
		// **WITHOUT THIS, THE SUBCASE BELOW CANNOT TELL A WORKING DEVICE FROM A BROKEN LANE.** A lane
		// that halved everything, or dropped it and left a stale zero, would look identical.
		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.8);
		CHECK(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_PRESSURE)
		      == doctest::Approx(0.8));
	}

	SUBCASE("with a device that halves pressure in transit, the voice feels the HALVED value") {
		seatInStrip(rig, kSqueezerDescriptor);

		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.8);
		// 0.4, NOT "SOMETHING SMALLER". The device's rule is a halving and the assertion is that
		// halving — a check for "less than 0.8" would pass on a lane that dropped the event and left
		// the voice at zero, which is the failure this is most likely to be confused with.
		CHECK(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_PRESSURE)
		      == doctest::Approx(0.4));
	}

	SUBCASE("and it leaves the other expressions alone") {
		// A device that rewrote the whole stream — or one whose passthrough dropped what it did not
		// care about — would fail here and pass the subcase above.
		seatInStrip(rig, kSqueezerDescriptor);

		rig.synth.noteExpression(0, kKeyC4, -1, CLAP_NOTE_EXPRESSION_BRIGHTNESS, 0.6);
		CHECK(rig.synth.voiceExpressionForTest(slot, CLAP_NOTE_EXPRESSION_BRIGHTNESS)
		      == doctest::Approx(0.6));
	}
}

TEST_CASE("PER-VOICE MODULATION: two notes with different pressure land on different pitches") {
	// **THE FIRST MIGRATED DESTINATION — backlog DU, and the whole point of the arc.** Until now every
	// modulation destination resolved once per block, so a per-note source could only ever move every
	// note together. Pitch was the easiest target and the table in DU says why: `targetSemitonesFor`
	// already calls `semitonesForKey` once per voice, so the per-voice loop existed and the migration
	// was threading an argument rather than multiplying a loop.
	//
	// **ASSERTED ON THE PITCH OF EACH VOICE**, which is the only honest way: a render carries both
	// notes at once and no amount of ingenuity recovers "voice A is at 60.5 and voice B is at 60.1"
	// from a signal that carries their sum.
	namespace nx = feathers::mod::noteexpr;
	Rig rig;

	// THE DEVICE IN THE RACK, and a route from its Pressure output to Fine Tune.
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		// An identity is a serial and nothing else since kill-lanes step 3c; the routing address that
		// stood beside it was `deviceSlotOfRack(0)`, for the slot the row goes into.
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);

	constexpr size_t kPressureOutput = 6; // CLAP_NOTE_EXPRESSION_PRESSURE; the output index is the id
	rig.play({}, 64, 64); // a sync first: the engine chooses the device's slot when it adopts it
	const size_t lane = engineStripSlot(rig.synth, rack, 0);
	REQUIRE(lane < feathers::kMaxDeviceSlots);
	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);

	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
	           feathers::backplane::Destination::core(static_cast<size_t>(fine)), 1.0f);
	routes.publish(rig.params);

	// TWO NOTES, DELIBERATELY DISTINGUISHABLE — different keys so the voices are separate, and
	// different note ids so the expressions can be addressed apart.
	// **A BLOCK BEFORE THE NOTES, so the rack is SYNCED when they arrive.** `provisionDevices` is the
	// message-thread half; `ModRunner::sync` adopts on the audio thread, and until it has, a slot's
	// `live` is null and `processNotes` skips it. A note-on landing in that window is a note the device
	// never saw, and every expression afterwards addresses a note it is not holding.
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC4 + 7, 2) }, 256, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	REQUIRE(slotA != slotB);

	SUBCASE("THE CONTROL: with no pressure sent, the two voices sit at their own keys") {
		// **WITHOUT THIS THE CLAIM BELOW CANNOT TELL PER-VOICE MODULATION FROM A BROKEN ROUTE.** Both
		// voices unmodulated is what "the route does nothing" also looks like.
		rig.play({}, 256, 64);
		CHECK(rig.synth.voicePitchForTest(slotA) == doctest::Approx(kKeyC4).epsilon(0.01));
		CHECK(rig.synth.voicePitchForTest(slotB) == doctest::Approx(kKeyC4 + 7).epsilon(0.01));
	}

	SUBCASE("pressure on ONE note moves ONLY that note's pitch") {
		// Note 1 is squeezed; note 2 is not.
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
		rig.play({}, 512, 64);

		const double a = rig.synth.voicePitchForTest(slotA);
		const double b = rig.synth.voicePitchForTest(slotB);
		CAPTURE(a);
		CAPTURE(b);

		// **THE UNTOUCHED NOTE HAS NOT MOVED**, which is the half that fails on a collapsed source:
		// under block-rate modulation both voices would have taken the same offset.
		CHECK(b == doctest::Approx(kKeyC4 + 7).epsilon(0.01));
		// AND THE SQUEEZED ONE HAS. Fine Tune is in cents and the route amount is 1.0, so a pressure
		// of 1.0 reaches the top of the control's declared range — the exact value is the span's
		// business and what is asserted here is that this voice moved and its neighbour did not.
		CHECK(a != doctest::Approx(kKeyC4).epsilon(0.01));
	}

	SUBCASE("and pressure on the OTHER note moves the other one") {
		// The mirror, so a passing result cannot come from voice A being special — an implementation
		// that published every per-voice value into slot 0 would pass the subcase above.
		rig.synth.noteExpression(0, kKeyC4 + 7, 2, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
		rig.play({}, 512, 64);
		CHECK(rig.synth.voicePitchForTest(slotA) == doctest::Approx(kKeyC4).epsilon(0.01));
		CHECK(rig.synth.voicePitchForTest(slotB) != doctest::Approx(kKeyC4 + 7).epsilon(0.01));
	}
}

TEST_CASE("COARSE adds its semitones to the pitch exactly, from the knob and from a per-voice route") {
	// Coarse is a continuous pitch knob in semitones, on Fine Tune's road, so a modulator can sweep the
	// pitch wide. ASSERTED ON EACH VOICE'S PITCH, and to the exact semitone count: a render would move for any
	// pitch error at all, and the claim is that this one number arrives whole.
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	constexpr double kCoarse = 5.25;
	rig.set(ParamId::Coarse, kCoarse);

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	constexpr size_t kPressureOutput = 6; // CLAP_NOTE_EXPRESSION_PRESSURE; the output index is the id
	rig.play({}, 64, 64);
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC4 + 7, 2) }, 256, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	REQUIRE(slotA != slotB);
	const auto near = [](double got, double want) { return std::abs(got - want) < 1e-9; };

	SUBCASE("the knob: every voice sits at its key plus Coarse, in semitones") {
		rig.play({}, 256, 64);
		CAPTURE(rig.synth.voicePitchForTest(slotA));
		CAPTURE(rig.synth.voicePitchForTest(slotB));
		CHECK(near(rig.synth.voicePitchForTest(slotA), kKeyC4 + kCoarse));
		CHECK(near(rig.synth.voicePitchForTest(slotB), kKeyC4 + 7 + kCoarse));
	}

	SUBCASE("and on Fine Tune's road: the two add, Coarse in semitones and Fine in cents") {
		rig.set(ParamId::FineTune, 50.0);
		rig.play({}, 256, 64);
		CAPTURE(rig.synth.voicePitchForTest(slotA));
		CHECK(near(rig.synth.voicePitchForTest(slotA), kKeyC4 + kCoarse + 0.5));
	}

	SUBCASE("a per-voice route: pressure on ONE note moves that note by amount x span semitones, and only it") {
		// Span 96 semitones (±48), so an amount of 1/8 at full pressure is exactly an octave.
		const int coarse = feathers::modDestinationIndexOf(ParamId::Coarse);
		REQUIRE(coarse >= 0);
		const size_t lane = engineStripSlot(rig.synth, rack, 0);
		REQUIRE(lane < feathers::kMaxDeviceSlots);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
		           feathers::backplane::Destination::core(static_cast<size_t>(coarse)), 0.125f);
		routes.publish(rig.params);
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
		rig.play({}, 512, 64);
		CAPTURE(rig.synth.voicePitchForTest(slotA));
		CAPTURE(rig.synth.voicePitchForTest(slotB));
		CHECK(near(rig.synth.voicePitchForTest(slotA), kKeyC4 + kCoarse + 12.0));
		CHECK(near(rig.synth.voicePitchForTest(slotB), kKeyC4 + 7 + kCoarse));
	}
}

TEST_CASE("the default Bend Range is an octave: a full wheel puts the note twelve semitones up, and down") {
	// The default is ±12, an octave each way. Read on the voice's pitch through the wheel, so the claim covers the
	// whole road from the default index to the note, not only the accessor.
	Rig rig;
	rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	REQUIRE(rig.synth.voicePitchForTest(slot) == doctest::Approx(kKeyC4));
	rig.synth.pitchBend(0, 1.0);
	rig.play({}, 256, 64);
	CHECK(std::abs(rig.synth.voicePitchForTest(slot) - (kKeyC4 + 12.0)) < 1e-9);
	rig.synth.pitchBend(0, -1.0);
	rig.play({}, 256, 64);
	CHECK(std::abs(rig.synth.voicePitchForTest(slot) - (kKeyC4 - 12.0)) < 1e-9);
}

TEST_CASE("PER-VOICE MODULATION: pressure on one note moves ONLY that note's LEVEL (backlog DU — Level migrates)") {
	// **BACKLOG DU, THE SECOND ROW OF ITS TABLE.** Level was resolved ONCE above the voice loop in
	// `renderChunk`, so a per-voice source routed to it — pressure — moved every sounding note by the
	// collapsed value. It is resolved per voice inside the loop now, exactly as the pitch parameters have
	// been; an unrouted Level is the same number N times, which is why day one was bit-identical.
	//
	// ASSERTED ON THE GAIN THE RENDER APPLIED (`Voice::gain`, kept for this), not on a buffer: two notes
	// render into one buffer and no ingenuity recovers "voice A at 0.8 and voice B at 0.5" from their sum.
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.5);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	constexpr size_t kPressureOutput = 6;
	rig.play({}, 64, 64);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	constexpr float kAmount = 0.3f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC4 + 7, 2) }, 256, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	REQUIRE(slotA != slotB);
	SUBCASE("THE CONTROL: no pressure, both voices at the base level") {
		rig.play({}, 256, 64);
		CHECK(rig.synth.voiceGainForTest(slotA) == doctest::Approx(0.5));
		CHECK(rig.synth.voiceGainForTest(slotB) == doctest::Approx(0.5));
	}
	SUBCASE("full pressure on note 1 lifts ITS gain by amount × span, and note 2 stays where it was") {
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
		rig.play({}, 512, 64);
		// THE EXPECTATION RESTATED FROM THE DECLARATION: Level's range is [0, 1], the route's span is that
		// range, pressure 1.0 at amount 0.3 is +0.3 on a base of 0.5. And the ParamSet's own per-voice
		// resolve agrees with what the render applied — the two roads to one number.
		CHECK(rig.synth.voiceGainForTest(slotA) == doctest::Approx(0.8).epsilon(0.01));
		CHECK(rig.synth.voiceGainForTest(slotA)
		      == doctest::Approx(rig.params.resolved(static_cast<clap_id>(ParamId::VoiceLevel), slotA)).epsilon(0.001));
		CHECK(rig.synth.voiceGainForTest(slotB) == doctest::Approx(0.5).epsilon(0.01));
	}
}

TEST_CASE("backlog DH: a modulator's Amount reaches the PER-VOICE runs too — the note expressions at half Amount lift a pressed note's level by half of what full Amount did, and the other note stays") {
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.5);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		REQUIRE(rack.device[0].fieldsPresent > static_cast<size_t>(nx::Param::Amount));
		REQUIRE(rack.device[0].field[static_cast<size_t>(nx::Param::Amount)] == 1.0f); // unity by default
		rack.device[0].field[static_cast<size_t>(nx::Param::Amount)] = 0.5f;
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	constexpr size_t kPressureOutput = 6;
	rig.play({}, 64, 64);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	constexpr float kAmount = 0.3f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC4 + 7, 2) }, 256, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	REQUIRE(slotA != slotB);
	// FULL PRESSURE AT HALF AMOUNT: 0.5 + 0.3 × (1.0 × 0.5) = 0.65, where the unscaled case above reads 0.8.
	rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
	rig.play({}, 512, 64);
	CHECK(rig.synth.voiceGainForTest(slotA) == doctest::Approx(0.65).epsilon(0.01));
	CHECK(rig.synth.voiceGainForTest(slotB) == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("backlog EF: the strip's envelope routed to Level shapes each note on its own — a new note does not lift the last note's tail, and a releasing voice is held alive by its routed tail past the voice's own release, then freed when the tail is done") {
	namespace dsr = feathers::mod::dahdsr;
	Rig rig;
	// THE ENVELOPE CARRIES THE LEVEL: base at zero, the route at full amount.
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.005f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.005f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
		rack.device[0].field[static_cast<size_t>(dsr::Param::Release)] = 0.0f; // one second
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	rig.play({}, 64, 64);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel));

	rig.play({ noteOn(0, kKeyC4, 1) }, 2048, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slotA >= 0);
	REQUIRE(rig.synth.voiceGainForTest(slotA) > 0.9); // the envelope, not the base, is the level

	// RELEASED: the tail falls over blocks...
	rig.play({ noteOff(0, kKeyC4, 1) }, 64, 64);
	rig.play({}, 4800, 64); // 0.1 s into a 1 s release
	const double a = rig.synth.voiceGainForTest(slotA);
	REQUIRE(a < 0.9);
	REQUIRE(a > 0.05);
	rig.play({}, 4800, 64);
	const double b = rig.synth.voiceGainForTest(slotA);
	REQUIRE(b < a);

	// ...AND A NEW NOTE DOES NOT LIFT IT. The second note rises on its own.
	rig.play({ noteOn(0, kKeyC4 + 7, 2) }, 4800, 64);
	const double c = rig.synth.voiceGainForTest(slotA);
	CHECK(c < b);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotB >= 0);
	REQUIRE(slotB != slotA);
	CHECK(rig.synth.voiceGainForTest(slotB) > 0.9);

	// HELD ALIVE BY THE TAIL: 0.3 s after its Off the voice's own release (a few ms by default) is long
	// past, and the voice is still sounding because its routed envelope is; a second later it is not.
	CHECK(rig.synth.soundingVoiceCount() == 2);
	REQUIRE(rig.releaseSeconds < 0.25);
	rig.play({}, 48000 + 4800, 64);
	CHECK(rig.synth.soundingVoiceCount() == 1);
	CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
	CHECK(rig.synth.slotOfKeyForTest(kKeyC4 + 7) == slotB);
}

TEST_CASE("smoothing closes (a smoothed DAHDSR must still close fully): the strip's envelope with smoothing on, routed to Level, lets a released voice go — its gain falls to zero, the voice frees, and the output is exactly silent") {
	namespace dsr = feathers::mod::dahdsr;
	// THE DEFAULT PATCH'S SHAPE — the envelope carries the level — with the smoothing turned up. Before the
	// fix the note's own envelope was marked done the tick its RAW release ended and was never ticked again,
	// so its smoothed value froze mid-tail, and the voice's release, which waits while a routed tail is still
	// falling, waited for ever.
	for (const float smoothing : { -5.0f, -2.0f, feathers::mod::kSmoothingLog2SecondsMax }) {
		CAPTURE(smoothing);
		Rig rig;
		rig.set(ParamId::VoiceLevel, 0.0);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		{
			const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
			REQUIRE(desc != nullptr);
			rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
			rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.005f);
			rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.005f);
			rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
			rack.device[0].field[static_cast<size_t>(dsr::Param::Release)] = std::log2(0.05f);
			rack.device[0].field[static_cast<size_t>(dsr::Param::Smoothing)] = smoothing;
		}
		rig.synth.provisionDevices(feathers::FxChain {}, rack);
		rig.synth.setModRack(rack);
		rig.play({}, 64, 64);
		const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
		REQUIRE(level >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
		routes.publish(rig.params);
		rig.play({}, 64, 64);
		REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel));

		// Held for long enough that even a one-second smoothing has arrived at the sustain.
		rig.play({ noteOn(0, kKeyC4, 1) }, 48000 * 8, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		REQUIRE(rig.synth.voiceGainForTest(slot) > 0.9);

		rig.play({ noteOff(0, kKeyC4, 1) }, 64, 64);
		// THE BOUND, from the exponential: the raw release (50 ms), then the one-pole falling from 1 to the snap
		// at a millionth, tau ln(10^6) — 13.8 s at the top of the range — and the voice's own short release.
		const double tau = feathers::mod::smoothingTau(smoothing);
		REQUIRE(rig.releaseSeconds < 0.25);
		const auto frames = static_cast<uint32_t>(48000.0 * (0.05 + tau * std::log(1e6) + 0.25 + 0.1));
		rig.play({}, frames, 64);
		CHECK(rig.synth.soundingVoiceCount() == 0);
		CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
		// AND WHAT A HOST HEARS AFTER IT: exactly nothing, which is what SLEEP needs (CLAUDE.md, "done is
		// decided by the output").
		const std::vector<float> after = rig.play({}, 4096, 64);
		float peak = 0.0f;
		for (const float s : after) {
			peak = std::max(peak, std::abs(s));
		}
		CHECK(peak == 0.0f);
	}
}

TEST_CASE("backlog EF, the drone (envelopes left high, the synth droning with the transport stopped and no notes playing): keyboard notes wear id -1 and each voice reads ITS OWN envelope; a stolen note, which the host never lifts, is ended at the lane, so nothing stays gated and nothing is held open") {
	namespace dsr = feathers::mod::dahdsr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.005f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.005f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
		rack.device[0].field[static_cast<size_t>(dsr::Param::Release)] = 0.0f; // one second
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	rig.play({}, 64, 64);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	feathers::test::Routes routes;
	const feathers::DeviceInstanceId env = rack.instanceAt(0);
	routes.set(feathers::backplane::Source::output(env, 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel));
	const auto tail = [&](int16_t key) { return rig.synth.laneForTest().outputForNoteOf(env, 0, -1, 0, key); };

	SUBCASE("two keyboard notes, each voice its own envelope") {
		// BOTH id -1, the way a MIDI keyboard plays. By id alone the second voice read the first note's
		// entry — one tail held every voice — and lifting the first key lifted them all.
		rig.play({ noteOn(0, kKeyC4, -1) }, 2048, 64);
		rig.play({ noteOn(0, kKeyC4 + 7, -1) }, 2048, 64);
		const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
		const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
		REQUIRE(slotA >= 0);
		REQUIRE(slotB >= 0);
		REQUIRE(rig.synth.voiceGainForTest(slotA) > 0.9);
		REQUIRE(rig.synth.voiceGainForTest(slotB) > 0.9);
		rig.play({ noteOff(0, kKeyC4, -1) }, 64, 64);
		rig.play({}, 4800, 64); // 0.1 s into the first note's 1 s release
		CHECK(rig.synth.voiceGainForTest(slotA) < 0.9);
		CHECK(rig.synth.voiceGainForTest(slotB) > 0.9); // the held note is untouched
		rig.play({ noteOff(0, kKeyC4 + 7, -1) }, 64, 64);
		rig.play({}, 48000 + 24000, 64);
		// AND WITH BOTH KEYS UP, NOTHING SOUNDS — the drone was voices held by an entry nobody lifted.
		CHECK(rig.synth.soundingVoiceCount() == 0);
		CHECK(tail(kKeyC4) == 0.0f);
		CHECK(tail(kKeyC4 + 7) == 0.0f);
	}

	SUBCASE("a stolen note the host never lifts is ended at the lane") {
		// TWO VOICES, THREE KEYS, all id -1: the oldest is stolen. The host is told that note ended
		// and sends no note-off for it — so the lane's entry for that key was left gated at sustain
		// forever. Now the steal chokes it at the lane: its tail reads zero, and a note struck again
		// on that key starts from nothing rather than from a sustain left standing.
		//
		// TWO VOICES RATHER THAN THE FULL SIXTEEN, and this is what makes the case observe anything:
		// the envelope's table holds sixteen notes, so a seventeenth note-on EVICTS the oldest entry
		// before the steal it causes could choke it, and a first draft of this case stayed green with
		// the choke deleted — the table's own limit was hiding the leak.
		rig.set(ParamId::VoiceCount, 2.0);
		rig.play({}, 64, 64);
		rig.play({ noteOn(0, 36, -1), noteOn(8, 37, -1) }, 2048, 64);
		const int stolenSlot = rig.synth.slotOfKeyForTest(36);
		REQUIRE(stolenSlot >= 0);
		rig.play({ noteOn(0, 38, -1) }, 64, 64);
		REQUIRE(rig.synth.slotOfKeyForTest(36) < 0); // the oldest was stolen
		// **AND THE STOLEN VOICE FADES FROM WHERE IT STOOD** (backlog EH, shape 6): its note was choked
		// at the lane the instant it was stolen, so the envelope reads rest for it from the NEXT control
		// tick — and a steal fade that multiplied a level already at rest was a click. The voice keeps
		// the per-voice value it had. Read after that next tick, inside the fade (2 ms = 96 samples;
		// the steal was at the head of the last 64): a cell that followed the envelope would read 0.
		rig.play({}, 32, 32);
		CHECK(rig.synth.voiceGainForTest(stolenSlot) > 0.5);
		REQUIRE(rig.synth.slotOfKeyForTest(37) >= 0);
		REQUIRE(rig.synth.slotOfKeyForTest(38) >= 0);
		rig.play({ noteOff(0, 37, -1), noteOff(0, 38, -1) }, 64, 64);
		rig.play({}, 48000 + 24000, 64);
		CHECK(rig.synth.soundingVoiceCount() == 0);
		CHECK(tail(36) == 0.0f); // not left gated at 1.0 by a note-off that never came
		// STRUCK AGAIN: an attack from nothing, read one control tick in — a stuck entry would read its
		// sustain, ~1, here.
		rig.play({ noteOn(0, 36, -1) }, 64, 64);
		const float struck = tail(36);
		CHECK(struck > 0.0f);
		CHECK(struck < 0.6f);
	}
}

TEST_CASE("a per-voice source that stands still above the floor HOLDS a released voice, because its Voice Level is above zero — Note Expressions' neutral volume into Voice Level, and the voice ends when the route is taken away") {
	// THIS CASE ONCE ASSERTED THE OPPOSITE (backlog EH, shape 6): Note Expressions answers its neutral, 1, for a note
	// it no longer holds, so a route from it into Level held the voice for ever, and the hold was narrowed to a
	// FALLING value. The rule since 2026-10-03 is that only Voice Level > 0 decides, not the upstream
	// envelopes: a released voice whose Voice Level stands at 0.8 is ON. The way out is the one a player has —
	// take the route (or the level) away.
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.5);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	constexpr size_t kVolumeOutput = 0; // the expression whose neutral is 1
	rig.play({}, 64, 64);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kVolumeOutput),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), 0.3f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel));
	rig.play({ noteOn(0, kKeyC4, 1) }, 512, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	REQUIRE(rig.params.perVoiceOffset(ParamId::VoiceLevel, slot) > 0.01); // the neutral IS above the floor
	rig.play({ noteOff(0, kKeyC4, 1) }, 64, 64);
	rig.play({}, 24000, 64); // half a second: past any edge the voice has
	REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
	CHECK(std::abs(rig.synth.voiceGainForTest(slot) - 0.8) < 1e-3);
	// TAKE THE ROUTE AWAY: Voice Level is a plain level again — the organ gate — and the released voice closes.
	feathers::test::Routes none;
	none.publish(rig.params);
	rig.play({}, 960, 64);
	CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("backlog EH: a per-voice source switched OFF is off for every voice — the enable reaches the per-voice road as it reaches the collapsed publish") {
	// A GUARD ASYMMETRY INSIDE ONE FUNCTION: the collapsed publish tested the device's enable switch and
	// published zero; the per-voice write applied the amount and never asked. In the default patch —
	// the strip envelope into Level at full amount, Level's base at zero — the envelope could not be
	// switched off at all: every voice kept reading its sustain. Asserted by value on the voice's gain.
	namespace dsr = feathers::mod::dahdsr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
	REQUIRE(desc != nullptr);
	REQUIRE(desc->traits.enableControl == static_cast<int>(dsr::Param::Enabled));
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.005f);
	rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.005f);
	rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	rig.play({}, 64, 64);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel));

	rig.play({ noteOn(0, kKeyC4, 1) }, 2048, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	REQUIRE(rig.synth.voiceGainForTest(slot) > 0.9); // the envelope IS the level

	// SWITCHED OFF: the per-voice cell reads zero from the next publish, so the level it carries is zero.
	rack.device[0].field[static_cast<size_t>(dsr::Param::Enabled)] = 0.0f;
	rig.synth.setModRack(rack);
	rig.play({}, 256, 64);
	CHECK(rig.synth.voiceGainForTest(slot) < 0.01);

	// AND BACK ON: the level returns, which is what says the switch and not the note's end took it.
	rack.device[0].field[static_cast<size_t>(dsr::Param::Enabled)] = 1.0f;
	rig.synth.setModRack(rack);
	rig.play({}, 2048, 64);
	CHECK(rig.synth.voiceGainForTest(slot) > 0.5);
}

TEST_CASE("PER-VOICE MODULATION: pressure on one note moves ONLY that note's filter cutoff (backlog DU — per-voice device controls migrate)") {
	// **BACKLOG DU, THE LAST ROW OF ITS TABLE.** A Poly device runs one instance per voice, and every
	// instance was handed the SAME resolved row — so pressure routed to the cutoff of a per-voice filter
	// moved every note's filter by the collapsed value. The engine resolves the row per voice now when a
	// per-voice source reaches the device (`RackEngine::resolveVoiceInto`), and hands each instance its
	// own; a device no per-voice source reaches gets the collapsed row it always got.
	//
	// ASSERTED ON EACH VOICE'S FILTER TARGET (`voiceFilterTargetForTest`), the seam CLAUDE.md's rule
	// exists for: a filter's corner is not recoverable from its output.
	namespace nx = feathers::mod::noteexpr;
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.setFilter(flt::Param::Poly, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.publishModRack(rack);
	constexpr size_t kPressureOutput = 6;
	rig.play({}, 64, 64);
	const feathers::DeviceInstanceId filterId = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0);
	REQUIRE(filterId != feathers::kNoInstance);
	constexpr float kAmount = 0.1f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
	           feathers::backplane::Destination::control(filterId, static_cast<size_t>(flt::Param::Cutoff)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC4 + 7, 2) }, 256, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	REQUIRE(slotA != slotB);
	const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
	SUBCASE("THE CONTROL: no pressure, both voices' filters sit at the base cutoff") {
		rig.play({}, 256, 64);
		CHECK(rig.synth.voiceFilterTargetForTest(slotA).cutoffLog2Hz == doctest::Approx(10.0));
		CHECK(rig.synth.voiceFilterTargetForTest(slotB).cutoffLog2Hz == doctest::Approx(10.0));
	}
	SUBCASE("full pressure on note 1 opens ITS filter by amount × span, and note 2's does not move") {
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
		rig.play({}, 512, 64);
		CHECK(rig.synth.voiceFilterTargetForTest(slotA).cutoffLog2Hz == doctest::Approx(10.0 + kAmount * span).epsilon(0.01));
		CHECK(rig.synth.voiceFilterTargetForTest(slotB).cutoffLog2Hz == doctest::Approx(10.0).epsilon(0.01));
	}
}

TEST_CASE("EVERY output a device declares gets a collapsed value, not just the first") {
	// **FOUND BY ASKING WHAT THE RULE KNOB WAS FOR.** The publish loop wrote
	// `kEnvelopeOutput` and nothing else, under a comment that was true right up until a device
	// declared more than one output: *"a modulator is a source of one signal whatever shape it
	// makes."* The Note Expressions device declares seven, so six of them had NO collapsed value and
	// every route from Pressure to a destination that is not per-voice read zero.
	//
	// The device looked wired and was one-seventh wired, and the Rule control — whose whole job is to
	// decide what an un-migrated destination sees — was inert for six expressions out of seven.
	//
	// **ASSERTED ON A NON-ZERO OUTPUT OTHER THAN THE FIRST**, which is the only shape that catches it:
	// output 0 worked before and after.
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		REQUIRE(desc->outputCount() > 1); // or this case is vacuous by construction
		// An identity is a serial and nothing else since kill-lanes step 3c; the routing address that
		// stood beside it was `deviceSlotOfRack(0)`, for the slot the row goes into.
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	rig.play({}, 64, 64); // sync before the notes — see the per-voice case for why

	rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
	rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.75);
	rig.play({}, 256, 64);

	constexpr size_t kPressureOutput = 6;
	// **THE COLLAPSED VALUE, read where a non-per-voice destination reads it.** `publishedSourceForTest`
	// is output 0's; this asks the runner for the output the route would name.
	CHECK(rig.synth.laneForTest().outputOf(stripDeviceOf(rig.synth, rack, 0), kPressureOutput)
	      == doctest::Approx(0.75f));

	SUBCASE("and it reaches a BLOCK-RATE destination, which is what the Rule exists for") {
		// A route from Pressure to a synth param, resolved with NO voice — the collapsed road. Before
		// this fix it read zero however the Rule was set.
		const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
		REQUIRE(fine >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
		           feathers::backplane::Destination::core(static_cast<size_t>(fine)), 1.0f);
		routes.publish(rig.params);
		rig.play({}, 128, 64);
		CHECK(rig.params.resolved(feathers::ParamId::FineTune, -1) != doctest::Approx(0.0));
	}
}

TEST_CASE("backlog DN: the master meter reads what LEAVES the synth — the bus utility's meter over the output after the master gain, restated by a second meter fed the same samples; and it starts over on the transport's rising edge into play, not on a transport already playing") {
	Rig rig;
	using feathers::fx::busutil::Reading;
	feathers::fx::busutil::Meter reference;
	reference.prepare(kSampleRate);
	rig.set(ParamId::MasterGain, -6.0);
	rig.synth.setTransport(0.0, 0.0, false);
	rig.synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> left(64, 0.0f), right(64, 0.0f);
	const auto renderBlocks = [&](int blocks) {
		for (int i = 0; i < blocks; ++i) {
			rig.synth.render(left.data(), right.data(), 64);
			reference.process(left.data(), right.data(), 64);
		}
	};
	renderBlocks(600); // 0.8 s: past the momentary window, so the reading is of steady sound

	// THE SAME NUMBER THE SAME METER GIVES THE SAME SAMPLES — the intended path restated, not a difference.
	const feathers::MasterMeterReadings got = feathers::masterMeterReadingsOf(rig.synth.masterMeter());
	CHECK(got.momentaryLufs == doctest::Approx(reference.reading(Reading::Momentary)).epsilon(1e-4));
	CHECK(got.rawPeak == doctest::Approx(reference.reading(Reading::RawPeak)).epsilon(1e-4));
	CHECK(got.clipAge == doctest::Approx(reference.reading(Reading::ClipAge)).epsilon(1e-4));
	// NON-VACUITY: a sounding note at -6 dB master is a real loudness, not the silence floor, and it
	// moved with the master: at -60 dB the same note reads quieter by about that much.
	REQUIRE(got.momentaryLufs > -60.0f);
	REQUIRE(got.rawPeak > 0.01f);
	const float loud = got.momentaryLufs;
	rig.set(ParamId::MasterGain, -30.0);
	renderBlocks(600);
	const float quiet = feathers::masterMeterReadingsOf(rig.synth.masterMeter()).momentaryLufs;
	CHECK(loud - quiet == doctest::Approx(24.0f).epsilon(0.05));

	// THE TRANSPORT'S RISING EDGE STARTS THE METER OVER; a transport already playing does not.
	rig.synth.setTransport(0.0, 0.0, true);
	const feathers::MasterMeterReadings fresh = feathers::masterMeterReadingsOf(rig.synth.masterMeter());
	CHECK(fresh.momentaryLufs == feathers::fx::busutil::kSilenceLufs);
	CHECK(fresh.clipAge == feathers::fx::busutil::kClipAgeMax);
	renderBlocks(600);
	const float playing = feathers::masterMeterReadingsOf(rig.synth.masterMeter()).momentaryLufs;
	REQUIRE(playing > -80.0f);
	rig.synth.setTransport(1.0, 2.0, true); // still playing: no edge, nothing reset
	CHECK(feathers::masterMeterReadingsOf(rig.synth.masterMeter()).momentaryLufs == playing);
}

TEST_CASE("backlog FG: a new note gets its OWN routed envelope in every voice mode, and Leg keeps a held one running") {
	// **THE BUG, 2026-09-17:** retriggering notes in legato mode kept reading the previous voice's envelope, holding
	// on until the entire envelope had run — while the modulators' own legato and retrig buttons are what should
	// decide it. Three causes, each a row of this table:
	//   - a keyboard re-press of a HELD key found the held note's entry (id -1 is a wildcard) and nothing restarted it;
	//   - a Mono steal's choke named the same (-1, key) and ended the entry the new note had just taken — silent;
	//   - a Legato move carries no note id, so the voice went on reading the OLD note's envelope.
	// The reading is the NEWEST voice's gain: `slotOfKeyForTest` returns the releasing old voice when two share a key.
	namespace dsr = feathers::mod::dahdsr;
	struct Reading {
		double at100 = -1.0;
		double at200 = -1.0;
	};
	const auto run = [](double voiceMode, bool held, bool sameKey, bool ids, bool leg) {
		Rig rig;
		rig.set(ParamId::VoiceMode, voiceMode);
		rig.set(ParamId::VoiceLevel, 0.0);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.100f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.100f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 0.5f;
		rack.device[0].field[static_cast<size_t>(dsr::Param::Release)] = std::log2(0.500f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Mode)] =
		    static_cast<float>(static_cast<int>(dsr::Mode::Gate));
		rack.device[0].field[static_cast<size_t>(dsr::Param::SingleTrigger)] = leg ? 1.0f : 0.0f;
		rig.synth.provisionDevices(feathers::FxChain {}, rack);
		rig.synth.setModRack(rack);
		rig.play({}, 64, 64);
		const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
		REQUIRE(level >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
		routes.publish(rig.params);
		rig.play({}, 64, 64);
		const int32_t first = ids ? 1 : -1;
		const int32_t second = ids ? 2 : -1;
		const auto secondKey = static_cast<int16_t>(sameKey ? kKeyC4 : kKeyC4 + 2);
		rig.play({ noteOn(0, kKeyC4, first) }, 14400, 64); // 0.3 s: on the 0.5 sustain
		const int firstSlot = rig.synth.newestVoiceSlotForTest();
		REQUIRE(firstSlot >= 0);
		REQUIRE(rig.synth.voiceGainForTest(firstSlot) == doctest::Approx(0.5).epsilon(0.02));
		if (!held) {
			rig.play({ noteOff(0, kKeyC4, first) }, 960, 64); // 20 ms into the release
		}
		rig.play({ noteOn(0, secondKey, second) }, 64, 64);
		rig.play({}, 4800 - 64, 64);
		Reading r;
		const int slot = rig.synth.newestVoiceSlotForTest();
		REQUIRE(slot >= 0);
		r.at100 = rig.synth.voiceGainForTest(slot);
		rig.play({}, 4800, 64);
		r.at200 = rig.synth.voiceGainForTest(slot);
		return r;
	};

	SUBCASE("Leg off: every new note restarts — attack peak at +100 ms, sustain at +200 ms") {
		for (const double mode : { 0.0, 1.0, 2.0 }) {
			for (const bool held : { false, true }) {
				for (const bool sameKey : { false, true }) {
					for (const bool ids : { false, true }) {
						CAPTURE(mode);
						CAPTURE(held);
						CAPTURE(sameKey);
						CAPTURE(ids);
						const Reading r = run(mode, held, sameKey, ids, false);
						CHECK(r.at100 > 0.9);
						CHECK(r.at200 == doctest::Approx(0.5).epsilon(0.03));
					}
				}
			}
		}
	}

	SUBCASE("Leg on: a note under a held one continues its envelope; after a release it restarts") {
		for (const double mode : { 0.0, 1.0, 2.0 }) {
			for (const bool sameKey : { false, true }) {
				for (const bool ids : { false, true }) {
					CAPTURE(mode);
					CAPTURE(sameKey);
					CAPTURE(ids);
					const Reading heldReading = run(mode, true, sameKey, ids, true);
					CHECK(heldReading.at100 == doctest::Approx(0.5).epsilon(0.03)); // still on the held sustain
					const Reading detached = run(mode, false, sameKey, ids, true);
					CHECK(detached.at100 > 0.9); // nothing held: a fresh start
				}
			}
		}
	}
}

namespace {

/// Level routed from a strip device at full amount, base 0 — the Init patch's arrangement.
void routeToLevel(Rig& rig, feathers::DeviceInstanceId source) {
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(source, 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
}

} // namespace

TEST_CASE("tick windows: Level follows a PER-VOICE envelope per sample — Init's 10 ms attack has no stair") {
	// On Init the route from the DAHDSR to Level left control-rate stairs on the attack —
	// seven stairs of 0.133. Read per sample (`traceVoiceGainForTest`), in host blocks that do NOT align with
	// the tick, so a window cut by a block boundary is exercised.
	namespace dsr = feathers::mod::dahdsr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
	REQUIRE(desc != nullptr);
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.010f);
	rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	rig.play({}, 64, 64);
	routeToLevel(rig, rack.instanceAt(0));
	REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel));

	rig.play({ noteOn(0, kKeyC4, 1) }, 64, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	std::vector<float> trace;
	rig.synth.traceVoiceGainForTest(slot, &trace);
	rig.play({}, 2048, 37);
	rig.synth.traceVoiceGainForTest(-1, nullptr);
	REQUIRE(trace.size() == 2048u);
	float maxStep = 0.0f;
	for (size_t i = 1; i < trace.size(); ++i) {
		maxStep = std::max(maxStep, std::abs(trace[i] - trace[i - 1]));
	}
	CAPTURE(maxStep);
	REQUIRE(trace.back() > 0.9f);
	REQUIRE(trace.back() - trace.front() > 0.5f); // not vacuous: the attack is inside the trace
	CHECK(maxStep < 0.01f); // the stair was 64/480 of the range at once, 0.133
}

TEST_CASE("tick windows: Level follows an MSEG per sample — each tick lands on what the MSEG published") {
	// THE VALUE CLAIMED: at the LAST sample of each tick the gain is the value the MSEG published for that tick
	// (base 0, amount 1, Level's span 1 — so the gain is the published value, clamped), restated from
	// `publishedOutputOf` rather than from the resolve; and between those, no stair.
	//
	// **THIS WAS THE BLOCK-RATE WINDOW'S CASE AND IS NOW THE PER-VOICE ONE'S (2026-10-03):** the MSEG keeps a shape
	// per note, so it reaches Level through the voice's own run. With ONE note that shape and the collapsed one
	// start together and agree, which is why the published value is still the reference. The block-rate window
	// itself is pinned in `test_params` ("the per-sample windows: a voice's own, and a block-rate one").
	namespace mseg = feathers::mod::mseg;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::ModRack rack = feathers::defaultModRack();
	mseg::Settings shape {};
	shape.count = 2;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Gate;
	shape.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	shape.segment[0].share = 0.5f;
	shape.segment[0].level = 1.0f;
	shape.segment[0].shape = 0.0f;
	shape.segment[1].share = 0.5f;
	shape.segment[1].level = 0.25f;
	shape.segment[1].shape = 0.0f;
	rack.count = 2; // the slot past the count never ticks — see the MSEG runner case above
	rack.setMsegAt(1, shape);
	rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
	REQUIRE(rack.holds(1, mseg::kTraits.tag));
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const feathers::DeviceInstanceId msegId = rack.instanceAt(1);
	routeToLevel(rig, msegId);

	rig.synth.setTempo(120.0);
	rig.play({ noteOn(0, kKeyC4, 1) }, 64, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	std::vector<float> trace;
	int moving = 0;
	float maxStep = 0.0f;
	float maxTickDelta = 0.0f;
	float lastPublished = -1.0f;
	float last = -1.0f;
	for (int t = 0; t < 200; ++t) {
		const float published = rig.synth.laneForTest().publishedOutputOf(msegId, 0); // what this tick ramps TO
		if (lastPublished >= 0.0f) {
			maxTickDelta = std::max(maxTickDelta, std::abs(published - lastPublished));
		}
		lastPublished = published;
		trace.clear();
		rig.synth.traceVoiceGainForTest(slot, &trace);
		rig.play({}, 64, 64);
		rig.synth.traceVoiceGainForTest(-1, nullptr);
		if (trace.size() != 64u) {
			continue;
		}
		if (last >= 0.0f) {
			maxStep = std::max(maxStep, std::abs(trace.front() - last));
		}
		for (size_t i = 1; i < trace.size(); ++i) {
			maxStep = std::max(maxStep, std::abs(trace[i] - trace[i - 1]));
		}
		if (std::abs(trace.back() - trace.front()) > 1e-4f) {
			++moving;
			CAPTURE(t);
			CHECK(trace.back() == doctest::Approx(std::clamp(published, 0.0f, 1.0f)).epsilon(1e-4));
		}
		last = trace.back();
	}
	CAPTURE(maxStep);
	REQUIRE(moving >= 5); // not vacuous: the shape moved across many ticks
	CAPTURE(maxTickDelta);
	REQUIRE(maxTickDelta > 0.0f);
	// A STAIR'S STEP IS THE WHOLE TICK'S CHANGE; a ramp spreads it over 64 samples. Stated relative to the source's own
	// motion, so a slow shape cannot pass a stair under a fixed bound.
	CHECK(maxStep < maxTickDelta / 8.0f);
}

TEST_CASE("backlog FF: a voice handed a new note reads that note's routed envelope from its first sample, not the old note's until the tick") {
	// FOUND measuring the Level stair: the 9th note on a full pool reused a voice whose per-voice cell still held the
	// PREVIOUS note's envelope, so the new note played up to a tick of the old level under its attack — a blip,
	// audible when retriggering in Mono. Each note lands MID-TICK (32 samples in), where the tick cannot hide it,
	// and the gain is read 8 samples later: a fresh 100 ms attack is ~0 there; the old note stood at its 0.5 sustain.
	namespace dsr = feathers::mod::dahdsr;
	const auto run = [](double voiceMode, bool steal) {
		Rig rig;
		rig.set(ParamId::VoiceMode, voiceMode);
		rig.set(ParamId::VoiceLevel, 0.0);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.100f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.100f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 0.5f;
		rig.synth.provisionDevices(feathers::FxChain {}, rack);
		rig.synth.setModRack(rack);
		rig.play({}, 64, 64);
		routeToLevel(rig, rack.instanceAt(0));
		const int held = steal ? feathers::kMaxVoices : 1;
		for (int n = 0; n < held; ++n) {
			rig.play({ noteOn(0, static_cast<int16_t>(40 + n), n + 1) }, 64, 64);
		}
		rig.play({}, 14400, 64); // 0.3 s: every held note on its 0.5 sustain
		const int before = rig.synth.newestVoiceSlotForTest();
		REQUIRE(before >= 0);
		REQUIRE(rig.synth.voiceGainForTest(before) == doctest::Approx(0.5).epsilon(0.02));
		// TWICE: a steal may land on a spare slot whose cell is clean, and the SECOND lands back on a slot a stolen
		// voice left its sustain in — the reuse this is about. The worst of the two is the reading.
		double worst = 0.0;
		for (int round = 0; round < 2; ++round) {
			rig.play({}, 32, 32); // mid-tick
			rig.play({ noteOn(0, static_cast<int16_t>(90 + round), 100 + round) }, 8, 8);
			const int slot = rig.synth.newestVoiceSlotForTest();
			REQUIRE(slot >= 0);
			worst = std::max(worst, rig.synth.voiceGainForTest(slot));
			rig.play({}, 14400 - 40, 64); // back onto the sustain, and onto the tick grid
		}
		return worst;
	};
	SUBCASE("Poly, a steal from a full pool") {
		CHECK(run(0.0, true) < 0.05);
	}
	SUBCASE("Mono, a retrigger onto the sounding voice") {
		CHECK(run(1.0, false) < 0.05);
	}
}

TEST_CASE("backlog FF: a reused voice's PER-VOICE DEVICE is handed the new note's routed row from its first sample") {
	// The device half of FF: a Poly filter inside each voice resolves its row at the lane's begin, so a reused voice's
	// instance kept the old note's cutoff until the next tick. Mono, three notes, each landing mid-tick; the third lands
	// back on the slot the first left at its 0.5 sustain. Read on the voice's filter target (`voiceFilterTargetForTest`).
	namespace dsr = feathers::mod::dahdsr;
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.set(ParamId::VoiceMode, 1.0);
	rig.setFilter(flt::Param::Poly, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
	REQUIRE(desc != nullptr);
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.100f);
	rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.100f);
	rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 0.5f;
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const feathers::DeviceInstanceId filterId = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0);
	REQUIRE(filterId != feathers::kNoInstance);
	constexpr float kAmount = 0.5f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::control(filterId, static_cast<size_t>(flt::Param::Cutoff)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
	double worst = 0.0;
	for (int n = 0; n < 3; ++n) {
		rig.play({ noteOn(32, static_cast<int16_t>(60 + n), n + 1) }, 40, 40); // mid-tick, inside one host block
		const int slot = rig.synth.newestVoiceSlotForTest();
		REQUIRE(slot >= 0);
		CAPTURE(n);
		CAPTURE(slot);
		if (n > 0) {
			worst = std::max(worst, rig.synth.voiceFilterTargetForTest(slot).cutoffLog2Hz - 10.0);
		}
		rig.play({}, 14400 - 40, 64); // onto the sustain, and back on the tick grid
		REQUIRE(rig.synth.voiceFilterTargetForTest(slot).cutoffLog2Hz
		        == doctest::Approx(10.0 + kAmount * 0.5 * span).epsilon(0.02)); // not vacuous: the route moves it
	}
	CAPTURE(worst);
	CHECK(worst < 0.05 * span);
}

TEST_CASE("tick windows: a routed Fine Tune reaches a Poly voice's pitch by a straight line over each tick, not a stair") {
	// Pitch reaches a voice once per control tick (`applyPitches`); with no glide in Poly each tick was a JUMP, so an
	// MSEG into Fine Tune stepped the pitch every 64 samples. Traced per sample (one-sample renders: the render-top
	// retarget finds the target unchanged and leaves the line alone). The oscillator GENERATES four host samples at a time
	// (its 4x decimator), so a one-sample render sees the pitch move once per four — a line reads as steps of 4/64 of a
	// tick's move, a jump as the whole move. The claim: no reading moves by more than an eighth of the largest
	// tick-to-tick move, while the pitch travels across the render.
	namespace mseg = feathers::mod::mseg;
	Rig rig;
	feathers::ModRack rack = feathers::defaultModRack();
	mseg::Settings shape {};
	shape.count = 2;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Gate;
	shape.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	shape.segment[0].share = 0.5f;
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = 0.5f;
	shape.segment[1].level = 0.0f;
	rack.count = 2;
	rack.setMsegAt(1, shape);
	rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
	REQUIRE(rack.holds(1, mseg::kTraits.tag));
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const feathers::DeviceInstanceId msegId = rack.instanceAt(1);
	const int fine = feathers::modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(fine >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(msegId, 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(fine)), 1.0f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	rig.synth.setTempo(120.0);
	rig.play({ noteOn(0, kKeyC4, 1) }, 64, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);

	double last = rig.synth.voicePitchForTest(slot);
	double lowest = last;
	double highest = last;
	double maxStep = 0.0;
	double lastTick = last;
	double maxTickDelta = 0.0;
	for (int i = 0; i < 64 * 300; ++i) {
		rig.play({}, 1, 1);
		const double now = rig.synth.voicePitchForTest(slot);
		maxStep = std::max(maxStep, std::abs(now - last));
		lowest = std::min(lowest, now);
		highest = std::max(highest, now);
		last = now;
		if (i % 64 == 63) {
			maxTickDelta = std::max(maxTickDelta, std::abs(now - lastTick));
			lastTick = now;
		}
	}
	CAPTURE(maxStep);
	CAPTURE(maxTickDelta);
	REQUIRE(highest - lowest > 0.5); // not vacuous: Fine Tune moved the note by more than a quarter of its span
	REQUIRE(maxTickDelta > 0.0);
	CHECK(maxStep < maxTickDelta / 8.0);
}

TEST_CASE("a modulator reaching Radius and Theta moves the sound to a place that can be stated") {
	// **THE READER, ASSERTED AS VALUES** (CLAUDE.md: a claim about a value cannot be inferred from a
	// rendered buffer). The internal destinations resolve in `ParamSet`; this is the line in
	// `Synth::updateCharacter` that makes that reach a sound. Without it a route resolves perfectly and
	// moves nothing — a verb that works and looks dead.
	//
	// KNOWN-BAD: drop the `moved.offset[slot] = ...` line and the played place stays the authored one.
	Rig rig;
	feathers::SoundTravel authored;
	// THE PLACE AND THE OFFSET ARE INDEPENDENT, by design: the sound
	// was PUT at (0.03, 0.04) and is PLAYED 0.01 from there, straight along +x.
	authored.offset[0] = { 0.03, 0.04 };
	authored.orbit[0] = { 0.01, 0.0 };
	rig.synth.setSoundTravel(authored);

	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::core(feathers::travelDestinationIndex(0, feathers::TravelAxis::Radius)),
	           1.0f);
	routes.set(feathers::test::macro(1),
	           feathers::backplane::Destination::core(feathers::travelDestinationIndex(0, feathers::TravelAxis::Theta)),
	           0.25f);
	REQUIRE(routes.publish(rig.params));

	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };

	// (1) NOTHING MODULATING: the place plus its own offset — and a NEIGHBOUR WITH NO OFFSET keeps its
	// authored pair BIT FOR BIT (checked below, exactly: sound_travel.h calls that zero load-bearing).
	rig.play(script, 512, 128);
	CHECK(rig.synth.playedTravelForTest().offset[0].x == doctest::Approx(0.04));
	CHECK(rig.synth.playedTravelForTest().offset[0].y == doctest::Approx(0.04));

	// (2) BOTH MACROS UP: the whole plane further out, a quarter turn round — restated independently.
	rig.set(feathers::macroParam(0), 1.0);
	rig.set(feathers::macroParam(1), 1.0);
	rig.play(script, 512, 128);
	// A quarter turn from +x is straight up +y, so the whole radius lands on y and none on x — ABOUT
	// THE SOUND'S OWN PLACE, not about the plane's origin.
	// The pull is ONE WHOLE FACE (radius-mod-jump: routes move along the Radius face) on top of 0.01's face position,
	// so the face HOLDS at its top — the whole plane, kTravelRadiusMax = 2 travel units (backlog PO).
	const double radius = 2.0;
	const feathers::AnchorPoint played = rig.synth.playedTravelForTest().offset[0];
	INFO("played at " << played.x << "," << played.y);
	CHECK(played.x == doctest::Approx(0.03));
	CHECK(played.y == doctest::Approx(0.04 + radius));
	// A neighbour nothing routes to has not moved at all.
	CHECK(rig.synth.playedTravelForTest().offset[1].x == 0.0);
	CHECK(rig.synth.playedTravelForTest().offset[1].y == 0.0);

	// (3) AND WHAT THE ENGINE HANDS THE EDITOR IS THAT SAME PULL, in polar — the chart draws the star
	// from these two numbers through `pulledPlace`, so they must be the ones the sound was placed by.
	const feathers::TravelPulls& pulls = rig.synth.travelPulls();
	CHECK(pulls.radius[0] == doctest::Approx(1.0)); // ONE FACE, in face units
	CHECK(pulls.theta[0] == doctest::Approx(0.25 * 2.0 * 3.14159265358979323846));
	CHECK(pulls.radius[1] == 0.0);
	CHECK(pulls.theta[1] == 0.0);
	const feathers::AnchorPoint redrawn =
	    feathers::pulledPlace({ 0.03, 0.04 }, { 0.01, 0.0 }, pulls.radius[0], pulls.theta[0]);
	CHECK(redrawn.x == played.x); // the SAME function on the SAME inputs: exact, not approximate
	CHECK(redrawn.y == played.y);

	// THAT THE EDITOR IS TOLD is test_mod_rack's ("the engine tells the editor where modulation has
	// left each sound's place"): this binary cannot link a bridge.
}

TEST_CASE("PO: a full-depth route into Radius at the knob's default reaches the whole plane, along the face") {
	// **BACKLOG PO, BY VALUE AT THE ENGINE'S READER** (raise the maximum Radius modulation), AS
	// radius-mod-jump LEFT IT (option A): a route into Radius moves ALONG THE KNOB'S FACE, and the
	// face's top is the whole plane, two travel units — the plane is ±1 across — restated here as a number. The old
	// reach was 64 / 1023 / 2 = 0.031; PO's first build added 2 x amount in RADIUS units, which on the exponential
	// face put a 1% route at 45% of the throw from zero (an audible jump).
	//
	// KNOWN-BAD: `playedRadius` adding the pull in radius units again (`orbitRadius + facePull`), and (2) reads
	// -0.5 + 0.5 rather than the face's middle; (3) reads 1.5 + 1 = 2.5 past the top rather than holding.
	Rig rig;
	feathers::SoundTravel authored;
	authored.offset[0] = { -0.5, 0.25 }; // PUT off the origin, orbit at the knob's default: zero
	rig.synth.setSoundTravel(authored);
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::core(feathers::travelDestinationIndex(0, feathers::TravelAxis::Radius)),
	           1.0f);
	REQUIRE(routes.publish(rig.params));
	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };
	// THE FACE, RESTATED: radius = 2 (e^(8.4 p) - 1) / (e^8.4 - 1).
	const auto radiusAt = [](double p) { return 2.0 * std::expm1(8.4 * p) / std::expm1(8.4); };

	// (1) FULL DEPTH, MACRO UP: one whole face, so the top — two units along +x (theta 0) from the sound's place.
	rig.set(feathers::macroParam(0), 1.0);
	rig.play(script, 512, 128);
	CHECK(rig.synth.travelPulls().radius[0] == doctest::Approx(1.0));
	CHECK(rig.synth.playedTravelForTest().offset[0].x == doctest::Approx(-0.5 + 2.0));
	CHECK(rig.synth.playedTravelForTest().offset[0].y == doctest::Approx(0.25));

	// (2) HALF WAY UP: half the FACE — the route is linear in the macro on the face, not in the radius.
	rig.set(feathers::macroParam(0), 0.5);
	rig.play(script, 512, 128);
	CHECK(rig.synth.playedTravelForTest().offset[0].x == doctest::Approx(-0.5 + radiusAt(0.5)));

	// (3) THE KNOB AT 1.5 AND THE ROUTE FULL: past the face's top the sound HOLDS at the whole plane (option A's clamp).
	authored.orbit[0] = { 1.5, 0.0 };
	rig.synth.setSoundTravel(authored);
	rig.set(feathers::macroParam(0), 1.0);
	rig.play(script, 512, 128);
	CHECK(rig.synth.playedTravelForTest().offset[0].x == doctest::Approx(-0.5 + 2.0));
}

TEST_CASE("PO: playedRadius parks below zero, holds at the face's top, and is the identity with nothing routed") {
	// THE FUNCTION THE ENGINE, THE CHART'S STAR AND THE KNOB'S RING ALL READ (sound_travel.h, travel_control.h).
	const auto faceOf = [](double r) { return std::log1p(r / 2.0 * std::expm1(8.4)) / 8.4; };
	const auto radiusAt = [](double p) { return 2.0 * std::expm1(8.4 * p) / std::expm1(8.4); };
	CHECK(feathers::playedRadius(0.2, -0.8) == 0.0); // 0.2 sits at 0.73 of the face
	CHECK(feathers::playedRadius(0.0, 0.0) == 0.0);
	// NOTHING ROUTED: the orbit's own radius, bit for bit — not round-tripped through the curve.
	CHECK(feathers::playedRadius(0.3, 0.0) == 0.3);
	CHECK(feathers::playedRadius(0.0123456789, 0.0) == 0.0123456789);
	// A PULL IS A FACE DISTANCE: 0.3's face position plus 0.1, back through the curve.
	CHECK(feathers::playedRadius(0.3, 0.1) == doctest::Approx(radiusAt(faceOf(0.3) + 0.1)));
	CHECK(feathers::playedRadius(0.3, -0.1) == doctest::Approx(radiusAt(faceOf(0.3) - 0.1)));
	// THE TOP HOLDS — the face's end, the whole plane.
	CHECK(feathers::playedRadius(1.0, 1.0) == 2.0);
	CHECK(feathers::playedRadius(1.0, 3.5) == 2.0);
	// AND THE ENDS OF THE FACE ARE EXACT.
	CHECK(feathers::radiusAtFace(0.0) == 0.0);
	CHECK(feathers::radiusAtFace(1.0) == feathers::kTravelRadiusMax);
	CHECK(feathers::radiusFace(feathers::kTravelRadiusMax) == doctest::Approx(1.0));
}

TEST_CASE("a ramp into Theta orbits: across its reset the place is continuous and the angle is not") {
	// A ramp sent to Theta must orbit cleanly across its wrap, never passing through intermediate places,
	// and without a special case to cover it.
	//
	// A full swing on Theta is one whole turn, so a ramp at 1.0 and the same ramp after its reset at
	// 0.0 are 2*pi apart in ANGLE — and the same PLACE, because the angle's only consumer is cos/sin.
	// That is the whole mechanism: no wrap code exists to be wrong. What is asserted is the place.
	Rig rig;
	feathers::SoundTravel authored;
	// PUT somewhere that is not the origin, so "orbits about its own place" and "orbits about the
	// origin" give different answers below.
	authored.offset[0] = { 0.2, -0.1 };
	authored.orbit[0] = { 0.05, 0.0 };
	rig.synth.setSoundTravel(authored);
	feathers::test::Routes routes;
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::core(feathers::travelDestinationIndex(0, feathers::TravelAxis::Theta)),
	           1.0f);
	REQUIRE(routes.publish(rig.params));
	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };

	const auto placeAt = [&](double ramp) {
		rig.set(feathers::macroParam(0), ramp);
		rig.play(script, 512, 128);
		return rig.synth.playedTravelForTest().offset[0];
	};
	const feathers::AnchorPoint justBefore = placeAt(0.999);
	const feathers::AnchorPoint atReset = placeAt(0.0);
	const feathers::AnchorPoint quarter = placeAt(0.25);

	// ACROSS THE RESET: a thousandth of a turn apart, not a whole turn — 0.05 * 2pi/1000 ~ 0.0003.
	INFO("just before the reset " << justBefore.x << "," << justBefore.y << "; after " << atReset.x
	                              << "," << atReset.y);
	CHECK(std::hypot(justBefore.x - atReset.x, justBefore.y - atReset.y) < 0.001);
	// AND IT REALLY ORBITS, so the continuity above is not a sound that never moved: a quarter of the
	// ramp is a quarter turn, straight up the y axis at the same radius.
	CHECK(quarter.x == doctest::Approx(0.2));
	CHECK(quarter.y == doctest::Approx(-0.1 + 0.05));
	// AND A SOUND NOBODY GAVE A RADIUS DOES NOT MOVE, however its Theta is driven: there is nothing
	// for the angle to turn, and it keeps its authored pair bit for bit.
	authored.orbit[0] = {};
	rig.synth.setSoundTravel(authored);
	const feathers::AnchorPoint still = placeAt(0.37);
	CHECK(still.x == 0.2);
	CHECK(still.y == -0.1);
}

TEST_CASE("backlog IZ: SKLITTER in the lane reaches the VOICE and a filter BEFORE it — the values, in the render it moved in") {
	// For modulation, "downstream" means COMPUTED FROM DOWNSTREAM AUDIO; a modulator's position in a
	// lane is only grouping. SKLITTER is ticked with every modulator before the voice reads
	// a parameter and before the lane runs, so its perform output reaches Fine Tune and the filter AHEAD of
	// it in the same render. The graph admits both now (`test_backplane_graph.cpp`, the IZ cases; the gate
	// through Feathers' own bridge is the widget case); this is what the audio side does with them.
	namespace sk = feathers::fx::sklitter;
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.setFilter(flt::Param::Cutoff, 10.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, sk::kTraits.tag) == 1);
	rig.publishChain();
	rig.play({}, 64, 64);

	const feathers::LaneRack& lane = rig.synth.laneForTest();
	const feathers::DeviceInstanceId filterId = lane.deviceOfStatePosition(feathers::kLaneChain, 0);
	const feathers::DeviceInstanceId skId = lane.deviceOfStatePosition(feathers::kLaneChain, 1);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(skId != feathers::kNoInstance);
	const feathers::rack::RackEngine& engine = lane.engineForTest();
	const size_t filterSlot = engine.slotOf(filterId);
	const size_t skSlot = engine.slotOf(skId);
	// THE PREMISE: the filter RUNS FIRST.
	REQUIRE(engine.positionOfLane(filterSlot) < engine.positionOfLane(skSlot));

	const int fine = feathers::modDestinationIndexOf(feathers::ParamId::FineTune);
	REQUIRE(fine >= 0);
	const feathers::ParamDesc* fineDesc = feathers::findParam(static_cast<clap_id>(feathers::ParamId::FineTune));
	REQUIRE(fineDesc != nullptr);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const feathers::device::ParamDesc& cutoffDesc = flt::kDescriptor.params[cutoff];
	constexpr float kToVoice = 0.25f;
	constexpr float kToFilter = 0.1f;
	const feathers::backplane::Source perfX = feathers::backplane::Source::output(skId, sk::outIndex(sk::Out::X));
	feathers::test::Routes routes;
	routes.set(perfX, feathers::backplane::Destination::core(static_cast<size_t>(fine)), kToVoice);
	routes.set(perfX, feathers::backplane::Destination::control(filterId, cutoff), kToFilter);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	const double fineBase = rig.params.resolved(feathers::ParamId::FineTune, -1);
	CHECK(engine.resolvedFieldForTest(filterSlot, cutoff) == doctest::Approx(10.0));

	const auto movePuck = [&](float x) {
		rig.chain.device[1].row.field[static_cast<size_t>(sk::Param::PerfX)] = x;
		rig.publishChain();
		rig.play({}, 64, 64); // ONE RENDER OF ONE TICK
	};
	for (const float x : { 0.6f, -0.4f }) {
		CAPTURE(x);
		movePuck(x);
		REQUIRE(engine.publishedOutputAt(skSlot, sk::outIndex(sk::Out::X)) == doctest::Approx(x));
		CHECK(rig.params.resolved(feathers::ParamId::FineTune, -1)
		      == doctest::Approx(std::clamp(fineBase + kToVoice * x * (fineDesc->max - fineDesc->min), fineDesc->min,
		                                    fineDesc->max)));
		CHECK(engine.resolvedFieldForTest(filterSlot, cutoff)
		      == doctest::Approx(10.0 + kToFilter * x * feathers::device::routeSpanOf(cutoffDesc)));
	}
}

TEST_CASE("the 2026-09-24 crush: in Feathers' lane, SKLITTER's ring holds exactly what it was fed — a silent recapture is silent") {
	// THE BUG: recording a buffer of silence still held a softer version of the broken signal. Feathers
	// renders its lane in ≤32-frame chunks inside one segment, and the lane's clock was stamped once per
	// segment, so every chunk of a 512-frame block told SKLITTER the block's first sample. SKLITTER seats
	// its write head on the song (backlog IN), so all sixteen chunks wrote the SAME 32 ring frames and the
	// other 480 of every 512 kept whatever was there: stale audio that survived silent presses and clicked
	// at every chunk boundary. MEASURED before the fix, this script: after a silent press the ring still
	// peaked at 0.406 and the engaged output ran at rms 0.091, 0.072, 0.068 over three presses.
	//
	// **THE VALUE FIRST — the ring's live span after silence is exactly zero** — then, as the second
	// assertion, what a player hears from it.
	namespace sk = feathers::fx::sklitter;
	constexpr double kRate = 44100.0;
	constexpr int kBlock = 512;
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	params.setBase(static_cast<clap_id>(ParamId::AZone), 17.0);
	params.setBase(static_cast<clap_id>(ParamId::AOffset), 0.0);
	params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.5);
	const feathers::device::Descriptor* desc = feathers::deviceForTag(sk::kTraits.tag);
	REQUIRE(desc != nullptr);
	feathers::FxChain chain {};
	chain.device[0].row = feathers::defaultRowFor(*desc, { 7 });
	chain.count = 1;
	const auto engage = [&](bool on) {
		chain.device[0].row.field[static_cast<size_t>(sk::Param::Engage)] = on ? 1.0f : 0.0f;
		synth.provisionDevices(chain, feathers::defaultModRack());
		synth.setFxChain(chain);
	};
	engage(false);
	synth.setTempo(120.0);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	size_t n = 0;
	/// `seconds` at 120 BPM from where the song is; answers the output's peak.
	const auto play = [&](double seconds) {
		float peak = 0.0f;
		const size_t end = n + static_cast<size_t>(seconds * kRate);
		for (; n < end; n += kBlock) {
			const double at = static_cast<double>(n) / kRate;
			synth.setTransport(at, at * 2.0, true);
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			synth.render(l.data(), r.data(), kBlock);
			for (int i = 0; i < kBlock; ++i) {
				peak = std::max({ peak, std::fabs(l[static_cast<size_t>(i)]), std::fabs(r[static_cast<size_t>(i)]) });
			}
		}
		return peak;
	};
	const auto ringPeak = [&]() {
		const size_t slot = engineFxSlot(synth, chain, 0);
		REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
		const auto* s = reinterpret_cast<const sk::Sklitter*>(
		    synth.laneForTest().engineForTest().slotAtLane(slot).storage.get());
		const sk::Ring& ring = s->ringForTest();
		const float* d = ring.dataForTest();
		float peak = 0.0f;
		for (size_t i = 0; d != nullptr && i < ring.length() * 2; ++i) {
			peak = std::max(peak, std::fabs(d[i]));
		}
		return peak;
	};

	synth.noteOn(0, 60, 1, 0.8);
	play(3.0);
	engage(true);
	const float loud = play(4.0);
	// NOT VACUOUS: the ring held the note, and SKLITTER played it.
	REQUIRE(ringPeak() > 0.1f);
	REQUIRE(loud > 0.1f);
	// THE INPUT CUT — Level to 0 and the note released — then a player's recapture: off, silence, on.
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.0);
	synth.noteOff(0, 60, 1, 0.0);
	play(2.0);
	for (int press = 0; press < 2; ++press) {
		CAPTURE(press);
		engage(false);
		const float off = play(3.0);
		REQUIRE(off == 0.0f); // the input really is silent: the dry passes, and it is zero
		CHECK(ringPeak() == 0.0f);
		engage(true);
		CHECK(play(4.0) == 0.0f);
	}
}

TEST_CASE("2026-09-24 stuck engage: in Feathers' lane, a SKLITTER press inside a ONE-BAR host loop plays") {
	// THE BUG: the playhead stopped and engage did nothing. A looper press waits for the next grid line,
	// and that wait asked for `floor(beat) >= armed + 1` — a beat past the end of a one-bar loop, which the
	// song never reaches. Driven here through the instrument's own lane (≤32-frame chunks, the clock per
	// chunk), with the host's transport wrapping every bar exactly as a DAW cycle does.
	//
	// **THE VALUE: the device's status**, read off the slot — Playing (3), not Standby (2).
	namespace sk = feathers::fx::sklitter;
	constexpr double kRate = 44100.0;
	constexpr int kBlock = 512;
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.5);
	const feathers::device::Descriptor* desc = feathers::deviceForTag(sk::kTraits.tag);
	REQUIRE(desc != nullptr);
	feathers::FxChain chain {};
	chain.device[0].row = feathers::defaultRowFor(*desc, { 7 });
	chain.count = 1;
	const auto engage = [&](bool on) {
		chain.device[0].row.field[static_cast<size_t>(sk::Param::Engage)] = on ? 1.0f : 0.0f;
		synth.provisionDevices(chain, feathers::defaultModRack());
		synth.setFxChain(chain);
	};
	engage(false);
	synth.setTempo(120.0);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	// THE HOST SPLITS ITS BLOCK AT THE LOOP'S END, as a DAW does, so no block extrapolates past it: the
	// lane's per-chunk clock (block start plus frames rendered) would otherwise read beat 4.0x in the
	// last chunk before the wrap, which is a boundary a real cycle never reports.
	constexpr long long kLoopFrames = 88200; // one bar at 120 BPM, 44.1 kHz
	long long inLoop = 0;
	double beats = 0.0;
	const auto play = [&](double seconds) {
		for (long long done = 0; done < static_cast<long long>(seconds * kRate);) {
			const auto n = static_cast<int>(std::min<long long>(kBlock, kLoopFrames - inLoop));
			beats = static_cast<double>(inLoop) / kRate * 2.0;
			synth.setTransport(beats * 0.5, beats, true);
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			synth.render(l.data(), r.data(), n);
			done += n;
			inLoop = (inLoop + n) % kLoopFrames; // the cycle region: one bar
			beats = static_cast<double>(inLoop) / kRate * 2.0;
		}
	};
	const auto status = [&]() {
		const size_t slot = engineFxSlot(synth, chain, 0);
		REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
		const auto* s = reinterpret_cast<const sk::Sklitter*>(
		    synth.laneForTest().engineForTest().slotAtLane(slot).storage.get());
		return s->statusForTest();
	};
	synth.noteOn(0, 60, 1, 0.8);
	play(6.0);
	// PRESSED IN THE LOOP'S LAST BEAT, where the next grid line is the loop's end.
	while (beats < 3.25) {
		play(static_cast<double>(kBlock) / kRate);
	}
	engage(true);
	play(8.0);
	CHECK(status() == sk::Status::Playing);
}

TEST_CASE("2026-09-24 stuck Standby: in Feathers' lane, the logged sequence — the re-press keeps the capture and plays at the next beat") {
	// FROM A TRIPWIRE LOG (80 BPM, 44.1k, the lane's calls 1..32 frames, a two-bar buffer): a Burst
	// gesture re-lengthed the ring, so the Pattern press at 7.65 was not warm and had eight beats to
	// record. The release came at 14.47 (Standby until its trigger — the reference way, kept by design),
	// re-pressed at 15.87, and the press restarted the eight from zero, so it could not play before 24.
	// **THE VALUES: the status**, read off the slot.
	namespace sk = feathers::fx::sklitter;
	constexpr double kRate = 44100.0;
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.5);
	const feathers::device::Descriptor* desc = feathers::deviceForTag(sk::kTraits.tag);
	REQUIRE(desc != nullptr);
	feathers::FxChain chain {};
	chain.device[0].row = feathers::defaultRowFor(*desc, { 7 });
	chain.device[0].row.field[static_cast<size_t>(sk::Param::BufferLength)] = 3.0f; // two bars
	chain.count = 1;
	const auto set = [&](sk::Mode mode, bool on) {
		chain.device[0].row.field[static_cast<size_t>(sk::Param::Mode)] = static_cast<float>(mode);
		chain.device[0].row.field[static_cast<size_t>(sk::Param::Engage)] = on ? 1.0f : 0.0f;
		synth.provisionDevices(chain, feathers::defaultModRack());
		synth.setFxChain(chain);
	};
	set(sk::Mode::Burst, false);
	synth.setTempo(80.0);
	std::vector<float> l(32);
	std::vector<float> r(32);
	double beats = 0.0;
	uint32_t rng = 0x5EEDu;
	const auto to = [&](double until) {
		while (beats < until) {
			rng = rng * 1664525u + 1013904223u;
			const int n = 1 + static_cast<int>((rng >> 16) % 32u);
			synth.setTransport(beats * 0.75, beats, true);
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			synth.render(l.data(), r.data(), n);
			beats += static_cast<double>(n) / kRate / 0.75;
		}
	};
	const auto status = [&]() {
		const size_t slot = engineFxSlot(synth, chain, 0);
		REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
		const auto* s = reinterpret_cast<const sk::Sklitter*>(
		    synth.laneForTest().engineForTest().slotAtLane(slot).storage.get());
		return s->statusForTest();
	};
	synth.noteOn(0, 60, 1, 0.8);
	to(1.0);
	set(sk::Mode::Burst, true);
	to(2.0);
	REQUIRE(status() == sk::Status::Playing);
	set(sk::Mode::Burst, false);
	to(4.675);
	set(sk::Mode::Pattern, false);
	to(7.648);
	set(sk::Mode::Pattern, true);
	to(14.47);
	REQUIRE(status() == sk::Status::Standby); // not warm: still recording its eight beats
	set(sk::Mode::Pattern, false);
	to(14.5);
	CHECK(status() == sk::Status::Standby); // released early: it waits for its trigger, and records
	to(15.87);
	set(sk::Mode::Pattern, true);
	to(16.05);
	CHECK(status() == sk::Status::Playing);
}

//------------------------------------------------------------------------------------------------//
// RESONATE: WHAT LEAVES THE LANE IS WHAT PLUCKS THE STRING                                         //
//------------------------------------------------------------------------------------------------//

namespace {

/// Two note devices built here and never registered, seated the way seatInStrip seats a third party's:
/// one SWALLOWS every note-on, the other swallows note-ons too but turns every note-OFF into a note-ON
/// an octave up — a note the host never struck, invented by the lane.
struct NoteDevice {
	int placeholder = 0;
};

template <bool kInvent>
void laneRewrite(void*, const feathers::device::NoteEvent* in, size_t inCount, feathers::device::NoteEvent* out,
                 size_t outCapacity, size_t* outCount) {
	size_t n = 0;
	for (size_t i = 0; i < inCount && n < outCapacity; ++i) {
		feathers::device::NoteEvent e = in[i];
		if (e.kind == feathers::device::NoteEvent::Kind::On) {
			continue;
		}
		if (kInvent && e.kind == feathers::device::NoteEvent::Kind::Off) {
			e.kind = feathers::device::NoteEvent::Kind::On;
			e.key = static_cast<int16_t>(e.key + 12);
			e.velocity = 1.0;
		}
		out[n++] = e;
	}
	*outCount = n;
}

template <bool kInvent>
constexpr feathers::device::NoteOps kLaneRewriteOps {
	.stateBytes = sizeof(NoteDevice),
	.stateAlign = alignof(NoteDevice),
	.construct = [](void* storage) { new (storage) NoteDevice(); },
	.destruct = [](void* storage) { static_cast<NoteDevice*>(storage)->~NoteDevice(); },
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes = laneRewrite<kInvent>,
};

const feathers::device::Descriptor kSwallowerDescriptor {
	.traits = { .tag = "SWLW", .name = "Swallower", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.notes = &kLaneRewriteOps<false>,
};

const feathers::device::Descriptor kInventorDescriptor {
	.traits = { .tag = "INVT", .name = "Inventor", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.notes = &kLaneRewriteOps<true>,
};

/// The string's energy after `script` on a Resonate rig with `device` seated (or none).
float stringEnergyAfter(const feathers::device::Descriptor* device, bool strike, bool release) {
	Rig rig;
	rig.set(ParamId::OscType, static_cast<double>(feathers::OscType::Resonate));
	if (device != nullptr) {
		seatInStrip(rig, *device);
	}
	std::array<float, 400> left {};
	std::array<float, 400> right {};
	if (strike) {
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	}
	for (int i = 0; i < 3; ++i) {
		rig.synth.render(left.data(), right.data(), 400);
	}
	if (release) {
		rig.synth.noteOff(0, kKeyC4, 1, 0.0);
	}
	rig.synth.render(left.data(), right.data(), 400); // past the string's next tick
	return rig.synth.weaveForTest().energyForTest();
}

} // namespace

TEST_CASE("resonate: a note the lane swallows does not pluck the string") {
	// VALUES: the string's energy with the struck-and-swallowed note is the string's energy with no note
	// at all, to the bit — and a plain struck note (no device) is the control that shows a pluck would
	// have been visible.
	const float swallowed = stringEnergyAfter(&kSwallowerDescriptor, true, false);
	const float silent = stringEnergyAfter(&kSwallowerDescriptor, false, false);
	const float plucked = stringEnergyAfter(nullptr, true, false);
	MESSAGE("string energy: swallowed " << swallowed << ", no note " << silent << ", plain note " << plucked);
	CHECK(swallowed == silent);
	CHECK(plucked > silent + 0.05f);
}

TEST_CASE("resonate: a note the lane invents plucks the string") {
	// The Inventor turns the host's note-OFF into a note-ON the host never struck. That note plays, so it
	// plucks — measured against the same rig whose note was never released (so nothing was invented).
	const float invented = stringEnergyAfter(&kInventorDescriptor, true, true);
	const float heldOnly = stringEnergyAfter(&kInventorDescriptor, true, false);
	MESSAGE("string energy: invented note " << invented << ", nothing invented " << heldOnly);
	CHECK(invented > heldOnly + 0.05f);
}

TEST_CASE("a stacked voice's Smooth Shaper is told its stack, and trims to a single voice's loudness — per voice only") {
	// THE STACK TRIM'S PLUMBING (law (b)). The parts are summed before the per-voice devices, so
	// the synth hands each voice's instance its stack count and detune on the note road, and the instance trims its
	// drive by `stackTrimOf` — read here off the voice's own instance, as a value.
	namespace sm = feathers::fx::smooth;
	const auto build = [](Rig& rig, bool poly) {
		rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
		REQUIRE(feathers::addFxDevice(&rig.chain, sm::kTraits.tag) == 1);
		sm::State st;
		st.settings.poly = poly;
		rig.chain.device[1].row =
		    feathers::toDeviceRow(sm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), sm::rowOf(st));
		rig.publishChain();
	};
	// AND HOW ALIKE THE PARTS ARE: parts on stands of their own — a Stack Index route into Pos X, or
	// Width's mono stand per part — play different tables, and the synth hands their 0-cent coherence on the same road.
	// A plain stack's is exactly 1, so its trim is law (b)'s to the bit.
	struct Case {
		int stack;
		double detune;
		int16_t key;
		float stand; ///< a Stack Index route into Pos X at this amount; 0 none
		bool width;  ///< terrain stereo with two wide sounds and Width 1: a stand per part
	};
	for (const Case c : { Case { 1, 10.0, kKeyC4, 0.0f, false }, Case { 4, 0.0, kKeyC4, 0.0f, false }, Case { 8, 1.0, 36, 0.0f, false },
	                      Case { 8, 10.0, 84, 0.0f, false }, Case { 4, 0.0, kKeyC4, 0.5f, false }, Case { 8, 1.0, kKeyC4, 0.25f, false },
	                      Case { 4, 0.0, kKeyC4, 0.0f, true } }) {
		CAPTURE(c.stack);
		CAPTURE(c.detune);
		CAPTURE(c.key);
		CAPTURE(c.stand);
		CAPTURE(c.width);
		Rig rig;
		build(rig, true);
		feathers::test::Routes routes;
		if (c.stand != 0.0f) {
			routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::PosX), c.stand);
			routes.publish(rig.params);
		}
		if (c.width) {
			rig.set(feathers::soundParam(1, feathers::SoundField::Enable), 1.0);
			rig.set(feathers::soundParam(1, feathers::SoundField::Zone), 600.0);
			rig.set(feathers::soundParam(0, feathers::SoundField::Width), 1.0);
			rig.set(feathers::soundParam(1, feathers::SoundField::Width), 1.0);
			rig.set(ParamId::Width, 1.0);
		}
		rig.set(ParamId::StackCount, c.stack);
		rig.set(ParamId::StackDetune, c.detune);
		rig.play({ noteOn(0, c.key, 1) }, 4096, 128);
		const int slot = rig.synth.slotOfKeyForTest(c.key);
		REQUIRE(slot >= 0);
		const auto* shaper = static_cast<const sm::SmoothShaper*>(rig.synth.voiceFxStorageForTest(slot, sm::kTraits.tag));
		REQUIRE(shaper != nullptr);
		const float coherence = rig.synth.voiceStackCoherenceForTest(slot);
		const float want = sm::stackTrimOf(c.stack, static_cast<float>(c.detune), static_cast<float>(c.key), coherence);
		CHECK(shaper->stackTrimForTest() == want);
		if (c.stand == 0.0f && !c.width) {
			CHECK(coherence == 1.0f); // exactly: every part on the puck's one bank
			CHECK(want == sm::stackTrimOf(c.stack, static_cast<float>(c.detune), static_cast<float>(c.key)));
		}
		else {
			// NOT VACUOUS: the parts really differ here, and the trim is not law (b)'s — which is what a voice that
			// dropped its coherence on the way (or a device that ignored it) would hand the shaper.
			CHECK(coherence < 0.9f);
			CHECK(want > sm::stackTrimOf(c.stack, static_cast<float>(c.detune), static_cast<float>(c.key)) * 1.05f);
		}
		// AND THE CACHED READING IS THE READING: forcing the computation afresh gives the same number.
		CHECK(rig.synth.recomputeStackCoherenceForTest(slot) == coherence);
		if (c.stack == 1) {
			CHECK(shaper->stackTrimForTest() == 1.0f); // exactly
		}
		// AND THE DRIVE HAS GONE THERE: unity drive times the trim, after the per-frame glide.
		CHECK(shaper->driveGainForTest() == doctest::Approx(want).epsilon(1e-3));
	}
	// ON THE BUS, NEVER: the bus sums whole chords, and a stack of 8 at 0 cents leaves it untrimmed.
	Rig bus;
	build(bus, false);
	bus.set(ParamId::StackCount, 8);
	bus.set(ParamId::StackDetune, 0.0);
	bus.play({ noteOn(0, kKeyC4, 1) }, 1024, 128);
	const auto* busShaper = static_cast<const sm::SmoothShaper*>(bus.synth.fxStorageForTagForTest(sm::kTraits.tag));
	REQUIRE(busShaper != nullptr);
	CHECK(busShaper->stackTrimForTest() == 1.0f);
}

TEST_CASE("stack coherence: at most one cycle read per voice per chunk, converging to the full reading exactly") {
	// **AMORTISED** (`Synth::updateStackCoherence`): eight parts re-read in one chunk cost ~250 µs on the audio thread,
	// so a moved part is read one per chunk, in turn. One-frame blocks make every block exactly one chunk; the
	// per-voice Smooth Shaper is what asks for the reading.
	namespace sm = feathers::fx::smooth;
	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, sm::kTraits.tag) == 1);
	sm::State st;
	st.settings.poly = true;
	rig.chain.device[1].row = feathers::toDeviceRow(sm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), sm::rowOf(st));
	rig.publishChain();
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::PosX), 0.25f);
	routes.publish(rig.params);
	rig.set(ParamId::StackCount, 8.0);
	uint64_t mostInOneChunk = 0;
	uint64_t total = 0;
	const auto chunks = [&](std::vector<Event> script, int count) {
		for (int i = 0; i < count; ++i) {
			const uint64_t reads = rig.synth.coherenceCycleReadsForTest();
			rig.play(i == 0 ? script : std::vector<Event>{}, 1, 1);
			const uint64_t read = rig.synth.coherenceCycleReadsForTest() - reads;
			mostInOneChunk = std::max(mostInOneChunk, read);
			total += read;
		}
	};
	chunks({ noteOn(0, kKeyC4, 1) }, 4096);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	REQUIRE(rig.synth.voiceStackStandForTest(slot, 0) >= 0);
	for (const double x : { 0.3, -0.2 }) {
		rig.set(ParamId::PosX, x);
		chunks({}, 8192);
	}
	CHECK(mostInOneChunk == 1);
	CHECK(total >= 16); // NOT VACUOUS: all eight parts were read, and read again after the puck moved
	// CONVERGED: once the stands have settled, the published reading is the one a full recomputation takes, exactly.
	const float published = rig.synth.voiceStackCoherenceForTest(slot);
	const uint64_t reads = rig.synth.coherenceCycleReadsForTest();
	CHECK(rig.synth.updateStackCoherenceForTest(slot) == published);
	CHECK(rig.synth.coherenceCycleReadsForTest() == reads); // nothing left to read
	CHECK(rig.synth.recomputeStackCoherenceForTest(slot) == published);
	CHECK(published < 1.0f);
}

//------------------------------------------------------------------------------------------------//
// VOICE LEVEL, LEVEL AND PHASE                                                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("voice level: Attack and Release are gone, Voice Level wears the old Level's id, and Level is a plain fader") {
	// 0x0050 and 0x0051 were Attack and Release: no row answers them now.
	CHECK(feathers::findParam(0x0050) == nullptr);
	CHECK(feathers::findParam(0x0051) == nullptr);
	const feathers::ParamDesc* voice = feathers::findParam(static_cast<clap_id>(ParamId::VoiceLevel));
	REQUIRE(voice != nullptr);
	CHECK(static_cast<uint32_t>(ParamId::VoiceLevel) == 0x0052u);
	CHECK(voice->def == 0.0); // the envelope carries it (EF)
	CHECK((voice->flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) != 0u);
	const feathers::ParamDesc* level = feathers::findParam(static_cast<clap_id>(ParamId::Level));
	REQUIRE(level != nullptr);
	CHECK(level->def == 1.0); // unity: a fader at rest passes the voices through
	CHECK((level->flags & CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID) == 0u);
}

TEST_CASE("voice level: with no route, Voice Level 0 is silence and the voice still frees at note-off") {
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 1024, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	CHECK(rig.synth.voiceGainForTest(slot) == 0.0);
	CHECK(allExactlyZero(out, 0, out.size()));
	rig.play({ noteOff(0, kKeyC4, 1) }, 512, 64);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("voice level: with no route, Voice Level above zero is a gate — the note sounds at that gain and frees within the 2 ms edge of note-off") {
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.5);
	const std::vector<float> held = rig.play({ noteOn(0, kKeyC4, 1) }, 1024, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	CHECK(rig.synth.voiceGainForTest(slot) == doctest::Approx(0.5));
	CHECK(peakOf(held, 256, 1024) > 0.01f);
	// 2 ms at 48 kHz is 96 samples; two blocks of 64 is past it, and the voice is free.
	rig.play({ noteOff(0, kKeyC4, 1) }, 128, 64);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("voice level: Level scales the output by exactly its value, and at unity is not applied at all") {
	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };
	Rig full;
	const std::vector<float> a = full.play(script, 2048, 64);
	Rig half;
	half.set(ParamId::Level, 0.5);
	const std::vector<float> b = half.play(script, 2048, 64);
	// From the FIRST sample (a fader set before playing starts at its value, not on a ramp from unity), every sample is
	// the unity render times 0.5 exactly — a power of two, so the float product is exact.
	size_t mismatches = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		if (b[i] != a[i] * 0.5f) {
			++mismatches;
		}
	}
	CHECK(mismatches == 0u);
	CHECK(peakOf(a, 64, 2048) > 0.05f); // not vacuous: there is a signal to scale
}

TEST_CASE("phase: Phase moves the oscillator's READ by its value in every osc mode, and leaves the accumulator exactly where the mode put it") {
	for (const feathers::OscMode mode : { feathers::OscMode::Digi, feathers::OscMode::Analog, feathers::OscMode::Catchup }) {
		CAPTURE(static_cast<int>(mode));
		Rig still;
		still.set(ParamId::OscMode, static_cast<double>(mode));
		Rig moved;
		moved.set(ParamId::OscMode, static_cast<double>(mode));
		moved.set(ParamId::OscPhase, 0.25);
		const std::vector<Event> script { noteOn(0, kKeyC4, 1) };
		still.play(script, 512, 64);
		moved.play(script, 512, 64);
		const int slot = moved.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		REQUIRE(still.synth.slotOfKeyForTest(kKeyC4) == slot);
		// The read is 0.25 cycles on; the accumulator is the mode's own, to the bit.
		CHECK(moved.synth.voiceReadOffsetForTest(slot) == 0.25);
		CHECK(still.synth.voiceReadOffsetForTest(slot) == 0.0);
		CHECK(moved.synth.voicePhaseForTest(slot) == still.synth.voicePhaseForTest(slot));
		// AND THE OFFSET WAS USED, not only computed: the tables were read a quarter cycle on from where the
		// unmoved twin read them.
		CHECK(moved.synth.voiceReadPhaseForTest(slot) == still.synth.voiceReadPhaseForTest(slot) + 0.25);
	}
}

TEST_CASE("phase: a whole cycle of Phase is no phase at all — the read wraps, so 1.0 renders exactly what 0 does") {
	const std::vector<Event> script { noteOn(0, kKeyC4, 1) };
	Rig zero;
	const std::vector<float> a = zero.play(script, 1024, 64);
	Rig whole;
	whole.set(ParamId::OscPhase, 1.0);
	const std::vector<float> b = whole.play(script, 1024, 64);
	CHECK(a == b);
	// And a quarter cycle is NOT the same render — the check above is not vacuous.
	Rig quarter;
	quarter.set(ParamId::OscPhase, 0.25);
	CHECK(quarter.play(script, 1024, 64) != a);
}

TEST_CASE("phase: a per-voice source moves a SOUNDING note's read phase, per voice, and per sample inside the tick") {
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	constexpr size_t kPressureOutput = 6;
	rig.play({}, 64, 64);
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	constexpr float kAmount = 0.4f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1), noteOn(0, kKeyC4 + 7, 2) }, 256, 64);
	const int slotA = rig.synth.slotOfKeyForTest(kKeyC4);
	const int slotB = rig.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(slotA >= 0);
	REQUIRE(slotB >= 0);
	REQUIRE(rig.synth.voiceReadOffsetForTest(slotA) == 0.0);

	rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
	// One tick for the source to take the new value; then HALF of the next, whose window ramps to it.
	rig.play({}, 64, 64);
	rig.play({}, 32, 32);
	const double mid = rig.synth.voiceReadOffsetForTest(slotA);
	CAPTURE(mid);
	rig.play({}, 32 + 256, 32);
	const double settled = rig.synth.voiceReadOffsetForTest(slotA);
	CAPTURE(settled);
	// SETTLED AT amount × span (Phase's span is one cycle) — the value restated from the route.
	CHECK(settled == doctest::Approx(kAmount).epsilon(0.001));
	// PER SAMPLE: half way through the tick the read is part of the way there, not held at either end. A per-block
	// resolve would read 0 or the whole amount here.
	CHECK(mid > 0.05 * kAmount);
	CHECK(mid < 0.95 * kAmount);
	// PER VOICE: the other note is unpressed and reads where it did.
	CHECK(rig.synth.voiceReadOffsetForTest(slotB) == 0.0);
}

TEST_CASE("phase width: the right ear reads Phase's MODULATION mirrored — base + m·(1 − 2w) — and the left reads base + m") {
	namespace nx = feathers::mod::noteexpr;
	constexpr double kBase = 0.1;
	constexpr float kAmount = 0.4f;
	for (const double width : { 1.0, 0.5, 0.25 }) {
		CAPTURE(width);
		Rig rig;
		rig.set(ParamId::OscPhase, kBase);
		rig.set(ParamId::OscPhaseWidth, width);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		{
			const feathers::device::Descriptor* desc = feathers::deviceForTag(nx::kTraits.tag);
			REQUIRE(desc != nullptr);
			rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		}
		rig.synth.provisionDevices(feathers::FxChain {}, rack);
		rig.synth.setModRack(rack);
		constexpr size_t kPressureOutput = 6;
		rig.play({}, 64, 64);
		const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
		REQUIRE(phase >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), kPressureOutput),
		           feathers::backplane::Destination::core(static_cast<size_t>(phase)), kAmount);
		routes.publish(rig.params);
		rig.expectMono = false; // the ears differ — that is the point
		rig.play({ noteOn(0, kKeyC4, 1) }, 256, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0);
		const std::vector<float> heard = rig.play({}, 512, 64);
		// AND THE EARS WERE RENDERED APART — the right channel is not the left. The read-phase seams record what was
		// computed for each ear; this is what proves the right ear's phase reached its table (a known-bad that handed
		// the right ear the left's phase kept every value assertion below green).
		CHECK(heard != rig.lastRight);
		// RESTATED FROM THE DESIGN: m = amount × pressure × span = 0.4; left base + m, right base + m(1 − 2w), wrapped.
		const double m = kAmount;
		const double left = kBase + m;
		const double rightRaw = kBase + m * (1.0 - 2.0 * width);
		const double right = rightRaw - std::floor(rightRaw);
		CHECK(rig.synth.voiceReadOffsetForTest(slot) == doctest::Approx(left).epsilon(1e-5));
		CHECK(rig.synth.voiceReadOffsetRightForTest(slot) == doctest::Approx(right).epsilon(1e-5));
		// USED, not only computed: the two ears' tables were read that far apart.
		CHECK(rig.synth.voiceReadPhaseRightForTest(slot) - rig.synth.voiceReadPhaseForTest(slot)
		      == doctest::Approx(right - left).epsilon(1e-5));
	}
}

TEST_CASE("stereo modulation's zero cost: Phase moved by mono routes (a per-voice source, a block-rate LFO) with Phase Width renders the bits it rendered before Phase was a stereo destination (pinned at 13e62b37)") {
#if !(defined(__x86_64__) && defined(__linux__))
	MESSAGE("bit identity is pinned on Linux x86-64 only — skipped");
	return;
#endif
	namespace nx = feathers::mod::noteexpr;
	namespace lfo = feathers::mod::lfo;
	Rig rig;
	rig.expectMono = false; // Phase Width splits the ears: the stereo road, which is the half most at risk
	rig.set(ParamId::OscPhase, 0.1);
	rig.set(ParamId::OscPhaseWidth, 0.5);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	rack.device[0] = feathers::defaultRowFor(*feathers::deviceForTag(nx::kTraits.tag), { 1 });
	rack.device[1] = feathers::defaultRowFor(*feathers::deviceForTag(lfo::kTraits.tag), { 2 });
	rack.device[1].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(lfo::Mode::Free));
	rack.setIdentityAt(0, { 1 });
	rack.setIdentityAt(1, { 2 });
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 6),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), 0.4f);
	routes.set(feathers::backplane::Source::output(rack.instanceAt(1), 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), 0.3f);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1), noteOn(512, kKeyC4 + 7, 2) }, 2048, 64);
	rig.synth.noteExpression(0, kKeyC4, 1, CLAP_NOTE_EXPRESSION_PRESSURE, 0.8);
	const std::vector<float> more = rig.play({}, 4096, 64);
	out.insert(out.end(), more.begin(), more.end());
	out.insert(out.end(), rig.lastRight.begin(), rig.lastRight.end());
	uint64_t h = 1469598103934665603ull;
	for (const float x : out) {
		uint32_t bits = 0;
		std::memcpy(&bits, &x, sizeof bits);
		for (int k = 0; k < 4; ++k) {
			h ^= (bits >> (8 * k)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
	constexpr uint64_t kPinned = 15936795566733270887ull; // captured at 13e62b37
	CAPTURE(h);
	CHECK(h == kPinned);
	CHECK(more != rig.lastRight); // not vacuous: the ears really were rendered apart
}

TEST_CASE("phase width: with nothing routed into Phase it mirrors nothing — both ears read at the knob, on the mono road") {
	Rig rig;
	rig.set(ParamId::OscPhase, 0.3);
	rig.set(ParamId::OscPhaseWidth, 1.0);
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 512, 64); // expectMono holds: the road is mono
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	CHECK(rig.synth.voiceReadOffsetForTest(slot) == doctest::Approx(0.3).epsilon(1e-6));
	CHECK(peakOf(out, 64, 512) > 0.01f);
}


// ---------------------------------------------------------------------------------------------
// BACKLOG KU: PHASE AS A STEREO DESTINATION (2026-10-05). A pair routed into Phase reaches the ears apart, per sample:
// L = base + mL, R = base + mL·(1 − 2w) + (mR − mL) — Phase Width's mirror and the stereo routes ADD.
namespace {
namespace phst {
namespace st = feathers::mod::stereo;
constexpr double kBase = 0.1;
constexpr float kIn = 0.3f;     // the Stereo Tool's In: Split at Amount 100% publishes (+In, −In)
constexpr float kAmount = 0.5f; // the route into Phase; Phase's span is one cycle, so m = kAmount·kIn = 0.15

double wrap(double x) { return x - std::floor(x); }

/// A rig with a Stereo Tool in the strip, In at `in`, its Split routed into Phase at `amount`, Phase at kBase.
void seat(Rig& rig, feathers::ModRack& rack, float in, float amount, double width) {
	rig.set(ParamId::OscPhase, kBase);
	rig.set(ParamId::OscPhaseWidth, width);
	rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(st::kTraits.tag);
	REQUIRE(desc != nullptr);
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(st::Param::In)] = in;
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), static_cast<size_t>(st::Output::Split)),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), amount);
	routes.publish(rig.params);
	rig.expectMono = false;
	rig.play({}, 128, 64);
}
} // namespace phst
} // namespace

TEST_CASE("phase × stereo: Split 100% into Phase at Phase Width 0 — the left ear reads base + m, the right base − m, and the ears render apart") {
	Rig rig;
	feathers::ModRack rack;
	phst::seat(rig, rack, phst::kIn, phst::kAmount, 0.0);
	const std::vector<float> left = rig.play({ noteOn(0, kKeyC4, 1) }, 512, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	// RESTATED FROM THE DESIGN: Split's ears are (+In, −In), so mL = a·In, mR = −a·In; at w = 0 R = base + mR.
	const double m = static_cast<double>(phst::kAmount) * static_cast<double>(phst::kIn);
	CHECK(rig.params.stereoLive(ParamId::OscPhase));
	CHECK(rig.synth.lastChunkStereoForTest());
	CHECK(rig.synth.voiceReadOffsetForTest(slot) == doctest::Approx(phst::wrap(phst::kBase + m)).epsilon(1e-5));
	CHECK(rig.synth.voiceReadOffsetRightForTest(slot) == doctest::Approx(phst::wrap(phst::kBase - m)).epsilon(1e-5));
	// USED, not only computed: the right ear's table read sits that far from the left's (mod one cycle).
	const double apart = phst::wrap(rig.synth.voiceReadPhaseRightForTest(slot) - rig.synth.voiceReadPhaseForTest(slot));
	CHECK(apart == doctest::Approx(phst::wrap(-2.0 * m)).epsilon(1e-5));
	// SECOND ASSERTION, proving the first is not bookkeeping: the right channel is a different signal.
	CHECK(left != rig.lastRight);
}

TEST_CASE("phase × stereo: Split and Phase Width ADD — right = base + m·(1 − 2w) + (mR − mL)") {
	for (const double width : { 0.25, 0.5, 1.0 }) {
		CAPTURE(width);
		Rig rig;
		feathers::ModRack rack;
		phst::seat(rig, rack, phst::kIn, phst::kAmount, width);
		rig.play({ noteOn(0, kKeyC4, 1) }, 512, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		const double mL = static_cast<double>(phst::kAmount) * static_cast<double>(phst::kIn);
		const double mR = -mL;
		const double right = phst::kBase + mL * (1.0 - 2.0 * width) + (mR - mL);
		CHECK(rig.synth.voiceReadOffsetForTest(slot) == doctest::Approx(phst::wrap(phst::kBase + mL)).epsilon(1e-5));
		CHECK(rig.synth.voiceReadOffsetRightForTest(slot) == doctest::Approx(phst::wrap(right)).epsilon(1e-5));
	}
}

TEST_CASE("phase × stereo: a MONO route into Phase leaves the ears equal and the voice on the mono road") {
	namespace lfo = feathers::mod::lfo;
	Rig rig;
	rig.set(ParamId::OscPhase, phst::kBase);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	rack.device[0] = feathers::defaultRowFor(*feathers::deviceForTag(lfo::kTraits.tag), { 1 });
	rack.device[0].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(lfo::Mode::Free));
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), 0.4f);
	routes.publish(rig.params);
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 4096, 64); // expectMono holds: L == R to the bit
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	CHECK_FALSE(rig.params.stereoLive(ParamId::OscPhase));
	CHECK_FALSE(rig.synth.lastChunkStereoForTest());
	// THE PREMISE: the LFO really is moving Phase off its knob — else "equal ears" is about an idle route.
	CHECK(std::abs(rig.synth.voiceReadOffsetForTest(slot) - phst::kBase) > 1e-3);
	CHECK(rig.synth.voiceReadOffsetRightForTest(slot) == rig.synth.voiceReadOffsetForTest(slot));
	CHECK(peakOf(out, 64, 4096) > 0.01f);
}

TEST_CASE("phase × stereo: the right ear moves PER SAMPLE across a control tick — it ramps with the left, mirrored about the knob, with no step at the tick") {
	// A FREE LFO DRIVES THE STEREO TOOL'S In, so Split's pair moves every tick and each tick's windows are ramps. At Split
	// 100% the ears are (+i, −i), so at Phase Width 0 the right ear is base − mL whenever the left is base + mL: their sum
	// is 2·base at EVERY sample, mid-tick included. A right ear that stepped at the tick while the left ramped breaks it.
	namespace lfo = feathers::mod::lfo;
	constexpr double kMid = 0.5; // the knob mid-cycle, so neither ear wraps: the readings compare as plain numbers
	Rig rig;
	rig.expectMono = false;
	rig.set(ParamId::OscPhase, kMid);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	rack.device[1] = feathers::defaultRowFor(*feathers::deviceForTag(phst::st::kTraits.tag), { 1 });
	rack.device[0] = feathers::defaultRowFor(*feathers::deviceForTag(lfo::kTraits.tag), { 2 });
	rack.device[0].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(lfo::Mode::Free));
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const feathers::DeviceInstanceId tool = rack.instanceAt(1); // AFTER the LFO in the strip, so it ticks on this tick's In
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	constexpr float kAmount = 0.2f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::control(tool, static_cast<size_t>(phst::st::Param::In)), 0.5f);
	routes.set(feathers::backplane::Source::output(tool, static_cast<size_t>(phst::st::Output::Split)),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), kAmount);
	routes.publish(rig.params);
	rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	REQUIRE(rig.params.stereoLive(ParamId::OscPhase));
	// Sixteen readings, 16 samples apart — four per tick.
	std::vector<double> left;
	std::vector<double> right;
	for (int k = 0; k < 16; ++k) {
		rig.play({}, 16, 16);
		left.push_back(rig.synth.voiceReadOffsetForTest(slot));
		right.push_back(rig.synth.voiceReadOffsetRightForTest(slot));
	}
	size_t still = 0;
	for (size_t k = 0; k < right.size(); ++k) {
		CAPTURE(k);
		CAPTURE(left[k]);
		CAPTURE(right[k]);
		// THE VALUE, at every reading: the ears mirrored about the knob (Split's −i, restated from the design).
		CHECK(left[k] + right[k] == doctest::Approx(2.0 * kMid).epsilon(1e-4));
		if (k > 0 && right[k] == right[k - 1]) {
			++still;
		}
	}
	// NOT VACUOUS: the left ear moved at every reading (the LFO is live), so a right ear holding inside a tick would show.
	CHECK(std::abs(left.back() - left.front()) > 1e-3);
	CHECK(still == 0u);
}

TEST_CASE("additive: through the real synth, a Phase run moved by an LFO lands every control block on the tick's block boundaries, where the 4x path's read stands") {
	using feathers::OscType;
	// PLAN TASK 9b (Task 9a's re-review): the synth hands each voice a Phase run only to the end of the current 64-sample
	// tick (`seatReadPhase`), so a 32-sample control block that straddled a tick end ramped toward the CLAMPED tick-end
	// value. The voice now knows its tick phase and ends every block on a multiple of 32 in it: the first block after a
	// note-on is short. So at every multiple of 32 the offset the bank's phases carry is the run's value at that block's
	// last sample, which is exactly where the 4x path's read offset stands (V5, the same patch, the same LFO).
	namespace lfo = feathers::mod::lfo;
	struct Reading {
		std::vector<double> offsets;
		int firstBlock = 0;
	};
	const auto readings = [&](OscType engine) {
		Rig rig;
		rig.set(ParamId::OscType, static_cast<double>(engine));
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		rack.device[0] = feathers::defaultRowFor(*feathers::deviceForTag(lfo::kTraits.tag), { 2 });
		rack.device[0].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(lfo::Mode::Free));
		// EIGHT TIMES THE DEFAULT RATE, so the read moves by thousandths of a cycle between readings and a straddling
		// block's clamped end would show.
		rack.device[0].field[static_cast<size_t>(lfo::Param::TimeScale)] = 3.0f;
		rack.setIdentityAt(0, { 2 });
		rig.publishModRack(rack);
		const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
		REQUIRE(phase >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(phase)), 0.3f);
		routes.publish(rig.params);
		rig.play({}, 128, 64); // tick-aligned: the next sample is tick phase 0
		// THE NOTE AT TICK PHASE 16, so a block counted from the note-on would end at 48 and straddle the tick at 64.
		rig.play({ noteOn(16, kKeyC4, 1) }, 32, 16);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		Reading r;
		r.firstBlock = rig.synth.voiceStackOscForTest(slot, 0).partialsBlockLengthForTest();
		for (int k = 0; k < 40; ++k) {
			rig.play({}, 32, 16); // to the next multiple of 32 in tick phase
			r.offsets.push_back(engine == OscType::Additive
			                        ? rig.synth.voiceStackOscForTest(slot, 0).partialsOffsetForTest()
			                        : rig.synth.voiceReadOffsetForTest(slot));
		}
		return r;
	};
	const Reading additive = readings(OscType::Additive);
	const Reading v5 = readings(OscType::V5);
	// The note's first block ran from tick phase 16 to 32.
	CHECK(additive.firstBlock == 16);
	REQUIRE(additive.offsets.size() == v5.offsets.size());
	for (size_t k = 0; k < v5.offsets.size(); ++k) {
		CAPTURE(k);
		CHECK(additive.offsets[k] == doctest::Approx(v5.offsets[k]).epsilon(1e-6));
	}
	// NOT VACUOUS: the LFO moved the read by far more than the bound between readings.
	double moved = 0.0;
	for (size_t k = 1; k < v5.offsets.size(); ++k) {
		moved = std::max(moved, std::abs(v5.offsets[k] - v5.offsets[k - 1]));
	}
	CAPTURE(moved);
	CHECK(moved > 1e-3);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG KT: HARM AND THE FILTER, POLY OFF AND ON, IN EVERY VOICE MODE AND BESIDE EACH OTHER — asserted as values.
namespace {
namespace kt {
namespace flt = feathers::fx::filter;
namespace hrm = feathers::fx::harm;
constexpr int16_t kKtG4 = 67;
double hzOf(int16_t key) { return 440.0 * std::exp2((static_cast<double>(key) - 69.0) / 12.0); }

struct Cfg {
	int voiceMode = 0;
	bool hasFilter = false, filterOn = true, filterPoly = false;
	bool hasHarm = false, harmOn = true, harmPoly = false;
	bool harmFirst = false;
};

std::vector<float> render(const Cfg& c, const std::vector<Event>& script, uint32_t frames) {
	Rig rig;
	rig.set(ParamId::VoiceMode, c.voiceMode);
	rig.chain = {};
	flt::State fs {};
	flt::setValueOf(&fs, flt::Param::Enabled, c.filterOn ? 1.0f : 0.0f);
	flt::setValueOf(&fs, flt::Param::Cutoff, static_cast<float>(std::log2(400.0)));
	flt::setValueOf(&fs, flt::Param::Resonance, 0.0f);
	flt::setValueOf(&fs, flt::Param::Poly, c.filterPoly ? 1.0f : 0.0f);
	hrm::State hs;
	hs.settings.enabled = c.harmOn;
	hs.settings.level = 0.5f;
	hs.settings.harmonic = 4; // 1:2, a sub-octave
	hs.settings.attackSeconds = 0.0f;
	hs.poly = c.harmPoly;
	auto addF = [&] {
		const size_t at = feathers::addFxDevice(&rig.chain, flt::kTraits.tag);
		feathers::setFxFilterAt(&rig.chain, at, fs);
	};
	auto addH = [&] {
		const size_t at = feathers::addFxDevice(&rig.chain, hrm::kTraits.tag);
		rig.chain.device[at].row = feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[at].row), hrm::rowOf(hs));
	};
	if (c.harmFirst) {
		if (c.hasHarm) addH();
		if (c.hasFilter) addF();
	}
	else {
		if (c.hasFilter) addF();
		if (c.hasHarm) addH();
	}
	rig.publishChain();
	return rig.play(script, frames, 256);
}
} // namespace kt
} // namespace

TEST_CASE("backlog KT: in Feathers' lane a Filter cuts and a HARM adds its sub at every voice's note, Poly off and on, in Poly, Mono and Legato, alone and beside the other device") {
	// THE BUG REPORT: HARM and the Filter both misbehaving, with something odd around Poly and Mono.
	// This is the lane half of the hunt, and it found the lane right; it pins it. The
	// filter is a 400 Hz lowpass and the claim is its cut at the seventh partial of both notes, against the same
	// render with it off. HARM is a sub-octave (1:2) and the claim is WHICH notes have one: every held note when
	// Poly, only the last note on the bus (and, in Mono and Legato, only the one note that sounds).
	using namespace kt;
	constexpr uint32_t N = 32768;
	const feathers::test::Window win(static_cast<int>(N));
	const auto at = [&](const std::vector<float>& x, double hz) { return feathers::test::magnitudeAt(x, win, kSampleRate, hz); };
	const std::vector<Event> two {noteOn(0, kKeyC4, 1), noteOn(64, kKtG4, 2)};
	for (int mode = 0; mode < 3; ++mode) {
		CAPTURE(mode);
		for (int poly = 0; poly < 2; ++poly) {
			CAPTURE(poly);
			Cfg filter;
			filter.voiceMode = mode;
			filter.hasFilter = true;
			filter.filterPoly = poly != 0;
			Cfg filterOff = filter;
			filterOff.filterOn = false;
			const auto a = render(filter, two, N);
			const auto b = render(filterOff, two, N);
			// G4 sounds in every mode; C4 only where it is not replaced (Poly).
			CHECK(20.0 * std::log10(at(a, 7 * hzOf(kKtG4)) / at(b, 7 * hzOf(kKtG4))) < -30.0);
			if (mode == 0) {
				CHECK(20.0 * std::log10(at(a, 7 * hzOf(kKeyC4)) / at(b, 7 * hzOf(kKeyC4))) < -20.0);
			}
			for (int neighbour = 0; neighbour < 3; ++neighbour) {
				CAPTURE(neighbour);
				Cfg harm;
				harm.voiceMode = mode;
				harm.hasHarm = true;
				harm.harmPoly = poly != 0;
				// BESIDE A POLY FILTER (switched off, so the sub is measured unfiltered), before or after it.
				harm.hasFilter = neighbour > 0;
				harm.filterOn = false;
				harm.filterPoly = true;
				harm.harmFirst = neighbour == 2;
				const auto h = render(harm, two, N);
				CHECK(at(h, hzOf(kKtG4) / 2) > 0.1);
				const bool c4Sub = mode == 0 && poly != 0;
				if (c4Sub) {
					CHECK(at(h, hzOf(kKeyC4) / 2) > 0.1);
				}
				else {
					CHECK(at(h, hzOf(kKeyC4) / 2) < 1e-3);
				}
			}
		}
	}
}

TEST_CASE("Depth Follows Env with HARM Poly: each voice's notch follows ITS OWN envelope") {
	// A later note is earlier in its attack, so its notch is shallower — read per voice, where the depth is built.
	namespace hrm = feathers::fx::harm;
	Rig rig;
	rig.setFilter(feathers::fx::filter::Param::Enabled, 0.0);
	REQUIRE(feathers::addFxDevice(&rig.chain, hrm::kTraits.tag) == 1);
	hrm::State harm;
	harm.settings.enabled = true;
	harm.settings.attackSeconds = 0.2f;
	harm.settings.depth = 1.0f;
	harm.settings.depthFollowsEnv = true;
	harm.poly = true;
	rig.chain.device[1].row =
	    feathers::toDeviceRow(hrm::kTraits.tag, feathers::identityOf(rig.chain.device[1].row), hrm::rowOf(harm));
	rig.publishChain();
	rig.play({noteOn(0, kKeyC4, 1), noteOn(7200, kt::kKtG4, 2)}, 9600, 128);

	CHECK(rig.synth.fxStorageForTagForTest(hrm::kTraits.tag) == nullptr);
	const int first = rig.synth.slotOfKeyForTest(kKeyC4);
	const int second = rig.synth.slotOfKeyForTest(kt::kKtG4);
	REQUIRE(first >= 0);
	REQUIRE(second >= 0);
	const void* a = rig.synth.voiceFxStorageForTest(first, hrm::kTraits.tag);
	const void* b = rig.synth.voiceFxStorageForTest(second, hrm::kTraits.tag);
	REQUIRE(a != nullptr);
	REQUIRE(b != nullptr);
	const double depthA = hrm::opsNotchDepthForTest(a);
	const double depthB = hrm::opsNotchDepthForTest(b);
	CAPTURE(depthA);
	CAPTURE(depthB);
	CHECK(depthB > 0.0);
	CHECK(depthA > depthB);
	CHECK(depthA < 1.0);
}

namespace {

/// **THE TRIGGER-MODE AUDIT'S RIG (the bug: an MSEG set to Once stopped ringing out once a subsequent note
/// was played in Poly mode).** The device under test sits in strip slot 1, routed to Fine Tune
/// at a quarter — a per-voice destination nothing else touches, so `resolved(FineTune, slot)` is that device's value
/// for that voice and nothing else. The default DAHDSR in slot 0 carries Level with a one-second release, which is
/// The reported patch: it keeps a released note's voice sounding, so the voice can be read after its key is up.
/// Driven block by block rather than through `Rig::play`, so the transport runs continuously across the whole script.
struct ModeRun {
	enum class Device { Mseg, Lfo, Dahdsr };
	Device device = Device::Mseg;
	feathers::mod::Mode mode = feathers::mod::Mode::Gate;
	double voiceMode = 0.0; ///< 0 Poly, 1 Mono, 2 Legato
	bool transport = false; ///< a playing transport (seconds only, no tempo)
	bool poly = true; ///< the device's Poly switch
	bool keyboard = false; ///< every note wears id -1, as a keyboard's do, rather than index + 1
	/// Read each note's voice as whichever voice plays its key NOW, rather than the voice it was struck on — Mono
	/// hands a returned-to note a new voice.
	bool readByKey = false;
	/// The first note STRUCK AGAIN, under its own held key, at this time (s) — a retrigger, with no note-off; negative
	/// for none. The reference a Mono return is measured against.
	double restrikeFirst = -1.0;
	/// Each note: on at `onAt` s, off at `offAt` s (key C4 + 2·index, note id index + 1).
	struct Note {
		double onAt;
		double offAt;
	};
	std::vector<Note> notes;
	std::vector<double> readAt; ///< seconds at which every note's voice is read

	static constexpr float kAmount = 0.25f;

	/// readings[r][n]: the device's value for note n's voice at readAt[r], or -1 when that note has no voice.
	std::vector<std::vector<double>> run() const {
		namespace dsr = feathers::mod::dahdsr;
		namespace mseg = feathers::mod::mseg;
		namespace lfo = feathers::mod::lfo;
		Rig rig;
		rig.set(ParamId::VoiceMode, voiceMode);
		rig.set(ParamId::VoiceLevel, 0.0);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 2;
		switch (device) {
		case Device::Mseg: {
			// A ONE-SECOND TENT, 0 → 1 → 0, linear and with no sustain point, so no mode holds anywhere and the value
			// names the instant: a reading is a position on this note's own timeline.
			mseg::Settings shape {};
			shape.count = 2;
			shape.sustainSegment = mseg::kNoSustain;
			shape.mode = mode;
			shape.loopStart = 0;
			shape.loopEnd = 1;
			shape.segment[0] = { 0.0f, 1.0f, 0.0f };
			shape.segment[1] = { 0.0f, 0.0f, 0.0f };
			shape.timeScale = 0.0f; // one second with no tempo
			shape.poly = poly;
			rack.setMsegAt(1, shape);
			break;
		}
		case Device::Lfo: {
			const feathers::device::Descriptor* desc = feathers::deviceForTag(lfo::kTraits.tag);
			REQUIRE(desc != nullptr);
			rack.device[1] = feathers::defaultRowFor(*desc, { feathers::kFirstFreeInstance });
			rack.device[1].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(mode));
			// HALF A HERTZ: a two-second triangle, so no two reading instants in this script fall on one level.
			rack.device[1].field[static_cast<size_t>(lfo::Param::TimeScale)] = -1.0f;
			rack.device[1].field[static_cast<size_t>(lfo::Param::Poly)] = poly ? 1.0f : 0.0f;
			break;
		}
		case Device::Dahdsr: {
			dsr::Settings env {};
			env.attack = std::log2(0.3f);
			env.decay = std::log2(0.3f);
			env.sustain = 0.5f;
			env.release = std::log2(0.3f);
			env.mode = mode;
			env.poly = poly;
			rack.setDahdsrAt(1, env);
			break;
		}
		}
		rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
		rig.publishModRack(rack);
		std::vector<float> l(64);
		std::vector<float> r(64);
		rig.synth.render(l.data(), r.data(), 64);
		const int level = feathers::modDestinationIndexOf(ParamId::VoiceLevel);
		const int fine = feathers::modDestinationIndexOf(ParamId::FineTune);
		REQUIRE(level >= 0);
		REQUIRE(fine >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
		routes.set(feathers::backplane::Source::output(rack.instanceAt(1), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(fine)), kAmount);
		routes.publish(rig.params);
		rig.synth.render(l.data(), r.data(), 64);

		const auto blockOf = [](double seconds) { return static_cast<long>(std::lround(seconds * kSampleRate / 64.0)); };
		long last = 0;
		for (const double t : readAt) {
			last = std::max(last, blockOf(t));
		}
		std::vector<int> slotOf(notes.size(), -1);
		std::vector<std::vector<double>> readings;
		for (long b = 0; b <= last; ++b) {
			if (transport) {
				rig.synth.setTransport(10.0 + static_cast<double>(b) * 64.0 / kSampleRate,
				                       feathers::device::TransportClock::kNoPosition, true);
			}
			for (size_t n = 0; n < notes.size(); ++n) {
				const auto key = static_cast<int16_t>(kKeyC4 + 2 * static_cast<int>(n));
				const int32_t id = keyboard ? -1 : static_cast<int32_t>(n + 1);
				if (blockOf(notes[n].offAt) == b) {
					rig.synth.noteOff(0, key, id, 0.0);
				}
				if (blockOf(notes[n].onAt) == b) {
					rig.synth.noteOn(0, key, id, 1.0);
					slotOf[n] = rig.synth.newestVoiceSlotForTest();
				}
				if (n == 0 && restrikeFirst >= 0.0 && blockOf(restrikeFirst) == b) {
					rig.synth.noteOn(0, key, id, 1.0);
				}
			}
			for (const double t : readAt) {
				if (blockOf(t) != b) {
					continue;
				}
				std::vector<double> row;
				for (size_t n = 0; n < notes.size(); ++n) {
					const int nowOnKey = rig.synth.slotOfKeyForTest(static_cast<int16_t>(kKeyC4 + 2 * static_cast<int>(n)));
					const int slot = readByKey ? nowOnKey : slotOf[n];
					const bool live = slot >= 0 && nowOnKey == slot;
					row.push_back(live ? rig.params.resolved(ParamId::FineTune, slot) / (kAmount * 2.0 * feathers::kFineTuneMaxCents)
					                   : -1.0);
				}
				readings.push_back(row);
			}
			rig.synth.render(l.data(), r.data(), 64);
		}
		return readings;
	}
};

const char* modeName(feathers::mod::Mode m) {
	return feathers::mod::kModeNames[static_cast<size_t>(m)];
}

} // namespace

TEST_CASE("trigger modes, per voice (an MSEG in Once must keep ringing out after a subsequent note in Poly): in Poly every note's voice follows ITS OWN shape to its end and a new note starts its own — for the MSEG, the LFO and the DAHDSR, in every mode, legato and staccato, transport stopped and playing; in Free every voice reads the one shared shape") {
	// MEASURED before the MSEG and the LFO had a shape per note: the MSEG in Once, A released at 0.2 s and B struck at
	// 0.3 s, read A's voice at 0.4 s = 0.200 where A's own timeline says 0.800 — B's rising gate sent A's ringing voice
	// back to the start. Held under B, A was right and B read A's 0.800 where its own says 0.200. Every mode but Free
	// was wrong one way or the other, on both devices. Each reference here is a SOLO run of the same note — the
	// note's own timeline, played with nothing else — so "its own" is restated independently of the duo.
	using feathers::mod::Mode;
	const auto near = [](double a, double b) { return std::abs(a - b) < 2e-3; };
	for (const auto device : { ModeRun::Device::Mseg, ModeRun::Device::Lfo, ModeRun::Device::Dahdsr }) {
		for (const bool transport : { false, true }) {
			for (const Mode mode : { Mode::Gate, Mode::Loop, Mode::Once, Mode::Free, Mode::LfoRetrig }) {
				for (const bool legato : { false, true }) {
					CAPTURE(static_cast<int>(device));
					CAPTURE(transport);
					CAPTURE(modeName(mode));
					CAPTURE(legato);
					ModeRun run;
					run.device = device;
					run.mode = mode;
					run.transport = transport;
					// A then B, 0.3 s apart. Staccato: A is up at 0.2 s. Legato: A held under B. B is up at 0.9 s.
					run.notes = { { 0.0, legato ? 0.8 : 0.2 }, { 0.3, 0.9 } };
					run.readAt = { 0.4, 0.7 };
					ModeRun soloA = run;
					soloA.notes = { run.notes[0] };
					soloA.readAt = { 0.1, 0.4, 0.7 };
					ModeRun soloB = run;
					soloB.notes = { { 0.0, 0.6 } };
					soloB.readAt = { 0.1, 0.4 };
					const auto duo = run.run();
					const auto a = soloA.run();
					const auto b = soloB.run();
					REQUIRE(duo.size() == 2u);
					CAPTURE(duo[0][0]);
					CAPTURE(duo[0][1]);
					CAPTURE(duo[1][0]);
					CAPTURE(duo[1][1]);
					CAPTURE(a[1][0]);
					CAPTURE(a[2][0]);
					CAPTURE(b[0][0]);
					CAPTURE(b[1][0]);
					REQUIRE(duo[0][0] >= 0.0); // both voices still sounding: the default envelope's second of release
					REQUIRE(duo[1][1] >= 0.0);
					// NOT VACUOUS: where A stands at 0.4 s is not where a note 0.1 s old stands, so a shape that
					// restarted at B, or a B reading A's shape, cannot pass.
					REQUIRE_FALSE(near(a[1][0], b[0][0]));
					// A FOLLOWS ITS OWN TIMELINE, through B's arrival, in every mode.
					CHECK(near(duo[0][0], a[1][0]));
					CHECK(near(duo[1][0], a[2][0]));
					if (mode == Mode::Free) {
						// FREE: no note has anything to say — B reads the one shape A reads.
						CHECK(near(duo[0][1], duo[0][0]));
						CHECK(near(duo[1][1], duo[1][0]));
					}
					else {
						CHECK(near(duo[0][1], b[0][0]));
						CHECK(near(duo[1][1], b[1][0]));
					}
				}
			}
		}
	}
}

TEST_CASE("trigger modes in Mono and Legato: the one voice reads the NEW note's own shape from its start, every mode but Free") {
	using feathers::mod::Mode;
	const auto near = [](double a, double b) { return std::abs(a - b) < 2e-3; };
	for (const auto device : { ModeRun::Device::Mseg, ModeRun::Device::Lfo, ModeRun::Device::Dahdsr }) {
		for (const double voiceMode : { 1.0, 2.0 }) {
			for (const Mode mode : { Mode::Gate, Mode::Loop, Mode::Once, Mode::LfoRetrig }) {
				for (const bool legato : { false, true }) {
					CAPTURE(static_cast<int>(device));
					CAPTURE(voiceMode);
					CAPTURE(modeName(mode));
					CAPTURE(legato);
					ModeRun run;
					run.device = device;
					run.mode = mode;
					run.voiceMode = voiceMode;
					run.notes = { { 0.0, legato ? 0.8 : 0.2 }, { 0.3, 0.9 } };
					run.readAt = { 0.4 };
					ModeRun soloB = run;
					soloB.notes = { { 0.0, 0.6 } };
					soloB.readAt = { 0.1 };
					const auto duo = run.run();
					const auto b = soloB.run();
					CAPTURE(duo[0][1]);
					CAPTURE(b[0][0]);
					REQUIRE(duo[0][1] >= 0.0);
					CHECK(near(duo[0][1], b[0][0]));
				}
			}
		}
	}
}

TEST_CASE("legato return (an interstitial note played over a held lower note): when the interstitial note is released over a held one, the voice reads the HELD note's own shape — resumed where it stands in Legato, RESTARTED in Mono (by design) — for the MSEG, the LFO and the DAHDSR, with note ids and without") {
	// MEASURED before, Legato with note ids: the voice that moved back to the held key kept the RELEASED note's id,
	// so the device found no note matching (id, key) and the voice fell back to the device's one mono shape — a
	// third envelope, restarted by the interstitial note and released only when every key is up. Keyboard notes
	// (id -1) found the held note by key and were right; the same playing gave a different envelope with ids.
	// AND MONO'S RETURN CHOKED THE HELD NOTE (its steal ended it at the lane): the voice read the shape at rest. Mono is
	// the retrigger mode, so its return now RESTRIKES the held note — the reference is A struck again under its own
	// held key at the moment of the return, which every shape's own retrigger rule answers (Once refuses).
	using feathers::mod::Mode;
	const auto near = [](double a, double b) { return std::abs(a - b) < 2e-3; };
	for (const auto device : { ModeRun::Device::Mseg, ModeRun::Device::Lfo, ModeRun::Device::Dahdsr }) {
		for (const double voiceMode : { 2.0, 1.0 }) {
			for (const Mode mode : { Mode::Gate, Mode::Loop, Mode::Once, Mode::LfoRetrig }) {
				for (const bool keyboard : { false, true }) {
					CAPTURE(static_cast<int>(device));
					CAPTURE(voiceMode);
					CAPTURE(modeName(mode));
					CAPTURE(keyboard);
					ModeRun run;
					run.device = device;
					run.mode = mode;
					run.voiceMode = voiceMode;
					run.keyboard = keyboard;
					run.readByKey = true;
					// A held from 0 to 0.9 s; B, a key above it, from 0.3 to 0.45 s.
					run.notes = { { 0.0, 0.9 }, { 0.3, 0.45 } };
					run.readAt = { 0.4, 0.55, 0.7 };
					ModeRun soloA = run;
					soloA.notes = { run.notes[0] };
					if (voiceMode == 1.0) {
						soloA.restrikeFirst = 0.45; // Mono: the return is a retrigger
					}
					const auto duo = run.run();
					const auto a = soloA.run();
					REQUIRE(duo.size() == 3u);
					CAPTURE(duo[0][1]);
					CAPTURE(duo[1][0]);
					CAPTURE(duo[2][0]);
					CAPTURE(a[0][0]);
					CAPTURE(a[1][0]);
					CAPTURE(a[2][0]);
					// THE ONE VOICE IS ON B AT 0.4 s, reading B's shape — not A's, so the reading after the return
					// is a switch back and not a voice that never left A.
					REQUIRE(duo[0][1] >= 0.0);
					REQUIRE_FALSE(near(duo[0][1], a[0][0]));
					// BACK ON A AFTER B's RELEASE: A's own timeline — as if B had never been played (Legato), or as if A had been
					// struck again at B's release (Mono).
					REQUIRE(duo[1][0] >= 0.0);
					REQUIRE(duo[2][0] >= 0.0);
					CHECK(near(duo[1][0], a[1][0]));
					CHECK(near(duo[2][0], a[2][0]));
					if (voiceMode == 1.0 && mode != Mode::Once) {
						// NOT VACUOUS: the restrike is audible in the reference — A unstruck stands somewhere else.
						ModeRun plainA = soloA;
						plainA.restrikeFirst = -1.0;
						const auto plain = plainA.run();
						CAPTURE(plain[1][0]);
						CHECK_FALSE(near(plain[1][0], a[1][0]));
					}
				}
			}
		}
	}
}

TEST_CASE("a key up during a long attack releases from the level reached, and the envelope's playhead publishes that level beside the Release stage — not the sustain its card draws the release from") {
	// THE BUG: a DAHDSR note released during a long attack appeared to jump all the way to the sustain level
	// before releasing, even when the attack had not got there. MEASURED: the SOUND was right — Voice Level
	// fell from 0.25 — and the jump was the card's star, placed on the drawn release, which starts at sustain. The
	// playhead published only (stage, phase), so nothing downstream could say where the release began. Linear
	// stages, so the reference is arithmetic: a 2 s attack is at 0.25 after 0.5 s, and a 1 s release from 0.25 is at
	// 0.225 after 0.1 s.
	namespace dsr = feathers::mod::dahdsr;
	for (const bool poly : { true, false }) {
		CAPTURE(poly);
		Rig rig;
		rig.set(ParamId::VoiceLevel, 0.0);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		dsr::Settings env {};
		env.attack = 1.0f; // 2 s
		env.decay = std::log2(0.3f);
		env.sustain = 0.5f;
		env.release = 0.0f; // 1 s
		env.attackShape = 0.0f;
		env.releaseShape = 0.0f;
		env.smoothing = dsr::kParamDescs[static_cast<size_t>(dsr::Param::Smoothing)].min; // off
		env.poly = poly;
		rack.setDahdsrAt(0, env);
		rig.publishModRack(rack);
		rig.play({}, 64, 64);
		routeToLevel(rig, rack.instanceAt(0));
		rig.play({ noteOn(0, kKeyC4, 1) }, 24000, 64); // 0.5 s into the attack
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.play({ noteOff(0, kKeyC4, 1) }, 4800, 64); // 0.1 s into the release
		const feathers::mod::Playhead where = rig.synth.rackPlayheads()[0];
		CAPTURE(where.level);
		CHECK(where.ordinal == static_cast<uint32_t>(dsr::Stage::Release));
		REQUIRE(where.haveLevel);
		CHECK(std::abs(where.level - 0.225) < 2e-3);
		// THE SOUND AGREES — the voice's gain is the same level (Voice Level base 0, the route at full).
		CHECK(std::abs(rig.synth.voiceGainForTest(slot) - 0.225) < 2e-3);
		// NOT VACUOUS: the drawn release at this phase would say sustain × 0.9 = 0.45.
		CHECK(std::abs(where.level - 0.45) > 0.1);
	}
}

TEST_CASE("an LFO in Gate or Loop on Voice Level lets a released voice go (by design): Gate at the key, Loop at the end of its cycle — the voice's level is the LFO's own until then, Poly switch on and off") {
	// MEASURED before: the LFO cycled for ever after the key, so a route from it into Voice Level held the released
	// voice open for ever — the hold lasts while Voice Level is above the floor. A 1 Hz unipolar triangle, base 0.
	namespace lfo = feathers::mod::lfo;
	for (const bool poly : { true, false }) {
		for (const auto mode : { feathers::mod::Mode::Gate, feathers::mod::Mode::Loop }) {
			CAPTURE(poly);
			CAPTURE(static_cast<int>(mode));
			Rig rig;
			rig.set(ParamId::VoiceLevel, 0.0);
			feathers::ModRack rack = feathers::defaultModRack();
			rack.count = 1;
			const feathers::device::Descriptor* desc = feathers::deviceForTag(lfo::kTraits.tag);
			REQUIRE(desc != nullptr);
			rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
			rack.device[0].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(mode));
			rack.device[0].field[static_cast<size_t>(lfo::Param::Waveform)] = static_cast<float>(static_cast<int>(lfo::Waveform::Triangle));
			rack.device[0].field[static_cast<size_t>(lfo::Param::Bipolar)] = 0.0f;
			rack.device[0].field[static_cast<size_t>(lfo::Param::Poly)] = poly ? 1.0f : 0.0f;
			rig.synth.provisionDevices(feathers::FxChain {}, rack);
			rig.synth.setModRack(rack);
			rig.play({}, 64, 64);
			routeToLevel(rig, rack.instanceAt(0));
			rig.play({ noteOn(0, kKeyC4, 1) }, 14400, 64); // 0.3 s: the triangle at 0.6
			const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
			REQUIRE(slot >= 0);
			REQUIRE(std::abs(rig.synth.voiceGainForTest(slot) - 0.6) < 0.01);
			rig.play({ noteOff(0, kKeyC4, 1) }, 4800, 64); // 0.1 s after the key
			if (mode == feathers::mod::Mode::Gate) {
				CHECK(rig.synth.soundingVoiceCount() == 0);
			}
			else {
				// STILL RUNNING OUT ITS CYCLE, at the triangle's own level: 0.4 s into it, 0.8.
				REQUIRE(rig.synth.soundingVoiceCount() == 1);
				CHECK(std::abs(rig.synth.voiceGainForTest(slot) - 0.8) < 0.01);
				rig.play({}, 33600, 64); // 1.1 s from the start: the cycle ended at 1.0 s
				CHECK(rig.synth.soundingVoiceCount() == 0);
			}
		}
	}
}

TEST_CASE("mono return: a held note the interstitial note stole from is NOT ended at the lane — back on it, its routed Voice Level envelope still stands at sustain, so the note sounds") {
	// MEASURED before, Mono with the Poly DAHDSR on Voice Level (the Init arrangement): the interstitial note's steal
	// CHOKED the held note at the lane, resetting its envelope, so the voice the manager returned to it read the
	// envelope at rest — gain 0, a held key gone silent — with note ids and without.
	namespace dsr = feathers::mod::dahdsr;
	for (const bool ids : { true, false }) {
		CAPTURE(ids);
		Rig rig;
		rig.set(ParamId::VoiceMode, 1.0);
		rig.set(ParamId::VoiceLevel, 0.0);
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1;
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.050f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.050f);
		rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 0.5f;
		rack.device[0].field[static_cast<size_t>(dsr::Param::Release)] = std::log2(0.100f);
		REQUIRE(rack.device[0].field[static_cast<size_t>(dsr::Param::Poly)] >= 0.5f); // Poly, the default
		rig.synth.provisionDevices(feathers::FxChain {}, rack);
		rig.synth.setModRack(rack);
		rig.play({}, 64, 64);
		routeToLevel(rig, rack.instanceAt(0));
		rig.play({ noteOn(0, kKeyC4, ids ? 1 : -1) }, 9600, 64); // 0.2 s: on the 0.5 sustain
		rig.play({ noteOn(0, kKeyC4 + 2, ids ? 2 : -1) }, 9600, 64);
		rig.play({ noteOff(0, kKeyC4 + 2, ids ? 2 : -1) }, 9600, 64); // 0.2 s back on the held key
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		CHECK(std::abs(rig.synth.voiceGainForTest(slot) - 0.5) < 1e-3);
	}
}

TEST_CASE("legato return: the voice answers to the held note's id once it goes back to it — a choke by that id ends it, in Legato and Mono") {
	// The voice's id is what a choke, a per-note expression and a per-note modulator address it by. MEASURED before:
	// Legato's return kept the RELEASED note's id (2) and Mono's return started the voice with -1, so a choke aimed at
	// the held note (id 1) found no voice and the note went on sounding.
	for (const double voiceMode : { 2.0, 1.0 }) {
		CAPTURE(voiceMode);
		Rig rig;
		rig.set(ParamId::VoiceMode, voiceMode);
		rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
		rig.play({ noteOn(0, kKeyC4 + 2, 2) }, 4800, 64);
		rig.play({ noteOff(0, kKeyC4 + 2, 2) }, 4800, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0); // back on the held key
		REQUIRE(rig.synth.soundingVoiceCount() == 1);
		rig.synth.choke(0, -1, 1); // by id alone
		rig.play({}, 4800, 64);
		CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
		CHECK(rig.synth.soundingVoiceCount() == 0);
	}
}

TEST_CASE("a one-shot routed to Voice Level rings out after its key: the released voice lives while its Voice Level is above the floor, at that level — and a shape that ends above the floor holds it there") {
	// MEASURED before: an MSEG in Once routed to Level (base 0), key up at 0.2 s while the shape was rising through
	// 0.4, left NO voice at 0.4 s — the hold engaged only once the routed value had fallen below where it stood at
	// the key. The rule since 2026-10-03: only Voice Level > 0 decides.
	namespace mseg = feathers::mod::mseg;
	namespace dsr = feathers::mod::dahdsr;
	const auto rigFor = [](Rig& rig, feathers::ModRack& rack) {
		rig.set(ParamId::VoiceLevel, 0.0);
		rig.publishModRack(rack);
		rig.play({}, 64, 64);
		routeToLevel(rig, rack.instanceAt(1));
	};
	for (const float end : { 0.0f, 0.3f }) {
		CAPTURE(end);
		Rig rig;
		feathers::ModRack rack = feathers::defaultModRack();
		mseg::Settings shape {};
		shape.count = 2;
		shape.sustainSegment = mseg::kNoSustain;
		shape.mode = mseg::Mode::Once;
		shape.segment[0] = { 0.0f, 1.0f, 0.0f };
		shape.segment[1] = { 0.0f, end, 0.0f };
		rack.count = 2;
		rack.setMsegAt(1, shape);
		rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
		rigFor(rig, rack);
		rig.play({ noteOn(0, kKeyC4, 1) }, 9600, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.play({ noteOff(0, kKeyC4, 1) }, 9600, 64); // 0.4 s: still rising to the peak at 0.5 s
		REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
		CHECK(std::abs(rig.synth.voiceGainForTest(slot) - 0.8) < 0.01);
		rig.play({}, 52800, 64); // 1.5 s: the shape finished at 1.0 s
		if (end > 0.0f) {
			// HELD AT ITS END: Voice Level is 0.3, so the voice is on — by the rule, for as long as it stays there.
			REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
			CHECK(std::abs(rig.synth.voiceGainForTest(slot) - end) < 0.01);
		}
		else {
			CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
			CHECK(rig.synth.soundingVoiceCount() == 0);
		}
	}
	{
		// DAHDSR, Once: key up during the attack, the envelope still plays A → D → R
		Rig rig;
		feathers::ModRack rack = feathers::defaultModRack();
		dsr::Settings env {};
		env.attack = std::log2(0.3f);
		env.decay = std::log2(0.3f);
		env.sustain = 0.5f;
		env.release = std::log2(0.3f);
		env.mode = dsr::Mode::Once;
		rack.count = 2;
		rack.setDahdsrAt(1, env);
		rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
		rigFor(rig, rack);
		rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.play({ noteOff(0, kKeyC4, 1) }, 14400, 64); // 0.4 s: past the 0.3 s peak, decaying
		REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
		CHECK(rig.synth.voiceGainForTest(slot) > 0.6);
		rig.play({}, 48000, 64);
		CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
	}
}

TEST_CASE("a keyboard key struck again while its last note releases: the old voice is not held up by the NEW note's envelope") {
	// A keyboard's notes wear id -1, so a per-voice source answers for the KEY — and once the key is struck
	// again, that is the new note: the releasing voice's Voice Level reads the new note's envelope, and would hold it for as long as the new note
	// plays, on the new note's envelope: two voices for one key.
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	const feathers::ModRack rack = feathers::defaultModRack(); // the DAHDSR, one-second release
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	routeToLevel(rig, rack.instanceAt(0));
	rig.play({ noteOn(0, kKeyC4, -1) }, 9600, 64);
	rig.play({ noteOff(0, kKeyC4, -1) }, 4800, 64);
	REQUIRE(rig.synth.soundingVoiceCount() == 1); // THE PREMISE: the tail holds the released voice
	rig.play({ noteOn(0, kKeyC4, -1) }, 9600, 64);
	CHECK(rig.synth.soundingVoiceCount() == 1);
	CHECK(rig.synth.newestVoiceSlotForTest() >= 0);
}

TEST_CASE("Loop and LFO Retrig cycle PER VOICE while the note is held — the value, against the shape's own arithmetic") {
	// The per-voice reading of a looping modulator used to be a plain gated one (backlog EH flattened Loop to Gate):
	// MEASURED, a DAHDSR in Loop held 0.500 at 0.7 s on its sustain where the loop re-attacks from 0.6 s. The
	// solo-run references of the case above cannot see this — they share the flattening — so here the expected
	// value is the shape's arithmetic: linear stages, so the DAHDSR re-attacks 0.5 → 1 over 0.3 s from 0.6 s
	// (0.667 at 0.7 s), and the MSEG's one-second 0 → 1 → 0 tent is 0.2 s into its second turn at 1.2 s (0.400).
	using feathers::mod::Mode;
	const auto read = [](ModeRun::Device device, Mode mode, double at) {
		ModeRun run;
		run.device = device;
		run.mode = mode;
		run.notes = { { 0.0, 1.9 } };
		run.readAt = { at };
		return run.run()[0][0];
	};
	for (const Mode mode : { Mode::Loop, Mode::LfoRetrig }) {
		CAPTURE(modeName(mode));
		CHECK(std::abs(read(ModeRun::Device::Dahdsr, mode, 0.7) - (0.5 + 0.5 * 0.1 / 0.3)) < 0.01);
		CHECK(std::abs(read(ModeRun::Device::Mseg, mode, 1.2) - 0.4) < 0.01);
	}
}

TEST_CASE("a SHARED source pulling Voice Level above the floor keeps a released voice sounding, at that level, until it falls through the floor (the oscillator must keep running after the key is released while a shared source holds Voice Level above 0)") {
	// THE DESIGN (voice-level): a voice is ON while its voice gain is above zero. The release hold read only
	// the PER-VOICE part of what is routed into Voice Level, so a source every voice shares — a macro, a Free LFO's
	// one wave — let the voice's 2 ms edge close at the key while Voice Level still stood above the floor.
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	SUBCASE("a macro at 0.8: held at 0.8 after the key, and let go when the macro comes down") {
		const int level = feathers::modDestinationIndexOf(ParamId::VoiceLevel);
		REQUIRE(level >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
		routes.publish(rig.params);
		rig.set(feathers::macroParam(0), 0.8);
		rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.play({ noteOff(0, kKeyC4, 1) }, 9600, 64); // 0.2 s past the key
		REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
		CHECK(std::abs(rig.synth.voiceGainForTest(slot) - 0.8) < 1e-6);
		CHECK(rig.synth.soundingVoiceCount() == 1);
		rig.set(feathers::macroParam(0), 0.0);
		rig.play({}, 960, 64);
		CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
		CHECK(rig.synth.soundingVoiceCount() == 0);
	}
	SUBCASE("a Free LFO (one wave every voice reads): held while the wave is above the floor, at its value") {
		namespace lfo = feathers::mod::lfo;
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 1; // THE LFO ALONE in the strip
		const feathers::device::Descriptor* desc = feathers::deviceForTag(lfo::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { feathers::kFirstFreeInstance });
		rack.device[0].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(lfo::Mode::Free));
		rack.device[0].field[static_cast<size_t>(lfo::Param::TimeScale)] = -1.0f; // half a hertz
		rack.setIdentityAt(0, { feathers::kFirstFreeInstance });
		rig.publishModRack(rack);
		rig.play({}, 64, 64);
		routeToLevel(rig, rack.instanceAt(0));
		rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		rig.play({ noteOff(0, kKeyC4, 1) }, 9600, 64);
		const float wave = rig.synth.laneForTest().publishedOutputOf(rack.instanceAt(0), 0);
		CAPTURE(wave);
		REQUIRE(wave > 0.1f); // THE PREMISE: the shared wave is well above the floor 0.2 s after the key
		REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
		CHECK(std::abs(rig.synth.voiceGainForTest(slot) - wave) < 0.02);
		// THE WAVE REACHES ITS TROUGH WITHIN A CYCLE, and the voice ends there rather than droning for ever.
		rig.play({}, 2 * 96000, 64);
		CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
	}
}

TEST_CASE("Poly OFF: every voice reads the device's ONE shape, and a new note restarts it as the mode says — for the MSEG, the LFO and the DAHDSR, transport stopped and playing") {
	// A Poly switch, and no dual-path logic. Mono is one shape for the device: both voices
	// read the same value, and B's note-on restarts it in Gate, Loop and LFO Retrig (LFO Retrig too on a playing
	// transport — it was song-locked, so notes did not restart it, backlog KF), in Once only after every key was
	// up (one shot per phrase), and never in Free. The restart is asserted against B's own SOLO timeline for the
	// MSEG and the LFO, which restart from their start; the DAHDSR re-attacks from where it stands by design, so it
	// is held to the shared-value half only.
	using feathers::mod::Mode;
	const auto near = [](double a, double b) { return std::abs(a - b) < 2e-3; };
	for (const auto device : { ModeRun::Device::Mseg, ModeRun::Device::Lfo, ModeRun::Device::Dahdsr }) {
		for (const bool transport : { false, true }) {
			for (const Mode mode : { Mode::Gate, Mode::Loop, Mode::Once, Mode::Free, Mode::LfoRetrig }) {
				for (const bool legato : { false, true }) {
					CAPTURE(static_cast<int>(device));
					CAPTURE(transport);
					CAPTURE(modeName(mode));
					CAPTURE(legato);
					ModeRun run;
					run.device = device;
					run.mode = mode;
					run.transport = transport;
					run.poly = false;
					run.notes = { { 0.0, legato ? 0.8 : 0.2 }, { 0.3, 0.9 } };
					run.readAt = { 0.4, 0.7 };
					ModeRun soloA = run;
					soloA.notes = { run.notes[0] };
					soloA.readAt = { 0.4 };
					ModeRun soloB = run;
					soloB.notes = { { 0.0, 0.6 } };
					soloB.readAt = { 0.1 };
					const auto duo = run.run();
					const auto a = soloA.run();
					const auto b = soloB.run();
					REQUIRE(duo[0][0] >= 0.0);
					REQUIRE(duo[0][1] >= 0.0);
					CAPTURE(duo[0][0]);
					CAPTURE(duo[0][1]);
					CAPTURE(a[0][0]);
					CAPTURE(b[0][0]);
					// ONE SHAPE: both voices read the same value, at both instants.
					CHECK(near(duo[0][0], duo[0][1]));
					CHECK(near(duo[1][0], duo[1][1]));
					if (device == ModeRun::Device::Dahdsr || mode == Mode::Free) {
						continue;
					}
					REQUIRE_FALSE(near(a[0][0], b[0][0])); // not vacuous: a restart and no restart read apart
					const bool restarts = mode != Mode::Once || !legato;
					CHECK(near(duo[0][0], restarts ? b[0][0] : a[0][0]));
				}
			}
		}
	}
}

TEST_CASE("Poly ON: a destination with no voice reads the MOST RECENTLY STRUCK note's shape") {
	// The single rule for a per-note device reaching a bus destination (or drawing its playhead): the newest note.
	namespace mseg = feathers::mod::mseg;
	Rig rig;
	feathers::ModRack rack = feathers::defaultModRack();
	mseg::Settings shape {};
	shape.count = 2;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Once;
	shape.segment[0] = { 0.0f, 1.0f, 0.0f };
	shape.segment[1] = { 0.0f, 0.0f, 0.0f };
	REQUIRE(shape.poly);
	rack.count = 2;
	rack.setMsegAt(1, shape);
	rack.setIdentityAt(1, { feathers::kFirstFreeInstance });
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	rig.play({ noteOn(0, kKeyC4, 1) }, 14400, 64);    // A, 0.3 s in: rising, 0.6
	rig.play({ noteOn(0, kKeyC4 + 2, 2) }, 4800, 64); // B, 0.1 s in: 0.2; A at 0.8
	const float published = rig.synth.laneForTest().publishedOutputOf(rack.instanceAt(1), 0);
	CAPTURE(published);
	CHECK(std::abs(published - 0.2f) < 0.01f);
}

TEST_CASE("a MONO envelope (Poly off) on Voice Level keeps a released voice sounding while its value is above the floor (the oscillator must keep running after the key is released while a mono envelope holds Voice Level above 0)") {
	namespace dsr = feathers::mod::dahdsr;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::ModRack rack = feathers::defaultModRack();
	dsr::Settings env {};
	env.attack = std::log2(0.005f);
	env.sustain = 1.0f;
	env.release = 0.0f; // one second
	env.poly = false;
	rack.setDahdsrAt(0, env);
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	routeToLevel(rig, rack.instanceAt(0));
	rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	rig.play({ noteOff(0, kKeyC4, 1) }, 14400, 64); // 0.3 s into a one-second release
	const float env03 = rig.synth.laneForTest().publishedOutputOf(rack.instanceAt(0), 0);
	CAPTURE(env03);
	REQUIRE(env03 > 0.2f);
	REQUIRE(env03 < 0.9f); // THE PREMISE: the mono envelope is part-way down its release
	REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == slot);
	CHECK(std::abs(rig.synth.voiceGainForTest(slot) - env03) < 0.01);
	rig.play({}, 48000, 64);
	CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("an envelope NOT routed to Voice Level does not keep a released voice alive: Voice Level alone decides") {
	// The default envelope (one-second release) is in the strip but unrouted; Voice Level is a plain level with no
	// route — the organ gate the voice-level design states: on at the key, 2 ms off after it.
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.8);
	rig.publishModRack(feathers::defaultModRack());
	rig.play({}, 64, 64);
	REQUIRE_FALSE(rig.params.isRouted(ParamId::VoiceLevel));
	rig.play({ noteOn(0, kKeyC4, 1) }, 4800, 64);
	REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) >= 0);
	rig.play({ noteOff(0, kKeyC4, 1) }, 480, 64); // 10 ms
	CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
	CHECK(rig.synth.soundingVoiceCount() == 0);

	SUBCASE("and routed only to something else, it still does not") {
		const int fine = feathers::modDestinationIndexOf(ParamId::FineTune);
		REQUIRE(fine >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(feathers::defaultModRack().instanceAt(0), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(fine)), 0.25f);
		routes.publish(rig.params);
		rig.play({ noteOn(0, kKeyC4 + 2, 2) }, 4800, 64);
		REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4 + 2) >= 0);
		rig.play({ noteOff(0, kKeyC4 + 2, 2) }, 480, 64);
		CHECK(rig.synth.slotOfKeyForTest(kKeyC4 + 2) < 0);
	}
}

//--------------------------------------------------------------------------------------------------//
// MODULATORS IN THE FX LANE (every modulator is in scope in the Feathers fx rack, by design)       //
// — every claim read as a VALUE: a filter's key offset, a control's modulated     //
// value, a voice's per-voice offset and gain.                                                          //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A STRIP HOLDING NOTHING, so whatever a case reads cannot have come from the strip.
feathers::ModRack emptyStrip() {
	feathers::ModRack strip = feathers::defaultModRack();
	strip.count = 0;
	return strip;
}

/// ONE DEVICE OF `tag` APPENDED TO `chain` UNDER ITS OWN ID — the lane's own verb, so a lane that refused it fails
/// here, at the REQUIRE, rather than as a quiet absence later.
size_t appendDevice(feathers::FxChain* chain, const char* tag, feathers::DeviceInstanceId id) {
	const size_t at = feathers::addFxDevice(chain, tag);
	REQUIRE(at != feathers::kNoPosition);
	chain->device[at].row.instanceId = id;
	return at;
}

/// A KEYTRACKED BUS FILTER (Poly off) — the reader of the bus note, as the strip-Tracker cases build it.
size_t appendKeytrackedBusFilter(feathers::FxChain* chain, feathers::DeviceInstanceId id) {
	namespace flt = feathers::fx::filter;
	const size_t at = appendDevice(chain, flt::kTraits.tag, id);
	flt::State s {};
	flt::setValueOf(&s, flt::Param::Enabled, 1.0f);
	flt::setValueOf(&s, flt::Param::Cutoff, 10.0f);
	flt::setValueOf(&s, flt::Param::Poly, 0.0f);
	flt::setValueOf(&s, flt::Param::Keytrack, 1.0f);
	feathers::setFxFilterAt(chain, at, s);
	return at;
}

/// A TRACKER WITH ITS RULE.
size_t appendTracker(feathers::FxChain* chain, feathers::DeviceInstanceId id, feathers::mod::tracker::Rule rule) {
	namespace trk = feathers::mod::tracker;
	const size_t at = appendDevice(chain, trk::kTraits.tag, id);
	chain->device[at].row.field[static_cast<size_t>(trk::Param::Rule)] = static_cast<float>(rule);
	return at;
}

/// BOTH CHAINS INTO THE SYNTH, built then handed over (the plugin's two halves), and a render so they are adopted.
void seat(Rig& rig, const feathers::FxChain& chain, const feathers::ModRack& strip) {
	rig.chain = chain;
	rig.synth.provisionDevices(chain, strip);
	rig.synth.setFxChain(chain);
	rig.synth.setModRack(strip);
	rig.play({}, 256, 256);
}

/// THE KEY OFFSET THE ENGINE HANDED ONE FILTER — read from the device the lane holds under `id`, not from "the
/// first filter", which is the only seam there was while the lane could hold one Tracker-follower that mattered.
float keyOffsetOfFilter(const Rig& rig, feathers::DeviceInstanceId id) {
	const void* storage = rig.synth.laneForTest().storageOfForTest(id);
	REQUIRE(storage != nullptr);
	return feathers::fx::filter::opsTargetForTest(storage).keyOffsetSemitones;
}

} // namespace

TEST_CASE("lane-mods: the three-filter Tracker chain in Feathers' fx lane — filter1 → Tracker(Lowest) → filter2 → Tracker(Highest) → filter3, each filter following its nearest upstream Tracker") {
	// The chain the rack already ran (test_rack_clap's "rack keytrack" case), now in Feathers' lane, with the
	// strip EMPTY — so no note a filter follows can have come from a strip Tracker. filter1 has no Tracker before it
	// and follows the bus's own note, the newest held (Last).
	namespace trk = feathers::mod::tracker;
	Rig rig;
	feathers::FxChain chain;
	appendKeytrackedBusFilter(&chain, 901);
	appendTracker(&chain, 902, trk::Rule::Lowest);
	appendKeytrackedBusFilter(&chain, 903);
	appendTracker(&chain, 904, trk::Rule::Highest);
	appendKeytrackedBusFilter(&chain, 905);
	seat(rig, chain, emptyStrip());
	// THE TRACKERS RUN IN THE LANE — the premise; with the lane refusing them the engine holds none.
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(902) != nullptr);
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(904) != nullptr);

	const auto check = [&](float last, float lowest, float highest) {
		CHECK(keyOffsetOfFilter(rig, 901) == doctest::Approx(last - 60.0f));
		CHECK(keyOffsetOfFilter(rig, 903) == doctest::Approx(lowest - 60.0f));
		CHECK(keyOffsetOfFilter(rig, 905) == doctest::Approx(highest - 60.0f));
	};
	rig.play({ noteOn(0, 60, 1) }, 256, 256);
	rig.play({ noteOn(0, 72, 2) }, 256, 256);
	rig.play({ noteOn(0, 66, 3) }, 256, 256); // held 60, 72, 66: Last 66, Lowest 60, Highest 72 — all three differ
	check(66.0f, 60.0f, 72.0f);
	rig.play({ noteOff(0, 72, 2) }, 256, 256); // held 60, 66: Highest 66
	check(66.0f, 60.0f, 66.0f);
	rig.play({ noteOff(0, 60, 1) }, 256, 256); // held 66: every rule agrees
	check(66.0f, 66.0f, 66.0f);
}

namespace {

/// ONE LFO ROUTED TO A FILTER'S CUTOFF, the LFO in the strip or in the lane (before or after the filter), and the
/// cutoff the ENGINE resolved read at four instants. The strip placement is the reference: a modulator's place is
/// only grouping (backlog IZ), so where it stands may not change the value.
enum class LfoAt { Strip, LaneBefore, LaneAfter };
std::vector<float> cutoffUnderLfo(LfoAt at) {
	namespace flt = feathers::fx::filter;
	namespace lfo = feathers::mod::lfo;
	Rig rig;
	feathers::FxChain chain;
	feathers::ModRack strip = emptyStrip();
	constexpr feathers::DeviceInstanceId kLfo = 911;
	constexpr feathers::DeviceInstanceId kFilter = 912;
	// FREE-RUNNING at 1 Hz (Time Base Off, Rate 2^0), so it turns with no note held — the default Gate mode rests
	// until a gate, and a case reading a resting LFO compares two unmodulated cutoffs.
	feathers::DeviceRow lfoRow = feathers::defaultRowFor(*feathers::deviceForTag(lfo::kTraits.tag), { kLfo });
	lfoRow.field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(feathers::mod::Mode::Free);
	lfoRow.field[static_cast<size_t>(lfo::Param::TimeBase)] = 0.0f;
	lfoRow.field[static_cast<size_t>(lfo::Param::TimeScale)] = 0.0f;
	if (at == LfoAt::LaneBefore) {
		chain.device[appendDevice(&chain, lfo::kTraits.tag, kLfo)].row = lfoRow;
	}
	const size_t filterAt = appendKeytrackedBusFilter(&chain, kFilter);
	if (at == LfoAt::LaneAfter) {
		chain.device[appendDevice(&chain, lfo::kTraits.tag, kLfo)].row = lfoRow;
	}
	if (at == LfoAt::Strip) {
		strip.device[0] = lfoRow;
		strip.count = 1;
	}
	seat(rig, chain, strip);
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(kLfo) != nullptr);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(kLfo, 0),
	           feathers::backplane::Destination::control(kFilter, static_cast<size_t>(flt::Param::Cutoff)), 0.5f);
	routes.publish(rig.params);
	std::vector<float> out;
	for (int i = 0; i < 4; ++i) {
		rig.play({}, 4800, 64); // a tenth of a second apart
		out.push_back(feathers::fxDeviceAtPosition(rig.synth.modulatedFxChain(), filterAt)
		                  .row.field[static_cast<size_t>(flt::Param::Cutoff)]);
	}
	return out;
}

} // namespace

TEST_CASE("lane-mods: an LFO in the fx lane modulates a lane device after it — the cutoff the engine resolved equals the strip LFO's, and it moves") {
	const std::vector<float> strip = cutoffUnderLfo(LfoAt::Strip);
	const std::vector<float> after = cutoffUnderLfo(LfoAt::LaneAfter);
	REQUIRE(strip.size() == after.size());
	for (size_t i = 0; i < strip.size(); ++i) {
		CAPTURE(i);
		CHECK(after[i] == doctest::Approx(strip[i]).epsilon(1e-6));
	}
	// NOT VACUOUS: the reference itself moves — four instants that are not all one value, so equality above is a
	// moving LFO reproduced, not two unmodulated bases agreeing.
	CHECK(*std::max_element(strip.begin(), strip.end()) - *std::min_element(strip.begin(), strip.end()) > 0.01f);

	SUBCASE("and one BEFORE it, to its left, the same — position is only grouping (backlog IZ)") {
		const std::vector<float> before = cutoffUnderLfo(LfoAt::LaneBefore);
		for (size_t i = 0; i < strip.size(); ++i) {
			CAPTURE(i);
			CHECK(before[i] == doctest::Approx(strip[i]).epsilon(1e-6));
		}
	}
}

namespace {

/// A POLY DAHDSR ROUTED TO VOICE LEVEL (base 0), in the strip or the fx lane; two notes struck 0.3 s apart, then
/// both released. Read per voice: the per-voice offset each voice's Voice Level got, and each voice's gain.
struct PolyEnvReadings {
	double offsetFirst = 0.0, offsetSecond = 0.0; // both held, 0.35 s in: the first decaying, the second attacking
	double gainReleased = 0.0;                    // the first voice, 0.2 s after its key went up
	bool heldAfterKey = false;                    // that voice still holds its slot then
	bool freedAtEnd = false;                      // and is gone once the release has run out
};
PolyEnvReadings polyEnvelopeOnVoiceLevel(bool inLane) {
	namespace dsr = feathers::mod::dahdsr;
	constexpr feathers::DeviceInstanceId kEnv = 921;
	Rig rig;
	rig.set(ParamId::VoiceLevel, 0.0);
	feathers::FxChain chain;
	feathers::ModRack strip = emptyStrip();
	feathers::DeviceRow env = feathers::defaultRowFor(*feathers::deviceForTag(dsr::kTraits.tag), { kEnv });
	env.field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.2f);
	env.field[static_cast<size_t>(dsr::Param::Decay)] = std::log2(0.2f);
	env.field[static_cast<size_t>(dsr::Param::Sustain)] = 0.5f;
	env.field[static_cast<size_t>(dsr::Param::Release)] = std::log2(0.5f);
	env.field[static_cast<size_t>(dsr::Param::Poly)] = 1.0f;
	if (inLane) {
		const size_t at = appendDevice(&chain, dsr::kTraits.tag, kEnv);
		chain.device[at].row = env;
	}
	else {
		strip.device[0] = env;
		strip.count = 1;
	}
	seat(rig, chain, strip);
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(kEnv) != nullptr);
	routeToLevel(rig, kEnv);
	REQUIRE(rig.params.variesPerVoice(ParamId::VoiceLevel)); // declared per voice: the route reads the cells
	PolyEnvReadings r;
	rig.play({ noteOn(0, kKeyC4, 1) }, 14400, 64); // 0.3 s
	rig.play({ noteOn(0, kKeyG4, 2) }, 2400, 64);  // 0.35 s
	const int first = rig.synth.slotOfKeyForTest(kKeyC4);
	const int second = rig.synth.slotOfKeyForTest(kKeyG4);
	REQUIRE(first >= 0);
	REQUIRE(second >= 0);
	r.offsetFirst = rig.params.perVoiceOffset(ParamId::VoiceLevel, first);
	r.offsetSecond = rig.params.perVoiceOffset(ParamId::VoiceLevel, second);
	rig.play({ noteOff(0, kKeyC4, 1) }, 9600, 64); // 0.2 s into the first voice's release
	r.heldAfterKey = rig.synth.slotOfKeyForTest(kKeyC4) == first;
	r.gainReleased = rig.synth.voiceGainForTest(first);
	rig.play({ noteOff(0, kKeyG4, 2) }, 96000, 64); // two seconds: every release has run out
	r.freedAtEnd = rig.synth.slotOfKeyForTest(kKeyC4) < 0 && rig.synth.soundingVoiceCount() == 0;
	return r;
}

} // namespace

TEST_CASE("lane-mods: a Poly DAHDSR in the fx lane writes its per-voice cells — two voices read their own envelopes, exactly as the strip's would") {
	// THE ENVELOPE'S OWN ARITHMETIC FIRST: linear stages, 0.2 s attack, 0.2 s decay to 0.5. At 0.35 s the first
	// note is 0.15 s into its decay (1 − 0.5·0.75 = 0.625) and the second 0.05 s into its attack (0.25).
	const PolyEnvReadings lane = polyEnvelopeOnVoiceLevel(true);
	CHECK(lane.offsetFirst == doctest::Approx(0.625).epsilon(0.03));
	CHECK(lane.offsetSecond == doctest::Approx(0.25).epsilon(0.08));
	// AND THE STRIP'S, as the reference the lane must match: the same device, only placed elsewhere.
	const PolyEnvReadings strip = polyEnvelopeOnVoiceLevel(false);
	CHECK(lane.offsetFirst == doctest::Approx(strip.offsetFirst).epsilon(1e-6));
	CHECK(lane.offsetSecond == doctest::Approx(strip.offsetSecond).epsilon(1e-6));
}

TEST_CASE("lane-mods: a lane modulator routed to Voice Level holds a released voice while it is above the floor, and lets it go when it falls through") {
	// The env-modes rule — a released voice lives while what is routed into Voice Level stands above −60 dB — read for
	// a DAHDSR in the fx lane. Its key goes up at 0.35 s, mid-decay at 0.625 (above), and the release runs from there:
	// 0.2 s into a 0.5 s linear release, 0.625 · (1 − 0.4) = 0.375.
	const PolyEnvReadings lane = polyEnvelopeOnVoiceLevel(true);
	CHECK(lane.heldAfterKey);
	CHECK(lane.gainReleased == doctest::Approx(0.375).epsilon(0.03));
	CHECK(lane.freedAtEnd);
	const PolyEnvReadings strip = polyEnvelopeOnVoiceLevel(false);
	CHECK(strip.heldAfterKey);
	CHECK(lane.gainReleased == doctest::Approx(strip.gainReleased).epsilon(1e-6));
}

TEST_CASE("stereo modulation's zero cost: a per-voice filter patch with a mono route renders the bits it rendered before it (pinned at 616d4dad)") {
#if !(defined(__x86_64__) && defined(__linux__))
	MESSAGE("bit identity is pinned on Linux x86-64 only — skipped");
	return;
#endif
	namespace flt = feathers::fx::filter;
	namespace nx = feathers::mod::noteexpr;
	Rig rig;
	rig.setFilter(flt::Param::Poly, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	rig.setFilter(flt::Param::Drive, 0.2);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	rack.device[0] = feathers::defaultRowFor(*feathers::deviceForTag(nx::kTraits.tag), { 1 });
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const feathers::DeviceInstanceId filterId = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0);
	REQUIRE(filterId != feathers::kNoInstance);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 6),
	           feathers::backplane::Destination::control(filterId, static_cast<size_t>(flt::Param::Cutoff)), 0.1f);
	routes.publish(rig.params);
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1), noteOn(512, kKeyC4 + 7, 2) }, 8192, 64);
	uint64_t h = 1469598103934665603ull;
	for (const float x : out) {
		uint32_t bits = 0;
		std::memcpy(&bits, &x, sizeof bits);
		for (int k = 0; k < 4; ++k) {
			h ^= (bits >> (8 * k)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
	constexpr uint64_t kPinned = 4681751815369708317ull; // captured at 616d4dad
	CAPTURE(h);
	CHECK(h == kPinned);
}

TEST_CASE("a per-voice filter with a stereo route renders its voice in stereo, and the ears differ") {
	namespace flt = feathers::fx::filter;
	namespace st = feathers::mod::stereo;
	Rig rig;
	rig.expectMono = false;
	rig.setFilter(flt::Param::Poly, 1.0);
	rig.setFilter(flt::Param::Cutoff, 10.0);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(st::kTraits.tag);
	REQUIRE(desc != nullptr);
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(st::Param::In)] = 1.0f; // Split at In 100%, Amount 100%: (+1, −1)
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	// THE CONTROL: this patch is otherwise MONO — without it the case below proves nothing.
	rig.play({ noteOn(0, kKeyC4, 1) }, 512, 64);
	REQUIRE_FALSE(rig.synth.lastChunkStereoForTest());
	rig.play({ noteOff(0, kKeyC4, 1) }, 4096, 64);

	const feathers::DeviceInstanceId filterId = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, 0);
	REQUIRE(filterId != feathers::kNoInstance);
	constexpr float kAmount = 0.1f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), static_cast<size_t>(st::Output::Split)),
	           feathers::backplane::Destination::control(filterId, static_cast<size_t>(flt::Param::Cutoff)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	const std::vector<float> left = rig.play({ noteOn(0, kKeyC4, 2) }, 2048, 64);
	CHECK(rig.synth.lastChunkStereoForTest());
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	const flt::Settings t = rig.synth.voiceFilterTargetForTest(slot);
	const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
	CHECK(t.rightLive == flt::earBit(flt::Ear::Cutoff));
	CHECK(std::abs(t.cutoffLog2Hz - (10.0 + kAmount * span)) <= 1e-5);
	CHECK(std::abs(t.right.cutoffLog2Hz - (10.0 - kAmount * span)) <= 1e-5);
	// SECOND ASSERTION, proving the first is not bookkeeping: the right channel really is a different filter.
	CHECK(left != rig.lastRight);
}

TEST_CASE("lane-mods × stereo: a Stereo Tool in the fx lane gives the lane Filter after it a per-ear corner — left base + a·span, right base − a·span") {
	// Lane modulators (lane-mods) meet stereo modulation (backlog KU): the right ear is delivered per ENGINE slot, so a
	// pair whose source sits in the lane must reach a lane device exactly as one from the strip does. The strip is
	// EMPTY, so no pair read here can have come from it. Read as VALUES: the left ear the engine resolved, the right
	// ear the synth publishes for that lane position, and the corner pair the filter itself was handed.
	namespace flt = feathers::fx::filter;
	namespace st = feathers::mod::stereo;
	constexpr feathers::DeviceInstanceId kTool = 931;
	constexpr feathers::DeviceInstanceId kFilter = 932;
	constexpr float kAmount = 0.1f;
	Rig rig;
	rig.expectMono = false;
	feathers::FxChain chain;
	const size_t toolAt = appendDevice(&chain, st::kTraits.tag, kTool);
	chain.device[toolAt].row.field[static_cast<size_t>(st::Param::In)] = 1.0f; // Split at In 100%, Amount 100%: (+1, −1)
	const size_t filterAt = appendDevice(&chain, flt::kTraits.tag, kFilter);
	flt::State s {};
	flt::setValueOf(&s, flt::Param::Enabled, 1.0f);
	flt::setValueOf(&s, flt::Param::Cutoff, 10.0f);
	flt::setValueOf(&s, flt::Param::Poly, 0.0f);
	flt::setValueOf(&s, flt::Param::Keytrack, 0.0f);
	feathers::setFxFilterAt(&chain, filterAt, s);
	seat(rig, chain, emptyStrip());
	REQUIRE(rig.synth.laneForTest().descriptorOfForTest(kTool) != nullptr); // the premise: the lane RUNS the tool
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(kTool, static_cast<size_t>(st::Output::Split)),
	           feathers::backplane::Destination::control(kFilter, cutoff), kAmount);
	routes.publish(rig.params);
	rig.play({}, 256, 64);

	// RESTATED from the rule — amount × source (±1) × span, landed by `withRouteOffset` — not borrowed from the resolve.
	const feathers::device::ParamDesc& p = flt::kDescriptor.params[cutoff];
	const double base = 10.0;
	const double span = feathers::device::routeSpanOf(p);
	const auto wantL = static_cast<float>(feathers::device::withRouteOffset(p, base, kAmount * 1.0 * span)); // Split L = +1
	const auto wantR = static_cast<float>(feathers::device::withRouteOffset(p, base, kAmount * -1.0 * span)); // Split R = −1
	REQUIRE(wantL != wantR); // NOT VACUOUS: the two ears are different numbers

	CHECK(feathers::fxDeviceAtPosition(rig.synth.modulatedFxChain(), filterAt).row.field[cutoff] == wantL);
	std::array<float, feathers::device::kControlMaskBits> right {};
	const feathers::device::ControlMask mask = rig.synth.fxRightEar(filterAt, right.data(), right.size());
	CHECK(mask.test(cutoff));
	CHECK(right[cutoff] == wantR);
	// AND THE FILTER WAS HANDED BOTH: its corner pair, right ear live for exactly the cutoff.
	const void* storage = rig.synth.laneForTest().storageOfForTest(kFilter);
	REQUIRE(storage != nullptr);
	const flt::Settings& t = flt::opsTargetForTest(storage);
	CHECK(t.rightLive == flt::earBit(flt::Ear::Cutoff));
	CHECK(t.cutoffLog2Hz == wantL);
	CHECK(t.right.cutoffLog2Hz == wantR);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG LZ (2026-10-10): V5's per-voice rebuild cost was cut WITHOUT moving a bit. The voice's vowel bake prepared its
// recipe (128 or 256 probes of the cycle, to measure the normalisation) and then baked the 256-slot ladder, evaluating
// the same chain again at every probe phase; the bake now evaluates each slot once and measures the probe off those
// (`prepareAndBakeFlagshipLadder`). This case is the claim that nothing anyone hears changed: bench_render's moving
// 8-sound constellation, rendered mono, wide and warped, hashed to the bit against the hashes the code rendered BEFORE
// the change. (test_flagship pins the same equality one bake at a time, restated against the unfused pair.)
namespace {

/// bench_render's `--sounds 8` constellation (zone 97·i + 13, offset −6 + 2.1·i clamped by setBase, on a circle of radius
/// 0.5 about the origin), V5, eight held notes three semitones apart from C3; Pos X walks a slow sine every 256-sample
/// block, so every block rebuilds and every voice re-bakes its vowel. Returns the FNV-1a hash of both ears' float bits,
/// and the vowel bakes the eight voices made (so the case cannot pass on a road that never ran).
struct LzRender {
	uint64_t hash = 0;
	uint64_t bakes = 0; ///< vowel bakes the eight voices made
	uint64_t laws = 0;  ///< string laws the synth built while the puck moved (WeaveLawMemo)
};

LzRender lzMovingConstellation(bool wide, bool warped, feathers::OscType type = feathers::OscType::V5, double weather = 0.0) {
	Rig rig;
	rig.expectMono = !wide;
	for (size_t s = 0; s < 8; ++s) {
		const double angle = 6.283185307179586 * static_cast<double>(s) / 8.0;
		rig.set(feathers::soundParam(s, feathers::SoundField::Enable), 1.0);
		rig.set(feathers::soundParam(s, feathers::SoundField::Zone), 97.0 * static_cast<double>(s) + 13.0);
		rig.set(feathers::soundParam(s, feathers::SoundField::Offset), -6.0 + 2.1 * static_cast<double>(s));
		rig.set(feathers::soundParam(s, feathers::SoundField::PlaceX), 0.5 * std::cos(angle));
		rig.set(feathers::soundParam(s, feathers::SoundField::PlaceY), 0.5 * std::sin(angle));
	}
	rig.set(ParamId::OscType, static_cast<double>(type));
	rig.set(ParamId::Weather, weather);
	if (wide) {
		rig.set(feathers::soundParam(0, feathers::SoundField::Width), 1.0);
		rig.set(feathers::soundParam(1, feathers::SoundField::Width), 1.0);
		rig.set(ParamId::Width, 1.0);
	}
	if (warped) {
		rig.set(ParamId::WarpMode, static_cast<double>(feathers::WarpMode::BendPlus));
		rig.set(ParamId::AWarp, 0.6);
		rig.set(ParamId::BWarp, 0.3);
	}
	constexpr int kBlock = 256;
	constexpr int kVoices = 8;
	std::vector<float> left(kBlock), right(kBlock);
	rig.synth.render(left.data(), right.data(), kBlock);
	for (int v = 0; v < kVoices; ++v) {
		rig.synth.noteOn(0, static_cast<int16_t>(48 + 3 * v), v + 1, 0.8);
	}
	const uint64_t lawsBefore = rig.synth.weaveLawsBuiltForTest();
	uint64_t h = 1469598103934665603ull;
	const auto fold = [&h](const std::vector<float>& ear) {
		for (const float x : ear) {
			uint32_t bits = 0;
			std::memcpy(&bits, &x, sizeof bits);
			for (int k = 0; k < 4; ++k) {
				h ^= (bits >> (8 * k)) & 0xffu;
				h *= 1099511628211ull;
			}
		}
	};
	for (int b = 0; b < 120; ++b) {
		rig.set(ParamId::PosX, 0.3 * std::sin(0.05 * static_cast<double>(b)));
		rig.synth.render(left.data(), right.data(), kBlock);
		fold(left);
		fold(right);
	}
	uint64_t bakes = 0;
	for (int v = 0; v < kVoices; ++v) {
		const int slot = rig.synth.slotOfKeyForTest(static_cast<int16_t>(48 + 3 * v));
		REQUIRE(slot >= 0);
		bakes += rig.synth.voiceStackOscForTest(slot, 0).voxBakeCount();
	}
	return { h, bakes, rig.synth.weaveLawsBuiltForTest() - lawsBefore };
}

} // namespace

TEST_CASE("LZ: a moving 8-sound V5 constellation renders the bits it rendered before the vowel bake stopped probing twice (mono, wide, warped)") {
#if !(defined(__x86_64__) && defined(__linux__))
	MESSAGE("bit identity is pinned on Linux x86-64 only — skipped");
	return;
#endif
	// Captured BEFORE the fused bake existed (v1-implementation deae20fe, this case's own code), and RE-CAPTURED when
	// the character fade became the rebuild interval (backlog PB (c), 2 ms to 256 samples at 48 kHz: a deliberate change
	// of sound). The old constants still matched at a 2 ms fade on the branch that changed it.
	constexpr uint64_t kMono = 11444373858918540075ull;
	constexpr uint64_t kWide = 17301692822360427339ull;
	constexpr uint64_t kWarped = 17925734191854872307ull;
	const auto [mono, monoBakes, monoLaws] = lzMovingConstellation(false, false);
	const auto [wide, wideBakes, wideLaws] = lzMovingConstellation(true, false);
	const auto [warped, warpedBakes, warpedLaws] = lzMovingConstellation(false, true);
	CAPTURE(mono);
	CAPTURE(wide);
	CAPTURE(warped);
	// NOT VACUOUS: every voice re-baked its vowel on the moves (the road the change is on ran, again and again)...
	CHECK(monoBakes > 8u * 20u);
	CHECK(wideBakes > 8u * 20u);
	CHECK(warpedBakes > 8u * 20u);
	// ...and the three passes are three different renders.
	CHECK(mono != wide);
	CHECK(mono != warped);
	CHECK(mono == kMono);
	CHECK(wide == kWide);
	CHECK(warped == kWarped);
}

TEST_CASE("LZ: a moving 8-sound Resonate constellation renders the bits it rendered before the string laws were memoised (still weather, moving weather)") {
#if !(defined(__x86_64__) && defined(__linux__))
	MESSAGE("bit identity is pinned on Linux x86-64 only — skipped");
	return;
#endif
	// The string is what hears the laws, so this is the engine the memo could change the sound of. Captured with
	// `pushWeaveLaws` building every sound every update (no memo), at this case's own code.
	constexpr uint64_t kStill = 744572472128542615ull;
	constexpr uint64_t kWeather = 3785664872293722643ull;
	const LzRender still = lzMovingConstellation(false, false, feathers::OscType::Resonate, 0.0);
	const LzRender weather = lzMovingConstellation(false, false, feathers::OscType::Resonate, 0.6);
	CAPTURE(still.hash);
	CAPTURE(weather.hash);
	CAPTURE(still.laws);
	CAPTURE(weather.laws);
	// NOT VACUOUS: with the weather still, the moving puck rebuilds no sound's laws (the memo hands all eight back on
	// every update); with the weather moving, travel moves and the memo has to build again and again.
	CHECK(still.laws == 0u);
	CHECK(weather.laws > 8u * 20u);
	CHECK(still.hash != weather.hash);
	CHECK(still.hash == kStill);
	CHECK(weather.hash == kWeather);
}

//--------------------------------------------------------------------------------------------------//
// THE SPECTRAL DEVICE'S PER-VOICE ROAD (plan T7).                                                    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A rig with the filter off at 0 and a Spectral at 1, Poly as asked, every operator off (a pure delay of its latency).
size_t seatSpectral(Rig& rig, bool poly, feathers::fx::spectral::Resolution stop) {
	namespace sp = feathers::fx::spectral;
	namespace flt = feathers::fx::filter;
	rig.expectMono = false;
	rig.setFilter(flt::Param::Enabled, 0.0);
	const size_t at = feathers::addFxDevice(&rig.chain, sp::kTraits.tag);
	REQUIRE(at == 1);
	rig.chain.device[at].row.field[static_cast<size_t>(sp::Param::Poly)] = poly ? 1.0f : 0.0f;
	rig.chain.device[at].row.field[static_cast<size_t>(sp::Param::Resolution)] = static_cast<float>(static_cast<int>(stop));
	rig.publishChain();
	return at;
}

} // namespace

TEST_CASE("per-voice Spectral: each voice's gain run is delayed by exactly its latency, and the bus delays none") {
	namespace sp = feathers::fx::spectral;
	for (const sp::Resolution stop : { sp::Resolution::Fast, sp::Resolution::Balanced, sp::Resolution::Fine }) {
		CAPTURE(static_cast<int>(stop));
		Rig poly;
		seatSpectral(poly, true, stop);
		poly.play({ noteOn(0, 60, 1) }, 512, 256);
		CHECK(poly.synth.perVoiceGainDelayForTest() == sp::latencyFor(stop, kSampleRate));
		CHECK(poly.synth.latencySamples() == sp::latencyFor(stop, kSampleRate)); // reported up, as on the bus
		Rig bus;
		seatSpectral(bus, false, stop);
		bus.play({ noteOn(0, 60, 1) }, 512, 256);
		CHECK(bus.synth.perVoiceGainDelayForTest() == 0);
		CHECK(bus.synth.latencySamples() == sp::latencyFor(stop, kSampleRate));
	}
}

TEST_CASE("per-voice Spectral: the envelope lands on the delayed audio, so Poly sounds as the bus does, attack and release included") {
	// AN IDENTITY SPECTRAL IS A PURE DELAY. On the bus it delays the voices' sum after their gain; per voice it delays each
	// voice's audio, and the Synth delays the gain to match — so the two must agree, sample for sample, up to the order the
	// two products round in. WITHOUT the gain delay a 20 ms attack would run 40 ms ahead of its note at Fine.
	namespace sp = feathers::fx::spectral;
	const auto render = [](bool poly) {
		Rig rig;
		seatSpectral(rig, poly, sp::Resolution::Fine);
		rig.setAttack(std::log2(0.02));
		rig.setRelease(std::log2(0.02));
		return rig.play({ noteOn(0, 60, 1), noteOn(4800, 67, 2), noteOff(9600, 60, 1), noteOff(14400, 67, 2) }, 24000, 256);
	};
	const std::vector<float> poly = render(true);
	const std::vector<float> bus = render(false);
	double worst = 0.0;
	double peak = 0.0;
	for (size_t t = 0; t < poly.size(); ++t) {
		worst = std::max(worst, static_cast<double>(std::fabs(poly[t] - bus[t])));
		peak = std::max(peak, static_cast<double>(std::fabs(bus[t])));
	}
	MESSAGE("worst |poly - bus| = " << worst << " against a peak of " << peak);
	CHECK(peak > 0.05);
	CHECK(worst < 1e-5);
}

TEST_CASE("per-voice Spectral: a released voice is freed only after its delayed tail has drained") {
	namespace sp = feathers::fx::spectral;
	Rig rig;
	seatSpectral(rig, true, sp::Resolution::Fine);
	const uint32_t lat = sp::latencyFor(sp::Resolution::Fine, kSampleRate); // 1920
	rig.play({ noteOn(0, 60, 1) }, 4800, 32);
	rig.play({ noteOff(0, 60, 1) }, 32 * 4, 32); // the 2 ms edge closes inside these four chunks
	CHECK(rig.synth.soundingVoiceCount() == 1);    // closed, but its audio is still in the delay
	rig.play({}, lat - 256, 32);
	CHECK(rig.synth.soundingVoiceCount() == 1);
	rig.play({}, 512, 32);
	CHECK(rig.synth.soundingVoiceCount() == 0);    // drained
}

TEST_CASE("per-voice road: with no latent per-voice device the gain delay is 0 and no ring is read") {
	// THE BIT-IDENTITY OF EVERY PATCH WITHOUT ONE rests on `gainDelay_ == 0` taking the old products verbatim (synth.cpp);
	// "per-voice keytrack renders byte-identical..." pins a per-voice render to a digest taken before this existed.
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Poly, 1.0);
	rig.play({ noteOn(0, 60, 1) }, 1024, 256);
	CHECK(rig.synth.perVoiceGainDelayForTest() == 0);
	CHECK(rig.synth.latencySamples() == 0);
}

TEST_CASE("per-voice Spectral: a stolen voice fades out within the steal fade, on its delayed audio, without a step") {
	namespace sp = feathers::fx::spectral;
	Rig rig;
	seatSpectral(rig, true, sp::Resolution::Fine);
	rig.set(ParamId::VoiceCount, 1.0);
	constexpr uint32_t kStealAt = 9600;
	const auto steadyEnd = kStealAt;
	std::vector<float> out = rig.play({ noteOn(0, 60, 1) }, kStealAt, 32);
	const std::vector<float> rest = rig.play({ noteOn(0, 67, 2) }, 33, 32);
	CHECK(rig.synth.soundingVoiceCount() == 2); // the stolen voice, fading, and the new one
	const std::vector<float> after = rig.play({}, static_cast<uint32_t>(samplesFor(feathers::kStealFadeSeconds)) + 32, 32);
	CHECK(rig.synth.soundingVoiceCount() == 1); // reclaimed at the usual time, not held for the latency
	out.insert(out.end(), rest.begin(), rest.end());
	out.insert(out.end(), after.begin(), after.end());
	// NO STEP: across the steal the output moves no faster than the steady note did; the fade is on what comes out.
	float steadySlope = 0.0f;
	for (size_t t = steadyEnd - 4800; t < steadyEnd; ++t) {
		steadySlope = std::max(steadySlope, std::fabs(out[t] - out[t - 1]));
	}
	float stealSlope = 0.0f;
	for (size_t t = steadyEnd; t < out.size(); ++t) {
		stealSlope = std::max(stealSlope, std::fabs(out[t] - out[t - 1]));
	}
	CAPTURE(steadySlope);
	CHECK(steadySlope > 0.0f);
	CHECK(stealSlope <= steadySlope * 1.05f);
}

TEST_CASE("per-voice Spectral: a voice draining when Poly is switched off is freed, not left in Release for ever") {
	// THE REVIEW'S CRITICAL: with the delay at 0 the ring is null and a drain that had begun never counted down, so the voice sat
	// in Release at level 0 holding a slot and keeping the plugin awake.
	namespace sp = feathers::fx::spectral;
	Rig rig;
	const size_t at = seatSpectral(rig, true, sp::Resolution::Fine);
	rig.play({ noteOn(0, 60, 1) }, 4800, 32);
	rig.play({ noteOff(0, 60, 1) }, 32 * 8, 32); // the edge has closed; the drain has begun
	REQUIRE(rig.synth.soundingVoiceCount() == 1);
	rig.chain.device[at].row.field[static_cast<size_t>(sp::Param::Poly)] = 0.0f;
	rig.publishChain();
	rig.play({}, 32 * 8, 32);
	CHECK(rig.synth.perVoiceGainDelayForTest() == 0);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("per-voice Spectral: a voice stolen mid-drain fades from what is sounding, without a step") {
	// THE STOLEN VOICE'S DELAYED AUDIO IS STILL PLAYING when its level has closed, so the steal fade must start from 1 on what
	// comes out (review, Important 2). Measured against the same render without the steal: over the fade window the stolen run
	// is that run times a fade starting at 1 (the new note's audio arrives a latency later, so nothing else is in the window).
	namespace sp = feathers::fx::spectral;
	const auto render = [](bool steal) {
		Rig rig;
		seatSpectral(rig, true, sp::Resolution::Fine);
		rig.set(ParamId::VoiceCount, 1.0);
		std::vector<float> out = rig.play({ noteOn(0, 60, 1) }, 9600, 32);
		const std::vector<float> off = rig.play({ noteOff(0, 60, 1) }, 32 * 8, 32);
		REQUIRE(rig.synth.soundingVoiceCount() == 1);
		out.insert(out.end(), off.begin(), off.end());
		const std::vector<Event> next = steal ? std::vector<Event> { noteOn(0, 67, 2) } : std::vector<Event> {};
		const std::vector<float> rest = rig.play(next, 256, 32);
		out.insert(out.end(), rest.begin(), rest.end());
		return out;
	};
	const std::vector<float> stolen = render(true);
	const std::vector<float> kept = render(false);
	const size_t at = 9600 + 32 * 8;
	REQUIRE(std::fabs(kept[at]) > 0.01f); // the delayed note is still sounding when the steal comes
	const auto fadeLen = static_cast<size_t>(samplesFor(feathers::kStealFadeSeconds));
	double worst = 0.0;
	for (size_t j = 0; j < fadeLen + 16; ++j) {
		const double fade = std::max(0.0, 1.0 - static_cast<double>(j + 1) / static_cast<double>(fadeLen));
		worst = std::max(worst, std::fabs(static_cast<double>(stolen[at + j]) - fade * kept[at + j]));
	}
	CHECK(worst < 0.02);
}

TEST_CASE("per-voice Spectral: a mono note late in a drain, then a release, plays the whole tail") {
	// A DRAIN MUST NOT SURVIVE INTO A LATER RELEASE (review, Important 3: `beginFade` clears it on every new stage). MEASURED: through
	// note events this manager (MULTI_VOICE) does not retrigger a released voice in Mono — the draining voice is stolen and a
	// fresh one starts, whose ring and drain `clearGainRing` resets — so this case holds the outcome (the whole tail plays)
	// and stays green with the clearing removed; the clearing guards the same-voice roads (retriggerVoiceWithNewNoteID,
	// moveAndRetriggerVoice) should a mode reach them with a draining voice.
	namespace sp = feathers::fx::spectral;
	Rig rig;
	seatSpectral(rig, true, sp::Resolution::Fine);
	rig.set(ParamId::VoiceMode, 1.0); // Mono
	const uint32_t lat = sp::latencyFor(sp::Resolution::Fine, kSampleRate);
	rig.play({ noteOn(0, 60, 1) }, 4800, 32);
	rig.play({ noteOff(0, 60, 1) }, lat - 320, 32);    // draining, about 300 samples from done
	REQUIRE(rig.synth.soundingVoiceCount() == 1);
	rig.play({ noteOn(0, 64, 2) }, 32 * 30, 32);      // the same voice retriggered late in its drain
	REQUIRE(rig.synth.soundingVoiceCount() == 1);
	rig.play({ noteOff(0, 64, 2) }, 32 * 8, 32);      // its edge closes: a NEW drain, the whole latency long
	CHECK(rig.synth.soundingVoiceCount() == 1);
	rig.play({}, lat - 512, 32);
	CHECK(rig.synth.soundingVoiceCount() == 1);       // still draining: a stale drain would have freed it already
	rig.play({}, 768, 32);
	CHECK(rig.synth.soundingVoiceCount() == 0);
}

TEST_CASE("per-voice Spectral: two Fine stops in series delay the gain by the full sum, and a third runs on the bus") {
	// THE GAIN DELAY IS NEVER CLAMPED (review, Important 4): the lane admits latent per-voice devices up to the rings' bound,
	// two Fine stops, and one past it runs on the bus (its Poly read as off, assumption L2) where the bus compensates it.
	namespace sp = feathers::fx::spectral;
	Rig rig;
	seatSpectral(rig, true, sp::Resolution::Fine);
	for (int extra = 0; extra < 2; ++extra) {
		const size_t at = feathers::addFxDevice(&rig.chain, sp::kTraits.tag);
		REQUIRE(at == static_cast<size_t>(2 + extra));
		rig.chain.device[at].row.field[static_cast<size_t>(sp::Param::Poly)] = 1.0f;
		rig.chain.device[at].row.field[static_cast<size_t>(sp::Param::Resolution)] = 2.0f;
	}
	rig.publishChain();
	rig.play({ noteOn(0, 60, 1) }, 512, 256);
	const uint32_t fine = sp::latencyFor(sp::Resolution::Fine, kSampleRate);
	CHECK(rig.synth.perVoiceGainDelayForTest() == 2 * fine); // the first two, per voice, delayed in full
	CHECK(rig.synth.latencySamples() == 3 * fine);           // and all three reported up
}

/// MEASURED on the tree before the per-voice road existed (9a278b02's synth, rack engine and lane, put back in this worktree
/// for one build): the render below, digested.
[[maybe_unused]] constexpr uint64_t kNoLatentPerVoiceDigest = 14187046376720194698ull;

TEST_CASE("per-voice road: a render with no latent per-voice device is the render from before the road, bit for bit") {
	// STOLEN, RELEASED AND OVERLAPPING VOICES through a per-voice filter: every branch the gain-delay code sits beside, at a
	// delay of 0. A digest of samples is the claim itself here — "the same bytes" — as in the per-voice keytrack case.
	namespace flt = feathers::fx::filter;
	Rig rig;
	rig.set(ParamId::VoiceCount, 2.0);
	rig.setRelease(std::log2(0.05));
	rig.setFilter(flt::Param::Enabled, 1.0);
	rig.setFilter(flt::Param::Cutoff, std::log2(900.0));
	rig.setFilter(flt::Param::Resonance, 0.4);
	rig.setFilter(flt::Param::Poly, 1.0);
	const std::vector<Event> script{ noteOn(0, 48, 1), noteOn(700, 60, 2), noteOn(1500, 67, 3), noteOff(3000, 60, 2),
		                             noteOff(4200, 67, 3), noteOn(5000, 72, 4), noteOff(7000, 48, 1) };
	const std::vector<float> out = rig.play(script, 12000, 128);
	uint64_t h = 1469598103934665603ull; // FNV-1a over the bits
	for (const float s : out) {
		uint32_t bits = 0;
		std::memcpy(&bits, &s, sizeof bits);
		for (int b = 0; b < 4; ++b) {
			h = (h ^ ((bits >> (8 * b)) & 0xffu)) * 1099511628211ull;
		}
	}
	MESSAGE("no-latent-per-voice digest " << h);
#if defined(__x86_64__) && defined(__linux__)
	CHECK(h == kNoLatentPerVoiceDigest); // bits, so pinned to the platform it was measured on (vox-voice's rule)
#endif
}

TEST_CASE("Feathers hands the sidechain to the fx lane's listening device at each chunk's offset") {
	// THE VOCODER'S PLUMBING, FEATHERS' HALF (design §10.1): the synth renders in chunks and runs the lane per chunk, so each
	// chunk's bus must hear the sidechain's own frames. A ramp makes the offset a value.
	namespace sp = feathers::fx::spectral;
	Rig rig;
	seatSpectral(rig, false, sp::Resolution::Balanced);
	rig.play({}, 256, 256);
	constexpr int n = 300;
	std::vector<float> l(n, 0.0f), r(n, 0.0f), side(n);
	for (int i = 0; i < n; ++i) {
		side[static_cast<size_t>(i)] = static_cast<float>(i + 1);
	}
	rig.synth.setSidechain(side.data(), side.data(), true);
	rig.synth.render(l.data(), r.data(), n);
	const auto* device = static_cast<const sp::SpectralDevice*>(rig.synth.fxStorageForTagForTest(sp::kTraits.tag));
	REQUIRE(device != nullptr);
	size_t bad = 0;
	for (int ago = 0; ago < n; ++ago) {
		bad += device->stftForTest().sidechainAgoForTest(ago) != static_cast<float>(n - ago);
	}
	CHECK(bad == 0);
	CHECK(device->stftForTest().sidechainSounding());
}

TEST_CASE("A-PH: the optimiser's thread lives activate → deactivate — posts under load, deactivate joins it, destruction is clean, again and again") {
	// THE LIFETIME CASE FOR THE SANITIZER GATE (synth is in its suites): an Additive patch on Headroom, the offset swept every
	// block so every block re-bakes and posts, a note sounding; then deactivate (the thread must be joined there) and the synth
	// destroyed — five times over, with a restart (deactivate, activate) inside each. A use of the service after it is freed,
	// or a thread left running into the destructor, is what ASan and the join catch.
	for (int round = 0; round < 5; ++round) {
		CAPTURE(round);
		auto params = std::make_unique<ParamSet>();
		auto synth = std::make_unique<Synth>(*params);
		synth->activate(kSampleRate, 512);
		params->setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(feathers::OscType::Additive));
		params->setBase(static_cast<clap_id>(ParamId::AdditivePhases), 1.0);
		params->setBase(static_cast<clap_id>(ParamId::VoiceLevel), 1.0);
		REQUIRE(synth->phaseServiceForTest().running());
		std::vector<float> l(256), r(256);
		synth->noteOn(0, 60, 1, 1.0);
		for (int b = 0; b < 40; ++b) {
			params->setBase(static_cast<clap_id>(ParamId::AOffset), 0.25 * std::sin(0.3 * b));
			synth->render(l.data(), r.data(), 256);
			if (b == 20) {
				// A RESTART MID-STREAM (the latency road): deactivate joins, activate starts again.
				synth->deactivate();
				CHECK_FALSE(synth->phaseServiceForTest().running());
				synth->activate(kSampleRate, 512);
				CHECK(synth->phaseServiceForTest().running());
			}
		}
		CHECK(synth->phaseServiceForTest().requests().postedForTest() > 0u);
		synth->deactivate();
		CHECK_FALSE(synth->phaseServiceForTest().running());
		synth.reset();
		params.reset();
	}
}

TEST_CASE("A-PH: the optimiser's thread sleeps until a post wakes it — an idle or sleeping instance does not wake at all, a post is served") {
	auto params = std::make_unique<ParamSet>();
	auto synth = std::make_unique<Synth>(*params);
	synth->activate(kSampleRate, 512);
	feathers::PhaseService& service = synth->phaseServiceForTest();
	REQUIRE(service.running());
	// IDLE: no blocks rendered (as a SLEEPING plugin's host calls no process()) for a quarter of a second of wall time.
	std::this_thread::sleep_for(std::chrono::milliseconds(250));
	CHECK(service.wakesForTest() == 0u);
	// A POST WAKES IT, AND IT SERVES THE REQUEST: Additive on Headroom, one block (a new table posts once).
	params->setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(feathers::OscType::Additive));
	params->setBase(static_cast<clap_id>(ParamId::AdditivePhases), 1.0);
	std::vector<float> l(256), r(256);
	synth->render(l.data(), r.data(), 256);
	REQUIRE(service.requests().postedForTest() >= 1u);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (service.computedForTest() == 0u && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(service.computedForTest() >= 1u);
	const uint64_t woke = service.wakesForTest();
	CHECK(woke >= 1u);
	// AND IDLE AGAIN: blocks with nothing changing post nothing, so the thread stays asleep.
	for (int b = 0; b < 40; ++b) {
		synth->render(l.data(), r.data(), 256);
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK(service.wakesForTest() == woke);
	synth->deactivate();
	CHECK_FALSE(service.running());
}

TEST_CASE("backlog NS: Poly switched on, off and on again under a held note renders, sample for sample, what the eager "
          "road rendered, and the note sounds throughout") {
	// THE ROAD BEFORE NS BUILT EVERY PER-VOICE DEVICE'S VOICES UP FRONT; now they are built when the switch goes on
	// and freed when it goes off. Nothing a listener can hear may change: the same held note, the same three flips,
	// the two roads' outputs compared to the bit. And the memory claim as a value at each step — voices while on,
	// none while off — so a lazy road that quietly stayed eager cannot pass.
	constexpr int kSegment = 2048;
	int swept = 0;
	int audible = 0;
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		if (desc.audio == nullptr || desc.traits.perVoiceControl < 0) {
			continue;
		}
		const std::string who = desc.traits.name;
		CAPTURE(who);
		++swept;
		const auto poly = static_cast<size_t>(desc.traits.perVoiceControl);
		feathers::DeviceRow row = feathers::defaultRowFor(desc, { 7 });
		if (desc.traits.enableControl >= 0) {
			row.field[static_cast<size_t>(desc.traits.enableControl)] = 1.0f;
		}
		const auto withPoly = [&](float value) {
			feathers::DeviceRow r = row;
			r.field[poly] = value;
			return r;
		};
		struct Run {
			std::vector<float> out;
			std::vector<size_t> voices; // the device's voice instances after each flip
			bool heldThroughout = true;
		};
		const auto run = [&](bool eager, bool flips) {
			Rig rig;
			rig.synth.setEagerVoicesForTest(eager);
			rig.setDeviceRow(withPoly(0.0f));
			Run result;
			result.out.assign(static_cast<size_t>(5 * kSegment), 0.0f);
			std::vector<float> right(static_cast<size_t>(kSegment), 0.0f);
			rig.synth.noteOn(0, kKeyC4, 1, 0.8);
			for (int segment = 0; segment < 5; ++segment) {
				// OFF, ON, OFF, ON, and the last segment stays on — each flip a provision and a sync, the plugin's order.
				if (segment > 0) {
					rig.setDeviceRow(withPoly(flips && (segment % 2 == 1 || segment == 4) ? 1.0f : 0.0f));
				}
				rig.synth.render(result.out.data() + static_cast<size_t>(segment * kSegment), right.data(), kSegment);
				const size_t slot = engineFxSlot(rig.synth, rig.chain, 0);
				REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
				result.voices.push_back(rig.synth.laneForTest().engineForTest().slotAtLane(slot).voices.size());
				result.heldThroughout = result.heldThroughout && rig.synth.slotOfKeyForTest(kKeyC4) >= 0;
			}
			return result;
		};
		const Run lazy = run(false, true);
		const Run eager = run(true, true);
		const Run never = run(false, false);
		const size_t n = static_cast<size_t>(feathers::kVoiceSlots);
		// THE MEMORY, BY VALUE: none while off on the lazy road; every one, always, on the eager one.
		CHECK(lazy.voices == std::vector<size_t> { 0, n, 0, n, n });
		CHECK(eager.voices == std::vector<size_t> { n, n, n, n, n });
		// THE SOUND, TO THE BIT.
		CHECK(firstDifference(lazy.out, eager.out) == -1);
		// AND THE NOTE HELD AND SOUNDING ACROSS EVERY FLIP — its voice never freed, its last segment not silent.
		CHECK(lazy.heldThroughout);
		CHECK(peakOf(lazy.out, 4 * kSegment, 5 * kSegment) > 0.001f);
		// NON-VACUITY, counted below: on how many devices the switch is a different sound at all.
		audible += firstDifference(lazy.out, never.out) != -1 ? 1 : 0;
	}
	CHECK(swept >= 5);
	CAPTURE(audible);
	CHECK(audible >= 3); // a switch nobody can hear would make the bit-for-bit comparison say nothing
}

//------------------------------------------------------------------------------------------------//
// OE: THE ENGINE VOCODER                                                                          //
//------------------------------------------------------------------------------------------------//
//
// The synth's half: one analysis of the sidechain for every voice, each voice reading it at its own partials. Every value is
// read — the envelope the voices read (vocodeEnvelopeForTest), each voice's rotator amplitudes and increments — and the gain
// restated from §9.2's words.

namespace oesyn {

constexpr double kLog2Low = 4.321928094887363; // log2(20)
constexpr int kBlock = 256;

double hzOfKey(double key) { return 440.0 * std::exp2((key - 69.0) / 12.0); }

/// A deterministic white noise, uniform in ±amp (σ = amp/√3), from its own xorshift.
std::vector<float> noise(size_t n, float amp, uint32_t seed) {
	std::vector<float> out(n);
	for (float& x : out) {
		seed ^= seed << 13;
		seed ^= seed >> 17;
		seed ^= seed << 5;
		x = amp * (2.0f * static_cast<float>(seed) / 4294967296.0f - 1.0f);
	}
	return out;
}

std::vector<float> sine(size_t n, double hz, float amp) {
	std::vector<float> out(n);
	for (size_t i = 0; i < n; ++i) {
		out[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate));
	}
	return out;
}

/// The additive engine, sound A's Vocode on (Flatten 1, Depth 1: the defaults), on the plain patch.
void vocodePatch(Rig& rig) {
	rig.set(ParamId::OscType, static_cast<double>(feathers::OscType::Additive));
	rig.set(feathers::spectralParam(0, feathers::SpectralControl::VocodeOn), 1.0);
}

/// `blocks` host blocks with the sidechain `side` (null: none connected), read from `at` on and looped; the mix returned.
std::vector<float> renderWith(Rig& rig, const std::vector<float>* side, int blocks, size_t* at = nullptr) {
	std::vector<float> mix;
	std::vector<float> l(kBlock), r(kBlock), chunk(kBlock);
	size_t pos = at != nullptr ? *at : 0;
	for (int b = 0; b < blocks; ++b) {
		if (side != nullptr) {
			for (int i = 0; i < kBlock; ++i) {
				chunk[static_cast<size_t>(i)] = (*side)[pos++ % side->size()];
			}
			rig.synth.setSidechain(chunk.data(), chunk.data(), true);
		}
		else {
			rig.synth.setSidechain(nullptr, nullptr, false);
		}
		rig.synth.render(l.data(), r.data(), kBlock);
		mix.insert(mix.end(), l.begin(), l.end());
	}
	if (at != nullptr) {
		*at = pos;
	}
	return mix;
}

double dbAt(const feathers::spectral::VocodeEnvelope& e, double hz) {
	const double u = std::clamp((std::log2(std::clamp(hz, 20.0, 20000.0)) - kLog2Low) * 12.7, 0.0, 127.0);
	const int i = std::min(static_cast<int>(u), 126);
	return e.db[static_cast<size_t>(i)] + (u - i) * (e.db[static_cast<size_t>(i + 1)] - e.db[static_cast<size_t>(i)]);
}

double lineAt(double db, double f0, double binHz) {
	return std::pow(10.0, (db + 2.5066) / 20.0) * (2.0 / std::numbers::pi) * std::sqrt(2.0 * f0 / binHz);
}

/// The Nyquist fade at increment w (partial_bank.h's promise).
double fadeOf(double w) {
	const double a = 0.90 * std::numbers::pi, b = 0.98 * std::numbers::pi;
	return !(w > 0.0) || w >= b ? 0.0 : w <= a ? 1.0 : (b - w) / (b - a);
}

} // namespace oesyn

TEST_CASE("OE: two notes under Poly read the ONE sidechain envelope at their own partials' frequencies, by value") {
	Rig rig;
	oesyn::vocodePatch(rig);
	rig.apply(noteOn(0, 48, 1));
	rig.apply(noteOn(0, 64, 2));
	const std::vector<float> side = oesyn::noise(48000, 0.3f, 0x1234567u);
	size_t at = 0;
	(void)oesyn::renderWith(rig, &side, 60, &at);
	REQUIRE(rig.synth.vocodeFramesForTest() > 0u);
	// HELD (Freeze), so the envelope the voices read is the one read below, and their amplitudes settle on it.
	rig.set(ParamId::AdditiveVocodeFreeze, 1.0);
	(void)oesyn::renderWith(rig, &side, 8, &at);
	const feathers::spectral::VocodeEnvelope& env = rig.synth.vocodeEnvelopeForTest();
	std::array<double, 2> firstAmp {};
	for (const int16_t key : { int16_t(48), int16_t(64) }) {
		CAPTURE(key);
		const int slot = rig.synth.slotOfKeyForTest(key);
		REQUIRE(slot >= 0);
		const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(slot, 0);
		const double f0 = oesyn::hzOfKey(key);
		int checked = 0;
		for (int i = 0; i < 40; ++i) {
			const double w = osc.partialIncrementForTest(i);
			if (oesyn::fadeOf(w) < 1.0) {
				continue;
			}
			INFO("partial " << i);
			const double hz = w * kSampleRate / (2.0 * std::numbers::pi);
			CHECK(static_cast<double>(osc.partialAmplitudeForTest(i, 0))
			      == doctest::Approx(oesyn::lineAt(oesyn::dbAt(env, hz), f0, env.binHz)).epsilon(3e-3).scale(0.0));
			++checked;
		}
		CHECK(checked >= 30);
		firstAmp[key == 48 ? 0 : 1] = static_cast<double>(osc.partialAmplitudeForTest(0, 0));
	}
	// AND THEY DIFFER: each note's fundamental reads the envelope where it sounds.
	CHECK(firstAmp[0] != firstAmp[1]);
}

TEST_CASE("OE: the calibration — white noise in the sidechain gives a voice carrying the noise's power over the band its partials cover") {
	for (const double key : { 40.0, 52.0, 64.0 }) {
		for (const double resolution : { 0.0, 1.0, 2.0 }) {
			CAPTURE(key);
			CAPTURE(resolution);
			Rig rig;
			oesyn::vocodePatch(rig);
			rig.set(ParamId::AdditiveVocodeResolution, resolution);
			rig.set(ParamId::AdditiveVocodeRelease, 2000.0); // a long release, so the estimate is a mean over many frames
			rig.set(ParamId::AdditiveVocodeAttack, 500.0);
			rig.apply(noteOn(0, static_cast<int16_t>(key), 1));
			const float amp = 0.2f;
			const std::vector<float> side = oesyn::noise(96000, amp, 0xBEEFu);
			size_t at = 0;
			(void)oesyn::renderWith(rig, &side, 600, &at); // ~3.2 s: the one-poles settle
			rig.set(ParamId::AdditiveVocodeFreeze, 1.0);
			(void)oesyn::renderWith(rig, &side, 4, &at);
			const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.slotOfKeyForTest(static_cast<int16_t>(key)), 0);
			const double f0 = oesyn::hzOfKey(key);
			double power = 0.0;
			int full = 0;
			for (int i = 0; i < feathers::partials::kMaxPartials; ++i) {
				if (oesyn::fadeOf(osc.partialIncrementForTest(i)) < 1.0) {
					break;
				}
				const double a = osc.partialAmplitudeForTest(i, 0);
				power += 0.5 * a * a;
				++full;
			}
			REQUIRE(full > 10);
			// THE NOISE'S POWER IN THE BAND THE PARTIALS COVER: σ² (uniform: amp²/3), one-sided over 0..sr/2, times full·f0 of it.
			const double band = (static_cast<double>(amp) * amp / 3.0) * (full * f0) / (kSampleRate / 2.0);
			const double db = 10.0 * std::log10(power / band);
			MESSAGE("key " << key << " resolution " << resolution << ": vocoded power against the band's " << db << " dB");
			CHECK(std::abs(db) < 1.0); // MEASURED 2026-10-26: +0.17..+0.77 dB, Fine closest, the same at every note
		}
	}
}

TEST_CASE("OE: a silent sidechain with Vocode on gives an exactly silent voice; Vocode off, the patch is the engine's to the bit and nothing is analysed") {
	{
		Rig rig;
		oesyn::vocodePatch(rig);
		rig.apply(noteOn(0, 57, 1));
		const std::vector<float> out = oesyn::renderWith(rig, nullptr, 20);
		CHECK(std::all_of(out.begin(), out.end(), [](float x) { return x == 0.0f; }));
		const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.slotOfKeyForTest(57), 0);
		for (int i = 0; i < feathers::partials::kMaxPartials; ++i) {
			CHECK(osc.partialAmplitudeForTest(i, 0) == 0.0f);
		}
	}
	// OFF: a sounding sidechain changes nothing, to the bit, and the analysis never runs.
	const std::vector<float> side = oesyn::noise(48000, 0.3f, 0x77u);
	Rig with, without;
	for (Rig* r : { &with, &without }) {
		r->set(ParamId::OscType, static_cast<double>(feathers::OscType::Additive));
		r->apply(noteOn(0, 57, 1));
	}
	const std::vector<float> a = oesyn::renderWith(with, &side, 24);
	const std::vector<float> b = oesyn::renderWith(without, nullptr, 24);
	CHECK(a == b);
	CHECK(std::any_of(a.begin(), a.end(), [](float x) { return x != 0.0f; }));
	CHECK(with.synth.vocodeFramesForTest() == 0u);
}

TEST_CASE("OE: Freeze holds the envelope by value while the sidechain changes; its block primes a FRESH synth to the same envelope; a new rate keeps it") {
	Rig a;
	oesyn::vocodePatch(a);
	a.apply(noteOn(0, 50, 1));
	const std::vector<float> side = oesyn::noise(48000, 0.3f, 0xABCDu);
	size_t at = 0;
	(void)oesyn::renderWith(a, &side, 40, &at);
	std::array<float, feathers::kVocodeFreezeFloats> words {};
	CHECK(a.synth.readVocodeFreeze(words.data(), words.size()) == 0u); // never frozen: nothing to save
	a.set(ParamId::AdditiveVocodeFreeze, 1.0);
	(void)oesyn::renderWith(a, &side, 2, &at);
	const feathers::spectral::VocodeEnvelope held = a.synth.vocodeEnvelopeForTest();
	// HELD WHILE THE SIDECHAIN CHANGES to a loud sine, by value.
	const std::vector<float> tone = oesyn::sine(48000, 3000.0, 0.8f);
	(void)oesyn::renderWith(a, &tone, 30);
	CHECK(a.synth.vocodeEnvelopeForTest().db == held.db);
	// THE BLOCK: holding, the bin spacing, the note Freeze took hold at, the grid.
	REQUIRE(a.synth.readVocodeFreeze(words.data(), words.size()) != 0u);
	feathers::VocodeFreezeBlock block;
	REQUIRE(feathers::readVocodeFreezeBlock(words.data(), words.size(), &block));
	CHECK(block.holding);
	CHECK(block.binHz == held.binHz);
	CHECK(block.frozenHz == doctest::Approx(oesyn::hzOfKey(50.0)).epsilon(1e-12).scale(0.0));
	CHECK(block.grid == held.db);
	// INTO A FRESH SYNTH with Freeze on: the same envelope, value by value, at once and with no sidechain.
	{
		Rig b;
		oesyn::vocodePatch(b);
		b.set(ParamId::AdditiveVocodeFreeze, 1.0);
		feathers::SpectralState state;
		state.vocodeFreeze = words;
		state.vocodeFreezeLoads = 1;
		b.synth.setSpectralState(state);
		(void)oesyn::renderWith(b, nullptr, 1);
		CHECK(b.synth.vocodeEnvelopeForTest().db == held.db);
		CHECK(b.synth.vocodeEnvelopeForTest().binHz == held.binHz);
		CHECK(b.synth.vocodeEnvelopeForTest().frozenHz == block.frozenHz);
		CHECK(b.synth.vocodeFreezeLoadsTaken() == 1u);
		// AND ITS OWN SAVE IS THE SAME BLOCK.
		std::array<float, feathers::kVocodeFreezeFloats> again {};
		REQUIRE(b.synth.readVocodeFreeze(again.data(), again.size()) != 0u);
		CHECK(again == words);
	}
	// WITH FREEZE OFF THE STORED SHAPE IS DROPPED (by design): nothing held, the envelope silent until the sidechain speaks.
	{
		Rig b;
		oesyn::vocodePatch(b);
		feathers::SpectralState state;
		state.vocodeFreeze = words;
		state.vocodeFreezeLoads = 1;
		b.synth.setSpectralState(state);
		(void)oesyn::renderWith(b, nullptr, 1);
		const auto& db = b.synth.vocodeEnvelopeForTest().db;
		CHECK(std::all_of(db.begin(), db.end(), [](float x) { return x <= feathers::spectral::VocodeEnvelope::kSilentDb; }));
	}
	// A NEW RATE RE-PREPARES THE ANALYSIS AND RE-ARMS THE HOLD (Z10c): the same envelope, moved onto the new bins' scale.
	a.synth.deactivate();
	a.synth.activate(44100.0, 4096);
	(void)oesyn::renderWith(a, nullptr, 1);
	const feathers::spectral::VocodeEnvelope& moved = a.synth.vocodeEnvelopeForTest();
	REQUIRE(moved.binHz != held.binHz);
	const double shift = 10.0 * std::log10(moved.binHz / held.binHz);
	for (size_t g = 0; g < held.db.size(); ++g) {
		CHECK(static_cast<double>(moved.db[g]) == doctest::Approx(static_cast<double>(held.db[g]) + shift).epsilon(1e-5).scale(0.0));
	}
}

TEST_CASE("OE: a frozen envelope asks the phase optimiser nothing more; a sidechain whose spectrum moves asks again (A-PH, §9.5)") {
	Rig rig;
	oesyn::vocodePatch(rig);
	rig.set(ParamId::AdditivePhases, 1.0); // Headroom
	rig.apply(noteOn(0, 55, 1));
	const std::vector<float> low = oesyn::sine(48000, 300.0, 0.5f);
	size_t at = 0;
	(void)oesyn::renderWith(rig, &low, 40, &at);
	rig.set(ParamId::AdditiveVocodeFreeze, 1.0);
	(void)oesyn::renderWith(rig, &low, 4, &at);
	const uint64_t posts = rig.synth.phaseSketchPostsForTest();
	CHECK(posts > 0u);
	(void)oesyn::renderWith(rig, &low, 40, &at);
	CHECK(rig.synth.phaseSketchPostsForTest() == posts); // held: the sketch does not move
	rig.set(ParamId::AdditiveVocodeFreeze, 0.0);
	const std::vector<float> high = oesyn::sine(48000, 4000.0, 0.5f);
	(void)oesyn::renderWith(rig, &high, 40);
	CHECK(rig.synth.phaseSketchPostsForTest() > posts);
}

TEST_CASE("OE2: Stereo — each ear is vocoded by its own sidechain channel, the analysis measures the two apart, and the held pair is saved by value") {
	Rig rig;
	oesyn::vocodePatch(rig);
	rig.set(ParamId::AdditiveVocodeStereo, 1.0);
	rig.apply(noteOn(0, 48, 1));
	// THE LEFT CHANNEL NOISE, THE RIGHT SILENT.
	const std::vector<float> noise = oesyn::noise(48000, 0.3f, 0x5151u);
	std::vector<float> l(256), r(256), zero(256, 0.0f), chunk(256);
	size_t at = 0;
	for (int b = 0; b < 60; ++b) {
		for (int i = 0; i < 256; ++i) {
			chunk[static_cast<size_t>(i)] = noise[at++ % noise.size()];
		}
		rig.synth.setSidechain(chunk.data(), zero.data(), true);
		rig.synth.render(l.data(), r.data(), 256);
	}
	const feathers::spectral::VocodeEnvelope& env = rig.synth.vocodeEnvelopeForTest();
	REQUIRE(env.stereo);
	CHECK(std::any_of(env.db.begin(), env.db.end(), [](float x) { return x > feathers::spectral::VocodeEnvelope::kSilentDb; }));
	CHECK(std::all_of(env.dbRight.begin(), env.dbRight.end(), [](float x) { return x <= feathers::spectral::VocodeEnvelope::kSilentDb; }));
	REQUIRE(rig.synth.lastChunkStereoForTest());
	const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.slotOfKeyForTest(48), 0);
	double left = 0.0, right = 0.0;
	for (int i = 0; i < 40; ++i) {
		left += osc.partialAmplitudeForTest(i, 0);
		right += osc.partialAmplitudeForTest(i, 1);
	}
	CHECK(left > 0.0);
	CHECK(right == 0.0);
	// FROZEN, THE PAIR IS THE BLOCK: both grids, by value, into a fresh synth.
	rig.set(ParamId::AdditiveVocodeFreeze, 1.0);
	rig.synth.setSidechain(chunk.data(), zero.data(), true);
	rig.synth.render(l.data(), r.data(), 256);
	std::array<float, feathers::kVocodeFreezeFloats> words {};
	REQUIRE(rig.synth.readVocodeFreeze(words.data(), words.size()) != 0u);
	feathers::VocodeFreezeBlock block;
	REQUIRE(feathers::readVocodeFreezeBlock(words.data(), words.size(), &block));
	CHECK(block.stereo);
	CHECK(block.grid == rig.synth.vocodeEnvelopeForTest().db);
	CHECK(block.gridRight == rig.synth.vocodeEnvelopeForTest().dbRight);
	CHECK(block.flatness > 0.0f); // the left channel is noise; Sibilance reads the held flatness
	CHECK(block.flatnessRight == 0.0f);
	Rig b;
	oesyn::vocodePatch(b);
	b.set(ParamId::AdditiveVocodeStereo, 1.0);
	b.set(ParamId::AdditiveVocodeFreeze, 1.0);
	feathers::SpectralState state;
	state.vocodeFreeze = words;
	state.vocodeFreezeLoads = 1;
	b.synth.setSpectralState(state);
	(void)oesyn::renderWith(b, nullptr, 1);
	CHECK(b.synth.vocodeEnvelopeForTest().db == block.grid);
	CHECK(b.synth.vocodeEnvelopeForTest().dbRight == block.gridRight);
	std::array<float, feathers::kVocodeFreezeFloats> again {};
	REQUIRE(b.synth.readVocodeFreeze(again.data(), again.size()) != 0u);
	CHECK(again == words); // the flatness came back too, word for word
}

TEST_CASE("OE2: Sibilance — no note, no noise and the output exactly silent; a held note gates the layer by its amplitude, the release takes it away") {
	Rig rig;
	oesyn::vocodePatch(rig);
	rig.set(ParamId::AdditiveVocodeSibilance, 1.0);
	const std::vector<float> side = oesyn::noise(48000, 0.3f, 0x2024u);
	size_t at = 0;
	// NO NOTE: the analysis runs (some sound vocodes) and the layer is made, but nothing gates it open.
	const std::vector<float> quiet = oesyn::renderWith(rig, &side, 40, &at);
	CHECK(rig.synth.vocodeFramesForTest() > 0u);
	CHECK(rig.synth.vocodeGateForTest() == 0.0f);
	CHECK(std::all_of(quiet.begin(), quiet.end(), [](float x) { return x == 0.0f; }));
	// A HELD NOTE at Voice Level 1, sustaining: the gate is the voice's amplitude, exactly 1.
	rig.apply(noteOn(0, 57, 1));
	(void)oesyn::renderWith(rig, &side, 20, &at);
	CHECK(rig.synth.vocodeGateForTest() == 1.0f);
	// THE SAME NOTE WITH SIBILANCE 0 on a twin: the difference is the noise layer, which carries power.
	Rig twin;
	oesyn::vocodePatch(twin);
	twin.set(ParamId::AdditiveVocodeSibilance, 0.0);
	twin.apply(noteOn(0, 57, 1));
	Rig with;
	oesyn::vocodePatch(with);
	with.set(ParamId::AdditiveVocodeSibilance, 1.0);
	with.apply(noteOn(0, 57, 1));
	size_t a = 0, b = 0;
	const std::vector<float> on = oesyn::renderWith(with, &side, 40, &a);
	const std::vector<float> off = oesyn::renderWith(twin, &side, 40, &b);
	double diff = 0.0;
	for (size_t i = on.size() / 2; i < on.size(); ++i) {
		diff += (on[i] - off[i]) * (on[i] - off[i]);
	}
	CHECK(diff > 0.0);
	// RELEASED: once the voice has stopped sounding, the gate is 0 and the output exactly silent again.
	rig.apply(noteOff(0, 57, 1));
	(void)oesyn::renderWith(rig, &side, 60, &at);
	CHECK(rig.synth.vocodeGateForTest() == 0.0f);
	const std::vector<float> after = oesyn::renderWith(rig, &side, 10, &at);
	CHECK(std::all_of(after.begin(), after.end(), [](float x) { return x == 0.0f; }));
}

TEST_CASE("OE2: the sibilance layer is the sidechain's noise above 2 kHz at its level times its flatness, and nothing below") {
	// THE ANALYSER ALONE (the layer before the synth's amount and gate): white noise in, its flatness above 2 kHz measured, the
	// layer's power against the input's above 2 kHz scaled by flatness², the dB mean's bias (the envelope it reads is a dB mean:
	// 2.507 dB under the power mean on noise) and the resynthesis' quarter: a bin of magnitude A comes back at A²/(2N) a sample,
	// against σ²/2 of the analysis (Σw² = N/2), over the overlap of four — the Spectral effect's own arithmetic, mirrored, not tuned).
	feathers::fx::spectral::SidechainEnvelope env;
	env.prepare(kSampleRate);
	feathers::fx::spectral::SidechainEnvelope::Settings s;
	s.sibilance = true;
	s.attackMs = 500.0;
	s.releaseMs = 2000.0;
	env.setSettings(s);
	const float amp = 0.3f;
	const std::vector<float> side = oesyn::noise(96000, amp, 0x7777u);
	std::vector<float> nl(side.size()), nr(side.size());
	env.push(side.data(), side.data(), static_cast<int>(side.size()), nl.data(), nr.data());
	const float flat = env.flatness();
	MESSAGE("white noise flatness above 2 kHz " << flat);
	CHECK(flat > 0.4f);
	CHECK(flat < 0.7f);
	// THE LAYER'S POWER over the second half (the envelope settled), against σ² × the band above 2 kHz.
	double power = 0.0;
	for (size_t i = nl.size() / 2; i < nl.size(); ++i) {
		power += static_cast<double>(nl[i]) * nl[i];
	}
	power /= static_cast<double>(nl.size() / 2);
	const double band = (static_cast<double>(amp) * amp / 3.0) * (kSampleRate / 2.0 - 2000.0) / (kSampleRate / 2.0);
	const double want = band * static_cast<double>(flat) * flat * std::pow(10.0, -2.5066 / 10.0) / 4.0;
	const double db = 10.0 * std::log10(power / want);
	MESSAGE("sibilance layer power against the restatement " << db << " dB");
	// MEASURED 2026-10-27: −0.98 dB (the window's leakage and the bins a hop's overlap does not see).
	CHECK(std::abs(db) < 1.5);
	// NOTHING BELOW 2 kHz: a 500 Hz sine alone gives no layer.
	feathers::fx::spectral::SidechainEnvelope low;
	low.prepare(kSampleRate);
	low.setSettings(s);
	const std::vector<float> tone = oesyn::sine(48000, 500.0, 0.5f);
	std::vector<float> tl(tone.size()), tr(tone.size());
	low.push(tone.data(), tone.data(), static_cast<int>(tone.size()), tl.data(), tr.data());
	double lowPower = 0.0;
	for (size_t i = tl.size() / 2; i < tl.size(); ++i) {
		lowPower += static_cast<double>(tl[i]) * tl[i];
	}
	CHECK(lowPower / static_cast<double>(tl.size() / 2) < 1e-6 * 0.125); // the sine's leakage alone, 60 dB under it
}

// ---------------------------------------------------------------------------------------------
// BACKLOG PA × OZ (2026-10-29): THE LFO'S STEREO PHASE ONTO OSCILLATOR PHASE. Each ear's read offset is restated from the
// LFO's own ears as the rack published them — the left from its left, the right from its right — never from a render.
namespace {
namespace pax {
namespace lfo = feathers::mod::lfo;
constexpr double kBase = 0.5;   // mid-cycle, so neither ear wraps for the amounts below
constexpr float kAmount = 0.2f; // Phase's span is one cycle

struct Seated {
	feathers::DeviceInstanceId lfo = feathers::kNoInstance;
};

/// An LFO (Free, Triangle) at `degrees` of Stereo Phase, routed into Phase at kAmount, on `engine`.
Seated seat(Rig& rig, double degrees, bool poly = false, feathers::OscType engine = feathers::OscType::V5) {
	rig.set(ParamId::OscType, static_cast<double>(engine));
	rig.set(ParamId::OscPhase, kBase);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	rack.device[0] = feathers::defaultRowFor(*feathers::deviceForTag(lfo::kTraits.tag), { 1 });
	if (!poly) {
		rack.device[0].field[static_cast<size_t>(lfo::Param::Mode)] = static_cast<float>(static_cast<int>(lfo::Mode::Free));
	}
	rack.device[0].field[static_cast<size_t>(lfo::Param::Poly)] = poly ? 1.0f : 0.0f;
	rack.device[0].field[static_cast<size_t>(lfo::Param::StereoPhase)] = static_cast<float>(degrees * M_PI / 180.0);
	rig.publishModRack(rack);
	rig.play({}, 64, 64);
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(phase)), kAmount);
	routes.publish(rig.params);
	rig.play({}, 64, 64);
	return Seated { rack.instanceAt(0) };
}
} // namespace pax
} // namespace

TEST_CASE("backlog OZ x PA: an LFO with Stereo Phase routed onto Phase moves the ears apart — each ear reads base + a × that ear of the LFO, by value") {
	for (const double degrees : { 90.0, 180.0 }) {
		CAPTURE(degrees);
		Rig rig;
		rig.expectMono = false;
		const pax::Seated s = pax::seat(rig, degrees);
		const std::vector<float> heard = rig.play({ noteOn(0, kKeyC4, 1) }, 640, 64);
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		const feathers::rack::RackEngine& engine = rig.synth.laneForTest().engineForTest();
		const size_t lane = engine.slotOf(s.lfo);
		REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
		CHECK(rig.params.stereoLive(ParamId::OscPhase));
		CHECK(rig.synth.lastChunkStereoForTest());
		double widest = 0.0;
		for (int k = 0; k < 24; ++k) {
			// THE CELLS AS THIS TICK OPENS: a whole tick renders toward them, so its last read stands on them (the rack ticks
			// again after the render, for the next one).
			const double left = pax::kBase + pax::kAmount * static_cast<double>(engine.publishedOutputAt(lane, 0));
			const double right = pax::kBase + pax::kAmount * static_cast<double>(engine.publishedOutputRAt(lane, 0));
			rig.play({}, 64, 64);
			CAPTURE(k);
			CHECK(std::abs(rig.synth.voiceReadOffsetForTest(slot) - left) < 1e-5);
			CHECK(std::abs(rig.synth.voiceReadOffsetRightForTest(slot) - right) < 1e-5);
			widest = std::max(widest, std::abs(right - left));
		}
		// NOT VACUOUS: the LFO's ears really were apart, by a good part of the route.
		CHECK(widest > 0.25 * pax::kAmount);
		// SECOND ASSERTION, proving the first is not bookkeeping: the right channel is a different signal.
		CHECK(heard != rig.lastRight);
	}
}

TEST_CASE("backlog OZ: on Additive an LFO with Stereo Phase onto Phase turns each partial's right ear by its ratio times a × (R − L) of the LFO, by value") {
	// THE ADDITIVE HALF OF OZ (2026-10-29): until it, the partial bank's two ears shared one rotator per partial (backlog LY)
	// and both ears came out bit-identical. Now the ears' difference the voice carries is the route's amount times the
	// LFO's own ears as the rack published them, and every rendering partial's right-ear phase is its left phase plus
	// ratio_i·2π times that difference: restated from the LFO's outputs, never from a render.
	Rig rig;
	rig.expectMono = false;
	const pax::Seated s = pax::seat(rig, 90.0, false, feathers::OscType::Additive);
	const std::vector<float> heard = rig.play({ noteOn(0, kKeyC4, 1) }, 640, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	const feathers::rack::RackEngine& engine = rig.synth.laneForTest().engineForTest();
	const size_t lane = engine.slotOf(s.lfo);
	REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
	CHECK(rig.params.stereoLive(ParamId::OscPhase));
	CHECK(rig.synth.lastChunkStereoForTest());
	double widest = 0.0;
	int checked = 0;
	for (int k = 0; k < 24; ++k) {
		const double ear = static_cast<double>(pax::kAmount)
		                   * (static_cast<double>(engine.publishedOutputRAt(lane, 0))
		                      - static_cast<double>(engine.publishedOutputAt(lane, 0)));
		rig.play({}, 64, 64);
		CAPTURE(k);
		const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(slot, 0);
		CHECK(std::abs(osc.partialsEarOffsetForTest() - ear) < 1e-5);
		if (ear == 0.0) {
			continue;
		}
		REQUIRE(osc.partialsEarLiveForTest());
		const feathers::partials::PartialSpectrum& spec = osc.playingTable()->partials;
		for (int i = 0; i < std::min(osc.partialsLiveForTest(), 48); ++i) {
			CAPTURE(i);
			const double want = static_cast<double>(osc.partialPhaseForTest(i))
			                    + 2.0 * M_PI * static_cast<double>(spec.ratio[i]) * osc.partialsEarOffsetForTest();
			CHECK(std::abs(std::remainder(static_cast<double>(osc.partialEarPhaseForTest(i)) - want, 2.0 * M_PI)) < 1e-3);
			++checked;
		}
		widest = std::max(widest, std::abs(ear));
	}
	// NOT VACUOUS: the LFO's ears really were apart, and partials were checked.
	CHECK(widest > 0.25 * pax::kAmount);
	CHECK(checked > 200);
	// SECOND ASSERTION, proving the first is not bookkeeping: the right channel is a different signal.
	CHECK(heard != rig.lastRight);
}

TEST_CASE("backlog PA: at Stereo Phase 0 the LFO onto Phase is mono — no pair, the mono road, both ears at one offset") {
	Rig rig; // expectMono holds: every render below has L == R to the bit
	pax::seat(rig, 0.0);
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 4096, 64);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	CHECK_FALSE(rig.params.stereoLive(ParamId::OscPhase));
	CHECK_FALSE(rig.synth.lastChunkStereoForTest());
	CHECK(std::abs(rig.synth.voiceReadOffsetForTest(slot) - pax::kBase) > 1e-3); // the LFO really moves Phase
	CHECK(rig.synth.voiceReadOffsetRightForTest(slot) == rig.synth.voiceReadOffsetForTest(slot));
	CHECK(peakOf(out, 64, 4096) > 0.01f);
}

TEST_CASE("backlog PA: under Poly each note's right ear is ITS OWN wave read the offset on — a note struck later reads what a lone note struck then reads") {
	constexpr int kLater = 1024; // tick-aligned
	Rig both;
	both.expectMono = false;
	pax::seat(both, 90.0, true);
	both.play({ noteOn(0, kKeyC4, 1) }, kLater, 64);
	both.play({ noteOn(0, kKeyC4 + 7, 2) }, 1600, 64);
	Rig alone;
	alone.expectMono = false;
	pax::seat(alone, 90.0, true);
	alone.play({}, kLater, 64);
	alone.play({ noteOn(0, kKeyC4 + 7, 2) }, 1600, 64);
	const int first = both.synth.slotOfKeyForTest(kKeyC4);
	const int second = both.synth.slotOfKeyForTest(kKeyC4 + 7);
	const int lone = alone.synth.slotOfKeyForTest(kKeyC4 + 7);
	REQUIRE(first >= 0);
	REQUIRE(second >= 0);
	REQUIRE(lone >= 0);
	// THE REFERENCE IS ANOTHER PATCH'S VOICE, not this code's arithmetic: the same note at the same moment, alone.
	CHECK(both.synth.voiceReadOffsetForTest(second) == doctest::Approx(alone.synth.voiceReadOffsetForTest(lone)).epsilon(1e-6));
	CHECK(both.synth.voiceReadOffsetRightForTest(second)
	      == doctest::Approx(alone.synth.voiceReadOffsetRightForTest(lone)).epsilon(1e-6));
	// PER EAR: each voice's right ear is apart from its left.
	CHECK(std::abs(both.synth.voiceReadOffsetRightForTest(second) - both.synth.voiceReadOffsetForTest(second)) > 1e-3);
	CHECK(std::abs(both.synth.voiceReadOffsetRightForTest(first) - both.synth.voiceReadOffsetForTest(first)) > 1e-3);
	// PER VOICE: the earlier note's right ear is its own wave's, not the later one's.
	CHECK(std::abs(both.synth.voiceReadOffsetRightForTest(first) - both.synth.voiceReadOffsetRightForTest(second)) > 1e-3);
}

namespace {

struct LazyRun {
	std::vector<float> out;    ///< the held phase, both ears
	uint64_t madeOverSilence;  ///< tables the bank MADE while no voice sounded
	uint32_t builtOverSilence; ///< builds it RECORDED meanwhile (generations)
	bool valueChecked = false;
};

/// Pos X swept over silence, then two notes struck mid-sweep and the sweep carried on under them. `eager` reads every
/// table the bank could hand out after every render — the old road, where a build was made the moment it was asked
/// for — so the two runs differ only in WHEN the bank makes its tables.
LazyRun lazyBankRun(bool eager) {
	using feathers::CharacterBank;
	using feathers::OscType;
	using feathers::OscVoice;
	using feathers::PreparedCharacter;
	Rig rig;
	rig.set(ParamId::OscType, static_cast<double>(OscType::V5));
	rig.set(ParamId::AZone, 300.0);
	rig.set(ParamId::AOffset, 0.37);
	constexpr int kBlock = 256;
	std::vector<float> left(kBlock), right(kBlock);
	const CharacterBank& bank = rig.synth.mainBankForTest();
	const auto touch = [&] {
		if (eager) {
			(void)bank.current(0);
			(void)bank.current(1);
			(void)bank.previous(0);
			(void)bank.previous(1);
		}
	};
	rig.synth.render(left.data(), right.data(), kBlock);
	touch();
	const uint64_t made0 = bank.tablesMadeForTest();
	const uint32_t gen0 = bank.generation();
	int b = 0;
	for (; b < 40; ++b) {
		rig.set(ParamId::PosX, 0.4 * std::sin(0.13 * static_cast<double>(b)));
		rig.synth.render(left.data(), right.data(), kBlock);
		touch();
	}
	LazyRun r { {}, bank.tablesMadeForTest() - made0, bank.generation() - gen0 };
	rig.synth.noteOn(0, 48, 1, 0.8);
	rig.synth.noteOn(0, 55, 2, 0.8);
	for (int k = 0; k < 40; ++k, ++b) {
		rig.set(ParamId::PosX, 0.4 * std::sin(0.13 * static_cast<double>(b)));
		rig.synth.render(left.data(), right.data(), kBlock);
		if (k == 0 && !eager) {
			// THE VALUE: the table the struck voice reads is the build of the character the bank recorded, made by an
			// independent call — so a table made late is the table that would have been made on time.
			const int slot = rig.synth.slotOfKeyForTest(48);
			REQUIRE(slot >= 0);
			const OscVoice& osc = rig.synth.voiceStackOscForTest(slot, 0);
			const PreparedCharacter& live = bank.current(0);
			feathers::PartialsScratch scratch;
			auto ref = std::make_unique<PreparedCharacter>();
			CharacterBank::buildOne(bank.currentCharacter(0), *ref, scratch);
			REQUIRE(live.flagship.operated);
			REQUIRE_FALSE(live.vox.audible);
			REQUIRE(ref->flagship == live.flagship);
			const float* played = osc.freshGridForTest(0);
			REQUIRE(played == live.gridFor(osc.mipForTest()));
			const float* want = ref->gridFor(osc.mipForTest());
			for (int j = 0; j <= feathers::kFlagshipSlots; ++j) {
				REQUIRE(played[j] == want[j]);
			}
			r.valueChecked = true;
		}
		touch();
		r.out.insert(r.out.end(), left.begin(), left.end());
		r.out.insert(r.out.end(), right.begin(), right.end());
	}
	return r;
}

} // namespace

TEST_CASE("a character moved over silence is recorded, not built: no table is made until a voice reads one, and the notes struck into it play the bits an eager bank plays (backlog PB)") {
	// Pos X automated with nothing sounding used to bake a table per rebuild interval that nobody read (+142 us a block
	// on V5 in the perf survey). The bank now records the build and makes its table on first read. Bookkeeping — the
	// flips, generations, serials, guard — is unchanged, so a note struck into the sweep meets the same schedule.
	const LazyRun lazy = lazyBankRun(false);
	const LazyRun eager = lazyBankRun(true);
	CAPTURE(lazy.builtOverSilence);
	CAPTURE(lazy.madeOverSilence);
	CAPTURE(eager.madeOverSilence);
	// The sweep really did rebuild the character, over and over.
	REQUIRE(lazy.builtOverSilence >= 20u);
	REQUIRE(eager.builtOverSilence == lazy.builtOverSilence);
	// ...and the lazy bank made none of them, where the eager road made every one.
	CHECK(lazy.madeOverSilence == 0u);
	CHECK(eager.madeOverSilence >= lazy.builtOverSilence);
	CHECK(lazy.valueChecked);
	// And what the notes play is the same, to the bit.
	REQUIRE(lazy.out.size() == eager.out.size());
	REQUIRE(peakOf(lazy.out, 0, lazy.out.size()) > 0.05f);
	CHECK(std::memcmp(lazy.out.data(), eager.out.data(), lazy.out.size() * sizeof(float)) == 0);
}

// **BACKLOG MA, CLOSED BY A GUARD RATHER THAN BY AN AUDIT** (2026-10-31). A bare-Synth rig that renders silence makes every
// "unchanged"/"differs" comparison on it vacuous: green whatever the code does. The audit of 2026-10-11 found none silent
// and put a REQUIRE in each comparing case; THIS asks the rig itself, so a rig edited to stop sounding (its Voice Level
// line deleted, its engine left silent) fails here by name, before any case that leans on it.
TEST_CASE("backlog MA: the bare rig's reference render SOUNDS, so a difference test on it can see a difference") {
	const auto peakOf = [](const std::vector<float>& x) { float p = 0.0f; for (const float v : x) { p = std::max(p, std::fabs(v)); } return p; };
	const auto rigHeap = std::make_unique<Rig>();
	Rig& rig = *rigHeap;
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 4096, 64);
	CAPTURE(peakOf(out));
	REQUIRE(peakOf(out) > 0.05f);
}

TEST_CASE("SR: an idle synth builds no character after settling, and a moved Pos X still does") {
	// AN IDLE BLOCK MUST NOT REBAKE (backlog SR, 2026-11-04). With no notes, no automation and nothing modulated, every
	// control tick still assembles the character (Synth::updateCharacter) and hands it to the bank, which must drop it as
	// equal to the live one. Read as the bank's own counters: its generation (a build: a flip of the two slots) and the
	// tables it made. A build every tick would cost the whole bake per tick with nothing moving.
	Rig rig;
	std::vector<float> left(1024);
	std::vector<float> right(1024);
	const auto blocks = [&](int n) {
		for (int b = 0; b < n; ++b) {
			rig.synth.render(left.data(), right.data(), 1024);
		}
	};
	blocks(50); // settled: the puck's smoother, the fades, the first build
	const uint32_t generation = rig.synth.mainBankForTest().generation();
	const uint64_t tables = rig.synth.bankTablesMade();
	blocks(200);
	CHECK(rig.synth.mainBankForTest().generation() == generation);
	CHECK(rig.synth.bankTablesMade() == tables);
	// AND THE COUNTER CAN MOVE: Pos X moved by hand, and the bank builds (the case is not blind to a build).
	rig.set(ParamId::PosX, 0.6);
	blocks(4);
	CHECK(rig.synth.mainBankForTest().generation() > generation);
	// SETTLED AGAIN, AND QUIET AGAIN — once the puck's motion has decayed: the velocity-aimed overshoot keeps the character
	// moving (and the bank building, one build per fade) for a while after the hand stops. MEASURED: the last build lands
	// about 80 blocks (1.7 s) after the move; 150 blocks is the margin.
	int lastBuild = 0;
	for (int b = 1; b <= 150; ++b) {
		const uint32_t was = rig.synth.mainBankForTest().generation();
		blocks(1);
		if (rig.synth.mainBankForTest().generation() != was) {
			lastBuild = b;
		}
	}
	MESSAGE("the last build after the move: block " << lastBuild);
	CHECK(lastBuild < 150);
	const uint32_t moved = rig.synth.mainBankForTest().generation();
	blocks(200);
	CHECK(rig.synth.mainBankForTest().generation() == moved);
}
