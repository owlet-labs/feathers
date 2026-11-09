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
#include "fx/delay/bbd_math.h"
#include "fx/delay/delay_taps.h"
#include "fx/delay/delay_types.h"
#include "fx/sklitter/ring.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace feathers::fx::delay {

/// **HOW MANY GRAINS MAY BE IN FLIGHT AT ONCE, AND THE NUMBER IS DERIVED RATHER THAN PICKED.**
/// Overlap is `density * grainSeconds`, so the worst case this device can be asked for is
/// `kDensityLog2Max` grains a second times `kGrainSizeLog2Max` seconds — 200 x 0.5 = 100. A pool of
/// 128 covers it with room, and the cost of the headroom is a swap-removed array that is only ever
/// walked over its ACTIVE prefix.
inline constexpr size_t kMaxGrains = 128;

//--------------------------------------------------------------------------------------------------//
// THE FEEDBACK LANE, AS THE DSP SEES IT.                                                             //
//--------------------------------------------------------------------------------------------------//
//
// **A HOOK, NOT A CHAIN.** `rack::InnerChain` lives above this library, so the DSP knows only a callback that
// processes a stretch of the loop's return in place and how late it leaves it. The holder (`delay_ops.cpp`)
// points it at the lane; a test points it at a gain or a plain delay and asserts the loop by value without a rack.
//
// **WHERE IT SITS (by design, Model B):** on what RE-ENTERS the line, after Tone and the tape's HF loss and before the
// Feedback gain — `Tone`'s contract, so the first repeat is untouched and repeat N has been through the lane N−1
// times. Then a DC blocker, the Feedback gain, the loop limiter, the safety clip, the Ping-Pong swap.
//
// **HOW A BLOCK-BASED CHAIN RUNS INSIDE A SAMPLE LOOP: WRITE THE INPUT NOW, ADD THE FEEDBACK LATER.** While the lane
// is live each sample writes only its input into the line and hands its loop return to a chunk buffer; at the
// chunk's end (`kLoopChunkMax` at most) the lane runs over the chunk and each of its outputs is added into the line
// at the position it belongs to — `latency` samples further back than the sample that produced it, which is the
// compensation: every repeat still lands exactly one tap apart, to the sample, in every mode, Granular included.
// The chunk is cut short so the loop's own feed read never reaches a position whose feedback has not landed yet
// (`chunkFor`). The cost, stated rather than hidden: a HEARD read nearer the write head than `latency + chunk`
// hears that slice without its newest feedback — only a Multi-Tap tap that close, or the first moment of a Reverse
// chunk, where its window is near zero.
//
// **CHUNKS ARE CUT ON THE SAMPLE STREAM, NOT ON THE HOST'S BLOCKS** — one open at a block's end carries into the next.
// MEASURED: flushed at every block end instead, a Multi-Tap at the floor rendered different bits at host blocks of
// 1, 7, 64 and 300, because which near reads saw a slice finished depended on where the host cut. The price is that a
// carried chunk's lane call runs in the next block (its inner clock up to one chunk late).
//
// **A NULL `process` IS NO LANE**, and then the sample loop is exactly what it was before the lane existed, to the
// bit (the MB/MD pinned hashes). The holder hands null while its lane holds no device, or only switched-off parked ones.
struct LoopInsert {
	void* context = nullptr;
	void (*process)(void* context, float* left, float* right, int frames) = nullptr;
	/// HOW LATE `process` LEAVES ITS OUTPUT, samples — read by the holder after the lane's drain.
	uint32_t latency = 0;
};

/// THE LONGEST STRETCH THE LANE IS HANDED AT ONCE — the device tick — in every mode but Buckets.
inline constexpr int kLoopChunkMax = 64;
/// **AND IN BUCKETS, FOUR TICKS** (backlog PV: buckets: yes optimize): fewer lane calls and
/// flushes. `InnerChain::process` cuts its own work on the inner tick (trap T4), so the lane's devices still step on
/// the 64-frame grid; `chunkFor` still cuts the chunk short wherever Time's floor needs it, so the feedback still lands
/// one tap apart to the sample ("PV: ..." in `test_delay_lane`). Buckets only: its heard read IS its fed read, so no
/// near heard read (a Multi-Tap tap, a Reverse window's start, a grain) waits longer for its newest feedback.
inline constexpr int kLoopChunkWide = 256;
/// **TIME'S FLOOR WHILE THE LANE IS LIVE IS ITS LATENCY PLUS THIS**: the shortest chunk the loop can run
/// at, so the head can never sit where no chunk is valid.
inline constexpr int kLoopChunkMin = 16;
/// THE LOOP'S DC BLOCKER, Hz (§1.6) — live only with the lane.
inline constexpr double kLoopDcHz = 5.0;
/// THE LOOP LIMITER (§1.6): a ceiling and two times — PROVISIONAL, feel numbers for the ear.
inline constexpr float kLoopLimitCeiling = 1.0f;
inline constexpr double kLoopLimitAttackSeconds = 0.0005;
inline constexpr double kLoopLimitReleaseSeconds = 0.150;

