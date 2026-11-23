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

#include "device/registry.h"
#include "dsp/drum_voice.h"
#include "dsp/ks_string.h"
#include "fx/drum/drum_likes.h"
#include "fx/drum/drum_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

// DRUM'S DSP: a small fixed pool of `drum::Hit`s struck by the notes a chain hands the device, summed (with Level and Pan) into the
// buffer the chain gave it. drum.cpp has the ops tables over one storage; this header has the object so a test can read it.
//
// **THE HITS ARE THE ENGINE'S**: one `drum::Hit` per sounding note, struck with `drum::lawsFor` at the fields this device's own terrain
// decodes to (`fieldsNow`), the pad's controls round them as the synth fills them (`controlsOf`). Where the engine's pad has things
// the voice manager does (Voices, the Gate, the choke group, the Bend), the device does them in its own pool.
//
// **A NOTE IS PLACED ON ITS SAMPLE.** `NoteEvent::timeSamples` is an offset in the 64-sample control tick; the host says where the next
// call starts in it (`NoteContext::tickPhase`), so a note lands at `timeSamples - tickPhase` inside that call. A host that says nothing
// (-1) is read as "from the start of the next call".

namespace feathers::fx::drumdevice {

/// CLAP's tuning expression (semitones, absolute), the one a held key's bend arrives as.
inline constexpr int32_t kTuningExpression = 2;

/// THE SEED OF ONE STRIKE — a pure function of what the strike is, never of when the object was built: the Seed control, the key, the
/// kind, and the song's place (`tick`, in 64-sample control ticks, or -1 when the transport does not play, then the key's strike count).
/// The engine's own hash (`Synth::strikeDrums`), with the kind folded in so two kinds on one key do not share a noise stream.
[[nodiscard]] uint64_t strikeSeed(uint64_t seed, int key, feathers::drum::Kind kind, int64_t tick, uint32_t count) noexcept;

/// THE PAD'S CONTROLS as the engine's `fillControls` makes them, from the row.
[[nodiscard]] feathers::drum::Controls controlsOf(const Settings& s);

/// THE FIELDS THE TERRAIN DECODES TO at this device's home (Zone, Offset, Gamma), Rotate and travel, through the seven scalers.
[[nodiscard]] feathers::drum::Fields fieldsOf(const Settings& s);

/// THE RESTING PITCH of a hit: its kind's, at Tune, moved by the Bend share of `movedSemitones` (the key's tuning expression).
[[nodiscard]] double baseHzOf(feathers::drum::Kind kind, const Settings& s, double movedSemitones) noexcept;
/// THE SAME FOR A HIT ON `key`: One Key as above (the key is not read); Pitched, the key's frequency (A4 = 440) moved by the whole of
/// `movedSemitones`, with Tune on top (the engine's Pitched mode).
[[nodiscard]] double baseHzOf(feathers::drum::Kind kind, const Settings& s, int key, double movedSemitones) noexcept;

class DrumDsp {
public:
	static constexpr int kPool = 2 * kMaxVoices; ///< live hits, and as many again fading out
	static constexpr int kQueue = 128;           ///< notes queued for the next call
	static constexpr int kSpan = 64;             ///< the most one render span covers

	DrumDsp();
	DrumDsp(const DrumDsp&) = delete;
	DrumDsp& operator=(const DrumDsp&) = delete;

	/// ALLOCATES: activate (the strings and the pluck builder), then every kind is struck once so nothing lazy is left for the audio thread.
	void prepare(double sampleRate);
	/// As if just switched on: every hit stopped, the queue emptied.
	void reset();
	void setSettings(const Settings& s);
	void setTickPhase(int phase) noexcept { phase_ = phase; }
	void setClock(const device::TransportClock& clock) noexcept {
		clock_ = clock;
		controls_.secondsPerBeat = clock.secondsPerBeat; // a synced Sweep's length follows the tempo
	}
	/// THE NOTES OF A PIECE, queued for the next `process`.
	void queueNotes(const device::NoteEvent* in, size_t count);
	/// Adds the drum to `left`/`right`.
	void process(float* left, float* right, int frames);
	/// WHETHER ANYTHING WILL STILL COME OUT: a hit sounding or waiting its Delay, a high-pass still ringing, a note not yet placed.
	[[nodiscard]] bool holdsTail() const noexcept;
	/// **THE LIKES BLOCK** (`drum.likes.v1`): a hand block kept here so it saves and restores with the device. The audio thread never
	/// reads it (the window scores and paints with it). MESSAGE THREAD to write; any non-audio thread to read.
	uint64_t readLikes(float* out, size_t capacity) const noexcept { return likes_.read(out, capacity); }
	bool writeLikes(const float* in, size_t n) noexcept;

