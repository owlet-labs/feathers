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

#include "dsp/character_bank.h"
#include "dsp/osc_voice.h"
#include "dsp/puck_motion.h"
#include "fx/busutil/busutil_meter.h"
#include "fx/filter/multimode_filter.h"
#include "fx/spectral/spectral_sidechain.h"
#include "device/state_blocks.h"
#include "backplane/container_bodies.h"
#include "plugin/feathers_chains.h"
#include "plugin/lane_rack.h"
#include "plugin/phase_service.h"
#include "device/ears.h"
#include "rack/published_chain.h"
#include "rack/inner_chain.h"
#include "mod/dahdsr/dahdsr.h"
#include "mod/mseg/mseg.h"
#include "plugin/audition_tuning.h"
#include "plugin/fx_chain.h"
#include "mod/modulator.h"
#include "plugin/mod_rack.h"
#include "plugin/params.h"
#include "plugin/preview.h"
#include "plugin/pad_hits.h"
#include "plugin/sound_orientation.h"
#include "plugin/sound_residue.h"
#include "plugin/sound_gamma.h"
#include "plugin/spectral_plot.h"
#include "plugin/spectral_state.h"
#include "plugin/sound_travel.h"
#include "plugin/sound_weights.h"

#include "sst/voicemanager/voicemanager.h"

#include <array>
#include <span>
#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

// The synth: a shared character bank, a pool of voices, and sst-voicemanager deciding which of them
// plays what.
//
// Deliberately not the CLAP plugin class. Everything below is reachable from a test with no host at
// all — notes go in as calls, audio comes out as buffers — which is what makes the voice logic
// testable at the level it is written at. feathers_clap.cpp translates a clap_process into these
// calls and owns no voice knowledge of its own.
//
// WHAT THE VOICE MANAGER OWNS: which key gets a voice, which voice is stolen, what mono and legato
// mean, and the note-id bookkeeping underneath per-note modulation. WHAT WE OWN: the sound of each
// of those decisions — the amplitude edges, the steal fade, the glide, and the pitch every voice
// ends up playing.
//
// NOTHING HERE ALLOCATES once activate() has run. The voice pool, the manager and its per-port key
// state are all built at construction; every method below is audio-thread callable.

namespace feathers {

/// Voices that exist beyond the ones that can sound at once.
///
/// A stolen voice is not cut, it is faded out over a couple of milliseconds — an instant cut at
/// whatever amplitude the waveform happened to be at is a click, and stealing is exactly when the
/// synth is busiest and most audible. That fading voice still needs somewhere to live, and its old
/// slot has already been handed to the note that stole it. These are that somewhere.
///
/// Four is a judgement, not a proof. A steal frees its slot within ~2 ms, so exhausting the spares
/// takes five steals inside that window — reachable by a host delivering twenty-one or more notes on
/// the same frame, and by a play-mode change, which hard-terminates every sounding voice at once.
/// Past that the extra steals are instant cuts rather than fades: one click, and the pool is whole
/// again 2 ms later. The failure is audible and self-healing rather than structural, which is what
/// makes four worth spending instead of sixteen.
inline constexpr int kStealFadeSpares = 4;

/// The physical voice pool. Larger than the polyphony limit for the reason above.
inline constexpr int kVoiceSlots = kMaxVoices + kStealFadeSpares;

/// **HOW MANY NOTE EXPRESSIONS THERE ARE — CLAP's number, not ours.**
///
/// `clap/events.h` defines VOLUME, PAN, TUNING, VIBRATO, EXPRESSION, BRIGHTNESS and PRESSURE as ids
/// 0 through 6. Derived from the highest rather than written as 7, so an eighth expression is a
/// compile error at every site that indexes by id instead of a value landing quietly nowhere.
inline constexpr size_t kNoteExpressionCount = CLAP_NOTE_EXPRESSION_PRESSURE + 1;

/// How long a stolen voice takes to get out of the way. Fixed rather than taken from the release
/// param: a steal has to finish promptly whatever the patch's release is set to, and it is masking a
/// discontinuity rather than shaping a sound.
inline constexpr double kStealFadeSeconds = 0.002;

/// **THE VOICE'S OWN EDGE, AT NOTE-ON AND NOTE-OFF — FIXED, AND NOT A CONTROL**. The envelope routed into Voice Level is the note's shape; this is only the
/// de-click under it, and it is load-bearing: a Digi restart, a gate-only patch (Voice Level up, no route) and the
/// last of a routed tail below the hold floor would each click without it. 2 ms, the deleted knobs' default, so the
/// default patch renders as it did.
inline constexpr double kVoiceEdgeSeconds = 0.002;

/// The most host samples the synth renders between one look at its parameters and the next. Purely
/// an internal chunk: rendering is sample-for-sample identical at any chunk size, and this one is
/// the oscillator's own so no buffer is bigger than it needs to be.
inline constexpr int kRenderChunk = Halfband4x::kMaxBlock;

/// One sounding voice: an oscillator, an amplitude edge, and the note that owns it.
struct Voice {
	/// Where the amplitude edge is. A voice is Idle exactly when it is available for a new note —
	/// which a Stealing voice is not, though the voice manager has already stopped counting it.
	enum class Stage : uint8_t {
		Idle,
		Attack,
		Sustain, ///< Held at full level. The design has no decay or sustain stage — these are edges.
		Release,
		Stealing,
	};

	OscVoice osc;

	/// NO DEVICE MEMBERS HERE ANY MORE, and their absence is the point.
	///
	/// This held a `MultimodeFilter` — the per-voice arrangement, hardcoded to the one device that had
	/// one. HARM became the second and there was nowhere to put it: a `harm` member beside the filter
	/// is two hardcoded devices, and that is the pin the fx lane spent this whole arc pulling out of
	/// the BUS road. A voice's devices now live in `Synth::voiceRunner_`, indexed by this voice's
	/// `slot` — still one instance per voice rather than per note, for the reason the old comment
	/// gave: a voice is forever and a note is not, so it costs no allocation when a key goes down.
	Stage stage{Stage::Idle};

	/// The amplitude edge, linear in amplitude and advanced once per host-rate sample after
	/// decimation. Linear rather than exponential because these are 2 ms edges whose job is to not
	/// click; the shape of a curve that short is not a sound anyone hears.
	float level{0.0f};
	float step{0.0f};

	/// WHEN THIS VOICE'S NOTE STARTED, as a serial that only ever climbs.
	///
	/// IT IS A NOTE STACK WITHOUT A STACK. 2026-08-24, replacing the lowest-note rule for the
	/// mono arrangement: mono follows the 'last' active note rather than the lowest. E.g. d0 is
	/// held, f0 is played temporarily and then released, the sub should porta up to f0 and back down
	/// to d0 when the note is released.
	///
	/// The fallback that example describes is "the most recent note that is STILL HELD", which is a
	/// stack — and the highest serial among the held voices IS the top of that stack, with no
	/// structure to keep in step with the voice pool and nothing to allocate when a key goes down.
	/// Releasing a note that is not the top changes no maximum, which is exactly the behaviour a
	/// scalar "previous note" variable gets wrong.
	///
	/// MONOTONIC, NEVER REUSED — device_instance.h's rule arriving on the other axis. A counter that
	/// wrapped or reset would make an old voice look like the newest one.
	uint64_t startSerial{0};

	/// THE PER-VOICE ROAD'S LATENCY (a latent per-voice device, `Synth::gainDelay_`): the steal fade applied to the delayed output
	/// at once, from 1 whatever the level; and a released voice draining its delayed tail.
	float stealFade{1.0f};
	uint32_t drainLeft{0};
	bool draining{false};

	/// Position in the pool, fixed for the life of the synth. The noise seed is derived from this and
	/// nothing else — not from allocation order, not from a clock — so the same notes render the same
	/// samples on every bounce.
	int slot{0};

	/// Modulation offset for this voice's Level, from a per-note-id CLAP param mod. Not clamped here:
	/// it is a distance from the base, and the sum is what gets clamped.
	double levelMod{0.0};
	/// RELEASING, AND ITS KEY STRUCK AGAIN ON ANOTHER VOICE — computed at the tick; such a voice is not held by its
	/// Voice Level, which now reads the new note's (see `heldByRoute`).
	bool superseded{false};
	/// **A DRUM ONE-SHOT WHOSE KEY HAS BEEN LET GO** (drum proposal D4): `releaseVoice` leaves such a voice in Sustain for
	/// the drum road to end at exact silence, and that road asks only while the engine is Drum. Switched to any other
	/// engine first, nothing would ever end it — it sustained the new engine's sound at its key for good, one more voice
	/// rendering per hit, until a steal took it (the soak, 2026-11-08: "CPU crept up ... switching engines"). So the let-go
	/// is remembered, and a voice carrying it off Drum is released (`endLetGoOneShots`). Cleared by any new stage.
	bool oneShotLetGo{false};

	/// THE GAIN THE LAST RENDER APPLIED TO THIS VOICE — Level resolved FOR THIS VOICE plus `levelMod`, clamped.
	/// Kept on the voice so a test can assert the value the render used (CLAUDE.md: a claim about a value
	/// cannot be inferred from a rendered buffer). Backlog DU: Level resolves per voice since 2026-09-13.
	float gain{1.0f};

	// What the voice manager last told us this voice is playing.
	int16_t channel{0};
	int16_t key{0};
	int32_t noteId{-1};
	double retuneSemitones{0.0};
	/// **WHAT A PER-NOTE PITCH EXPRESSION HAS BENT THIS VOICE BY, in semitones — backlog DS.**
	///
	/// SEPARATE FROM `retuneSemitones` AND IT HAS TO BE. That one is the note's tuning at note-on —
	/// the manager hands it over once, with the key — and this one moves for the whole life of the
	/// note. Folding them into one field would make the next note-on erase a slide in progress, and
	/// on a mono retrigger it would erase it mid-gesture.
	///
	/// A DELTA, NOT A PITCH, because `CLAP_NOTE_EXPRESSION_TUNING` is one: the spec says semitones
	/// relative to the note, so zero is "expressed at the note's own pitch" and the field starts and
	/// resets there.
	double expressionSemitones{0.0};
	/// **THE TUNING `applyPitches` LAST APPLIED** — so a moved tuning reaches the oscillator as a line over the control
	/// tick rather than a jump (terrain-arp design §11.3,;
	/// MEASURED there: a +2 st TUNING landed whole within 8 samples, so a bend written every 64 samples was a staircase).
	double appliedExpressionSemitones{0.0};

	/// **EVERY NOTE EXPRESSION THIS VOICE IS CARRYING, indexed by CLAP's own expression id.**
	///
	/// `clap/events.h` defines seven — VOLUME, PAN, TUNING, VIBRATO, EXPRESSION, BRIGHTNESS, PRESSURE,
	/// ids 0..6 — and the manager already routes each one to the right voice by (port, channel, key,
	/// note_id) with CLAP's wildcards. **The per-voice ROAD was never the missing piece**; what was
	/// missing is anywhere for the values to go, which is what this is.
	///
	/// **INDEXED BY CLAP'S ID RATHER THAN BY AN ENUM OF OUR OWN**, so nothing has to map between two
	/// numberings and a host's id is the array index directly. `kNoteExpressionCount` is derived from
	/// CLAP's highest rather than written out, so an eighth expression is a compile error here instead
	/// of a value silently landing nowhere.
	///
	/// **TUNING IS STORED HERE *AND* IN `expressionSemitones`, WHICH IS NOT A SECOND STORE OF ONE
	/// FACT.** They are the same number arriving for two different consumers: the oscillator reads the
	/// semitone field on the pitch road (backlog DS), and this array is the MODULATION road — what a
	/// note-expression source publishes. Keeping tuning out of the array would make it the one
	/// expression a patch could not route, for an implementation reason a player cannot see.
	std::array<double, kNoteExpressionCount> expression {};

	/// The pitch last handed to the oscillator, so a slice that changes nothing does not re-assert it
	/// — which for a gliding voice would be harmless and for a poly one would cancel nothing, but
	/// both cost a conversion per voice per slice for no reason.
	double lastTargetSemitones{0.0};
	bool haveTarget{false};

	/// **ONE OSCILLATOR OF A STACK — backlog KE (voice stacking).**
	///
	/// Part 0 is `osc` above and parts 1.. live in `Synth::partOscs_`, indexed by this voice's slot; this is
	/// what the voice knows about each of them. A part stands at a FAN POSITION, −½..+½ of the total detune
	/// (so part k of N is `k/(N−1) − ½`, and the middle part of an odd stack is exactly 0, on the note), and
	/// sounds at a GAIN, 1/√N while it belongs to the stack (equal power, so N uncorrelated parts are
	/// as loud as one).
	///
	/// **A COUNT CHANGE ON A HELD NOTE MOVES THESE, IT NEVER JUMPS THEM**. Each has a
	/// start and a destination: the fan position walks there over `kStackRespaceSeconds` on the control tick
	/// (pitch cannot click — the oscillator is phase-continuous — but a few cents should not audibly leap),
	/// and the gain walks there per sample over the same time, because a gain step IS a click. A part being
	/// added is born at its nearest sounding neighbour's position with gain 0; a part being removed holds its
	/// position and fades to 0, and only then stops rendering (`live` false). Nothing is allocated either way:
	/// the part array is the maximum, fixed at construction.
	struct StackPart {
		double fan{0.0};
		double fanFrom{0.0};
		double fanTo{0.0};
		float gain{0.0f};
		float gainTo{0.0f};
		float gainStep{0.0f};
		/// WHERE THE PART SITS IN THE STEREO FIELD, −1..+1, and where it is going: its place in the fan with the
		/// firmware's alternating sign, times Stack Spread (step 2). Slewed per sample, never stepped.
		float pan{0.0f};
		float panTo{0.0f};
		/// **THE PART'S STACK INDEX, −1..+1** (step 3; `stack_index.h`): what a route from the Stack Index source
		/// multiplies for this part. Re-decided with the pitches; a part leaving the stack keeps the one it had.
		float index{0.0f};
		/// WHICH STAND THE PART READS: −1 the main bank (the puck), otherwise a slot in `Synth::standBanks_` (step 4).
		int stand{-1};
		/// Drift's walk around its base, this part's own (the Drift shape only; 0 otherwise).
		float drift{0.0f};
		/// THE PART'S SHARE OF A STACK-INDEX ROUTE INTO LEVEL, as a multiple of the voice's level: where the last
		/// chunk left it. It walks to the new share across each chunk, since a gain step is a click.
		float levelScale{1.0f};
		/// Rendering — in the stack, or fading out of it.
		bool live{false};
		/// The pitch last handed to this part's oscillator: `Voice::lastTargetSemitones`'s job, per part.
		double lastTargetSemitones{0.0};
		bool haveTarget{false};
	};
	std::array<StackPart, kMaxStack> parts{};
	/// The stack this voice is spread for, and the control ticks left in a re-space.
	int stackCount{1};
	int respaceTicksLeft{0};
	/// ANYTHING BUT PART 0 AT UNITY: the stack road runs. False for a stack of one at rest, which is what keeps
	/// that voice's render the render it always was, sample for sample.
	bool stacked{false};
	/// ANY PART OFF CENTRE, OR ON ITS WAY: the voice needs the stereo road (step 2). Set by `aimStackPans`.
	bool panned{false};
	/// Random Fresh's draw for this note, and Drift's noise stream — both seeded at the note-on (step 3).
	uint32_t freshSeed{0};
	uint32_t driftRng{1};
	/// The song tick Drift's walk has stepped to, while the transport plays — the walk takes as
	/// many steps as the SONG has ticked, so a loop or a seek replays it; -1 when it walks by the engine's ticks.
	int64_t driftSongTick{-1};
	/// **HOW ALIKE THE PARTS ARE AT 0 CENTS** (`device::NoteContext::stackCoherence`;), and what it was
	/// computed from — per part, the bank it reads, that table's build serial, the grid it plays and the vowel bakes
	/// behind that grid — so a part's cycle is re-read only when one of those moves (`Synth::updateStackCoherence`).
	/// Keyed on the serial, not the table's address: the banks double-buffer, so an address comes back holding
	/// another build.
	struct CoherenceKey {
		std::array<const void*, kMaxStack> bank{};
		std::array<uint64_t, kMaxStack> serial{};
		std::array<const float*, kMaxStack> grid{};
		std::array<uint32_t, kMaxStack> bakes{};
		friend bool operator==(const CoherenceKey&, const CoherenceKey&) = default;
	};
	float stackCoherence{1.0f};
	CoherenceKey coherenceKey{};

	bool isSounding() const { return stage != Stage::Idle; }
};

/// How long a Count change takes to re-space a held note's fan and fade its parts in or out. A judgement,
/// to be set by ear (the voice-stacking doc §3.4): long enough that a few cents of re-spacing glide rather
/// than leap, short enough that the new count is heard as the answer to the gesture.
inline constexpr double kStackRespaceSeconds = 0.02;

/// A part whose share of a Level route has not been decided yet — a new note, or a part just born — takes its share
/// at once rather than walking in from unity.
inline constexpr float kLevelScaleUnseated = -1.0f;

class Synth {
public:
	/// Holds a reference: the param set outlives the synth and is shared with the host-facing
	/// extensions, which read it from the main thread.
	explicit Synth(ParamSet& params);

