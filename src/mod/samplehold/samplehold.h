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

#pragma once

#include "dsp/random_events.h"
#include "mod/per_note.h"
#include "samplehold_types.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace feathers::mod::samplehold {

/// **THE VALUE STREAM** — xorshift64*, `dsp::RandomEvents`' generator, held apart from the clock's own so the two
/// cannot pull draws from each other: a Random clock's timing and the values it takes are separate courses.
class Stream {
public:
	void seed(uint64_t seed) noexcept {
		state_ = seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e5f5ull;
		if (state_ == 0) {
			state_ = 0x2545f4914f6cdd1dull;
		}
	}
	/// [0, 1), the top 53 bits.
	[[nodiscard]] double uniform() noexcept {
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		const uint64_t r = state_ * 0x2545f4914f6cdd1dull;
		return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
	}
	/// WHERE IT STANDS, for the patch (backlog NM).
	[[nodiscard]] uint64_t state() const noexcept { return state_; }
	void setState(uint64_t s) noexcept { state_ = s != 0 ? s : 0x2545f4914f6cdd1dull; }

private:
	uint64_t state_ = 0x2545f4914f6cdd1dull;
};

/// **THE COLOURS, AS RULES FOR THE NEXT SAMPLE** — each takes the same three uniforms per clock whatever the colour
/// (`Shape::take`), so switching colour never shifts Probability's draws.
///
/// FEEL CONSTANTS, the implementer's picks (rule on shape, let the ear set the number):
/// - Pink: Voss-McCartney over the CLOCK count, `kPinkRows` rows (one updated per clock, the row its count's
///   trailing zeros name) plus a white term, the sum's spread scaled by `kPinkSpread` around the middle and clamped.
/// - Brown: a walk that steps by up to ±`kBrownStep`/2 per clock and reflects off both ends.
inline constexpr int kPinkRows = 8;
inline constexpr double kPinkSpread = 2.0;
inline constexpr double kBrownStep = 0.25;

/// THE SAMPLE IN OUTPUT UNITS (0..1, or -1..1 bipolar), QUANTIZED — the one rounding the engine and the card share.
/// Steps of 1/N of full scale: 0 is continuous. The clamp first, so a route past the end lands on the end's step.
[[nodiscard]] inline float quantized(float v, int steps) noexcept {
	if (steps <= 0 || !std::isfinite(v)) {
		return v;
	}
	const auto n = static_cast<float>(steps);
	return std::round(v * n) / n;
}

/// **THE RATE IN Hz** — `lfo::rateHz`'s rule over this device's two controls: a division of the tempo when synced and
/// the host has said one, the scale read as Hz otherwise.
[[nodiscard]] double rateHz(const Settings& s, double secondsPerBeat) noexcept;

//------------------------------------------------------------------------------------------------//
// Shape — one hold                                                                                //
//------------------------------------------------------------------------------------------------//

/// ONE HOLD: a clock, two streams, a held value and its slew. `mod::PerNoteTable` keeps one per note; the device keeps
/// one more for mono. The Input value and a Trig fire are handed over BEFORE the tick that reads them.
class Shape {
public:
	void prepare(double sampleRate);
	/// BACK TO THE TOP: nothing held, and the next tick takes a first sample. A Stable stream restarts from its seed
	/// there; a Free one goes on where it was.
	void reset();
	/// **A NOTE'S START** — `reset`, and `ordinal` is the note's place since the last restart (Stable seeds from it) or
	/// a count that never restarts (Free folds it in), so two notes struck together do not hold the same values. The
	/// streams are seeded afresh under either mode.
	void start(uint64_t ordinal);
	/// **A STABLE RESTART** (the transport starting): `reset`, keeping the ordinal this hold was started with.
	void restart() noexcept;
	/// **THE HOLD, IN AND OUT OF THE PATCH** (backlog NM, `kHoldWords`): everything a reload needs to go on where this
	/// left off. `load` is all-or-nothing: false leaves the hold as it was.
	void save(device::BlockWriter& w) const noexcept;
	[[nodiscard]] bool load(device::BlockReader& r) noexcept;
	void setInput(float value) noexcept { input_ = value; }
	/// A TRIG FIRED: the next tick takes a sample, under every Clock. Under Rate it also restarts the clock there — and
	/// synced on a playing transport, re-locks the song's step to the fire.
	void fire() noexcept { firePending_ = true; }

	/// ONE TICK. Returns the held output (slewed, before Amount).
	float tick(const Settings& s, bool gate, bool retrigger, TransportClock clock, float rateMul = 1.0f);

	/// THE HELD OUTPUT, SLEWED, before Amount — what output 0 is made of.
	[[nodiscard]] float value() const noexcept { return static_cast<float>(slewed_); }
	/// THE VALUE THE HOLD IS GOING TO — the last sample taken, quantized, in output units.
	[[nodiscard]] float held() const noexcept { return held_; }
	/// TRUE FOR THE ONE TICK A SAMPLE WAS TAKEN — Trig Out.
	[[nodiscard]] bool pulse() const noexcept { return pulse_; }
	/// HOW MANY SAMPLES HAVE BEEN TAKEN since the last reset — the playhead's ordinal, and a test's count.
	[[nodiscard]] uint32_t taken() const noexcept { return taken_; }
	/// HOW MANY CLOCKS HAVE COME since the last reset, taken or not — what Probability is a fraction of.
	[[nodiscard]] uint32_t clocks() const noexcept { return clocks_; }
	[[nodiscard]] Playhead playhead() const noexcept;
	/// **THE NEWEST SAMPLES TAKEN, NEWEST FIRST** (`ModOps::recentEvents`): each one's place in `taken` and its raw 0..1
	/// level, the playhead level it stood at — so a scope that looks once a frame draws every step a fast clock took.
	size_t recentEvents(device::RecentEvent* out, size_t capacity) const noexcept;

