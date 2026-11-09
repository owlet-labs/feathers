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

#include "device/background.h"
#include "device/registry.h"
#include "device/state_blocks.h"
#include "dsp/slice_basis.h"
#include "mod/arp/genes.h"
#include "mod/arp/pattern.h"
#include "mod/arp/pattern_cycle.h"
#include "mod/arp/pattern_likes.h"
#include "mod/arp/pattern_warp.h"
#include "mod/arp/pattern_types.h"
#include "mod/arp/step_emitter.h"
#include "mod/trig.h"

#include <array>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

// THE PATTERN DEVICE AT PLAY. The shared `StepEmitter` keeps the clock, the
// held chord, the sounding table and `arp_play`; this class decides what a slot plays:
//
//   - WHICH PATTERN (design §7.3): the selector's band over the patterns in range, read at each step onset of the
//     playing pattern's grid — so a change lands on the next step and the notes sounding finish. A pattern with
//     another step length switches the grid at that onset, and its first step is its own next onset.
//   - WHAT IT PLAYS: the pattern's cycle (`pattern::build`), rebuilt at an onset only when one of its inputs moved
//     (design §10.2's measured cost: never per block).
//   - HOW: a note, a chord on one sample, a tie (nothing), a ratchet or a grace note (queued in the step), a slide
//     (its off lands just after the next on, so a Legato synth glides).

namespace feathers::mod::arp::pat {

/// **MODULATION'S LANE LEVELS** (design §7.10): per step, each Drum lane read as a level — where the lane hits, that hit's
/// velocity; where it does not, the level HOLDS from its last hit, wrapping round the cycle so a loop is seamless.
using ModLevels = std::array<std::array<float, pattern::kDrumLanes>, genes::kMaxSteps>;
/// The levels of `cycle` (a Drum decode, the hand's edits applied); `*live` gets a bit per lane that hits at least once and
/// is switched on in `laneOn`. A lane that is not live has no level: it rests at identity.
void modLevelsOf(const pattern::PatternCycle& cycle, uint8_t laneOn, ModLevels& out, uint8_t* live) noexcept;
/// **ONE LANE'S VALUE** at `frac` (0..1) through step `k` of a `length`-step cycle, before polarity: Hold the step's level;
/// Glide a one-pole slew (τ = `time` steps) toward it from where the cycle's periodic slew stood at the step's start; Ramp
/// a straight line from this step's level to the next step's over the last `min(time, 1)` of the step.
[[nodiscard]] double modLevelAt(const ModLevels& levels, size_t length, size_t k, double frac, size_t lane,
                                ModSmooth smooth, double time) noexcept;
/// The lane's output: 0 (identity) when it is not live or the tab is forced to identity, else its level, as 2v − 1 when
/// Bipolar.
[[nodiscard]] float modOutput(const ModLevels& levels, uint8_t live, size_t length, size_t k, double frac, size_t lane,
                              const ModLanes& m) noexcept;

/// **ONE PATTERN'S SLICE OF THE HAND'S BLOCK** (`pattern_hand`): its banked rotate residue, its pin switches, its zoom.
struct HandSlot {
	SliceResidue<genes::kRowCount> residue {};
	std::array<bool, genes::kPinCount> pinOn = genes::kPinOnByDefault; // every pin on but Ratchet (opt in)
	float zoom = 1.0f;
};
using HandState = std::array<HandSlot, kPatterns>;
/// The block decoded — or, when `n` is not the block's length (nothing written yet), the defaults (every pin on but Ratchet). A pin
/// switch is one bit of a u16 (`kPinCount` ≤ 16); a block written before Ratchet had its bit reads it Off, its default.
[[nodiscard]] HandState handStateOf(const float* in, size_t n) noexcept;
void writeHandState(const HandState& state, float* out) noexcept;
/// **TURNING ROTATE KEEPS THE PATTERN UNDER THE PUCK** (Feathers' rule, `foldResidue`): the residue `slot` must hold
/// after pattern `before` turns to `rotateAfter`, banking what the turn would otherwise have moved at its place.
[[nodiscard]] HandSlot foldRotate(const HandSlot& slot, const PatternSettings& before, float rotateAfter) noexcept;

/// **WHERE A STORED POINT LANDS ON A PATTERN'S PLANE** — `ui::terrainPlaceOn`'s least squares on this device's rows: the
/// travel (x, y) on the plane `q` and `residue` stand on (its home, its rotate, its banked turns) whose decode is nearest
/// `point` (phases then rate offsets, as `likes::Like::point` stores them), and how far off that plane it is, in travel
/// units against the plane's own scale. Reported, never used to hide a mark.
struct PlaneMark {
	double x = 0.0;
	double y = 0.0;
	double offPlane = 0.0;
	bool solved = false;
};
[[nodiscard]] PlaneMark markOn(const PatternSettings& q, const SliceResidue<genes::kRowCount>& residue,
                               const std::array<double, 2 * genes::kRowCount>& point) noexcept;
/// **THE LIKES' WARP'S STRENGTH** (lead's call from the study): each like weighed as 2 against the prior.
inline constexpr double kLikesWarpStrength = 2.0;
/// **ONE SLOT'S WARP, FITTED SYNCHRONOUSLY** — what the background fit computes for it (its likes' angles at the heart's
/// length, from each like's stored point). False if `stop` cut it short. Message thread or worker: it allocates.
bool fitLikesWarp(const likes::SlotLikes& slot, genes::Warp& out, const std::function<bool()>& stop = {});

/// The point a place decodes to, as a like stores it (`placeOf`'s phases, then its rate offsets).
[[nodiscard]] std::array<double, 2 * genes::kRowCount> pointOf(const genes::Place& place) noexcept;

/// **THE RECIPE PATTERN `p` IS BUILT FROM**, given the row and its pin switches — the device's and the card's lane's
/// one road, so the lane draws what the device would play.
[[nodiscard]] pattern::Recipe recipeFor(const Settings& s, size_t p, const std::array<bool, genes::kPinCount>& pinOn) noexcept;

class PatternDevice {
public:
	/// THE MOST NOTES THAT SOUND AT ONCE: a 13th (7 tones) held into the next chord, plus ratchets' tails.
	static constexpr size_t kMaxSounding = 32;
	/// HOW FAR A SLIDE'S OFF LANDS PAST THE NEXT ON, in seconds — enough that every host keeps the order, so a Legato
	/// synth sees an overlap (design §11.1, TB-Mono).
	static constexpr double kSlideOverlapSeconds = 0.002;