	/// Sample rate and every rate-derived quantity. Allocation-free, and safe to call again on a rate
	/// change — it silences everything first, since a voice mid-edge would otherwise keep a step
	/// sized for the old rate.
	///
	/// **AT THE RATE IT IS ALREADY RUNNING, A REACTIVATION KEEPS EVERY VOICE**. A host restart is not a musical event: a
	/// latency that moves (the EQ's Phase, the Phaser's Mode) costs a deactivate/activate, and the host still holds the
	/// key, so a voice cleared here stays silent until the NEXT note. Nothing a voice holds is stale at an unchanged
	/// rate, so nothing is reset and nothing is re-prepared: the voices, the string, the lane's devices and its
	/// modulators run on. Clearing everything is `reset()`'s job, which the host calls on its own.
	///
	/// `maxFrames` IS THE HOST'S LONGEST BLOCK (CLAP `activate`'s `max_frames_count`, backlog GO), handed to every lane
	/// device's `prepare` so a device can size its scratch to it. AT THE SAME RATE IT IS GROW-ONLY: the voices are kept
	/// either way, and the lane's devices are re-prepared only when the block EXCEEDS what they were prepared with — a
	/// latency restart or a shrink leaves every device's state and tail alone.
	void activate(double sampleRate, int maxFrames);
	/// THE MAIN THREAD, AT THE HOST'S DEACTIVATE: stops and joins A-PH's optimiser thread (its lifetime is activate → deactivate;
	/// it reads only its own request slot, store and optimiser, never the synth or a host API). The voices keep any profile.
	void deactivate();

	/// **HOW LATE THE LANE LEAVES THE OUTPUT, IN SAMPLES** (backlog JM) — the serial sum of what its devices
	/// declare, at the rate `activate` was handed. The voices add none (a latent device never runs per
	/// voice), so this is the plugin's whole latency. AUDIO THREAD, or while nothing renders.
	/// AND THE KIT PADS' FX LANES' (drum proposal §17 P4): the latest live pad lane delays the whole kit to meet it.
	[[nodiscard]] uint32_t latencySamples() const noexcept { return lane_.latencySamples(sampleRate_) + padLatency_; }
	/// TEST ONLY: the per-voice road's gain delay the last block used (0 without a latent per-voice device).
	[[nodiscard]] uint32_t perVoiceGainDelayForTest() const noexcept { return gainDelay_; }

	/// **WHETHER A LANE DEVICE STILL HOLDS SOUND THAT WILL COME OUT WITH NO NEW EVENT** (backlog MC) — a delay between
	/// repeats, whose output is exactly silent while its line is not. SLEEP waits for it. AUDIO THREAD, after a block.
	/// AND EVERY KIT PAD'S FX LANE (drum proposal §17): a pad's reverb rings on after its voices end, and SLEEP waits for it too.
	[[nodiscard]] bool holdsTail() const noexcept { return lane_.holdsTail() || padLanesHoldTail(); }

	/// **A KIT PAD'S OWN FX LANE** (drum proposal §17, stage (d); 2026-11-07: per-pad fx lanes, the kit window's right side).
	/// Each pad's voices sum into its own bus, the bus runs its lane, and the lane's output joins the voices' mix before the
	/// engine's fx lane. A pad with no lane, or an empty one, or one whose every device is off, sums straight into the mix as
	/// it always did. Kit mode alone (Pitched mode is one drum, and the engine's lane is its lane). Delay's feedback-lane
	/// pattern exactly: built lazily on the MESSAGE THREAD, published by an atomic, taken by the audio thread at a block's start.
	/// MESSAGE THREAD (it allocates). Never freed while the synth lives, so a published pointer is never dangling.
	rack::InnerChain* ensurePadLane(size_t pad);
	/// The pad's lane, or null until one is made. Any thread: the published pointer.
	[[nodiscard]] rack::InnerChain* padLane(size_t pad) const noexcept {
		return pad < padLanes_.size() ? padLanes_[pad].published.load(std::memory_order_acquire) : nullptr;
	}
	/// TEST ONLY: whether pad `pad`'s voices went through its lane in the last render call.
	[[nodiscard]] bool padLaneLiveForTest(size_t pad) const noexcept { return pad < padLanes_.size() && padLanes_[pad].live; }

	/// Every voice silent and every note forgotten, without the release tails a note-off would give.
	void reset();

	// The note lifecycle, one call per host event. `channel` and `key` are MIDI-numbered; `noteId` is
	// the CLAP note id, or -1 from a host speaking MIDI, in which case matching falls back to
	// channel and key.
	void noteOn(int16_t channel, int16_t key, int32_t noteId, double velocity);
	void noteOff(int16_t channel, int16_t key, int32_t noteId, double velocity);

	/// Silence the matching voices now, with no release. What a host means by NOTE_CHOKE — the note
	/// is not finishing, it is being taken away — so it still gets the steal fade rather than a cut.
	void choke(int16_t channel, int16_t key, int32_t noteId);

	/// Pitch wheel, -1 to 1, scaled by the Bend Range param. Per channel, because MIDI is.
	void pitchBend(int16_t channel, double wheel);

	void sustainPedal(int16_t channel, bool down);

	/// A CLAP param modulation addressed to a note rather than to the whole plugin. Only Level is
	/// declared per-note-modulatable, and anything else is dropped here rather than silently applied
	/// to the global lane, which is a different param with a different owner.
	void paramModForNote(int16_t channel, int16_t key, int32_t noteId, clap_id paramId, double amount);

	/// **A PER-NOTE EXPRESSION FROM THE HOST — backlog DS.** `expression` is a `clap_note_expression`,
	/// passed through rather than translated: the manager routes by address and the RESPONDER decides
	/// which expressions this instrument answers to, so a second enum here would be a third place for
	/// the same fact to live. Today only `CLAP_NOTE_EXPRESSION_TUNING` is answered.
	void noteExpression(int16_t channel, int16_t key, int32_t noteId, int32_t expression,
	                    double value);

	/// Render `n` host-rate samples into two channels. Mono — both channels the same signal — except
	/// while terrain stereo is live: a V5 patch at Width > 0 evaluates the character once per ear at
	/// puck ∓(w/2)·(1,0) (§5.5, decisions V11/V12), and the two channels are then the two ears. Width
	/// 0, every frozen engine, and every patch that predates the param render exactly as before,
	/// down the untouched mono path.
	///
	/// Parameters are read once at entry, which is what makes the caller's event slicing meaningful:
	/// a param event splits the block, and the samples after the split are rendered with the new
	/// value. Nothing in here reads the wall clock or the block length, so the same events at the
	/// same frames produce the same samples however the host chops its buffer.
	void render(float* left, float* right, int n);
	/// **THE SIDECHAIN FOR THE NEXT `render`** (design §10.1): the plugin's "Sidechain" input over the same `n` frames, null for
	/// silence. The fx lane's bus devices that listen to it read it chunk by chunk, at each chunk's offset. AUDIO THREAD.
	void setSidechain(const float* left, const float* right, bool sounding) noexcept {
		sideL_ = left;
		sideR_ = right;
		sideSounding_ = sounding;
	}

	/// WHERE THE SONG IS, at the first sample of the block about to be rendered. Audio thread, once
	/// per block, before render — the same place every other not-an-event input is taken.
	///
	/// THE POSITION IS RE-ANCHORED FROM THE HOST EVERY BLOCK AND ADVANCED BY RENDERING IN BETWEEN,
	/// and that combination is the whole point rather than an implementation detail. Advancing alone
	/// would be an accumulator, and an accumulator gives a different answer for "bar 5 played
	/// through" than for "bar 5 located to" — which is exactly the non-determinism this exists to
	/// remove. Re-anchoring alone would be too coarse, because a block is up to a few thousand
	/// samples and a note-on lands somewhere inside one. Together they give the exact instant of any
	/// note-on, blockStart + samplesSince/rate, which is the SAME absolute time whatever block size
	/// the host bounced with.
	///
	/// `playing` is the host's transport state. A stopped transport is not a position to lock to —
	/// it is the same position for as long as the user leaves it there — so the oscillator free-runs
	/// while stopped rather than handing every note an identical start phase.
	/// @param songPositionSeconds where the song is, or NaN if the host offers no seconds timeline
	/// @param songPositionBeats   where the song is in beats, or NaN if it offers no beats timeline
	///
	/// **BOTH TIMELINES, WHERE THIS TOOK ONLY SECONDS — backlog DQ.** `feathers_clap.cpp` used to
	/// convert beats into seconds and hand over the result, which was right while the only consumer
	/// was oscillator phase (a lock to "the timeline" is a lock either way). It is wrong for a synced
	/// LFO: its period is N BEATS, so a beats-derived phase stays on the grid through a tempo change
	/// where a seconds-derived one goes discontinuous. The conversion collapsed the distinction before
	/// any device could act on it.
	void setTransport(double songPositionSeconds, double songPositionBeats, bool playing);

	/// THE HOST'S TEMPO, ON ITS OWN ROAD — and the separation is the point rather than tidiness.
	///
	/// RATE COMES FROM TEMPO; PHASE COMES FROM THE TRANSPORT. They are separable, and only
	/// phase needs a playhead.
	///
	/// THE FIRST DRAFT PUT TEMPO INSIDE setTransport AND THAT WAS WRONG, which the compiler found
	/// before any test did. `applyTransport` returns early when the host offers neither a seconds nor
	/// a beats timeline — a real case, and one where CLAP_TRANSPORT_HAS_TEMPO may still be set. A
	/// tempo carried on the position's road would have been dropped for exactly those hosts, which is
	/// the population most likely to be a small or unusual one. Separate roads cannot lose it.
	///
	/// PER BLOCK, NEVER LATCHED — not at note-on, not at prepare. Hosts AUTOMATE tempo and a
	/// `clap_event_transport` arrives with every process call carrying the current value.
	///
	/// `bpm` <= 0 or non-finite means the host has not said. Absence as a value rather than as a
	/// second boolean, the same choice `kNoSustain` makes.
	void setTempo(double bpm) noexcept;

	/// The clock the rack's modulators are ticked with — what the host said, or `unknown()`.
	mod::TransportClock rackClock() const noexcept;

	/// THE CLOCK THE FX LANE IS HOLDING — a test-only seam. `rackClock()` is what the synth COMPUTES;
	/// this is what actually arrived at the other runner, and the two being equal is a claim no
	/// buffer can carry.
	[[nodiscard]] const device::TransportClock& fxClockForTest() const noexcept {
		return lane_.clockForTest();
	}

	/// WHETHER THE LANE'S ENGINE READS AS INSIDE A BLOCK — a test-only seam. Between renders it must
	/// not: the lane's last segment ends with the render (`LaneRack::finish`).
	[[nodiscard]] bool laneInBlockForTest() const noexcept {
		return lane_.engineForTest().inBlockForTest();
	}

	/// ONE RACK SLOT'S SHAPE, so a case can ask what the device was actually handed.
	///
	/// A TEST-ONLY SEAM AND IT EARNS ITS FIVE LINES. "The tempo reached the device" is a claim about
	/// a value, and until something DEPENDS on the tempo there is no rendered buffer that could
	/// carry it — CLAUDE.md's rule, and the same reason `voicePitchForTest` and its two neighbours
	/// exist one file over.
	/// **IT READS THE RUNNER'S STORAGE, WHICH IS WHAT THE DEVICE IS ACTUALLY RUNNING.** It used to
	/// index `shapes_`, an array that existed whether or not the slot held an MSEG — so it answered
	/// for a slot holding an envelope, with a default-constructed shape that looked like a stopped
	/// one. Now a slot that is not running an MSEG has no shape to return, and the seam says so by
	/// handing back a file-static default rather than a lie about a live device.
	const mod::mseg::Shape& msegShapeForTest(size_t slot) const noexcept;

	/// True while the host has told us where the song is. FALSE IS THE HONEST DEFAULT and a real
	/// state: CLAP does not oblige a host to provide transport at all, and a plugin that assumed
	/// time zero would hand every note the same phase and call it determinism. Without it, Analog
	/// keeps the per-voice behaviour it has always had.
	bool haveTransport() const noexcept { return haveTransport_; }

	/// Where the next sample to be rendered sits, in seconds of song.
	double songSecondsNow() const;
	/// The song's control tick at this sample (song samples / kTickSamples, floored), or -1 unless the transport is
	/// playing — the clock the Stack Index's Drift and Random Fresh are made deterministic against (Q9).
	int64_t songTickNow() const;
	/// THE SAME INSTANT IN BEATS, or NaN with no beats timeline. Advances at the tempo, so it needs
	/// one — a beats position with no tempo cannot be moved forward within a block.
	double songBeatsNow() const;

	/// THE PHASE A TRANSPORT-LOCKED OSCILLATOR AT `semitones` STANDS AT RIGHT NOW — the whole of
	/// what "Analog" means since V27, and empty when there is nothing to lock to.
	///
	/// frac(f0 · t): where a free-running oscillator at this note's frequency would be if it had
	/// been running since the song began. A PURE FUNCTION OF SONG POSITION and nothing else, which
	/// is the property the feature exists for — an accumulator would give one answer for "bar 5
	/// played through" and another for "bar 5 located to", and a bounce would not match itself.
	///
	/// THE FREQUENCY IS THE NOTE'S TARGET, not the oscillator's current one, and that matters under
	/// glide: a mono retrigger starts the oscillator at the key the manager hands over, which is a
	/// fact about WHAT WAS PLAYING BEFORE. Locking to that would smuggle history back into the one
	/// place this design exists to keep it out of.
	///
	/// EMPTY IS A REAL ANSWER, not a failure. CLAP does not oblige a host to provide transport, and
	/// a plugin that assumed time zero would hand every note the same phase and call it determinism.
	/// Every caller falls back to the per-voice behaviour Analog has always had.
	std::optional<double> lockedPhaseFor(double semitones) const;

	/// Put  at the song's phase for the note it is playing, or leave it alone when there is
	/// no transport. The same-voice retrigger roads' half of the lock.
	void lockPhaseToSong(Voice& voice);

	/// The audition's handles — an orientation held fixed by hand, the falloff A/B. Audio thread,
	/// before render.
	///
	/// None of them is a parameter and none of them is intended ever to become one. Their defaults are the
	/// ordinary case and the one that plays: a build that never calls this derives every sound's plane
	/// from where the sound is and blends at the power every saved patch was made on.
	void setTuning(const AuditionTuning& tuning) { tuning_ = tuning; }

	/// What the puck is doing, measured rather than set — see PuckMotion. The plugin publishes this for
	/// the editor, so the scope draws the sound being made mid-gesture rather than the one that would be
	/// made standing still.
	AnchorPoint puckVelocity() const { return motion_.velocity(); }

	/// HOW MUCH OF EACH SOUND IS BEING HEARD, by slot — measured rather than set, exactly like the
	/// velocity above, and taken from the character this synth last built rather than recomputed. The
	/// blend is the one thing on this surface that a second reading could get wrong without sounding
	/// wrong, so there is one reading and this is a copy of it.
	///
	/// Empty until the first character is built, which is a synth that has not been asked to play yet.
	SoundWeights soundWeights() const { return weights_; }

	/// Where each sound has drifted to — patch state, read once a block like the tuning. Audio thread,
	/// before render.
	void setSoundTravel(const SoundTravel& travel) { drift_ = travel; }

	/// Which plane each sound is on — patch state, read once a block like the drift.
	void setSoundOrientations(const SoundOrientations& planes) { planes_ = planes; }

	/// What each sound's rotations banked, so that a turn left the standing sound alone — patch state,
	/// on the same channel as the planes it belongs to. Its own setter rather than a field of the
	/// planes because the two are written by different roads: a load restores both, but only a rotation
	/// writes this one, and the state block that carries it is absent from patches that never rotated.
	void setSoundResidue(const SoundResidue& residue) { residue_ = residue; }

	/// WHICH SOUND IS BEING LISTENED TO ON ITS OWN, or none — see preview.h. Read once a block like the
	/// tuning, and for the same reason: it is not an event and carries no timestamp. None is the engine
	/// exactly as it is without the feature, which is what every patch plays.
	void setPreview(const Preview& preview) { preview_ = preview; }
	/// EVERY KIT PAD'S HITS so far (pad_hits.h), for the editor's pulse; read after a block on the thread that rendered it.
	[[nodiscard]] const PadHits& padHits() const noexcept { return padHits_; }
	/// WHETHER A KIT PAD TAKES HITS: its sound card switched on (not muted), and no other card soloed.
	[[nodiscard]] bool padAudible(size_t slot) const;

	/// Gamma's unbounded half, per sound. Zero is the engine as it was, to the bit.
	void setSoundGamma(const SoundGamma& gamma) { gamma_ = gamma; }
	/// **THE LIKES' PHASE WARPS** for this block (backlog ON, dsp/phase_warp.h): per sound slot, the warp, the engine it was
	/// derived for, and the set's serial. Pointers into the bridge's audio-owned front set, valid until the next take; the
	/// synth copies them into its own tables (filtered to the engine being played) only when the serial or the engine changes.
	void setLikeWarps(const LikeWarp* warps, const uint8_t* engines, uint64_t serial) noexcept {
		likeWarps_ = warps;
		likeEngines_ = engines;
		likeSourceSerial_ = serial;
	}
	/// THE SPECTRAL DRAWER'S MASTER SWITCH AND DRAWN CURVE (additive design §3.1) — patch state, handed over on
	/// process()'s schedule like the gammas. Read into every Additive voice's drawer at the next chunk (seatDrawers).
	void setSpectralState(const SpectralState& state) { spectralState_ = state; }
	/// **OE: THE ENGINE VOCODER'S FROZEN ENVELOPE FOR THE PATCH** (additive design §9.3): ANY THREAD, the block as the audio thread
	/// last published it (kVocodeFreezeFloats words) and its serial — 0 when nothing was published (a fresh instance, whose save
	/// then writes no key).
	uint64_t readVocodeFreeze(float* out, size_t capacity) const noexcept { return vocodeFreezeSnapshot_.read(out, capacity); }
	/// The load count of the last load this synth primed from (SpectralState::vocodeFreezeLoads): equal to the bridge's, a save
	/// reads this synth's snapshot; behind it, the load has not reached the audio thread yet and the save writes the loaded words.
	[[nodiscard]] uint32_t vocodeFreezeLoadsTaken() const noexcept { return vocodeLoadsTaken_.load(std::memory_order_acquire); }
	/// TEST ONLY: the envelope the voices read, and how many analysis frames have run.
	[[nodiscard]] const spectral::VocodeEnvelope& vocodeEnvelopeForTest() const noexcept { return vocodeEnv_; }
	[[nodiscard]] uint64_t vocodeFramesForTest() const noexcept { return vocodeAnalyser_->framesForTest(); }
	/// TEST ONLY: the sibilance gate the last chunk ended on (the loudest vocoding voice's amplitude; 0 with no voice).
	[[nodiscard]] float vocodeGateForTest() const noexcept { return vocodeGateWas_; }
	/// TEST ONLY: how many sketches have been posted to the phase optimiser since construction.
	[[nodiscard]] uint64_t phaseSketchPostsForTest() const noexcept { return vocodeSketchPosts_; }

