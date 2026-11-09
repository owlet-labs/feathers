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
#include "fx/spectral/spectral_stft.h"
#include "fx/spectral/spectral_types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace feathers::fx::spectral {

/// **THE SPECTRAL DEVICE**: the settings and the note made into a gain per bin, run through the STFT.
///
/// THE CURVE IS RECOMPUTED ONLY WHEN SOMETHING IT READS MOVED: the settings, the stop, or (per voice) the note's f0.
/// f0: the latched note (Key Track on) or C3, raised by Key Offset. ON THE BUS the note is the last one the host hands every
/// device; PER VOICE (Poly, `Traits::latentPerVoice`) each instance is a voice's and hears that voice's note.
class SpectralDevice {
public:
	/// ALLOCATE. Activate-time only.
	void prepare(double sampleRate);
	void reset() noexcept;
	void setTarget(const Settings& s) noexcept;
	void setNoteContext(const device::NoteContext& note) noexcept;
	/// THE CARD'S EAR: the input spectrum, kReadingCount bands in dBFS, published after each block that analysed a frame.
	void setReadingsSink(device::ReadingsSink sink) noexcept;
	/// The sidechain for the next `processStereo` (AudioOps::setSidechain).
	void setSidechain(const float* left, const float* right, bool connected) noexcept { stft_.setSidechain(left, right, connected); }
	void processStereo(float* left, float* right, int frames) noexcept;
	void processMono(float* mono, int frames) noexcept;

	/// TEST SEAMS: the gain each bin is given now, how many there are, the placement and the note's f0.
	[[nodiscard]] const float* gainsForTest() const noexcept { return stft_.gains(); }
	[[nodiscard]] int binCountForTest() const noexcept { return stft_.binCount(); }
	[[nodiscard]] bool perVoiceForTest() const noexcept { return target_.poly; }
	[[nodiscard]] double noteHzForTest() const noexcept { return noteHz_; }
	[[nodiscard]] const Stft& stftForTest() const noexcept { return stft_; }

	/// **THE FROZEN ENVELOPE FOR THE PATCH (Z10b, `kStateOps`)**: ANY THREAD, the block as last published, and its serial.
	uint64_t readFreeze(float* out, size_t capacity) const noexcept { return freezeSnapshot_.read(out, capacity); }
	/// MESSAGE THREAD, on an instance nothing runs yet: take a saved block. Held until the first block runs, which primes the
	/// envelope if Freeze is on and drops it if Freeze is off (the load decisions) — so a prepare or reset between the load and
	/// the first block cannot lose it.
	bool restoreFreeze(const float* in, size_t n) noexcept;

	/// **THE DRAWN CURVE, THE HAND'S BLOCK** (`spectral_curve_db`, `device::HandBlock`): band b in dB, harmonic b + 1 of f0.
	/// MESSAGE THREAD, running instance: `applyCurve` writes it whole (the one writer); ANY non-audio thread `readCurve`s the
	/// hand's latest. AUDIO THREAD: `pullCurve` takes the bus instance's block into this instance's own copy, one attempt,
	/// the last good copy kept on a torn read. `restoreCurve` is a load's: apply, then pull at once.
	bool applyCurve(const float* in, size_t n) noexcept;
	uint64_t readCurve(float* out, size_t capacity) const noexcept { return handCurve_.read(out, capacity); }
	void pullCurve(const SpectralDevice& source) noexcept;
	bool restoreCurve(const float* in, size_t n) noexcept;
	[[nodiscard]] const std::array<float, shaper::kCurveBands>& curveForTest() const noexcept { return curveCopy_.values; }
	/// TEST ONLY: whether a restored block is still waiting for the first block.
	[[nodiscard]] bool freezePendingForTest() const noexcept { return pending_.armed; }

private:
	void takePendingFreeze() noexcept;
	void rearmFreeze() noexcept;
	void publishFreeze() noexcept;
	void refresh() noexcept;
	/// Freeze follows the note with Key Track on (Z10): remembers the note as Freeze takes hold and hands the STFT the distance.
	void trackFreeze() noexcept;
	void publish() noexcept;

	Stft stft_;
	Settings target_ {};
	double noteHz_ = 261.6255653005986;
	bool dirty_ = true;
	/// Whether the operators' curve is the identity (the STFT's identity also asks the vocoder, a block at a time).
	bool gainsIdentity_ = true;
	std::vector<float> k_, hz_, base_;
	device::ReadingsSink sink_ {};
	uint64_t published_ = 0;
	double publishedF0_ = 0.0;
	bool publishedPlaying_ = false;
	bool publishedSounding_ = false;
	bool noteSeen_ = false; ///< a note context has arrived: Key Track's f0 is a played note
	/// THE NOTE IN SEMITONES (60 is C3 until one arrives), and FREEZE'S KEY TRACK (Z10): the note remembered when Freeze took hold
	/// with Key Track on; while both hold, the frozen envelope is transposed by the note's distance from it (`trackFreeze`).
	double noteSemis_ = 60.0;
	double frozenNoteSemis_ = 60.0;
	bool frozenTracked_ = false;
	/// Z10b: A RESTORED BLOCK, until the first block takes it (`takePendingFreeze`).
	struct PendingFreeze {
		bool armed = false;
		std::array<bool, 2> held {};
		std::array<std::array<float, kEnvelopePoints>, 2> dbfs {};
		std::array<float, 2> flatness {};
		bool tracked = false;
		double noteSemis = 60.0;
	};
	PendingFreeze pending_ {};
	/// Z10b: THE BLOCK AS LAST PUBLISHED, and what it was published from (republished when any of it moves; the first block
	/// always publishes, so the host's stored block never outlives the instance's own state).
	device::BlockSnapshot<kFreezeWords> freezeSnapshot_;
	device::HandBlock<static_cast<size_t>(shaper::kCurveBands)> handCurve_;
	device::HandBlockCopy<static_cast<size_t>(shaper::kCurveBands)> curveCopy_;
	std::array<float, shaper::kCurveBands> curveScratch_ {};
	uint32_t publishedFreezeMask_ = 0xFFFFFFFFu;
	bool publishedFreezeTracked_ = false;
	double publishedFreezeNote_ = 0.0;
};

} // namespace feathers::fx::spectral