	PatternDevice() noexcept { em_.setSoundingLimit(kMaxSounding); }

	void prepare(double sampleRate) noexcept;
	void reset() noexcept;
	void setFields(const float* fields, size_t count) noexcept;

	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                  size_t* outCount) noexcept {
		em_.processNotes(in, inCount, out, outCapacity, outCount);
	}
	size_t emit(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out, size_t capacity,
	            uint32_t* samplesUntilNext) noexcept;

	[[nodiscard]] float outputAt(size_t output) const noexcept;
	void tick() noexcept { em_.publish(); }
	/// See `NoteOps::seedPassed` (backlog QU).
	void seedPassed(const device::NoteEvent* ons, size_t count) noexcept { em_.seedPassed(ons, count); }
	size_t recentEvents(device::RecentEvent* out, size_t capacity) const noexcept {
		return em_.recentEvents(out, capacity);
	}
	/// **THE PLAYHEAD**: the step playing now, on the grid
	/// (Humanize's shift is not drawn), every step — rests and ties too — as `pattern << 6 | step`; `Playhead::kIdle` when
	/// nothing plays (no key held, or the clock has not reached a step for two steps' time: a stopped transport under
	/// Run on Transport).
	void playhead(uint32_t* ordinal, float* phase) const noexcept;
	static constexpr uint32_t kPlayheadStepBits = 6;
	/// The armed fill, as published beside the playhead (`ModOps::playheadLevel`: (tab + 1) / 8, 0 for none).
	[[nodiscard]] float pendingLevel() const noexcept;
	/// The tab whose fill is armed, read back from that level: `kPatterns` for none.
	[[nodiscard]] static size_t armedFillOfLevel(float level) noexcept {
		const auto t = static_cast<long>(std::lround(level * 8.0f));
		return t >= 1 && t <= static_cast<long>(kPatterns) ? static_cast<size_t>(t - 1) : kPatterns;
	}