	/// `mod::PerNoteTable`'s shape: a hold has no end of its own, so a released note is done at once — its slot is
	/// reclaimed first, while it goes on holding for any voice still sounding it.
	[[nodiscard]] bool idle() const noexcept { return true; }
	void carrySmoothingFrom(const Shape&) noexcept {}

private:
	/// One clock: draw, maybe take. `first` takes whatever Probability says — a hold begins holding something.
	void take(const Settings& s, bool first);
	[[nodiscard]] bool advanceRate(const Settings& s, const TransportClock& clock, float rateMul);
	void seedStreams(const Settings& s);
	[[nodiscard]] double noise(const Settings& s, double uA, double uB) noexcept;

	double sampleRate_ = 48000.0;
	double tickSeconds_ = static_cast<double>(kBlockSamples) / 48000.0;

	Stream values_ {};
	dsp::RandomEvents events_ {};
	/// WHERE THE STREAMS STAND: never seeded (the first tick seeds them), or a restart owed (`reset`, `start`).
	bool seeded_ = false;
	bool restartOwed_ = true;
	/// A NOTE'S START OWES A RESEED under Free too; a reset does not (Free never restarts).
	bool reseedOwed_ = false;
	uint64_t ordinal_ = 0;
	/// THE SEED THE STREAMS WERE LAST SEEDED FROM, so a Free stream whose Seed moves is seeded from the new one at once.
	int seededSeed_ = 0;
	/// THE LAST TICK'S POLARITY, so the playhead can say where the hold stands in raw 0..1.
	bool bipolar_ = true;
	bool rated_ = true;

	float input_ = 0.0f;
	bool firePending_ = false;
	bool primed_ = false;
	/// RATE'S CLOCK: its phase free-running, or the song's step while synced and playing.
	double phase_ = 0.0;
	TransportLock lock_ {};
	/// THE TURNS EVERY TRIG'S RE-LOCK HAS SHIFTED THE SONG'S STEP BY since the lock began — added back to name the song's
	/// own step for Free's draws. Ends with the lock, as `TransportLock`'s shift does; not saved, as the lock is not.
	double relock_ = 0.0;
	bool haveStep_ = false;
	double step_ = 0.0;

	std::array<double, kPinkRows> pinkRows_ {};
	double pinkWhite_ = 0.0;
	uint64_t pinkCount_ = 0;
	double brown_ = 0.5;

	float held_ = 0.0f;
	double slewed_ = 0.0;
	bool pulse_ = false;
	uint32_t taken_ = 0;
	uint32_t clocks_ = 0;
	/// THE LAST `kRecentEvents` SAMPLES, at `taken % kRecentEvents` — a display's, not the patch's: never saved.
	std::array<device::RecentEvent, device::kRecentEvents> recent_ {};
};

//------------------------------------------------------------------------------------------------//
// The device                                                                                      //
//------------------------------------------------------------------------------------------------//

/// **THE DEVICE: A HOLD FOR MONO, ONE PER NOTE FOR POLY** — `mod::PolyShape`'s arrangement, written out because a hold
/// takes a second per-note input: the Input is resolved per note beside the Trig.
struct Device {
	Shape mono;
	PerNoteTable<Shape> notes;
	Settings settings {};
	TrigEdge trig;
	uint32_t trigFires = 0;
	/// THE NOTE ORDINALS — since the last Stable restart, and since construction (Free never restarts).
	uint64_t notesSinceRestart = 0;
	uint64_t notesEver = 0;
	bool wasPlaying = false;
	/// THE ONE HOLD AS LAST PUBLISHED, for the patch (`kStateOps`; backlog NM): written after every tick.
	device::BlockSnapshot<kHoldWords> holdSnapshot;

	[[nodiscard]] bool poly() const noexcept { return settings.poly; }
	/// The block: the mono hold and the note ordinals.
	void saveHold(float* out, size_t capacity) const noexcept;
	[[nodiscard]] bool loadHold(const float* in, size_t n) noexcept;
	void prepare(double sampleRate);
	void reset();
	void tick(const device::ModTickContext& ctx);
	/// THE HOLD A DESTINATION WITH NO VOICE READS — mono, or under Poly the newest note's.
	[[nodiscard]] const Shape& sharedShape() const noexcept;
	[[nodiscard]] const Shape& shapeForNote(int32_t noteId, int16_t channel, int16_t key) const noexcept;
	[[nodiscard]] float outputOf(const Shape& shape, size_t output) const noexcept;
	void processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                  size_t* outCount) noexcept;
};

} // namespace feathers::mod::samplehold