	/// The orientation alone, for callers that hold nothing else. Same channel, same rules.
	void setOrientation(const SliceOrientationPin& pin) { tuning_.orientation = pin; }

	/// WHAT THE MODULATOR RACK HOLDS AND WHAT IT IS SET TO — patch state, read once a block like the
	/// drift and the planes above, and on the same argument: it is not an event and carries no
	/// timestamp, so cutting the block for it would invent a precision it does not have.
	///
	/// The SETTINGS arrive here; the STATE does not. Each envelope's own phase and stage live in this
	/// object and survive every one of these calls, which is what makes a knob moved mid-note change
	/// the shape the note is already playing rather than restart it.
	void setModRack(const ModRack& rack);

	/// WHAT THE FX LANE HOLDS AND WHAT EACH DEVICE IS SET TO, handed over on the same schedule and for
	/// the same reason as the rack above: it is not an event, it carries no timestamp, and cutting the
	/// block for it would be inventing a precision it does not have.
	///
	/// NOTHING IN THE ENGINE READS THESE SETTINGS YET. The filter is still driven from its parameters,
	/// and the commit that removes those parameters is the one that switches it over. This road exists
	/// first ON PURPOSE — the alternative order puts the audio thread on a plain member for one
	/// commit, which is a data race that looks like it works and would be the hardest kind to
	/// reproduce afterwards.
	///
	/// AND THIS IS THE LINE THE RACK DID NOT HAVE FOR A WHOLE EVENING. setModRack existed and was
	/// wired only to a patch load, so every envelope control was dead in the editor and it presented
	/// as three unrelated bugs — the times did nothing, the drawn curve disagreed with the sound, the
	/// loop never ran. One missing per-block handoff. It is written here before there is anything to
	/// read it, rather than after somebody notices.
	void setFxChain(const FxChain& chain);

	/// **THE EDITOR'S CHAINS, TAKEN WHOLE — AUDIO THREAD (or the flush that never overlaps it).** The
	/// bridge's record publishes its state after every change (chain-as-a-device 5b, C4); this takes a
	/// snapshot when the sequence has moved and hands both views to `setModRack` / `setFxChain`. A
	/// snapshot that straddled a publish is refused and asked again next block, so the engine never runs
	/// half of one edit and half of another. Returns whether it took a new state. No allocation.
	bool adoptChains(const rack::PublishedChainState& published) noexcept;
	/// **THE LANE SETTLED BEFORE A BLOCK'S EVENTS** (backlog QU): the lane converged — the devices this block will run
	/// installed — so a key at the block's first sample meets them. The CONVERGE ONLY: `render`'s `applyParams` still runs
	/// the rest (and begins the lane's block once: an early `applyParams` began it twice and lost a held note).
	void settleLaneBeforeEvents() noexcept;
	/// The lane was settled before this block's events: `render`'s next converge is skipped once (it ran there).
	bool laneSettled_ = false;
	/// THE LAST CHAINS PUBLISH THIS SYNTH ADOPTED, for the message thread (`ParamBridge::landHeldControls`).
	[[nodiscard]] uint32_t chainsAdopted() const noexcept { return chainsAdoptedMirror_.load(std::memory_order_acquire); }

	/// **GROW THE CHAINS THE AUDIO THREAD KEEPS — MAIN THREAD, AND IT ALLOCATES** (unbounded devices step 3).
	/// Every chain this synth holds a copy of (`fxChain_`, `rack_`, the adopted and modulated views, the
	/// published-state snapshot and its staging, the playheads and drive meters) is a buffer the audio
	/// thread copies into every block, so none of them may grow there. This builds bigger ones here and
	/// hands them over; the drain at the top of the next `adoptChains` copies what the old ones hold into
	/// them and swaps — moves only, no allocation, no free — and `reapChainGrowth` frees the old ones back on
	/// this thread. A capacity never shrinks, so asking for less is a no-op.
	///
	/// `stateCapacity` is the published state's room PER CHAIN, and must match what the bridge publishes:
	/// `PublishedChainState::adopt` refuses a snapshot of another shape. Returns false while an earlier
	/// growth is still waiting for its drain — ask again after `reapChainGrowth`.
	bool requestChainCapacity(size_t fxCapacity, size_t stripCapacity, size_t stateCapacity);
	/// **ROOM FOR `routes` ROUTES IN THE LANE'S ENGINE — MESSAGE THREAD, ALLOCATES** (backlog OJ). In place: the drain may
	/// be compiling inputs meanwhile. `wireRouteGrowth` calls it from the bridge's list before a publish needs it.
	void reserveRoutes(size_t routes) { lane_.reserveRoutes(routes); }
	[[nodiscard]] size_t routeRoomRefusals() const noexcept { return lane_.routeRoomRefusals(); }
	/// The main thread's half of the handoff: frees a growth the drain has taken. Also run by
	/// `drainRetiredDevices`. Returns whether one was freed.
	bool reapChainGrowth() noexcept;
	/// What the chains have been asked to hold — the MAIN THREAD's record, never the audio thread's buffers.
	[[nodiscard]] size_t requestedFxCapacity() const noexcept { return requestedFx_; }
	[[nodiscard]] size_t requestedStripCapacity() const noexcept { return requestedStrip_; }
	[[nodiscard]] size_t requestedStateCapacity() const noexcept { return requestedState_; }
	/// Test seams: whether a growth is waiting for a drain, and the buffers' sizes now (read with no block running).
	[[nodiscard]] bool chainGrowthPendingForTest() const noexcept {
		return growthState_.load(std::memory_order_acquire) == kGrowthPending;
	}
	[[nodiscard]] size_t fxChainCapacityForTest() const noexcept { return fxChain_.device.size(); }
	[[nodiscard]] size_t stripCapacityForTest() const noexcept { return rack_.device.size(); }


	/// **HOW MANY TIMES THE CHAINS THIS SYNTH RUNS HAVE CHANGED** — by an adopted publish or a chain handed
	/// in by value — so a compile of the routes against `devicePlacement` knows when to happen again.
	uint32_t structureRevision() const noexcept { return structureRevision_; }