	// ---- test seams ----
	[[nodiscard]] int soundingForTest() const noexcept;
	[[nodiscard]] int fadingForTest() const noexcept;
	[[nodiscard]] uint32_t strikesForTest() const noexcept { return strikes_; }
	[[nodiscard]] uint32_t staleDroppedForTest() const noexcept { return staleDropped_; }
	[[nodiscard]] uint32_t keyStrikesForTest(int key) const noexcept { return key >= 0 && key < 128 ? keyStrikes_[static_cast<size_t>(key)] : 0u; }
	[[nodiscard]] const Settings& settingsForTest() const noexcept { return s_; }
	[[nodiscard]] const feathers::drum::Fields& fieldsForTest() const noexcept { return fields_; }
	[[nodiscard]] const feathers::drum::Controls& controlsForTest() const noexcept { return controls_; }
	/// The live (not fading) hit struck latest, or null.
	[[nodiscard]] const feathers::drum::Hit* newestForTest() const noexcept;
	[[nodiscard]] uint64_t newestSeedForTest() const noexcept;
	[[nodiscard]] float keyTuningForTest(int key) const noexcept { return key >= 0 && key < 128 ? keyTuning_[static_cast<size_t>(key)] : 0.0f; }
	[[nodiscard]] float gainLeftForTest() const noexcept { return gainL_; }
	[[nodiscard]] float gainRightForTest() const noexcept { return gainR_; }
	/// The drone's hit, or null while it is not running.
	[[nodiscard]] const feathers::drum::Hit* droneForTest() const noexcept { return drone_.used ? &drone_.hit : nullptr; }

private:
	struct Slot {
		feathers::drum::Hit hit {};
		feathers::ks::KsString string {};
		bool used = false;     ///< taken by a note: waiting its Delay, sounding, or fading
		bool struck = false;   ///< `hit.strike` has run
		bool released = false; ///< the key was let go (the Gate acts on it at the strike or at once)
		bool gateApplied = false;
		int delayLeft = 0;
		bool fading = false;   ///< a choke or a steal: fading out to nothing over `fadeTotal`
		int fadeLeft = 0;
		int fadeTotal = 1;
		uint64_t order = 0;    ///< the strike's serial: the lowest is the oldest
		feathers::drum::Kind kind = feathers::drum::Kind::Kick;
		int key = 0;
		int32_t noteId = -1;
		double velocity = 1.0;
		uint64_t seed = 0;
		std::array<float, 2> hpX {};
		std::array<float, 2> hpY {};
	};

	void handle(const device::NoteEvent& e, int pos, int base);
	void endKey(const device::NoteEvent& e);
	Slot* acquire(int key);
	Slot* claimSlot();
	void strikeNow(Slot& s);
	void release(Slot& s);
	void fade(Slot& s);
	void renderSpan(float* left, float* right, int n);
	/// ONE SLOT'S SPAN, added to the ears: the pool's hits and the drone take the same road.
	void renderSlot(Slot& s, float* left, float* right, int n);
	void refreshLaws();
	/// **THE DRONE**: while the play mode is Drone on a kind that can be held, one hit of its own (outside the pool, so it takes no voice) is kept
	/// struck from the moment the device runs. A moved control reaches it as any hit's (`setLaws`); a changed Seed or Kind restarts it; leaving the
	/// mode fades it out. Called at the top of every `process`.
	void serveDrone();
	/// THE TRIG PARAM'S EDGES, as the notes a hand on the key would make (`process` puts them at the front of its queue).
	void injectTrig(size_t& head, int base);
	[[nodiscard]] bool droneWanted() const noexcept;
	[[nodiscard]] bool hpRinging(const Slot& s) const noexcept;

	double sr_ = 48000.0;
	Settings s_ {};
	feathers::drum::Fields fields_ {};
	feathers::drum::Controls controls_ {};
	std::array<float, 13> terrainKey_ {};
	bool terrainValid_ = false;
	float gainL_ = 1.0f;
	float gainR_ = 1.0f;
	float hpA_ = 1.0f;
	int phase_ = -1;
	device::TransportClock clock_ {};
	feathers::ks::PluckBuilder builder_ {};
	std::array<Slot, kPool> slots_ {};
	Slot drone_ {};
	std::array<device::NoteEvent, kQueue> queue_ {};
	size_t queued_ = 0;
	/// The Trig param's edge detector, the rises it has seen since the last call, whether it has fallen since, and whether a strike of its own is held.
	feathers::mod::TrigEdge trigEdge_ {};
	uint32_t trigRisen_ = 0;
	bool trigFell_ = false;
	bool trigHeld_ = false;
	std::array<float, 128> keyTuning_ {};
	std::array<uint32_t, 128> keyStrikes_ {};
	uint64_t order_ = 0;
	uint32_t strikes_ = 0;
	uint32_t staleDropped_ = 0;
	std::array<float, kSpan> tl_ {};
	std::array<float, kSpan> tr_ {};
	device::HandBlock<kLikesBlockWords> likes_;
};

} // namespace feathers::fx::drumdevice
