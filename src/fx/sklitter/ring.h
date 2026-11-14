/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace feathers::fx::sklitter {

//--------------------------------------------------------------------------------------------------//
// THE AUDIO SKLITTER SCATTERS.                                                                        //
//--------------------------------------------------------------------------------------------------//
//
// **RE-EXPRESSED, NOT PORTED, AND FOR A REASON THE REFERENCE COULD NOT HAVE.** The reference keeps three
// different buffers here: one for Burst whose size is the stutter rate, allocated when
// the gesture begins; a looper buffer of `44100 * 4` frames for the six looper modes; and a delay
// buffer of 32768 for per-grain delay sends. On the reference, allocating inside the gesture is fine.
//
// **HERE IT IS FORBIDDEN** -- no allocation on the audio thread post-activate -- so the rate-sized
// buffer becomes a CAPACITY allocated once in `prepare()` and a LOGICAL LENGTH moved for free inside
// it. One type serves all three uses; only the seconds differ.
//
// Interleaved rather than two planes: it is what `StereoSample` is, it is one allocation, and a
// Sklitter read touches both channels of one frame at a time.

/// FOUR SECONDS, from the reference's `kLooperBufferSize = 44100 * 4`. The 44100 was the reference's
/// fixed rate and does not survive the port; the four seconds is the actual intent.
inline constexpr double kLooperSeconds = 16.0;

/// The per-grain delay send. The reference's `kDelayBufferSize = 32768` at 44.1 kHz, which its own
/// comment calls "~0.74s ... quarter bar down to ~80 BPM". Written as the division so the number can
/// be checked rather than trusted.
inline constexpr double kDelaySeconds = 32768.0 / 44100.0;

/// A STEREO RING WITH A CAPACITY AND A LENGTH, which are different numbers on purpose.
class Ring {
public:
	/// ALLOCATE. Activate-time only. Idempotent at the same rate so a host may call it repeatedly.
	void prepare(double sampleRate, double seconds) {
		const double frames = sampleRate * seconds;
		const size_t want = (frames > 0.0) ? static_cast<size_t>(std::llround(frames)) : 0u;
		if (want == capacity_) {
			// **WHAT THIS GUARD ACTUALLY BUYS, stated carefully because the obvious answer is wrong.**
			// Not a malloc: `assign` at the size the vector already holds reuses its buffer and
			// allocates nothing, guard or no guard. A known-bad that deleted this line stayed GREEN
			// against a test asserting pointer stability, which is how that test was found to be
			// vacuous. What it buys is (a) not zeroing up to 1.5 MB on a redundant activate, and (b)
			// not resetting length, write position and filled out from under a ring that already
			// holds audio. Those are observable, and `test_fx_sklitter_ring.cpp` asserts them.
			return;
		}
		capacity_ = want;
		data_.assign(capacity_ * 2u, 0.0f);
		dirty_ = 0;
		length_ = capacity_;
		writePos_ = 0;
		filled_ = false;
		soundFrames_ = 0;
	}

	/// ZERO THE AUDIO AND THE POSITION, KEEP THE MEMORY. Runs on the audio thread.
	///
	/// **IT DOES NOT CLEAR `length_`.** That is a configuration rather than a running value, and a
	/// ring of length zero divides by zero on its next wrap.
	void reset() noexcept {
		// ONLY WHAT WAS EVER WRITTEN IS CLEARED: everything past `dirty_` is still the zeros `prepare` made it, so
		// the ring is bit-identical to a whole-capacity fill at the cost of the written span. A host tempo ramp
		// restarts the pre-warm nearly every block, and the whole capacity is 16 s of two channels.
		const size_t span = dirty_;
		std::fill(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(span * 2u), 0.0f);
		clearedFrames_ += span;
		dirty_ = 0;
		writePos_ = 0;
		filled_ = false;
		soundFrames_ = 0;
	}

	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }
	[[nodiscard]] size_t length() const noexcept { return length_; }
	[[nodiscard]] size_t writePos() const noexcept { return writePos_; }

	/// WHETHER THERE IS A WHOLE LOOP IN HERE YET. The reference's `looperBufferFull`, and it matters:
	/// reading a region that has never been written hands back silence dressed as audio.
	[[nodiscard]] bool filled() const noexcept { return filled_; }
	/// **WHETHER ANY FRAME HOLDS SOUND** (backlog MC; above `kSoundFloor`) — counted as every write, overwrite and seam fade
	/// lands, so it is exact whatever road wrote it and costs a compare per write rather than a scan. Over the whole
	/// capacity: a frame past a shortened length still counts, because a longer length would read it again. A device
	/// whose output can be exactly zero while its ring is not answers `AudioOps::holdsTail` from this.
	[[nodiscard]] bool holdsSound() const noexcept { return soundFrames_ > 0; }
	/// **HOW MANY WRITES HAVE LANDED, EVER — backlog MJ.** A store at a write gain above 0 (any writer: `write`,
	/// `writeAt`); a frozen or gated store counts nothing, and nor do `scale`/`fadeSeamAt`/`reset`, which are not
	/// recordings. Monotone and never cleared, so a reader differences two reads; the record light is "it moved".
	[[nodiscard]] size_t landed() const noexcept { return landed_; }

	/// MOVE THE LOGICAL LENGTH. Free, clamped, and never zero.
	void setLength(size_t frames) noexcept {
		if (capacity_ == 0) {
			return;
		}
		length_ = std::clamp(frames, size_t { 1 }, capacity_);
		if (writePos_ >= length_) {
			writePos_ = 0;
			filled_ = true; // shrinking past the write head means the shorter ring is already full
		}
	}

	void write(float left, float right) noexcept {
		if (capacity_ == 0) {
			return;
		}
		store(writePos_, left, right);
		if (++writePos_ >= length_) {
			writePos_ = 0;
			filled_ = true;
		}
	}

	/// **PUT THE APPEND HEAD AT A FRAME THE CALLER DERIVED**, wrapping — so the next `write` lands
	/// there. The idle and recording roads append, and appending keeps whatever phase the head STARTED
	/// at; a caller that knows where the song is re-seats the head so the ring's index is the song's
	/// phase, not a count of whatever it has been fed since (backlog IN).
	void seekWrite(size_t frame) noexcept {
		if (capacity_ == 0) {
			return;
		}
		writePos_ = frame % length_;
	}

	/// **FADE THE SEAM WHERE THE LOOP MEETS ITSELF.** The reference's `kBufferWrapFadeLen` pass, and
	/// its comment is the whole argument: *"Apply fade at buffer wrap boundary (position 0) to
	/// eliminate ring buffer discontinuity. Position 0 and bufSize-1 were recorded ~4s apart — fade
	/// once here instead of per-sample."*
	///
	/// ONCE, WHEN THE CAPTURE COMPLETES, ON THE BUFFER ITSELF. Every other anti-click measure in this
	/// device works on the OUTPUT and has to run per sample; this one is a property of the recording
	/// and is paid for a single time. The two are not alternatives — the zero-crossing gate handles
	/// grain edges, and this handles the one edge that is in the material rather than in the playback.
	///
	/// **AND THE SEAM IS WHERE THE NEWEST FRAME MEETS THE OLDEST — the append head — NOT INDEX 0 (groove-click,
	/// 2026-09-23).** The reference's seam is at 0 because its capture starts there. Since backlog IN the ring
	/// is appended on the song's phase, so frame len−1 and frame 0 are CONSECUTIVE song frames and the join
	/// that is "a loop apart" sits wherever the capture ended. Faded at 0, this dipped every DOWNBEAT to zero
	/// over 10 ms — MEASURED, a 0.5 sine read back at 0.00 at the bar top, in every setting — and left the real
	/// seam bare.
	///
	/// @param at     the first frame after the seam — the oldest one, which is `writePos()` once full
	/// @param frames how long the fade is; the reference's 220 is ~5 ms at 44.1 kHz, so callers pass
	///        a count computed from the host's rate rather than the integer.
	void fadeSeamAt(size_t at, size_t frames) noexcept {
		if (capacity_ == 0 || length_ < 2) {
			return;
		}
		const size_t n = std::min(frames, length_ / 2);
		for (size_t i = 0; i < n; ++i) {
			const auto g = static_cast<float>(i) / static_cast<float>(n);
			const size_t upIdx = (at + i) % length_;
			scale(upIdx, g);
			scale((at + length_ - 1 - i) % length_, g);
		}
	}

	/// WRITE A FRAME BY INDEX, wrapping. **This is not `write`**: `write` appends at the head and
	/// advances it, which is how recording works; this overwrites in place at a position the caller
	/// chose, which is how `pWrite` lets a captured loop evolve under the read head.
	void writeAt(size_t frame, float left, float right) noexcept {
		if (capacity_ == 0) {
			return;
		}
		store(frame % length_, left, right);
	}

	/// **THE FREEZE'S WRITE GAIN — backlog KX.** Every `write` and `writeAt` lands as `old + (new − old)·g`: at 1
	/// (the default, and every unfrozen sample) the frame is stored exactly as before, at 0 NOTHING is stored —
	/// not a multiply by zero, which would still turn a −0 into a +0 and a NaN input into a NaN — and between
	/// them the write is a crossfade, which is the ramp that keeps the freeze boundary a seam rather than a cut.
	/// `write` still advances its head at 0, so the append head keeps the song's phase while nothing lands.
	/// The caller sets it per sample (`Sklitter::freezeGain_`); `reset` and `fadeSeamAt` ignore it, and the
	/// device skips those itself while frozen.
	void setWriteGain(float g) noexcept { writeGain_ = g; }
	[[nodiscard]] float writeGain() const noexcept { return writeGain_; }

	/// READ A FRAME BY INDEX, wrapping rather than reading past the end. An unprepared ring hands
	/// back silence instead of touching nothing.
	void readAt(size_t frame, float* left, float* right) const noexcept {
		if (capacity_ == 0) {
			*left = 0.0f;
			*right = 0.0f;
			return;
		}
		const size_t i = frame % length_;
		*left = data_[i * 2u];
		*right = data_[i * 2u + 1u];
	}
	/// `readAt` FOR A CALLER THAT HAS ALREADY WRAPPED: `i` must be below `length()`, and nothing is checked. The
	/// Delay's multi-tap reads up to thirty-two of these a sample (backlog MD), and the modulo is most of each.
	void readWrapped(size_t i, float* left, float* right) const noexcept {
		*left = data_[i * 2u];
		*right = data_[i * 2u + 1u];
	}

	/// TEST-ONLY. **A REALLOCATION IS NOT OBSERVABLE FROM ANYTHING ELSE THIS CLASS EXPOSES** --
	/// capacity and length would read identically either way -- so the one case that matters most
	/// here, "activate twice and do not malloc", cannot be written without it. Five lines, per the
	/// rule that the remedy is cheap and the cleverness is not.
	/// TEST-ONLY. FRAMES `reset` HAS WRITTEN ZEROS OVER, summed over every call: a reset is O(frames written), not O(capacity).
	[[nodiscard]] size_t clearedFramesForTest() const noexcept { return clearedFrames_; }
	[[nodiscard]] const float* dataForTest() const noexcept {
		return data_.empty() ? nullptr : data_.data();
	}

private:
	/// **A FRAME HOLDS SOUND ABOVE -600 dBFS** (`kSoundFloor`; a NaN counts). MEASURED on the factory preset
	/// machine_elf_book_of_dreams: SKLITTER engaged in Shuffle kept 72,437 frames of a pWrite loop decaying through
	/// 1e-34 to 1e-36 for minutes, with its output exactly silent the whole time — counted as sound, a host was never
	/// let sleep. The filter's `kRestLevel` precedent: nothing that small is heard, or can grow back without input.
	static constexpr float kSoundFloor = 1.0e-30f;
	[[nodiscard]] bool soundAt(size_t i) const noexcept {
		return !(std::fabs(data_[i * 2u]) <= kSoundFloor && std::fabs(data_[i * 2u + 1u]) <= kSoundFloor);
	}

	void store(size_t i, float left, float right) noexcept {
		dirty_ = i >= dirty_ ? i + 1 : dirty_;
		const bool was = soundAt(i);
		// backlog MJ: a write that LANDS, counted where it lands — the record light's reading. Gain 0 lands nothing.
		landed_ += writeGain_ > 0.0f ? 1u : 0u;
		if (writeGain_ >= 1.0f) {
			data_[i * 2u] = left;
			data_[i * 2u + 1u] = right;
		}
		else if (writeGain_ > 0.0f) {
			const float oldL = data_[i * 2u];
			const float oldR = data_[i * 2u + 1u];
			data_[i * 2u] = oldL + (left - oldL) * writeGain_;
			data_[i * 2u + 1u] = oldR + (right - oldR) * writeGain_;
		}
		recount(was, soundAt(i));
	}

	void scale(size_t i, float g) noexcept {
		const bool was = soundAt(i);
		data_[i * 2u] *= g;
		data_[i * 2u + 1u] *= g;
		recount(was, soundAt(i));
	}

	void recount(bool was, bool now) noexcept {
		if (was != now) {
			soundFrames_ = now ? soundFrames_ + 1 : soundFrames_ - 1;
		}
	}

	std::vector<float> data_; ///< interleaved L,R -- 2 * capacity_ floats
	float writeGain_ = 1.0f;  ///< backlog KX — see `setWriteGain`
	size_t capacity_ = 0;
	size_t length_ = 0;
	size_t writePos_ = 0;
	bool filled_ = false;
	/// Frames holding a non-zero sample, over the whole capacity (backlog MC) — see `holdsSound`.
	size_t soundFrames_ = 0;
	size_t landed_ = 0; ///< backlog MJ — see `landed`
	/// ONE PAST THE HIGHEST FRAME EVER STORED SINCE THE LAST `reset` (or `prepare`): every frame at or past it is zero.
	size_t dirty_ = 0;
	size_t clearedFrames_ = 0; ///< see `clearedFramesForTest`
};

} // namespace feathers::fx::sklitter