/// **THE DELAY, WITH THE GRANULATOR IN ITS LOOP.** The topology and what is deliberately missing are
/// in `delay_types.h`; this is how it runs.
class Delay {
public:
	/// ALLOCATE. Activate-time only — `AudioOps::prepare` is the one moment this device may, and
	/// `processStereo` must never grow anything.
	///
	/// **THE LINE IS SIZED FROM `kMaxDelaySeconds` AND NOT FROM THE SETTINGS**, because a setting can
	/// move on the audio thread and an allocation cannot follow it there. That is the whole reason
	/// the tap is clamped rather than the buffer resized, and `kMaxDelaySeconds` documents what the
	/// clamp costs.
	void prepare(double sampleRate);

	/// **AS IF JUST SWITCHED ON, not merely with the memory cleared** — `registry.h`'s contract, and
	/// the difference is audible: a device that forgets its line but remembers it was running ramps
	/// its filters in from the last note's state. So the line, the filters, the grains, the spawn
	/// accumulator AND the random stream all go back to where they were at construction.
	void reset() noexcept;

	void setTarget(const Settings& settings) noexcept;
	[[nodiscard]] const Settings& target() const noexcept { return target_; }

	/// HELD, NOT ACTED ON — `automod`'s pattern. The tap is re-derived per block in `processStereo`
	/// from whatever this holds; a tempo that does not change the tap changes nothing.
	void setTransportClock(const device::TransportClock& clock) noexcept;

	/// **THE BUS NOTE (S6b), FOR `Sync = Note`**: latched only while a key is held, so the tap keeps the last note's period
	/// after the release (the Phaser's key-track rule). Which note while several are held is the bus note's answer: the
	/// last key still held, or an upstream Tracker's choice (`RackEngine::busNoteForTest`). Forgotten at `reset`.
	void setNoteContext(const device::NoteContext& note) noexcept {
		if (note.gate && std::isfinite(note.pitchSemitones)) {
			notePitch_ = note.pitchSemitones;
		}
	}
	[[nodiscard]] float notePitchForTest() const noexcept { return notePitch_; }
	[[nodiscard]] const device::TransportClock& clock() const noexcept { return clock_; }

	void processStereo(float* left, float* right, int frames) noexcept;

	/// **DECLARED RATHER THAN LEFT TO THE NULL-RIGHT ACCIDENT.** `registry.h` records that HARM
	/// tolerates a null right by chance while `MultimodeFilter` would crash on the same call, so a
	/// mono road is stated or it does not exist. This one runs the stereo path over one buffer.
	void processMono(float* mono, int frames) noexcept;