	uint64_t readPlay(float* out, size_t capacity) const noexcept { return em_.readPlay(out, capacity); }
	bool restorePlay(const float* in, size_t n) noexcept { return em_.restorePlay(in, n); }
	/// THE HAND'S BLOCK: what was last written (any non-audio thread), restored into a fresh instance, or applied to
	/// a running one (the message thread, the one writer).
	uint64_t readHand(float* out, size_t capacity) const noexcept { return hand_.read(out, capacity); }
	bool writeHand(const float* in, size_t n) noexcept;
	/// THE LIKES' BLOCK: kept here so it saves and restores with the device, read only off the audio thread.
	uint64_t readLikes(float* out, size_t capacity) const noexcept { return likes_.read(out, capacity); }
	bool writeLikes(const float* in, size_t n) noexcept;
	/// THE HAND'S EDITS (`pattern.edits.v1`): the same road as the lane map's.
	uint64_t readEdits(float* out, size_t capacity) const noexcept { return edits_.read(out, capacity); }
	bool writeEdits(const float* in, size_t n) noexcept;
	/// A LOAD'S EDITS: v4 as written; v3 (a patch from before cells) read into v4 with no cell set, unless a v4 came first.
	bool restoreEditsV4(const float* in, size_t n) noexcept {
		const bool ok = writeEdits(in, n);
		editsV4Restored_ = editsV4Restored_ || ok;
		return ok;
	}
	bool restoreEditsV3(const float* in, size_t n);

	/// THE PLACE PATTERN `p` IS AT — the house's decode: home `zone/1023 + offset + gamma` (`sliceHome`), travel at
	/// its place plus its polar orbit (`pulledPlace`), on the plane `Rotate` aims with what past turns banked
	/// (`sliceFrameFor`). `residue` is the pattern's own from the hand's block.
	[[nodiscard]] static genes::Place placeOf(const PatternSettings& q, const SliceResidue<genes::kRowCount>& residue) noexcept;

	// TEST SEAMS — the values, not a render.
	[[nodiscard]] const Settings& settingsForTest() const noexcept { return s_; }
	/// How many times `setFields` decoded its row (`settingsOf`): a row handed again with the same bits is not decoded.
	[[nodiscard]] uint64_t decodesForTest() const noexcept { return decodes_; }
	[[nodiscard]] size_t playingForTest() const noexcept { return playing_; }
	[[nodiscard]] const pattern::PatternCycle& cycleForTest(size_t p) const noexcept { return built_[p].cycle; }
	//-- THE LIKES' WARP, test seams ---------------------------------------------------------------------------------//
	/// Blocks until no fit is queued or running.
	void waitForWarpForTest() { deriver_.waitIdle(); }
	/// The published slot's warp (null: none published, or the slot has no likes), its serial (0: none), how many
	/// publishes there have been, and the newest request's serial.
	[[nodiscard]] const genes::Warp* publishedWarpForTest(size_t p) const noexcept;
	[[nodiscard]] uint64_t publishedSerialForTest() const noexcept;
	[[nodiscard]] uint32_t warpPublishesForTest() const noexcept { return warpPublishes_.load(); }
	[[nodiscard]] uint64_t warpRequestForTest() const noexcept { return deriver_.latest(); }
	/// The warp serial the slot's cycle was last built with (0: plain).
	[[nodiscard]] uint64_t builtWarpSerialForTest(size_t p) const noexcept { return built_[p].warpSerial; }
	[[nodiscard]] size_t soundingForTest() const noexcept { return em_.soundingCount(); }
	[[nodiscard]] const HeldChord& chordForTest() const noexcept { return em_.chord(); }
	/// THE RECIPE pattern `p` is built from now.
	[[nodiscard]] pattern::Recipe recipeOf(size_t p) const noexcept;
	[[nodiscard]] double stepSecondsForTest() const noexcept { return stepSecondsNow_; }
	/// CYCLE (design §7.6): the walk as the device reads it, and the configuration the row asks for now (on the song
	/// clock when `song`, at `secondsPerBeat`).
	[[nodiscard]] const CycleWalk& walkForTest() const noexcept { return walk_; }
	[[nodiscard]] CycleConfig cycleConfigForTest(bool song, double secondsPerBeat) noexcept {
		spbNow_ = secondsPerBeat;
		return cycleConfigNow(song);
	}
	/// Whether the walk has stopped (a Stop's boundary passed, or nothing in range) at the last slot.
	[[nodiscard]] bool cycleStoppedForTest() const noexcept { return cycleStopped_; }
	/// How many slots have played a step (rests and ties too), and the last one's `pattern << 6 | step`.
	[[nodiscard]] uint64_t slotsPlayedForTest() const noexcept { return slotsPlayed_; }
	[[nodiscard]] uint32_t lastOrdinalForTest() const noexcept { return playheadOrdinal_; }
	/// The fill armed by its trigger and the fill sounding over the playing pattern (`kPatterns`: none).
	[[nodiscard]] size_t armedFillForTest() const noexcept { return armedFill_; }
	[[nodiscard]] size_t activeFillForTest() const noexcept { return activeFill_; }
	/// **THE IDENTITY GATES AND THE MODULATION LANES** (design §7.10), as the sources will read them — not registered as
	/// sources yet (the device's output count waits on the per-device output cap).
	[[nodiscard]] float tabActive(size_t tab) const noexcept;
	[[nodiscard]] float modLane(size_t tab, size_t lane) const noexcept;
	/// The lane levels pattern `p` was last built with, and which lanes are live (hit at least once, switched on).
	[[nodiscard]] const ModLevels& modLevelsForTest(size_t p) const noexcept { return built_[p].levels; }
	[[nodiscard]] uint8_t modLiveForTest(size_t p) const noexcept { return built_[p].live; }

private:
	struct Built {
		pattern::PatternCycle cycle {};
		PatternSettings from {};
		HeldChord chord {};
		uint64_t handSerial = 0;
		uint64_t editsSerial = 0;
		Keys keys = Keys::Transpose;
		bool fixedVelocity = false;
		float velocity = 0.0f;
		/// THE WARP IT WAS BUILT WITH, by its build serial (never its address: the two buffers are reused).
		uint64_t warpSerial = 0;
		bool valid = false;
		/// MODULATION'S LANE LEVELS (design §7.10), built with the cycle when the Mode decodes Drum.
		ModLevels levels {};
		uint8_t live = 0;
	};
	/// ONE PATTERN'S SLICE OF THE HAND'S BLOCK, decoded.
	using Hand = HandSlot;