	/// MESSAGE THREAD. BUILD THE DEVICE INSTANCES THIS CHAIN NEEDS AND THIS ENGINE HAS NOT BUILT.
	/// Returns how many were built, across both runners.
	///
	/// THIS IS THE OTHER HALF OF A CHAIN CHANGE, AND IT IS ON A DIFFERENT THREAD FROM `setFxChain`.
	/// `setFxChain` is a per-block handoff on the AUDIO thread; building a device allocates, so it
	/// cannot happen there. and
	/// they are fine exactly here, where the gesture that adds one actually happens.
	///
	/// WHERE THE PLUGIN CALLS IT: `pumpEditor`, immediately after `gui_.processEvents()`, which is the
	/// same main-thread visit the editor's add/remove/reorder gesture ran in — so a device is built
	/// before the frame that added it returns, and there is no audible gap. Also at `activate` (the
	/// default lane has a filter in it) and after `stateLoad` (a patch may name devices the editor
	/// never touched, and it may arrive with no editor open at all).
	///
	/// IT TAKES THE RAW CHAIN. Modulation moves a device's controls and cannot move its tag; the tag
	/// is the whole of what decides what to build.
	/// `bodies` IS WHAT EACH CONTAINER IN THE CHAIN HOLDS — the bridge's, read on the main thread
	/// where this runs. Null provisions every container empty, which is what a chain just added is.
	size_t provisionDevices(const FxChain& chain, const ModRack& rack,
	                        ContainerBodies* bodies = nullptr);
	/// THE SAME, FROM THE BRIDGE'S RECORD: its state and the publish it went out as — what the plugin's
	/// main-thread visits call, so a block still on an older publish keeps what this builds.
	size_t provisionDevices(const rack::ChainState& state, uint32_t forPublish, ContainerBodies* bodies);
	/// THE HOST OUTSIDE FOR A CHAIN IN THE LANE — rack roadmap §7. The bridge hands its record's gate maker
	/// once, before provisioning, and every chain device provisioned afterwards is told it before its
	/// body loads.
	void setOuterGateMaker(device::OuterGate (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept;
	/// **THE HOST SLOT POOL FOR A CHAIN IN THE LANE** (host slots T11) — the bridge's maker, handed once before
	/// provisioning, beside the gate: a control inside a chain takes its slot from Feathers' pool.
	void setSlotLinkMaker(device::HostSlotLink (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept {
		lane_.setSlotLinkMaker(make, host);
	}
	/// **THE LANE'S CHAINS UNHOOKED FROM THE BRIDGE THE TWO MAKERS ABOVE POINT AT** — before that bridge dies
	/// (`LaneRack::unhookHostLinks`). Message thread, no block running.
	void unhookHostLinks() noexcept { lane_.unhookHostLinks(); }
	/// **EVERY CHAIN IN THE LANE SERVICED, NOTHING BUILT** (host slots T11) — the plugin's slot visit runs this
	/// after the pool's service, so a rack in the lane pulls its bindings and mirrors the host's writes on every
	/// main-thread visit, editor or none. Message thread.
	void serviceContainers(const rack::ChainState& state, ContainerBodies* bodies) {
		lane_.serviceContainers(state, bodies);
	}
	/// WHAT THE CHAINS IN THE LANE DROPPED AT THEIR SERVICE since last taken — the plugin entry hands these
	/// to the bridge's refusal road after each `provisionDevices`, so the editor's tray says them.
	[[nodiscard]] rack::RouteRefusals takeOuterDrops() noexcept;

	/// **THE DEVICES THE LANE MAY BUILD** — Feathers' catalogue, which the lane also hands to any device
	/// it builds that holds devices. The lane defaults to Feathers' own list; the plugin entry point
	/// wires it anyway, because the list is the host's. MESSAGE THREAD, once, before provisioning.
	void setCatalogue(const device::Catalogue& catalogue) noexcept {
		lane_.setCatalogue(catalogue);
		padCatalogue_ = catalogue; // a COPY for every kit pad's lane, made later (ensurePadLane): a caller may hand a temporary
		havePadCatalogue_ = true;
	}
	/// **WHERE THE LANE READS ITS HOST SLOTS' VALUES FROM** (host slots, T6b): the plugin's bus, whose
	/// bindings ride the record. Before activate, like the catalogue.
	void setHostSlotBus(rack::RackEngine::SlotBus* bus) noexcept { lane_.setHostSlotBus(bus); }

	/// MESSAGE THREAD. Destroy everything the audio thread has retired since the last call. A device
	/// dropped on the audio thread is parked rather than destroyed — the engine's parking, and the
	/// lane's spent provisions — so this is the line that actually frees the comb's delay line, and a
	/// retirement road that stops draining is a leak nothing else can observe.
	void drainRetiredDevices();

	/// WHAT THE ENGINE IS HOLDING on the two non-parameter lanes. Exposed for the venue that backlog
	/// item 1 was about: the block that carries these had no test, because every fixture performed
	/// the copy by hand and deleting the line under test changed nothing any case could see. A case
	/// that asks the ENGINE what arrived cannot be satisfied by a fixture that set it directly.
	/// **WHERE EACH SOUND WAS PLACED ON THE LAST CHARACTER BUILD — authored travel plus what modulation
	/// added.** A value seam for the reason `voicePitchForTest` is one (CLAUDE.md: a claim about a value
	/// cannot be inferred from a rendered buffer): "a modulator moved the sound to X" is a statement
	/// about two numbers, and a render comparison passes whenever anything at all differs. Stamped BY
	/// `updateCharacter` from the very struct it hands `characterFromParams`, so it cannot be had
	/// without the reader having run.
	[[nodiscard]] const SoundTravel& playedTravelForTest() const noexcept { return playedTravel_; }
	/// The Resonate engine's one string, to read its state as a value — the laws it was handed, its
	/// energy, a voice's scan of it — rather than inferring them from a buffer (CLAUDE.md).
	[[nodiscard]] const WeaveString& weaveForTest() const noexcept { return weave_; }
	/// How many sounds' string laws the synth has BUILT through its memo (backlog LZ): a skipped build does not
	/// move it.
	[[nodiscard]] uint64_t weaveLawsBuiltForTest() const noexcept { return weaveMemo_.built; }
	/// The drawer's route scans and `modulated_`'s copies from `rack_` so far (the idle-overhead pass): none at rest.
	[[nodiscard]] uint64_t drawerScansForTest() const noexcept { return drawerScans_; }
	[[nodiscard]] uint64_t modulatedCopiesForTest() const noexcept { return modulatedCopies_; }
	/// The drawer's two patch-wide answers as the last block decided them (`applyParamsRoad`).
	[[nodiscard]] bool drawerVariesForTest() const noexcept { return drawerVaries_; }
	[[nodiscard]] bool drawerPairLiveForTest() const noexcept { return drawerPairLive_; }
	/// The crossfade the string was last handed (buildWeaveFor's cf): the value the puck's motion moves.
	[[nodiscard]] float weaveCrossfadeForTest() const noexcept { return weave_.crossfade(); }
	/// What modulation added to each sound's place on the last character build, in polar. Published to
	/// the editor once a block so the Explore star is drawn where the sound is PLAYED. Audio thread.
	[[nodiscard]] const TravelPulls& travelPulls() const noexcept { return travelPulls_; }
	/// **ANSWER THE EDITOR'S AS-MODULATED READOUT** (backlog PU) — from whichever resolve the destination is in: a
	/// device's control from the lane engine's own inputs (`RackEngine::probeRoute`), a core parameter from
	/// `ParamSet`'s (`ParamSet::probeRoute`). Audio thread.
	void answerRouteProbe(backplane::RouteProbeChannel& channel) const noexcept {
		backplane::Source source;
		backplane::Destination destination;
		if (!channel.watched(source, destination)) {
			return;
		}
		if (destination.isDevice()) {
			lane_.engineForTest().answerRouteProbe(channel);
			return;
		}
		backplane::RouteProbe p;
		if (params_.probeRoute(destination, source, p)) {
			channel.publish(source, destination, p);
		}
	}
	const ModRack& modRackForTest() const noexcept { return rack_; }
	const FxChain& fxChainForTest() const noexcept { return fxChain_; }

	/// THE RACK AS MODULATION HAS ACTUALLY LEFT IT — every control with the offsets reaching it
	/// applied, which is what the engine ticked the envelopes with this block.
	///
	/// PUBLISHED RATHER THAN RECOMPUTED EDITOR-SIDE, and that is the whole point. The editor could in
	/// principle work this out for itself — it knows the routes, and the source readings could be
	/// published too — but it would be a second implementation of ParamSet::rackOffsetAt living on
	/// the other side of a thread boundary, free to disagree with the one that makes the sound. This
	/// is the same shape ParamBridge::publish already uses for parameters, where `publishedMod_` is
	/// the engine's own resolved() rather than the editor's arithmetic.
	///
	/// Costs nothing to produce: tickEnvelopes computes these settings anyway, because it has to tick
	/// the envelopes WITH them. This just keeps the copy.
	const ModRack& modulatedRack() const noexcept { return modulated_; }

	/// **THE FX LANE AFTER MODULATION — `modulatedRack()`'s twin, and kept on the same argument.**
	///
	/// **READ BACK FROM THE LANE'S ENGINE**, which resolves the lane's devices itself through Feathers'
	/// published routes — so this is the number each device was handed, not a second computation of it.
	/// It used to be `modulatedRowAt`'s output, which the runner was then driven with.
	///
	/// **SO THE EDITOR CAN DRAW WHERE MODULATION LEFT AN FX CONTROL WITHOUT RECOMPUTING IT.** That
	/// recomputation is what synth.h's own note above `rackOffsetAt` warns off — a second
	/// implementation of the engine's arithmetic on the far side of a thread boundary, free to drift.
	/// The rack has had this since it was written and the lane simply never got it.
	///
	/// Costs nothing to produce, for exactly the reason stated at `modulatedRack()`.
	const FxChain& modulatedFxChain() const noexcept { return modulatedFx_; }

	/// **THE RIGHT EAR OF ONE FX POSITION'S CONTROLS** (backlog KU): fills `out[c]` for every control a stereo route
	/// reaches and returns which ones. An empty mask — a mono position — writes nothing.
	device::ControlMask fxRightEar(size_t position, float* out, size_t n) const noexcept;
	/// And of one strip slot's, on the same terms.
	device::ControlMask stripRightEar(size_t slot, float* out, size_t n) const noexcept;

	/// WHERE EVERY RACK MODULATOR STANDS, one reading per slot, as of the last tick.
	///
	/// **THE ENGINE'S OWN, LIKE `modulatedRack()` BESIDE IT** — the editor draws a playhead from this
	/// rather than running a second copy of a shape to find out where the first one got to, which is
	/// the same argument the modulated rack makes about the bars.
	///
	/// A slot holding neither device, or a device that has finished, reads `Playhead::kIdle`.

	/// **HOW HARD EACH FX SLOT IS BEING DRIVEN**, refreshed per block from the devices that publish it
	/// (`AudioOps::drivePeak`). A slot whose device has no meter keeps the negative sentinel, so the
	/// bridge can tell "not published" from "silent" — see `ParamBridge::publishFxDrivePeak`.
	///
	/// GATHERED HERE RATHER THAN READ ACROSS THE THREAD BY THE UI, because the runner's storage is the
	/// audio thread's and the atomics in the bridge are the seam. Same shape as `rackPlayheads`, which
	/// is why it travels on the same publish.
	const FxDrivePeaks& fxDrivePeaks() const noexcept { return fxDrivePeaks_; }
	/// **AND HOW LOUD EACH FX POSITION'S DEVICE IS COMING OUT** — every card's courtesy output meter (backlog MK):
	/// `RackEngine::outputPeakAt`, linear and unclamped, negative where the position meters nothing. Gathered beside
	/// the drive peaks, on the same terms.
	const FxDrivePeaks& fxOutputPeaks() const noexcept { return fxOutputPeaks_; }
	const RackPlayheads& rackPlayheads() const noexcept { return playheads_; }
	/// **AND WHERE EACH FX POSITION'S MODULATOR STANDS** — a lane holds modulators since 2026-10-04, and a lane
	/// MSEG or LFO draws its marker from this as a strip one draws it from `rackPlayheads`. Idle for a position
	/// holding an audio device, or nothing. One per position, sized and grown like `fxDrivePeaks`.
	const RackPlayheads& fxPlayheads() const noexcept { return fxPlayheads_; }
	/// **THE EXTRA STATE A STRIP SLOT'S OR A LANE POSITION'S DEVICE CHANGED ITSELF** (backlog MX): its serial (0 for none),
	/// `*count` floats in `extra`, and which device it is about. The audio thread, beside the playheads.
	uint64_t stripExtraState(size_t slot, float* extra, size_t capacity, size_t* count, DeviceInstanceId* device) const;
	uint64_t fxExtraState(size_t position, float* extra, size_t capacity, size_t* count, DeviceInstanceId* device) const;

	/// **THE MASTER METER** (backlog DN): the bus utility's K-weighted meter over this synth's OUTPUT —
	/// after the master gain, so it reads what leaves the plugin. Fed at the end of every rendered chunk;
	/// RESET on the transport's rising edge into play, so a clip from the last
	/// take is not still glowing into this one. Read on the audio thread's own side and published across
	/// by the plugin beside the drive peaks (`master_meter.h`).
	const fx::busutil::Meter& masterMeter() const noexcept { return masterMeter_; }

	/// WHAT CAME OUT OF THE NOTE LANE ON THE LAST CONTROL TICK — a test-only seam, and it exists for
	/// CLAUDE.md's reason. A note that reached a device leaves no trace in any rendered buffer under
	/// reading (1), because nothing downstream plays it; "the notes got there" is exactly the kind of
	/// claim that must be read rather than inferred from a render.
	/// **THE NOTES THIS CONTROL TICK OFFERED THE LANE**, which is what a case can assert against. It
	/// reports the tick's INPUT rather than the lane's output, and that is deliberate: since the
	/// voices now play what leaves the lane, the honest assertion about the output is on the VOICES
	/// themselves (`voicePitchForTest` and its neighbours), not on a buffer only tests read.
	[[nodiscard]] std::span<const device::NoteEvent> laneNotesForTest() const noexcept {
		return { laneNotes_.data(), laneNoteCount_ };
	}

	/// **ONE EVENT THE VOICES WERE HANDED, AND THE SAMPLE IT WAS HANDED AT** — samples rendered since this synth was
	/// built, so a case reads an arp step's onset by value rather than from a buffer (CLAUDE.md, "assert the value").
	struct DeliveredNote {
		device::NoteEvent event {};
		uint64_t sample = 0;
	};
	static constexpr size_t kDeliveredLog = 1024;
	/// TEST SEAM: every event delivered since `clearDeliveredForTest`, oldest first — at most the newest
	/// `kDeliveredLog`, and `deliveredOverflowForTest` says whether older ones were lost.
	[[nodiscard]] std::vector<DeliveredNote> deliveredForTest() const {
		std::vector<DeliveredNote> out;
		const uint64_t first = deliveredCount_ > kDeliveredLog ? deliveredCount_ - kDeliveredLog : 0;
		for (uint64_t i = first; i < deliveredCount_; ++i) {
			out.push_back(deliveredLog_[static_cast<size_t>(i % kDeliveredLog)]);
		}
		return out;
	}
	[[nodiscard]] bool deliveredOverflowForTest() const noexcept { return deliveredCount_ > kDeliveredLog; }
	void clearDeliveredForTest() noexcept { deliveredCount_ = 0; }
	/// Samples rendered since construction — the clock `deliveredForTest` stamps with.
	[[nodiscard]] uint64_t samplesRenderedForTest() const noexcept { return samplesRendered_; }

	/// **WHAT SLOT `slot` PUBLISHES AS A SOURCE — the gated number, not the device's own output.**
	///
	/// `envelopeValueForTest` is the DEVICE's output and is deliberately ungated: a bypassed modulator
	/// keeps running and that seam is how a case asks what it is doing. This is the other quantity —
	/// what a destination reads — and the two differ by exactly the bypass. A case that wants to
	/// assert the switch works must read THIS one; asserting the device seam measures the wrong thing
	/// and fails against a working gate, which is how this seam came to exist.
	[[nodiscard]] float publishedSourceForTest(size_t slot) const { return publishedSourceAt(slot); }

	/// THE ENGINE THE LANE AND THE STRIP RUN ON — for a case asking what a lane holds. A device the
	/// registry does not carry is seated the way a third party's is: a catalogue that knows it
	/// (`setCatalogue`), and a row naming it in the chain.
	[[nodiscard]] const LaneRack& laneForTest() const noexcept { return lane_; }
	/// See `LaneRack::setEagerVoicesForTest`. Before the first provision.
	void setEagerVoicesForTest(bool eager) noexcept { lane_.setEagerVoicesForTest(eager); }

	/// TEST ONLY. Where an envelope stands, 0..1 or -1..1 — the fourth of these seams and the fifth
	/// value accessor on this class.
	///
	/// IT EXISTS FOR THE REASON THE OTHER FOUR DO, which the project states as a rule: a claim about
	/// a VALUE cannot be inferred from a rendered buffer. Every existing case here proves it again —
	/// an envelope pointed at the puck changes the sound, but so does every other thing this synth
	/// does, and "the render differs" passes whenever anything differs. Worse, the failure that
	/// matters is the PARTIAL one: an envelope that reaches half its depth, or one tick late, or
	/// scaled by the wrong amount, renders a sound that is different from the unmodulated one in
	/// exactly the way a correct envelope would. Five lines here; no amount of cleverness with
	/// buffers recovers the number.
	float envelopeValueForTest(size_t slot) const;

	/// TEST ONLY. What a slot's MSEG is SET TO once its routes have been applied — the settings the
	/// tick runs on, before it runs on them.
	///
	/// NOT RECOVERABLE FROM THE SHAPE IT DRAWS, which is the same reason the accessor above exists.
	/// A route on Rate and a route on the segment count both make the output differ from the
	/// unmodulated one, both stay in range, and both look like a shape being modulated; only the
	/// number says which control moved. This seam was added because a bug did exactly that — see
	/// modulatedMsegSettings for the descriptor that named the wrong control.
	mod::mseg::Settings modulatedMsegSettingsForTest(size_t slot) const {
		return modulatedMsegSettings(slot);
	}

	/// TEST ONLY. Whether the note stack is holding the envelope gate open — the other half of the
	/// same claim, because a value that never moved and a gate that never opened look identical from
	/// outside and have entirely different causes.
	bool envelopeGateForTest() const;

	/// Voices making sound, including those in a release tail or a steal fade. Zero means the output
	/// is exactly silent and will stay that way until the next note.
	int soundingVoiceCount() const;

	/// Voices the manager still counts as live — which excludes one that has been stolen and is only
	/// fading out. Tests use it to talk about polyphony rather than about audio.
	int managedVoiceCount() const;

	/// TEST ONLY. **ONE SOAK READING BY VALUE** (`clap_feathers_synth_reading`, feathers_test_chain.h): a count of something
	/// that must stay flat over a long session. False for an unknown `what`. Between blocks.
	bool soakReadingForTest(uint32_t what, double* out) const;

	/// TEST ONLY. The slot playing `key`, ignoring voices that are only fading out of the way, or -1.
	int slotOfKeyForTest(int16_t key) const;
	/// TEST ONLY — the sounding, non-stealing voice started most recently (highest `startSerial`), or -1.
	/// Not `slotOfKeyForTest`: a re-pressed key has TWO voices on it while the old one releases.
	int newestVoiceSlotForTest() const;

	/// **THE SPECTRUM DRAWER'S PLOT: WHAT IS HEARD, INTO `out`** (additive design §4.2, D2; spectral_plot.h). The newest
	/// sounding voice's partials as its bank renders them (stack part 0: OscVoice::plotPartials); with no note, the main
	/// bank's spectrum at the last played note's pitch (A2 before any) through the drawer the next note would get
	/// (OscVoice::plotReference, whose gain caches live here, so a held drawer recomputes nothing). Zero partials off
	/// Additive. The plugin asks for it only while the editor's drawer is open (ParamBridge::plotWanted), from
	/// publishEngineReadings — the audio thread, or a flush, which never overlaps it; the caches are that thread's alone.
	void plotFrame(SpectralPlotFrame& out) const;
	/// TEST ONLY. The no-note plot's drawer work, per ear (0 left, 1 right): GainCache's counts, so a case can see that a held
	/// drawer recomputes nothing however often the plot is read.
	spectral::GainCache<partials::kMaxPartials>::Work plotWorkForTest(int ear) const {
		return (ear != 0 ? plotCacheR_ : plotCache_).workForTest();
	}

	/// TEST ONLY — a voice's per-note level offset, which is the ONLY thing a per-note-id modulation
	/// may reach. The claim is "a modulation addressed to a note lands on Level and on nothing else",
	/// and that is a statement about a VALUE: a render would show a level change either way, because
	/// a per-note modulation folded onto the wrong parameter still moves the sound.
	double voiceLevelModForTest(int slot) const;
	/// The gain the last render applied to the voice in `slot` — Level resolved for that voice, plus its
	/// level mod, clamped to [0, 1]. See `Voice::gain`.
	double voiceGainForTest(int slot) const;
	/// TEST ONLY — every per-sample Level gain the next renders apply to `slot`, appended to `trace`
	/// (nullptr stops it). The claim about a ramp is a claim about a value on each sample.
	void traceVoiceGainForTest(int slot, std::vector<float>* trace) {
		gainTraceSlot_ = slot;
		gainTrace_ = trace;
	}

	/// TEST ONLY. Where a voice's pitch has got to, in semitones.
	///
	/// Read directly because the glide is a rule with a stated shape — an exponential approach in
	/// semitone space with the glide time as its constant — and a spectrum can only find a moving
	/// peak by smearing it. The audio side of the same claim is measured where the glide has
	/// finished and the note is stationary, which is what a spectrum is good at.
	double voicePitchForTest(int slot) const;
	/// TEST ONLY. The voice edge's level (`Voice::level`), 0 to 1: the amplitude envelope every note-on restarts and
	/// a legato move does not. A restart is a claim about this value, read rather than inferred from a buffer.
	float voiceEdgeLevelForTest(int slot) const;

	/// **TEST ONLY. THE DRUM ENGINE'S VALUES** (drum proposal §9): which pad a voice plays and its hit (its kind, laws, seed,
	/// modes), the kit as last resolved, its stage, and how many chokes and drum-ended voices there have been.
	uint8_t voiceDrumPadForTest(int slot) const { return voices_[static_cast<size_t>(slot)].osc.drumPad(); }
	const drum::Hit& voiceDrumHitForTest(int slot) const { return voices_[static_cast<size_t>(slot)].osc.drumHitForTest(); }
	uint32_t voiceDrumStrikesForTest(int slot) const { return voices_[static_cast<size_t>(slot)].osc.drumStrikesForTest(); }
	const drum::Hit& voiceDrumLayerForTest(int slot) const { return voices_[static_cast<size_t>(slot)].osc.drumLayerForTest(); }
	uint32_t voiceDrumLayerStrikesForTest(int slot) const { return voices_[static_cast<size_t>(slot)].osc.drumLayerStrikesForTest(); }
	float voiceDrumLayerHighPassForTest(int slot) const { return voices_[static_cast<size_t>(slot)].osc.drumLayerHighPassForTest(); }
	Voice::Stage voiceStageForTest(int slot) const { return voices_[static_cast<size_t>(slot)].stage; }
	int voiceKeyForTest(int slot) const { return voices_[static_cast<size_t>(slot)].key; }
	int voiceNoteIdForTest(int slot) const { return voices_[static_cast<size_t>(slot)].noteId; }
	const drum::KitSetup& drumKitForTest() const { return drum_->kit; }
	uint32_t drumChokesForTest() const { return drum_->chokes; }
	uint32_t drumEndedForTest() const { return drum_->ended; }

	/// **TEST ONLY. One stacked part's pitch, fan position, gain and liveness** (backlog KE) — the stack's claims
	/// are all about values (where each part stands in the fan, how loud it is mid-fade), and CLAUDE.md's rule
	/// is that such a claim is read, never inferred from a buffer. Part 0 is `voicePitchForTest`'s oscillator.
	double voiceStackPitchForTest(int slot, int part) const;
	double voiceStackFanForTest(int slot, int part) const;
	float voiceStackGainForTest(int slot, int part) const;
	bool voiceStackLiveForTest(int slot, int part) const;
	/// TEST ONLY. Where a part sits in the stereo field now, −1..+1 (backlog KE step 2).
	float voiceStackPanForTest(int slot, int part) const;
	/// TEST ONLY. The two channel gains the render applies to that part at that pan — the pan law, read back.
	void voiceStackChannelGainsForTest(int slot, int part, float* toLeft, float* toRight) const;
	/// TEST ONLY. A part's Stack Index, and its share of a Stack Index route into Level (step 3).
	float voiceStackIndexForTest(int slot, int part) const;
	float voiceStackLevelScaleForTest(int slot, int part) const;
	/// TEST ONLY. The stand a part reads (−1: the puck), a stand slot's offset and its bank, and the overflow count.
	int voiceStackStandForTest(int slot, int part) const {
		return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].stand;
	}
	std::array<double, 3> standOffsetForTest(int stand) const { return standOffset_[static_cast<size_t>(stand)]; }
	/// TEST ONLY. The parts' 0-cent coherence as the voice last computed it (`NoteContext::stackCoherence`), and the
	/// same computation forced afresh — to time it, and to check the cached value against it.
	float voiceStackCoherenceForTest(int slot) const { return voices_[static_cast<size_t>(slot)].stackCoherence; }
	/// TEST ONLY. The reading the render would take now — cached, so it reads no cycle unless a part's table moved.
	float updateStackCoherenceForTest(int slot) {
		updateStackCoherence(voices_[static_cast<size_t>(slot)]);
		return voices_[static_cast<size_t>(slot)].stackCoherence;
	}
	/// TEST ONLY. How many part cycles have been read for the coherence, ever — the cost, and the cache, counted.
	uint64_t coherenceCycleReadsForTest() const noexcept { return coherenceCycleReads_; }
	/// TEST ONLY. Every part's cycle read again at once — the reading the amortised one converges to.
	float recomputeStackCoherenceForTest(int slot) {
		Voice& voice = voices_[static_cast<size_t>(slot)];
		voice.coherenceKey = {};
		updateStackCoherence(voice, true);
		return voice.stackCoherence;
	}
	bool widthStandsForTest() const noexcept { return widthStands_; }
	const TableSource& standBankForTest(int stand) const { return bankForStand(stand); }
	const StandBank& standSlotForTest(int stand) const { return (*standBanks_)[static_cast<size_t>(stand)]; }
	static constexpr int standPoolForTest() { return kStandPool; }
	const CharacterBank& mainBankForTest() const { return bank_; }
	/// THE SPIKE RECORDER'S TWO READS (backlog SR): the main bank's table count and the likes' warp serial.
	uint64_t bankTablesMade() const noexcept { return bank_.tablesMade(); }
	uint64_t likeWarpSerial() const noexcept { return likeSourceSerial_; }
	int standOverflowForTest() const noexcept { return standOverflow_; }
	/// TEST ONLY. One part's oscillator, read-only — its bakes, its grids, its lender (step 4).
	const OscVoice& voiceStackOscForTest(int slot, int part) const {
		return partOsc(voices_[static_cast<size_t>(slot)], part);
	}
	/// TEST ONLY. Whether the drawer was stereo-live at the last chunk: Additive, the master switch on, and a pair routed
	/// into a sound's S control or ears whose sound weights differ — what puts the patch on the stereo road for it.
	bool drawerStereoLiveForTest() const noexcept { return drawerStereoLive_; }
	/// TEST ONLY. The drawer the synth assembles for `slot` (`voice` < 0: the collapsed reading every voice shares when
	/// nothing per voice reaches the drawer), for one ear — Synth::drawerFor, which seatDrawers hands each oscillator.
	spectral::ShaperParams drawerForTest(int voice, bool rightEar) const { return drawerFor(voice, -1, rightEar); }
	/// TEST ONLY: the A-PH optimiser service (its request slot, its latest profile, its counters).
	PhaseService& phaseServiceForTest() { return *phases_; }
	/// TEST ONLY. The weights each ear's drawer is blended by (0 left, 1 right).
	const SoundWeights& drawerWeightsForTest(int ear) const { return earWeights_[ear != 0 ? 1 : 0]; }
	/// TEST ONLY. The weights a part on `stand` has its drawer blended by.
	const SoundWeights& standWeightsForTest(int stand) const { return standWeights_[static_cast<size_t>(stand)]; }
	uint32_t drawerSeatingsForTest() const noexcept { return drawerSeatings_; }
	/// TEST ONLY. The polyphony the voice manager was last handed — Voice Count under the stack's guard.
	int appliedVoiceLimitForTest() const noexcept { return appliedVoiceCount_; }

	/// **TEST ONLY. What note expression `expression` has left on the voice in `slot`.**
	///
	/// Here for the reason `voicePitchForTest` and `voiceFilterTargetForTest` are, which CLAUDE.md
	/// states as a rule: a claim about a VALUE cannot be inferred from a rendered buffer. Pressure has
	/// no audible consequence at all until a patch routes it somewhere, so there is not even a signal
	/// to fail to infer it from — without this seam, "the host's expression reached the voice" is
	/// unassertable rather than merely hard.
	///
	/// Zero for an unknown slot or an id this build does not know, which is the same answer an
	/// unexpressed voice gives; the cases that care assert a NON-zero value they put there.
	double voiceExpressionForTest(int slot, int32_t expression) const;

	/// TEST ONLY. Lengthen the voice's fixed de-click edges, so a case can land a note inside a Release or an
	/// Attack the way the deleted Attack/Release knobs once let it. The stages are real in a shipping build —
	/// a routed tail holds a voice in Release — but at 2 ms they are too short to aim an event into.
	void setVoiceEdgesForTest(double attackSeconds, double releaseSeconds) {
		attackEdgeSeconds_ = attackSeconds;
		releaseEdgeSeconds_ = releaseSeconds;
	}

	/// TEST ONLY. The offset Phase put on a voice's READ at its last evaluation, in cycles, wrapped to 0..1 —
	/// beside the accumulator `voicePhaseForTest` reads, which Phase never moves.
	double voiceReadOffsetForTest(int slot) const {
		return slot < 0 || slot >= kVoiceSlots ? 0.0 : voices_[static_cast<size_t>(slot)].osc.readOffsetForTest();
	}

	/// TEST ONLY. The right ear's offset and read phase at the voice's last STEREO evaluation (Phase Width, Q10).
	double voiceReadOffsetRightForTest(int slot) const {
		return slot < 0 || slot >= kVoiceSlots ? 0.0 : voices_[static_cast<size_t>(slot)].osc.readOffsetRightForTest();
	}
	double voiceReadPhaseRightForTest(int slot) const {
		return slot < 0 || slot >= kVoiceSlots ? 0.0 : voices_[static_cast<size_t>(slot)].osc.lastReadPhaseRightForTest();
	}
	/// TEST ONLY. Random Fresh's seed and Drift's stream state for a voice — the note's draws (Q9).
	std::pair<uint32_t, uint32_t> voiceStackDrawsForTest(int slot) const {
		if (slot < 0 || slot >= kVoiceSlots) {
			return { 0u, 0u };
		}
		return { voices_[static_cast<size_t>(slot)].freshSeed, voices_[static_cast<size_t>(slot)].driftRng };
	}

	/// TEST ONLY. The phase a voice's tables were last READ at (accumulator + Phase's offset, before wrapping).
	double voiceReadPhaseForTest(int slot) const {
		return slot < 0 || slot >= kVoiceSlots ? 0.0 : voices_[static_cast<size_t>(slot)].osc.lastReadPhaseForTest();
	}

	/// TEST ONLY. Where a voice's oscillator phase stands, 0..1.
	///
	/// Read directly for the reason the pitch above is: the transport lock's whole claim is about
	/// ONE NUMBER at ONE INSTANT — the phase a note-on is given — and inferring it from a rendered
	/// buffer cannot isolate it. Two renders differ for every reason at once, so a comparison
	/// between them passes whenever ANYTHING differs, which is how two drafts of these cases came to
	/// pass while the thing they were about was mutated away.
	double voicePhaseForTest(int slot) const;
	/// And the string mip that voice scans on, so band-limiting is asserted as the value it is.
	/// (part: which of the voice's stacked oscillators; 0 is the voice's own.)
	int voiceWeaveMipForTest(int slot, int part = 0) const;
	int voiceWeaveFadeCountForTest(int slot, int part = 0) const;
	WeaveString::Fade voiceWeaveFadeReadForTest(int slot, int part = 0) const;
	/// Where the voice's right ear reads the ring (cycles from the left's): Resonate's Width, as a value.
	double voiceWeaveTapForTest(int slot, int part = 0) const;
	/// TEST ONLY. Where a voice's FM MODULATOR phase stands, 0..1 — the fourth of these seams, added
	/// for the reason the three above give and which is now a house rule: the claim is that a note-on
	/// puts ONE NUMBER at zero, and no comparison of two renders can isolate it. Two notes played with
	/// a free-running modulator differ anyway, which is exactly how a case pinning "the modulator
	/// restarts" would pass on a build where it does not.
	double voiceModulatorPhaseForTest(int slot) const;
	/// TEST ONLY. The settings a voice's own filter was last handed, key offset included — what the
	/// keytracking architecture actually asked of the DSP, rather than what a test recomputes beside
	/// it. Meaningful only after a render, since the offset is set per chunk.
	fx::filter::Settings voiceFilterTargetForTest(int slot) const;
	/// **TEST ONLY. Whether the last chunk rendered its voices in stereo** (backlog KU: a per-voice device with a
	/// stereo route forces it). A value, because a render cannot say which road made it.
	[[nodiscard]] bool lastChunkStereoForTest() const noexcept { return lastChunkStereo_; }

	/// WHAT THE BUS FILTER IS RUNNING, for tests only.
	///
	/// The companion to voiceFilterTargetForTest, and it exists for the same reason: a claim about a
	/// corner cannot be recovered from a rendered buffer. It became necessary when the filter's
	/// settings stopped being readable from the parameter table — a case asserting the bus corner had
	/// been reading ParamSet::filterSettings, which after the swallowing describes nothing the engine
	/// renders from.
	/// ASKED OF THE POOL, NOT OF A MEMBER, since the bus filter is now whatever the lane holds. It
	/// walks to the first slot actually holding a filter rather than assuming slot 0 — a lane is
	/// reorderable, so "the filter" has no fixed address any more.
	fx::filter::Settings busFilterTargetForTest() const;

	/// The live storage of the first bus device carrying `tag`, or nullptr. Generic on purpose: Synth
	/// has no business knowing which devices exist, and a test that wants one asks by tag and decodes
	/// with that device's own accessor.
	const void* fxStorageForTagForTest(const char* tag) const;

	/// THE SAME, FOR ONE VOICE'S OWN COPY — the per-voice arrangement's half of the seam above.
	///
	/// IT IS THE ONLY WAY TO ASK THE QUESTION THIS FEATURE TURNS ON. "Poly on sounds different from
	/// poly off" is a difference check and passes for any bug that changes anything; what per-voice
	/// HARM actually claims is that there are now N sub-oscillators AT N DIFFERENT FREQUENCIES, one
	/// per sounding note, where the bus arrangement has exactly one at the lowest. That is a claim
	/// about VALUES — a frequency per voice — and no comparison of two rendered buffers can recover
	/// one frequency from a signal carrying a notch, an envelope and a sine at once.
	///
	/// Null for a slot holding nothing, or while no device of that tag is in its per-voice
	/// arrangement — which is itself the assertion that the two runners take complementary halves.
  const void* voiceFxStorageForTest(int slot, const char* tag) const;

  /// SET THE OWLTT METER CONNECTOR — the plugin calls this to provide a function that connects a
  /// OWLTT DSP to the meter system. The runner calls this function after constructing each device.
  void setDspConnector(std::function<void(const char* tag, size_t slot, void* dspStorage)> connector);

private:
	struct VoiceManagerConfig {
		using voice_t = Voice;
		static constexpr size_t maxVoiceCount = static_cast<size_t>(kMaxVoices);
	};

	/// The voice-level half of the voice manager's contract. Every method is called from inside a
	/// note event, on the audio thread.
	struct Responder {
		Synth& synth;

		void setVoiceEndCallback(std::function<void(Voice*)> callback);
		int32_t initializeMultipleVoices(
		    int32_t voices,
		    const typename sst::voicemanager::VoiceInitInstructionsEntry<VoiceManagerConfig>::buffer_t&
		        instructions,
		    typename sst::voicemanager::VoiceInitBufferEntry<VoiceManagerConfig>::buffer_t& working,
		    uint16_t port, uint16_t channel, uint16_t key, int32_t noteId, float velocity, float retune);
		int32_t beginVoiceCreationTransaction(
		    typename sst::voicemanager::VoiceBeginBufferEntry<VoiceManagerConfig>::buffer_t& buffer,
		    uint16_t port, uint16_t channel, uint16_t key, int32_t noteId, float velocity);
		void endVoiceCreationTransaction(uint16_t port, uint16_t channel, uint16_t key, int32_t noteId,
		                                 float velocity);
		void terminateVoice(Voice* voice);
		void releaseVoice(Voice* voice, float velocity);
		void retriggerVoiceWithNewNoteID(Voice* voice, int32_t noteId, float velocity);
		void moveVoice(Voice* voice, uint16_t port, uint16_t channel, uint16_t key, float velocity);
		void moveAndRetriggerVoice(Voice* voice, uint16_t port, uint16_t channel, uint16_t key,
		                           float velocity);
		void discardHostVoice(int32_t voiceId);
		void setNoteExpression(Voice* voice, int32_t expression, double value);
		void setVoicePolyphonicParameterModulation(Voice* voice, uint32_t parameter, double value);
		void setVoiceMonophonicParameterModulation(Voice* voice, uint32_t parameter, double value);
		void setPolyphonicAftertouch(Voice* voice, int8_t value);
		void setVoiceMIDIMPEChannelPitchBend(Voice* voice, uint16_t bend);
		void setVoiceMIDIMPEChannelPressure(Voice* voice, int8_t pressure);
		void setVoiceMIDIMPETimbre(Voice* voice, int8_t timbre);
	};

	/// The half of the contract that is about a whole MIDI channel rather than a voice.
	struct MonoResponder {
		Synth& synth;

		void setMIDIPitchBend(int16_t channel, int16_t bend14bit);
		void setMIDI1CC(int16_t channel, int16_t cc, int8_t value);
		void setMIDIChannelPressure(int16_t channel, int16_t pressure);
	};

	using VoiceManager = sst::voicemanager::VoiceManager<VoiceManagerConfig, Responder, MonoResponder>;

	/// Read the params and push anything that changed into the bank and the voice manager. Once per
	/// render call, which is once per event slice.
	void applyParams();
	/// **`applyParams` ON A CONTROL TICK INSIDE A RENDER** (backlog FI): the same, less the convergence — the chains move
	/// only at a drain, which is the top of a render call, so re-converging on every tick found nothing to do, measured.
	void applyParamsOnTick();
	void applyParamsRoad(bool converge);
	/// What a note-on does to the running phase, by the table's own stepped rule — the one question
	/// all three retrigger roads ask.
	OscMode oscMode() const;
	/// The character alone, rebuilt when the puck's velocity crosses a tick. See render().
	void updateCharacter();
	/// Hands the one string this character's laws, crossfade and model, on every engine (see the .cpp).
	void pushWeaveLaws(const Character& c);
	/// THE LAWS A FROZEN STRING IS OWED, built now (`weaveLawsOwed_`): before anything reads them — a string reset.
	void settleWeaveLaws();
	/// Resonate's nine engine-wide knobs, as the engine reads them (see the .cpp).
	ResonateShaping resonateShaping() const;
	/// Plucks the one string for a note that starts sounding (see the .cpp).
	void pluckForNoteOn();
	/// THE KARPLUS-STRONG PLUCK for `voice`: every live part plucked with its own seed, hash(Seed, key, channel, part, and the
	/// note-on's song tick — or, on a stopped transport, the key's strike count), never anything incidental (KS design §6).
	void pluckStrings(Voice& voice);
	/// THE DRUM STRIKE for `voice` on pad `pad` (drum proposal §7): every live part struck with its own seed, hash(Seed, pad or
	/// key, part, and the note-on's song tick — or, on a stopped transport, the pad's strike count), never anything incidental.
	void strikeDrums(Voice& voice, uint8_t pad);
	PadHits padHits_ {};
	// THE PER-PAD FX LANES (drum proposal §17): owned on the message thread, published, taken per block on the audio thread.
	struct PadLane {
		std::unique_ptr<rack::InnerChain> owned;
		std::atomic<rack::InnerChain*> published { nullptr };
		rack::InnerChain* running = nullptr; ///< AUDIO THREAD: the lane taken at this block's start
		bool live = false;                   ///< AUDIO THREAD: it holds a device that is on, so its pad's voices go through it
		uint32_t latency = 0;                ///< AUDIO THREAD: what its devices declare this render call (live lanes)
	};
	std::array<PadLane, kMaxSounds> padLanes_ {};
	bool padLanesLive_ = false; ///< AUDIO THREAD: any pad lane live this render call
	// THE PADS' LATENCY ALIGNED (drum proposal §17 P4; the lead: "a late pad is a real defect"): while any live pad lane is
	// latent, every live pad's bus is delayed to the latest one, and the voices that sum straight into the mix by all of it;
	// the plugin reports it (`latencySamples`). Rings prepared on the MESSAGE THREAD (activate, ensurePadLane).
	std::array<device::CompensationDelay, kMaxSounds> padAlign_ {};
	device::CompensationDelay directAlign_ {};
	uint32_t padLatency_ = 0; ///< AUDIO THREAD: the latest live pad lane's latency this render call
	std::vector<float> padBusL_, padBusR_; ///< kMaxSounds × kRenderChunk each, sized at activate
	device::Catalogue padCatalogue_ {};
	bool havePadCatalogue_ = false;
	double padLaneRate_ = 0.0;
	int padLaneFrames_ = 0;
	[[nodiscard]] bool padLanesHoldTail() const noexcept;
	void beginPadLanes();
	void endPadLanes();
	void runPadLanes(float* left, float* right, int n);
	/// The kit as the params resolve now, into `drum_->kit` (every voice reads it through one pointer).
	void fillDrumKit();
	/// THE CHOKES this note-on asked for (D6), applied once the voice manager has returned (never inside its transaction).
	void applyDrumChokes();
	/// A ONE-SHOT HIT THAT HAS RUN OUT (Attack or Sustain, its output exactly silent): the lane is told the note is over and the
	/// voice releases into silence, which the ordinary release road then finishes and reports to the manager.
	void endDrumVoice(Voice& v);
	/// The engine-wide string controls, as the params resolve now.
	ks::StringControls stringControls() const;

	/// Push the voice manager's settings that come from params, when they have changed.
	///
	/// Called from the note entry points as well as from render(), and it has to be: setPlaymode
	/// hard-terminates every voice in the group when the mode changes, so a mode that was still
	/// waiting for the next render call would silence the note that had just arrived under it. That
	/// is also why every setting here is pushed only on a real change — calling setPlaymode every
	/// slice would silence the synth every slice.
	void syncVoiceManagerSettings();

	/// Retarget every sounding voice's pitch, gliding in the modes that glide. Also once per slice:
	/// transpose, fine tune and bend all move notes that are already down. `onTick`: a routed pitch arriving on the
	/// control tick, which a voice that does not glide reaches by a straight line over the tick (backlog FK).
	void applyPitches(bool onTick);

	/// Render one chunk of at most kRenderChunk samples into `mix`, summing every sounding voice with
	/// its edge and level applied.
	/// Run this voice's OWN COPY of every per-voice device over its decimated output, in place.
	/// `right` is null on the mono path. Called before the amplitude edge is applied, which is where
	/// an insert belongs: in front of the amplifier, not behind it.
	///
	/// IT NAMES NO DEVICE. It was `applyVoiceFilter` and it set a filter's key offset by hand; the
	/// note goes to the runner as a NoteContext now and each device folds it in with its own rule —
	/// the filter's keytrack, HARM's tracked frequency — which is knowledge the device has and the
	/// engine does not.
	void applyVoiceFx(Voice& voice, float* left, float* right, int n);

	void renderChunk(float* mix, int n);

	/// The stereo twin of renderChunk, for while the bank's stereo window is open: every voice
	/// renders both ears, and one advance of its amplitude edge applies to the pair. Kept beside the
	/// mono loop rather than folded into it — the mono loop is today's render to the bit, which is
	/// the Width-0 guarantee, so a change to either must be mirrored in the other.
	void renderChunkStereo(float* left, float* right, int n, bool earsLive);

	/// The semitone a key sounds at, with every tuning param and the channel's bend applied.
	/// **`voice` IS A VOICE SLOT OR -1 FOR THE COLLAPSED READING** — backlog DU's migration, and this
	/// is the first destination to take it. The pitch params are resolved PER VOICE already:
	/// `targetSemitonesFor(voice)` calls this once per sounding voice in `applyPitches`, so threading a
	/// voice through costs nothing that was not already being paid — which is why the migration table
	/// in backlog DU names pitch as the easiest target by a wide margin.
	///
	/// -1 for the mono-retrigger road below, which asks where the PREVIOUS key was resting and has no
	/// voice of its own to be per.
	double semitonesForKey(int16_t key, int16_t channel, double retuneSemitones,
	                       int voice = -1) const;

	/// The semitone a voice should be playing.
	double targetSemitonesFor(const Voice& voice) const;

	/// Glide time as it applies right now: zero in Poly, which is the v1 rule — a portamento between
	/// two notes only means something when there is one note at a time, and poly glide is deferred.
	double glideSecondsNow() const;

	/// Set `voice` fading to silence over `seconds`, from wherever its level currently is.
	/// THE SERIAL IS A REQUIRED ARGUMENT, not something the three call sites remember. Every road into
	/// Attack comes through here, and an Attack is exactly what has to be stamped — so making the
	/// number impossible to omit is what stops a fourth road appearing without one. The playbook's
	/// rule: make forgetting unrepresentable rather than writing a comment asking people not to.
	/// Ignored for the stages that are not a note starting.
	static void beginFade(Voice& voice, Voice::Stage stage, double seconds, double sampleRate,
	                      uint64_t attackSerial);

	/// Free slot with the lowest index, or null when every one of them is busy. Lowest index rather
	/// than round-robin so allocation is reproducible: the seed a note gets must depend on the notes
	/// played, not on how long the plugin has been open.
	Voice* claimFreeVoice();

	ParamSet& params_;
	CharacterBank bank_;
	/// Every handle at the value that behaves as if none of them were here: planes derived per sound,
	/// falloff at the power every saved patch was made on. The plugin overwrites this each block with
	/// whatever the audition channel is holding, which is usually exactly this.
	AuditionTuning tuning_ {};
	/// The one piece of state in this class that is about TIME rather than about notes: how fast the
	/// puck is moving, which is what gives the blue shift its direction. Stepped on a fixed sample grid
	/// so the answer does not depend on the host's buffer size.
	PuckMotion motion_ {};
	/// THE RESONATE ENGINE'S ONE STRING (decided 2026-08-18, task-43 decision 4): shared by every voice,
	/// plucked by every note, continuous across notes — the deliberate purity exception. Advanced once
	/// per chunk beside the bank and the puck, on its own fixed tick clock, and the render loop cuts its
	/// chunks at that tick so the string moves identically at every host block size.
	WeaveString weave_ {};
	/// THE KARPLUS-STRONG ENGINE'S SYNTH-WIDE STATE, BEHIND ONE POINTER (KS design §1.5, §6): a Synth is built on the stack
	/// by tests, several to a case, with only kilobytes to spare (MEASURED: test_terrain_stereo's five-Synth case).
	struct KsState {
		/// The pluck builder: working buffers shared by every voice, prepared at activate.
		ks::PluckBuilder pluckBuilder {};
		/// Each key's strikes since the transport last changed state or the patch loaded. NOT PERSISTED, on purpose: a
		/// playing transport draws from the song position instead, and a live take on a stopped one restarts its count at
		/// load, so the first plucks after a load are the same every time.
		std::array<uint32_t, 128> strikes {};
		/// Whether this chunk's strings have two pickups apart (Pluck Width above 0): the stereo road with live ears.
		bool stereo{false};
	};
	std::unique_ptr<KsState> ks_ = std::make_unique<KsState>();

	/// THE DRUM ENGINE'S SYNTH-WIDE STATE (drum proposal §4, §5, §7), behind one pointer for KsState's reason.
	struct DrumState {
		/// The kit every voice reads (OscVoice::setDrumKit), filled once a chunk.
		drum::KitSetup kit {};
		/// Each pad's (Kit) or key's (Pitched) strikes since the transport last changed state or the patch loaded. NOT
		/// PERSISTED, for the Pluck strike count's reason: a playing transport draws from the song position instead.
		std::array<uint32_t, 128> strikes {};
		/// THE NOTE BEING STARTED: which pads its voices play, in the order the manager asks for them.
		std::array<uint8_t, kMaxSounds> notePads {};
		int notePadCount{0};
		/// The voices this note-on started, and their pads: whose chokes to apply once the manager returns.
		std::array<Voice*, kMaxSounds> started {};
		std::array<uint8_t, kMaxSounds> startedPads {};
		int startedCount{0};
		/// What each pad group's voice limit was last set to (-1: never).
		std::array<int, kMaxSounds> appliedLimit { -1, -1, -1, -1, -1, -1, -1, -1 };
		/// TEST ONLY: chokes applied, voices the drum road ended.
		uint32_t chokes{0};
		uint32_t ended{0};
	};
	std::unique_ptr<DrumState> drum_ = std::make_unique<DrumState>();
	/// EACH SOUND'S STRING LAWS AS LAST BUILT, with what they were built from (backlog LZ): pushWeaveLaws runs on every
	/// character update on every engine, and a sound whose inputs have not moved is handed its laws back rather than
	/// rebuilt — the same laws, to the bit (WeaveLawMemo).
	WeaveLawMemo weaveMemo_ {};
	/// **THE LAWS A FROZEN STRING HAS NOT BEEN HANDED YET** (the idle-overhead pass, 2026-11-05). Off Resonate the string
	/// does not tick, and its laws are read only by `WeaveString::reset` (a reset's rest pose) — so `pushWeaveLaws` keeps
	/// the character and the shaping it would have built them from, and `settleWeaveLaws` builds them before a reset. A
	/// switch to Resonate builds them anyway, on its own tick. The character carries its likes by reference into the
	/// synth's two like tables, which flip only inside `updateCharacter`, where this copy is replaced in the same pass.
	Character weaveOwedCharacter_ {};
	ResonateShaping weaveOwedShaping_ {};
	bool weaveLawsOwed_ = false;
	/// Where the string's fade stands at each sample of the chunk being rendered, filled ONCE per chunk
	/// and read by every voice (OscVoice::setWeaveFades). Sized for the chunk plus the up-to-three
	/// samples a voice's decimator generates past it.
	std::array<WeaveString::Fade, kRenderChunk + 4> weaveFades_ {};
	/// The one entry frozen voices read (the fade where the string stopped), and whether they have it.
	WeaveString::Fade frozenFade_ {};
	/// RESONATE'S WIDTH: where the second scan head is aimed (the target, from the knob) and where it is,
	/// slewed per chunk so a knob move walks the head rather than jumping it (a jump in a read position is a
	/// step in the signal). Stereo while either is above zero.
	double resonateWidthTarget_ = 0.0;
	double resonateWidthNow_ = 0.0;
	bool weaveFrozen_ = false;
	/// Zero unless a patch or a drag says otherwise, and zero is the engine before drift existed.
	SoundTravel drift_ {};
	SoundTravel playedTravel_ {};
	TravelPulls travelPulls_ {};
	/// Every sound deriving from its own home unless a patch or a turn says otherwise.
	SoundOrientations planes_ {};
	/// Empty until a rotation is made at a drift, and empty adds exactly nothing to the decode — the
	/// migration guarantee, structural rather than promised.
	SoundResidue residue_ {};
	SoundGamma gamma_ {};
	const LikeWarp* likeWarps_ = nullptr;
	const uint8_t* likeEngines_ = nullptr;
	uint64_t likeSourceSerial_ = 0;
	/// THE SYNTH'S OWN TABLES of the warps, filtered to the engine being played: the current and the previous, so a character
	/// still pending in a bank names a table that is still there (LikeWarpRef). Copied only when the set or the engine
	/// changes — a like, an unlike, a clear — never per block. ON THE HEAP, allocated with the synth (never on the audio
	/// thread): about 3 MB of warps a synth would otherwise carry inline, and test rigs hold several synths on the stack.
	std::unique_ptr<std::array<std::array<LikeWarp, kMaxSounds>, 2>> likeTables_ =
	    std::make_unique<std::array<std::array<LikeWarp, kMaxSounds>, 2>>();
	std::unique_ptr<std::array<std::array<PhaseWarp, kMaxSounds>, 2>> likeStores_ =
	    std::make_unique<std::array<std::array<PhaseWarp, kMaxSounds>, 2>>();
	static inline const LikeWarp kNoLikeWarp {};
	int likeCurrent_ = 0;
	uint64_t likeHeldSource_ = ~uint64_t { 0 };
	OscType likeHeldType_ = OscType::kCount;
	uint64_t likeSerial_ = 0;
	uint64_t likeCounter_ = 0;
	/// Each sound's warp into a character (by reference, LikeWarpRef), where the engine it was derived for is the one being
	/// built. The warp's content is copied into the synth's own storage (assignLikeWarpInto): no reference is held, so
	/// nothing here counts or frees.
	void attachLikeWarps(Character& c, OscType type) noexcept {
		if (likeWarps_ == nullptr || likeEngines_ == nullptr) {
			return;
		}
		if (likeSourceSerial_ != likeHeldSource_ || type != likeHeldType_) {
			likeCurrent_ = 1 - likeCurrent_;
			bool any = false;
			for (size_t slot = 0; slot < kMaxSounds; ++slot) {
				const bool match = likeEngines_[slot] == static_cast<uint8_t>(type);
				assignLikeWarpInto((*likeTables_)[static_cast<size_t>(likeCurrent_)][slot], match ? likeWarps_[slot] : kNoLikeWarp,
				                   (*likeStores_)[static_cast<size_t>(likeCurrent_)][slot]);
				any = any || (match && !likeWarps_[slot].identity());
			}
			likeHeldSource_ = likeSourceSerial_;
			likeHeldType_ = type;
			// NO WARPS AT ALL is serial 0, the reference every character carries by default — so a patch without likes
			// builds the very characters it built before the feature.
			likeSerial_ = any ? ++likeCounter_ : 0;
		}
		if (likeSerial_ != 0) {
			c.likes = { &(*likeTables_)[static_cast<size_t>(likeCurrent_)], likeSerial_ };
		}
	}
	/// Nobody, unless an editor is open and a hand has asked to hear one sound on its own.
	Preview preview_ {};
	/// The steal handoff (§5.8): terminateVoice records the dying voice's running phase, and the
	/// first voice the SAME note-on creates adopts it and catches up — the manager's ordering
	/// (terminate strictly before create within processNoteOnEvent, verified in
	/// sst-voicemanager's voicemanager_impl.h) is what makes one slot enough. Cleared at every
	/// noteOn entry so a terminate with no create (allSoundsOff, a reset) cannot leak a dead phase
	/// into an unrelated later note.
	double stolenPhase_ = 0.0;
	bool haveStolenPhase_ = false;
	/// AND EVERY OTHER PART'S, for a stack (backlog KE): part k of the new voice catches up from part k of
	/// the dying one. `stolenPhase_` stays part 0's, so a stack of one takes the road it always took.
	std::array<double, kMaxStack> stolenPartPhases_ {};
	/// WHAT THE LAST CHARACTER BUILD HEARD OF EACH SOUND. Written where the character is built and read
	/// by whoever publishes it; empty until then, which is a synth nobody has asked to play.
	SoundWeights weights_ {};
	std::array<Voice, kVoiceSlots> voices_;
	/// The sidechain for this render (setSidechain), offset per chunk into the lane.
	const float* sideL_ = nullptr;
	const float* sideR_ = nullptr;
	bool sideSounding_ = false;
	/// **THE PER-VOICE ROAD'S GAIN DELAY** (the Spectral device's per-voice road).
	/// A latent device running per voice (`Traits::latentPerVoice`) delays the voice's audio by `gainDelay_` samples, and Voice
	/// Level is applied AFTER the per-voice devices, so each voice's gain run is delayed by the same amount here: one ring per
	/// voice slot, `gainRingCap_` floats each, sized at activate for the longest latency a device can report. AT 0 NOTHING HERE
	/// IS TOUCHED and the render is today's, bit for bit.
	std::vector<float> gainRing_;
	size_t gainRingCap_ = 0;
	std::array<size_t, kVoiceSlots> gainWrite_ {};
	uint32_t gainDelay_ = 0;
	/// Clear `slot`'s ring: a new note starts from silence, as the delayed audio does.
	void clearGainRing(int slot) noexcept;
	/// One voice's gain at sample i, through the ring when the per-voice road is late; `ring` null otherwise.
	float delayedGain(Voice& v, float* ring, float now) noexcept;

	/// **EVERY VOICE'S STACKED PARTS BEYOND THE FIRST** (backlog KE), indexed by voice slot then part − 1.
	/// Heap-allocated at construction for `voiceManager_`'s reason below — at 14 KB an oscillator this is
	/// 2 MB (the voice-stacking doc §2.2), which no thread stack that holds a Synth as a local should carry —
	/// and never resized, so the audio thread never allocates for a stack of any size. `partOsc` is the one
	/// door to it, so part 0 (`Voice::osc`) and the others are reached the same way.
	using StackOscillators = std::array<std::array<OscVoice, kMaxStack - 1>, kVoiceSlots>;
	std::unique_ptr<StackOscillators> partOscs_;
	OscVoice& partOsc(Voice& voice, int part);
	const OscVoice& partOsc(const Voice& voice, int part) const;

	/// **THE STAND POOL** (backlog KE step 4; the doc's §3.9–3.11). A Stack Index route into Pos X or Pos Y stands each
	/// part at `puck + amount × index[k] × span`, where it hears that place's character and vowel. A stand is keyed
	/// by its OFFSET from the puck (so moving the puck moves every stand with it, as the main bank's rebuild), and
	/// one bank serves every part of every voice standing there. Fixed at construction (the pool is the maximum),
	/// heap-held for `partOscs_`'s reason; a part at offset (0, 0) reads the main bank and needs no slot.
	///
	/// A SLOT IS RE-KEYED ONLY THROUGH ITS BANK'S OWN REQUEST, which is what keeps a part fading OUT of it safe: the
	/// bank writes into its non-current table and cannot build twice inside one fade guard, so the table a leaving
	/// part is mixing out of is untouched for the whole of its fade — the quarantine §3.11 asks for, without a
	/// timer of its own.
	/// **80, DERIVED: every part that can be live at once** (step 4c; the doc's slot design). At most B parts sound under
	/// the guard, and each steal-fade spare may still be fading a whole stack — so a part can always have a slot.
	static constexpr int kStandPool = kOscillatorBudget + kStealFadeSpares * kMaxStack;
	using StandBanks = std::array<StandBank, kStandPool>;
	std::unique_ptr<StandBanks> standBanks_;
	/// A stand's key: its offset from the puck on X and Y, and its ear side (0 unless terrain stereo is live, step 4c).
	using StandKey = std::array<double, 3>;
	std::array<StandKey, kStandPool> standOffset_ {};
	/// Whether a slot's table is built for its CURRENT key — a part is moved onto a slot only then (the note-on cap).
	std::array<bool, kStandPool> standReady_ {};
	/// Whether the last assignment stood a stacked voice's parts on ear sides (terrain stereo with a stack).
	bool widthStands_{false};
	static constexpr int kStandNewBuildsPerPass = 16;
	std::array<bool, kStandPool> standKeyed_ {};
	std::array<bool, kStandPool> standUsed_ {};
	/// Keyed or re-keyed on this pass: built now, whatever the build budget says (a part must not wait on an empty stand).
	std::array<bool, kStandPool> standFresh_ {};
	/// **THE BUDGETS** (§3.11): at most this many stands rebuilt, and this many own-stand vowel rebakes, per control tick
	/// (64 samples) — the doc's 8 and 8 per 256-sample block. Served round-robin from these cursors. A stand just keyed
	/// and a first bake are never deferred; the shared bakes on the puck keep today's schedule.
	static constexpr int kStandBuildsPerTick = 2;
	static constexpr int kOwnBakesPerTick = 2;
	int standBuildCursor_{0};
	int ownBakeCursor_{0};
	/// The summed Stack Index routes into Pos X and Pos Y, read with Fine Tune's and Level's (`applyPitches`).
	double stackPosXAmount_{0.0};
	double stackPosYAmount_{0.0};
	/// How many parts wanted a stand of their own and found no slot, at the last assignment (TEST ONLY reads it).
	int standOverflow_{0};
	/// Decide every live part's stand for this tick (`StackPart::stand`), keying slots as needed. Returns whether
	/// any part stands off the puck.
	bool assignStands(bool widthStands);
	/// Move each live part to its stand's bank, and point its vowel at the first part of its voice on the same stand.
	void seatPartsOnStands(Voice& voice);
	/// **THE PARTS' 0-CENT COHERENCE** (`Voice::stackCoherence`;): exactly 1 while every part reads
	/// one bank, else recomputed from the cycles the parts play — only when a part's table, grid or bake moved, and AT
	/// MOST ONE PART'S CYCLE PER CALL (one call per voice per chunk), so it converges over N chunks. `all` reads every
	/// moved part at once (TEST ONLY).
	void updateStackCoherence(Voice& voice, bool all = false);
	/// Points per cycle the coherence reads: 256 lands within 0.021 of 512 in every measured cell (0.03 dB of trim).
	static constexpr int kCoherencePoints = 256;
	/// EVERY PART'S CYCLE AS LAST READ, per voice slot, and the pairwise sums c_ij between them — kept, so a change to
	/// one part re-reads one cycle and recomputes one row rather than eight (the evaluations are most of the cost;
	/// measured in the design doc), and the cursor that takes moved parts in turn. Heap-held at construction (~170 KB),
	/// for `partOscs_`'s reason, and never resized.
	struct CoherenceSlot {
		std::array<std::array<float, kCoherencePoints>, kMaxStack> cycles{};
		std::array<std::array<double, kMaxStack>, kMaxStack> sums{};
		int cursor{0};
	};
	using CoherenceCycles = std::array<CoherenceSlot, kVoiceSlots>;
	std::unique_ptr<CoherenceCycles> coherenceCycles_;
	/// A-PH (additive design §8): the phase optimiser's thread, its request slot and its latest profile; the build serial last
	/// posted (a new table posts once). Heap, for the same reason as the stands: it holds two kMaxPartials seqlocks.
	std::unique_ptr<PhaseService> phases_;
	uint64_t lastPhaseSerial_{0};
	/// A-PH3: what the last post was made from (the drawer, the note's pitch), the pitch posts are sketched at (the last held
	/// note's, C3 until one), and the shaper cache the sketch prepares (heap: a GainCache is large).
	spectral::ShaperParams lastPhaseDrawer_ {};
	double lastPhaseSemis_{-1.0};
	double phaseSketchSemis_{60.0};
	std::unique_ptr<spectral::GainCache<partials::kMaxPartials>> phaseSketchCache_;
	/// Post the current Partials table's spectrum to the optimiser if it is a new build and the phases are not Natural.
	void postPhaseRequest() noexcept;
	/// How many part cycles `updateStackCoherence` has read, ever (TEST ONLY reads it): the cost, counted.
	uint64_t coherenceCycleReads_{0};
	/// The bank a part reads: the main one, or its stand's.
	const TableSource& bankForStand(int stand) const;
	/// The soonest any keyed stand builds, which caps a render chunk as the main bank's pending build does.
	int standsUntilBuild() const;
	/// A phase verb said to every part a voice is rendering — the retrigger roads, which said it to `osc`.
	template <typename F>
	void forEachLivePart(Voice& voice, F&& verb) {
		for (int part = 0; part < kMaxStack; ++part) {
			if (voice.parts[static_cast<size_t>(part)].live) {
				verb(partOsc(voice, part));
			}
		}
	}

	/// WHETHER EVERY LIVE PART'S STRING IS EXACTLY SILENT, nothing ringing and nothing waiting to be plucked — and the engine is
	/// Pluck, so a voice of any other engine never answers yes (its idle string means nothing about its sound).
	/// WHETHER EVERY LIVE PART'S DRUM HIT IS EXACTLY SILENT and nothing waits to strike — and the engine is Drum, so a voice of
	/// any other engine never answers yes.
	bool drumsSilent(Voice& voice) {
		if (params_.oscType() != OscType::Drum) {
			return false;
		}
		bool silent = true;
		forEachLivePart(voice, [&silent](OscVoice& osc) { silent = silent && osc.drumIdle(); });
		return silent;
	}
	/// A ONE-SHOT LET GO ON DRUM, STILL HELD, AND THE ENGINE NO LONGER DRUM (`Voice::oneShotLetGo`): nothing else ends it.
	bool letGoOffDrum(const Voice& voice) const {
		return voice.oneShotLetGo && params_.oscType() != OscType::Drum
		       && (voice.stage == Voice::Stage::Attack || voice.stage == Voice::Stage::Sustain);
	}
	bool stringsSilent(Voice& voice) {
		if (params_.oscType() != OscType::Pluck) {
			return false;
		}
		bool silent = true;
		forEachLivePart(voice, [&silent](OscVoice& osc) { silent = silent && osc.stringIdle(); });
		return silent;
	}

	/// A note's stack at its first sample: every part at its place in the fan, at its gain, pitched.
	void seatStack(Voice& voice, int count);
	/// A held note's stack re-spaced to `count` parts — live, with fades (§3.4 of the doc).
	void respaceStack(Voice& voice, int count);
	/// Every sounding voice re-spaced when Stack moves, once per change.
	void syncStack();
	/// Whether any sounding voice is mid re-space — the tick road re-decides pitches while one is.
	bool anyStackRespacing() const;
	/// One control tick of a re-space's fan walk.
	static void advanceRespace(Voice& voice);
	/// The note's own pitch plus part `part`'s place in the fan, in semitones.
	double partTargetFor(const Voice& voice, double target, int part) const;
	/// Render every live part of `voice` into `left` (and `right`), each at its gain and pan. Part 0 is already in
	/// `left` (and, with `earsLive`, in `right`); without live ears the stereo road copies it across first.
	/// `level` is the voice's level this chunk, which a Stack Index route into Level offsets per part.
	void renderStackParts(Voice& voice, float* left, float* right, int n, bool earsLive, float level);
	/// **THE PARTS' STACK INDEX, RE-DECIDED** (step 3) from the Shape, Mapping and Curve as resolved FOR THIS VOICE;
	/// on the control tick Drift's walk takes a step first.
	void updateStackIndex(Voice& voice, bool onTick);
	/// The voice's level this chunk, as the render loop's own `gain` is computed — for `renderStackParts`.
	float stackVoiceLevel(const Voice& voice, bool levelVaries, double levelOnce) const;
	/// Whether a route from the Stack Index reaches anything a stacked voice is sounding — the tick road then
	/// re-decides the parts' index and pitches on every tick, as it does while a re-space walks.
	bool stackIndexLive() const;
	/// The summed Stack Index routes into Fine Tune and Level, read with the pitches (`applyPitches`).
	double stackFineAmount_{0.0};
	double stackLevelAmount_{0.0};
	/// Where the note-on draws each voice's Random Fresh seed and Drift stream from: one stream, reset with the
	/// engine, so a bounce draws the same notes. **NOT SAVED WITH THE PATCH, AND WHY** (backlog NM): each draw is a
	/// note's, differing by design and ending with its voice — and while the transport plays the seed is the song tick's
	/// hash, so a reload reproduces it there by construction.
	uint32_t stackSeedState_{0x9E3779B9u};
	/// The voice edge's two lengths: `kVoiceEdgeSeconds` always, except in a test that lengthens them to land an
	/// event inside a Release or an Attack (`setVoiceEdgesForTest`).
	double attackEdgeSeconds_{kVoiceEdgeSeconds};
	double releaseEdgeSeconds_{kVoiceEdgeSeconds};
	/// Where the Level fader's ramp left off at the end of the last chunk (render()).
	float outputLevelWas_{1.0f};
	bool outputLevelPrimed_{false};
	/// Each live part's pan destination, from its place in the fan and this voice's Stack Spread (step 2).
	void aimStackPans(Voice& voice);
	/// Whether any sounding stack has a part off centre, or on its way off — the stereo road's second reason.
	bool anyStackPanned() const;
	int appliedStackCount_{1};
	alignas(16) float partScratch_[kRenderChunk];
	alignas(16) float partScratchR_[kRenderChunk];

	// Heap-allocated because it is large — the manager carries a 16x128 key-state block per port —
	// and a member of that size would overflow a small thread stack wherever a Synth is a local.
	// Built once, in the constructor; the audio thread only ever uses it.
	std::unique_ptr<VoiceManager> voiceManager_;
	Responder responder_;
	MonoResponder monoResponder_;

	/// Called when a voice's release has run to silence. The manager hands it to us at construction
	/// and it is how it learns a voice is free — a synth that never called it would leak polyphony.
	std::function<void(Voice*)> voiceEnded_;

	double sampleRate_{48000.0};
	/// The rate the last `activate` prepared everything at; 0 before the first. An `activate` at this rate keeps the
	/// voices (see `activate`).
	double activeRate_{0.0};
	/// The longest block the lane's devices were last PREPARED with (CLAP's `max_frames_count`, backlog GO). Grow-only
	/// at an unchanged rate: an `activate` with a longer one re-prepares the lane's devices (keeping the voices), and a
	/// shorter or equal one leaves them — and their state — alone. 0 before the first.
	int preparedMaxFrames_{0};

	/// The song position at the start of the current block, and how far into it we have rendered.
	/// Kept apart rather than summed so the re-anchoring is visible: the host writes the first and
	/// zeroes the second, and rendering only ever moves the second.
	double songBlockStartSeconds_{0.0};
	/// WHERE THE BLOCK STARTS IN BEATS, or NaN when the host offers no beats timeline. Kept beside the
	/// seconds rather than derived from it — see `setTransport`.
	double songBlockStartBeats_{device::TransportClock::kNoPosition};
	int songSamplesIntoBlock_{0};
	bool haveTransport_{false};
	bool transportPlaying_{false};
	/// The host's tempo as of THIS BLOCK, or <= 0 when it has not said. Rewritten every block.
	double tempoBpm_{0.0};
	/// Advances on every rendered sample and is never re-anchored — what a stopped transport locks
	/// to instead, so the oscillator keeps moving when the song is not.
	double freeRunSamples_{0.0};

	/// Pitch wheel per MIDI channel, in wheel units rather than semitones: the bend range is a param
	/// and can move while a note is bent.
	std::array<double, 16> bendWheel_{};

	// The last values pushed into the voice manager, so an unchanged param is not re-pushed. That is
	// not only an optimisation: setPlaymode hard-terminates the group's voices whenever the mode or
	// its feature flags change, so calling it every slice would silence the synth every slice.
	int appliedVoiceCount_{0};
	VoiceMode appliedVoiceMode_{VoiceMode::Poly};
	NotePriority appliedNotePriority_{NotePriority::Last};
	bool haveAppliedModes_{false};

	//------------------//
	// The modulator rack //
	//------------------//

	/// WHETHER THE NOTE STACK IS HOLDING ANYTHING — the global gate decided on 2026-08-22.
	///
	/// DERIVED FROM THE VOICES' OWN STAGES rather than from a counter kept beside them, and that is
	/// the load-bearing choice here. A voice is in Attack or Sustain exactly while its key is down or
	/// the pedal is holding it; it leaves for Release the moment neither is true. So the voice
	/// manager's answers to sustain pedal, mono, legato, note stealing, choke and all-notes-off are
	/// already in this reading, and a second count maintained in noteOn/noteOff would have to
	/// reproduce every one of them and would be wrong about the first one somebody forgot.
	bool noteStackHeld() const;

	/// The same, for a slot holding an MSEG: the device's settings with every routed offset applied,
	/// in the control's own units. Separate from the one above because the two devices have different
	/// controls and a shared version would have to be told which — see MsegControl's header on why
	/// the second instance is not yet a template.
	mod::mseg::Settings modulatedMsegSettings(size_t slot) const;

	/// Advance every envelope one control tick and publish where they got to. Returns whether any of
	/// them can currently reach anything, which is what decides how much of applyParams the tick has
	/// to redo — see the call site in render().
	bool tickEnvelopes();
	/// **A VOICE HANDED A NOTE WITH A NEW ATTACK READS THAT NOTE'S PER-VOICE SOURCES FROM ITS FIRST SAMPLE** (backlog FF).
	void startVoiceSources(const Voice& voice);

	/// WHAT SLOT `slot` PUBLISHES THIS BLOCK — its output, or its REST value when its bypass is off.
	/// See the definition: the device keeps running either way.
	[[nodiscard]] float publishedSourceAt(size_t slot) const;
	/// **THE SAME, FOR ONE OUTPUT.** A device may declare more than one — the Note Expressions device
	/// declares seven — and publishing only the first left six of them reading zero at every
	/// destination that is not per-voice.
	[[nodiscard]] float publishedSourceAt(size_t slot, size_t output) const;

	/// Publish where the envelopes stand without advancing them. What a reset does.
	void publishEnvelopes();

	/// WHERE ONE SLOT'S MODULATOR STANDS — the tag branch, in one place. See the definition.
	[[nodiscard]] mod::Playhead playheadOf(size_t slot) const;

	/// **IS THE MODULATOR IN THIS SLOT SWITCHED ON?** — one authority, because the VALUE it publishes
	/// and the POSITION it reports must not be able to disagree. See the note at `playheadOf`: a star
	/// that keeps moving for a bypassed device reads as proof it is live, and cost an evening.
	[[nodiscard]] bool modulatorSwitchedOnAt(size_t slot) const;
	/// **THE FX LANE'S TWIN OF `playheadOf`**, by position: the device's playhead while its enable reads on (the
	/// engine's resolved row, which is what its published outputs obey), idle otherwise.
	[[nodiscard]] mod::Playhead fxPlayheadOf(size_t position) const;

	/// WHERE IN THE CURRENT CONTROL TICK A NOTE ARRIVING NOW SITS. See the definition: stamping zero
	/// is the block-rate quantisation `NoteEvent::timeSamples` exists to prevent.
	[[nodiscard]] uint32_t noteOffsetInTick() const noexcept;

	/// REMEMBER ONE NOTE FOR THIS CONTROL TICK'S LANE. See the definition on why a drop here costs a
	/// device's knowledge rather than leaving a note stuck down.
	void captureNote(const device::NoteEvent& event) noexcept;

	/// **THE SEAM: THE ONE PLACE THE VOICE MANAGER IS TOLD ABOUT A NOTE.** Every road that plays,
	/// releases or chokes a note goes through here, so which STREAM plays is a question of who calls
	/// this rather than a question spread across three methods. See the definition for the two
	/// readings and which line changes between them.
	void deliverNotes(const device::NoteEvent* events, size_t count);

	/// OFFER THESE NOTES TO THE RACK AND PLAY WHAT COMES OUT, at the instant they arrived. See the
	/// definition on why this is not batched to the control tick. A device that emits on its own clock
	/// has the other road, `emitLaneNotes`.
	void runLaneFor(const device::NoteEvent* events, size_t count);

	/// **THE NOTE LANE'S CLOCK CALL, AT THE START OF EVERY RENDER CHUNK** (arp design §3): what a
	/// self-clocked device (the arp) emits at this sample is played now, and the return is how many samples until it
	/// next wants a call, which `render` caps its chunk at. `UINT32_MAX`, and nothing delivered, when no device emits.
	uint32_t emitLaneNotes();
	/// ON THE HEAP, SIZED AT CONSTRUCTION — a `Synth` is built on the stack by many cases, and these two are 70 KB.
	std::vector<device::NoteEvent> emittedNotes_ = std::vector<device::NoteEvent>(rack::RackEngine::kMaxNotes);
	/// THE TEST LOG of every event `deliverNotes` played and the sample it played at (samples since construction), for
	/// cases that assert onsets by value. A fixed ring, so the audio thread never allocates; the newest `kDeliveredLog`.
	std::vector<DeliveredNote> deliveredLog_ = std::vector<DeliveredNote>(kDeliveredLog);
	uint64_t deliveredCount_ = 0;
	uint64_t samplesRendered_ = 0;

	/// End this control tick's record of what the lane saw. Returns how many notes that was.
	size_t runNoteLane();

	/// THE MODULATORS RUN ON THE LANE'S ENGINE (`lane_`, lanes 8..11) — they were two typed arrays,
	/// then a `ModRunner`, and are now the rack engine's devices like everything else Feathers hosts. A
	/// settings edit keeps the instance (identity is the instance id), so the shape stays where it was.

	/// **THIS CONTROL TICK'S NOTES, AS THE HOST SENT THEM** — the lane's input, filled by the three
	/// event methods and emptied by `runNoteLane`. Fixed size because the audio thread may not
	/// allocate, and the rack engine's own cap: a stream that fits one host must fit the other, or a
	/// note survives in one rack and not the other.
	std::array<device::NoteEvent, rack::RackEngine::kMaxNotes> pendingNotes_ {};
	size_t pendingNoteCount_ = 0;

	/// **DOUBLE DUTY, AND BOTH USES ARE REAL.** `runLaneFor` writes the lane's OUTPUT here on its way
	/// to the voices — that is the working use. `runNoteLane` then overwrites it at the tick boundary
	/// with the tick's INPUT record, which is what the test seam reports. The second write is the
	/// last one before anything outside the engine can look, so the seam is unambiguous; and the
	/// buffer is one array rather than two because they are never live at the same instant.
	std::array<device::NoteEvent, rack::RackEngine::kMaxNotes> laneNotes_ {};
	size_t laneNoteCount_ = 0;

	/// **A NOTE THIS SYNTH ENDED ITSELF IS SAID TO THE LANE** — a choke for the voice's note, to the
	/// lane's devices only. Its own
	/// scratch for the lane's output, because `laneNotes_` may be mid-delivery when a steal calls this.
	void chokeLaneNote(const Voice& voice) noexcept;
	/// **A MONO RETURN RESTRIKES THE HELD NOTE AT THE LANE**: the note it goes back to is struck
	/// again for the lane's devices only — a note-on for a note they hold is a RETRIGGER, which each shape's own rule
	/// admits or refuses (Leg, Once). See `initializeMultipleVoices`.
	void retriggerLaneNote(const Voice& voice) noexcept;
	/// **THE NOTE-ON THE VOICE MANAGER IS HANDLING RIGHT NOW** (backlog FG), so the responder calls it makes can
	/// tell which note they are for: a legato `moveVoice` carries no note id, and a steal's choke must not end
	/// the entry the new note has just taken over. Valid only inside `processNoteOnEvent`.
	struct PendingNoteOn {
		int32_t noteId = -1;
		int16_t channel = -1;
		int16_t key = -1;
		bool valid = false;
	};
	PendingNoteOn pendingNoteOn_ {};
	std::array<device::NoteEvent, 8> chokeScratch_ {};

	/// **EVERY KEY THE VOICES HAVE BEEN TOLD IS DOWN, AND THE NOTE ID IT WENT DOWN WITH** (2026-10-07: the
	/// env misbehaved after playing an interstitial note while holding a lower note). Mono and Legato RETURN to a
	/// held key when the note above it is released, and the voice manager says only which key — a legato move
	/// carries no id, and Mono's retrigger hands the returned-to voice id -1. Without the id the voice did not answer
	/// to the note it now plays: a per-note modulator found no entry for (released id, held key) and the voice read
	/// the device's mono shape instead. And a Mono steal must not end a note whose key is still down (`chokeLaneNote`),
	/// because the voice will come back to it. Set as a note-on reaches the voice manager and cleared as its note-off does,
	/// BEFORE the manager handles it, so a return made inside that note-off sees the released key as up.
	struct HeldNote {
		int32_t noteId = -1;
		double velocity = 0.0;
		bool down = false;
	};
	std::array<HeldNote, 16 * 128> heldNotes_ {};
	[[nodiscard]] HeldNote& heldNote(int16_t channel, int16_t key) noexcept {
		return heldNotes_[static_cast<size_t>(channel) * 128u + static_cast<size_t>(key)];
	}
	/// The id of the note held on (channel, key), or `fallback` when that key is not down.
	[[nodiscard]] int32_t heldNoteIdOr(int16_t channel, int16_t key, int32_t fallback) noexcept;
	/// Every held key an off or a choke names lets go; -1 in any field is a wildcard, as on the note itself.
	void liftHeldNotes(int16_t channel, int16_t key, int32_t noteId) noexcept;

	/// WHERE EACH OF THEM STANDS, refreshed wherever their value is. Kept beside the devices for the
	/// reason `modulated_` is kept: the editor must read a snapshot the engine took, not reach across
	/// the thread boundary into a device that is mid-tick.
	RackPlayheads playheads_ = RackPlayheads(kMaxRackSlots);
	/// THE FX DRIVE METERS, one per slot, negative where nobody publishes. See `fxDrivePeaks()`.
	FxDrivePeaks fxDrivePeaks_ = FxDrivePeaks(kMaxFxSlots, -1.0f);
	/// THE COURTESY OUTPUT METERS, one per slot, negative where nothing is metered. See `fxOutputPeaks()`.
	FxDrivePeaks fxOutputPeaks_ = FxDrivePeaks(kMaxFxSlots, -1.0f);
	/// THE FX LANE'S MODULATOR PLAYHEADS, one per position. See `fxPlayheads()`.
	RackPlayheads fxPlayheads_ = RackPlayheads(kMaxFxSlots);
	/// See `masterMeter()`. Prepared at activate; its buffers are fixed arrays, so nothing allocates after.
	fx::busutil::Meter masterMeter_ {};
	/// What they are set to, and how many of them there are.
	ModRack rack_ { defaultModRack() };
	/// The lane, as the editor last published it. Held for the same reason rack_ is: the audio thread
	/// must read a snapshot it owns rather than reach across to the bridge mid-block.
	FxChain fxChain_ {};
	/// `adoptChains`' own: the snapshot it lands a publish in, the sequence it last took, and the two
	/// views it builds before handing them on. Sized with this object, never on the audio thread.
	rack::ChainState chainState_ { kMaxDeviceSlots, 2 };
	/// WHERE A SNAPSHOT LANDS BEFORE IT IS KNOWN TO BE WHOLE — `PublishedChainState::adopt`.
	rack::ChainState chainStaging_ { kMaxDeviceSlots, 2 };
	uint32_t chainsSequence_ = 0;
	/// `chainsSequence_` for the message thread (`chainsAdopted`): the last chains publish this synth adopted.
	std::atomic<uint32_t> chainsAdoptedMirror_ { 0 };
	/// See `structureRevision()`. Starts at 1, so the first compile of the routes is never skipped as
	/// "the same structure" against a default of zero.
	uint32_t structureRevision_ = 1;
	/// **THE PLAN'S TWO COUNTERS** — the route publishes and the structure revision `compileRoutes` last
	/// built every node's inputs against (kill-lanes §2.4b). Either moving means a rebuild at the next drain.
	uint32_t routesSeen_ = 0;
	uint32_t routesStructure_ = 0;
	/// Hand the staged routes to the engine and rebuild the synth's own node's inputs, when either
	/// counter moved. Drain only — no allocation.
	void compileRoutes();
	/// WHETHER `fxChain_`/`rack_` ARE THAT SNAPSHOT — true after `adoptChains`, false after a caller hands
	/// a chain in by value. Decides which the lane syncs from, and so whether it can name a publish.
	bool chainsFromPublish_ = false;
	FxChain adoptedFx_ {};
	ModRack adoptedRack_ {};
	/// And the same after modulation — see modulatedRack(). Kept rather than recomputed, because
	/// tickEnvelopes has to build it either way.
	ModRack modulated_ { defaultModRack() };
	/// And the lane's, on the same terms — see modulatedFxChain(). `applyParams` has to build it to
	/// drive the runner, so this is a kept copy rather than extra work.
	FxChain modulatedFx_ {};

	/// **A GROWTH IN FLIGHT** — see `requestChainCapacity`. Built and owned by the main thread; the drain only
	/// swaps contents with it. One at a time: Idle, then Pending (built, waiting for a drain), then Taken
	/// (the drain swapped, so it now holds the OLD buffers), then freed by `reapChainGrowth` and Idle again.
	struct ChainGrowth {
		FxChain fx, adoptedFx, modulatedFx;
		ModRack rack, adoptedRack, modulated;
		rack::ChainState state, staging;
		RackPlayheads playheads, fxPlayheads;
		FxDrivePeaks drivePeaks;
		FxDrivePeaks outputPeaks;
	};
	static constexpr int kGrowthIdle = 0;
	static constexpr int kGrowthPending = 1;
	static constexpr int kGrowthTaken = 2;
	std::unique_ptr<ChainGrowth> growth_;
	std::atomic<int> growthState_ { kGrowthIdle };
	size_t requestedFx_ = kMaxFxSlots;
	size_t requestedStrip_ = kMaxRackSlots;
	size_t requestedState_ = kMaxDeviceSlots;
	/// The drain's half — at the top of `adoptChains`. Moves only.
	void adoptChainGrowth() noexcept;

	/// A NOTE-ON HAS ARRIVED SINCE THE LAST CONTROL TICK. A LATCH, NOT AN EDGE, and the difference is
	/// a real one: a note that starts and finishes inside one 64-sample tick would otherwise leave no
	/// trace by the time the envelope is asked, and a fast passage would have notes that silently did
	/// nothing. Set by noteOn, cleared by the tick that consumes it.
	bool envRetrigger_ = false;

	/// Whether the last tick found the note stack holding anything. Kept only so a test can read the
	/// gate — the tick derives it fresh from the voices every time, because they are the only thing
	/// that knows.
	bool envGate_ = false;


	/// **WHERE THE ENGINE KEEPS A DEVICE THIS SYNTH'S VIEWS NAME** (kill-lanes step 3a) — the strip device
	/// at `slot`, the fx device at `position`, by its id, in the engine's OWN slots: the index every per-
	/// slot road on `LaneRack` and every device coordinate `ParamSet` holds are in. `kNoEngineSlot` for a
	/// position holding nothing or a device the engine does not run. NOT the surface's device-slot
	/// coordinate, which is a THIRD space and one nothing crosses into any more (see below); the views
	/// (`rack_`, `fxChain_`, and the modulated copies the editor reads) are keyed by POSITION since
	/// kill-lanes step 3c, and a position is not an engine slot either.
	static constexpr size_t kNoEngineSlot = rack::RackEngine::kNoSlot;
	/// **THE DEVICE ITSELF**, which is what every per-device road on `LaneRack` takes — its output, its
	/// playhead, its meter, its instance. `kNoInstance` for a position holding nothing.
	DeviceInstanceId stripDevice(size_t slot) const noexcept;
	DeviceInstanceId fxDevice(size_t position) const noexcept;
	/// **EVERY DEVICE BOTH CHAINS HOLD, BY ID** — the strip in slot order, then the fx lane in its own. For the
	/// roads that are about a DEVICE rather than a chain: the per-voice cells (`startVoiceSources`,
	/// `tickEnvelopes`). They walked the strip alone while the lane held no modulators; since 2026-10-04 it
	/// does, and a road that walked one chain would leave a lane DAHDSR's per-voice cells unwritten — every
	/// per-voice route from it dead, and Voice Level holding a released voice on nothing.
	template <class F>
	void forEachChainDevice(F&& f) const {
		for (size_t slot = 0, n = std::min(rack_.count, rack_.device.size()); slot < n; ++slot) {
			f(stripDevice(slot));
		}
		for (size_t position = 0, n = fxChainLength(fxChain_); position < n; ++position) {
			f(fxDevice(position));
		}
	}
	/// AND WHERE THE ENGINE KEEPS IT, for the ONE reader that legitimately holds a slot: the coordinate
	/// space `ParamSet`'s plan and its device sources are in.
	size_t stripEngineSlot(size_t slot) const noexcept;
	size_t fxEngineSlot(size_t position) const noexcept;
	// **`deviceSlotOfEngineSlot` STOOD HERE AND IS DELETED (kill-lanes step 5).** It crossed from an
	// engine slot into the SURFACE's device-slot coordinate, and it existed for exactly one reader:
	// `fillBusFor`, which had to reach that space to ask `sourceLaneIsUpstreamOf` for the order. §2.4a
	// compiled the relation into the engine's own slots (`compileReach`), §2.4b deleted the relation with the
	// bus (a node pulls its inputs; legality is the graph's, at the edit) and the band is gone, so
	// there is no rule left written in that space and nothing to cross into it for. A third coordinate
	// space that nothing crosses into is one fewer place for the three to drift apart.


	/// Which note the summed bus is tracking — the lowest held voice. See the definition.
	device::NoteContext busNoteContext() const;

	/// The serial handed to the next note that starts. Only ever climbs; see Voice::startSerial.
	uint64_t nextNoteSerial_ = 1;

	/// **THE FX LANE — A RACK ENGINE.** The same `rack::RackEngine` the standalone rack and the rack
	/// device are, running the bus and, for a device whose Poly is on, an instance per VOICE SLOT
	/// (kVoiceSlots rather than kMaxVoices, because a steal-fade spare is a slot still sounding). It
	/// replaced two `FxRunner`s — a bus one and a per-voice one — that were a second chain beside the
	/// rack's. See lane_rack.h for what is still interim.
	LaneRack lane_;
	/// Level's inputs for the per-sample resolve — see `ParamSet::WindowPlan`.
	ParamSet::WindowPlan levelPlan_ {};
	/// **PHASE'S, the same way** (2026-10-03): a moving source reaches the oscillator's read per SAMPLE, which is
	/// what makes a fast one phase modulation rather than a staircase.
	ParamSet::WindowPlan phasePlan_ {};
	/// Phase for this chunk when nothing reaches it per voice (`renderChunk` decides, as it does for Level).
	bool phaseVaries_{false};
	double phaseOnce_{0.0};
	/// PHASE WIDTH this chunk, and whether it has anything to mirror — a width above 0 with something moving Phase
	/// off its knob. Only then does a voice take the stereo road for it (render()).
	float phaseWidth_{0.0f};
	bool phaseSpreadLive_{false};
	/// **PHASE IS STEREO-LIVE** (backlog KU, 2026-10-05): a route from a source that is a pair reaches it this block
	/// (`phasePlan_.pairs`), so the ears read at different phases and the voice takes the stereo road.
	bool phaseStereoLive_{false};
	/// THE SPECTRAL DRAWER (additive design §3; plan Task 10's per-voice resolution; per sound since the decision of
	/// 2026-10-10). The stored half, as last handed over; whether the ears' drawers can differ (a pair into a sound's S
	/// control, or ears whose sound weights differ — applyParamsRoad, where the routes and the character move); and whether
	/// any per-voice source reaches any sound's continuous control, so the common patch assembles ONE drawer per stand.
	SpectralState spectralState_ {};
	/// **OE: THE ENGINE VOCODER** (additive design §9): the one analysis of the sidechain (allocated at construction, prepared at
	/// activate), the envelope every voice reads (OscVoice::setVocodeEnvelope), and the freeze's persistence (the Spectral effect's
	/// Z10b road): the held envelope published whenever it changes, the last load primed from, and the note Key Track measures from.
	std::unique_ptr<fx::spectral::SidechainEnvelope> vocodeAnalyser_;
	spectral::VocodeEnvelope vocodeEnv_ {};
	device::BlockSnapshot<kVocodeFreezeFloats> vocodeFreezeSnapshot_;
	std::atomic<uint32_t> vocodeLoadsTaken_ {0};
	bool vocodeWasHolding_ = false;
	bool vocodePublished_ = false;
	bool vocodeWasPublishedHolding_ = false;
	bool vocodeEverHeld_ = false;
	uint64_t vocodeEnvSerial_ = 0;
	bool vocodeStereoLive_ = false; ///< OE2: some sound vocodes in Stereo or Spread, so chunks take the stereo road
	/// OE2 SIBILANCE: this chunk's noise per ear (the analysis makes it), whether it has any, and the gate the last chunk ended on.
	std::array<float, kRenderChunk> vocodeNoiseL_ {};
	std::array<float, kRenderChunk> vocodeNoiseR_ {};
	bool vocodeNoiseLive_ = false;
	float vocodeGateWas_ = 0.0f;
	void addVocodeNoise(float* left, float* right, int n) noexcept;
	uint64_t vocodePublishedSerial_ = 0;
	double vocodeFrozenHz_ = 261.6255653005986;
	uint64_t vocodeSketchSerial_ = 0;
	std::array<float, partials::kMaxPartials> vocodeSketch_ {};
	int vocodeSketchCount_ = 0;
	uint64_t vocodeSketchPosts_ = 0; ///< every sketch posted to the optimiser (a test seam)
	/// Once a chunk, before the voices render: the analysis' settings, the sidechain's `frames` from `offset`, the freeze.
	void updateVocode(int offset, int frames) noexcept;
	/// Whether any sound's Vocode is on, on Additive with the drawer on: the analysis runs only then.
	[[nodiscard]] bool vocodeLive() const noexcept;
	void publishVocodeFreeze() noexcept;
	/// The analysis' settings as the params say now (engine-wide, resolved).
	[[nodiscard]] fx::spectral::SidechainEnvelope::Settings vocodeSettings() const noexcept;
	/// A saved held envelope (a load, or a re-arm): primed if Freeze is on, else the analysis starts fresh.
	void primeVocode(const VocodeFreezeBlock& held) noexcept;
	bool drawerStereoLive_{false};
	bool drawerPairLive_{false}; ///< a pair reaches some sound's S control (the routes' half of drawerStereoLive_)
	bool drawerVaries_{false};
	/// **THE DRAWER SCAN'S ROUTE-ONLY HALF, KEPT BETWEEN COMPILES** (the idle-overhead pass): what `drawerVaries_` reads,
	/// and the S controls an input reaches, scanned when `routesCompiles_` moved (`drawerScanAt_`). `drawerScans_` counts
	/// the scans, for the case that pins none at rest.
	bool drawerVariesScanned_{false};
	std::array<ParamId, kMaxSounds * kSpectralStereoControls.size()> drawerRoutedS_ {};
	size_t drawerRoutedPairs_ = 0;
	uint64_t drawerScanAt_ = ~uint64_t { 0 };
	uint64_t drawerScans_ = 0;
	/// THE ROUTES' COMPILES: moves every time `compileRoutes` rebuilds the core ranges.
	uint64_t routesCompiles_ = 0;
	/// **`modulated_` IS COPIED FROM `rack_` ONLY WHEN THIS KEY MOVED** (`tickEnvelopes`): `rack_`'s writes
	/// (`rackRevision_`), the lane's structure and the row count. `modulatedCopies_` counts the copies.
	uint64_t rackRevision_ = 0;
	uint64_t modulatedRackAt_ = ~uint64_t { 0 };
	uint64_t modulatedStructureAt_ = ~uint64_t { 0 };
	size_t modulatedCountAt_ = SIZE_MAX;
	uint64_t modulatedCopies_ = 0;
	/// THE WEIGHTS EACH DRAWER IS BLENDED BY: the character each ear plays (the head's twice when the terrain is mono) and
	/// each keyed stand's (voice stacking step 4: a part on a stand hears that stand's blend, so it hears its drawer too).
	/// Stamped where each character is ASKED FOR: the bank may still be playing the previous build behind its fade guard
	/// (~2 ms), so for that long a drawer can lead the spectrum it shapes by one build. Not ramped (design §4.2).
	std::array<SoundWeights, 2> earWeights_ {};
	std::array<SoundWeights, kStandPool> standWeights_ {};
	/// Each stand's drawer as last assembled, and the seating it was assembled in (seatDrawers' memo: a stand is blended once
	/// per seating however many parts stand on it). Message-free audio-thread state, sized at construction.
	std::array<spectral::ShaperParams, kStandPool> standDrawer_ {};
	std::array<uint32_t, kStandPool> standDrawerSeating_ {};
	/// THE ONE DRAWER A VOICE RUNS: each sound's settings, resolved for
	/// `voice` (< 0: the collapsed reading) and the ear, blended by the weights of the character that part plays
	/// (`stand` < 0: the main bank, per ear; else that stand's). An operator's settings are the weighted mean over the
	/// sounds where it is ON, and its effect is faded by its MIX — the summed weight of those sounds — so an operator on in
	/// one sound and off in another fades in by the weight, and an off operator counts as neutral. The Filter is grouped
	/// by Mode, each mode present its own filter at its own mix (lead's call, 2026-10-10). Hz controls are log2, so their
	/// mean is a log-frequency blend. The right ear adds each stereo-live S control's ear difference, clamped.
	spectral::ShaperParams drawerFor(int voice, int stand, bool rightEar) const;
	/// Hand every sounding voice's oscillators their drawer. Additive only; ONCE PER CONTROL TICK (PuckMotion::kTickSamples,
	/// 64 samples), the cadence at which every per-voice source moves (tickEnvelopes), and at the top of each render call.
	void seatDrawers();
	/// THE PLOT'S NO-NOTE READING (plotFrame): each ear's drawer memory. Mutable because they are memos of a const reading;
	/// only the publishing thread touches them.
	mutable spectral::GainCache<partials::kMaxPartials> plotCache_ {};
	mutable spectral::GainCache<partials::kMaxPartials> plotCacheR_ {};
	/// The sounding, non-stealing voice started most recently (highest `startSerial`), or -1.
	int newestVoiceSlot() const;
	/// TEST ONLY. How many times seatDrawers handed the voices their drawers (the tick cadence's instrument).
	uint32_t drawerSeatings_{0};
	/// The last chunk's stereo choice — `lastChunkStereoForTest`'s value. Audio thread; written once per chunk.
	bool lastChunkStereo_{false};
	/// The summed Stack Index routes into Phase: part k reads `amount × index` cycles from the voice's phase.
	double stackPhaseAmount_{0.0};
	/// The per-sample read offsets handed to an oscillator for one render call — the voice's own, and a part's —
	/// to the end of the control tick, which no render call crosses.
	alignas(16) std::array<float, PuckMotion::kTickSamples> readRun_ {};
	alignas(16) std::array<float, PuckMotion::kTickSamples> partReadRun_ {};
	/// The ears' difference (`ParamSet::resolveEarDifferencePlanned`) for the same two calls — written only while stereo-live.
	alignas(16) std::array<float, PuckMotion::kTickSamples> earRun_ {};
	alignas(16) std::array<float, PuckMotion::kTickSamples> partEarRun_ {};
	/// Phase's offset for `osc` (part 0 is the voice's own oscillator) for the render call about to run.
	void seatReadPhase(const Voice& voice, OscVoice& osc, int part);
	std::vector<float>* gainTrace_ = nullptr; // TEST ONLY — see `traceVoiceGainForTest`
	int gainTraceSlot_ = -1;
	/// The last pitch the bus had a note for, so a release tail does not slide to zero semitones.
	mutable float lastBusPitch_ = 60.0f;

	/// Scratch for one voice's oscillator output, before its edge and level are applied. One buffer
	/// rather than one per voice: voices are summed one at a time. The R twin is written only by the
	/// stereo chunk path — the mono path never touches it.
	alignas(16) float scratch_[kRenderChunk];
	alignas(16) float scratchR_[kRenderChunk];
};

} // namespace feathers