	/// WHERE THE TAPS STRIP'S READINGS GO, once per process call (`delay_taps.h`); a null `publish`
	/// publishes nothing.
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }

	/// **THE FEEDBACK LANE FOR THE NEXT BLOCK** — audio thread, before `processStereo`; see `LoopInsert`. Held until
	/// replaced, so a holder hands it every block (null `process` for no lane).
	void setLoopInsert(const LoopInsert& insert) noexcept { insert_ = insert; }
	/// WHETHER ANYTHING HANDED TO THE LANE, OR WRITTEN INTO THE LINE NOT YET FINISHED BY IT, IS STILL TO COME — the
	/// part of `holdsTail` the lane adds beside the line's own quiet run. The lane's own ringing is the holder's to OR.
	[[nodiscard]] bool laneHoldsTail() const noexcept {
		return insert_.process != nullptr && laneQuiet_ < static_cast<size_t>(insert_.latency) + static_cast<size_t>(laneCap_);
	}

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS.                                                                                    //
	//----------------------------------------------------------------------------------------------//
	//
	// **A CLAIM ABOUT A VALUE CANNOT BE INFERRED FROM A RENDERED BUFFER.** CLAUDE.md's rule, found
	// independently by two sessions in one week and then twice more: a comparison between two renders
	// is evidence that SOMETHING changed, never that the right thing did. A delay's tap is exactly
	// this shape — a buffer carries every quantity at once and no amount of cleverness recovers "the
	// tap was 0.25 s at this instant" from it. Five lines each, per the rule that the remedy is cheap
	// and the cleverness is not.

	/// THE TAP THE DEVICE IS ACTUALLY RUNNING, in seconds, after the clamp. Not what the controls ask
	/// for — what came out the other end of `tapSeconds` and `kMaxDelaySeconds`.
	[[nodiscard]] double tapSecondsForTest() const noexcept { return lastTapSeconds_; }

	/// THE RIGHT CHANNEL'S TAP, which Spread moves and the left channel's does not.
	[[nodiscard]] double tapSecondsRightForTest() const noexcept { return lastTapSecondsRight_; }

	/// HOW MANY GRAINS ARE IN FLIGHT. Zero in Clean mode and at the Grain Size OFF position, which is
	/// the only way to tell those two apart from outside.
	[[nodiscard]] size_t activeGrainsForTest() const noexcept { return activeGrains_; }

	/// HAS THE LINE REACHED EXACT SILENCE — see `kSilenceFloor`. The flag the tail case reads, so it
	/// asserts the mechanism rather than only the samples it produces.
	[[nodiscard]] bool silentForTest() const noexcept { return quietRun_ >= line_.length(); }
	/// WHETHER THE LINE STILL HOLDS A REPEAT TO PLAY — `AudioOps::holdsTail` (backlog MC). Off, nothing is played.
	[[nodiscard]] bool holdsTail() const noexcept { return target_.enabled && (!silentForTest() || laneHoldsTail()); }

	/// THE LOOP LIMITER'S GAIN ON THE LAST SAMPLE IT RAN (1 = not limiting), and how many lane samples the guard has
	/// replaced for not being finite since `prepare` — the §1.6 seams.
	[[nodiscard]] float loopLimitGainForTest() const noexcept { return limitGain_; }
	[[nodiscard]] uint64_t loopResetsForTest() const noexcept { return loopResets_; }
	[[nodiscard]] float lanePassForTest() const noexcept { return lanePass_; }
	/// S4: the freeze's ramp (0 open, 1 holding), the tap it holds at in frames, the duck's gain on the last sample, and
	/// how many clears have emptied the line.
	[[nodiscard]] float freezeForTest() const noexcept { return freezeNow_; }
	[[nodiscard]] size_t frozenTapForTest(int channel) const noexcept { return channel == 0 ? frozenL_ : frozenR_; }
	[[nodiscard]] float duckGainForTest() const noexcept { return lastDuckGain_; }
	[[nodiscard]] uint64_t clearsForTest() const noexcept { return clears_; }
	/// BBD (2026-11-05): the clock each channel's chip runs at for its head, and the corner its filters sit at, Hz.
	[[nodiscard]] double bbdClockForTest(int channel) const noexcept { return bbdClockHz_[channel == 0 ? 0 : 1]; }
	[[nodiscard]] double bbdCutoffForTest(int channel) const noexcept { return bbdCutoffHz_[channel == 0 ? 0 : 1]; }
	/// THE CLOCK'S PHASE (cycles) and the whine phasor's sine after the last frame (backlog PV): the phasor must be
	/// sin(2π·phase), whatever the blocks were.
	[[nodiscard]] double bbdPhaseForTest(int channel) const noexcept { return bbdPhase_[channel == 0 ? 0 : 1]; }
	[[nodiscard]] double bbdWhineForTest(int channel) const noexcept { return bbdWhine_[channel == 0 ? 0 : 1].s; }
	/// SWARM: the draws of the FIRST grains spawned since reset (offset frames, pan -1..1, semitones), at most 1024; and how
	/// many grains Swarm has drawn for in all.
	[[nodiscard]] size_t swarmDrawCountForTest() const noexcept { return swarmDrawCount_; }
	[[nodiscard]] uint64_t swarmSerialForTest() const noexcept { return swarmSerial_; }
	[[nodiscard]] double swarmOffsetForTest(size_t i) const noexcept { return swarmDraws_[i % kSwarmDraws].offsetFrames; }
	[[nodiscard]] float swarmPanForTest(size_t i) const noexcept { return swarmDraws_[i % kSwarmDraws].pan; }
	[[nodiscard]] float swarmSemitonesForTest(size_t i) const noexcept { return swarmDraws_[i % kSwarmDraws].semitones; }
	/// GRANULAR ON A PLAYING TRANSPORT: how many grains were spawned on the song's grid, and the index of the last — the song
	/// frame it was born at divided by the spawn period, floored (`processStereo`). -1 before the first.
	[[nodiscard]] uint64_t songSpawnsForTest() const noexcept { return songSpawns_; }
	[[nodiscard]] int64_t songLastIndexForTest() const noexcept { return songLastIndex_; }
	/// THE SPRAY'S UNIT DRAW (-1..1) of the FIRST grains spawned since reset, at any Swarm, at most 1024.
	[[nodiscard]] size_t sprayDrawCountForTest() const noexcept { return sprayDrawCount_; }
	[[nodiscard]] double sprayDrawForTest(size_t i) const noexcept { return sprayDraws_[i % kSwarmDraws]; }
	/// HOW MANY CHUNKS THE LANE HAS BEEN HANDED since `prepare`, and the longest — the chunking's own seams.
	[[nodiscard]] uint64_t laneChunksForTest() const noexcept { return laneChunks_; }
	[[nodiscard]] int laneChunkMaxForTest() const noexcept { return laneChunkMaxSeen_; }

	[[nodiscard]] double sampleRateForTest() const noexcept { return sampleRate_; }

	/// **WHERE THE LAST SAMPLE ACTUALLY READ**, in seconds, channel 0 left and 1 right — the PLACED head
	/// with the tape's wobble on it, not the target `tapSecondsForTest` reports. In Granular, where the
	/// grains spawn: the left head, and the right one the spread behind it. Read off the members the sample
	/// loop wrote, not off the readings, so a case comparing the two compares two roads.
	[[nodiscard]] double headSecondsForTest(int channel) const noexcept {
		return (channel == 0 ? lastHeadL_ : lastHeadR_) / sampleRate_;
	}
	/// THE MULTI-TAP'S GEOMETRY AS THE LAST BLOCK RAN IT — the fraction of the head tap `k` sits at, and
	/// its gain in one channel with the power normalisation applied. One tap at 1 and unity outside Multi-Tap.
	[[nodiscard]] int tapCountForTest() const noexcept { return tapCount_; }
	[[nodiscard]] double tapFractionForTest(size_t k) const noexcept { return k < tapFrac_.size() ? tapFrac_[k] : 0.0; }
	[[nodiscard]] float tapGainForTest(int channel, size_t k) const noexcept {
		if (k >= tapGainL_.size()) {
			return 0.0f;
		}
		return (channel == 0 ? tapGainL_[k] : tapGainR_[k]) * tapNorm_;
	}
	/// ONE MULTI-TAP SLOT AS THE LAST SAMPLE LEFT IT (backlog MD), counted BACK FROM THE BASE TAP: slot 0 is the
	/// base, slot 1 the tap before it. A slot past `Taps` is one fading out. `fading` while it crossfades from
	/// `fromFraction` to `fraction`; its gain is the ramped one, before the power normalisation.
	[[nodiscard]] bool slotFadingForTest(size_t slot) const noexcept { return slot < slots_.size() && slots_[slot].fadePos < 1.0; }
	[[nodiscard]] double slotFractionForTest(size_t slot) const noexcept { return slot < slots_.size() ? slots_[slot].frac : 0.0; }
	[[nodiscard]] double slotFromFractionForTest(size_t slot) const noexcept {
		return slot < slots_.size() ? slots_[slot].fracFrom : 0.0;
	}
	[[nodiscard]] float slotGainForTest(size_t slot, int channel) const noexcept {
		return slot < slots_.size() ? slots_[slot].gain[channel == 0 ? 0 : 1].now : 0.0f;
	}
	/// THE MULTI-TAP'S FAST READ AND THE SHARED ONE AT ONE POSITION, [L, R] each — `readTap` claims `readLerp`'s
	/// answer to the bit, for any position 1 to `length` frames behind the write head (backlog MD).
	void readBothForTest(double absolutePos, float tap[2], float lerp[2]) const noexcept {
		readTap(absolutePos, &tap[0], &tap[1]);
		readLerp(absolutePos, &lerp[0], &lerp[1]);
	}
	[[nodiscard]] uint64_t writeCounterForTest() const noexcept { return writeCounter_; }
	[[nodiscard]] size_t lineLengthForTest() const noexcept { return line_.length(); }
	/// THE XFADE'S TWO WEIGHTS ON THE LAST SAMPLE — 1 and 0 when no fade is running.
	[[nodiscard]] float fadeNewForTest() const noexcept { return lastFadeNew_; }
	[[nodiscard]] float fadeOldForTest() const noexcept { return lastFadeOld_; }
	/// THE REVERSE'S CHUNK, in seconds, and its leading head's weight on the last sample — `sin^2` of its
	/// phase, the trailing head's being one minus it (`delay_types.h`). Zero chunk outside Reverse.
	[[nodiscard]] double reverseChunkSecondsForTest(int channel) const noexcept {
		return revChunk_[channel == 0 ? 0 : 1] / sampleRate_;
	}
	[[nodiscard]] double reverseWeightForTest(int channel) const noexcept { return revWeight_[channel == 0 ? 0 : 1]; }
	/// HOW FAR THE NEXT SAMPLE IS PAST THE LEADING HEAD'S MIRROR, in seconds, unwrapped — read off the member, so a
	/// case comparing it with the published phase compares two roads.
	[[nodiscard]] double reverseAgeSecondsForTest(int channel) const noexcept {
		return (static_cast<double>(writeCounter_) - revMirrorA_[channel == 0 ? 0 : 1]) / sampleRate_;
	}