	/// HUMANIZE'S GRID HOOK (`StepGrid::shift`): slot g's timing offset in steps, at the step length now.
	static double shiftOf(const void* self, int64_t g) noexcept;
	static void playSlot(void* self, int64_t g, StepEmitter& e) noexcept;
	static void betweenSlots(void* self, double u, double sps, StepEmitter& e) noexcept;
	void play(int64_t g, StepEmitter& e) noexcept;
	/// THE PLAYING PATTERN'S STEP `step` (wrapped by its cycle's length) at slot `g` of its grid.
	void playStep(int64_t g, int64_t step, StepEmitter& e) noexcept;
	/// PATTERN `pat`'S STEP `step` at slot g of the grid playing (a fill sounds on the grid under it), every off at or before
	/// `offLimit` (in steps of the grid).
	void playStepOf(size_t pat, int64_t g, int64_t step, StepEmitter& e, double offLimit) noexcept;
	/// **ONE SLOT OF THE PATTERN PLAYING, with its phase, its Span slot and any fill over it** (design §7.8): `segment` is the
	/// walk's (Cycle) or null (Selector); j is the slot less Offset.
	void playOwned(const CycleSegment* segment, size_t base, int64_t g, StepEmitter& e) noexcept;
	/// One step of pattern p as a position (Cycle's walk unit, or the same rule under Selector).
	[[nodiscard]] double unitOf(size_t p, bool song) const noexcept;
	struct FillWindow;
	/// Fill f's window laid against `end`, at its own Rate, nothing before `floor`.
	[[nodiscard]] FillWindow windowAt(size_t f, double end, double floor, bool song) const noexcept;
	/// The emitter onto pattern `to`'s grid (what sounds carried at its song position).
	void switchGrid(size_t to, bool song, StepEmitter& e) noexcept;
	/// CYCLE'S SLOT (design §7.6): which pattern the walk says plays at slot g's position, a switch of grid when it is
	/// another, and its step.
	void playCycle(int64_t g, StepEmitter& e) noexcept;
	[[nodiscard]] CycleConfig cycleConfigNow(bool song) noexcept;
	/// Each pattern's cycle length (`cycleLength_`), recomputed when the row or the hand moved.
	void refreshLengths() noexcept;
	/// One step of pattern `p`, in seconds (the song clock's slot).
	[[nodiscard]] double stepSecondsOf(size_t p) const noexcept;
	/// The first pattern Cycle plays, or `kPatterns`.
	[[nodiscard]] size_t cycleFirst() const noexcept;
	void bendNow(double u, double sps, StepEmitter& e) noexcept;
	/// THE ACCENT SWEEP AT `u`, from the pattern and the grid (design §13) — derived, so a scrub reads the same value.
	void sweepNow(double u, const StepEmitter& e) noexcept;
	[[nodiscard]] ClockSettings clockOf(size_t p) const noexcept;
	void rebuildIfStale(size_t p, const HeldChord& chord) noexcept;
	void decodeHand() noexcept;