private:
	/// ONE GRAIN IN FLIGHT.
	struct Grain {
		/// WHERE IT READS, in the ring's ABSOLUTE frame counter — not a ring index. The two agree
		/// (`writeCounter_ % length == writePos`) because the length never moves after `prepare`, and
		/// an absolute position is what lets a pitched grain drift without wrap arithmetic at every
		/// step.
		double readPos = 0.0;
		/// THE PITCH RATIO, as frames advanced per output frame.
		double inc = 1.0;
		double age = 0.0;
		double length = 1.0;
		/// SWARM'S PAN (2026-11-05): constant-power gains, unity in both at the centre — and never read with Swarm at 0.
		float panL = 1.0f;
		float panR = 1.0f;
		/// HOW FAR BEHIND THE WRITE HEAD IT READS, in frames — the same place as `readPos`, kept RELATIVE. Swarm reads by
		/// this (2026-11-05, song-locked): an absolute position's fraction is rounded at the magnitude of the write counter,
		/// so two instances that started writing at different moments would read the same grain a few ulps apart.
		double back = 1.0;
	};

	/// ONE FRAME OUT OF THE LINE, INTERPOLATED. A delay whose tap is modulated lands between samples,
	/// and the nearest-neighbour alternative steps audibly on every move.
	void readLerp(double absolutePos, float* left, float* right) const noexcept;
	/// THE SAME READ, FOR A POSITION 1 TO `length` FRAMES BEHIND THE WRITE HEAD — the multi-tap's (backlog MD).
	void readTap(double absolutePos, float* left, float* right) const noexcept;

	/// `songIndex` other than `kNoSongGrain` (negative is a count-in, and real): the grain is the song's grain of that index, and every draw it takes is a hash of it.
	void spawnGrain(double tapFrames, double sprayFrames, double pitchRatio, double grainFrames,
	                int64_t songIndex = kNoSongGrain) noexcept;
	static constexpr int64_t kNoSongGrain = std::numeric_limits<int64_t>::min();
	void renderGrains(double* outLeft, double* outRight) noexcept;

	/// XORSHIFT32, seeded at construction and RE-SEEDED BY `reset`. Determinism is not decoration
	/// here: `test_fx_devices.cpp` asserts that the same samples in a different partition are the
	/// same samples out, and a granulator drawing from a clock or from an unreset stream fails it for
	/// a reason that has nothing to do with the bug the case hunts.
	[[nodiscard]] float nextBipolar() noexcept;
	[[nodiscard]] float nextBbdNoise() noexcept;

	fx::sklitter::Ring line_ {};
	double sampleRate_ = 48000.0;

	/// **THE ABSOLUTE WRITE POSITION, which `Ring` does not keep.** `Ring::writePos()` wraps; a grain
	/// that drifts needs a monotonic one. They stay in step by construction rather than by
	/// maintenance — `setLength` is never called, so `writeCounter_ % length == writePos` from zero.
	uint64_t writeCounter_ = 0;

	Settings target_ {};
	float notePitch_ = kDefaultNotePitch;
	device::TransportClock clock_ {};

	/// THE TONE TILT'S STATE — one lowpass pole and one highpass pole per channel, both always run.
	/// The inactive one sits at a passthrough coefficient rather than being branched around, so the
	/// sample loop has one shape and a tone sweep through zero has no discontinuity at the crossing.
	float lpL_ = 0.0f;
	float lpR_ = 0.0f;
	float hpL_ = 0.0f;
	float hpR_ = 0.0f;

	std::array<Grain, kMaxGrains> grains_ {};
	/// THE ACTIVE PREFIX. Expired grains are swap-removed, so the per-sample walk is over the grains
	/// that exist rather than over the pool.
	size_t activeGrains_ = 0;
	double spawnAccum_ = 0.0;

	/// **NOT SAVED WITH THE PATCH, AND WHY** (backlog NM): the wow/flutter and grain draws come from a constant seed reset
	/// with the device, and the line is a tail. Even a Feedback-above-1 self-loop is AUDIO, which a patch does not carry
	/// unless it is added later (2026-10-18: no audio unless specifically added).
	uint32_t rng_ = kRngSeed;
	static constexpr uint32_t kRngSeed = 0x9e3779b9u;

	/// HOW LONG THE LINE HAS BEEN WRITING EXACT ZEROES. Once this reaches the buffer length every
	/// sample in it is zero, which is what makes the tail EXACTLY silent rather than merely small.
	size_t quietRun_ = 0;

	//----------------------------------------------------------------------------------------------//
	// WHERE THE READ HEAD IS, AS OPPOSED TO WHERE IT HAS BEEN ASKED TO BE.                           //
	//----------------------------------------------------------------------------------------------//
	//
	// **THE TAP USED TO BE A PER-BLOCK NUMBER READ STRAIGHT INTO `readLerp`, AND THAT IS A JUMP.**
	// Moving `Delay Time` one notch stepped the output by 5.5x its own sample-to-sample slope —
	// a click. These four members are the difference between asking for a new tap and arriving at
	// one.
	//
	// **NEGATIVE MEANS "NOT YET PLACED"**, so the first block after `prepare`/`reset` SNAPS to
	// whatever the controls say instead of gliding up from zero — a device that swooped into pitch
	// every time it was switched on.
	double tapNowL_ = -1.0;
	double tapNowR_ = -1.0;
	/// Where the OLD head sits while an `Xfade` is running; meaningless once `fadePos_` reaches 1.
	double tapFadeFromL_ = 0.0;
	double tapFadeFromR_ = 0.0;
	/// 0..1 through a crossfade; **1 means no fade is running**, which is also the resting state.
	double fadePos_ = 1.0;
	double fadeInc_ = 1.0;

	//----------------------------------------------------------------------------------------------//
	// THE TAPE MACHINE'S RUNNING STATE.                                                              //
	//----------------------------------------------------------------------------------------------//
	//
	// **FOUR PHASES, ADVANCED PER SAMPLE AND NEVER PER BLOCK** — two wow components and two flutter,
	// each pair at incommensurate rates so neither reads as an LFO. Per sample because the registry's
	// partition sweep demands it and because a transport does not know where the host cut the
	// buffer; reset with everything else, because "as if just switched on" includes where the
	// capstan was in its turn.
	double wowPhaseA_ = 0.0;
	double wowPhaseB_ = 0.0;
	double flutterPhaseA_ = 0.0;
	double flutterPhaseB_ = 0.0;
	/// THE PER-PASS HF LOSS'S POLE, one per channel, in the feedback path beside the Tone pair.
	float tapeLpL_ = 0.0f;
	float tapeLpR_ = 0.0f;

	double lastTapSeconds_ = 0.0;
	double lastTapSecondsRight_ = 0.0;
	/// HOW FAR APART THE TWO CHANNELS' TAPS ARE, in frames — resolved once per block and read by
	/// `renderGrains`, which is the only place that needs it and is called per sample.
	double lastSpreadFrames_ = 0.0;

	//----------------------------------------------------------------------------------------------//
	// WHAT THE TAPS STRIP IS TOLD (backlog LF).                                                      //
	//----------------------------------------------------------------------------------------------//
	//
	// The multi-tap geometry used to be locals of `processStereo`; it is resolved in the same place,
	// once per block, and kept so the block's end can publish what the block ran.
	std::array<double, kMaxTaps> tapFrac_ {};
	std::array<float, kMaxTaps> tapGainL_ {};
	std::array<float, kMaxTaps> tapGainR_ {};
	float tapNorm_ = 1.0f;
	int tapCount_ = 1;

	//----------------------------------------------------------------------------------------------//
	// THE MULTI-TAP'S SLOTS, AS OPPOSED TO THE GEOMETRY THE CONTROLS ASK FOR (backlog MD).           //
	//----------------------------------------------------------------------------------------------//
	//
	// The arrays above are what the controls ask for this block; these are where each tap IS. A tap that moves
	// crossfades from its old place to its new one, and its gain ramps, so neither Shape nor Taps steps the output.
	//
	// **SLOTS ARE COUNTED BACK FROM THE BASE TAP**, slot `N-1-k` for tap k of N, because the base tap is the one
	// that never moves: it sits AT the head whatever Shape says, and it feeds the loop. So a Taps step from four
	// to five leaves slot 0 where it was, slides 3/4, 2/4 and 1/4 to 4/5, 3/5 and 2/5, and fades a new tap in at
	// 1/5 — rather than moving the base tap and the loop with it.

	/// A VALUE THAT RAMPS LINEARLY to its target over `kTapFadeSeconds` and lands on it exactly. The ramp is
	/// restarted only when the target CHANGES, so splitting a block where nothing changed splits nothing.
	struct Ramp {
		float now = 0.0f;
		float target = 0.0f;
		float step = 0.0f;
		int left = 0;
		void retarget(float to, int frames) noexcept;
		void snap(float to) noexcept {
			now = to;
			target = to;
			step = 0.0f;
			left = 0;
		}
		void advance() noexcept {
			if (left > 0) {
				now = --left == 0 ? target : now + step;
			}
		}
	};
	struct Slot {
		double frac = 1.0;     ///< where it reads, as a fraction of the head — or where it is fading TO
		double fracFrom = 1.0; ///< where it read before, while a fade runs
		double fadePos = 1.0;  ///< 0..1 through the fade; **1 means none is running**
		double target = 1.0;   ///< where the controls last asked it to be; picked up when a fade ends
		std::array<Ramp, 2> gain {};
		/// NOTHING TO READ: no gain now or to come, and no fade. Such a slot is skipped, and SNAPS when it is
		/// next asked for, since moving a tap nobody hears cannot click.
		[[nodiscard]] bool silent() const noexcept {
			return gain[0].now == 0.0f && gain[1].now == 0.0f && gain[0].target == 0.0f && gain[1].target == 0.0f
			       && fadePos >= 1.0;
		}
	};
	std::array<Slot, kMaxTaps> slots_ {};
	Ramp tapNormRamp_ {};
	/// **NOT PLACED UNTIL A MULTI-TAP BLOCK RUNS**, and unplaced again by any other mode's block — Reverse's rule:
	/// entering Multi-Tap seats the taps where the controls say instead of fading in from a pattern left long ago.
	bool slotsPlaced_ = false;
	/// A FADE STARTS ONLY ON A REAL MOVE — half a frame at the head, `Xfade`'s threshold — or a control resting a
	/// hair off its last value would keep a tap crossfading with itself.
	void startSlotFade(Slot& slot, double headFrames) const noexcept;
	double tapFadeInc_ = 1.0;
	int tapFadeFrames_ = 1;
	/// THE HEADS AND THE FADE WEIGHTS THE LAST SAMPLE READ WITH, in frames — written per sample, two
	/// stores each, because the wobble and the glide move them per sample.
	double lastHeadL_ = 0.0;
	double lastHeadR_ = 0.0;
	float lastFadeNew_ = 1.0f;
	float lastFadeOld_ = 0.0f;
	device::ReadingsSink sink_ {};

	//----------------------------------------------------------------------------------------------//
	// THE REVERSE'S TWO HEADS (backlog MB), per channel.                                             //
	//----------------------------------------------------------------------------------------------//
	//
	// Each head is a MIRROR in the ring's absolute frame: it reads `2·mirror − now`. The leading head's
	// mirror advances by one chunk when its age reaches the chunk; the trailing head's is re-seated half a
	// chunk after the leading one's, at the instant the leading head passes its middle — so the two stay
	// complementary while a glide moves the chunk under them. `delay_types.h` has the why.
	//
	// **NOT PLACED UNTIL A REVERSE BLOCK RUNS**, and unplaced again by any other mode's block, so entering
	// Reverse seats the heads where the line is now rather than where they were when it was last left.
	double revMirrorA_[2] = { 0.0, 0.0 };
	double revMirrorB_[2] = { 0.0, 0.0 };
	bool revHalfDone_[2] = { false, false };
	bool revPlaced_ = false;
	/// THE CHUNK THE LAST SAMPLE RAN, frames, and the leading head's weight on it — the test seams'.
	double revChunk_[2] = { 0.0, 0.0 };
	float revWeight_[2] = { 0.0f, 0.0f };

	/// ONE CHANNEL OF THE REVERSE AT ABSOLUTE FRAME `now`: the two windowed heads over a chunk of `chunk`
	/// frames, summed.
	[[nodiscard]] float reverseRead(int channel, double chunk, double now) noexcept;

	//----------------------------------------------------------------------------------------------//
	// THE FEEDBACK LANE'S LOOP (see `LoopInsert`).                                                   //
	//----------------------------------------------------------------------------------------------//
	//----------------------------------------------------------------------------------------------//
	// THE DUCK, THE FREEZE, THE CLEAR (delay-love S4; `delay_types.h` has what each is).             //
	//----------------------------------------------------------------------------------------------//
	float duckEnv_ = 0.0f;
	float lastDuckGain_ = 1.0f;
	/// 0 open, 1 holding — ramped over `kFreezeRampSeconds`.
	float freezeNow_ = 0.0f;
	/// THE TAP THE HOLD RECIRCULATES AT, whole frames, latched as the freeze begins; 0 while not latched.
	size_t frozenL_ = 0;
	size_t frozenR_ = 0;
	bool clearPrev_ = false;
	bool clearPrimed_ = false;
	/// THE HEARD WET'S GAIN THROUGH A CLEAR: 1 normally, ramping to 0, and the line emptied at the next block.
	float clearFade_ = 1.0f;
	bool clearFading_ = false;
	uint64_t clears_ = 0;

	//----------------------------------------------------------------------------------------------//
	// THE BUCKET BRIGADE AND THE SWARM (2026-11-05; `delay_types.h` has what each is).              //
	//----------------------------------------------------------------------------------------------//
	struct Biquad {
		float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
	};
	/// Per channel: the corner's coefficients, and the two filters' states (anti-alias, reconstruction).
	std::array<Biquad, 2> bbdCoef_ {};
	std::array<std::array<float, 2>, 2> bbdAa_ {};
	std::array<std::array<float, 2>, 2> bbdRe_ {};
	std::array<double, 2> bbdPhase_ {};
	/// THE WHINE, sin(2π·bbdPhase_) as a rotating phasor re-anchored each block (backlog PV, `bbd_math.h`).
	std::array<WhineRotor, 2> bbdWhine_ {};
	std::array<float, 2> bbdHeld_ {};
	std::array<double, 2> bbdClockHz_ {};
	std::array<double, 2> bbdCutoffHz_ {};
	float bbdEnvIn_ = 0.0f;
	float bbdEnvOut_ = 0.0f;
	std::array<float, 2> bbdEnvCh_ {};
	uint32_t bbdRng_ = kRngSeed;
	/// THE CHIP'S INPUT CHAIN'S SETTINGS FOR ONE PASS — the saturation's drive, the compressor's follower and its amount.
	struct BbdRecord {
		float drive = 0.0f;
		float attack = 0.0f;
		float release = 0.0f;
		float compander = 0.0f;
	};
	/// THE CHIP RECORDS: saturate, compress (the follower linked across the pair), anti-alias — in place. ONE body for both
	/// roads: the plain loop runs it as it writes, the lane's flush runs it on input + return (`flushLane`).
	void bbdRecord(float& l, float& r, const BbdRecord& p) noexcept;
	/// WHAT THE OPEN CHUNK RECORDS WITH, taken at its first sample as `chunkDrive_` is: Buckets or not, and its settings.
	bool chunkBbd_ = false;
	BbdRecord chunkBbdRecord_ {};
	static float biquad(const Biquad& c, std::array<float, 2>& z, float x) noexcept;
	struct SwarmDraw {
		double offsetFrames = 0.0;
		float pan = 0.0f;
		float semitones = 0.0f;
	};
	static constexpr size_t kSwarmDraws = 1024;
	std::array<SwarmDraw, kSwarmDraws> swarmDraws_ {};
	size_t swarmDrawCount_ = 0;
	uint64_t swarmSerial_ = 0;
	float swarm_ = 0.0f;
	uint64_t songSpawns_ = 0;
	int64_t songLastIndex_ = -1;
	/// THIS BLOCK'S GRAINS ARE ON THE SONG'S GRID — `renderGrains` then takes the song-safe walk at any Swarm.
	bool grainsOnSong_ = false;
	std::array<double, kSwarmDraws> sprayDraws_ {};
	size_t sprayDrawCount_ = 0;
	/// A SWARM GRAIN'S READ AS A DISTANCE BEHIND THE WRITE HEAD (1..length-2, fractional), so what it reads is the same
	/// bits however long this instance has been writing — see `renderGrains`.
	void readBehind(double back, float* left, float* right) const noexcept;

	LoopInsert insert_ {};


	/// THE CHUNK IN FLIGHT: each sample's loop return (pre-swap, post-Tone), what the lane is handed.
	std::array<float, kLoopChunkWide> laneL_ {};
	std::array<float, kLoopChunkWide> laneR_ {};
	/// THE ONE-POLE DC BLOCKER AFTER THE LANE: last input and output per channel.
	float dcInL_ = 0.0f;
	float dcInR_ = 0.0f;
	float dcOutL_ = 0.0f;
	float dcOutR_ = 0.0f;
	/// THE LANE'S MEASURED GAIN PER PASS (delay-love S3): its output's energy over its input's, each smoothed over ~200 ms of
	/// chunks, held while nothing goes in; 1 without a lane. The strip's decay is drawn at Feedback times this.
	float lanePass_ = 1.0f;
	double laneInEnergy_ = 0.0;
	double laneOutEnergy_ = 0.0;
	/// THE LIMITER'S GAIN, stereo-linked.
	float limitGain_ = 1.0f;
	/// HOW LONG NOTHING HAS GONE INTO THE LANE OR THE LINE — `laneHoldsTail`.
	size_t laneQuiet_ = 0;
	uint64_t loopResets_ = 0;
	uint64_t laneChunks_ = 0;
	int laneChunkMaxSeen_ = 0;
	/// THE CHUNK FOR THE SAMPLE NOW, given the nearest feed head in frames: short enough that the feed never reads a
	/// position whose feedback has not landed (the head may glide half a frame a sample toward the write head).
	[[nodiscard]] int chunkFor(double nearestFeedFrames, int cap = kLoopChunkMax) const noexcept;
	/// THE CAP THE CHUNK NOW OPEN (OR LAST OPENED) WAS CUT UNDER — how long a quiet input can still be waiting in it.
	int laneCap_ = kLoopChunkMax;
	/// THE LANE OVER THE CHUNK JUST WRITTEN, and its output added into the line where it belongs.
	void flushLane(int n) noexcept;
	/// THE CHUNK IN FLIGHT, which may span host blocks: how far in, how long, and what it is flushed with — taken at
	/// its first sample, so a control that moves mid-chunk moves the next chunk, wherever the host cut.
	int chunkAt_ = 0;
	int chunkN_ = 0;
	uint32_t chunkLatency_ = 0;
	float chunkFeedback_ = 0.0f;
	bool chunkPingPong_ = false;
	bool chunkTape_ = false;
	float chunkDrive_ = 0.0f;
};

} // namespace feathers::fx::delay