	Settings s_ {};
	/// THE ROW `s_` WAS LAST DECODED FROM (`setFields`): its first `kParamCount` fields as given, and how many were given
	/// (`SIZE_MAX`: none yet, so the first row always decodes). `decodes_` counts the decodes, for the case that pins it.
	std::array<float, kParamCount> decoded_ {};
	size_t decodedCount_ = SIZE_MAX;
	uint64_t decodes_ = 0;
	double sampleRate_ = 48000.0;
	StepEmitter em_;
	/// The pattern sounding now, `kPatterns` for none.
	size_t playing_ = kPatterns;
	std::array<Built, kPatterns> built_ {};

	device::HandBlock<kHandWords> hand_;
	device::HandBlock<likes::kBlockWords> likes_;
	device::HandBlockCopy<kHandWords> handCopy_;
	std::array<float, kHandWords> handScratch_ {};
	std::array<Hand, kPatterns> handDecoded_ {};
	device::HandBlock<kEditsWords> edits_;
	device::HandBlockCopy<kEditsWords> editsCopy_;
	std::array<float, kEditsWords> editsScratch_ {};
	EditState editsDecoded_ {};
	/// A v4 edits block was restored into this instance (so a stale v3 beside it is not read over it).
	bool editsV4Restored_ = false;
	/// DRUM: one pattern step's hits, sounded lane by lane (design §11-ter).
	void playDrum(const pattern::PatternStep& st, int64_t g, StepEmitter& e) noexcept;

	/// **TB-GLIDE'S RUN** (design §11.3): one held note across consecutive slides, bent arp-side by TUNING expressions.
	/// `value(u)` is the 303 lag from `from` toward `target`, started at `startU`; the run ends at `endU`.
	struct Bend {
		bool active = false;
		/// The step that last played slides on into the next: the next note continues the run.
		bool slideInto = false;
		int16_t key = 60;
		int16_t channel = 0;
		double from = 0.0;
		double target = 0.0;
		double startU = 0.0;
		double endU = 0.0;
		/// The lag's time constant, in seconds: the playing pattern's Glide Time at the segment's start.
		double tauSeconds = kTb303SlideSeconds;
		/// The step length the lag is timed against, in seconds (the grid's at the run's start).
		double stepSeconds = 0.125;
		double sentU = -1e300;
		double sentValue = 0.0;
		[[nodiscard]] double value(double u) const noexcept;
	};
	Bend bend_ {};
	/// THE STEP'S LENGTH NOW, in seconds and in beats (0 under Hz) — what Humanize's milliseconds and its bar read.
	double stepSecondsNow_ = 0.125;
	double stepBeatsNow_ = 0.25;

	float stepOut_ = 0.0f;
	float pitchOut_ = 0.0f;
	float velocityOut_ = 0.0f;
	float accentOut_ = 0.0f;
	float slideOut_ = 0.0f;
	float sweepOut_ = 0.0f;
	uint32_t playheadOrdinal_ = 0xFFFFFFFFu;
	uint64_t slotsPlayed_ = 0;
	uint64_t lastPlaySample_ = 0;
	uint64_t sampleNow_ = 0;
	bool playheadIdle_ = true;
	float degreeOut_ = 0.0f;
	/// **THE MODULATION STEP IN HAND** (design §7.10): the tab (`kPatterns`: none — the step playing is not Modulation), its
	/// step in the cycle and its slot on the grid; `modOut_` its eight lanes, refreshed between slots.
	size_t modPattern_ = kPatterns;
	size_t modStep_ = 0;
	int64_t modSlot_ = 0;
	std::array<float, pattern::kDrumLanes> modOut_ {};
	void modNow(double u, const StepEmitter& e) noexcept;
	/// **WOBBLE'S EXTRAS AT SLOT g** (design §12-ter), each only where it does something: a forced slide (the TB modes), a burst
	/// of hits (the modes that strike ratchets), an octave jump (every pitched note Mode but TB-Glide), a strum (a chord in
	/// Chords or Progression). Zero everywhere at Wobble 0.
	struct WobbleExtras {
		bool slide = false;
		int burst = 0;
		int octave = 0;
		bool strum = false;
		bool strumDown = false;
	};
	[[nodiscard]] WobbleExtras wobbleExtrasOf(Mode mode, const pattern::PatternStep& st, int64_t g) const noexcept;

public:
	[[nodiscard]] WobbleExtras wobbleExtrasForTest(Mode mode, const pattern::PatternStep& st, int64_t g) const noexcept {
		return wobbleExtrasOf(mode, st, g);
	}

private:

	//-- CYCLE (design §7.6) --------------------------------------------------------------------------------------------//
	/// THE WALK PLAYING — its configuration LATCHED: a row change waits for the next boundary (a Repeats × Length edge of
	/// the segment playing), a jump or a stop; a change of the steps' units (a Rate, the clock) takes at once.
	CycleWalk walk_;
	/// Where the last slot was: its segment's index and its repeat within it — a change of either is a boundary.
	int64_t cycleMarkIndex_ = -1;
	int64_t cycleMarkRepeat_ = 0;
	/// The last slot played on Cycle, on the grid then; a slot that does not follow it is a jump.
	int64_t cycleLastSlot_ = 0;
	bool cycleHave_ = false;
	bool cycleSong_ = false;
	bool cycleStopped_ = false;
	/// The song's seconds a beat at the last clock call (Hz patterns in a synced walk).
	double spbNow_ = 0.5;
	/// EACH PATTERN'S CYCLE LENGTH (Progression's is its progression's), recomputed when the row or the hand moved.
	std::array<int64_t, kPatterns> cycleLength_ {};
	bool cycleLengthDirty_ = true;
	pattern::PatternCycle lengthScratch_ {};
	/// A PlaySlot moved the grid this call: `betweenSlots` skips this call (its `u` is the old grid's).
	bool regridded_ = false;
	/// THE PATTERN THE STEP IN HAND IS (`playStepOf`) and how late its notes may end.
	size_t stepPattern_ = kPatterns;
	double offLimit_ = std::numeric_limits<double>::infinity();
	/// **THE FILL TRIGGERS** (design §7.8): each tab's edge, the fill armed (waiting for the end of the playing pattern's
	/// repeat), and the fill sounding: its tab, the slot it ends against, the window it plays, on which grid.
	std::array<mod::TrigEdge, kPatterns> fillEdge_ {};
	size_t armedFill_ = kPatterns;
	size_t activeFill_ = kPatterns;
	/// THE TRIGGERED FILL SOUNDING: its window (positions, its own grid's slots) and where it was taken.
	struct FillWindow {
		size_t fill = kPatterns;
		double unit = 1.0;
		int64_t endSlot = 0;   ///< the fill's cycle ends with this slot of its grid (exclusive)
		int64_t firstSlot = 0; ///< its window's first slot, and one past its last
		int64_t lastSlot = 0;
		double from = 0.0; ///< the window, as positions
		double to = 0.0;
	};
	FillWindow activeWindow_ {};
	double activeAt_ = 0.0;
	/// Selector is under a triggered fill (its slots go through `playOwned` until the fill is over).
	bool selectorFill_ = false;

	//-- THE LIKES' WARP (design §14-quater) ----------------------------------------------------------------------------//
	/// TWO BUFFERS, each every slot's warp and the serial of the request it was fitted for. The worker writes the one the
	/// audio thread is not in (`warpReading_`), then flips `warpFront_`; the audio thread names the buffer it reads before
	/// it reads and re-checks the front (so it never settles in a buffer the writer may take). Allocated by the worker on
	/// its first fit, freed after the deriver has stopped.
	struct WarpBuffer {
		uint64_t serial = 0;
		std::array<bool, kPatterns> live {};
		std::array<genes::Warp, kPatterns> warp {};
	};
	struct WarpBank {
		std::array<WarpBuffer, 2> buffer {};
	};
	static constexpr uint32_t kNotReading = 2;
	void requestWarp(const likes::AllLikes& all);
	void fitAndPublish(const likes::AllLikes& all, const device::Deriver::Job& job);
	std::unique_ptr<WarpBank> warpOwned_;
	std::atomic<WarpBank*> warpBank_ { nullptr };
	std::atomic<uint32_t> warpFront_ { 0 };
	std::atomic<uint32_t> warpReading_ { kNotReading };
	std::atomic<uint32_t> warpPublishes_ { 0 };
	/// LAST, so it is destroyed FIRST: its destructor stops and waits for a fit that captures this device.
	device::Deriver deriver_;
};

} // namespace feathers::mod::arp::pat
